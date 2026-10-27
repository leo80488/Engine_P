// Renderer_Materials.cpp - the Renderer's materials on the GPU side: writing
// a material's GPU record (WriteMaterialSlot and the frame's queued writes),
// custom material bindings, the runtime texture / shader resolve and its
// releases, persistent material slots, the preview material runtime,
// blend-mode classification and registering material shaders (one
// DynamicShaderRegistry id that every pass library resolves).
// The stable instance slots are not here: EmitBatches stages a record per
// instance and with LTCG off only a same-TU call can inline, so they live in
// Renderer_EmitBatches.cpp (IsStaticLaneInstance, which PlanEmit calls too,
// is inline in RendererSceneShared.h).

#include "Renderer/Renderer.h"
#include "Diagnostics/Profiler.h"   // Mat.* (the first resolve of a custom shader)

// engine graphics / backend
#include "Graphics/DynamicShaderRegistry.h"   // the one dynamic shader registry
#include "Graphics/GraphicsDX12.h"
#include "Graphics/MaterialShaderDecl.h"     // what a material's source declares
#include "Graphics/ShadingStencil.h"   // RefFor - the lighting route from the model
#include "Renderer/MaterialPermutation.h"   // the material's draw permutation + reflection
#include "System/HostPolicy.h"

// render passes
#include "RenderGraph/RenderPass/GBufferPass.h"
#include "RenderGraph/RenderPass/TransparentPass.h"
#include "RenderGraph/RenderPass/WaterSurfacePass.h"
#include "RenderGraph/RenderPass/TranslucentFrontLayerPass.h"
#include "RenderGraph/RenderPass/SpotShadowPass.h"
#include "RenderGraph/RenderPass/PointShadowPass.h"
#include "RenderGraph/RenderPass/PerObjectShadowPass.h"

// ECS components + systems
#include "ECS/MaterialReflectionSync.h"
#include "ECS/MaterialSchema.h"
#include "ECS/VFXSpawnRequests.h"

// resource system
#include "Resource/MaterialSerializer.h"
#include "Resource/MeshLibIdentity.h"   // VFX mesh sec= (MeshKeyToHex)

// engine systems
#include "System/JobDefaults.h"
#include "System/CommandLine.h"
#include "System/TaskSystem.h"
#include "System/TaskParallelSort.h"
#include "System/TaskGraph.h"

// STL / DirectXMath
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <execution>
#include <cmath>
#include <string_view>
#include <type_traits>

using namespace DirectX;

// The material bits and the material-packing helpers Renderer_Scene.cpp and
// its splits share.
#include "Renderer/RendererSceneShared.h"

namespace
{
    // -matpack_dump (harness; Renderer::s_matPackDump): each DISTINCT pack line
    // once per process (at most 50000), so a run's material bindings read as a set:
    // the same binding packed again (every frame for a ring material) adds nothing,
    // and two builds compare line for line. The logger keeps 1023 characters of a
    // message, so a long line goes out in pieces that share the line's FNV-64:
    // "[MATPACK] <hash> <i>/<n> <text>"; the comparison joins them.
    void MatPackLine(const std::string& line)
    {
        static std::mutex                      s_mutex;
        static std::unordered_set<std::string> s_seen;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (s_seen.size() >= 50000 || !s_seen.insert(line).second) return;
        }
        uint64_t h = 1469598103934665603ull;
        for (const char c : line) { h ^= static_cast<uint8_t>(c); h *= 1099511628211ull; }
        char id[24];
        std::snprintf(id, sizeof(id), "%016llx", static_cast<unsigned long long>(h));
        constexpr size_t kPiece = 900;
        const size_t n = (line.size() + kPiece - 1) / kPiece;
        for (size_t i = 0; i < n; ++i)
            LOG_INFO("[MATPACK] %s %zu/%zu %s", id, i + 1, n, line.substr(i * kPiece, kPiece).c_str());
    }

    std::string MatPackVec(const float* v)
    {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%.6g,%.6g,%.6g,%.6g", v[0], v[1], v[2], v[3]);
        return buf;
    }

    // The two shader paths a line is about.
    std::string MatPackShaders(const MaterialComponent& m)
    {
        return "ps=" + (m.customShaderPath.empty() ? std::string("-") : m.customShaderPath) +
               " vs=" + (m.customVertexShaderPath.empty() ? std::string("-") : m.customVertexShaderPath);
    }

    // The bag after a Sync: every customParams / customTextures entry, sorted.
    std::string MatPackBag(const MaterialComponent& m)
    {
        std::vector<std::string> params, textures;
        for (const auto& [name, v] : m.customParams) params.push_back(name + "=" + MatPackVec(v.data()));
        for (const auto& [name, t] : m.customTextures) textures.push_back(name + "=" + (t.name.empty() ? "-" : t.name));
        std::sort(params.begin(), params.end());
        std::sort(textures.begin(), textures.end());
        std::string s = " params";
        for (const std::string& p : params) s += " " + p;
        s += " textures";
        for (const std::string& t : textures) s += " " + t;
        return s;
    }
}


