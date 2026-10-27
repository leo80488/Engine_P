// RenderExplain.cpp - Render.explain(id): why an entity looks the way it does.
// See the header for the rules the hints encode; every hint is backed by a reverse
// test in tests/unit/test_render_explain.py (the seeded fault must name itself).
#include "Renderer/RenderExplain.h"

#include "ECS/AnimationComponents.h"      // MeshSkinnedComponent
#include "ECS/CameraStackComponents.h"    // VirtualCameraComponent (Render.addView's camera)
#include "ECS/Components.h"               // MaterialComponent, MeshHandle, LightData
#include "ECS/HierarchyComponents.h"      // VisibilityComponent, hide tags, MeshLibRef, MaterialSourcePath
#include "Graphics/MaterialShaderDecl.h"  // a custom shader's declared domain
#include "Graphics/MeshDescriptorHeap.h"  // the meshlet sidecar (explain's path half)
#include "Graphics/RenderTypes.h"         // DrawPacket, DrawFilter
#include "Graphics/ShaderReflection.h"    // the custom PS's @engine roles
#include "Graphics/ShadingModel.h"
#include "RenderGraph/RenderPass/AutoExposurePass.h"
#include "Graphics/VideoMemory.h"          // Render.views' budget fields
#include "RenderGraph/RenderPass/GBufferPass.h"
#include "RenderGraph/RenderPass/TransparentPass.h"
#include "RenderGraph/RenderPass/WaterSurfacePass.h"   // Render.materialCosts' water rows
#include "Renderer/Renderer.h"
#include "Renderer/ViewBudget.h"           // Render.views' budget fields
#include "Resource/AssetFS.h"
#include "Resource/MeshLibIdentity.h"      // Render.explain's meshlib keys
#include "Scripting/LuaDoc.h"              // Render.plan
#include "Scripting/LuaEntityId.h"
#include "System/HostPolicy.h"
#include "System/PathUtil.h"

#include <sol/sol.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <map>
#include <thread>
#include <tuple>

// ==== The renderer's half ====
// Renderer members: they read private state.
void Renderer::ExplainEntity(Entity e, EntityRenderReport& out) const
{
    out = EntityRenderReport{};
    out.gpuCulling = m_gpuCullingEnabled;

    const auto bindIt = m_entityMaterialRuntimeBindings.find(e);
    const RuntimeMaterialEntityBinding* binding =
        bindIt != m_entityMaterialRuntimeBindings.end() ? &bindIt->second : nullptr;
    const RuntimeMaterialResource* resource = FindRuntimeMaterialResource(e);
    out.bound = binding && resource;

    auto status = [this](Resource::TextureHandle h)
    {
        return (m_texSys && m_resMgr) ? m_texSys->GetStatus(h, *m_resMgr)
                                      : Resource::TextureSystem::Status{};
    };
    if (resource)
    {
        static constexpr const char* kLabels[] = {
#define X(name, label, path) label,
            MATERIAL_TEXTURE_SLOTS(X)
#undef X
        };
        for (uint32_t s = 0; s < MaterialComponent::TEXTURESLOT_COUNT; ++s)
        {
            const MatTexEntry& t = resource->engineTextures[s];
            EntityRenderReport::Texture r;
            r.slot       = kLabels[s];
            r.engineSlot = s;
            r.path       = t.path;
            r.bindless   = t.view.bindlessIndex;
            r.status     = status(t.handle);
            if (binding && binding->overrideMask[s])
            {
                r.overridden = true;
                r.bindless   = binding->engineOverrides[s].bindlessIndex;
            }
            out.textures.push_back(std::move(r));
        }
        for (const auto& [name, t] : resource->customTextures)
        {
            EntityRenderReport::Texture r;
            r.slot     = name;
            r.path     = t.path;
            r.bindless = t.view.bindlessIndex;
            r.status   = status(t.handle);
            out.textures.push_back(std::move(r));
        }
        const auto sh = m_runtimeMaterialShaders.find(resource->shaderKey);
        if (sh != m_runtimeMaterialShaders.end())
        {
            out.customPSID = sh->second.customPSID;
            out.customVSID = sh->second.customVSID;
        }
    }

    // The ids resolve in every pass library (one DynamicShaderRegistry); an
    // opaque draw compiles its variants in the GBuffer library, a transparent one
    // in the Transparent library.
    auto shaderStatus = [this](int id)
    {
        ShaderLibrary::DynamicShaderStatus sum;
        if (id <= 0) return sum;
        ShaderLibrary* libs[] = {
            m_gbufferPass     ? &m_gbufferPass->GetShaderLibrary()                : nullptr,
            m_transparentPass ? m_transparentPass->GetReloadableShaderLibrary()   : nullptr,
        };
        for (ShaderLibrary* lib : libs)
        {
            if (!lib) continue;
            const ShaderLibrary::DynamicShaderStatus s = lib->GetDynamicShaderStatus(static_cast<uint32_t>(id));
            sum.compiled += s.compiled;
            if (s.failed && !sum.failed)
            {
                sum.failedPermBits = s.failedPermBits;
                sum.error          = s.error;
            }
            sum.failed += s.failed;
        }
        return sum;
    };
    out.ps = shaderStatus(out.customPSID);
    out.vs = shaderStatus(out.customVSID);

    // Emitted at all this material epoch (any list, shadow ones included) -
    // and which lists: the READ side, the last built frame's packets with the
    // instance remap that belongs to them (CullWorldForPreview's walk).
    out.emitted = GetEntityMaterialSlot(e) < m_maxMaterials;
    for (const DrawPacket& dp : DrawPacketsRead())
    {
        const uint32_t end = (std::min)(dp.instanceOffset + dp.instanceCount, m_maxInstances);
        for (uint32_t i = dp.instanceOffset; i < end; ++i)
        {
            if (EntityForDrawLocalInstance(i) != e) continue;
            const uint32_t f = static_cast<uint32_t>(dp.filter);
            if (f < std::size(out.inList)) ++out.inList[f];
            const bool mainList = dp.filter == DrawFilter::Opaque || dp.filter == DrawFilter::Transparent ||
                                  dp.filter == DrawFilter::Water;
            if (mainList && dp.renderInMainPass && (dp.viewMask & ViewBit::MainCamera))
                out.mainView = true;
        }
    }
}

// The geometry PATH of the entity's main-view opaque packet in the last built
// frame - the same tests, in the same order, as the scene build (geometric half,
// DrawPacket::meshletBlocks), the SceneEmit latch (path half) and GBufferPass
// (per-packet eligibility, dispatch limits, which loop recorded the frame). The
// reason is a stable snake_case code.
void Renderer::ExplainMeshletPath(Entity e, MeshletPathReport& out) const
{
    out = MeshletPathReport{};
    const DrawPacket* found = nullptr;
    bool anyPacket = false;
    for (const DrawPacket& dp : DrawPacketsRead())
    {
        const uint32_t end = (std::min)(dp.instanceOffset + dp.instanceCount, m_maxInstances);
        bool mine = false;
        for (uint32_t i = dp.instanceOffset; i < end && !mine; ++i)
            mine = EntityForDrawLocalInstance(i) == e;
        if (!mine) continue;
        anyPacket = true;
        if (dp.filter == DrawFilter::Opaque && dp.renderInMainPass && (dp.viewMask & ViewBit::MainCamera))
        {
            found = &dp;
            break;
        }
    }
    out.path = "vs";
    if (!found) { out.reason = anyPacket ? "not_opaque_main_view" : "not_drawn"; return; }
    const DrawPacket& dp = *found;
    if (dp.meshletBlocks == 0)
    {
        // The geometric half, in MeshletBlocksForSlot's order.
        const MeshDescriptorHeap::MeshletInfo info =
            m_meshMgr.GetDescriptorHeap().GetMeshletInfo(dp.meshDescriptorIndex);
        // A skinned draw carries a prev-pos base.
        if (dp.prevPosElementBase != 0xFFFFFFFFu)
            out.reason = "skinned";
        else if (dp.customVSID != 0)
            out.reason = "custom_vs";
        else if (info.blobBufferIdx == MeshDescriptorHeap::kNoMeshlets.blobBufferIdx)
            out.reason = "no_data";
        else
            out.reason = "too_many_clusters";
        return;
    }
    const SceneEmit& emit = SceneEmitRead();
    if (!emit.meshletFrameActive) { out.reason = emit.meshletReason ? emit.meshletReason : "off"; return; }
    if (dp.batchHasDissolve)                                 { out.reason = "dissolve";   return; }
    if (dp.permutation.Has(PermutationKey::BILLBOARD))       { out.reason = "billboard";  return; }
    if (!GBufferPass::MeshletDispatchFits(dp.meshletBlocks, dp.instanceCount))
                                                             { out.reason = "dispatch_limit"; return; }
    if (!m_gbufferPass || !m_gbufferPass->MeshletPsoReady()) { out.reason = "pso_failed"; return; }
    if (m_gbufferPass->GetMeshletFrameStats().indirect)      { out.reason = "indirect_path"; return; }
    out.path = "ms_cpu";
}

