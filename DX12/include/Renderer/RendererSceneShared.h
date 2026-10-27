#pragma once
// RendererSceneShared.h - file-local helpers Renderer_Scene.cpp shares with the TUs
// split out of it: the material bits (MatBitCheck, ComputeMatBits) and the
// material-packing helpers (the MATERIAL_GPU_FLAGS table, the binding / runtime keys,
// HashMatParams, the linear-color cache).
//
// Private to those TUs: include it after the TU's own includes and its
// `using namespace DirectX;`. The helpers keep internal linkage, so every TU compiles
// its own copy and the per-candidate ones inline at their call sites (LTCG is off).
// They hold no state (BuildMaterialRuntimeKey's thread_local scratch only keeps its
// capacity), so the copies cannot disagree.

#include "ECS/Components.h"   // MaterialComponent
#include "Renderer/Renderer.h" // the MB_* material bits
#include "System/Check.h"      // ENGINE_CHECK_ONCE
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Reads a predicate from the cached bits and cross-checks it against the live
// material: a drifted cached bit becomes a named error counted in @p mismatchCounter
// (FrameStats::matBitsMismatches) instead of a silent classification change. The
// cross-check is scaffolding: the bits exist so the render side can stop reading `mc`.
static inline bool MatBitCheck(uint32_t& mismatchCounter,
                               uint16_t bits, uint16_t bit, bool live,
                               const char* name)
{
    const bool cached = (bits & bit) != 0;
    if (cached != live)
    {
        ++mismatchCounter;
        ENGINE_CHECK_ONCE(false,
            "[SPLITCHECK][MATBITS] %s: cached=%d live=%d - the per-candidate "
            "material predicate cached at gather time disagrees with the live "
            "MaterialComponent. A gather site is not calling ComputeMatBits, or "
            "the patch path let a material change through.",
            name, (int)cached, (int)live);
        return live;   // prefer the live answer while the scaffolding is up
    }
    return cached;
}

static uint16_t ComputeMatBits(const MaterialComponent* mc)
{
    if (!mc) return 0;
    uint16_t b = MB_HAS_MATERIAL;
    if (mc->IsAlphaTestEnabled())                    b |= MB_ALPHA_TEST;
    if (IsTransparentBlendMode(mc->userBlendMode))   b |= MB_TRANSPARENT;
    if (mc->IsWaterShadingModel())                   b |= MB_WATER;
    if (mc->useCustomShader)                         b |= MB_CUSTOM_SHADER;
    if (mc->HasPerFrameParams())                     b |= MB_FRAME_PARAMS;
    if (mc->rtUseStandardMaterial)                  b |= MB_AUTHORED_RT;
    if (mc->IsCastingShadow())                       b |= MB_CAST_SHADOW;
    if (mc->texMulAdd.x == 1.0f && mc->texMulAdd.y == 1.0f &&
        mc->texMulAdd.z == 0.0f && mc->texMulAdd.w == 0.0f)
        b |= MB_IDENTITY_UV;
    return b;
}

// ==== Material-packing helpers ====
namespace
{
    // The MaterialComponent flags that reach the GPU, as one list. WriteMaterialSlot packs
    // MaterialGPUData::materialFlags from it and HashMatParams masks flags with it, so a
    // material's binding key covers exactly the flag bits the GPU can observe. Do not pack a
    // MaterialComponent flag outside this table: a packed-but-unhashed flag lets two materials
    // share one slot, and toggling it shows nothing until another parameter changes.
    //   X(cpuFlag, gpuBit) - gpuBit must match shaders/material.hlsli MAT_FLAG_*.
#define MATERIAL_GPU_FLAGS(X)         \
    X(EXCLUDE_FROM_SSAO,      0)      \
    X(DISABLE_RECEIVE_SHADOW, 1)      \
    X(USE_VERTEXCOLORS,       2)      \
    X(NPR_BRIGHTNESS_CLAMP,   3)      \
    X(HAIR_MASK,              4)      \
    X(NPR_TOON_SHADOW_EDGE,   5)      \
    X(SOURCE_OUTLINE_EXEMPT,  7)

    constexpr uint32_t kMaterialGpuFlagMask =
#define MAT_GPU_FLAG_OR_(f, bit) MaterialComponent::f |
        MATERIAL_GPU_FLAGS(MAT_GPU_FLAG_OR_)
#undef MAT_GPU_FLAG_OR_
        0u;