// Contract at the declaration (Renderer.h). Walks customParams / customTextures
// in reflection order: it must match the shader-side indexing.
void Renderer::WriteMaterialSlot(Resource::MaterialGPUData* buf, uint32_t slot,
                                 const MaterialComponent* mc,
                                 const Renderer::ResolvedMaterialRuntime& rt,
                                 const ShaderReflect::Reflection* customRefl,
                                 bool frameRecord)
{
    if (!buf) return;
    Resource::MaterialGPUData& dst = buf[slot];
    if (mc)
    {
        if (mc->flags & MaterialComponent::GPU_LINEAR_CACHE_DIRTY)
            RefreshLinearCache(*mc);

        // Schema-driven scalar upload: every Kind::Float schema row with a gpuOffset
        // is copied CPU -> GPU here, so a new scalar material param is one row in
        // MaterialSchema.h (offsetof(MaterialGPUData, ...)) plus its material.hlsli
        // field. Colors are skipped (uploaded from the sRGB -> linear cache below);
        // nprMinBrightness is written below, gated to 0 for non-NPR materials.
        for (const auto& f : MaterialSchema::kMaterialPBRSchema)
        {
            if (f.gpuOffset == MaterialSchema::kNoGpu)     continue;
            if (f.kind     != MaterialSchema::Kind::Float) continue;
            *reinterpret_cast<float*>(reinterpret_cast<char*>(&dst) + f.gpuOffset)
                = *MaterialSchema::FieldPtr(*mc, f);
        }

        // Mask clip value: the loop copied the AUTHORED alphaRef, but the GPU clips
        // against the reconciled one - Unreal's 0.5 for Masked with alphaRef unset
        // (1.0), else the authored value (MaskClipValueFor() in Components.h says why
        // the authored field is never rewritten). Every clip site (custom_surface's
        // ALPHA_TEST block, Shadow.ps, ProbeCapture.ps) reads THIS field. Must stay
        // after the loop, or the loop clobbers it. The slot key is unaffected:
        // HashMatParams mixes both alphaRef (schema walk) and userBlendMode.
        dst.alphaRef = mc->MaskClipValue();

        // PBR colors from the linear cache: no powf on the hot path.
        dst.baseColor[0]      = mc->_linearBaseColor.x;
        dst.baseColor[1]      = mc->_linearBaseColor.y;
        dst.baseColor[2]      = mc->_linearBaseColor.z;
        dst.baseColor[3]      = mc->_linearBaseColor.w;
        dst.specularColor[0]  = mc->_linearSpecularColor.x;
        dst.specularColor[1]  = mc->_linearSpecularColor.y;
        dst.specularColor[2]  = mc->_linearSpecularColor.z;
        dst.specularColor[3]  = mc->_linearSpecularColor.w;
        dst.emissiveColor[0]  = mc->_linearEmissiveColor.x;
        dst.emissiveColor[1]  = mc->_linearEmissiveColor.y;
        dst.emissiveColor[2]  = mc->_linearEmissiveColor.z;
        dst.emissiveColor[3]  = mc->_linearEmissiveColor.w;

        // NPR mode and the gated / color fields (not in the generic loop).
        // nprMinBrightness is gated to 0 for non-toon materials so the NPR
        // brightness floor cannot act on a PBR pixel.
        const bool isNPR = (mc->customShadingModel == ShadingModel::Toon);
        dst.shaderType       = static_cast<uint32_t>(mc->shaderType);
        dst.nprMinBrightness = isNPR ? mc->nprMinBrightness : 0.0f;

        dst.nprDiffuseRampColor[0] = mc->_linearNprDiffuseRamp.x;
        dst.nprDiffuseRampColor[1] = mc->_linearNprDiffuseRamp.y;
        dst.nprDiffuseRampColor[2] = mc->_linearNprDiffuseRamp.z;
        dst.nprShadowRampColor[0]  = mc->_linearNprShadowRamp.x;
        dst.nprShadowRampColor[1]  = mc->_linearNprShadowRamp.y;
        dst.nprShadowRampColor[2]  = mc->_linearNprShadowRamp.z;
        dst._padNprShadow          = 0.0f;
        dst._padNprSss             = 0.0f;

        // Subsurface tint: a Kind::Color3 schema row, so like every colour it is
        // kNoGpu and uploads from the sRGB -> linear cache, not the raw authored field.
        dst.subsurfaceColor[0]     = mc->_linearSubsurfaceColor.x;
        dst.subsurfaceColor[1]     = mc->_linearSubsurfaceColor.y;
        dst.subsurfaceColor[2]     = mc->_linearSubsurfaceColor.z;

        // Transmission's attenuation colour: Kind::Color3, uploaded linear like the tint above.
        dst.attenuationColor[0]    = mc->_linearAttenuationColor.x;
        dst.attenuationColor[1]    = mc->_linearAttenuationColor.y;
        dst.attenuationColor[2]    = mc->_linearAttenuationColor.z;
        dst._transmissionPad0      = 0.0f;

        dst.paramCount = 14;  // nonzero: the engine fields are filled

        // UV transform (Inspector "UV Transform" Tiling/Offset): a straight copy of
        // MaterialComponent::texMulAdd; the shader applies uv' = uv * xy + zw in
        // MaterialUV(). Not a schema row: texMulAdd is a float4 with a hand-written
        // inspector widget, which the Kind::Float loop cannot carry.
        dst.uvTilingOffset[0] = mc->texMulAdd.x;
        dst.uvTilingOffset[1] = mc->texMulAdd.y;
        dst.uvTilingOffset[2] = mc->texMulAdd.z;
        dst.uvTilingOffset[3] = mc->texMulAdd.w;
        dst._nprEnergyPad0     = 0.0f;

        for (int s = 0; s < MaterialComponent::TEXTURESLOT_COUNT && s < Resource::kMaxTextureSlots; ++s)
            dst.textureHandleIds[s] = rt.EngineTex(s).bindlessIndex;
        // Clear the unused tail (the engine uses 6 of kMaxTextureSlots = 8: 5
        // unified-surface slots + OPACITYMAP) so no shader reads a stale handle.
        for (int s = MaterialComponent::TEXTURESLOT_COUNT; s < Resource::kMaxTextureSlots; ++s)
            dst.textureHandleIds[s] = -1;
        dst.textureCount = MaterialComponent::TEXTURESLOT_COUNT;

        // Per-material GPU flag bitmask read inside pixel shaders. Driven by
        // MATERIAL_GPU_FLAGS so HashMatParams hashes exactly this set.
        dst.materialFlags = 0u;
#define MAT_GPU_FLAG_PACK_(f, bit) \
        if (mc->flags & MaterialComponent::f) dst.materialFlags |= (1u << (bit));
        MATERIAL_GPU_FLAGS(MAT_GPU_FLAG_PACK_)
#undef MAT_GPU_FLAG_PACK_
        // Derived from the blend mode, not from flags: the deferred
        // ALPHA_TEST block and Shadow.ps switch to the dither threshold
        // on this bit. userBlendMode is in HashMatParams, so the slot
        // key already splits on it.
        if (mc->userBlendMode == BlendMode::Dithered)
            dst.materialFlags |= kMatGpuFlagDitheredCoverage;
        // The deferred toon reads this slot's head frame from its frame record.
        // Only the main emit asks, for a PER_FRAME_PARAMS material, whose slot key
        // carries its identity.
        if (frameRecord)
            dst.materialFlags |= kMatGpuFlagFrameRecord;
        // The custom pixel shader animates with the frame's time: the path tracer restarts where
        // it is on screen while the world clock runs.
        if (customRefl && customRefl->readsFrameTime)
            dst.materialFlags |= kMatGpuFlagReadsTime;

        // Custom-shader params / textures in reflection order, skipping reserved
        // cbuffers, structs and arrays.
        std::memset(dst.customParams,     0, sizeof(dst.customParams));
        for (int i = 0; i < Resource::kMaxCustomTextures; ++i) dst.customTextureIds[i] = -1;
        dst.customParamCount    = 0;
        dst.customTextureCount  = 0;
        // The shading model feeds the forward template's Unlit branch, the deferred
        // early-out, the deferred BRDF route and the GBuffer's RT4 packing - for every
        // material: Subsurface, Anisotropic etc. are ENGINE models that work off the
        // standard GBuffer the weak-default hook produces.
        // One exception: a non-custom Unlit material on the FORWARD route (not Opaque /
        // Masked / Dithered) uploads Standard. The forward Unlit branch expects the
        // hook to fill s.emissive + s.opacity, and the engine weak-default hook fills
        // emissive from the emissive map alone, so its base texture would render black;
        // it stays on lit forward. On the deferred route Unlit is kept: this is the
        // only writer of MaterialGPUData::customShadingModel, GPU consumers key on it
        // (NPRStabilize's Unlit exposure protection), and Lighting.ps's runtime Unlit
        // early-out returns the same flat albedo as the stencil-3 `#if UNLIT` variant.
        {
            uint32_t sm = static_cast<uint32_t>(mc->customShadingModel);
            const bool deferredRoute =
                (mc->userBlendMode == BlendMode::Opaque ||
                 mc->userBlendMode == BlendMode::Masked ||
                 mc->userBlendMode == BlendMode::Dithered);
            if (!mc->useCustomShader && !deferredRoute &&
                sm == static_cast<uint32_t>(ShadingModel::Unlit))
                sm = static_cast<uint32_t>(ShadingModel::Standard);
            dst.customShadingModel = sm;
        }

        // Where this shader keeps what the ENGINE reads by meaning (`//@engine <role>`):
        // a customParams[] / customTextureIds[] index, or -1 when it names none.
        int rampSlot = -1, faceParam = -1;
        int tintParam = -1, wrapParam = -1, strengthParam = -1;

        uint32_t texParamBits = 0;   // -matpack_dump: which customParams[] hold a bindless index
        if (customRefl && mc->useCustomShader)
        {
            uint32_t pi = 0;
            for (const auto& cb : customRefl->cbuffers)
            {
                // The b8 cbuffer ONLY, by register as well as by name - the selection the b8
                // packer, the Inspector and Sync make. The deferred lighting reads these
                // slots POSITIONALLY: an engine cbuffer at another register (sampler_heap.hlsli's
                // DynamicSamplerCB at b0 space9) taking slot 0 would shift every parameter
                // after it (foliage tint and face shadow read from the wrong slot).
                if (cb.bindPoint != 8u || cb.space != 0u) continue;
                if (MaterialReflectionSync::IsReservedCBuffer(cb.name.c_str())) continue;
                for (const auto& v : cb.vars)
                {
                    if (pi >= Resource::kMaxCustomParams) break;
                    if (v.type == ShaderReflect::VarType::Struct)    continue;
                    if (v.type == ShaderReflect::VarType::Float4x4)  continue;
                    if (v.elements > 0)                              continue;

                    // "Tex_*" slots mirror the resolved bindless index (-1 = unbound),
                    // matching the b8 CBV packing - never a stale zero, which would alias
                    // bindless slot 0. Still ++pi so the mirror slot index keeps matching
                    // declaration order.
                    if (v.name.rfind("Tex_", 0) == 0)
                    {
                        const MaterialRuntimeTextureView rtv = rt.CustomTex(v.name);
                        dst.customParams[pi][0] = (rtv.bindlessIndex >= 0)
                            ? static_cast<float>(rtv.bindlessIndex) : -1.0f;
                        texParamBits |= 1u << pi;
                        ++pi;
                        continue;
                    }

                    if (!v.engineRole.empty())
                    {
                        const int at = static_cast<int>(pi);
                        if      (v.engineRole == ShaderReflect::kEngineRoleFaceShadow)      faceParam     = at;
                        else if (v.engineRole == ShaderReflect::kEngineRoleFoliageTint)     tintParam     = at;
                        else if (v.engineRole == ShaderReflect::kEngineRoleFoliageWrap)     wrapParam     = at;
                        else if (v.engineRole == ShaderReflect::kEngineRoleFoliageStrength) strengthParam = at;
                    }

                    // A slot with a frame record keeps its per-frame values out of this
                    // record, which then never changes. Any other writer of a driven face -
                    // the preview, the path tracer's table - has no record, so it mirrors
                    // this frame's frameParams like the b8 does.
                    const std::array<float, 4>* val = nullptr;
                    if (frameRecord)
                    {
                        auto it = mc->customParams.find(v.name);
                        if (it != mc->customParams.end()) val = &it->second;
                    }
                    else
                        val = mc->FindGpuParam(v.name);
                    if (val)
                    {
                        dst.customParams[pi][0] = (*val)[0];
                        dst.customParams[pi][1] = (*val)[1];
                        dst.customParams[pi][2] = (*val)[2];
                        dst.customParams[pi][3] = (*val)[3];
                    }
                    ++pi;
                }
                if (pi >= Resource::kMaxCustomParams) break;
            }
            dst.customParamCount = pi;

            uint32_t ti = 0;
            for (const auto& b : customRefl->bindings)
            {
                if (b.type != ShaderReflect::ResourceType::Texture) continue;
                // Skip engine-bound names: they are not user-owned slots.
                if (MaterialReflectionSync::IsReservedTexture(b.name.c_str())) continue;
                // Custom textures live ONLY at space3 (mirrors MaterialReflectionSync)
                // so engine textures pulled in by a shared header can never leak in.
                if (b.space != 3u) continue;

                // Index by the REFLECTED register, never by iteration order: DXC
                // strips textures the shader never samples out of the reflection,
                // and a running counter would then shift every later slot. Shaders
                // read customTextureIds[N] meaning tN, so N must be bindPoint.
                // Slots no binding claims keep the -1 pre-fill above.
                const uint32_t dstSlot = b.bindPoint;
                if (dstSlot >= Resource::kMaxCustomTextures) continue;

                auto it = mc->customTextures.find(b.name);
                if (it != mc->customTextures.end())
                    dst.customTextureIds[dstSlot] = rt.CustomTex(b.name).bindlessIndex;
                if (dstSlot + 1u > ti) ti = dstSlot + 1u;
                if (b.engineRole == ShaderReflect::kEngineRoleToonRamp)
                    rampSlot = static_cast<int>(dstSlot);
            }
            dst.customTextureCount = ti;
        }

        // Engine-read fields. The lighting paths read the toon ramp, the face shadow
        // and the foliage tint / wrap / strength BY NAME; this is the one place that
        // knows where a custom shader keeps them. A shader with an `//@engine`
        // annotation gets exactly what it names (a role it does not name is absent);
        // one without keeps the positional convention the shipping shaders were
        // written against. On a plain material the custom arrays are the -1 / zero
        // pre-fill above, so the ramp comes out -1 and the face shadow zero.
        const bool explicitRoles = customRefl && mc->useCustomShader &&
                                   customRefl->engineRolesExplicit;
        if (!explicitRoles)
        {
            rampSlot      = 3;   // MAT_CUSTOM_TEX_FACE_RAMP
            faceParam     = 2;   // face_sdf.hlsli's V2 contract
            tintParam     = 0;
            wrapParam     = 1;
            strengthParam = 2;
        }
        dst.toonRampTex = (rampSlot >= 0) ? dst.customTextureIds[rampSlot] : -1;
        for (int c = 0; c < 4; ++c)
            dst.faceShadow[c] = (faceParam >= 0) ? dst.customParams[faceParam][c] : 0.0f;

        // Two-sided foliage on a custom shader with reflected params reads its own
        // parameters, not the named subsurface rows - resolved once here, not per
        // pixel: an unset (<= 0) wrap is 0.5 and an unset strength is 1.
        if (dst.customShadingModel == static_cast<uint32_t>(ShadingModel::TwoSidedFoliage) &&
            dst.customParamCount > 0)
        {
            const float wrap = (wrapParam     >= 0) ? dst.customParams[wrapParam][0]     : 0.0f;
            const float str  = (strengthParam >= 0) ? dst.customParams[strengthParam][0] : 0.0f;
            for (int c = 0; c < 3; ++c)
                dst.subsurfaceColor[c] = (tintParam >= 0) ? dst.customParams[tintParam][c] : 0.0f;
            dst.subsurfaceWrap     = (wrap > 0.0f) ? wrap : 0.5f;
            dst.subsurfaceStrength = (str  > 0.0f) ? str  : 1.0f;
        }

        // -matpack_dump: what this record carries for the custom shader - every
        // mirrored param, which texture slots are bound (a bindless index is not
        // stable across runs: bound or not), the engine-read fields derived from them.
        if (s_matPackDump && customRefl && mc->useCustomShader)
        {
            std::string s = "mirror " + MatPackShaders(*mc) + " count=" + std::to_string(dst.customParamCount) +
                            " texCount=" + std::to_string(dst.customTextureCount) +
                            " explicit=" + (explicitRoles ? "1" : "0") + " frameRecord=" + (frameRecord ? "1" : "0");
            // The names, by the same walk as the mirror loop above.
            std::vector<std::string> names;
            for (const auto& cb : customRefl->cbuffers)
            {
                if (cb.bindPoint != 8u || cb.space != 0u) continue;
                if (MaterialReflectionSync::IsReservedCBuffer(cb.name.c_str())) continue;
                for (const auto& v : cb.vars)
                    if (v.type != ShaderReflect::VarType::Struct && v.type != ShaderReflect::VarType::Float4x4 &&
                        v.elements == 0)
                        names.push_back(v.name);
            }
            for (uint32_t i = 0; i < dst.customParamCount; ++i)
                s += " p" + std::to_string(i) + ":" + (i < names.size() ? names[i] : std::string("?")) + "=" +
                     ((texParamBits & (1u << i)) ? std::string(dst.customParams[i][0] >= 0.0f ? "tex:bound" : "tex:unbound")
                                                 : MatPackVec(dst.customParams[i]));
            for (uint32_t i = 0; i < dst.customTextureCount; ++i)
                s += " t" + std::to_string(i) + "=" + (dst.customTextureIds[i] >= 0 ? "bound" : "-");
            s += std::string(" ramp=") + (dst.toonRampTex >= 0 ? "bound" : "-") + " face=" + MatPackVec(dst.faceShadow);
            if (dst.customShadingModel == static_cast<uint32_t>(ShadingModel::TwoSidedFoliage))
            {
                const float f[4] = { dst.subsurfaceColor[0], dst.subsurfaceColor[1], dst.subsurfaceColor[2],
                                     dst.subsurfaceWrap };
                s += " foliage=" + MatPackVec(f) + "," + std::to_string(dst.subsurfaceStrength);
            }
            MatPackLine(s);
        }
    }
    else
    {
        dst = {};
        dst.roughnessMin = 0.0f;  dst.roughnessMax = 0.5f;
        dst.metalnessMin = 0.0f;  dst.metalnessMax = 0.0f;
        dst.reflectance  = 0.5f;  // F0 = 0.16 * 0.5^2 = 0.04
        dst.baseColor[0] = dst.baseColor[1] =
        dst.baseColor[2] = dst.baseColor[3] = 1.0f;  // white
        dst.paramCount   = 9;
        // Identity UV transform: the `dst = {}` above would leave tiling at (0,0),
        // collapsing every UV to one texel.
        dst.uvTilingOffset[0] = dst.uvTilingOffset[1] = 1.0f;
        dst.uvTilingOffset[2] = dst.uvTilingOffset[3] = 0.0f;
    }
}