// The material cost table's rows (Renderer.h). One ShaderLibrary::Report per
// distinct (library, pixel shader, permutation), so a frame of thousands of
// packets asks the libraries a few dozen times. The ids resolve in every pass
// library (one DynamicShaderRegistry); each packet is asked of the library of the
// pass that draws it, in the permutation that pass draws it with: an opaque one
// the GBuffer's (its meshlet path draws the same pixel shader), a water one
// WaterSurfacePass's (WATER_SURFACE forced, as its BuildPSODesc does - water
// stamps its slot into GBuffer1.a), a forward one the Transparent library's.
void Renderer::CollectMaterialCosts(std::vector<MaterialCostRow>& out, bool details) const
{
    out.clear();
    ShaderLibrary* libs[3] = {
        m_gbufferPass      ? &m_gbufferPass->GetShaderLibrary()                  : nullptr,   // GBuffer
        m_transparentPass  ? m_transparentPass->GetReloadableShaderLibrary()     : nullptr,   // Forward
        m_waterSurfacePass ? m_waterSurfacePass->GetReloadableShaderLibrary()    : nullptr,   // Water
    };
    const ShaderID defaults[3] = { ShaderID::GBuffer_PS, ShaderID::Transparent_PS, ShaderID::WaterSurface_PS };
    struct Known
    {
        ShaderLibrary*              lib;
        std::uint32_t               ps, perm;
        ShaderLibrary::ShaderReport report;
        bool                        ok;
    };
    std::vector<Known> known;
    std::map<std::tuple<std::uint32_t, std::uint8_t, std::uint32_t, std::uint32_t>, size_t> rowOf;
    for (const DrawPacket& dp : DrawPacketsRead())
    {
        std::uint8_t pass;
        if      (dp.filter == DrawFilter::Opaque)      pass = MaterialCostRow::GBuffer;
        else if (dp.filter == DrawFilter::Transparent) pass = MaterialCostRow::Forward;
        else if (dp.filter == DrawFilter::Water)       pass = MaterialCostRow::Water;
        else continue;
        if (!dp.renderInMainPass || !(dp.viewMask & ViewBit::MainCamera) || dp.materialIndex >= m_maxMaterials)
            continue;
        ShaderLibrary* lib = libs[pass];
        if (!lib) continue;
        PermutationKey drawn = dp.permutation;
        if (pass == MaterialCostRow::Water) drawn.Set(PermutationKey::WATER_SURFACE, true);
        const std::uint32_t ps = dp.customPSID != 0 ? dp.customPSID : static_cast<std::uint32_t>(defaults[pass]);
        const std::uint32_t perm = ShaderLibrary::KeyBits(ps, drawn);
        auto k = std::find_if(known.begin(), known.end(), [&](const Known& x)
                              { return x.lib == lib && x.ps == ps && x.perm == perm; });
        if (k == known.end())
        {
            Known n{ lib, ps, perm, {}, false };
            n.ok = lib->Report(ps, drawn, n.report);
            known.push_back(std::move(n));
            k = known.end() - 1;
        }
        const auto key = std::make_tuple(dp.materialIndex, pass, ps, perm);
        auto at = rowOf.find(key);
        if (at == rowOf.end())
        {
            MaterialCostRow r;
            r.slot         = dp.materialIndex;
            r.pass         = pass;
            r.psId         = ps;
            r.permBits     = perm;
            r.compiled     = k->ok;
            r.instructions = k->ok ? k->report.instructions : 0u;
            if (details)
            {
                r.file  = k->report.file;
                r.entry = k->report.entry;
                r.blob  = k->report.cachePath;
            }
            at = rowOf.emplace(key, out.size()).first;
            out.push_back(std::move(r));
        }
        MaterialCostRow& row = out[at->second];
        ++row.packets;
        row.instances += dp.instanceCount;
        if (details && row.entity == NullEntity && dp.instanceCount != 0)
            row.entity = EntityForDrawLocalInstance(dp.instanceOffset);
    }
    std::sort(out.begin(), out.end(), [](const MaterialCostRow& a, const MaterialCostRow& b)
              { return std::tie(a.slot, a.pass, a.psId, a.permBits) < std::tie(b.slot, b.pass, b.psId, b.permBits); });
}

std::uint64_t Renderer::DebugMaterialCoverage(std::vector<std::uint32_t>& pixels) const
{
    std::lock_guard<std::mutex> lk(m_debugCoverageMutex);
    pixels = m_debugCoverage;
    return m_debugCoverageFrame;
}