    // Self-check of the table: a duplicated CPU flag or GPU bit collapses under OR, so
    // comparing popcount with the row count catches a copy-paste that would fold two
    // flags into one and drop a material feature from the binding key.
    constexpr int PopCount32(uint32_t v) { int n = 0; while (v) { n += static_cast<int>(v & 1u); v >>= 1; } return n; }

    constexpr int kMaterialGpuFlagRows =
#define MAT_GPU_ROW_(f, bit) 1 +
        MATERIAL_GPU_FLAGS(MAT_GPU_ROW_)
#undef MAT_GPU_ROW_
        0;
    constexpr uint32_t kMaterialGpuBitMask =
#define MAT_GPU_BIT_OR_(f, bit) (1u << (bit)) |
        MATERIAL_GPU_FLAGS(MAT_GPU_BIT_OR_)
#undef MAT_GPU_BIT_OR_
        0u;
    static_assert(PopCount32(kMaterialGpuFlagMask) == kMaterialGpuFlagRows,
                  "MATERIAL_GPU_FLAGS lists the same MaterialComponent flag twice");
    static_assert(PopCount32(kMaterialGpuBitMask) == kMaterialGpuFlagRows,
                  "MATERIAL_GPU_FLAGS lists the same shader bit twice");
    // GPU bits derived from something other than MaterialComponent::flags
    // live OUTSIDE the X-macro table (WriteMaterialSlot ORs them in after the
    // expansion). They must never collide with a table bit, and they reach
    // the binding key through the field they derive from (userBlendMode is
    // mixed in HashMatParams), not through kMaterialGpuFlagMask.
    static_assert((kMatGpuFlagDitheredCoverage & kMaterialGpuBitMask) == 0,
                  "MATERIAL_GPU_FLAGS claims the Dithered coverage bit (1<<6)");
    // The frame-record bit reaches the key through MaterialBindingKey::identity: only
    // an identity-keyed slot ever has it.
    static_assert((kMatGpuFlagFrameRecord & kMaterialGpuBitMask) == 0,
                  "MATERIAL_GPU_FLAGS claims the frame-record bit (1<<8)");

    // Recomputes the linear-color cache (24 powf) and clears GPU_LINEAR_CACHE_DIRTY; callers
    // run it only when that flag says the sRGB colors changed.
    static void RefreshLinearCache(const MaterialComponent& mc)
    {
        auto srgbToLinear = [](float c) { return std::powf(std::max(c, 0.f), 2.2f); };
        mc._linearBaseColor     = { srgbToLinear(mc.baseColor.x),
                                    srgbToLinear(mc.baseColor.y),
                                    srgbToLinear(mc.baseColor.z),
                                    mc.baseColor.w };
        mc._linearSpecularColor = { srgbToLinear(mc.specularColor.x),
                                    srgbToLinear(mc.specularColor.y),
                                    srgbToLinear(mc.specularColor.z),
                                    mc.specularColor.w };
        mc._linearEmissiveColor = { srgbToLinear(mc.emissiveColor.x),
                                    srgbToLinear(mc.emissiveColor.y),
                                    srgbToLinear(mc.emissiveColor.z),
                                    mc.emissiveColor.w };
        mc._linearNprDiffuseRamp = { srgbToLinear(mc.nprDiffuseRampColor.x),
                                     srgbToLinear(mc.nprDiffuseRampColor.y),
                                     srgbToLinear(mc.nprDiffuseRampColor.z) };
        mc._linearNprShadowRamp  = { srgbToLinear(mc.nprShadowRampColor.x),
                                     srgbToLinear(mc.nprShadowRampColor.y),
                                     srgbToLinear(mc.nprShadowRampColor.z) };
        mc._linearSubsurfaceColor = { srgbToLinear(mc.subsurfaceColor.x),
                                      srgbToLinear(mc.subsurfaceColor.y),
                                      srgbToLinear(mc.subsurfaceColor.z) };
        mc._linearAttenuationColor = { srgbToLinear(mc.attenuationColor.x),
                                       srgbToLinear(mc.attenuationColor.y),
                                       srgbToLinear(mc.attenuationColor.z) };
        const_cast<MaterialComponent&>(mc).flags &= ~MaterialComponent::GPU_LINEAR_CACHE_DIRTY;
    }