// Packs the frame's material writes on up to four CPU-frame lanes. Every CPU
// input is owned before crossing the SortAndEmit boundary: workers pack into
// ordinary CPU memory, and only the commit closures (in upload write order)
// hold the real targets. `verify` repacks from the live inputs and compares.
void Renderer::QueueFrameMaterialWrites(const std::vector<MaterialCpuWrite>& writes, bool verify)
{
    ZoneScopedN("FrameMaterial.Snapshot");
    struct Input {
        std::shared_ptr<const MaterialComponent> material; // retained immutable lease
        std::optional<RuntimeMaterialEntityBinding> binding;
        MaterialCpuWrite write;
    };
    using OutputBlock = FrameMaterialOutputBlock;
    static_assert(alignof(Resource::MaterialGPUData) <= alignof(OutputBlock));
    struct Payload {
        std::vector<Input> inputs;
        std::vector<RuntimeMaterialResource> resources;
        std::vector<ShaderReflect::Reflection> reflections;
        std::shared_ptr<std::vector<OutputBlock>> output;
    };
    auto payload = std::make_shared<Payload>();
    payload->inputs.resize(writes.size());
    // Only this owner's snapshot-building phase uses original pointer identities.
    // Workers receive owned values, never these lookup maps or borrowed pointers.
    std::unordered_map<const RuntimeMaterialResource*, size_t> resourceIndices;
    std::unordered_map<const ShaderReflect::Reflection*, size_t> reflectionIndices;
    for (const auto& source : writes) {
        if (source.runtime.resource) resourceIndices.try_emplace(source.runtime.resource, resourceIndices.size());
        if (source.ps) reflectionIndices.try_emplace(source.ps, reflectionIndices.size());
        if (source.vs) reflectionIndices.try_emplace(source.vs, reflectionIndices.size());
    }
    payload->resources.resize(resourceIndices.size());
    payload->reflections.resize(reflectionIndices.size());
    for (const auto& [source, index] : resourceIndices) payload->resources[index] = *source;
    for (const auto& [source, index] : reflectionIndices) payload->reflections[index] = *source;
    size_t outputBlocks = 0, outputBytes = 0, resourceRefs = 0, reflectionRefs = 0;
    for (const auto& source : writes) {
        outputBlocks += (size_t(source.bytes) + 63) / 64;
        outputBytes += source.bytes;
        resourceRefs += source.runtime.resource != nullptr;
        reflectionRefs += (source.ps != nullptr) + (source.vs != nullptr);
    }
    bool outputAllocated = false;
    payload->output = m_frameMaterialOutput.Acquire(outputBlocks, &outputAllocated);
    const uint32_t count = static_cast<uint32_t>(writes.size());
    const uint32_t lanes = (std::max)(1u, (std::min)(4u,
        (std::min)(TaskSystem::Get().GetWorkerCount(), count / 64u)));
    struct CommitData {
        std::vector<void*> targets;
        std::vector<std::vector<uint8_t>> expected;
    };
    auto commitData = std::make_shared<CommitData>();
    commitData->targets.resize(writes.size());
    if (verify) commitData->expected.resize(writes.size());
    const uint64_t generation = m_cpuFrame->Generation();
    const uint32_t uploadSlot = m_cpuFrame->UploadSlot();
    Sys::TaskHandle previousCommit;
    auto queueBatch = [&](uint32_t lane) {
        const uint32_t lo = uint64_t(count) * lane / lanes;
        const uint32_t hi = uint64_t(count) * (lane + 1) / lanes;
        m_cpuFrame->Submit([payload, lo, hi] {
            ZoneScopedN("FrameMaterial.Prepare");
            for (uint32_t i = lo; i < hi; ++i) RunMaterialCpuWrite(payload->inputs[i].write);
        });
        // Keep original upload write order, even if targets alias. This batch
        // waits for its own prepare and prior commit, not later CPU batches.
        previousCommit = m_cpuFrame->CommitStage([this, payload, commitData,
            generation, uploadSlot, lo, hi, verify] {
            ZoneScopedN("FrameMaterial.OwnerCommit");
            // Checked against the frame's LEASE, not live state: FrameViewWrite() and
            // m_gfx.GetUploadSlot() describe F+1 once main has moved on, and would report
            // a stale lease for a good commit whenever preparation runs off main.
            // FrameViewEmit() and m_frameUploadSlot name the frame in flight.
            // IsUploadSlotWritable() still reads the device: it is a device-side gate,
            // not a per-frame value, and the one term of the three that has to come from
            // the lease when the lane owns the commit.
            if (!ENGINE_CHECK(FrameViewEmit().currentFrame == generation &&
                              m_gfx.IsUploadSlotWritable() && m_frameUploadSlot == uploadSlot,
                              "Frame material commit has a stale generation/upload lease"))
                throw std::logic_error("Stale frame material commit");
            for (uint32_t i = lo; i < hi; ++i) {
                const auto& write = payload->inputs[i].write;
                if (verify && !ENGINE_CHECK(std::memcmp(write.target, commitData->expected[i].data(), write.bytes) == 0,
                    "Owned material snapshot differs from live-input packing"))
                    throw std::logic_error("Frame material byte mismatch");
                std::memcpy(commitData->targets[i], write.target, write.bytes);
            }
            if (verify)
                LOG_INFO("[FRAME_MATERIAL_STAGE] frame=%llu slot=%u begin=%u end=%u match=1",
                    static_cast<unsigned long long>(generation), uploadSlot, lo, hi);
        }, std::span<const Sys::TaskHandle>(&previousCommit, 1));
        if (verify)
            LOG_INFO("[FRAME_MATERIAL_RELEASE] frame=%llu lane=%u ready=%u total=%u",
                static_cast<unsigned long long>(generation), lane, hi, count);
    };
    static const bool s_copyMaterial = Sys::CommandLine::Get().Has("-framematerialcopy");
    size_t materialLeases = 0, materialCopies = 0;
    size_t baseCount = 0, blockOffset = 0;
    uint32_t nextLane = 0;
    for (size_t i = 0; i < writes.size(); ++i) {
        const auto& source = writes[i]; auto& dst = payload->inputs[i];
        if (source.material) {
            if (!ENGINE_CHECK(source.snapshotIndex < m_matSnapshots.size() &&
                m_matSnapshots[source.snapshotIndex].mat.Read() == source.material,
                "Frame material source is not its retained snapshot"))
                throw std::logic_error("Invalid frame material snapshot lease");
            // The base writer must never refresh a shared material from a worker.
            if (!ENGINE_CHECK(!source.baseRecord ||
                !(source.material->flags & MaterialComponent::GPU_LINEAR_CACHE_DIRTY),
                "Frame material color cache must be ready before leasing"))
                throw std::logic_error("Mutable frame material snapshot");
            if (s_copyMaterial) { dst.material = std::make_shared<MaterialComponent>(*source.material); ++materialCopies; }
            else { dst.material = m_matSnapshots[source.snapshotIndex].mat.Acquire(); ++materialLeases; }
        }
        if (source.runtime.binding) dst.binding = *source.runtime.binding;
        dst.write = source;
        dst.write.target = payload->output->data() + blockOffset;
        blockOffset += (size_t(source.bytes) + 63) / 64;
        dst.write.material = dst.material.get();
        dst.write.runtime = {dst.binding ? &*dst.binding : nullptr,
            source.runtime.resource ? &payload->resources[resourceIndices.at(source.runtime.resource)] : nullptr};
        dst.write.ps = source.ps ? &payload->reflections[reflectionIndices.at(source.ps)] : nullptr;
        dst.write.vs = source.vs ? &payload->reflections[reflectionIndices.at(source.vs)] : nullptr;
        commitData->targets[i] = source.target;
        baseCount += source.baseRecord;
        if (verify) {
            commitData->expected[i].resize(source.bytes);
            auto reference = source; reference.target = commitData->expected[i].data();
            RunMaterialCpuWrite(reference);
        }
        // Containers and output storage were sized before publication. Later
        // iterations write only other Input/target/expected elements; workers
        // retain owned leases and never read the unfinished suffix.
        if (nextLane < lanes && i + 1 == uint64_t(count) * (nextLane + 1) / lanes)
            queueBatch(nextLane++);
    }
    // Zero writes still queue one (empty) stage.
    if (count == 0) queueBatch(nextLane++);
    if (verify)
        LOG_INFO("[FRAME_MATERIAL_SNAPSHOT] writes=%zu resourceRefs=%zu resourceCopies=%zu reflectionRefs=%zu reflectionCopies=%zu outputAllocations=%u outputBytes=%zu paddedBytes=%zu",
            writes.size(), resourceRefs, payload->resources.size(), reflectionRefs, payload->reflections.size(), unsigned(outputAllocated), outputBytes, outputBlocks * 64);
    if (verify)
        LOG_INFO("[FRAME_MATERIAL_LEASE] leases=%zu copies=%zu match=1", materialLeases, materialCopies);
    if (verify)
        LOG_INFO("[FRAME_MATERIAL_BUFFER] allocated=%u requestedBytes=%zu retainedBytes=%zu",
            unsigned(outputAllocated), outputBlocks * 64, payload->output->size() * 64);
    m_cpuFrame->Seal([commitData, generation, uploadSlot, lanes, baseCount, verify] {
        ZoneScopedN("FrameMaterial.Publish");
        if (verify) {
            for (size_t i = 0; i < commitData->expected.size(); ++i)
                if (!ENGINE_CHECK(std::memcmp(commitData->targets[i], commitData->expected[i].data(),
                                  commitData->expected[i].size()) == 0,
                                  "Frame material commit differs from expected bytes"))
                    throw std::logic_error("Frame material upload mismatch");
            const size_t writes = commitData->targets.size();
            LOG_INFO("[MATERIAL_CPU_ORACLE] writes=%zu base=%zu b8=%zu lanes=%u earlyChunks=0 match=1",
                writes, baseCount, writes-baseCount, lanes);
            LOG_INFO("[FRAME_MATERIAL_COMMIT] frame=%llu slot=%u writes=%zu tasks=%u match=1",
                static_cast<unsigned long long>(generation), uploadSlot, writes, lanes);
        }
    });
}

void Renderer::RunMaterialCpuWrite(const MaterialCpuWrite& write)
{
    if (write.baseRecord) {
        // The field writer leaves reserved/padding bytes untouched. Reused CPU
        // staging must match fresh zeroed storage, independently of prior frames.
        std::memset(write.target, 0, write.bytes);
        WriteMaterialSlot(static_cast<Resource::MaterialGPUData*>(write.target), 0,
                          write.material, write.runtime, write.ps, write.frameRecord);
        return;
    }
    void* cbvCpu = write.target;
    const uint32_t cbSize = write.bytes;
    const auto* batchMat = write.material;
    const auto& rt = write.runtime;
    const ShaderReflect::Reflection* refls[] = {write.ps, write.vs};
    std::memset(cbvCpu, 0, cbSize);
    // Pack from BOTH reflections' b8 vars: each stage's reflection gives the
    // offsets of the members it uses, so the union covers vertex-only and
    // pixel-only tunables.
    for (const auto* r : refls)
    {
        if (!r) continue;
        for (const auto& cb : r->cbuffers)
        {
            if (cb.bindPoint != 8u || cb.space != 0u) continue;
            if (MaterialReflectionSync::IsReservedCBuffer(cb.name.c_str())) continue;
            for (const auto& v : cb.vars)
            {
                // "Tex_*" b8 params are texture SLOTS (VFX convention): pack the
                // resolved bindless index as a float VALUE, -1 = unbound so shader
                // fallbacks kick in (never 0, which would alias bindless slot 0).
                if (v.name.rfind("Tex_", 0) == 0)
                {
                    const MaterialRuntimeTextureView rtv =
                        rt.CustomTex(v.name);
                    const float idx = (rtv.bindlessIndex >= 0)
                        ? static_cast<float>(rtv.bindlessIndex) : -1.0f;
                    if (v.offset + sizeof(float) <= cbSize)
                        std::memcpy(static_cast<uint8_t*>(cbvCpu) + v.offset,
                                    &idx, sizeof(float));
                    continue;
                }
                // This frame's frameParams value when an engine system writes one
                // (the face's head frame), else the authored customParams one.
                const std::array<float, 4>* value = batchMat->FindGpuParam(v.name);
                if (!value) continue;

                // HLSL cbuffer `bool` is a 32-bit integer, not a float: the float bag's
                // 1.0f stores 0x3F800000, which tests non-zero but reads 1.06e9 in
                // arithmetic (`lerp(a, b, myBool)`, `x * myBool`). Write the real ABI.
                if (v.type == ShaderReflect::VarType::Bool)
                {
                    const uint32_t bv = ((*value)[0] != 0.0f) ? 1u : 0u;
                    if (v.offset + sizeof(uint32_t) <= cbSize)
                        std::memcpy(static_cast<uint8_t*>(cbvCpu) + v.offset,
                                    &bv, sizeof(uint32_t));
                    continue;
                }

                // Copy min(v.size, sizeof(*value)): values are stored as float[4].
                const uint32_t copy = (v.size < sizeof(*value))
                    ? v.size : static_cast<uint32_t>(sizeof(*value));
                if (v.offset + copy <= cbSize)
                    std::memcpy(static_cast<uint8_t*>(cbvCpu) + v.offset,
                                value->data(), copy);
            }
        }
    }

    // -matpack_dump: every b8 member at its offset, with the value packed and
    // where it came from (this frame's frameParams, the authored bag, or nothing:
    // the zero fill). A Tex_ member says bound or not.
    if (s_matPackDump && batchMat)
    {
        std::string s = "b8 " + MatPackShaders(*batchMat) + " size=" + std::to_string(cbSize);
        std::set<std::string> listed;   // a unified material's two stages share members
        for (const auto* r : refls)
        {
            if (!r) continue;
            for (const auto& cb : r->cbuffers)
            {
                if (cb.bindPoint != 8u || cb.space != 0u) continue;
                if (MaterialReflectionSync::IsReservedCBuffer(cb.name.c_str())) continue;
                for (const auto& v : cb.vars)
                {
                    if (!listed.insert(v.name).second) continue;
                    s += " " + v.name + "@" + std::to_string(v.offset);
                    if (v.name.rfind("Tex_", 0) == 0)
                    {
                        s += rt.CustomTex(v.name).bindlessIndex >= 0 ? "=tex:bound" : "=tex:unbound";
                        continue;
                    }
                    const bool frame = !batchMat->frameParams.empty() && batchMat->frameParams.count(v.name) != 0;
                    const std::array<float, 4>* value = batchMat->FindGpuParam(v.name);
                    s += value ? "=" + MatPackVec(value->data()) + (frame ? ":frame" : ":bag") : std::string("=0:none");
                }
            }
        }
        MatPackLine(s);
    }
}