void Renderer::TextureConsumers(const std::string& path, std::vector<Entity>& out) const
{
    auto fold = [](std::string s)
    {
        for (char& c : s)
        {
            if (c == '\\') c = '/';
            else c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return s;
    };
    const std::string want = fold(path);
    if (want.empty()) return;
    for (const auto& [e, binding] : m_entityMaterialRuntimeBindings)
    {
        const RuntimeMaterialResource* resource = FindRuntimeMaterialResource(e);
        if (!resource) continue;
        bool samples = false;
        // An overridden slot samples its per-entity override, not the resource's texture.
        for (uint32_t s = 0; s < MaterialComponent::TEXTURESLOT_COUNT && !samples; ++s)
            samples = !binding.overrideMask[s] && !resource->engineTextures[s].path.empty() &&
                      fold(resource->engineTextures[s].path) == want;
        for (const auto& [name, t] : resource->customTextures)
            if (!samples) samples = !t.path.empty() && fold(t.path) == want;
        if (samples) out.push_back(e);
    }
}

uint64_t Renderer::TextureUserKey(const std::string& path)
{
    uint64_t h = 1469598103934665603ull;
    for (char ch : path)
    {
        unsigned char c = static_cast<unsigned char>(ch);
        c = (c == '\\') ? static_cast<unsigned char>('/') : static_cast<unsigned char>(std::tolower(c));
        h ^= c;
        h *= 1099511628211ull;
    }
    return path.empty() ? 0ull : h;
}

void Renderer::TextureConsumerCounts(std::unordered_map<uint64_t, uint32_t>& out) const
{
    // A resource's keys, hashed once: thousands of entities share a few hundred materials.
    struct Keys
    {
        std::array<uint64_t, MaterialComponent::TEXTURESLOT_COUNT> engine{};
        std::vector<uint64_t> custom;
    };
    std::unordered_map<const RuntimeMaterialResource*, Keys> byResource;
    std::vector<uint64_t> sampled;   // one entity's distinct textures: it counts once per texture
    for (const auto& [e, binding] : m_entityMaterialRuntimeBindings)
    {
        const RuntimeMaterialResource* resource = FindRuntimeMaterialResource(e);
        if (!resource) continue;
        auto [it, fresh] = byResource.try_emplace(resource);
        Keys& k = it->second;
        if (fresh)
        {
            for (uint32_t s = 0; s < MaterialComponent::TEXTURESLOT_COUNT; ++s)
                k.engine[s] = TextureUserKey(resource->engineTextures[s].path);
            for (const auto& [name, t] : resource->customTextures)
                if (!t.path.empty()) k.custom.push_back(TextureUserKey(t.path));
        }
        sampled.clear();
        // An overridden slot samples its per-entity override, not the resource's texture.
        for (uint32_t s = 0; s < MaterialComponent::TEXTURESLOT_COUNT; ++s)
            if (!binding.overrideMask[s] && k.engine[s]) sampled.push_back(k.engine[s]);
        sampled.insert(sampled.end(), k.custom.begin(), k.custom.end());
        std::sort(sampled.begin(), sampled.end());
        sampled.erase(std::unique(sampled.begin(), sampled.end()), sampled.end());
        for (uint64_t key : sampled) ++out[key];
    }
}

namespace
{
    constexpr const char* kFilterNames[] = { "Opaque", "Shadow", "Transparent", "Custom", "Water",
                                             "SecondaryView", "SecondaryViewBlend", "ProbeView",
                                             "SecondaryViewCustom" };
    static_assert(std::size(kFilterNames) == static_cast<size_t>(DrawFilter::Count),
                  "a DrawFilter needs its name here (Render.explain's lists)");
    constexpr const char* kBlendNames[]  = { "Opaque", "Alpha", "Additive", "Premultiplied", "Multiply",
                                             "Masked", "Dithered", "Transmissive" };
    constexpr const char* kDomainNames[] = { "Opaque", "Transparent", "Decal", "PostProcess" };

    // The engine's own default Emissive map: 1x1 black, MULTIPLIED in.
    constexpr const char* kDefaultEmissive = "engine/textures/Default_Emissive.itex";

    struct Hint { const char* code; const char* kind; std::string text; };

    bool AssetExists(const std::string& path)
    {
        if (path.empty()) return false;
        return Resource::AssetFS::Get().Exists(path);
    }

    std::string LowerSlash(std::string s)
    {
        for (char& c : s)
        {
            if (c == '\\') c = '/';
            else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return s;
    }

    std::string FirstLine(const std::string& s, size_t max = 200)
    {
        // The first line that says something (DXC output starts with the file:line: error).
        size_t start = 0;
        while (start < s.size())
        {
            size_t end = s.find('\n', start);
            if (end == std::string::npos) end = s.size();
            std::string line = s.substr(start, end - start);
            if (line.find_first_not_of(" \t\r") != std::string::npos)
            {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                return line.size() > max ? line.substr(0, max) + "..." : line;
            }
            start = end + 1;
        }
        return {};
    }

    std::string Num(double v)
    {
        char b[32];
        std::snprintf(b, sizeof(b), "%.3g", v);
        return b;
    }

    // What the shader samples when a slot has no resident texture
    // (material_surface.hlsli, GBufferPass/TransparentPass per-draw bindings).
    const char* FallbackFor(uint32_t engineSlot)
    {
        switch (engineSlot)
        {
        case MaterialComponent::BASECOLORMAP: return "white";
        case MaterialComponent::NORMALMAP:    return "a flat normal";
        case MaterialComponent::SURFACEMAP:   return "white (AO 1, roughness = roughnessMax, metalness = metalnessMax)";
        case MaterialComponent::EMISSIVEMAP:  return "nothing (the emissive colour is not multiplied by a map)";
        case MaterialComponent::OPACITYMAP:   return "nothing (no opacity mask)";
        case ~0u:                             return "white (an unbound custom slot samples white)";
        default:                              return "the pass's default";
        }
    }

    const char* StateName(const Resource::TextureSystem::Status& s, const std::string& path)
    {
        using State = Resource::TextureSystem::Status::State;
        if (path.empty()) return "empty";
        switch (s.state)
        {
        case State::Ready:   return "ready";
        case State::Loading: return "loading";
        case State::Failed:  return "failed";
        default:             return "unbound";
        }
    }

    // ---- Material rules ----
    using HintSink = std::function<void(const char* code, const char* kind, std::string text)>;

    // Every hint about the MATERIAL, in Render.explain's order. Explain calls this
    // for its hints and RenderExplain::MaterialHints for the same list without Lua
    // (the Inspector's material row), so the two cannot drift apart.
    void CollectMaterialHints(World& world, Entity e, const MaterialComponent& mc, const EntityRenderReport& rr,
                              const HintSink& hint)
    {
        if (const MaterialSourcePath* src = world.GetComponent<MaterialSourcePath>(e); src && !src->path.empty())
            if (!AssetExists(src->path))
                hint("material_file_missing", "cause",
                     "MaterialSourcePath '" + src->path + "' does not exist: the loader used a default "
                     "material in its place");

        // Custom shaders: missing, or failed over the variants the passes asked for.
        auto shader = [&](const std::string& path, const ShaderLibrary::DynamicShaderStatus& st, bool pixel)
        {
            if (!mc.useCustomShader || path.empty()) return;
            const char* skipped = pixel ? "every pass SKIPS the draw: the entity is absent (not black) and "
                                          "still casts shadows"
                                        : "every pass SKIPS the draw AND its shadows";
            if (!AssetExists(path))
                hint(pixel ? "custom_ps_missing" : "custom_vs_missing", "cause",
                     std::string("custom ") + (pixel ? "pixel" : "vertex") + " shader '" + path +
                     "' does not exist: " + skipped);
            else if (st.failed)
                hint(pixel ? "custom_ps_failed" : "custom_vs_failed", "cause",
                     std::string("custom ") + (pixel ? "pixel" : "vertex") + " shader '" + path +
                     "' failed to compile (" + std::to_string(st.failed) + " variant(s)): " + skipped +
                     ". DXC: " + FirstLine(st.error));
        };
        shader(mc.customShaderPath, rr.ps, true);
        shader(mc.customVertexShaderPath, rr.vs, false);

        // Textures: a slot that is not resident samples a fallback.
        std::string armState = "unbound";
        std::string emissivePath, emissiveState;
        for (const EntityRenderReport::Texture& t : rr.textures)
        {
            const char* state = StateName(t.status, t.path);
            if (t.engineSlot == MaterialComponent::SURFACEMAP) armState = state;
            if (t.engineSlot == MaterialComponent::EMISSIVEMAP) { emissivePath = t.path; emissiveState = state; }
            const bool engineSlotUsed = t.engineSlot != MaterialComponent::RAMPMAP;   // NPR only
            if (std::string(state) == "failed" && engineSlotUsed)
                hint("texture_failed", "cause", t.slot + " '" + t.path + "' failed to load (" + t.status.reason +
                                                "): the shader samples " + FallbackFor(t.engineSlot) + " instead");
            else if (std::string(state) == "loading" && engineSlotUsed)
                hint("texture_loading", "note", t.slot + " '" + t.path + "' is not resident yet: the shader samples " +
                                                FallbackFor(t.engineSlot) + " until it is");
        }
        if (!rr.bound)
            hint("material_not_bound", "note", "the renderer has no runtime material for it yet (first frames, or "
                                               "it never entered a draw gather): texture states are unknown");

        // Standard-shader rules - a custom pixel shader computes its own surface.
        const bool customPS = mc.useCustomShader && !mc.customShaderPath.empty();
        if (customPS) return;
        const auto blend = static_cast<uint32_t>(mc.userBlendMode);
        const float maxRgb = (std::max)({ mc.baseColor.x, mc.baseColor.y, mc.baseColor.z });
        if (maxRgb < 0.02f)
            hint("base_color_black", "cause", "baseColor is (" + Num(mc.baseColor.x) + ", " +
                                              Num(mc.baseColor.y) + ", " + Num(mc.baseColor.z) +
                                              "): black before any texture or light");
        const bool unlit = mc.customShadingModel == ShadingModel::Unlit;
        const bool armWhite = rr.bound && armState != "ready";
        if (!unlit && (mc.metalnessMin >= 0.9f || (armWhite && mc.metalnessMax >= 0.9f)))
        {
            const std::string why = mc.metalnessMin >= 0.9f
                ? "metalnessMin = " + Num(mc.metalnessMin)
                : "the Surface (ARM) map is " + armState + ", so the shader samples white and metalness = "
                  "metalnessMax = " + Num(mc.metalnessMax);
            hint("fully_metallic", "cause", "metalness resolves to ~1 (" + why + "): a metal has no diffuse - "
                 "the surface shows only what it reflects, black wherever there is nothing to reflect "
                 "(no sky, IBL, probe or screen-space reflection)");
        }
        if (IsTransparentBlendMode(mc.userBlendMode) && mc.baseColor.w < 0.02f)
            hint("transparent_invisible", "cause", std::string("blend ") +
                 (blend < std::size(kBlendNames) ? kBlendNames[blend] : "?") +
                 " with opacity (baseColor.a) " + Num(mc.baseColor.w) + ": nothing shows");
        else if (mc.IsAlphaTestEnabled() && mc.baseColor.w < mc.MaskClipValue())
            hint("masked_clipped", "cause", "alpha-tested (clip value " + Num(mc.MaskClipValue()) +
                 ") with opacity " + Num(mc.baseColor.w) + ": every pixel is clipped");
        if (mc.GetEmissiveStrength() > 0.f && emissiveState == "ready" &&
            LowerSlash(emissivePath) == LowerSlash(kDefaultEmissive))
            hint("emissive_masked", "cause", "emissive strength " + Num(mc.GetEmissiveStrength()) +
                 ", but the Emissive map is the engine default (1x1 black) and is multiplied in: no glow - "
                 "clear the slot or assign a map");
        if (unlit)
            hint("unlit", "note", "shading model Unlit: the surface is baseColor x texture (+ emissive); "
                                  "lights do not change it");
    }

    // ---- Render.explain ----
    sol::table Explain(sol::this_state ts, World& world, Renderer& renderer, Entity e, const LuaEntity& id)
    {
        sol::state_view lua(ts);
        sol::table out = lua.create_table();
        std::vector<Hint> hints;
        auto hint = [&hints](const char* code, const char* kind, std::string text)
        { hints.push_back({ code, kind, std::move(text) }); };

        out["id"]   = id;
        out["name"] = world.GetName(e);

        // ---- mesh ----
        const char* mesh = world.HasComponent<MeshSkinnedComponent>(e) ? "skinned"
                         : world.HasComponent<MeshLibRef>(e)           ? "meshlib"
                         : world.HasComponent<MeshHandle>(e)           ? "primitive" : "none";
        out["mesh"] = mesh;
        // A library draw: which entry, and its keys (the entry a model's `sec=`
        // resolved to, not the `mesh=` hint).
        if (const MeshLibRef* ref = world.GetComponent<MeshLibRef>(e); ref && ref->IsValid())
            if (const Resource::MeshLibrary* lib = renderer.GetMeshLibrary())
            {
                sol::table ml = lua.create_table();
                ml["library"] = lib->GetSourcePath(ref->libHandle);
                ml["entry"]   = ref->meshId;
                if (const auto id = lib->GetIdentity(ref->libHandle); id && ref->meshId < id->sections.size())
                {
                    const Resource::MeshIdentity::Section& s = id->sections[ref->meshId];
                    ml["key"]         = Resource::MeshKeyToHex(s.key);
                    ml["synthesized"] = (id->flags & Resource::IDNT_FLAG_SYNTHESIZED) != 0;
                    if (s.ownerMesh < id->meshes.size())
                    {
                        ml["meshKey"]  = Resource::MeshKeyToHex(id->meshes[s.ownerMesh].key);
                        ml["meshName"] = id->meshes[s.ownerMesh].name;
                    }
                }
                out["meshlib"] = ml;

                // ---- meshlets ----
                // What the library carries for this entry. `data`: resident (the
                // entry has clusters on the GPU), no_data (the file has no MLET -
                // `python tools/asset_tool.py meshlets --apply` builds it), dropped
                // (the MLET did not validate; warned at load), upload_failed, or
                // no_entry (the library has meshlets, this entry none - a skinned or
                // empty section).
                sol::table mt = lua.create_table();
                const Resource::MeshLibrary::MeshletLibraryInfo li = lib->GetMeshletInfo(ref->libHandle);
                Resource::MeshLibrary::MeshletEntryInfo         ei;
                const bool hasEntry = lib->GetMeshletEntry(ref->libHandle, ref->meshId, ei);
                const char* data = hasEntry ? "resident"
                                 : li.state == Resource::MeshLibrary::MeshletState::Resident ? "no_entry"
                                 : li.state == Resource::MeshLibrary::MeshletState::None     ? "no_data"
                                 : Resource::MeshLibrary::MeshletStateName(li.state);
                mt["data"]            = data;
                mt["libraryClusters"] = li.clusters;
                mt["libraryBytes"]    = li.gpuBytes;
                if (hasEntry)
                {
                    mt["clusters"]       = ei.clusterCount;
                    mt["levels"]         = ei.levelCount;
                    mt["level0Clusters"] = ei.level0Clusters;
                    // The levels the shaded view draws (the LOD shading check,
                    // Resource/MeshletData.h); the shadows draw all `levels`.
                    mt["shadedLevels"]   = ei.shadedLevelCount;
                }
                Renderer::MeshletPathReport mp;
                renderer.ExplainMeshletPath(e, mp);
                mt["path"] = mp.path;
                if (!mp.reason.empty()) mt["reason"] = mp.reason;
                // The level the LOD rule picks for this entity now, and each level's
                // clusters / triangles / error (projected = pixels now).
                Renderer::MeshletLodReport lr;
                renderer.ExplainMeshletLod(e, lr);
                if (lr.valid)
                {
                    sol::table lod = lua.create_table();
                    lod["tau"]      = lr.tau;
                    lod["selected"] = lr.selected;
                    sol::table lv = lua.create_table();
                    for (size_t l = 0; l < lr.levels.size(); ++l)
                        lv[l + 1] = lua.create_table_with("clusters", lr.levels[l].clusters,
                                                          "triangles", lr.levels[l].triangles,
                                                          "selfError", lr.levels[l].selfError,
                                                          "parentError", lr.levels[l].parentError,
                                                          "projected", lr.levels[l].projected);
                    lod["levels"] = lv;
                    mt["lod"] = lod;
                }
                out["meshlet"] = mt;
                if (!hasEntry && li.state == Resource::MeshLibrary::MeshletState::None)
                    hint("no_meshlets", "note", "the library has no meshlets (no MLET chunk): it draws through the "
                                                "vertex shader - python tools/asset_tool.py meshlets --apply builds them");
            }

        // ---- visibility (the gather's own filters, in its order) ----
        const VisibilityComponent* vis = world.GetComponent<VisibilityComponent>(e);
        const bool editorHost = Sys::IsEditorHost();
        const bool disabled   = world.HasComponent<DisabledTag>(e);
        const bool hideInGame = !editorHost && world.HasComponent<HiddenInGameTag>(e);
        const bool editorOnly = !editorHost && world.HasComponent<EditorOnlyTag>(e);
        sol::table v = lua.create_table();
        v["visible"]         = vis ? vis->IsVisible() : true;
        v["inheritedHidden"] = vis ? vis->inheritedHidden : false;
        v["mainPass"]        = vis ? vis->RendersInMainPass() : true;
        v["castShadow"]      = vis ? vis->CastsShadow() : true;
        v["viewMask"]        = vis ? vis->viewMask : ViewBit::All;
        v["disabled"]        = disabled;
        v["hiddenInGame"]    = world.HasComponent<HiddenInGameTag>(e);
        v["editorOnly"]      = world.HasComponent<EditorOnlyTag>(e);
        out["visibility"] = v;

        const bool hasMesh = std::string(mesh) != "none";
        bool hidden = false;
        if (!hasMesh)
            hint("no_mesh", "cause", "the entity has no mesh component (MeshLibRef / MeshHandle / "
                                     "MeshSkinnedComponent): nothing of its own is drawn - its children may be");
        if (vis && !vis->IsVisible())
        { hidden = true; hint("hidden", "cause", "VisibilityComponent: Visible is off"); }
        else if (vis && vis->inheritedHidden)
        { hidden = true; hint("hidden", "cause", "an ancestor is hidden (VisibilityComponent.inheritedHidden)"); }
        if (disabled)   { hidden = true; hint("hidden", "cause", "DisabledTag: culled in every build"); }
        if (hideInGame) { hidden = true; hint("hidden", "cause", "HiddenInGameTag: not drawn outside the editor"); }
        if (editorOnly) { hidden = true; hint("hidden", "cause", "EditorOnlyTag: not drawn outside the editor"); }
        if (vis && !hidden && !vis->RendersInMainPass())
            hint("not_in_main_pass", "cause", "VisibilityComponent: RenderInMainPass is off - shadow only");
        if (vis && !hidden && (vis->viewMask & ViewBit::MainCamera) == 0)
            hint("not_in_main_view", "cause", "VisibilityComponent.viewMask excludes the main camera");

        // ---- the renderer's half ----
        EntityRenderReport rr;
        renderer.ExplainEntity(e, rr);

        sol::table drawn = lua.create_table();
        drawn["emitted"]    = rr.emitted;
        drawn["mainView"]   = rr.mainView;
        drawn["gpuCulling"] = rr.gpuCulling;
        sol::table lists = lua.create_table();
        for (uint32_t f = 0; f < std::size(kFilterNames); ++f)
            if (rr.inList[f]) lists[kFilterNames[f]] = rr.inList[f];
        drawn["lists"] = lists;
        out["drawn"] = drawn;

        // ---- material ----
        const MaterialComponent* mc = world.GetComponent<MaterialComponent>(e);
        if (mc)
        {
            sol::table m = lua.create_table();
            if (const MaterialSourcePath* src = world.GetComponent<MaterialSourcePath>(e); src && !src->path.empty())
            {
                m["source"]       = src->path;
                m["sourceExists"] = AssetExists(src->path);
            }
            const auto blend  = static_cast<uint32_t>(mc->userBlendMode);
            const auto domain = static_cast<uint32_t>(mc->domain);
            m["blend"]        = blend < std::size(kBlendNames) ? kBlendNames[blend] : "?";
            m["domain"]       = domain < std::size(kDomainNames) ? kDomainNames[domain] : "?";
            m["shadingModel"] = static_cast<int>(mc->customShadingModel);
            m["twoSided"]     = mc->IsDoubleSided();
            m["castShadow"]   = mc->IsCastingShadow();
            m["baseColor"]    = lua.create_table_with(1, mc->baseColor.x, 2, mc->baseColor.y, 3, mc->baseColor.z,
                                                      4, mc->baseColor.w);
            m["emissive"]     = lua.create_table_with(1, mc->emissiveColor.x, 2, mc->emissiveColor.y,
                                                      3, mc->emissiveColor.z, 4, mc->emissiveColor.w);
            m["metalness"]    = lua.create_table_with(1, mc->metalnessMin, 2, mc->metalnessMax);
            m["roughness"]    = lua.create_table_with(1, mc->roughnessMin, 2, mc->roughnessMax);
            m["alphaRef"]     = mc->alphaRef;

            // Custom shaders: the path, whether the file is there, the compile
            // state over every variant the passes asked for.
            auto shader = [&](const std::string& path, int id, const ShaderLibrary::DynamicShaderStatus& st)
                -> sol::object
            {
                if (!mc->useCustomShader || path.empty()) return sol::make_object(lua, sol::lua_nil);
                sol::table s = lua.create_table();
                const bool exists = AssetExists(path);
                s["path"]     = path;
                s["exists"]   = exists;
                s["id"]       = id;
                s["compiled"] = st.compiled;
                s["failed"]   = st.failed;
                if (st.failed) s["error"] = FirstLine(st.error, 400);
                // What the source declares - its domain
                // (#define MATERIAL_DOMAIN_WATER; recorded, not enforced) - and
                // where the interface is read from: the declarations when they
                // parse exactly, else the DXIL reflection.
                if (id > 0)
                    if (ShaderLibrary* lib = renderer.GetMaterialShaderLibrary())
                        if (const auto decl = lib->GetDynamicDecl(static_cast<uint32_t>(id)); decl && decl->read)
                        {
                            s["declaredDomain"] = decl->domain == MaterialShaderDecl::Domain::Water ? "water" : "surface";
                            s["interface"]      = decl->Exact() ? "declarations" : "dxil";
                        }
                return s;
            };
            m["customPS"] = shader(mc->customShaderPath, rr.customPSID, rr.ps);
            m["customVS"] = shader(mc->customVertexShaderPath, rr.customVSID, rr.vs);

            // What the lighting reads out of the custom PS by meaning, and the
            // declarations that carry it. Explicit: only these; otherwise
            // WriteMaterialSlot takes the positional slots.
            if (mc->useCustomShader && rr.customPSID > 0)
                if (const ShaderReflect::Reflection* refl = renderer.GetCustomShaderReflection(rr.customPSID, mc))
                {
                    sol::table roles = lua.create_table();
                    for (const ShaderReflect::CBufferLayout& cb : refl->cbuffers)
                        for (const ShaderReflect::CBufferVar& v : cb.vars)
                            if (!v.engineRole.empty()) roles[v.engineRole] = v.name;
                    for (const ShaderReflect::ResourceBinding& b : refl->bindings)
                        if (!b.engineRole.empty()) roles[b.engineRole] = b.name;
                    m["engineRoles"] = lua.create_table_with("explicit", refl->engineRolesExplicit, "roles", roles);
                }

            // The per-frame values an engine system writes (e.g. the face's
            // HeadForward / HeadRight / HeadCenter); the authored customParams do not
            // carry them.
            if (!mc->frameParams.empty())
            {
                sol::table fp = lua.create_table();
                for (const auto& [name, v] : mc->frameParams)
                    fp[name] = lua.create_table_with(1, v[0], 2, v[1], 3, v[2], 4, v[3]);
                m["frameParams"] = fp;
            }

            // Textures: what each slot resolved to, and what the shader samples
            // when it is not resident.
            sol::table texs = lua.create_table();
            int n = 0;
            for (const EntityRenderReport::Texture& t : rr.textures)
            {
                const char* state = StateName(t.status, t.path);
                sol::table tt = lua.create_table();
                tt["slot"]     = t.slot;
                tt["path"]     = t.path;
                tt["state"]    = state;
                tt["bindless"] = t.bindless;
                tt["custom"]   = t.engineSlot == ~0u;
                if (!t.status.reason.empty()) tt["reason"] = t.status.reason;
                if (std::string(state) != "ready") tt["fallback"] = FallbackFor(t.engineSlot);
                if (t.overridden) tt["overridden"] = true;
                texs[++n] = tt;
            }
            m["textures"] = texs;

            // The material's hints: one rule set, shared with RenderExplain::MaterialHints.
            CollectMaterialHints(world, e, *mc, rr, hint);
            out["material"] = m;
        }
        else if (hasMesh)
            hint("no_material", "note", "no MaterialComponent: drawn with the neutral default material");

        // ---- drawn in the last frame? ----
        if (hasMesh && !hidden)
        {
            if (!rr.emitted)
                hint("not_drawn", "cause", "no draw in the last frame: off-screen (culled on the CPU), or its mesh "
                                           "is not loaded yet");
            else if (!rr.mainView && rr.inList[static_cast<uint32_t>(DrawFilter::Shadow)])
                hint("shadow_only", "cause", "only in shadow lists in the last frame: outside the main camera's view "
                                             "(it still casts shadows)");
        }

        // ---- scene ----
        uint32_t lights = 0;
        if (auto* pool = world.GetPool<LightData>())
            for (const LightData& l : pool->Data())
                if (l.enabled && l.intensity > 0.f) ++lights;
        sol::table scene = lua.create_table();
        scene["lights"] = lights;
        out["scene"] = scene;
        if (lights == 0)
            hint("no_lights", "note", "the scene has no enabled light: only sky / environment light reaches surfaces");

        sol::table h = lua.create_table();
        for (size_t i = 0; i < hints.size(); ++i)
            h[i + 1] = lua.create_table_with("code", hints[i].code, "kind", hints[i].kind, "text", hints[i].text);
        out["hints"] = h;
        return out;
    }
}

std::vector<RenderExplain::Hint> RenderExplain::MaterialHints(World& world, const Renderer& renderer, Entity e)
{
    std::vector<Hint> out;
    const MaterialComponent* mc = world.IsAlive(e) ? world.GetComponent<MaterialComponent>(e) : nullptr;
    if (!mc) return out;
    EntityRenderReport rr;
    renderer.ExplainEntity(e, rr);
    CollectMaterialHints(world, e, *mc, rr, [&out](const char* code, const char* kind, std::string text)
                         { out.push_back({ code, kind, std::move(text) }); });
    return out;
}

void RenderExplain::RegisterLua(lua_State* L, World& world, Renderer& renderer, std::function<void()> drain)
{
    if (!L) return;
    sol::state_view lua(L);
    sol::table render = lua["Render"].get_or_create<sol::table>();
    const std::thread::id mainThread = std::this_thread::get_id();
    World* w = &world;
    Renderer* r = &renderer;
    render.set_function("explain", [w, r, drain, mainThread](sol::this_state ts, const LuaEntity& id) -> sol::table
    {
        // The draw list and the runtime-material maps are written by the scene
        // build: drain the frame's render work first, which only main may do.
        if (std::this_thread::get_id() != mainThread)
            throw sol::error("Render.explain: wrong_thread: call it from the main thread (a control-channel run, "
                             "a scenario driver)");
        const Entity e = LuaId::ResolveLive(*w, id);
        if (e == NullEntity)
            throw sol::error("Render.explain: stale_handle: entity " + std::to_string(id.raw()) +
                             " is not alive (destroyed, or its id was reused)");
        if (drain) drain();
        return Explain(ts, *w, *r, e, id);
    });

    // Render.views() -> { {id = "main", width, height, owner}, {id = <preview view>,
    //   lease, kind = "camera" | "material", camera, width, height, renders,
    //   pip}, ... }
    // What the renderer draws this session: the main view and every live preview
    // view. `renders` is the view's monotonic picture count: compare two readings.
    // `lease` is its lifetime generation: (id, lease) names this tenancy of the slot,
    // which is what capture {"view", "lease"} takes - a view closed and reopened (or
    // the id handed to another panel) has a new lease and the old pair is refused
    // stale_view. A camera view's `camera` is the entity it looks through; `pip`
    // marks the in-game PiP (Preview.SetPiPCamera, and a capture {"camera"}). The main
    // view's `owner` is whose camera it renders in the editor ("editor" / "game"; "" in
    // Game.exe, Renderer::SetMainViewOwner).
    render.set_function("views", [w, r, drain, mainThread](sol::this_state ts) -> sol::table
    {
        if (std::this_thread::get_id() != mainThread)
            throw sol::error("Render.views: wrong_thread: call it from the main thread");
        if (drain) drain();
        sol::state_view lua(ts);
        sol::table out = lua.create_table();
        int n = 0;
        sol::table mainView = lua.create_table();
        mainView["id"]     = "main";
        mainView["kind"]   = "main";
        mainView["width"]  = r->GetRenderWidth();
        mainView["height"] = r->GetRenderHeight();
        mainView["owner"]  = r->MainViewOwnerName();
        mainView["viewAliasHeapBytes"] = r->GetViewAliasHeapBytes();
        // The video memory the process holds against the budget it plans by (VideoMemory), where the views'
        // ladder stands (ViewBudget: 0 normal, 1 half size, 2 suspended) and what it has refused or seen move.
        const VideoMemory::Info vm = VideoMemory::Last();
        mainView["vramUsageBytes"]   = vm.usage;
        mainView["vramBudgetBytes"]  = vm.budget;
        mainView["budgetStage"]      = static_cast<int>(ViewBudget::Current());
        mainView["budgetRefusals"]   = ViewBudget::Refusals();
        mainView["budgetChanges"]    = VideoMemory::BudgetChanges();
        mainView["residencyHigh"]    = VideoMemory::PrioritizedCount();
        out[++n] = mainView;
        const std::uint32_t pip = r->GetGamePiPView();
        for (std::uint32_t v = 0; v < Preview::kMaxPreviews; ++v)
        {
            std::uint32_t vw = 0, vh = 0;
            if (!r->GetPreviewViewRequestSize(v, vw, vh)) continue;   // not live
            sol::table t = lua.create_table();
            const bool camera = r->IsPreviewWorldCullSlot(v);
            t["id"]      = v;
            t["lease"]   = r->GetPreviewViewLease(v);
            t["kind"]    = camera ? "camera" : "material";
            t["width"]   = vw;
            t["height"]  = vh;
            t["renders"] = r->GetPreviewViewRenderCount(v);
            t["pip"]     = (v == pip);
            if (camera)
                t["camera"] = LuaId::Out(*w, r->GetPreviewCameraEntity(v));
            out[++n] = t;
        }
        // The main pipeline's secondary views (Render.addView): the same passes again through their
        // camera; id = Renderer::kSecondaryViewIdBase + slot.
        for (std::uint32_t slot = 1; slot < RHI::kMaxViews; ++slot)
        {
            const Renderer::ViewInfo vi = r->GetViewInfo(slot);
            if (!vi.live) continue;
            sol::table t = lua.create_table();
            t["id"]      = Renderer::kSecondaryViewIdBase + slot;
            t["slot"]    = slot;
            t["lease"]   = vi.lease;
            t["kind"]    = "view";
            t["name"]    = std::string(vi.name);
            t["width"]   = vi.width;
            t["height"]  = vi.height;
            t["renders"] = vi.renders;
            t["vramBytes"]        = vi.vramBytes;
            t["graphBytes"]       = vi.graphBytes;
            t["graphPlacedBytes"] = vi.graphPlacedBytes;
            out[++n] = t;
        }
        return out;
    });

    // Render.addView{camera = <entity> | "main", width, height, name, show} -> {slot, id, lease}
    LuaDoc::Fn(render, "Render", "addView", [w, r, mainThread](sol::this_state ts, sol::table opts) -> sol::table
    {
        if (std::this_thread::get_id() != mainThread)
            throw sol::error("Render.addView: wrong_thread: call it from the main thread");
        const sol::object cam = opts["camera"];
        bool followMain = false;
        Entity camera = NullEntity;
        if (cam.is<std::string>() && cam.as<std::string>() == "main")
            followMain = true;
        else if (cam.is<LuaEntity>())
        {
            camera = LuaId::ResolveLive(*w, cam.as<LuaEntity>());
            if (camera == NullEntity) throw sol::error("Render.addView: bad_camera: the entity is not alive");
            if (!w->GetComponent<CameraComponent>(camera) && !w->GetComponent<VirtualCameraComponent>(camera))
                throw sol::error("Render.addView: bad_camera: the entity has no Camera or VirtualCamera component");
            // Its pose is its GlobalTransform, which TransformSystem keeps only on an entity that has
            // one (App backfills the main camera's the same way); a script-made camera may have none.
            if (!w->HasComponent<GlobalTransform>(camera)) w->AddComponent<GlobalTransform>(camera, GlobalTransform{});
        }
        else
            throw sol::error("Render.addView: bad_camera: camera is a camera entity or \"main\"");
        const int width  = opts.get_or("width", 0);
        const int height = opts.get_or("height", 0);
        if (width < 16 || height < 16 || width > 8192 || height > 8192)
            throw sol::error("Render.addView: bad_size: width and height are 16 .. 8192");
        const std::string name = opts.get_or("name", std::string("view"));
        // What it shows: "game" (everything but the editor's overlays, the default), "scene" (everything) or
        // a list of RG::ShowFlag names.
        std::uint32_t show = RG::kShowAll & ~static_cast<std::uint32_t>(RG::ShowFlag::EditorOverlays);
        const sol::object showOpt = opts["show"];
        if (showOpt.is<std::string>())
        {
            const std::string preset = showOpt.as<std::string>();
            if (preset == "scene")     show = RG::kShowAll;
            else if (preset != "game") throw sol::error("Render.addView: bad_show: \"game\", \"scene\" or a list of flag names");
        }
        else if (showOpt.is<sol::table>())
        {
            show = 0;
            for (const auto& kv : showOpt.as<sol::table>())
            {
                RG::ShowFlag f{};
                if (!kv.second.is<std::string>() || !RG::ShowFlagFromName(kv.second.as<std::string>(), f))
                    throw sol::error("Render.addView: bad_show: not a show flag (" +
                                     RG::ShowFlagList(RG::kShowAll, ", ") + ")");
                show |= static_cast<std::uint32_t>(f);
            }
        }
        else if (showOpt.valid() && showOpt.get_type() != sol::type::lua_nil)
            throw sol::error("Render.addView: bad_show: \"game\", \"scene\" or a list of flag names");
        const std::uint32_t slot = r->AcquireView(name.c_str(), static_cast<std::uint32_t>(width),
                                                  static_cast<std::uint32_t>(height));
        if (slot == 0)
            throw sol::error("Render.addView: no_slot: every secondary view is taken, or the video memory budget "
                             "has no room for another");
        r->SetViewCamera(slot, camera, followMain, static_cast<std::uint32_t>(width),
                         static_cast<std::uint32_t>(height), show);
        sol::state_view lua(ts);
        sol::table out = lua.create_table();
        out["slot"]  = slot;
        out["id"]    = Renderer::kSecondaryViewIdBase + slot;
        out["lease"] = r->GetViewInfo(slot).lease;
        return out;
    },
        { .doc = "Adds a secondary view of the main pipeline: the scene rendered again, every frame, through "
                 "a camera entity (its transform and lens) or the main camera (\"main\"), at width x height, "
                 "into its own picture. It renders from the next frame on; Render.views() lists it (kind "
                 "\"view\") and a capture {view = id, lease = lease} reads its picture. show: \"game\" "
                 "(default: everything but the editor's overlays - grid, debug wires and icons, the selection "
                 "outline), \"scene\" (everything) or a list of show-flag names.",
          .params = { { .name = "opts", .doc = "{camera = entity | \"main\", width, height, name, "
                                               "show = \"game\" | \"scene\" | {flag names}}" } },
          .returns = "table", .returnsDoc = "{slot, id, lease}",
          .effect = LuaDoc::Effect::World,
          .errors = { "wrong_thread", "bad_camera", "bad_size", "bad_show", "no_slot" } });

    // Render.removeView(slot)
    LuaDoc::Fn(render, "Render", "removeView", [r, mainThread](std::uint32_t slot)
    {
        if (std::this_thread::get_id() != mainThread)
            throw sol::error("Render.removeView: wrong_thread: call it from the main thread");
        r->ReleaseView(slot);
    },
        { .doc = "Removes the secondary view in @p slot (Render.addView's slot); its resources go at the next "
                 "frame boundary.",
          .params = { { .name = "slot" } }, .effect = LuaDoc::Effect::World, .errors = { "wrong_thread" } });

    // Render.aliasPlan() -> { {name, pin, first, last, bytes, rtds, outsideBy}, ... }: the main graph's
    // memory-aliasing plan from its last Compile (RG::RenderGraph::AliasPlan).
    LuaDoc::Fn(render, "Render", "aliasPlan", [r, mainThread](sol::this_state ts) -> sol::table
    {
        if (std::this_thread::get_id() != mainThread)
            throw sol::error("Render.aliasPlan: wrong_thread: call it from the main thread");
        sol::state_view lua(ts);
        sol::table out = lua.create_table();
        int n = 0;
        for (const RG::RenderGraph::AliasRow& row : r->GetMainGraph().AliasPlan())
        {
            sol::table t = lua.create_table();
            t["name"]  = row.name;
            t["pin"]   = RG::RenderGraph::AliasPinName(row.pin);
            t["first"] = row.first;
            t["last"]  = row.last;
            t["bytes"] = static_cast<double>(row.bytes);
            t["rtds"]  = row.rtds;
            if (!row.outsideBy.empty()) t["outsideBy"] = row.outsideBy;
            out[++n] = t;
        }
        return out;
    },
        { .doc = "The main render graph's memory-aliasing plan from its last compile: for each graph texture its "
                 "pin (candidate = it could share a heap with textures whose lifetimes do not overlap; otherwise "
                 "why it keeps its own memory: imported, onDemand, outsideUse, firstUseLoads, crossFrame, unused), "
                 "the execution-order indices of the first and last pass that declares it, its placed size in "
                 "bytes, whether it is a render target or depth texture, and who uses it outside the graph.",
          .returns = "table",
          .returnsDoc = "{ {name, pin, first, last, bytes, rtds, outsideBy}, ... }",
          .errors = { "wrong_thread" } });

    // Render.viewMode([name]) -> name. The MAIN view's mode (the editor's View
    // Mode combo; Renderer::ViewMode), read or set by name - the meshlet views have
    // no other way in on Game.exe.
    render.set_function("viewMode", [r, mainThread](sol::this_state ts, sol::optional<std::string> name) -> std::string
    {
        static constexpr const char* kNames[] = {
            "lit", "unlit", "wireframe",
            "directdiffuse", "directspecular", "directtransmission", "indirectdiffuse", "indirectspecular",
            "directonly", "indirectonly", "physicalvisibility", "formshadow",
            "basecolor", "normal", "roughness", "metallic", "ao", "depth",
            "meshlets", "meshletlod",
            "skyvisibility", "iblonly",
        };
        static_assert(std::size(kNames) == static_cast<size_t>(Renderer::ViewMode::Count),
                      "Render.viewMode's names must mirror Renderer::ViewMode 1:1");
        (void)ts;
        if (std::this_thread::get_id() != mainThread)
            throw sol::error("Render.viewMode: wrong_thread: call it from the main thread");
        if (name)
        {
            for (size_t i = 0; i < std::size(kNames); ++i)
                if (*name == kNames[i])
                {
                    r->SetViewMode(static_cast<Renderer::ViewMode>(i));
                    return kNames[i];
                }
            std::string all;
            for (const char* n : kNames) { if (!all.empty()) all += ", "; all += n; }
            throw sol::error("Render.viewMode: bad_value: '" + *name + "' is not a view mode (" + all + ")");
        }
        const uint32_t cur = static_cast<uint32_t>(r->GetViewMode());
        return cur < std::size(kNames) ? kNames[cur] : "lit";
    });

    // Render.plan() -> { path = "Raster", lighting = "T1", tier = 1, ddgi = 1,
    //   ssgi = 0, ... }. The frame plan of the last frame rendered, every stage
    // of it (Renderer::LastRenderedPlan; Renderer/FramePlan.h): every decision
    // a [FRAMEPLAN] line prints, as a number (a bool is 0 / 1), plus the
    // path's and the tier's names - what runs, where Settings.where says what
    // was asked for.
    LuaDoc::Fn(render, "Render", "plan", [r, mainThread](sol::this_state ts) -> sol::table
    {
        if (std::this_thread::get_id() != mainThread)
            throw sol::error("Render.plan: wrong_thread: call it from the main thread");
        sol::state_view lua(ts);
        sol::table out = lua.create_table();
        const FramePlan::Decisions& plan = r->LastRenderedPlan();
        out["path"]     = std::string(FramePlan::PathName(plan.path));
        out["lighting"] = std::string(FramePlan::TierName(plan));
        FramePlan::ForEachDecision(plan, [](void* ctx, const char* name, uint32_t v)
                                   { (*static_cast<sol::table*>(ctx))[name] = v; }, &out);
        // Not a plan decision: the volumetric fog pass's predicate that frame
        // (the scene's fog intent capped by the player's allowance).
        out["fog"] = r->LastRenderedFog() ? 1u : 0u;
        return out;
    },
        { .doc = "The frame plan of the last frame rendered: path and lighting (the tier) as names, every other "
                 "decision of a [FRAMEPLAN] line as a number (0 / 1 for a switch) - ddgi, ssgi, rtr, rtShadow, rtao, "
                 "xegtao, pt, di, gi, tlas, ... - and fog (whether volumetric fog ran). A Settings.apply() or a "
                 "scene change shows here once a frame has rendered with it (a frame or two later).",
          .returns = "table", .errors = { "wrong_thread" } });

    // Render.ptStep(seconds) -> boolean. One step of a frozen world (Renderer::RequestWorldStep).
    LuaDoc::Fn(render, "Render", "ptStep", [r, mainThread](double seconds) -> bool
    {
        if (std::this_thread::get_id() != mainThread)
            throw sol::error("Render.ptStep: wrong_thread: call it from the main thread");
        return r->RequestWorldStep(static_cast<float>(seconds));
    },
        { .doc = "Advances a frozen world (raytracing.ptFreezeTime on Path Trace (Reference)) by one step: the next "
                 "frame's simulation and world clocks move by `seconds`, then the world holds again and the path "
                 "tracer restarts on the new state. False while the world is not frozen or for a step outside "
                 "(0, 1] seconds; Render.ptStatus().steps counts the steps taken.",
          .params = { { "seconds", "the step's length (0 < seconds <= 1; 1/fps for a sequence)" } },
          .returns = "boolean", .effect = LuaDoc::Effect::World, .errors = { "wrong_thread" } });

    // Render.ptStatus() -> { samples, resets, frozen, steps, stepPending, denoise, fadeSpp }
    LuaDoc::Fn(render, "Render", "ptStatus", [r](sol::this_state ts) -> sol::table
    {
        sol::state_view lua(ts);
        sol::table out = lua.create_table();
        out["samples"]     = r->GetPathTraceSamples();
        out["resets"]      = r->GetPathTraceResets();
        out["frozen"]      = r->IsWorldClockFrozen();
        out["steps"]       = r->GetWorldStepsDone();
        out["stepPending"] = r->IsWorldStepPending();
        out["denoise"]     = r->GetPTDenoise();
        out["fadeSpp"]     = r->GetPTDenoiseFadeSpp();
        const AutoExposurePass* ae = r->GetAutoExposurePass();
        out["exposureHeld"] = ae && ae->IsHolding();
        return out;
    },
        { .doc = "The path tracer's progress: samples per pixel of the image on screen and how many times it "
                 "restarted, whether the world clock is frozen (raytracing.ptFreezeTime on the reference path), "
                 "the frozen steps taken (Render.ptStep) and whether one is pending, and the preview denoiser's "
                 "setting and fade sample count.",
          .returns = "table" });

    // Render.exposure([{histogram = true, texture = true}]) -> { exposure, ev, auto, locked, holding, nonFinite
    //   [, histogram, minLogLuma, maxLogLuma] [, texture] }
    LuaDoc::Fn(render, "Render", "exposure", [r](sol::this_state ts, sol::optional<sol::table> opts) -> sol::table
    {
        sol::state_view lua(ts);
        sol::table out = lua.create_table();
        AutoExposurePass* ae = r->GetAutoExposurePass();
        if (!ae) return out;
        if (opts && opts->get_or("histogram", false))
        {
            std::array<uint32_t, AutoExposurePass::kHistogramBins> bins{};
            float lo = 0.0f, hi = 0.0f;
            if (ae->GetLastHistogram(bins, lo, hi))
            {
                sol::table h = lua.create_table(static_cast<int>(bins.size()), 0);
                for (size_t b = 0; b < bins.size(); ++b) h[b + 1] = bins[b];
                out["histogram"] = h;
                out["minLogLuma"] = lo;
                out["maxLogLuma"] = hi;
            }
        }
        if (opts && opts->get_or("texture", false))
        {
            float v = 0.0f;
            std::string why;
            if (ae->ReadExposureTexture(v, why)) out["texture"] = v;
        }
        ae->SetDebugReadbackEnabled(true);   // the readback ring lags a few frames behind
        out["exposure"]  = ae->GetLastExposure();
        out["ev"]        = ae->GetLastExposureEV();
        out["auto"]      = ae->IsEnabled();
        out["locked"]    = ae->IsLocked();
        out["holding"]   = ae->IsHolding();
        out["nonFinite"] = static_cast<double>(ae->GetLastNonFinitePixels());
        return out;
    },
        { .doc = "The main view's exposure as the GPU last wrote it (a readback a few frames old; the first call "
                 "arms it, so read again a frame or two later): exposure (the multiplier), ev (log2 of it, stops "
                 "from 1.0), auto (the histogram meter runs; false = the manual exposure), locked (Render.lockExposure), "
                 "holding (locked, or the path tracer's time freeze holds the world clock) and nonFinite (metered "
                 "pixels that were NaN / Inf, -1 = no reading). With {histogram = true} also histogram (256 weighted "
                 "counts of the last metered frame, bin b at log2 luma minLogLuma + b/255 * (maxLogLuma - minLogLuma); "
                 "absent until a frame has metered and read back) and the two range ends. With {texture = true} also "
                 "texture: the 1x1 exposure texture the upscalers are handed, read back synchronously (absent unless "
                 "FSR 2 or DLSS runs with pass.upscaler.engineExposure). 0 / false fields while no "
                 "auto-exposure pass exists.",
          .params = { { "options", "optional { histogram = true, texture = true }" } },
          .returns = "table" });

    // Render.lockExposure(on) -> boolean
    LuaDoc::Fn(render, "Render", "lockExposure", [r](bool on) -> bool
    {
        AutoExposurePass* ae = r->GetAutoExposurePass();
        if (!ae) return false;
        ae->SetLocked(on);
        return true;
    },
        { .doc = "Holds the auto-exposure at its current value (true) or releases it (false): no metering and no "
                 "adaptation, so a path-traced sequence's frames agree. The reference path tracer's time freeze "
                 "(raytracing.ptFreezeTime) holds it without this. False when the renderer has no auto-exposure pass.",
          .params = { { "on", "true holds the exposure, false releases it" } },
          .returns = "boolean", .effect = LuaDoc::Effect::World });

    // Render.meterExposure() -> boolean
    LuaDoc::Fn(render, "Render", "meterExposure", [r]() -> bool
    {
        AutoExposurePass* ae = r->GetAutoExposurePass();
        if (!ae) return false;
        ae->RequestMeter();
        return true;
    },
        { .doc = "Meters the next frame once and snaps the exposure to the result, whether it is held or not: the "
                 "way to set a held exposure from a converged path-traced frame (tools/pt_sequence.py does).",
          .returns = "boolean", .effect = LuaDoc::Effect::World });

    // Render.materialCosts([{sort = "slot" | "weight"}]) -> { {slot, pass, ps, entry, perm, defines, instructions,
    //   compiled, blob, packets, instances, entity, pixels, weight, coverageFrame}, ... }
    LuaDoc::Fn(render, "Render", "materialCosts", [w, r, drain, mainThread](sol::this_state ts,
                                                                            sol::optional<sol::table> opts) -> sol::table
    {
        if (std::this_thread::get_id() != mainThread)
            throw sol::error("Render.materialCosts: wrong_thread: call it from the main thread");
        bool byWeight = false;
        if (opts)
            for (const auto& kv : *opts)
            {
                const std::string key = kv.first.is<std::string>() ? kv.first.as<std::string>() : std::string();
                const std::string val = kv.second.is<std::string>() ? kv.second.as<std::string>() : std::string();
                if (key != "sort")
                    throw sol::error("Render.materialCosts: bad_option: unknown option '" + key + "' (sort)");
                if (val != "slot" && val != "weight")
                    throw sol::error("Render.materialCosts: bad_option: sort is \"slot\" or \"weight\"");
                byWeight = val == "weight";
            }
        // The draw list is written by the scene build: drain first, as explain does.
        if (drain) drain();
        sol::state_view lua(ts);
        std::vector<Renderer::MaterialCostRow> rows;
        r->CollectMaterialCosts(rows, /*details=*/true);
        std::vector<std::uint32_t> pixels;
        const std::uint64_t coverageFrame = r->DebugMaterialCoverage(pixels);
        auto pixelsOf = [&](const Renderer::MaterialCostRow& row) -> std::uint64_t
        { return row.slot < pixels.size() ? pixels[row.slot] : 0u; };
        if (byWeight && !pixels.empty())
            std::stable_sort(rows.begin(), rows.end(), [&](const auto& a, const auto& b)
                             { return pixelsOf(a) * a.instructions > pixelsOf(b) * b.instructions; });
        sol::table out = lua.create_table();
        int n = 0;
        for (const Renderer::MaterialCostRow& row : rows)
        {
            sol::table t = lua.create_table();
            t["slot"]         = row.slot;
            static const char* const kPass[] = { "gbuffer", "forward", "water" };   // MaterialCostRow::pass
            t["pass"]         = kPass[row.pass];
            t["ps"]           = row.file;
            t["entry"]        = row.entry;
            t["perm"]         = row.permBits;
            sol::table defines = lua.create_table();
            int d = 0;
            for (const PermutationDefine& def : kPermutationDefines)
                if ((row.permBits & def.bit) != 0) defines[++d] = def.name;
            t["defines"]      = defines;
            t["instructions"] = row.instructions;
            t["compiled"]     = row.compiled;
            if (!row.blob.empty()) t["blob"] = row.blob;
            t["packets"]      = row.packets;
            t["instances"]    = row.instances;
            if (row.entity != NullEntity) t["entity"] = LuaId::Out(*w, row.entity);
            if (!pixels.empty())
            {
                t["pixels"]        = pixelsOf(row);
                t["weight"]        = pixelsOf(row) * row.instructions;
                t["coverageFrame"] = coverageFrame;
            }
            out[++n] = t;
        }
        return out;
    },
        { .doc = "The material cost table of the last built frame (render-arch TL3.7): per material slot the main "
                 "view's opaque, water and translucent packets draw, the pixel shader that draws it and its static "
                 "instruction count (DXC reflection's InstructionCount - every branch of the permutation once, the "
                 "shader's size, not what a pixel executes). pass gbuffer (GBuffer.ps in the packet's permutation, "
                 "or the material's custom pixel shader), water (WaterSurface.ps or the custom one, WATER_SURFACE) "
                 "or forward (Transparent.ps or the custom one: the forward shader carries the whole lighting, the "
                 "deferred lighting pass is not in a gbuffer row); perm its "
                 "permutation bits and defines their names; blob the shader cache file it was loaded from; packets "
                 "and instances drawing it; entity one of them. pixels: the slot's visible pixels on the last frame "
                 "the debug target materialCost was made (shown or captured) and read back - a few frames later - "
                 "at coverageFrame, with weight = pixels * instructions; absent before. Sorted by slot, or by weight "
                 "(largest first) with {sort = \"weight\"}. materialCost paints the gbuffer and water rows on the "
                 "visible surfaces that wrote the GBuffer; shadingCost sums every fragment's entry per pixel.",
          .params = { { "opts", "{sort = \"slot\" | \"weight\"}", "table", true } },
          .returns = "table", .errors = { "wrong_thread", "bad_option" } });
}