    // The running hash HashMatParams and HashMatScalars both fold into.
    struct MatHashMixer
    {
        uint64_t h = 0xcbf29ce484222325ULL;
        void mix(uint64_t u)
        {
            h ^= u + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            h *= 0x100000001b3ULL;
        }
        void mixF(float f) { uint32_t u; std::memcpy(&u, &f, 4); mix(u); }
    };

    // The scalar block of HashMatParams: every plain-value field it mixes before the
    // first string, in its order. Shared with HashMatScalars so the two hashes cannot
    // drift: a scalar added here reaches both the binding key and the volatile-snapshot
    // guard.
    static void MixMaterialScalars(MatHashMixer& m, const MaterialComponent& mc)
    {
        // Schema-driven: every Kind::Float row (PBR + NPR, including the gated
        // nprMinBrightness), so an inspector edit to any scalar invalidates the batch slot.
        // Colors are mixed component-wise below from the raw sRGB field the inspector edits.
        for (const auto& f : MaterialSchema::kMaterialPBRSchema)
            if (f.kind == MaterialSchema::Kind::Float)
                m.mixF(*MaterialSchema::FieldPtr(mc, f));

        m.mixF(mc.baseColor.x);     m.mixF(mc.baseColor.y);
        m.mixF(mc.baseColor.z);     m.mixF(mc.baseColor.w);
        m.mixF(mc.specularColor.x); m.mixF(mc.specularColor.y);
        m.mixF(mc.specularColor.z); m.mixF(mc.specularColor.w);
        m.mixF(mc.emissiveColor.x); m.mixF(mc.emissiveColor.y);
        m.mixF(mc.emissiveColor.z); m.mixF(mc.emissiveColor.w);

        // UV transform: not a schema row (hand-written float4 widget), so it is mixed
        // explicitly; without it two materials differing only in tiling share a slot.
        m.mixF(mc.texMulAdd.x); m.mixF(mc.texMulAdd.y);
        m.mixF(mc.texMulAdd.z); m.mixF(mc.texMulAdd.w);

        m.mix(static_cast<uint32_t>(mc.shaderType));
        // Blend mode also reaches the key through the permutation bits; mixed explicitly in
        // case a mode changes GPU state outside the permutation.
        m.mix(static_cast<uint32_t>(mc.userBlendMode));
        // Shadow cull mode picks the ShadowPass draw group / rasterizer cull but is not in
        // the permutation bits: without it two materials differing only here merge into one
        // batch, which takes the first instance's mode (the packet copies
        // first.shadowCullMode).
        m.mix(static_cast<uint32_t>(mc.shadowCullMode));
        // Domain + decal-authored params: decal-domain materials never batch as draws, but a
        // domain flip must never share a stale slot.
        m.mix(static_cast<uint32_t>(mc.domain));
        m.mix(mc.decalWriteMask);
        m.mixF(mc.decalAngleFadeStart);
        m.mix(static_cast<uint32_t>(mc.decalSortLayer));
        // Only flags written into MaterialGPUData belong in the key: the MATERIAL_GPU_FLAGS
        // table WriteMaterialSlot packs from. DIRTY / ASSET_DIRTY are runtime / editor state
        // and must not split identical assets into separate GPU slots. The bits packed
        // outside the table derive from fields keyed elsewhere (Dithered coverage from
        // userBlendMode, mixed above; the frame-record bit through the identity key).
        m.mix(mc.flags & kMaterialGpuFlagMask);

        // NPR colors
        m.mixF(mc.nprDiffuseRampColor.x); m.mixF(mc.nprDiffuseRampColor.y);
        m.mixF(mc.nprDiffuseRampColor.z);
        m.mixF(mc.nprShadowRampColor.x);  m.mixF(mc.nprShadowRampColor.y);
        m.mixF(mc.nprShadowRampColor.z);
        // Subsurface tint: a Color3 row, so the Kind::Float loop skips it; without it two
        // materials differing only in subsurface color share a slot.
        m.mixF(mc.subsurfaceColor.x); m.mixF(mc.subsurfaceColor.y);
        m.mixF(mc.subsurfaceColor.z);
        // Transmission's attenuation colour: a Color3 row too.
        m.mixF(mc.attenuationColor.x); m.mixF(mc.attenuationColor.y);
        m.mixF(mc.attenuationColor.z);

        // Custom-shader authored identity. The candidate's custom shader ids, resolved
        // lazily, are separate members of MaterialBindingKey.
        m.mix(mc.useCustomShader ? 1u : 0u);
        m.mix(mc.rtUseStandardMaterial ? 1u : 0u);
    }