namespace
{
    // The first time an empty custom-material slot samples its declared
    // //@default, said once per (texture, slot, kind) - the evidence the
    // annotation reached the binding and not just the reflection (pack
    // material_slots).
    void NoteTypedDefault(const ShaderReflect::ResourceBinding& b)
    {
        static std::mutex            s_mutex;
        static std::set<std::string> s_seen;
        static const char* const     kNames[] = { "white", "black", "grey", "normal" };
        const char* kind = kNames[static_cast<size_t>(b.texDefault)];
        const bool  srgb = b.colorSpace == ShaderReflect::TextureColorSpace::SRGB;
        const std::string key = b.name + "|" + std::to_string(b.bindPoint) + "|" + kind + (srgb ? "|srgb" : "");
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (!s_seen.insert(key).second) return;
        }
        LOG_INFO("[MATSLOT] empty slot t%u '%s' samples its //@default %s%s",
                 b.bindPoint, b.name.c_str(), kind, srgb ? " (sRGB)" : "");
    }
}

void Renderer::PackCustomMaterialBindings(CustomPackDest&                  dest,
                                          const MaterialComponent*         mat,
                                          const ResolvedMaterialRuntime&   rt,
                                          const ShaderReflect::Reflection* customRefl,
                                          uint32_t                         customVSID,
                                          bool                             isCustomShader,
                                          std::vector<MaterialCpuWrite>* deferredWrites)
{
    // Contract and the two destination modes: CustomPackDest (Renderer.h).
    // persistentCustom is ANDed with slotKeyed on purpose: every slot-keyed branch
    // below short-circuits on it, so forcing it false in caller-owned mode keeps
    // m_matSlotCustomPacked / m_matSlotCustomOversize / m_matSlotSrvBlock untouched
    // without a guard on each of them.
    const bool     slotKeyed        = dest.slot != CustomPackDest::kNoSlot;
    const uint32_t thisMatIdx       = slotKeyed ? dest.slot : 0u;
    const bool     persistentCustom = dest.persistentCustom && slotKeyed;
    const MaterialComponent* const batchMat = mat;

    // Reflections that drive the per-material custom CBV / texture table. A
    // unified single-file material is compiled for BOTH stages, and DXC strips
    // each stage's reflection to the vars THAT stage uses, so the b8 tunables can
    // live in the PS reflection (surface params), the VS reflection
    // (World-Position-Offset params), or both. Both stages' vars are packed into
    // ONE b8 buffer at their own reflected offsets, so two stages that both declare
    // b8 params must share one cbuffer layout. The texture table walks the PS
    // reflection, or the VS one for a VS-only material.
    const ShaderReflect::Reflection* reflPS = customRefl;
    const ShaderReflect::Reflection* reflVS =
        (customVSID > 0 && m_gbufferPass)
            ? (mat ? MaterialShaderReflection(m_gbufferPass->GetShaderLibrary(), customVSID, *mat)
                   : MaterialShaderInterface(m_gbufferPass->GetShaderLibrary(), customVSID))
            : nullptr;
    const ShaderReflect::Reflection* packRefl = reflPS ? reflPS : reflVS;
    const ShaderReflect::Reflection* refls[2] = { reflPS, reflVS };

    // The per-material CBV from reflection: the cbuffer at b8 space0 is the ONLY
    // slot the root signature binds for custom params; customParams[name] goes at
    // the reflected byte offset.
    // Persistent mode packs ONCE per slot ASSIGNMENT (the key folds every param
    // and resolved bindless index, so a key's content is immutable: any change
    // mints a new key -> new slot -> a fresh pack here). The rings remain (a) the
    // flag-off path and (b) the fallback for oversize b8 cbuffers / SRV-store
    // exhaustion; a fallback slot keeps m_matSlotCustomPacked == 0 and repacks on
    // every first-sight frame.
    const bool wantCustomPack = !persistentCustom ||
        thisMatIdx >= m_matSlotCustomPacked.size() ||
        !m_matSlotCustomPacked[thisMatIdx];
    if (batchMat && packRefl && isCustomShader && wantCustomPack)
    {
        bool persistentDst = persistentCustom &&
            thisMatIdx < m_matSlotCustomPacked.size() &&
            !m_matSlotCustomOversize[thisMatIdx];

        // Size the b8 cbuffer from whichever stage's reflection declares it (engine
        // cbuffers at other registers are skipped: they cannot hijack b8).
        uint32_t cbSize = 0;
        for (const auto* r : refls)
        {
            if (!r) continue;
            for (const auto& cb : r->cbuffers)
                if (cb.bindPoint == 8u && cb.space == 0u &&
                    !MaterialReflectionSync::IsReservedCBuffer(cb.name.c_str()) &&
                    cb.sizeBytes > cbSize)
                    cbSize = cb.sizeBytes;
        }
        if (cbSize > 0)
        {
            // Reported whether or not the pack succeeds: outCbvVA alone cannot tell
            // "declares no b8 cbuffer" (a legitimate custom shader) from "declares
            // one we could not write", and a caller-owned destination must refuse to
            // draw in the second case rather than bind a zeroed slice - a zeroed
            // "Tex_*" member is bindless index 0 (an unrelated texture), not the -1
            // fallback the shader is written against.
            dest.outCbBytes = cbSize;
            void*    cbvCpu = nullptr;
            uint64_t cbvGpu = 0;
            if (!slotKeyed)
            {
                // Caller-owned destination (the preview). Never the per-frame
                // ring below (game-half state whose cursor SortAndEmit owns), and
                // never the persistent store (keyed on a material slot the
                // preview does not have).
                if (cbSize <= dest.cbvCapacity)
                {
                    cbvCpu = dest.cbvCpu;
                    cbvGpu = dest.cbvGpu;
                }
                else
                {
                    (void)ENGINE_CHECK_ONCE(false,
                        "PackCustomMaterialBindings: reflected b8 cbuffer is "
                        "%u B but the caller's slice is only %u B; refusing to "
                        "write past it (the caller sees outCbvVA == 0 and must "
                        "not draw)", cbSize, dest.cbvCapacity);
                }
            }
            else if (persistentDst && cbSize <= MaterialCBVStore::kSlotStride)
            {
                cbvCpu = m_customMatCbvStore.SlotCpu(thisMatIdx);
                cbvGpu = m_customMatCbvStore.SlotVA(thisMatIdx);
            }
            else
            {
                if (persistentDst)
                {
                    // The reflected layout exceeds the fixed slot stride:
                    // this key stays on the per-frame ring for its life.
                    m_matSlotCustomOversize[thisMatIdx] = 1;
                    persistentDst = false;
                    LOG_WARNING("Renderer: custom material b8 cbuffer "
                                "(%u B) exceeds persistent slot stride "
                                "(%u B); slot %u falls back to the "
                                "per-frame ring",
                                cbSize, MaterialCBVStore::kSlotStride,
                                thisMatIdx);
                }
                auto slice = m_customMatCbvRing.Allocate(cbSize);
                cbvCpu = slice.cpu;
                cbvGpu = slice.gpu;
            }
            if (cbvCpu)
            {
                // Packed now, or queued for the frame's material lanes
                // (QueueFrameMaterialWrites); RunMaterialCpuWrite zero-fills first
                // so cbuffer holes are deterministic.
                const MaterialCpuWrite write{cbvCpu, cbSize, false, batchMat, rt, reflPS, reflVS};
                if (deferredWrites) deferredWrites->push_back(write);
                else RunMaterialCpuWrite(write);
                if (slotKeyed) m_matCustomCbvVA[thisMatIdx] = cbvGpu;
                dest.outCbvVA   = cbvGpu;
            }
            else
            {
                // Ring full: never leave a prior frame's (recycled)
                // ring VA behind.
                if (slotKeyed) m_matCustomCbvVA[thisMatIdx] = 0;
                dest.outCbvVA = 0;
            }
        }

        // The per-material texture descriptor table, indexed by the reflected space3
        // register, reserved names skipped. A persistent store block stays attached
        // to the material slot across key reuse and is rewritten only here, at
        // (re)assignment, which SweepPersistentMatSlots defers past frames in flight,
        // so no live frame observes the rewrite.
        uint64_t texCpu = 0;
        uint64_t texGpu = 0;
        uint32_t texInc = 0;
        if (!slotKeyed)
        {
            // Caller-owned 8-descriptor block; same reasoning as the CBV.
            texCpu = dest.texCpu;
            texGpu = dest.texGpu;
            texInc = dest.texInc;
        }
        else if (persistentDst)
        {
            uint32_t block = m_matSlotSrvBlock[thisMatIdx];
            if (block == ~0u)
            {
                block = m_customMatSrvStore.AllocateBlock();
                m_matSlotSrvBlock[thisMatIdx] = block;
            }
            if (block != ~0u)
            {
                const auto b = m_customMatSrvStore.GetBlock(block);
                texCpu = b.cpu.ptr;
                texGpu = b.gpu;
                texInc = b.descIncBytes;
            }
            else
            {
                // Store exhausted -> per-frame ring, and PIN the slot
                // there for the key's life: unpinned, the store would be
                // re-selected for the CBV on every first-sight repack - an
                // in-place memset + pack of a region in-flight frames still
                // read (this frame's store write is safe: no prior frame
                // references a freshly assigned slot).
                m_matSlotCustomOversize[thisMatIdx] = 1;
                persistentDst = false;
            }
        }
        if (texGpu == 0 && slotKeyed)
        {
            auto texSlice = m_customMatSrvRing.Allocate(
                GraphicsDX12::kCustomMatTextureSlots);
            texCpu = texSlice.cpu.ptr;
            texGpu = texSlice.gpu;
            texInc = texSlice.descIncBytes;
        }
        if (texGpu)
        {
            // MaterialSRVRing::Allocate hands back UNCLEARED descriptors (a static
            // 3-frame heap region: frames N and N+3 reuse the same slots), so a slot
            // not written this frame keeps a previous material's descriptor. After a
            // delete + reload a fresh material's custom textures are still loading
            // (bindlessIndex < 0) for >= 1 frame, the table would reference the deleted
            // model's deferred-freed SRVs, and the custom draw hangs the GPU
            // (DEVICE_HUNG, no page fault). Pre-fill every slot with the
            // renderer-lifetime default-white SRV so an unresolved slot samples white
            // until its texture loads.
            const uint64_t fallbackSrc = m_gbufferPass
                ? m_gfx.GetTextureSRVCpuHandle(m_gbufferPass->GetDefaultWhiteTexture())
                : 0;
            if (fallbackSrc)
                for (uint32_t s = 0; s < GraphicsDX12::kCustomMatTextureSlots; ++s)
                    m_gfx.CopyCbvSrvUavDescriptors(
                        texCpu + static_cast<uint64_t>(s) * texInc,
                        fallbackSrc, 1);

            // -matpack_dump: what each slot of the table holds - the
            // material's texture or the declared default.
            std::string texDump;
            if (s_matPackDump && batchMat) texDump = "tex " + MatPackShaders(*batchMat);
            for (const auto& b : packRefl->bindings)
            {
                if (b.type != ShaderReflect::ResourceType::Texture) continue;
                if (MaterialReflectionSync::IsReservedTexture(b.name.c_str())) continue;
                // Custom textures live ONLY in this table at space3 (root 36):
                // filter by register exactly like MaterialReflectionSync does.
                if (b.space != 3u) continue;

                // Bind by the REFLECTED register, never by iteration order: DXC
                // strips textures the shader never samples out of the reflection,
                // so a running counter mis-binds the whole table from the first
                // stripped entry on (a GPU hang). The gaps binding by register
                // leaves keep the default-white pre-fill above.
                const uint32_t dstSlot = b.bindPoint;
                if (dstSlot >= GraphicsDX12::kCustomMatTextureSlots) continue;

                const uint64_t dst = texCpu +
                    static_cast<uint64_t>(dstSlot) * texInc;

                // Until (or unless) a texture is assigned, the slot samples
                // what its declaration asks for (//@default black|grey|normal,
                // ShaderReflect::TextureDefault) instead of the white floor.
                if (b.texDefault != ShaderReflect::TextureDefault::White && m_gbufferPass)
                {
                    const uint64_t typed = m_gfx.GetTextureSRVCpuHandle(
                        m_gbufferPass->GetDefaultTexture(b.texDefault, b.colorSpace));
                    if (typed) m_gfx.CopyCbvSrvUavDescriptors(dst, typed, 1);
                }

                uint64_t src = 0;
                const MaterialRuntimeTextureView runtimeTexture =
                    rt.CustomTex(b.name);
                if (runtimeTexture.bindlessIndex >= 0)
                {
                    RHI::Texture t;
                    t.handleId = static_cast<uint32_t>(
                        runtimeTexture.bindlessIndex);
                    src = m_gfx.GetTextureSRVCpuHandle(t);
                }

                if (src != 0)
                {
                    m_gfx.CopyCbvSrvUavDescriptors(dst, src, 1);
                    ++dest.outTexResolved;
                }
                else if (b.texDefault != ShaderReflect::TextureDefault::White)
                    NoteTypedDefault(b);
                if (!texDump.empty())
                {
                    static const char* const kDefaults[] = { "white", "black", "grey", "normal" };
                    const auto it = batchMat->customTextures.find(b.name);
                    texDump += " t" + std::to_string(dstSlot) + ":" + b.name + "=" +
                               (src != 0 ? (it != batchMat->customTextures.end() ? it->second.name : std::string("?"))
                                         : std::string("default:") + kDefaults[static_cast<size_t>(b.texDefault)] +
                                           (b.colorSpace == ShaderReflect::TextureColorSpace::SRGB ? "/srgb" : ""));
                }
            }
            if (!texDump.empty()) MatPackLine(texDump);
            if (slotKeyed) m_matCustomTexHandle[thisMatIdx] = texGpu;
            dest.outTexHandle = texGpu;
        }
        else
        {
            // SRV ring full (store path never yields texGpu==0): same
            // stale-VA guard as the CBV above.
            if (slotKeyed) m_matCustomTexHandle[thisMatIdx] = 0;
            dest.outTexHandle = 0;
        }
        // Persistent content is valid only when BOTH regions landed in the
        // store; any ring fallback repacks at the next first sight.
        if (persistentCustom && thisMatIdx < m_matSlotCustomPacked.size())
            m_matSlotCustomPacked[thisMatIdx] = persistentDst ? 1u : 0u;
    }
    else if (persistentCustom && wantCustomPack &&
             thisMatIdx < m_matSlotCustomPacked.size())
    {
        // Non-custom key -> stable zero VAs (assignment already zeroed
        // them; kept idempotent). A custom key whose reflection has not
        // compiled yet stays unpacked and retries on every first sight
        // until GetDynamicReflection resolves.
        m_matCustomCbvVA[thisMatIdx]     = 0;
        m_matCustomTexHandle[thisMatIdx] = 0;
        m_matSlotCustomPacked[thisMatIdx] =
            isCustomShader ? 0u : 1u;
    }
}


// VFXSpawnSystem already created the entity (transform / attach / lifetime /
// default material); this adds only the mesh (MeshLibRef) and the authored material.
void Renderer::DrainMeshVFXSpawns(World& world)
{
    if (!m_meshLib) return;

    // Collect-then-resolve: AddComponent<MeshLibRef> below mutates pools, so we
    // drain the mailbox into a local list first to keep its view stable.
    std::vector<PendingMeshVFXSpawns::Req> reqs;
    world.ForEach<PendingMeshVFXSpawns>([&](Entity, PendingMeshVFXSpawns& mb){
        for (auto& r : mb.reqs) reqs.push_back(std::move(r));
        mb.reqs.clear();
    });

    for (auto& r : reqs)
    {
        if (!world.IsHandleValid(r.entity)) continue;   // VFX entity already gone
        const Entity e = r.entity.entity;
        if (r.meshPath.empty()) continue;

        // MeshLibrary::Load is path-deduplicated and refcounted, and nothing Releases
        // on VFX-entity destroy: the slot's refcount stays unbalanced until the world
        // reloads (m_meshLib teardown frees it). Dedup keeps it to one GPU buffer per
        // unique mesh path, not a per-spawn leak; the fix is a Release-on-destroy hook
        // in OnEntityDestroyed.
        Resource::Handle h = m_meshLib->Load(r.meshPath, m_gfx);
        if (!h.IsValid()) {
            LOG_WARNING("VFX mesh: failed to load '%s'", r.meshPath.c_str());
            continue;
        }

        // Default-constructed cache fields force a fresh descriptor resolve in
        // BuildRenderScene (cachedGeneration 0 != current generation).
        MeshLibRef ref;
        ref.libHandle = h;
        ref.meshId    = 0;                  // a one-mesh VFX library: its only entry
        // `sec=` names the entry by key.
        if (r.sectionKey != 0)
        {
            const int entry = m_meshLib->FindEntryBySectionKey(h, r.sectionKey);
            if (entry >= 0) ref.meshId = static_cast<uint32_t>(entry);
            else
                LOG_WARNING("VFX mesh: '%s' has no section %s - entry 0 drawn", r.meshPath.c_str(),
                            Resource::MeshKeyToHex(r.sectionKey).c_str());
        }
        world.AddComponent<MeshLibRef>(e, ref);

        // Load the authored material into the entity's pre-created
        // MaterialComponent (mirrors MeshSpawner). Empty path keeps the default.
        if (!r.materialPath.empty())
            if (auto* mc = world.GetComponent<MaterialComponent>(e)) {
                if (Resource::LoadMaterial(r.materialPath, *mc))
                    mc->SetDirty();
            }

        (void)r.castShadow;   // TODO: map to VisibilityComponent CastShadow flag
    }
}

void Renderer::SyncMaterialTextures(Entity e, const MaterialComponent& mc)
{
    if (!m_texSys || !m_resMgr) return;

    const uint64_t shaderKey  = BuildMaterialShaderKey(mc);
    const uint64_t desiredKey = BuildMaterialRuntimeKey(mc, shaderKey);
    auto entityBinding = m_entityMaterialRuntimeBindings.find(e);

    // Unchanged layout - nothing to rebind. PromoteRuntimeMaterialTextures()
    // (called once after the whole sync loop) refreshes this resource's views.
    if (entityBinding != m_entityMaterialRuntimeBindings.end() &&
        entityBinding->second.resourceKey == desiredKey)
        return;

    // Layout is new or changed. Acquire/create the new resource BEFORE releasing
    // the old one. Shared paths therefore transition TextureSystem refcount
    // 1->2->1 instead of briefly hitting zero, losing the path map and forcing a
    // GPU re-upload.
    auto [newResourceIt, inserted] =
        m_runtimeMaterialResources.try_emplace(desiredKey);
    RuntimeMaterialResource& resource = newResourceIt->second;
    if (inserted)
    {
        resource.shaderKey = shaderKey;
        for (int slot = 0; slot < MaterialComponent::TEXTURESLOT_COUNT; ++slot)
        {
            MatTexEntry& texture = resource.engineTextures[slot];
            texture.path = mc.textures[slot].name;
            texture.handle = texture.path.empty()
                ? Resource::kInvalidTextureHandle
                : m_texSys->Acquire(texture.path, *m_resMgr, m_gfx);
        }
        resource.customTextures.reserve(mc.customTextures.size());
        for (const auto& [name, authoredTexture] : mc.customTextures)
        {
            MatTexEntry texture;
            texture.path = authoredTexture.name;
            texture.handle = texture.path.empty()
                ? Resource::kInvalidTextureHandle
                : m_texSys->Acquire(texture.path, *m_resMgr, m_gfx);
            resource.customTextures.emplace(name, std::move(texture));
        }
    }
    ++resource.refCount;

    uint64_t previousKey = 0;
    bool     hasPrevious = false;
    if (entityBinding == m_entityMaterialRuntimeBindings.end())
    {
        RuntimeMaterialEntityBinding binding;
        binding.resourceKey = desiredKey;
        m_entityMaterialRuntimeBindings.emplace(e, std::move(binding));
        if (e >= m_matBoundBits.size()) m_matBoundBits.resize(e + 1, 0);
        m_matBoundBits[e] = 1;
    }
    else
    {
        previousKey = entityBinding->second.resourceKey;
        hasPrevious = true;
        entityBinding->second.resourceKey = desiredKey;
        entityBinding->second.overrideMask.fill(0);
    }
    // previousKey != desiredKey here (we are in the changed branch), so erasing
    // the old resource cannot invalidate newResourceIt/resource above.
    if (hasPrevious)
        ReleaseRuntimeMaterialResource(previousKey, /*deferRelease=*/false);
}

void Renderer::PromoteRuntimeMaterialTextures()
{
    m_matTexUnsettled = 0;
    m_matTexUnsettledKeys.clear();   // which ones (TexUnsettledTouches)
    if (!m_texSys) return;

    // Promote one texture; return true if it is still PENDING (a valid handle
    // that has not become Ready yet) so the caller keeps re-checking next frame.
    auto promote = [&](MatTexEntry& shared) -> bool
    {
        shared.view = {};
        if (shared.handle == Resource::kInvalidTextureHandle)
            return false;                        // empty slot - never pending
        if (!m_texSys->IsReady(shared.handle))
            return true;                         // loading - re-check next frame
        if (const RHI::Texture* texture = m_texSys->GetTexture(shared.handle))
        {
            shared.view.gpuHandle =
                m_gfx.GetTextureSRVGpuHandle(*texture);
            shared.view.bindlessIndex =
                static_cast<int32_t>(texture->handleId);
            shared.view.previewGpuHandle =
                m_gfx.GetTexturePreviewSrvGpuHandle(*texture);
        }
        return false;
    };

    // Once per shared resource (identical layouts share one
    // RuntimeMaterialResource): O(unique layouts), not O(entities). Once all of a
    // resource's textures resolve, its views are frozen (bindless indices are
    // stable per handle) and it is skipped; in steady state this loop touches
    // nothing.
    for (auto& [_key, resource] : m_runtimeMaterialResources)
    {
        if (resource.allTexturesResolved) continue;
        // Counted BEFORE promoting: on the frame a resource resolves, its
        // gpuHandles just changed - retained candidates must rebuild (not
        // patch) THIS frame too, so the cache re-snapshots the new handles.
        ++m_matTexUnsettled;
        m_matTexUnsettledKeys.insert(_key);
        bool anyPending = false;
        for (MatTexEntry& texture : resource.engineTextures)
            anyPending |= promote(texture);
        for (auto& [_name, texture] : resource.customTextures)
            anyPending |= promote(texture);
        resource.allTexturesResolved = !anyPending;
    }
}

// Does a cache holding @p entityToIdx (dense entity id -> row, ~0u =
// absent) bind any entity whose material resource is still settling?
bool Renderer::TexUnsettledTouches(const std::vector<uint32_t>& entityToIdx) const
{
    for (Entity e : m_matTexUnsettledEntities)
    {
        const uint32_t id = static_cast<uint32_t>(e);
        if (id < entityToIdx.size() && entityToIdx[id] != 0xFFFFFFFFu) return true;
    }
    return false;
}

Renderer::RuntimeMaterialResource*
Renderer::FindRuntimeMaterialResource(Entity e)
{
    const auto binding = m_entityMaterialRuntimeBindings.find(e);
    if (binding == m_entityMaterialRuntimeBindings.end()) return nullptr;
    const auto resource =
        m_runtimeMaterialResources.find(binding->second.resourceKey);
    return resource != m_runtimeMaterialResources.end()
        ? &resource->second : nullptr;
}

const Renderer::RuntimeMaterialResource*
Renderer::FindRuntimeMaterialResource(Entity e) const
{
    const auto binding = m_entityMaterialRuntimeBindings.find(e);
    if (binding == m_entityMaterialRuntimeBindings.end()) return nullptr;
    const auto resource =
        m_runtimeMaterialResources.find(binding->second.resourceKey);
    return resource != m_runtimeMaterialResources.end()
        ? &resource->second : nullptr;
}

// Resolve an entity's runtime material binding + shared resource in ONE pair
// of map lookups. Thread-safe (const, read-only): the parallel mesh gather
// calls this once per candidate, then reads every slot off the returned
// handle instead of a 2-lookup resolve per texture.
Renderer::ResolvedMaterialRuntime Renderer::ResolveMaterialRuntime(Entity e) const
{
    ResolvedMaterialRuntime rt;
    const auto binding = m_entityMaterialRuntimeBindings.find(e);
    if (binding == m_entityMaterialRuntimeBindings.end()) return rt;
    rt.binding = &binding->second;
    const auto resource =
        m_runtimeMaterialResources.find(binding->second.resourceKey);
    if (resource != m_runtimeMaterialResources.end())
        rt.resource = &resource->second;
    return rt;
}

MaterialRuntimeTextureView
Renderer::ResolvedMaterialRuntime::EngineTex(uint32_t slot) const
{
    if (slot >= MaterialComponent::TEXTURESLOT_COUNT) return {};
    if (binding && binding->overrideMask[slot])
        return binding->engineOverrides[slot];
    return resource ? resource->engineTextures[slot].view
                    : MaterialRuntimeTextureView{};
}