    uint64_t BuildMaterialShaderKey(const MaterialComponent& material);
    uint64_t BuildMaterialRuntimeKey(const MaterialComponent& material, uint64_t shaderKey);

    // Hash of every render-affecting MaterialComponent field; keys batch-slot reuse.
    // Do not miss a field: two materials differing only there share a slot and the edit
    // is invisible.
    //
    // With kOrderKey the same walk also yields the candidate's order key
    // (DrawCandidate::orderKey): this hash's content minus every resource id. matHash ends
    // in the textures' bindless indices, and ordering by ids orders materials by
    // allocation, so one more texture created anywhere flips which of two coplanar layers
    // wins the depth test. The order key takes the engine textures' paths instead (the
    // binding's resourceKey, or BuildMaterialRuntimeKey when there is no binding) and the
    // custom textures without their index. matHash is the same value in both
    // instantiations.
    template <bool kOrderKey>
    static uint64_t HashMatParamsImpl(const MaterialComponent* mc,
                                      const Renderer::ResolvedMaterialRuntime& rt,
                                      uint32_t* orderKey)
    {
        if (!mc)
        {
            if constexpr (kOrderKey) *orderKey = 0;
            return 0;
        }
        MatHashMixer m;
        MixMaterialScalars(m, *mc);
        auto mix = [&](uint64_t u) { m.mix(u); };
        auto hashText = [](std::string_view text) {
            uint64_t value = 0xcbf29ce484222325ULL;
            for (const unsigned char c : text)
            {
                value ^= c;
                value *= 0x100000001b3ULL;
            }
            return value;
        };
        auto avalanche = [](uint64_t value) {
            value ^= value >> 30;
            value *= 0xbf58476d1ce4e5b9ULL;
            value ^= value >> 27;
            value *= 0x94d049bb133111ebULL;
            return value ^ (value >> 31);
        };

        // The scalar block (schema floats through rtUseStandardMaterial) is
        // MixMaterialScalars, mixed first and in the same order.
        mix(hashText(mc->customShaderPath));
        mix(hashText(mc->customVertexShaderPath));
        mix(static_cast<uint32_t>(mc->customShadingModel));
        // unordered_map iteration order differs between separately loaded copies: hash each
        // named entry alone and combine commutatively, so two equal .imat assets form the
        // same binding key.
        //
        // frameParams are deliberately not folded: they change every frame, and a material
        // with them is keyed by its identity (MaterialBindingKey::identity), so this hash
        // stays stable while the per-frame values travel in the b8 repack and the frame
        // record.
        mix(static_cast<uint32_t>(mc->customParams.size()));
        uint64_t customParamHash = 0;
        for (const auto& [name, val] : mc->customParams)
        {
            uint64_t entry = hashText(name);
            for (float f : val)
            {
                uint32_t bits;
                std::memcpy(&bits, &f, sizeof(bits));
                entry ^= static_cast<uint64_t>(bits) + 0x9e3779b97f4a7c15ULL
                       + (entry << 6) + (entry >> 2);
            }
            customParamHash += avalanche(entry);
        }
        mix(customParamHash);
        mix(static_cast<uint32_t>(mc->customTextures.size()));
        uint64_t customTextureHash  = 0;
        uint64_t customTextureOrder = 0;   // kOrderKey: the same entries, no index
        for (const auto& [name, tex] : mc->customTextures)
        {
            uint64_t entry = hashText(name);
            entry ^= hashText(tex.name) + 0x9e3779b97f4a7c15ULL
                   + (entry << 6) + (entry >> 2);
            entry ^= static_cast<uint64_t>(tex.uvset) << 32;
            if constexpr (kOrderKey) customTextureOrder += avalanche(entry);
            entry ^= static_cast<uint32_t>(rt.CustomTex(name).bindlessIndex);
            customTextureHash += avalanche(entry);
        }

        if constexpr (kOrderKey)
        {
            // Everything mixed so far is content. The engine textures by path:
            // resourceKey is BuildMaterialRuntimeKey over the normalised shader
            // and texture paths (refreshed every frame by the material sync);
            // computing it here when there is no binding gives one material
            // one key with or without one. A runtime override has no path, so
            // only WHICH slots are overridden is content - two overrides of
            // the same material tie here and the id fields after orderKey in
            // the sort keep them apart.
            MatHashMixer o = m;
            o.mix(customTextureOrder);
            uint32_t overridden = 0;
            if (rt.binding)
            {
                o.mix(rt.binding->resourceKey);
                for (int i = 0; i < MaterialComponent::TEXTURESLOT_COUNT; ++i)
                    if (rt.binding->overrideMask[i]) overridden |= 1u << i;
            }
            else
                o.mix(BuildMaterialRuntimeKey(*mc, BuildMaterialShaderKey(*mc)));
            o.mix(overridden);
            *orderKey = static_cast<uint32_t>(avalanche(o.h) >> 32);
        }
        mix(customTextureHash);

        // Every engine texture slot's bindless index: a partial set lets emissive / ramp /
        // AO swaps share a batch wrongly.
        for (int i = 0; i < MaterialComponent::TEXTURESLOT_COUNT; ++i)
            mix(static_cast<uint32_t>(rt.EngineTex(i).bindlessIndex));

        return m.h;
    }