MaterialRuntimeTextureView
Renderer::ResolvedMaterialRuntime::CustomTex(std::string_view name) const
{
    if (!resource) return {};
    // Heterogeneous find: no std::string materialized per query (the
    // container's transparent hasher).
    const auto texture = resource->customTextures.find(name);
    return texture != resource->customTextures.end()
        ? texture->second.view : MaterialRuntimeTextureView{};
}

// Single-slot IMaterialRuntimeLookup accessors (editor / DDGI / non-batched
// callers). These resolve per call; the per-draw gather resolves ONCE via
// ResolveMaterialRuntime and reads many slots off the handle.
MaterialRuntimeTextureView Renderer::GetMaterialTextureRuntime(
    Entity entity, uint32_t slot) const
{
    return ResolveMaterialRuntime(entity).EngineTex(slot);
}

MaterialRuntimeTextureView Renderer::GetMaterialCustomTextureRuntime(
    Entity entity, std::string_view bindingName) const
{
    return ResolveMaterialRuntime(entity).CustomTex(bindingName);
}

size_t Renderer::MaterialBindingKeyHash::operator()(
    const MaterialBindingKey& key) const noexcept
{
    uint64_t h = key.paramsHash ^ 0x9e3779b97f4a7c15ULL;
    auto mix = [&](uint64_t value) {
        h ^= value + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    };
    mix(key.texBaseColor);
    mix(key.texSurfaceMap);
    mix(key.texNormalMap);
    mix((static_cast<uint64_t>(key.customPSID) << 32) | key.customVSID);
    mix(key.hasMaterial ? 1u : 0u);
    mix(key.identity);
    return static_cast<size_t>(h);
}

Renderer::MaterialBindingKey Renderer::BindingKeyOf(const DrawCandidate& c)
{
    MaterialBindingKey key{ c.matHash, c.texBaseColor, c.texSurfaceMap, c.texNormalMap,
                            c.customPSID, c.customVSID, (c.matBits & MB_HAS_MATERIAL) != 0 };
    // A PER_FRAME_PARAMS material gets a slot of its own: the slot carries
    // this frame's b8 values and frame record.
    if ((c.matBits & MB_FRAME_PARAMS) && c.mc)
        key.identity = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(c.mc));
    return key;
}

// Contract at the declaration; why a persistent per-key slot is safe
// (immutable per-key content): m_persistentMatSlots. On overflow the
// caller degrades to slot 0.
uint32_t Renderer::AcquirePersistentMatSlot(
    const MaterialBindingKey& key, uint32_t frameSlot,
    bool& outFirstSight, bool& outNeedsBaseWrite, bool& outSlotValid)
{
    const uint8_t frameBit = static_cast<uint8_t>(1u << frameSlot);

    auto it = m_persistentMatSlots.find(key);
    if (it != m_persistentMatSlots.end())
    {
        PersistentMatSlot& s = it->second;
        outSlotValid   = true;
        outFirstSight  = (s.lastSeenEpoch != m_materialFrameEpoch);
        outNeedsBaseWrite = outFirstSight && !(s.frameMask & frameBit);
        if (outFirstSight)
        {
            s.lastSeenEpoch = m_materialFrameEpoch;
            s.frameMask |= frameBit;   // this frameSlot's buffer will hold it after the write
        }
        return s.slot;
    }

    // New key. Prefer a slot reclaimed by the end-of-frame GC (an evicted orphan)
    // over bumping the high-water: this keeps the map bounded to live keys.
    uint32_t slot;
    if (!m_freeMatSlots.empty())
    {
        slot = m_freeMatSlots.back();
        m_freeMatSlots.pop_back();
    }
    else if (m_persistentMatSlotNext < m_maxMaterials)
    {
        slot = m_persistentMatSlotNext++;
    }
    else
    {
        // Genuine overflow: > m_maxMaterials DISTINCT materials live in one frame
        // (the GC already recycles per-frame churn). Degrade to slot 0, the RESERVED
        // neutral no-material record, so overflow draws render default PBR instead
        // of aliasing a live material. Do NOT arm a reset: it would clear + rebuild
        // every frame and sawtooth, and the scene would just overflow again.
        outSlotValid = false;
        outFirstSight = true;
        outNeedsBaseWrite = false;      // never overwrite the reserved slot
        return 0u;
    }

    // Fresh assignment: whatever the previous key packed into this slot's
    // persistent CBV / SRV store region is stale. Drop the stable VAs and force a
    // repack; the slot only reaches here >= kMatSlotReuseLagEpochs after its
    // previous key retired (SweepPersistentMatSlots), so no in-flight frame
    // still reads the region about to be rewritten. Unconditional (not gated on
    // m_persistentCustomMatStore) so a flag toggle can never resurrect stale
    // packed state; harmless in ring mode (the arrays are zero-filled per frame).
    if (slot < m_matSlotCustomPacked.size())
    {
        m_matSlotCustomPacked[slot]   = 0;
        m_matSlotCustomOversize[slot] = 0;
    }
    if (slot < m_matCustomCbvVA.size())     m_matCustomCbvVA[slot] = 0;
    if (slot < m_matCustomTexHandle.size()) m_matCustomTexHandle[slot] = 0;

    PersistentMatSlot s;
    s.slot          = slot;
    s.frameMask     = frameBit;   // reused slots start fresh -> all 3 buffers rewritten
    s.lastSeenEpoch = m_materialFrameEpoch;
    m_persistentMatSlots.emplace(key, s);
    ++m_matSlotWindowNewKeys;
    outSlotValid      = true;
    outFirstSight     = true;
    outNeedsBaseWrite = true;
    return s.slot;
}

// The ONLY producer of material copy work, driven by AcquirePersistentMatSlot's
// needsBaseWrite (true exactly when this frameSlot's staging buffer did not
// hold the key's content): in steady state nothing is copied. Slots are handed
// out in emit order, so a cold frame's writes are dense and coalesce into a
// few CopyBufferRegions; src offset == dst offset because staging is indexed
// by the same persistent slot.
void Renderer::StageMaterialSlot(uint32_t frameSlot, uint32_t slot)
{
    if (!MaterialGpuBufferActive()) return;
    if (MatUploadWrite().fullInit)      return;   // the pending full blit supersedes

    if (!MatUploadWrite().ranges.empty())
    {
        MaterialCopyRange& last = MatUploadWrite().ranges.back();
        if (last.frameSlot == frameSlot && last.slot + last.count == slot)
        {
            ++last.count;
            return;
        }
    }
    // Fragmentation guard: a frame built but never rendered leaves ranges
    // undrained, and pathological slot ordering can fragment them. Past the
    // point where per-slot copies stop being cheaper than one table blit,
    // fall back to the full copy (still correct: staging is a complete table).
    if (MatUploadWrite().ranges.size() >= size_t(m_maxMaterials) / 4)
    {
        MatUploadWrite().ranges.clear();
        MatUploadWrite().fullInit = true;
        return;
    }
    MatUploadWrite().ranges.push_back({ frameSlot, slot, 1u });
}

// End-of-frame GC (contract at the declaration): every key not seen this
// frame is evicted and its slot parked for reuse.
uint32_t Renderer::SweepPersistentMatSlots()
{
    // Liveness: SortAndEmit acquires the key of every batch it draws, every frame
    // (the emit nest, or ResolveBatchPlan's pass B for it), and
    // AcquirePersistentMatSlot stamps lastSeenEpoch, so the lastSeenEpoch test
    // below is an exact refcount over this frame's draws - the persistent store
    // needs no separate candidate refcount.

    // Retire the deferred-reuse queue first. A parked slot's persistent
    // CBV/SRV store content may still be read by in-flight GPU frames; it
    // re-enters the free list only kMatSlotReuseLagEpochs after retirement, so
    // the rewrite-at-reassignment in AcquirePersistentMatSlot is never observed.
    {
        size_t w = 0;
        for (size_t r = 0; r < m_matSlotsPendingReuse.size(); ++r)
        {
            const auto& p = m_matSlotsPendingReuse[r];
            if (m_materialFrameEpoch - p.second >= kMatSlotReuseLagEpochs)
                m_freeMatSlots.push_back(p.first);
            else
                m_matSlotsPendingReuse[w++] = p;
        }
        m_matSlotsPendingReuse.resize(w);
    }

    // Deferred reuse is also mandatory with the single DEFAULT-heap material
    // table: reassigning a slot rewrites content in-flight frames may still draw
    // from (they all read the same DEFAULT copy, so per-frameSlot staging does
    // not shield them).
    const bool deferReuse = (m_persistentCustomMatStore || MaterialGpuBufferActive())
                            && m_retainMaterialSlots;
    for (auto it = m_persistentMatSlots.begin(); it != m_persistentMatSlots.end(); )
    {
        if (it->second.lastSeenEpoch != m_materialFrameEpoch)
        {
            if (deferReuse)
                m_matSlotsPendingReuse.emplace_back(it->second.slot,
                                                    m_materialFrameEpoch);
            else
                m_freeMatSlots.push_back(it->second.slot);
            it = m_persistentMatSlots.erase(it);
        }
        else
        {
            ++it;
        }
    }
    return static_cast<uint32_t>(m_persistentMatSlots.size());
}

int Renderer::GetMaterialRuntimeCustomPSID(Entity entity) const
{
    const RuntimeMaterialResource* resource =
        FindRuntimeMaterialResource(entity);
    if (!resource) return -1;
    const auto shader = m_runtimeMaterialShaders.find(resource->shaderKey);
    return shader != m_runtimeMaterialShaders.end()
        ? shader->second.customPSID : -1;
}

int Renderer::GetMaterialRuntimeCustomVSID(Entity entity) const
{
    const RuntimeMaterialResource* resource =
        FindRuntimeMaterialResource(entity);
    if (!resource) return -1;
    const auto shader = m_runtimeMaterialShaders.find(resource->shaderKey);
    return shader != m_runtimeMaterialShaders.end()
        ? shader->second.customVSID : -1;
}

void Renderer::SetMaterialTextureRuntimeOverride(
    Entity entity, uint32_t slot, const MaterialRuntimeTextureView* view)
{
    if (slot >= MaterialComponent::TEXTURESLOT_COUNT) return;
    const auto binding = m_entityMaterialRuntimeBindings.find(entity);
    if (binding == m_entityMaterialRuntimeBindings.end()) return;

    binding->second.overrideMask[slot] = view ? 1u : 0u;
    binding->second.engineOverrides[slot] =
        view ? *view : MaterialRuntimeTextureView{};
}

void Renderer::ReleaseRuntimeMaterialResource(uint64_t key,
                                              bool deferRelease)
{
    const auto it = m_runtimeMaterialResources.find(key);
    if (it == m_runtimeMaterialResources.end()) return;

    RuntimeMaterialResource& resource = it->second;
    if (resource.refCount > 1)
    {
        --resource.refCount;
        return;
    }

    auto release = [&](MatTexEntry& texture)
    {
        if (texture.handle == Resource::kInvalidTextureHandle) return;
        if (deferRelease)
            m_pendingMatTexReleaseAfterNextSync.push_back(texture.handle);
        else if (m_texSys)
            m_texSys->Release(texture.handle, m_gfx);
        texture.handle = Resource::kInvalidTextureHandle;
        texture.view = {};
    };
    for (MatTexEntry& texture : resource.engineTextures)
        release(texture);
    for (auto& [_name, texture] : resource.customTextures)
        release(texture);
    m_runtimeMaterialResources.erase(it);
}

void Renderer::DrainPendingBindingReleases()
{
    if (!m_pendingBindingRelease.empty())
    {
        static bool s_reported = false;
        if (!s_reported)
        {
            s_reported = true;
            LOG_INFO("[SPLITAUDIT][DESTROYLANE] released %zu deferred material "
                     "bindings - the lane is exercised",
                     m_pendingBindingRelease.size());
        }
    }
    for (Entity e : m_pendingBindingRelease)
        ReleaseEntityMaterialRuntimeBinding(e);
    m_pendingBindingRelease.clear();
}

void Renderer::ReleaseEntityMaterialRuntimeBinding(Entity e)
{
    if (e < m_matBoundBits.size()) m_matBoundBits[e] = 0;
    const auto it = m_entityMaterialRuntimeBindings.find(e);
    if (it == m_entityMaterialRuntimeBindings.end()) return;
    const uint64_t key = it->second.resourceKey;
    m_entityMaterialRuntimeBindings.erase(it);
    ReleaseRuntimeMaterialResource(key, /*deferRelease=*/false);
}

// ==== By-key material runtime (its six rules: Renderer.h) ====
// The two enforced HERE: game half only, and no invented entity ids - nothing
// below touches m_matBoundBits, m_entityMatSlot or m_entityMaterialRuntimeBindings.
uint64_t Renderer::BuildPreviewMaterialRuntimeKey(const MaterialComponent& mc) const
{
    return BuildMaterialRuntimeKey(mc, BuildMaterialShaderKey(mc));
}

uint64_t Renderer::AcquirePreviewMaterialRuntime(const MaterialComponent& mc)
{
    // Rule 1, checked: the render half holds raw pointers INTO this map while it
    // packs materials (WriteMaterialSlot) and builds the DDGI material proxy, so
    // an insert from any other context can rehash the table under them.
    // SyncMaterialTextures, the main path's writer, runs only from
    // BuildScene_SyncMaterials for the same reason.
    (void)ENGINE_CHECK_ONCE(Sys::ThreadCtx::Current() != Sys::ThreadCtx::Render,
        "[SPLITCHECK][RENDERCTX] AcquirePreviewMaterialRuntime ran in the Render "
        "context. The runtime-material maps may only change on the game half - "
        "the render half holds raw pointers into them (plan sec.3.6 constraint 4).");

    if (!m_texSys || !m_resMgr) return 0;

    const uint64_t shaderKey = BuildMaterialShaderKey(mc);
    const uint64_t key       = BuildMaterialRuntimeKey(mc, shaderKey);

    auto [it, inserted] = m_runtimeMaterialResources.try_emplace(key);
    RuntimeMaterialResource& resource = it->second;
    if (inserted)
    {
        // The same population SyncMaterialTextures does for an entity, so a
        // preview of a material already on screen finds the resource present and
        // only refcounts it: it samples the SAME uploads, not a second copy.
        resource.shaderKey = shaderKey;
        for (int slot = 0; slot < MaterialComponent::TEXTURESLOT_COUNT; ++slot)
        {
            MatTexEntry& texture = resource.engineTextures[slot];
            texture.path   = mc.textures[slot].name;
            texture.handle = texture.path.empty()
                ? Resource::kInvalidTextureHandle
                : m_texSys->Acquire(texture.path, *m_resMgr, m_gfx);
        }
        resource.customTextures.reserve(mc.customTextures.size());
        for (const auto& [name, authoredTexture] : mc.customTextures)
        {
            MatTexEntry texture;
            texture.path   = authoredTexture.name;
            texture.handle = texture.path.empty()
                ? Resource::kInvalidTextureHandle
                : m_texSys->Acquire(texture.path, *m_resMgr, m_gfx);
            resource.customTextures.emplace(name, std::move(texture));
        }
    }
    ++resource.refCount;
    return key;
}

void Renderer::ReleasePreviewMaterialRuntime(uint64_t key)
{
    if (key == 0) return;
    (void)ENGINE_CHECK_ONCE(Sys::ThreadCtx::Current() != Sys::ThreadCtx::Render,
        "[SPLITCHECK][RENDERCTX] ReleasePreviewMaterialRuntime ran in the Render "
        "context. See AcquirePreviewMaterialRuntime.");
    // deferRelease=false matches SyncMaterialTextures' own release: the
    // TextureSystem handle goes back immediately, and TextureSystem's own
    // deferred-destroy is what keeps an in-flight frame safe.
    ReleaseRuntimeMaterialResource(key, /*deferRelease=*/false);
}

Renderer::ResolvedMaterialRuntime
Renderer::ResolvePreviewMaterialRuntime(uint64_t key) const
{
    ResolvedMaterialRuntime rt;
    // `binding` stays null on purpose: a preview has no entity to override for
    // (see the declaration).
    const auto resource = m_runtimeMaterialResources.find(key);
    if (resource != m_runtimeMaterialResources.end())
        rt.resource = &resource->second;
    return rt;
}

bool Renderer::HasPreviewMaterialRuntime(uint64_t key) const
{
    return key != 0 &&
           m_runtimeMaterialResources.find(key) != m_runtimeMaterialResources.end();
}

bool Renderer::IsPreviewMaterialRuntimeReady(uint64_t key) const
{
    const auto it = m_runtimeMaterialResources.find(key);
    if (it == m_runtimeMaterialResources.end()) return false;
    // allTexturesResolved is PromoteRuntimeMaterialTextures' own latch: it flips
    // once every valid handle in the resource has become Ready, and a resource
    // with no textures at all is born true on the first promote. Reading the
    // latch rather than re-polling TextureSystem keeps this callable from the
    // render half (TextureSystem::GetTexture is main-thread only).
    return it->second.allTexturesResolved;
}

MaterialRuntimeTextureView
Renderer::GetPreviewEngineTextureRuntime(uint64_t key, uint32_t slot) const
{
    if (slot >= MaterialComponent::TEXTURESLOT_COUNT) return {};
    const auto it = m_runtimeMaterialResources.find(key);
    if (it == m_runtimeMaterialResources.end()) return {};
    return it->second.engineTextures[slot].view;
}

MaterialRuntimeTextureView
Renderer::GetPreviewCustomTextureRuntime(uint64_t key,
                                         std::string_view bindingName) const
{
    const auto it = m_runtimeMaterialResources.find(key);
    if (it == m_runtimeMaterialResources.end()) return {};
    const auto texture = it->second.customTextures.find(bindingName);
    return texture != it->second.customTextures.end() ? texture->second.view
                                                      : MaterialRuntimeTextureView{};
}

uint64_t Renderer::HashMaterialParams(const MaterialComponent& mc,
                                      const ResolvedMaterialRuntime& rt) const
{
    // The SAME function BuildScene keys GPU material slots with. Exposed rather
    // than re-derived because it is the only hash that folds exactly the
    // GPU-observable field set and is generated from the MATERIAL_GPU_FLAGS
    // X-macro - a re-implementation would silently miss the next flag added.
    return HashMatParams(&mc, rt);
}

uint32_t Renderer::ResolvePreviewCustomPSID(const MaterialComponent& mc)
{
    // ResolveCustomPSID ignores its Entity argument, so NullEntity is not a
    // sentinel here: there is no entity. The game-half rule is enforced one
    // level down, in RegisterMaterialShader.
    return ResolveCustomPSID(NullEntity, mc);
}

uint32_t Renderer::ResolvePreviewCustomVSID(const MaterialComponent& mc)
{
    return ResolveCustomVSID(NullEntity, mc);
}

const ShaderReflect::Reflection*
Renderer::LookupCustomShaderReflection(uint32_t shaderID, const MaterialComponent* mc) const
{
    // Pixel or vertex: both stages register into the same dynamic id space, so
    // one lookup serves both (which lets a caller apply the main packer's
    // `reflPS ? reflPS : reflVS` fallback). Returns the interface the shader is
    // synced and packed from: its declarations; failing those, the union over
    // @p mc's permutations, or permutation 0 without a material.
    if (shaderID == 0 || !m_gbufferPass) return nullptr;
    auto& lib = m_gbufferPass->GetShaderLibrary();
    return mc ? MaterialShaderReflection(lib, shaderID, *mc) : MaterialShaderInterface(lib, shaderID);
}

// Material -> filter / permutation / stencil ref / view-space depth.
// `camForward` is the current-frame camera forward (m_view is only updated
// after BuildRenderScene).
void Renderer::ApplyBlendMode(DrawCandidate& c, const XMFLOAT3& camForward)
{
    if (!c.mc) return;
    // Stencil ref = the deferred lighting route, derived from the SHADING MODEL
    // (Graphics/ShadingStencil.h), as in Unreal; shaderType is not a routing axis.
    c.stencilRef = ShadingStencil::RefFor(c.mc->customShadingModel);
    // Migration tripwire: recomputes the shaderType routing so the running engine
    // names every loaded material whose two axes disagree (the fix:
    // tools/migrate_shadertype.py); silence is the acceptance test. Delete this
    // block once the corpus is migrated and it has stayed silent for a release.
    {
        uint8_t legacyRef = ShadingStencil::kPBR;
        switch (c.mc->shaderType)
        {
        case MaterialComponent::SHADERTYPE_NPR_RAMP:
        case MaterialComponent::SHADERTYPE_NPR_COLOR: legacyRef = ShadingStencil::kNPR;   break;
        case MaterialComponent::SHADERTYPE_UNLIT:     legacyRef = ShadingStencil::kUnlit; break;
        default: break;
        }
        (void)ENGINE_CHECK_ONCE(legacyRef == c.stencilRef,
            "ShadingModel migration: material still carries shaderType=%d whose "
            "lighting route (stencil %u) disagrees with its shading model '%s' "
            "(stencil %u). Run tools/migrate_shadertype.py.",
            static_cast<int>(c.mc->shaderType), static_cast<unsigned>(legacyRef),
            ShadingModelName(c.mc->customShadingModel),
            static_cast<unsigned>(c.stencilRef));
    }
    // The permutation bits the material decides - double-sided, the blend
    // route, alpha test, the water redirect - live in ApplyMaterialPermutation
    // (Renderer/MaterialPermutation.h), which the material's reflection also
    // uses. This function keeps the ROUTING: filter, shadow cull mode,
    // cast-shadow, depth.
    ApplyMaterialPermutation(*c.mc, c.perm);
    c.shadowCullMode = static_cast<uint8_t>(c.mc->shadowCullMode);
    // Cast-shadow is the AND of two layers (docs/design/ecs_visibility_design.md):
    //   Material::CAST_SHADOW           - art-time per-asset opt-out (glass etc.)
    //   VisibilityComponent.CastShadow  - runtime per-entity toggle (stealth etc.)
    // Visibility writes c.castShadow BEFORE ApplyBlendMode runs (primitive and
    // MeshLibRef paths), so the AND keeps both. Billboards / beams never call
    // ApplyBlendMode without a material; their default castShadow = 1 lets the
    // material flag alone gate them.
    c.castShadow = (c.castShadow && c.mc->IsCastingShadow()) ? 1 : 0;

    // Blend routing: the translucent modes draw forward (DrawFilter::Transparent),
    // Opaque / Masked / Dithered deferred (DrawFilter::Opaque). Water is
    // redirected after this switch.
    switch (c.mc->userBlendMode)
    {
    case BlendMode::Alpha:
    case BlendMode::Additive:
    case BlendMode::Premultiplied:
    case BlendMode::Multiply:
    case BlendMode::Transmissive:
        c.filter = DrawFilter::Transparent;
        // A forward-translucent surface never writes a shadow map (CSM, spot,
        // point, per-object), whatever its Alpha Ref: the alpha-test shadow bucket
        // clips on texture alpha and never sees the surface's opacity, so glass whose
        // opacity lives in baseColor.w or a custom shader would cast a SOLID shadow
        // (UE imports carry an OpacityMaskClipValue that UE ignores for translucent
        // materials). The blend mode decides, as for the RT alpha records
        // (DDGISceneAS, p.blendShadow) and the path tracer (forward-translucent
        // instances are outside its 0x08 mask). Cut-out cards that should cast a
        // shadow are Masked or Dithered.
        // -shadow_translucent_casters: translucent surfaces keep their cast-shadow
        // flag (for A/B).
        {
            static const bool s_translucentCasters =
                Sys::CommandLine::Get().Has("-shadow_translucent_casters");
            if (!s_translucentCasters) c.castShadow = 0;
        }
        break;
    case BlendMode::Masked:
        // Unreal-style Masked: opaque deferred GBuffer route with the ALPHA_TEST
        // clip. Shadow passes pick the ALPHA_TEST bucket from the same perm bit;
        // RT follows via IsAlphaTestEnabled() in DDGISceneAS.
    case BlendMode::Dithered:
        // Stochastic transparency: the SAME deferred route and ALPHA_TEST
        // permutation as Masked; the clip block selects the temporal-dither
        // threshold from MAT_FLAG_DITHERED_COVERAGE (WriteMaterialSlot) instead of
        // alphaRef. Shadow passes land in the AlphaTest bucket and dither
        // statically; DDGISceneAS publishes an opacity BLEND record like Alpha.
    default: // BlendMode::Opaque
        c.filter = DrawFilter::Opaque;
        break;
    }

    // Water shading model (Unreal "Single Layer Water"): runs AFTER the blend
    // switch on purpose. Water is a translucent Surface material and keeps the
    // blend the author picked (WaterSurfacePass::BuildPSODesc turns it into the
    // HDR target's blend state); this only REDIRECTS the draw, because water needs
    // depth write + a GBuffer stamp the ordinary translucency pass cannot give it.
    // An Opaque / Masked water material lands here too; the pass falls back to
    // straight alpha (a water surface blending with nothing is an opaque lid).
    // castShadow off: a forward alpha-blended sheet has no shadow-pass PSO, and in
    // the CSM lists it would drop a solid slab of shadow under the lake.
    // Do NOT early-return from here: the depth computed below is what
    // back-to-front sorts overlapping water bodies.
    if (c.mc->IsWaterShadingModel())
    {
        // The permutation half (WATER_SURFACE on, ALPHA_TEST off) is in
        // ApplyMaterialPermutation above.
        c.filter     = DrawFilter::Water;
        c.castShadow = 0;
    }

    // View-space Z depth: worldCenter projected onto the camera forward (NOT
    // Euclidean: that pops). The builder sets worldCenter: the WorldAabb centroid
    // for MeshLibRef, the pivot for primitives.
    {
        const float dx = c.worldCenter.x - FrameViewWrite().camera.position.x;
        const float dy = c.worldCenter.y - FrameViewWrite().camera.position.y;
        const float dz = c.worldCenter.z - FrameViewWrite().camera.position.z;
        c.depth = dx * camForward.x
                + dy * camForward.y
                + dz * camForward.z;
    }
}