    static uint64_t HashMatParams(const MaterialComponent* mc,
                                  const Renderer::ResolvedMaterialRuntime& rt)
    {
        return HashMatParamsImpl<false>(mc, rt, nullptr);
    }

    // A draw candidate's two material keys in one walk: returns matHash (the
    // batch / GPU-slot identity, HashMatParams' value) and writes the content
    // order key (DrawCandidate::orderKey).
    static uint64_t HashMatKeys(const MaterialComponent* mc,
                                const Renderer::ResolvedMaterialRuntime& rt,
                                uint32_t& orderKey)
    {
        return HashMatParamsImpl<true>(mc, rt, &orderKey);
    }

    // Guard of a volatile snapshot's params-only refresh: every plain value
    // HashMatParams keys on (the shared block), the plain fields it does not (reflection
    // stamp - a shader reload re-syncs customParamMeta; layer, sampler, camera source)
    // and the sizes of every string / map. A change that skipped SetDirty still falls
    // back to a whole copy unless it hides inside a same-length string. Structural edits
    // (texture or shader paths, custom bindings) are SetDirty's contract and reach the
    // refresh as matDirty.
    static uint64_t HashMatScalars(const MaterialComponent& mc)
    {
        MatHashMixer m;
        MixMaterialScalars(m, mc);
        m.mix(static_cast<uint32_t>(mc.customShadingModel));
        m.mix(mc.reflSyncStamp);
        m.mix(mc.layerMask);
        m.mix(static_cast<uint32_t>(mc.samplerDescriptor));
        m.mix(static_cast<uint32_t>(mc.cameraSource));
        m.mix(mc.customParamsLegacyFormat ? 1u : 0u);
        m.mix(mc.customParams.size());
        m.mix(mc.customTextures.size());
        m.mix(mc.customParamMeta.size());
        m.mix(mc.customShaderPath.size());
        m.mix(mc.customVertexShaderPath.size());
        for (int i = 0; i < MaterialComponent::TEXTURESLOT_COUNT; ++i)
        {
            m.mix(mc.textures[i].name.size());
            m.mix(mc.textures[i].uvset);
        }
        return m.h;
    }

    // Copies the customParams and frameParams values of src into the snapshot through
    // the map built at the last whole copy (src's iteration order -> the snapshot's
    // nodes; customParams first, then frameParams, which a PER_FRAME_PARAMS material
    // changes every frame). Returns false on any difference - a key added or removed, or
    // src rehashed into a new order; the caller then copies the whole component and
    // rebuilds the map, overwriting any partial write.
    using ParamMap = std::vector<std::pair<const std::string*, std::array<float, 4>*>>;
    static bool CopyMappedParamValues(const ParamMap& map, const MaterialComponent& src)
    {
        if (map.size() != src.customParams.size() + src.frameParams.size()) return false;
        size_t i = 0;
        for (const auto* params : { &src.customParams, &src.frameParams })
            for (const auto& [key, value] : *params)
            {
                const auto& [dstKey, dstValue] = map[i++];
                if (*dstKey != key) return false;
                *dstValue = value;
            }
        return true;
    }
    // After a whole copy: dst holds every key of src, so every find succeeds.
    static bool BuildParamMap(ParamMap& map, MaterialComponent& dst, const MaterialComponent& src)
    {
        map.clear();
        map.reserve(src.customParams.size() + src.frameParams.size());
        using Params = decltype(MaterialComponent::customParams);
        const std::pair<const Params*, Params*> maps[] = {
            { &src.customParams, &dst.customParams }, { &src.frameParams, &dst.frameParams } };
        for (const auto& [from, to] : maps)
            for (const auto& [key, value] : *from)
            {
                auto it = to->find(key);
                if (it == to->end()) { map.clear(); return false; }
                map.emplace_back(&it->first, &it->second);
            }
        return true;
    }

    // 64-bit hash helpers for the runtime-material identity key. BuildScene_SyncMaterials
    // recomputes it for every material every frame, so the authored texture / shader
    // layout folds into a uint64 instead of a std::string (no allocation). Collision odds
    // across a scene's layouts are ~n^2/2^64; the worst case is two layouts sharing one
    // TextureSystem ref set (a visual glitch), never a crash.
    inline uint64_t HashCombine(uint64_t h, uint64_t value)
    {
        return h ^ (value + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
    }

    // Fold one text part (optionally path-normalized) into the running hash,
    // length-prefixed so "ab"+"c" and "a"+"bc" cannot collide at a boundary.
    inline uint64_t HashTextPart(uint64_t h, std::string_view text, bool normalizePath)
    {
        h = HashCombine(h, text.size());
        uint64_t value = 0xcbf29ce484222325ULL;
        for (char c : text)
        {
            if (normalizePath)
            {
                if (c == '\\') c = '/';
                else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
            }
            value ^= static_cast<unsigned char>(c);
            value *= 0x100000001b3ULL;
        }
        return HashCombine(h, value);
    }

    uint64_t BuildMaterialShaderKey(const MaterialComponent& material)
    {
        uint64_t h = 0xcbf29ce484222325ULL;
        h = HashCombine(h, material.useCustomShader ? 1u : 0u);
        h = HashTextPart(h, material.customShaderPath,       /*normalizePath=*/true);
        h = HashTextPart(h, material.customVertexShaderPath, /*normalizePath=*/true);
        return h;
    }

    uint64_t BuildMaterialRuntimeKey(const MaterialComponent& material,
                                     uint64_t shaderKey)
    {
        uint64_t h = 0xcbf29ce484222325ULL;
        h = HashCombine(h, shaderKey);
        for (int slot = 0; slot < MaterialComponent::TEXTURESLOT_COUNT; ++slot)
            h = HashTextPart(h, material.textures[slot].name, /*normalizePath=*/true);

        // Sort custom binding names so the key does not depend on unordered_map iteration
        // order. The thread_local vector keeps its capacity across frames: no per-call
        // allocation.
        thread_local std::vector<const std::string*> t_customNames;
        t_customNames.clear();
        t_customNames.reserve(material.customTextures.size());
        for (const auto& [name, _texture] : material.customTextures)
            t_customNames.push_back(&name);
        std::sort(t_customNames.begin(), t_customNames.end(),
                  [](const std::string* a, const std::string* b) { return *a < *b; });
        h = HashCombine(h, t_customNames.size());
        for (const std::string* name : t_customNames)
        {
            h = HashTextPart(h, *name, /*normalizePath=*/false);
            h = HashTextPart(h, material.customTextures.at(*name).name,
                             /*normalizePath=*/true);
        }
        return h;
    }

} // namespace

// ==== Shared by PlanEmit and EmitBatches ====
// Both call these per candidate / per instance: defined inline here so every TU can
// inline them (LTCG is off).

#if WITH_HARNESS
// -surfacepagesverify's frame counter: PlanEmit increments it, the nest's oracle line
// prints it. An inline variable so the PlanEmit and EmitBatches TUs share one counter.
inline uint64_t g_surfacePagesFrame = 0;
#endif

// The GPU-TDR guard's bounds. EmitBatches is where the guard fires; ResolveBatchPlan
// (PlanEmit) must predict the skip, because a skipped packet never reaches the
// surface-cache gate and a page acquired for it would put the plan one page off the
// nest. One definition, so the prediction and the guard cannot drift apart.
constexpr uint32_t kMaxDrawVerts     = 64u * 1024u * 1024u; // 64M verts/draw
constexpr uint32_t kMaxDrawInstances = 1u  * 1024u * 1024u; // 1M instances

// Static lane: the instance record is a pure function of tracked scene state - the
// scene store snapshots it, the diff dirties it, and the emit-side shadow compare
// catches every field change. Everything else (skinned prev-pos ring, live dissolve
// params, untracked emitters: billboards / beams / afterimages) is rewritten per
// frame in the reserved dynamic segment.
inline bool Renderer::IsStaticLaneInstance(const DrawCandidate& c) const
{
    if (c.entity == NullEntity) return false;
    const uint32_t id = static_cast<uint32_t>(c.entity);
    const SceneItemStore& st = m_sceneStore;
    if (id >= st.tracked.size() || !st.tracked[id]) return false;
    if (st.keys[id].lane != 0) return false;
    if (c.prevPosElementBase != 0xFFFFFFFFu) return false;
    // Live dissolve params animate outside the diff: dynamic by definition.
    if (c.dissolveParams.x != 0.0f || c.dissolveParams.y != 0.0f ||
        c.dissolveParams.z != 0.0f || c.dissolveParams.w != 0.0f)
        return false;
    return true;
}

// Batch compatibility of two candidates. Read by pass A of ResolveBatchPlan
// (PlanEmit) and by the nest's boundary walk (EmitBatches), so it lives in neither TU;
// a static member because DrawCandidate is a nested type of Renderer. It touches no
// member.
//
// The prepared path compares adjacent candidates; the baseline / oracle compares every
// candidate with the run's first. Any unequal or NaN field breaks the run.
//
// __forceinline, not inline: MSVC does not inline this per-candidate call into the
// ~16 KB BuildScene_PlanEmit on its own (+5% brsPlanMs).
__forceinline bool Renderer::SameEmitBatch(const DrawCandidate& first, const DrawCandidate& cur) {
    return !(cur.filter        != first.filter        ||
            cur.perm          != first.perm          ||
            // A packet gets exactly ONE PSO, so a mirrored instance can
            // never ride along with an unmirrored one (see the field doc).
            cur.mirrored      != first.mirrored      ||
            cur.stencilRef    != first.stencilRef   ||
            // Implied by matHash already; kept local so the one-mode-per-packet
            // invariant is checkable at the break site (dp copies first's).
            cur.shadowCullMode != first.shadowCullMode ||
            cur.meshDescSlot  != first.meshDescSlot  ||
            // The packet carries one prevPosElementBase (root constant [3], copied from first)
            // for all its instances. The per-entity skinned descriptor slot already forces a
            // break before this can differ; checking it here keeps the one-base-per-packet
            // invariant visible at the break site.
            cur.prevPosElementBase != first.prevPosElementBase ||
            cur.texBaseColor  != first.texBaseColor  ||
            cur.texSurfaceMap != first.texSurfaceMap ||
            cur.texNormalMap  != first.texNormalMap  ||
            cur.matHash       != first.matHash       ||
            // A PER_FRAME_PARAMS material's slot is its own (MaterialBindingKey::identity) but matHash
            // omits its per-frame values, so this keeps two such components out of one packet even
            // when matHash matches. (A skinned face breaks on meshDescSlot anyway; a rigid one would
            // not.)
            (((cur.matBits | first.matBits) & MB_FRAME_PARAMS) != 0 && cur.mc != first.mc) ||
            cur.outlinePixels != first.outlinePixels ||
            cur.isPickingOutline != first.isPickingOutline ||
            cur.customPSID    != first.customPSID    ||
            cur.customVSID    != first.customVSID    ||
            // One DispatchMesh X per packet. Implied by meshDescSlot (+ skinned /
            // customVSID); kept local so the invariant is checkable here.
            cur.meshletBlocks != first.meshletBlocks ||
            cur.viewMask      != first.viewMask      ||
            cur.castShadow    != first.castShadow    ||
            cur.perObjectShadowCaster != first.perObjectShadowCaster ||
            cur.hairShadowCaster != first.hairShadowCaster ||
            cur.faceOverlay      != first.faceOverlay      ||
            cur.faceOverlayParams != first.faceOverlayParams ||
            cur.renderInMainPass != first.renderInMainPass);
}