// One DynamicShaderRegistry registration that every pass library resolves
// (contract at the declaration); the id is a hash of the (path, entry, stage)
// key. Callers: ResolveCustomPSID / ResolveCustomVSID and the editor's
// material-instance repair and authoring panels.
uint32_t Renderer::RegisterMaterialShader(const char* path, RHI::ShaderStage stage)
{
    (void)ENGINE_CHECK_ONCE(Sys::ThreadCtx::Current() != Sys::ThreadCtx::Render,
                      "[SPLITCHECK][SHADERLIB] RegisterMaterialShader ran in the Render "
                      "context. Material shader registration is game-half only: its callers "
                      "insert into m_runtimeMaterialShaders, which the render half reads "
                      "(plan T22). path='%s' stage=%d",
                      path ? path : "(null)", static_cast<int>(stage));
    return DynamicShaderRegistry::Get().Register(path, stage);
}

uint32_t Renderer::ResolveCustomPSID(Entity entity, const MaterialComponent& m)
{
    if (!m.useCustomShader || m.customShaderPath.empty()) return 0;
    (void)entity;
    RuntimeMaterialShaderState& state =
        m_runtimeMaterialShaders[BuildMaterialShaderKey(m)];
    if (state.customPSID < 0 && m_gbufferPass)
    {
        PROF_SCOPE("Mat.RegisterCustomPS");   // once per shader key
        const uint32_t id = RegisterMaterialShader(
            m.customShaderPath.c_str(), RHI::ShaderStage::PS);
        state.customPSID =
            (id == ShaderLibrary::kInvalidDynShaderID) ? -1 : static_cast<int>(id);
    }

    // Shader ids are shared by layout, but reflection defaults belong to each
    // authored entity snapshot. The Sync runs once per (paths, reload epoch)
    // stage stamp; a still-compiling shader (null reflection) retries next frame
    // because the stage bit is stamped only after a completed Sync.
    if (state.customPSID > 0 && m_gbufferPass)
    {
        auto& lib = m_gbufferPass->GetShaderLibrary();
        const uint32_t epoch = lib.GetReloadEpoch();
        if (!MaterialReflectionSync::StageSynced(m, epoch, /*vsStage*/ false))
            if (auto* refl = [&] {
                    // The first time this is the GBuffer library's shader
                    // miss (the .ishdr load, reflection, //@ annotations):
                    // most of a cold first gather.
                    PROF_SCOPE("Mat.ReflectCustomPS");
                    return MaterialShaderReflection(lib, static_cast<uint32_t>(state.customPSID), m);
                }())
            {
                MaterialReflectionSync::Sync(const_cast<MaterialComponent&>(m), *refl);
                MaterialReflectionSync::MarkStageSynced(
                    const_cast<MaterialComponent&>(m), epoch, /*vsStage*/ false);
                // -matpack_dump: the bag this Sync left.
                if (s_matPackDump) MatPackLine("sync PS " + MatPackShaders(m) + MatPackBag(m));
            }
    }

    return (state.customPSID > 0) ? static_cast<uint32_t>(state.customPSID) : 0;
}

// The vertex source (ResolveCustomVSID's and the preload's):
//   1. a two-file material -> customVertexShaderPath (an explicit VS file);
//   2. a unified single-file material -> customShaderPath, ONLY when it declares
//      World-Position-Offset: #define MATERIAL_HAS_VERTEX_OFFSET in its own
//      files, read from its declarations (a comment naming it does not count).
// A pixel-only material compiles no per-material VS: it shares the engine's
// generic GBuffer.vs and depth / shadow shaders.
std::string Renderer::CustomVertexShaderPath(const MaterialComponent& m)
{
    if (!m.customVertexShaderPath.empty())
        return m.customVertexShaderPath;
    if (!m.customShaderPath.empty() && m_gbufferPass &&
        MaterialShaderDecl::Get(m.customShaderPath,
                                m_gbufferPass->GetShaderLibrary().GetShaderDir())->hasVertexOffset)
        return m.customShaderPath;
    return {};
}

// Mirrors ResolveCustomPSID (contract at the declaration), and syncs the VS's
// reflected b8 params / space3 textures into the SAME custom bag the PS uses,
// so a VS-only material still surfaces editable tunables.
uint32_t Renderer::ResolveCustomVSID(Entity entity, const MaterialComponent& m)
{
    if (!m.useCustomShader) return 0;
    (void)entity;
    RuntimeMaterialShaderState& state =
        m_runtimeMaterialShaders[BuildMaterialShaderKey(m)];
    if (state.customVSID < 0 && m_gbufferPass)
    {
        PROF_SCOPE("Mat.RegisterCustomVS");   // once per shader key
        const std::string vsPath = CustomVertexShaderPath(m);

        if (vsPath.empty())
        {
            // Resolved as pixel-only: cache 0 so we don't re-scan the source every
            // frame (customVSID stays >= 0, so the < 0 gate above won't fire).
            state.customVSID = 0;
            // The PS sync already ran (ResolveCustomPSID, just before) and there is no
            // VS stage, so the one-shot customParamsLegacyFormat migration is complete:
            // clear the flag so the next save writes the diff-against-default format.
            // Only for a RESOLVED PS (id > 0): an unresolved one never synced, so the
            // flag stays for a retry next frame.
            if (state.customPSID > 0)
                const_cast<MaterialComponent&>(m).customParamsLegacyFormat = false;
            return 0;
        }

        const uint32_t id = RegisterMaterialShader(vsPath.c_str(), RHI::ShaderStage::VS);
        state.customVSID =
            (id == ShaderLibrary::kInvalidDynShaderID) ? -1 : static_cast<int>(id);
    }
    // Same epoch/path stamp as the PS side (ResolveCustomPSID).
    if (state.customVSID > 0 && m_gbufferPass)
    {
        auto& lib = m_gbufferPass->GetShaderLibrary();
        const uint32_t epoch = lib.GetReloadEpoch();
        if (!MaterialReflectionSync::StageSynced(m, epoch, /*vsStage*/ true))
            if (auto* refl = [&] {
                    PROF_SCOPE("Mat.ReflectCustomVS");
                    return MaterialShaderReflection(lib, static_cast<uint32_t>(state.customVSID), m);
                }())
            {
                MaterialReflectionSync::Sync(const_cast<MaterialComponent&>(m), *refl);
                MaterialReflectionSync::MarkStageSynced(
                    const_cast<MaterialComponent&>(m), epoch, /*vsStage*/ true);
                // -matpack_dump: the bag this Sync left.
                if (s_matPackDump) MatPackLine("sync VS " + MatPackShaders(m) + MatPackBag(m));
            }
    }
    // Both stages have synced (PS in ResolveCustomPSID just before, VS above):
    // finish the one-shot customParamsLegacyFormat migration started at .imat
    // load. Done here, NOT inside Sync, because a param can live in only one
    // stage's b8 cbuffer. Only for a RESOLVED shader, so a pending / failed
    // compile (no Sync yet) retries.
    if (state.customPSID > 0 || state.customVSID > 0)
        const_cast<MaterialComponent&>(m).customParamsLegacyFormat = false;
    return (state.customVSID > 0) ? static_cast<uint32_t>(state.customVSID) : 0;
}

// The same two ids keyed by the MATERIAL (contract at the declaration).
// Read-only: find(), never operator[] (an insert would grow a map the render
// half walks and make an inspector click look like a resolved material), and
// never RegisterMaterialShader (a miss degrades the UI; it never invents an
// id). m_runtimeMaterialShaders is never cleared, not even by OnWorldClear,
// so -1 means "not this process", not "not this world".
int Renderer::LookupMaterialCustomPSID(const MaterialComponent& m) const
{
    // Same early-out as ResolveCustomPSID, so the lookup can never answer for a
    // material the resolver would have refused to key.
    if (!m.useCustomShader || m.customShaderPath.empty()) return -1;
    // Game / extract half only, like every reader of this map: the game
    // half inserts with operator[] (a rehash), so a render-half find() is UB.
    // A preview caller must extract on the game half.
    (void)ENGINE_CHECK_ONCE(Sys::ThreadCtx::Current() != Sys::ThreadCtx::Render,
                      "[SPLITCHECK][SHADERLIB] LookupMaterialCustomPSID ran in the Render context");
    const auto shader = m_runtimeMaterialShaders.find(BuildMaterialShaderKey(m));
    return shader != m_runtimeMaterialShaders.end()
        ? shader->second.customPSID : -1;
}

void Renderer::QueueSpawnMaterialResolve(const std::vector<Entity>& entities)
{
    m_spawnMaterialResolve.insert(m_spawnMaterialResolve.end(), entities.begin(), entities.end());
}

// The queue's custom shaders, exactly as the gather's serial evalJob resolves
// them (ResolveCustomPSID, then ResolveCustomVSID) - only earlier in the frame,
// before BuildScene_SyncMaterials. The gather then finds both settled and calls
// nothing that writes. An entity destroyed meanwhile is skipped; one whose id was
// reused is resolved as what it is now (harmless: the gather would do the same).
void Renderer::ResolveSpawnMaterials(World& world)
{
    if (m_spawnMaterialResolve.empty()) return;
    // -gather_noaddpath (prefab_spawn_gather's reverse arm): no early resolve -
    // the gather resolves these the frame it meets them.
    static const bool s_off = Sys::CommandLine::Get().Has("-gather_noaddpath");
    if (s_off) { m_spawnMaterialResolve.clear(); return; }
    PROF_SCOPE("BRS.SpawnMatResolve");
    (void)ENGINE_CHECK_ONCE(Sys::ThreadCtx::Current() != Sys::ThreadCtx::Render,
                      "[SPLITCHECK][SHADERLIB] ResolveSpawnMaterials ran in the Render context");
    const auto* pMaterial = world.GetPool<MaterialComponent>();
    for (Entity e : m_spawnMaterialResolve)
    {
        if (!world.IsAlive(e)) continue;
        const MaterialComponent* mc = pMaterial ? pMaterial->Get(e) : nullptr;
        if (!mc || !mc->useCustomShader) continue;
        ResolveCustomPSID(e, *mc);
        ResolveCustomVSID(e, *mc);
    }
    m_spawnMaterialResolve.clear();
}

void Renderer::CountColdMaterialInputs(const std::vector<const MaterialComponent*>& materials,
                                       uint32_t& coldShaders, uint32_t& coldTextures) const
{
    coldShaders = 0;
    coldTextures = 0;
    (void)ENGINE_CHECK_ONCE(Sys::ThreadCtx::Current() != Sys::ThreadCtx::Render,
                      "[SPLITCHECK][SHADERLIB] CountColdMaterialInputs ran in the Render context");
    std::unordered_set<uint64_t>    shaderKeys;
    std::unordered_set<std::string> texturePaths;
    for (const MaterialComponent* m : materials)
    {
        if (!m) continue;
        // The resolvers key the map by exactly these three fields
        // (BuildMaterialShaderKey); an absent key has never been resolved.
        if (m->useCustomShader && (!m->customShaderPath.empty() || !m->customVertexShaderPath.empty()))
        {
            const uint64_t key = BuildMaterialShaderKey(*m);
            if (m_runtimeMaterialShaders.find(key) == m_runtimeMaterialShaders.end())
                shaderKeys.insert(key);
        }
        // The paths SyncMaterialTextures acquires: every engine slot, every
        // custom binding.
        for (int slot = 0; slot < MaterialComponent::TEXTURESLOT_COUNT; ++slot)
            if (!m->textures[slot].name.empty()) texturePaths.insert(m->textures[slot].name);
        for (const auto& [name, texture] : m->customTextures)
            if (!texture.name.empty()) texturePaths.insert(texture.name);
    }
    coldShaders = static_cast<uint32_t>(shaderKeys.size());
    if (!m_texSys)
    {
        coldTextures = static_cast<uint32_t>(texturePaths.size());
        return;
    }
    for (const std::string& path : texturePaths)
        if (!m_texSys->IsReady(m_texSys->Find(path))) ++coldTextures;
}

int Renderer::LookupMaterialCustomVSID(const MaterialComponent& m) const
{
    // ResolveCustomVSID has no customShaderPath.empty() early-out (a two-file
    // material may carry only customVertexShaderPath), so neither does this.
    if (!m.useCustomShader) return -1;
    (void)ENGINE_CHECK_ONCE(Sys::ThreadCtx::Current() != Sys::ThreadCtx::Render,
                      "[SPLITCHECK][SHADERLIB] LookupMaterialCustomVSID ran in the Render context");
    const auto shader = m_runtimeMaterialShaders.find(BuildMaterialShaderKey(m));
    return shader != m_runtimeMaterialShaders.end()
        ? shader->second.customVSID : -1;
}
