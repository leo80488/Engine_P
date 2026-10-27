#include "Renderer/DDGISceneAS.h"
#include "Diagnostics/Profiler.h"        // PROF_SCOPE
#include <optional>
#include "Graphics/IGraphicsDevice.h"
#include "System/HostPolicy.h"   // IsEditorHost - EditorOnlyTag culling
#include "Renderer/Renderer.h"        // ResolveMaterialRuntime - batched material lookup
#include "Renderer/GatherGateStats.h"  // the retained-gather gate's Reason bits
#include "Graphics/MaterialSurfaceCache.h" // baked RT proxy for custom-shader materials
#include "Graphics/MeshManager.h"
#include "Resource/MeshLibrary.h"
#include "Resource/ProceduralMesh.h"  // PrimitiveMeshType - MeshHandle.gpuMeshID range check
#include "Renderer/SkinnedMeshSubsystem.h" // SkinnedVertexRing + skin-job list (skinned BLAS source)
#include "ECS/ECS.h"
#include "ECS/HierarchyComponents.h"
#include "ECS/Components.h"
#include "Graphics/FoliageWind.h"   // CPU mirror of the WPO sway -> TLAS shear
#include "Graphics/ShadingModelTable.h" // which models' proxies carry the material slot
#include "ECS/AnimationComponents.h"  // MeshSkinnedComponent + SkinningOutputComponent
#include "ECS/DDGIComponents.h"
#include "ECS/DissolveComponent.h"    // RT mask drop for dissolving entities
#include <type_traits>                // is_trivially_copyable - retained Pending blob
#include "System/Log.h"
#include "System/TaskGraph.h"
#include "System/CoarseTaskPlan.h"
#include "System/JobDefaults.h"
#include "System/CommandLine.h"
#include "System/Check.h"             // ENGINE_CHECK_ONCE, never assert
#include "System/WordHash.h"

#include <vector>
#include <cstring>
#include <unordered_set>

namespace
{
// Round @p value up to a multiple of @p align (@p align 0 returns @p value).
constexpr uint64_t AlignTo(uint64_t value, uint64_t align)
{
    return align ? ((value + align - 1) / align) * align : value;
}

// Mirror of HLSL `struct RTInstanceData` in shaders/rt_instance_data.hlsli, 128 bytes. The
// layouts must match: the buffers bind as root SRVs, so the stride comes from the HLSL struct and
// a mismatch silently shears every instance after [0].
struct DDGIInstanceData
{
    float    baseColor[4];     // 16B: rgb albedo, a = opacity (the candidate alpha test multiplies by it)
    float    emissive[4];      // 16B: rgb pre-multiplied by strength;
                               // a = customShadingModel as float; 1 = Unlit:
                               // an RT hit shades flat albedo + emissive, as
                               // Lighting.ps does. DDGIRayTrace ignores it.
    uint32_t vbBindlessIdx;
    uint32_t ibBindlessIdx;
    uint32_t vertexStart;      // first vertex (count of vertices, not bytes)
    uint32_t indexStart;       // first index  (count of indices,  not bytes)
    int32_t  emissiveTexIdx;   // bindless texture pool index, -1 = none
    // Bytes per vertex in the bindless VB - the same per-library stride the BLAS build uses
    // (MeshLibrary strides vary: 32, 48 with tangent, L.stride for PMX/scene imports). pos/nrm/uv0
    // sit in the first 32 bytes of every layout; only the stride differs. A wrong stride reads garbage
    // normals/UVs, which flips DDGI's front/backface test and leaves probes inactive and black.
    uint32_t vertexStride;
    // De-interleaved stream mode (procedural primitives, skinned meshes): when normalBufIdx !=
    // kInvalidBufferIndex, vbBindlessIdx is the POSITION stream (float3, stride 12) and normals/uv0
    // come from the streams below. Invalid -> interleaved walk (pos@0 nrm@12 uv@24, x vertexStride).
    uint32_t normalBufIdx;     // float3 stream, stride 12
    uint32_t uvBufIdx;         // float2 stream, stride 8 (may stay invalid)
    // ---- RTR hit-shading material proxy (bytes 64-95) ----
    // Lets HybridReflIntersectHW shade analytic (off-screen) hits with the real material: albedo
    // texture, the GBuffer's roughness/metalness remap (lerp(min, max, surfaceMap.g/b)) and
    // reflectance for F0 (0.16 x refl^2). DDGI's own shading uses baseColor x albedoTexIdx and
    // emissiveTexIdx (plus, for a surface-cached instance, the emissive mask in the surfaceTexIdx
    // page's alpha); roughness, metalness and reflectance serve the other RT consumers.
    int32_t  albedoTexIdx;     // BASECOLORMAP bindless pool index, -1 = none
    int32_t  surfaceTexIdx;    // SURFACEMAP (R=AO,G=rough,B=metal), -1 = none
    float    roughnessMin;
    float    roughnessMax;
    float    metalnessMin;
    float    metalnessMax;
    float    reflectance;      // F0 scalar = 0.16 * reflectance^2
    uint32_t indexBytes;       // 2 = R16_UINT (primitives), else 4 = R32_UINT
    // ---- De-interleaved stream base elements (bytes 96-111) ----
    // Skinned meshes read pos/nrm from the shared SkinnedVertexRing arena, so their streams start at a
    // per-entity ELEMENT base in the bindless buffer; vertexStart then doubles as the position-stream
    // base. Procedural primitives keep all bases 0 (whole-buffer streams). Interleaved (MeshLibrary)
    // fetches ignore all three.
    uint32_t nrmStart;         // base element into the normal stream (float3)
    uint32_t uvStart;          // base element into the uv stream (float2)
    uint32_t rtFlags;          // kRTInstFlag* bits; bits 16-31 = material slot + 1
    // NORMALMAP bindless index, -1 = none.
    int32_t  normalTexIdx;
    // ---- Material UV transform (bytes 112-127) ----
    // uv' = uv * xy + zw, mirror of MaterialComponent::texMulAdd (the raster's
    // MaterialGPUData::uvTilingOffset), so a tiled material reflects the way it renders. Every RT
    // consumer applies it via RTInstanceUV() (aliased HR_MaterialUV / DDGI_MaterialUV) at each
    // material-texture fetch, like MaterialUV() in material.hlsli. A surface-cached instance carries
    // its page rect here instead (kRTInstFlagSurfaceCache). Default (1,1,0,0): a zeroed value
    // collapses every hit UV to one texel.
    float    uvTilingOffset[4];
};
static_assert(sizeof(DDGIInstanceData) == 128, "DDGIInstanceData must be 128 bytes (sync BOTH HLSL mirrors)");

// Patch frames on which a moved surface-cache epoch re-bound the static rows from the PRE-binding
// blob instead of forcing a full World gather. Counted by Gather (game half), printed by
// -tlasdigest (render half), hence atomic.
static std::atomic<uint64_t> g_ddgiEpochRebinds{0};
static_assert(offsetof(DDGIInstanceData, albedoTexIdx)   == 64,  "DDGIInstanceData layout drift");
static_assert(offsetof(DDGIInstanceData, nrmStart)       == 96,  "DDGIInstanceData layout drift");
static_assert(offsetof(DDGIInstanceData, normalTexIdx)   == 108, "DDGIInstanceData layout drift");
    static_assert(offsetof(DDGIInstanceData, uvTilingOffset) == 112, "DDGIInstanceData layout drift");

// Mirrors of the RT_INST_FLAG_* defines in shaders/rt_instance_data.hlsli. They use spare bits of
// `rtFlags`: the struct is exactly full at 128 B.
constexpr uint32_t kRTInstFlagSkinned        = 1u << 0; // deforming BLAS, rebuilt/refit per frame
// Custom-shader material whose albedo resolved to neither a known space3 alias nor an authored
// engine slot - every proxy field is an ENGINE GUESS. The trace prefers an environment estimate
// over shading a confidently wrong surface.
constexpr uint32_t kRTInstFlagProxyUntrusted = 1u << 1;
// albedo/surface come from a material surface-cache atlas page; uvTilingOffset
// carries the page rect instead of the material's texMulAdd, and opacity lives
// in the albedo page's alpha channel.
constexpr uint32_t kRTInstFlagSurfaceCache   = 1u << 2;
// Backface hits are legitimate. Carried here, not only in RTAlphaRecord: the trace gates that
// record behind `alphaTestEnabled`, so a frame without the alpha table or a bindless table would
// turn every two-sided material single-sided.
constexpr uint32_t kRTInstFlagDoubleSided    = 1u << 3;
// The material's shading model is Toon (the stencil-2 deferred toon path). The reflection trace
// swaps its Cook-Torrance sun term for a ramp approximation so the reflected character keeps the
// toon model's non-black shadow floor.
constexpr uint32_t kRTInstFlagNPR            = 1u << 4;
// Surface-cached instance keeping its FULL-RES aliased albedo (raw mesh UV);
// the page carries surface/emissive-mask only and baseColor stays authored.
constexpr uint32_t kRTInstFlagAlbedoRaw      = 1u << 5;
// Material draws an inverted-hull outline on screen; reflections ink the
// silhouette analytically (the hull never enters the TLAS).
constexpr uint32_t kRTInstFlagOutline        = 1u << 6;
constexpr uint32_t kRTInstFlagInterleavedTangent = 1u << 7;
// ReSTIR GI emitter: RTGI's cosine rays can hit the instance (mask bit 0) and it emits at a hit
// (emissive, or Unlit, whose hit radiance is albedo + emissive). RTGI samples exactly these
// instances and splits the MIS weight by the integrand's emitted part, so this flag and the GI
// emitter table must describe the same set. Left out: blend instances (GI rays pass through them
// or commit them without the emitter sampler's alpha test, so the cosine ray alone carries their
// light) and skinned instances (a section outside this frame's rebuild budget keeps last frame's
// BLAS pose while the sampler reads this frame's vertices).
constexpr uint32_t kRTInstFlagGIEmitter      = 1u << 8;
// BlendMode::Transmissive: a dielectric the path tracer crosses; no ray culls its back faces.
constexpr uint32_t kRTInstFlagTransmissive   = 1u << 9;
// The light a Transmissive material lets through at normal incidence, per channel: the share that
// refracts (transmission x (1 - metalness)), the interface (a thin wall's slab, a solid's two
// faces), baseColor once and a solid's absorption over its thickness. The RT sun shadow keeps this
// much of the light under a covered pixel.
static std::array<float, 3> TransmissiveShadowTransmittance(const MaterialComponent& mc)
{
    auto toLinear = [](float c) { return std::pow(std::max(c, 0.f), 2.2f); };
    const float share = std::clamp(mc.transmission, 0.f, 1.f) * (1.f - std::clamp(mc.metalnessMax, 0.f, 1.f));
    const float ior   = std::max(mc.ior, 1.f);
    const float r     = (ior - 1.f) / (ior + 1.f);
    const float f0    = r * r;
    const bool  thin  = mc.thickness <= 0.f;
    const float interfaces = thin ? (1.f - f0) / (1.f + f0) : (1.f - f0) * (1.f - f0);
    float rgb[3] = { toLinear(mc.baseColor.x), toLinear(mc.baseColor.y), toLinear(mc.baseColor.z) };
    if (!thin && mc.attenuationDistance > 0.f)
    {
        // Beer: exp(-sigma d) with sigma = -ln(c) / D is c^(d / D).
        const float attenuation[3] = { mc.attenuationColor.x, mc.attenuationColor.y, mc.attenuationColor.z };
        for (int i = 0; i < 3; ++i)
            rgb[i] *= std::pow(std::clamp(toLinear(attenuation[i]), 1e-4f, 1.f), mc.thickness / mc.attenuationDistance);
    }
    return { share * interfaces * rgb[0], share * interfaces * rgb[1], share * interfaces * rgb[2] };
}

// Luminance of an instance's emitted radiance before textures: the GI
// emitter table's power estimate. `emissiveW == 1` is the shader's Unlit test.
static float GIEmitterRadiance(const float* emissive, const float* baseColor)
{
    float r = emissive[0], g = emissive[1], b = emissive[2];
    if (emissive[3] == 1.0f) { r += baseColor[0]; g += baseColor[1]; b += baseColor[2]; }
    const float y = 0.2126f * (std::max)(r, 0.0f) + 0.7152f * (std::max)(g, 0.0f)
                  + 0.0722f * (std::max)(b, 0.0f);
    return std::isfinite(y) ? y : 0.0f;
}
static bool IsGIEmitter(uint8_t instanceMask, bool blend, bool skinned, const DirectX::XMFLOAT4& emissive,
                        const DirectX::XMFLOAT4& baseColor)
{
    const float e[4] = { emissive.x, emissive.y, emissive.z, emissive.w };
    const float c[4] = { baseColor.x, baseColor.y, baseColor.z, baseColor.w };
    return (instanceMask & 1u) != 0 && !blend && !skinned && GIEmitterRadiance(e, c) > 0.0f;
}
// One GI emitter table row (112 bytes; mirror: GIEmitter in RTGI.cs.hlsl).
struct GIEmitterGPU
{
    float    objectToWorld[3][4];
    float    worldToObject[3][4];
    uint32_t instanceID, triangleCount;
    float    radiance;
    uint32_t padding;
};
static_assert(sizeof(GIEmitterGPU) == 112);

// Mirror of `struct RTAlphaRecord` in shaders/rt_alpha_record.hlsli (40 B; the offset asserts
// below catch a field reorder the size assert would miss). A per-TLAS-instance alpha-cutout
// descriptor indexed by InstanceID() like DDGIInstanceData, kept separate because the alpha test
// runs per traversal CANDIDATE and wants a compact stride.
struct RTAlphaRecord
{
    uint32_t flags;          // kRTAlphaFlag* bits
    float    alphaRef;
    int32_t  albedoTexIdx;   // -1 = none
    uint32_t vbBindlessIdx;
    uint32_t ibBindlessIdx;
    uint32_t uvBufIdx;
    uint32_t indexStart;
    uint32_t uvStart;
    uint32_t vertexStride;
    uint32_t indexBytes;
};
static_assert(sizeof(RTAlphaRecord) == 40, "sync DX12/shaders/rt_alpha_record.hlsli");
static_assert(offsetof(RTAlphaRecord, flags)         ==  0, "RTAlphaRecord layout drift");
static_assert(offsetof(RTAlphaRecord, alphaRef)      ==  4, "RTAlphaRecord layout drift");
static_assert(offsetof(RTAlphaRecord, albedoTexIdx)  ==  8, "RTAlphaRecord layout drift");
static_assert(offsetof(RTAlphaRecord, vbBindlessIdx) == 12, "RTAlphaRecord layout drift");
static_assert(offsetof(RTAlphaRecord, ibBindlessIdx) == 16, "RTAlphaRecord layout drift");
static_assert(offsetof(RTAlphaRecord, uvBufIdx)      == 20, "RTAlphaRecord layout drift");
static_assert(offsetof(RTAlphaRecord, indexStart)    == 24, "RTAlphaRecord layout drift");
static_assert(offsetof(RTAlphaRecord, uvStart)       == 28, "RTAlphaRecord layout drift");
static_assert(offsetof(RTAlphaRecord, vertexStride)  == 32, "RTAlphaRecord layout drift");
static_assert(offsetof(RTAlphaRecord, indexBytes)    == 36, "RTAlphaRecord layout drift");

// Mirrors of the RT_ALPHA_FLAG_* defines in rt_alpha_record.hlsli.
constexpr uint32_t kRTAlphaFlagAlphaClip    = 1u << 0;
constexpr uint32_t kRTAlphaFlagDoubleSided  = 1u << 1;
constexpr uint32_t kRTAlphaFlagDeinterleaved = 1u << 2;
// albedoTexIdx carries the OPACITYMAP (Masked mode): sample .r, not .a.
constexpr uint32_t kRTAlphaFlagOpacityR     = 1u << 3;
// Alpha-BLEND (translucent) occluder; `alphaRef` carries OPACITY, not a clip
// threshold. Mutually exclusive with kRTAlphaFlagAlphaClip.
constexpr uint32_t kRTAlphaFlagBlend        = 1u << 4;
// A Transmissive blend record: bits 8-31 carry the colour one crossing keeps (RTAlphaTint).
constexpr uint32_t kRTAlphaFlagTint         = 1u << 5;
}

using namespace DirectX;

namespace
{
// Default interleaved vertex: float3 pos + float3 nrm + float2 uv (32 B). The stride fallback for a
// row that carries none.
constexpr uint32_t kEngineVertexStride = 32;

// MeshLibrary uses uint32 indices.
constexpr RHI::Format kEngineIndexFormat = RHI::Format::R32_UINT;
constexpr uint32_t    kEngineIndexBytes  = 4;

// Build flags per AS kind; a prebuild size query must pass its build's flags.
// Static: built once and compacted, then traced for many frames.
constexpr RHI::ASBuildFlags kStaticBLASFlags  = RHI::ASBuildFlags::PREFER_FAST_TRACE |
                                                RHI::ASBuildFlags::ALLOW_COMPACTION;
// Skinned: rebuilt or refit every frame, traced for one - build cost dominates.
constexpr RHI::ASBuildFlags kSkinnedBLASFlags = RHI::ASBuildFlags::PREFER_FAST_BUILD |
                                                RHI::ASBuildFlags::ALLOW_UPDATE;
// ALLOW_UPDATE permits transform refits; FAST_TRACE prioritises traversal.
constexpr RHI::ASBuildFlags kTLASFlags        = RHI::ASBuildFlags::ALLOW_UPDATE |
                                                RHI::ASBuildFlags::PREFER_FAST_TRACE;

// Convert the engine's row-major XMFLOAT4X4 (row vector: pos x M, translation in row 3) into the
// 3x4 TLAS instance transform (column vector: transform x (pos, 1), translation in column 3) - a
// transpose of the first three columns.
void ToInstanceTransform(const XMFLOAT4X4& m, float t[3][4])
{
    t[0][0] = m._11; t[0][1] = m._21; t[0][2] = m._31; t[0][3] = m._41;
    t[1][0] = m._12; t[1][1] = m._22; t[1][2] = m._32; t[1][3] = m._42;
    t[2][0] = m._13; t[2][1] = m._23; t[2][2] = m._33; t[2][3] = m._43;
}

// GPU virtual address of @p buf.
uint64_t BufferGPUVA(IGraphicsDevice& gfx, const RHI::GPUBuffer& buf)
{
    return gfx.GetBufferGPUAddress(buf);
}

// Prebuild sizes of one geometry's BLAS built with @p flags; false if the device cannot report them.
bool QueryBLASSize(IGraphicsDevice& gfx, const RHI::ASTriangles& g, RHI::ASBuildFlags flags,
                   uint64_t& resultSize, uint64_t& scratchSize, uint64_t* updateScratchSize = nullptr)
{
    RHI::ASPrebuildInfo info;
    const bool ok = gfx.GetBLASPrebuildInfo(&g, 1, flags, info);
    resultSize  = info.resultSize;
    scratchSize = info.scratchSize;
    if (updateScratchSize) *updateScratchSize = info.updateScratchSize;
    return ok;
}

} // namespace

// ==== Scratch / TLAS resize ====

void DDGISceneAS::EnsureScratch(IGraphicsDevice& gfx, uint64_t needed)
{
    if (needed == 0) return;
    if (m_scratch.IsValid() && m_scratch.sizeBytes >= needed) return;
    // Grow with 25% headroom to avoid reallocating on small fluctuations.
    const uint64_t grown = needed + (needed >> 2);
    RHI::AccelerationStructure fresh;
    if (gfx.CreateAccelerationStructure(grown, RHI::ASMemory::SCRATCH, fresh))
    {
        // The old scratch may still be in use by an AS-build CL from an earlier frame. Release it against
        // the last signaled COMPUTE fence: scratch is compute-only, so this is tighter than the
        // kFrameCount slot defer.
        if (m_scratch.IsValid())
            gfx.DestroyAccelerationStructure(m_scratch, RHI::QueueType::COMPUTE);
        m_scratch = fresh;
    }
}

void DDGISceneAS::EnsureTLAS(IGraphicsDevice& gfx, uint32_t maxInstances)
{
    if (maxInstances == 0) return;
    if (m_tlas.as.IsValid() && m_tlasCapacity >= maxInstances) return;
    // Round up to a power of two to limit TLAS reallocations as the scene grows.
    uint32_t cap = 1;
    while (cap < maxInstances) cap <<= 1;
    // The result buffer sized for `cap` instances, plus SceneTLAS::kInstanceUploadSlots
    // instance-upload buffers.
    SceneTLAS fresh;
    RHI::ASPrebuildInfo info;
    bool ok = gfx.GetTLASPrebuildInfo(cap, kTLASFlags, info) &&
              gfx.CreateAccelerationStructure(info.resultSize, RHI::ASMemory::RESULT, fresh.as);
    fresh.scratchSize      = info.scratchSize;
    fresh.instanceCapacity = cap;
    RHI::GPUBufferDesc upload;
    upload.size       = uint64_t(cap) * sizeof(RHI::ASInstance);
    upload.usage      = RHI::Usage::UPLOAD;
    upload.bindFlags = RHI::BindFlag::NONE;
    upload.debugName = "DDGISceneAS.TLASInstances";
    for (uint32_t s = 0; ok && s < SceneTLAS::kInstanceUploadSlots; ++s)
        ok = gfx.CreateBuffer(upload, fresh.instances[s]);
    if (!ok)
    {
        ReleaseTLAS(gfx, fresh);   // nothing recorded with any of it yet
        return;
    }
    // Same hazard as EnsureScratch: defer-release the old TLAS and its upload buffers. Lighting/SSR
    // also read the TLAS through the g_DDGITLAS SRV, so it must outlive every in-flight frame.
    ReleaseTLAS(gfx, m_tlas);
    m_tlas         = fresh;
    m_tlasCapacity = cap;
    m_tlasFresh    = false; // freshly allocated -> the first build cannot be an update
}

void DDGISceneAS::ReleaseTLAS(IGraphicsDevice& gfx, SceneTLAS& tlas)
{
    if (tlas.as.IsValid()) gfx.DestroyAccelerationStructure(tlas.as);
    for (RHI::GPUBuffer& b : tlas.instances)
        if (b.IsValid()) gfx.DestroyBuffer(b);
    tlas = SceneTLAS{};
}

uint32_t DDGISceneAS::WriteTLASInstances(IGraphicsDevice& gfx, const SceneASInstance* instances,
                                         uint32_t count, uint32_t frameSlot)
{
    const uint32_t slot = frameSlot % SceneTLAS::kInstanceUploadSlots;
    if (!m_tlas.instances[slot].IsValid() || m_tlas.instanceCapacity == 0) return 0;

    const uint32_t writeCount = std::min(count, m_tlas.instanceCapacity);
    auto* dst = static_cast<RHI::ASInstance*>(gfx.MapBuffer(m_tlas.instances[slot]));
    if (!dst) return 0;
    for (uint32_t i = 0; i < writeCount; ++i)
    {
        const SceneASInstance& src = instances[i];
        RHI::ASInstance d{};
        std::memcpy(d.transform, src.transform, sizeof(d.transform));
        d.instanceID    = src.instanceID & 0x00FFFFFFu;
        d.instanceMask  = src.instanceMask & 0xFFu;
        d.hitGroupIndex = src.hitGroupIndex & 0x00FFFFFFu;
        d.flags         = src.flags;
        d.blasAddress   = src.blasVA;
        dst[i] = d;
    }
    gfx.UnmapBuffer(m_tlas.instances[slot]);

    m_tlas.uploadSlot    = slot;
    m_tlas.instanceCount = writeCount;
    return writeCount;
}

void DDGISceneAS::EnsureMaterialBuffer(IGraphicsDevice& gfx, uint32_t maxInstances)
{
    if (maxInstances == 0) return;
    if (m_matBuffer[0].IsValid() && m_matBufferGPU.IsValid() &&
        m_matCapacity >= maxInstances)
        return;

    // Growth only: free the staging ring and the resident table, then reallocate. This runs from
    // BuildOrRefit before this frame's CL is recorded, so no CL of this frame references the old
    // buffers; DestroyBuffer defers the release kFrameCount frames for the earlier frames' CLs that
    // still read the resident table.
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (m_matBuffer[i].IsValid())
        {
            if (m_matMapped[i]) { gfx.UnmapBuffer(m_matBuffer[i]); m_matMapped[i] = nullptr; }
            gfx.DestroyBuffer(m_matBuffer[i]);
        }
    }
    if (m_matBufferGPU.IsValid()) { gfx.DestroyBuffer(m_matBufferGPU); m_matSrv = 0; }

    uint32_t cap = 1;
    while (cap < maxInstances) cap <<= 1;

    RHI::GPUBufferDesc bd{};
    bd.size       = uint64_t(cap) * sizeof(DDGIInstanceData);
    bd.stride     = sizeof(DDGIInstanceData);
    bd.usage      = RHI::Usage::UPLOAD;
    bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
    bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (gfx.CreateBuffer(bd, m_matBuffer[i]))
        {
            m_matMapped[i] = gfx.MapBuffer(m_matBuffer[i]);
            if (m_matMapped[i]) std::memset(m_matMapped[i], 0, bd.size);
        }
    }

    // The resident table every shader actually reads (g_DDGIInstances, bound
    // as a root SRV by DDGIPass / RTReflectionPass / RTShadowPass / RTAOPass).
    RHI::GPUBufferDesc dd = bd;
    dd.usage = RHI::Usage::DEFAULT;
    if (gfx.CreateBuffer(dd, m_matBufferGPU))
        m_matSrv = gfx.GetBufferSRVGpuHandle(m_matBufferGPU);
    else
        (void)ENGINE_CHECK_ONCE(false,
            "DDGISceneAS: DEFAULT instance table creation failed (%u records) "
            "— RT hit shading reads an unbound SRV this frame", cap);

    m_matCapacity = cap;
    // A fresh DEFAULT-heap table is uninitialised and the master does not describe it: re-upload
    // every record once (m_recordUploadAll).
    m_matMaster.assign(size_t(cap) * sizeof(DDGIInstanceData), 0);
    m_matCopyRanges.clear();
    m_recordUploadAll = true;
}

void DDGISceneAS::EnsureAlphaBuffer(IGraphicsDevice& gfx, uint32_t maxInstances)
{
    // Same lifetime rules as EnsureMaterialBuffer.
    if (maxInstances == 0) return;
    if (m_alphaBuffer[0].IsValid() && m_alphaBufferGPU.IsValid() &&
        m_alphaCapacity >= maxInstances)
        return;

    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (m_alphaBuffer[i].IsValid())
        {
            if (m_alphaMapped[i]) { gfx.UnmapBuffer(m_alphaBuffer[i]); m_alphaMapped[i] = nullptr; }
            gfx.DestroyBuffer(m_alphaBuffer[i]);
        }
    }
    if (m_alphaBufferGPU.IsValid()) { gfx.DestroyBuffer(m_alphaBufferGPU); m_alphaSrv = 0; }

    uint32_t cap = 1;
    while (cap < maxInstances) cap <<= 1;

    RHI::GPUBufferDesc bd{};
    bd.size       = uint64_t(cap) * sizeof(RTAlphaRecord);
    bd.stride     = sizeof(RTAlphaRecord);
    bd.usage      = RHI::Usage::UPLOAD;
    bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
    bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (gfx.CreateBuffer(bd, m_alphaBuffer[i]))
        {
            m_alphaMapped[i] = gfx.MapBuffer(m_alphaBuffer[i]);
            // Zero = flags 0 = "not alpha clipped": a never-written slot reads as fully opaque.
            if (m_alphaMapped[i]) std::memset(m_alphaMapped[i], 0, bd.size);
        }
    }

    RHI::GPUBufferDesc dd = bd;
    dd.usage = RHI::Usage::DEFAULT;
    if (gfx.CreateBuffer(dd, m_alphaBufferGPU))
        m_alphaSrv = gfx.GetBufferSRVGpuHandle(m_alphaBufferGPU);
    else
        (void)ENGINE_CHECK_ONCE(false,
            "DDGISceneAS: DEFAULT alpha-record table creation failed (%u "
            "records) — RT candidate alpha tests read an unbound SRV", cap);

    m_alphaCapacity = cap;
    m_alphaMaster.assign(size_t(cap) * sizeof(RTAlphaRecord), 0);
    m_alphaCopyRanges.clear();
    m_recordUploadAll = true;
}

// ==== World clear ====

void DDGISceneAS::OnWorldClear(IGraphicsDevice& gfx, bool idsPreserved)
{
    // Same reason as Renderer::OnWorldClear: these name IDs from the world
    // being torn down, and World::Clear fires no destroy listeners.
    m_pendingDestroyed.clear();

    // The BLAS cache survives a world reload: MeshLibrary path-dedupes and refcounts its slots, so a
    // same-world reload keeps every (libGen<<32 | libSlot, meshId) key and its BLAS valid (clearing
    // would rebuild 600+ BLAS per reload). Entries left stale by world cycles (A->B->A) are reclaimed
    // by PruneStaleBLAS, which Renderer::OnWorldClear calls after the meshlib two-stage release.
    // The TLAS stays allocated; the next build is a full rebuild, not an update.
    m_tlasFresh         = false;
    m_lastInstanceCount = 0;
    m_transmissiveCount = 0;
    // The alpha table is rewritten from scratch on the next BuildOrRefit; zero the published count so
    // a shader that runs before then bound-checks every InstanceID() out (reads "not alpha clipped").
    m_alphaRecordCount  = 0;
    m_pathMaterialsReady = false;
    m_pathMaterialHash = 0;
    m_giEmitterCount = 0;
    m_giEmitterCacheValid = false;   // the cached table names the old world
    m_pathMatValid = false;          // entity ids recycle with the world

    // Reset the record masters. Instance indices recycle with the world, so stale master bytes would
    // make WriteInstances' memcmp report "unchanged" for records the DEFAULT tables never received.
    // Arming the flag keeps the allocations and re-uploads every record once, as one coalesced span,
    // on the next build.
    m_recordUploadAll   = true;
    m_matCopyRanges.clear();
    m_alphaCopyRanges.clear();

    // Retained gather store: entity ids recycle with the world; the next
    // BuildOrRefit must run a full static gather.
    m_retainedValid      = false;
    m_pendingMirrorsBlob = false;   // the gather scratch does not mirror the blob
    for (BufMirror& bm : m_bufMirror) { bm.valid = false; bm.maskRows.clear(); bm.pageRows.clear(); }
    m_prevGatherDirty.clear();
    m_wantsSlotRows.clear();
    ++m_blobGen;                    // nothing mirrors the old world's blob
    m_retainedLastStamp  = 0;
    m_retainedUnresolved = 0;

    // Skinned BLAS are keyed per entity. OnWorldClear runs mid-frame, so a resource that is dropped
    // goes through the deferred-release queue rather than dying while a submitted CL references it.
    if (idsPreserved)
    {
        // Play -> Stop: the same world comes back from a snapshot, so the entity -> skinned-mesh mapping
        // and the allocation sizes still hold. Keep the buffers: reallocating them is the cost (35
        // AllocateBLAS calls made one Stop ~47 ms - CreateCommittedResource zero-fills on the calling
        // thread). But force a full REBUILD, not a refit: the reuse test only compares vertex/index
        // counts, and a refit of a recycled id against a different mesh's topology hangs traversal.
        // ApplyDestroyed applies the same guard per entity mid-session.
        for (auto& [e, entry] : m_skinnedBlas)
        {
            entry.builtOnce          = false;
            entry.framesSinceRebuild = 0;
        }
    }
    else
    {
        // Real scene change: entity ids recycle against completely different
        // meshes, so every key is invalid.
        for (auto& [e, entry] : m_skinnedBlas)
            if (entry.blas.Valid())
                gfx.DestroyAccelerationStructure(entry.blas.as);
        m_skinnedBlas.clear();
    }
}

void DDGISceneAS::OnEntityDestroyed(Entity e)
{
    // Record only; BuildOrRefit applies it (ApplyDestroyed).
    m_pendingDestroyed.push_back(e);

    // Bounded: only BuildOrRefit drains this, and it runs only while DDGI or an RT screen effect is on,
    // so with both off the list would grow forever. On overflow mark every cached skinned BLAS for a
    // full rebuild and drop the list - correct whatever the threading, unlike applying the destroy
    // inline here.
    constexpr size_t kMaxPendingDestroyed = 4096;
    if (m_pendingDestroyed.size() > kMaxPendingDestroyed)
    {
        for (auto& kv : m_skinnedBlas)
        {
            kv.second.builtOnce          = false;
            kv.second.framesSinceRebuild = 0;
        }
        m_pendingDestroyed.clear();
    }
}

// Applies the destroys OnEntityDestroyed recorded: each id keeps its allocation, but its next
// build is a full REBUILD (OnEntityDestroyed's declaration says why).
void DDGISceneAS::ApplyDestroyed()
{
    if (!m_pendingDestroyed.empty())
    {
        static bool s_reported = false;
        if (!s_reported)
        {
            s_reported = true;
            LOG_INFO("[SPLITAUDIT][DESTROYLANE] DDGI applied %zu deferred destroys "
                     "- the lane is exercised", m_pendingDestroyed.size());
        }
    }
    for (Entity e : m_pendingDestroyed)
    {
        auto it = m_skinnedBlas.find(e);
        if (it != m_skinnedBlas.end())
        {
            it->second.builtOnce          = false;
            it->second.framesSinceRebuild = 0;
        }
    }
    m_pendingDestroyed.clear();
}

void DDGISceneAS::PruneStaleBLAS(Resource::MeshLibrary* meshLib, IGraphicsDevice& gfx)
{
    if (!meshLib || m_blasCache.empty()) return;

    size_t pruned = 0;
    for (auto it = m_blasCache.begin(); it != m_blasCache.end(); )
    {
        if (it->first.sourceKind != 0)
        {
            // MeshHandle (procedural): no generation to compare, and primitive BLAS are tiny - keep them.
            ++it;
            continue;
        }

        // Reconstruct the meshlib Handle from CacheKey.libOrSlot.
        const uint32_t slotIdx = static_cast<uint32_t>(it->first.libOrSlot & 0xFFFFFFFFu);
        const uint16_t slotGen = static_cast<uint16_t>(it->first.libOrSlot >> 32);
        const Resource::Handle libHandle = Resource::Handle::Make(
            slotIdx, Resource::ResourceType::MeshLibrary, slotGen);

        if (meshLib->IsValid(libHandle))
        {
            ++it;
            continue;
        }

        // Slot freed or generation bumped: the entry is stale. The BLAS is separate memory from the
        // source VB/IB, so defer-releasing it is safe; DestroyAccelerationStructure also defer-releases a
        // mid-compaction entry's size-query buffers.
        if (it->second.Valid())
            gfx.DestroyAccelerationStructure(it->second.as);
        it = m_blasCache.erase(it);
        ++pruned;
    }

    if (pruned > 0)
        LOG_INFO("DDGISceneAS: pruned %zu stale BLAS entries", pruned);
}

// ==== Per-frame build/refit ====

// The BINDING half of a row's material proxy (see the header).
void DDGISceneAS::BindProxy(Pending& p, Entity e) const
{
    if (!p.hasMaterial) return;
    p.matSlot = 0u;
    if (p.wantsMatSlot && m_bindRenderer)
    {
        // Same-frame slot the raster emitted for this entity's material - valid because
        // BuildRenderScene runs before BuildOrRefit. Entities that emitted nothing this frame (culled
        // statics) stay 0 and the shader falls back to defaults (toon knobs, no exposure protection).
        const uint32_t slot = m_bindRenderer->GetEntityMaterialSlot(e);
        if (slot < m_bindRenderer->GetMaterialSlotCapacity())
            p.matSlot = slot + 1u;
    }
    // Material surface cache: the authoritative proxy for a custom-shader material, replacing
    // applyCustomProxy's name-alias guess. Runs last: it must overwrite the hit-shading fields
    // (albedo/surface/rough/metal/uv) and the candidate alpha-test fields (alphaTexIdx/alphaFromRed)
    // together. Committed-hit shading and the per-candidate alpha test read them in different stages
    // of one trace, so repointing only one makes every cutout silhouette sample the wrong texture.
    if (m_bindSurfaceCache && !p.rtStdMat)
    {
        MaterialSurfaceCache::PageBinding page;
        if (m_bindSurfaceCache->LookupEntity(static_cast<uint32_t>(e), page))
        {
            p.surfaceCache   = true;
            // The page is measured material output, not a guess: switch off the environment-substitution
            // fallback for this instance.
            p.proxyUntrusted = false;
            p.surfaceTexIdx  = page.surfaceTexIdx;
            // Albedo resolution: a page is kPageSize^2 (128^2) texels for the whole material, so a 2048^2
            // character texture loses 16x per axis. When the space3 alias resolved the real albedo, keep it
            // (full res, raw mesh UV, RT_INST_FLAG_ALBEDO_RAW) and use the page for surface, emissive mask
            // and opacity only. baseColor then stays the authored tint: the raw texture lacks the multiply
            // the page bakes in.
            p.albedoRaw    = (p.aliasAlbedoTexIdx >= 0);
            p.albedoTexIdx = p.albedoRaw ? p.aliasAlbedoTexIdx
                                         : page.albedoTexIdx;

            // Neutralise the material scalars. The page already holds baseColor x texture and the final
            // roughness/metalness, while the RT shader computes
            //     albedo   = inst.baseColor.rgb * atlas.rgb;
            //     hitRough = lerp(roughnessMin, roughnessMax, surf.g);
            //     hitMetal = lerp(metalnessMin, metalnessMax, surf.b);
            // A white tint and [0,1] ranges make both the identity, with no shader change. baseColor.w stays:
            // it is the material opacity the candidate alpha test multiplies by. ALBEDO_RAW instances keep
            // the tint (their raw map does not bake it in).
            if (!p.albedoRaw)
            {
                p.baseColor.x = 1.0f;
                p.baseColor.y = 1.0f;
                p.baseColor.z = 1.0f;
            }
            p.roughnessMin = 0.0f;  p.roughnessMax = 1.0f;
            p.metalnessMin = 0.0f;  p.metalnessMax = 1.0f;

            // From here uvTilingOffset locates the page in the atlas (set below), so every texture sampled
            // through it must be the page: one of the material's own textures would be sampled as a
            // 1/256-area crop at an arbitrary offset. Emissive (read by HybridReflIntersectHW and
            // DDGIRayTrace) is therefore replaced by the surface page's baked emissive mask. normalTexIdx
            // stays: pages are only acquired for identity-texMulAdd materials (Renderer_Scene), and
            // HybridReflIntersectHW samples it at the raw mesh UV. DDGI never reads normalTexIdx.
            p.emissiveTexIdx = -1;

            // The material's own texMulAdd is baked into the page, so this slot is free to carry the page
            // rect. Renderer_Scene only pages identity-tiling materials, so nothing is lost.
            p.uvTilingOffset[0] = page.rect[0];
            p.uvTilingOffset[1] = page.rect[1];
            p.uvTilingOffset[2] = page.rect[2];
            p.uvTilingOffset[3] = page.rect[3];

            // The alpha test moves to the page in the same write (see above): opacity lives in the page's
            // alpha channel, so the OPACITYMAP .r mode clears.
            p.alphaTexIdx  = page.albedoTexIdx;
            p.alphaFromRed = false;
        }
    }
}

// ==== GATHER half: runs on the game side ====
// Everything that reads the World is here; BuildOrRefit takes no World and touches no pools.
// Call it after Renderer::BuildRenderScene (Renderer_Frame calls it just before UploadFrameData):
// the retained-gather gate compares the surface cache's publish epoch with the last one seen, and
// MaterialSurfaceCache::AcquirePage - called only from BuildScene_SortAndEmit, inside
// BuildRenderScene - bumps it on page eviction and on a content-hash un-publish. An earlier call
// captures a pre-eviction epoch and shades RT/DDGI for a frame against an overwritten page. Never
// snapshot the epoch between MaterialSurfaceCache::BeginFrame and SortAndEmit.
// Keep the three epoch lines together: the compare and the store are one action, and split they
// leave the gate stuck true or stuck false (a full gather every frame).
void DDGISceneAS::Gather(World&                        world,
                         IGraphicsDevice&                 gfx,
                         Resource::MeshLibrary*        meshLib,
                         MeshManager*                  meshMgr,
                         const IMaterialRuntimeLookup* materialRuntime,
                         SkinnedMeshSubsystem*         skin,
                         bool                          includeSkinned)
{
    ZoneScopedN("DDGI.Gather");

    // The write buffer's rows are not cleared up front: its static prefix may still mirror the
    // retained blob (m_bufMirror). Every exit that does not reuse it drops it; the full gather clears
    // it itself.
    auto dropWriteBuffer = [this]()
    {
        GatherWrite().pending.clear();
        m_bufMirror[m_gatherWrite].valid = false;
        m_bufMirror[m_gatherWrite].pageRows.clear();
    };
    GatherWrite().skinnedCount = 0;
    GatherWrite().deformHashed = false;
    GatherWrite().valid        = false;
    if (!gfx.GetCapabilities().raytracing) { dropWriteBuffer(); return; }
    ++m_gatherSerial;

    auto& pending = GatherWrite().pending;
    // -ddgibindfused: bind the proxy inline in fillMaterialProxy instead of in BuildOrRefit - the A/B
    // arm of -tlasdigest.
    static const bool s_bindFused = Sys::CommandLine::Get().Has("-ddgibindfused");
    // -ddgiblobverify: check every frame, after both gather paths, that the retained blob is still
    // PRE-binding. -ddgiblobfullcopy: its reverse arm - the patch path copies the whole record into
    // the blob, not just the matrix.
    static const bool s_blobVerify   = Sys::CommandLine::Get().Has("-ddgiblobverify");
    static const bool s_blobFullCopy = Sys::CommandLine::Get().Has("-ddgiblobfullcopy");
    // -ddgimirroroff: every patch frame replays the blob instead of using the per-buffer mirrors.
    // -ddgimirrorverify: on every mirror hit also build the replay and compare the two static prefixes
    // byte for byte.
    static const bool s_mirrorOff    = Sys::CommandLine::Get().Has("-ddgimirroroff");
    static const bool s_mirrorVerify = Sys::CommandLine::Get().Has("-ddgimirrorverify");

    // ---- Phase A: collect the static-mesh rows ----
    // A row needs GlobalTransform and either MeshLibRef (with meshLib) or MeshHandle (with meshMgr),
    // and no MeshSkinnedComponent (those take the skinned path below).
    // TODO(DDGI_Integration_Plan 3.1): honour DDGISceneTagComponent to force-include / force-exclude
    // entities; until then the rule above is conservative.
    auto* pGT     = world.GetPool<GlobalTransform>();
    auto* pLibRef = world.GetPool<MeshLibRef>();
    auto* pMesh   = world.GetPool<::MeshHandle>();
    auto* pSkin   = world.GetPool<MeshSkinnedComponent>();
    auto* pVis    = world.GetPool<VisibilityComponent>();
    if (!pGT) { dropWriteBuffer(); return; }

    // DXR instance mask, bit-partitioned so ONE TLAS serves every RT consumer:
    //   bit 0 (0x01) reflections + RTAO: main-pass-visible; a hidden object drops it and rays pass
    //                through (no mirror ghosts)
    //   bit 1 (0x02) RT sun-shadow caster: VisibilityComponent::CastsShadow and
    //                MaterialComponent::IsCastingShadow, the raster ShadowPass gates
    //   bit 2 (0x04) DDGI probe/shadow rays: all statics, even main-pass-hidden ones; never skinned
    //                (deforming BLAS destabilise probe relocation and backface classification)
    //   bit 3 (0x08) translucent front-layer reflection rays (RTReflectionPass
    //                Variant::TranslucentFront): main-pass-visible minus the forward-translucent
    //                instances. Those rays start on glass and a blend instance is a committed hit for
    //                them, so with it a pane would reflect its own far side at t ~ 0 (a black pane);
    //                glass-in-glass is second-order.
    // Ray masks: RTR + RTAO 0x01, RTShadow 0x02, DDGI 0x04, front-layer RTR 0x08.
    // No VisibilityComponent, with no material or an opaque shadow-casting one -> 0x0F.
    auto* pMat = world.GetPool<MaterialComponent>();
    // Dissolve past the halfway point drops bits 0, 1 and 3: the RT shaders trace FORCE_OPAQUE, so a
    // per-pixel noise clip is impossible and a binary flip at 0.5 is the cheapest consistent answer
    // (raster shadows clip per pixel in Shadow.ps). Bit 2 (DDGI) stays: the probe EMA absorbs the
    // change, and flipping it risks probe reclassification churn. The nearest-tagged-ancestor walk
    // mirrors Renderer::BuildScene_ComputeDissolveSet.
    auto* pDissolve  = world.GetPool<DissolveComponent>();
    auto* pParentDis = world.GetPool<Parent>();
    const bool anyDissolve = pDissolve && !pDissolve->Entities().empty();
    auto dissolvedPastHalf = [pDissolve, pParentDis, anyDissolve](Entity e) -> bool
    {
        if (!anyDissolve) return false;
        Entity cur = e;
        for (int depth = 0; depth < 64 && cur != NullEntity; ++depth)
        {
            if (const DissolveComponent* d = pDissolve->Get(cur))
                return d->amount > 0.5f;   // nearest tag wins (nested overrides)
            const Parent* p = pParentDis ? pParentDis->Get(cur) : nullptr;
            cur = p ? p->entity : NullEntity;
        }
        return false;
    };
    auto instanceMask = [pVis, pMat, dissolvedPastHalf](Entity e) -> uint8_t
    {
        bool effVisible = true, mainPass = true, castShadow = true;
        bool forwardTranslucent = false;   // Alpha / Additive / Premultiplied / Multiply / Transmissive
        bool transmissive = false;
        if (pVis)
            if (const VisibilityComponent* v = pVis->Get(e))
            {
                effVisible = v->IsEffectivelyVisible();
                mainPass   = v->RendersInMainPass();
                castShadow = v->CastsShadow();
            }
        if (pMat)
            if (const MaterialComponent* mc = pMat->Get(e))
            {
                castShadow = castShadow && mc->IsCastingShadow();
                // Dithered draws on the opaque route and IS a solid GBuffer surface - it keeps bit 3; only the
                // forward modes lose it.
                forwardTranslucent = IsTransparentBlendMode(mc->userBlendMode);
                transmissive       = IsTransmissiveBlendMode(mc->userBlendMode);
                // Additive / Multiply add or modulate light and never absorb it: raster skips them as shadow
                // casters (ClassifyShadowDrawGroup), so RT does too. Alpha and Premultiplied keep the bit and
                // become partial occluders via kRTAlphaFlagBlend.
                if (mc->userBlendMode == BlendMode::Additive ||
                    mc->userBlendMode == BlendMode::Multiply)
                    castShadow = false;

                // Water surfaces leave the TLAS entirely (mask 0). A water plane is ordinary geometry and would
                // trace as an opaque slab: a grey lid in RT reflections, RTAO occlusion and darkened RT shadows
                // under it, black DDGI probes below the surface. Raster routes it away via DrawFilter::Water;
                // this is the RT half of that gate. RT reflections therefore never see water; water's own
                // reflection comes from the screen-space chain plus gRadiance/probes.
                if (mc->IsWaterShadingModel())
                    return 0u;

                // NPR screen-space overlay cards leave the TLAS entirely (mask 0): forward-transparent
                // custom-shader materials that cast no shadow (eyeshadow quads, brow/lash blend cards, additive
                // eye sheens). RT traversal cannot blend: an Alpha card commits as a solid triangle
                // (RT_ALPHA_FLAG_BLEND accepts unconditionally) and Additive/Multiply never get
                // FORCE_NON_OPAQUE (blendOpacity 0), so a 5%-opacity lash card would reflect as an opaque,
                // usually env-substituted quad (PROXY_UNTRUSTED). They are a screen-space technique
                // (FaceOverlayPass), not geometry. Masked overlays keep their bit: they alpha-clip correctly.
                if (mc->useCustomShader && !mc->IsCastingShadow() &&
                    IsTransparentBlendMode(mc->userBlendMode))
                    return 0u;
            }
        // Bit 2 is DDGI_RAY_MASK, which DDGI's probe and shadow rays trace FORCE_OPAQUE, so the
        // forward-translucent modes do not get it: a glass pane would be a full-albedo wall that blocks
        // all indirect light through windows (and ReSTIR GI takes its second bounce from those probes).
        // ReSTIR GI's and the path tracer's own rays pass through glass; the forward composite tints it.
        // Additive / Multiply VFX are not DDGI walls either.
        uint8_t m = forwardTranslucent ? 0x00u : 0x04u;
        if (effVisible && mainPass)   m |= 0x01u;
        if (effVisible && castShadow) m |= 0x02u;
        if (effVisible && mainPass && !forwardTranslucent) m |= 0x08u;
        // Bit 4: a Transmissive surface the path tracer's continuation crosses as a vertex (it
        // traces 0x08 | 0x10); every other ray leaves it to the bits above.
        if (effVisible && mainPass && transmissive) m |= 0x10u;
        if (dissolvedPastHalf(e))     m &= static_cast<uint8_t>(~0x1Bu);
        return m;
    };

    // Structural-hide tags - the raster gather's rule (always skip Disabled; in Game builds also
    // Editor-only / HiddenInGame). These entities render nowhere, so unlike VisibilityComponent-hidden
    // ones they leave the TLAS entirely (no GI contribution either).
    auto* pDisabledTag   = world.GetPool<DisabledTag>();
    // Runtime check, not #ifdef WITH_EDITOR: EngineCore is compiled once for every exe and never sees
    // that define (System/HostPolicy.h). In an authoring host the two pools stay null, so the tests
    // below are free.
    const bool cullEditorOnly = !Sys::IsEditorHost();
    auto* pEditorOnlyTag = cullEditorOnly ? world.GetPool<EditorOnlyTag>()   : nullptr;
    auto* pHiddenInGame  = cullEditorOnly ? world.GetPool<HiddenInGameTag>() : nullptr;
    auto structurallyHidden = [&](Entity e) -> bool
    {
        if (pDisabledTag   && pDisabledTag  ->Get(e)) return true;
        if (pEditorOnlyTag && pEditorOnlyTag->Get(e)) return true;
        if (pHiddenInGame  && pHiddenInGame ->Get(e)) return true;
        return false;
    };

#if DDGI_GT_DIAGNOSTICS
    m_bindlessInvalidCount = 0; // per-gather diagnostics, see the meshlib loop
    m_emissiveTexCount     = 0; // + fillMaterialProxy
    m_emissiveTexDistinct.clear();
#endif
    // Reserve the worst case: Pending is ~200 B and city-scale scenes gather 7K+ rows (~2 MB); the
    // pool sizes are exact upper bounds (rows are only filtered out). `pending` is a member buffer, so
    // its capacity persists across frames, and it is not cleared here (see m_bufMirror): the patch
    // path reuses or replays its rows, the full gather clears it. Not a thread_local: Gather and
    // BuildOrRefit run on different threads.
    size_t gatherUpperBound = 0;
    if (pLibRef)                 gatherUpperBound += pLibRef->Entities().size();
    if (pMesh)                   gatherUpperBound += pMesh->Entities().size();
    if (includeSkinned && pSkin) gatherUpperBound += pSkin->Entities().size();
    pending.reserve(std::max<size_t>(gatherUpperBound, 256));

    // Downcast to the concrete Renderer for the batched material resolve: the interface's
    // GetMaterialTextureRuntime pays a 2-map resolve per slot (four slots per row), which dominated
    // this gather at 7K entities. Null (another IMaterialRuntimeLookup) falls back to the per-slot
    // calls.
    const Renderer* rendererLookup = dynamic_cast<const Renderer*>(materialRuntime);

    // Material surface cache: non-null whenever the Renderer is the lookup. LookupEntity returns
    // "no page" while the feature is off, but the publish epoch must stay readable so the off edge
    // invalidates the retained gather.
    const MaterialSurfaceCache* surfaceCache =
        rendererLookup ? rendererLookup->GetMaterialSurfaceCache() : nullptr;

    // RTR material proxy for custom-shader materials: their look comes from the custom PS (space3
    // textures + b8 params), which a ray cannot run. Fold the engine template's knobs into the proxy
    // so reflections track what artists author - TintColor (lerped by TintStrength, as the template
    // does), EmissiveColor x EmissiveIntensity, and the albedo/normal maps found by name (texture
    // aliases below). Other shaders keep the standard baseColor/emissive fields.
    auto applyCustomProxy = [&](Entity e, const MaterialComponent& mc, auto& p)
    {
        if (!mc.useCustomShader || mc.rtUseStandardMaterial) return;
        auto find4 = [&](const char* n) -> const std::array<float, 4>*
        {
            auto it = mc.customParams.find(n);
            return it != mc.customParams.end() ? &it->second : nullptr;
        };
        if (const auto* tint = find4("TintColor"))
        {
            float s = 1.0f;
            if (const auto* ts = find4("TintStrength")) s = (*ts)[0];
            // lerp(base, base*tint, s) == base * lerp(1, tint, s)
            p.baseColor.x *= 1.0f + ((*tint)[0] - 1.0f) * s;
            p.baseColor.y *= 1.0f + ((*tint)[1] - 1.0f) * s;
            p.baseColor.z *= 1.0f + ((*tint)[2] - 1.0f) * s;
        }
        if (const auto* ec = find4("EmissiveColor"))
        {
            // Require an explicit intensity param: a shader whose reflected EmissiveColor merely
            // defaults to white must not glow in RTR.
            const auto* ei = find4("EmissiveIntensity");
            const float k  = ei ? (*ei)[0] : 0.0f;
            p.emissive.x += (*ec)[0] * k;
            p.emissive.y += (*ec)[1] * k;
            p.emissive.z += (*ec)[2] * k;
        }
        // ---- Texture aliases ----
        // A custom-shader material hides the standard Texture-Slots panel, so its real maps live in
        // name-keyed space3 slots while textures[BASECOLORMAP] keeps the importer's default
        // (asset/Default_Texture/Default_*.itex) - a white mannequin in every mirror, DDGI bounce and RT
        // alpha test. Look the maps up by the names this project's shaders declare, e.g.
        //     Texture2D _BaseMap : register(t0, space3);
        // This is the fallback: the material surface cache bakes the material's own GBuffer output into
        // an atlas page; the aliases cover materials whose page is not baked yet or failed to bake, or a
        // disabled cache.
        // Surface (roughness/metalness) is NOT aliased: channel packing is per shader (endfield's
        // `_PBRMap` is R=metallic, B=occlusion, A=smoothness, not the engine's R=AO G=rough B=metal), so
        // rough/metal stay on the authored min/max scalars until a page supplies them.
        if (materialRuntime)
        {
            auto firstTex = [&](std::initializer_list<const char*> names) -> int32_t
            {
                for (const char* n : names)
                {
                    const auto t =
                        materialRuntime->GetMaterialCustomTextureRuntime(e, n);
                    if (t.bindlessIndex >= 0) return t.bindlessIndex;
                }
                return -1;
            };
            // Ordered by specificity: the engine's own canonical name first,
            // then the conventions actually used by this project's shaders.
            const int32_t a = firstTex({ "AlbedoMap", "_BaseMap", "BaseMap",
                                         "_BaseColorMap", "_MainTex", "MainTex",
                                         "_DiffuseMap", "_D" });
            if (a >= 0) p.albedoTexIdx = a;
            // Kept on the row so BindProxy's surface-cache override can keep the full-res aliased
            // albedo (RT_INST_FLAG_ALBEDO_RAW) instead of the 128^2 page.
            p.aliasAlbedoTexIdx = a;
            const int32_t n = firstTex({ "NormalMap", "_NormalMap", "_HairNormal",
                                         "_NormalTex", "_HN", "_N" });
            if (n >= 0) p.normalTexIdx = n;

            // Untrusted needs BOTH: no alias matched, and the engine slot is empty or still the
            // importer's default. A custom-shader material may legitimately author BASECOLORMAP;
            // flagging it would throw away a good albedo.
            if (a < 0)
            {
                const std::string& baseName =
                    mc.textures[MaterialComponent::BASECOLORMAP].name;
                const bool placeholder =
                    baseName.empty() ||
                    baseName.find("Default_Texture") != std::string::npos;
                // A guess, not authored data: RT_INST_FLAG_PROXY_UNTRUSTED lets the trace substitute an
                // environment estimate. BindProxy clears it when the surface cache has a page.
                p.proxyUntrusted = placeholder;
            }
        }
    };

    // fillMaterialProxy: a row's base color, emissive and RTR material proxy (the World half), shared
    // by all three gather paths; neutral defaults without a MaterialComponent. Emissive matches
    // GBuffer.ps:
    //   final = emissiveColor.rgb * emissiveColor.w * emissiveTex.rgb
    // The constant part is pre-multiplied here; the trace samples the texture per hit. emissive.a
    // carries the shading model (an Unlit hit shades flat albedo, as Lighting.ps does). Proxy fields
    // follow material_surface.hlsli's remap rules.
    // The BINDING half (BindProxy) needs this frame's emit: the raster material slot
    // (GetEntityMaterialSlot, epoch-gated on the emit) and the surface-cache page (LookupEntity).
    // BuildOrRefit binds the rows (BindGatheredRows) after this frame's plan and emit nest, on the
    // thread that records the AS, so Gather's rows and the retained blob stay PRE-binding. With
    // -ddgibindfused, fillMaterialProxy binds at its end instead. The page block runs last and
    // overwrites the hit-shading and alpha-test fields together.
    m_bindRenderer     = rendererLookup;
    m_bindSurfaceCache = surfaceCache;
    auto bindProxy = [this](Pending& p, Entity e) { BindProxy(p, e); };

    auto fillMaterialProxy = [&](Entity e, auto& p)
    {
        constexpr XMFLOAT4 kDefaultColor{ 0.5f, 0.5f, 0.5f, 1.0f };
        p.baseColor      = kDefaultColor;
        p.emissive       = { 0.0f, 0.0f, 0.0f, 0.0f };
        p.emissiveTexIdx = -1;
        p.wind           = {};
        p.hasMaterial    = false;
        p.rtStdMat       = false;
        p.wantsMatSlot   = false;
        if (!pMat) return;
        const MaterialComponent* mc = pMat->Get(e);
        if (!mc) return;
        p.hasMaterial = true;
        p.rtStdMat    = mc->rtUseStandardMaterial;

        // ---- Foliage wind opt-in ----
        // The engine cannot introspect an arbitrary WPO hook, so a material opts in by declaring the two
        // FoliageWind customParams (FoliageWind.h owns the contract and the CPU copy of the sway). A
        // material edit sets matDirty, which forces the full gather, so the retained blob never carries
        // stale wind values.
        if (!mc->customParams.empty())
        {
            const auto itDir = mc->customParams.find(FoliageWind::kParamDirection);
            const auto itMot = mc->customParams.find(FoliageWind::kParamMotion);
            if (itDir != mc->customParams.end() && itMot != mc->customParams.end())
                p.wind = FoliageWind::FromRaw(itDir->second.data(), itMot->second.data());
        }
        // sRGB -> linear, as the raster path uploads mc->_linearBaseColor / _linearEmissiveColor
        // (WriteMaterialSlot); the raw sRGB value would make every reflected and GI-bounced tint far too
        // bright (0.3 instead of 0.3^2.2 = 0.073). Same curve as RefreshLinearCache (gamma 2.2); alpha is
        // opacity and passes through.
        // Use the cache only when clean: GPU_LINEAR_CACHE_DIRTY starts set and only WriteMaterialSlot
        // clears it, which never runs for an off-screen material - exactly the reflection-only case,
        // which would get the white {1,1,1,1} default.
        const bool linCacheValid =
            (mc->flags & MaterialComponent::GPU_LINEAR_CACHE_DIRTY) == 0;
        auto toLinear = [](float c) { return std::powf(std::max(c, 0.0f), 2.2f); };

        p.baseColor = linCacheValid
            ? mc->_linearBaseColor
            : XMFLOAT4{ toLinear(mc->baseColor.x), toLinear(mc->baseColor.y),
                        toLinear(mc->baseColor.z), mc->baseColor.w };

        const float s = mc->emissiveColor.w;
        const XMFLOAT4 emLin = linCacheValid
            ? mc->_linearEmissiveColor
            : XMFLOAT4{ toLinear(mc->emissiveColor.x), toLinear(mc->emissiveColor.y),
                        toLinear(mc->emissiveColor.z), mc->emissiveColor.w };
        // .a = the shading model, raw. Renderer::WriteMaterialSlot applies one transform to the same
        // value: on the FORWARD route a material without a custom shader reports Standard instead of
        // Unlit (the forward Unlit branch needs the hook to fill s.emissive). On the deferred route both
        // report Unlit, and every other model agrees. Read by the Unlit tests of HybridReflIntersectHW
        // (`hitUnlit`) and rt_hit_surface.hlsli (`s.unlit`).
        p.emissive  = { emLin.x * s,
                        emLin.y * s,
                        emLin.z * s,
                        static_cast<float>(mc->customShadingModel) };

        // NPR detection: the axis the raster stencil classifier keys on (ShadingStencil::RefFor ->
        // stencil ref 2 for Toon).
        p.npr = (mc->customShadingModel == ShadingModel::Toon);
        p.outline = mc->IsOutlineEnabled();
        // Which models want the material slot is the shading-model table's `rtMatSlot` column (Toon for
        // its NPR knobs, Unlit for exposureCompensation; see Pending::matSlot). Keyed on the raw model
        // packed into emissive.a, so it agrees with the shader's `hitUnlit` test by construction. The
        // slot lookup is BindProxy's; this records only whether one is wanted.
        p.wantsMatSlot = ShadingModels::Of(mc->customShadingModel).rtMatSlot;
        p.matSlot = 0u;
        if (rendererLookup)
        {
            // One 2-lookup resolve for all four slots, bit-identical to the per-slot fallback below:
            // GetMaterialTextureRuntime(e, slot) is ResolveMaterialRuntime(e).EngineTex(slot), so
            // per-entity overrides (binding->overrideMask) apply identically.
            const Renderer::ResolvedMaterialRuntime rt =
                rendererLookup->ResolveMaterialRuntime(e);
            p.emissiveTexIdx = rt.EngineTex(MaterialComponent::EMISSIVEMAP).bindlessIndex;
            p.albedoTexIdx   = rt.EngineTex(MaterialComponent::BASECOLORMAP).bindlessIndex;
            p.surfaceTexIdx  = rt.EngineTex(MaterialComponent::SURFACEMAP).bindlessIndex;
            p.normalTexIdx   = rt.EngineTex(MaterialComponent::NORMALMAP).bindlessIndex;
        }
        else if (materialRuntime)
        {
            p.emissiveTexIdx = materialRuntime->GetMaterialTextureRuntime(
                e, MaterialComponent::EMISSIVEMAP).bindlessIndex;
            p.albedoTexIdx = materialRuntime->GetMaterialTextureRuntime(
                e, MaterialComponent::BASECOLORMAP).bindlessIndex;
            p.surfaceTexIdx = materialRuntime->GetMaterialTextureRuntime(
                e, MaterialComponent::SURFACEMAP).bindlessIndex;
            p.normalTexIdx = materialRuntime->GetMaterialTextureRuntime(
                e, MaterialComponent::NORMALMAP).bindlessIndex;
        }
        p.roughnessMin   = mc->roughnessMin;
        p.roughnessMax   = mc->roughnessMax;
        p.metalnessMin   = mc->metalnessMin;
        p.metalnessMax   = mc->metalnessMax;
        p.reflectance    = mc->reflectance;
        // Material UV transform: the source the raster path uploads into
        // MaterialGPUData::uvTilingOffset (WriteMaterialSlot), so a tiled material reflects the way it
        // rasterises.
        p.uvTilingOffset[0] = mc->texMulAdd.x;
        p.uvTilingOffset[1] = mc->texMulAdd.y;
        p.uvTilingOffset[2] = mc->texMulAdd.z;
        p.uvTilingOffset[3] = mc->texMulAdd.w;
        applyCustomProxy(e, *mc, p);
#if DDGI_GT_DIAGNOSTICS
        // Diagnostic: instances with a resolved emissive texture pay a NonUniformResourceIndex
        // SampleLevel per hit in DDGIRayTrace; the count and diversity feed the throttled SceneAS line.
        if (p.emissiveTexIdx >= 0)
        {
            m_emissiveTexCount++;
            m_emissiveTexDistinct.insert(p.emissiveTexIdx);
        }
#endif

        // ---- RT alpha cutout ----
        // The raster clip predicate (IsAlphaTestEnabled, which also sets PermutationKey::ALPHA_TEST and
        // drives Shadow.ps), so RT is never coarser than raster for the same entity. alphaRef <= 1/256 is
        // excluded: it reports alpha test on but never discards, and must not pay non-opaque traversal.
        // Not gated on albedoTexIdx >= 0: that index is transiently -1 while streaming and after
        // OnWorldClear; gating on it would leave foliage solid after a load and flip the instance flag
        // when it resolves (defeating the fingerprint skip). The shaders already treat < 0 as solid.
        // ---- RT alpha blend (translucent occluder) ----
        // The forward-transparent modes reach the TLAS with a FLAG_OPAQUE BLAS, so a window would cast a
        // solid RT sun shadow; flag them so the RTShadow candidate loop attenuates instead.
        // The blend mode decides, not alphaRef: IsAlphaTestEnabled has no blend-mode gate, so keying on
        // !alphaClip would shadow two identical panes differently over a stray alphaRef, and the
        // record's shared `alphaRef` slot would be ambiguous. A transparent card keeps its silhouette:
        // the blend path multiplies BASECOLORMAP.a by opacity, so it softens instead of clipping.
        // Dithered rasterises on the opaque route but is translucent: attenuate it like Alpha.
        const bool isBlendMode = IsTransparentBlendMode(mc->userBlendMode)
                              || IsDitheredBlendMode(mc->userBlendMode);

        // The reconciled clip threshold MaskClipValue() - what raster clips against
        // (MaterialGPUData.alphaRef, WriteMaterialSlot) - not the raw field, or a Masked material with
        // an unset Alpha Ref gets a different silhouette in RT than in raster.
        p.alphaRef    = mc->MaskClipValue();
        p.alphaClip   = !isBlendMode && mc->IsAlphaTestEnabled()
                                     && mc->alphaRef > (1.0f / 256.0f);
        p.doubleSided = mc->IsDoubleSided();

        // Additive and Multiply get opacity 0 (never attenuate): they add or modulate light, and raster
        // skips them as shadow casters (ClassifyShadowDrawGroup). instanceMask already drops their
        // caster bit; the 0 keeps them out of non-opaque traversal (the FORCE_NON_OPAQUE gate in
        // BuildOrRefit), so high-instance-count VFX pay no candidate callback in RTR/RTAO.
        p.blendShadow = isBlendMode && m_blendShadowOccluders;
        p.shadowTint  = 0;
        if (p.blendShadow)
        {
            const bool nonAbsorbing = (mc->userBlendMode == BlendMode::Additive)
                                   || (mc->userBlendMode == BlendMode::Multiply);
            p.blendOpacity = nonAbsorbing ? 0.0f : mc->GetOpacity();
            // Transmissive: what one crossing stops, of a covered pixel what does not cross. A shadow
            // ray meets every face (none is culled): a thin wall is a wall at each, and a solid's
            // entering and leaving faces each keep the square root of the whole object's light.
            // The record's opacity is the grey of it (luminance); its colour rides in the flags.
            if (IsTransmissiveBlendMode(mc->userBlendMode))
            {
                const std::array<float, 3> through = TransmissiveShadowTransmittance(*mc);
                const float cover = p.blendOpacity;
                const bool  solid = mc->thickness > 0.f;
                auto perFace = [cover, solid](float t)
                {
                    const float kept = 1.0f - cover * (1.0f - t);
                    return solid ? std::sqrt(kept) : kept;
                };
                p.blendOpacity = 1.0f - perFace(0.2126f * through[0] + 0.7152f * through[1] + 0.0722f * through[2]);
                p.shadowTint   = kRTAlphaFlagTint;
                for (uint32_t c = 0; c < 3; ++c)
                    p.shadowTint |= uint32_t(std::lround(std::clamp(perFace(through[c]), 0.f, 1.f) * 255.f)) << (8u + 8u * c);
            }
        }
        p.transmissive = IsTransmissiveBlendMode(mc->userBlendMode);

        // Masked-mode opacity mask: when the material authors an OPACITYMAP (gated on the path - stable
        // authored intent, unlike the streaming bindless index), the candidate alpha test samples its .r
        // instead of BASECOLORMAP.a; an unresolved mask index degrades to solid like the albedo case.
        // Not for blend occluders: the forward transparent pass ignores OPACITYMAP and blends with
        // BASECOLORMAP.a x baseColor.w, so the shadow must too.
        p.alphaTexIdx  = p.albedoTexIdx;
        p.alphaFromRed = false;
        if (!p.blendShadow && !mc->textures[MaterialComponent::OPACITYMAP].name.empty())
        {
            int32_t maskIdx = -1;
            if (rendererLookup)
                maskIdx = rendererLookup->ResolveMaterialRuntime(e)
                              .EngineTex(MaterialComponent::OPACITYMAP).bindlessIndex;
            else if (materialRuntime)
                maskIdx = materialRuntime->GetMaterialTextureRuntime(
                    e, MaterialComponent::OPACITYMAP).bindlessIndex;
            p.alphaTexIdx  = maskIdx;
            p.alphaFromRed = true;
        }

        // -ddgibindfused: bind here, last; otherwise BuildOrRefit binds.
        if (s_bindFused) bindProxy(p, e);
    };

    // ---- Retained-static gate (see DDGISceneAS.h) ----
    // The patch path runs only when the Renderer's dirty feed is continuous and the frame carried
    // nothing membership-affecting; every other case re-runs the full static gather below.
    static_assert(std::is_trivially_copyable_v<Pending>,
                  "retained blob requires a trivially-copyable Pending");
    Renderer::SceneDiffView sceneDiff{};
    if (rendererLookup) sceneDiff = rendererLookup->GetSceneDiffView();
    const bool diffContinuous =
        sceneDiff.valid && (sceneDiff.frameStamp == m_retainedLastStamp + 1);
    m_retainedLastStamp = sceneDiff.valid ? sceneDiff.frameStamp : 0;
    // m_retainedUnresolved covers unresolved GEOMETRY only (mesh entry, VB/IB, GPU VAs). Textures
    // resolve asynchronously, a frame or more after the material went dirty, so texture settling is
    // a separate, mandatory term: otherwise the full gather latches -1 texture indices into
    // m_retainedStaticBlob and the patch path replays them forever (RT shadows solid, RTAO and
    // reflections opaque, GI albedo flat) while raster looks right. Mirrors the raster gate in
    // Renderer_Scene.cpp.
    // The surface-cache epoch is the same class of term: a page becoming usable, a page evicted or
    // the feature toggled changes the proxy of entities the dirty feed calls clean, and without it
    // the blob would replay the pre-bake alias proxy forever.
    const uint32_t scEpoch = surfaceCache ? surfaceCache->GetPublishEpoch() : 0u;
    const bool     scStable = (scEpoch == m_surfaceCacheEpoch);
    m_surfaceCacheEpoch = scEpoch;
    // The epoch only moves the BINDING (the page mapping), never the World-derived fields, so a moved
    // epoch re-binds the static rows from the PRE-binding blob (rebindStatics) instead of forcing a
    // full World gather.
    const bool rebindStatics = !s_bindFused && !scStable;
    // A frame whose only dirt is skinned adds (carrying their own dirt) keeps the static blob: the
    // static gather skips skinned entities, which are appended every frame below. Textures still
    // arriving matter only where a retained static row samples them (texUnsettledEntities).
    const bool addPatch = sceneDiff.added && !sceneDiff.added->empty() &&
                          sceneDiff.addsSkinnedOnly && sceneDiff.addsOwnDirt;
    bool texSettled = !sceneDiff.matTexUnsettled;
    if (!texSettled && sceneDiff.texUnsettledEntities)
    {
        texSettled = true;
        for (Entity e : *sceneDiff.texUnsettledEntities)
        {
            const uint32_t id = static_cast<uint32_t>(e);
            if (id < m_retainedEntityToIdx.size() && m_retainedEntityToIdx[id] != 0xFFFFFFFFu)
            { texSettled = false; break; }
        }
    }
    const bool usePatch =
        m_retainedGather && m_retainedValid && diffContinuous && (scStable || !s_bindFused) &&
        m_retainedUnresolved == 0 && texSettled &&
        ((sceneDiff.added->empty()      && sceneDiff.removed->empty() &&
          sceneDiff.dirtyOther->empty() && sceneDiff.matDirty->empty()) || addPatch);
#ifdef TRACY_ENABLE
    // Which term vetoed the patch this frame, as a bitmask: a fallback costs a full ~7K-entity
    // static gather, and the zone list alone cannot say which condition caused it.
    //   1 retainedGather(off)  2 retainedValid  4 diffContinuous  8 scStable
    //  16 unresolved  32 matTexUnsettled  64 membership(added/removed/other/mat)
    TracyPlot("DDGI.gateFail", static_cast<int64_t>(
        (!m_retainedGather        ? 1  : 0) |
        (!m_retainedValid         ? 2  : 0) |
        (!diffContinuous          ? 4  : 0) |
        (!scStable                ? 8  : 0) |
        (m_retainedUnresolved     ? 16 : 0) |
        (sceneDiff.matTexUnsettled ? 32 : 0) |
        // Pointer-guarded: without a renderer lookup sceneDiff is default-constructed (null list
        // pointers); the gate never reaches these terms (diffContinuous short-circuits), and
        // telemetry must not dereference them.
        ((!sceneDiff.added || !sceneDiff.removed || !sceneDiff.dirtyOther ||
          !sceneDiff.matDirty ||
          !sceneDiff.added->empty()      || !sceneDiff.removed->empty() ||
          !sceneDiff.dirtyOther->empty() || !sceneDiff.matDirty->empty()) ? 64 : 0)));
#endif
    // The same vetoes split per term, for the per-frame record (GatherGateStats.h;
    // Renderer_Frame.cpp notes them with the frame). Pointer-guarded like the Tracy mask.
    {
        namespace G = GatherGateStats;
        uint32_t why = 0;
        if (!m_retainedGather) why = G::kOff;
        else
        {
            if (!m_retainedValid)                     why |= G::kInvalid;
            if (!diffContinuous)                      why |= G::kContinuity;
            if (!scStable && s_bindFused)             why |= G::kSurfaceCache;
            if (m_retainedUnresolved)                 why |= G::kUnresolved;
            if (sceneDiff.matTexUnsettled)            why |= G::kTexUnsettled;
            if (!sceneDiff.added || !sceneDiff.added->empty())           why |= G::kAdded;
            if (!sceneDiff.removed || !sceneDiff.removed->empty())       why |= G::kRemoved;
            if (!sceneDiff.dirtyOther || !sceneDiff.dirtyOther->empty()) why |= G::kOther;
            if (!sceneDiff.matDirty || !sceneDiff.matDirty->empty())     why |= G::kMat;
        }
        m_lastGateFull   = !usePatch;
        m_lastGateReason = usePatch ? 0u : why;
        ++m_lastGateSerial;
    }
    m_retainedValid = false; // re-armed by whichever path completes below

    if (usePatch)
    {
        ZoneScopedN("DDGI.GatherPatch");
        PROF_SCOPE("DDGI.GatherPatch");
        // Consecutive patch frames on the same buffer catch up instead of replaying the blob (the
        // invariant is m_bufMirror's, in the header). A hit needs this buffer written by the gather
        // exactly two calls ago against the current blob generation, which also makes m_prevGatherDirty
        // the complete list of rows the blob changed since.
        // Replay: insert-from-range, not resize + memcpy - resize() value-initialises every element
        // first (Pending has NSDMIs: ~2 MB of writes at 7K rows); insert() of a trivially-copyable range
        // is one memmove.
        // Neither branch binds: a hit keeps this buffer's last binding (page-bound rows in pageRows,
        // Toon/Unlit slots), a replay is PRE-binding; BuildOrRefit binds them.
        BufMirror& bm = m_bufMirror[m_gatherWrite];
        const Pending* blob = reinterpret_cast<const Pending*>(m_retainedStaticBlob.data());
        const bool mirrorHit = !s_mirrorOff && bm.valid && bm.blobGen == m_blobGen &&
                               bm.serial + 2 == m_gatherSerial &&
                               pending.size() >= m_retainedStaticCount;
        std::optional<Diag::Prof::Scope> _catchUp;
        { static Diag::Prof::NameSlot s_n{ "DDGI.Patch.CatchUp" };
          _catchUp.emplace(s_n, Diag::Prof::Cat::Work, Diag::Prof::Level::Stats); }
        if (mirrorHit)
        {
            ++m_mirrorHits;
            pending.resize(m_retainedStaticCount);   // drop the skinned tail
            Pending* rows = pending.data();
            // The previous gather's transform patches went into the blob and
            // the OTHER buffer only.
            for (uint32_t idx : m_prevGatherDirty) rows[idx].worldMatrix = blob[idx].worldMatrix;
            // The dissolve masks this buffer's own last patch wrote.
            for (uint32_t idx : bm.maskRows) rows[idx].instanceMask = blob[idx].instanceMask;
            // No binding here: BuildOrRefit binds this buffer after the plan, starting from what its
            // last binding left (bm.pageRows is that binding's record).
        }
        else
        {
            ++m_mirrorReplays;
            pending.clear();
            if (m_retainedStaticCount)
                pending.insert(pending.end(), blob, blob + m_retainedStaticCount);
            bm.pageRows.clear();   // fresh PRE-binding rows: nothing is page-bound
        }
        if (rebindStatics) g_ddgiEpochRebinds.fetch_add(1, std::memory_order_relaxed);
        _catchUp.reset();

        // Transform-only dirt: same source the full gather reads (gt->matrix).
        std::optional<Diag::Prof::Scope> _xform;
        { static Diag::Prof::NameSlot s_n{ "DDGI.Patch.Xform" };
          _xform.emplace(s_n, Diag::Prof::Cat::Work, Diag::Prof::Level::Stats); }
        m_curGatherDirty.clear();
        for (Entity e : *sceneDiff.dirtyXform)
        {
            const uint32_t id = static_cast<uint32_t>(e);
            if (id >= m_retainedEntityToIdx.size()) continue;
            const uint32_t idx = m_retainedEntityToIdx[id];
            if (idx == 0xFFFFFFFFu || idx >= m_retainedStaticCount) continue;
            const GlobalTransform* gt = pGT->Get(e);
            if (!gt) continue;
            pending[idx].worldMatrix = gt->matrix;
            // Only the matrix goes back into the blob: pending's prefix may be POST-binding while the
            // blob must stay PRE-binding, or an epoch-move re-bind of an entity whose page was evicted
            // would keep the page-neutralised fields (identity rough/metal range, page rect as
            // uvTilingOffset). Everything else in the blob is what the full gather wrote.
            if (s_blobFullCopy)   // -ddgiblobfullcopy (reverse arm): copy the whole record
                std::memcpy(m_retainedStaticBlob.data() + size_t(idx) * sizeof(Pending),
                            &pending[idx], sizeof(Pending));
            else
                reinterpret_cast<Pending*>(m_retainedStaticBlob.data())[idx].worldMatrix = gt->matrix;
            m_curGatherDirty.push_back(idx);
        }

        // Dissolving entities animate mask bits 0/1/3 across the 0.5 threshold: re-derive their
        // instanceMask every frame (a small live set) on the working copy only; dissolve start/stop
        // changes the render key -> full rebuild. The rows are remembered (maskRows) so this buffer's
        // next mirror hit restores the blob's mask before re-deriving.
        _xform.reset();
        bm.maskRows.clear();
        auto deriveDissolveMasks = [&](Pending* rows, std::vector<uint32_t>* touched)
        {
            if (!(anyDissolve && pDissolve)) return;
            for (Entity e : pDissolve->Entities())
            {
                const uint32_t id = static_cast<uint32_t>(e);
                if (id >= m_retainedEntityToIdx.size()) continue;
                const uint32_t idx = m_retainedEntityToIdx[id];
                if (idx == 0xFFFFFFFFu || idx >= m_retainedStaticCount) continue;
                rows[idx].instanceMask = instanceMask(e);
                if (touched) touched->push_back(idx);
            }
        };
        deriveDissolveMasks(pending.data(), &bm.maskRows);

        // ---- -ddgimirrorverify, gather half ----
        // The replay's rows are the blob plus this frame's dissolve masks; the binding is BuildOrRefit's
        // and is verified there. A hit is right when every static row has the blob's matrix and the
        // replay's mask.
        if (mirrorHit && s_mirrorVerify)
        {
            static std::vector<uint8_t> s_maskRef;
            s_maskRef.resize(m_retainedStaticCount);
            for (uint32_t i = 0; i < m_retainedStaticCount; ++i) s_maskRef[i] = blob[i].instanceMask;
            if (anyDissolve && pDissolve)
                for (Entity e : pDissolve->Entities())
                {
                    const uint32_t id = static_cast<uint32_t>(e);
                    if (id >= m_retainedEntityToIdx.size()) continue;
                    const uint32_t idx = m_retainedEntityToIdx[id];
                    if (idx == 0xFFFFFFFFu || idx >= m_retainedStaticCount) continue;
                    s_maskRef[idx] = instanceMask(e);
                }
            uint64_t bad = 0;
            for (uint32_t i = 0; i < m_retainedStaticCount; ++i)
                if (std::memcmp(&pending[i].worldMatrix, &blob[i].worldMatrix,
                                sizeof(pending[i].worldMatrix)) != 0 ||
                    pending[i].instanceMask != s_maskRef[i]) ++bad;
            ++m_mirrorVerified;
            m_mirrorMismatchRows += bad;
        }

        bm.valid   = true;
        bm.blobGen = m_blobGen;
        bm.serial  = m_gatherSerial;
        std::swap(m_prevGatherDirty, m_curGatherDirty);
        m_retainedValid      = true;
        m_pendingMirrorsBlob = true;
    }
    else
    {
    m_retainedUnresolved = 0;
    pending.clear();   // only the full gather clears (see m_bufMirror)

    // ---- MeshLibRef path (scene-loaded meshes, the bulk of the geometry) ----
    if (pLibRef && meshLib)
    {
        ZoneScopedN("DDGI.Gather");
        PROF_SCOPE("DDGI.GatherStatic");
        const auto& ents = pLibRef->Entities();
        const auto& data = pLibRef->Data();
        for (size_t i = 0; i < ents.size(); ++i)
        {
            Entity e = ents[i];
            if (!world.IsAlive(e)) continue;
            if (pSkin && pSkin->Get(e)) continue; // skinned -> deformable path below
            if (structurallyHidden(e)) continue;

            const GlobalTransform* gt = pGT->Get(e);
            if (!gt) continue;

            const MeshLibRef& ref = data[i];
            if (!ref.IsValid()) continue;

            const auto* entry = meshLib->GetEntry(ref.libHandle, ref.meshId);
            if (!entry || entry->vertexCount == 0 || entry->indexCount == 0)
            {
                // Library still loading = transient: keep re-gathering until it resolves (a
                // permanently bad ref costs the full gather every frame).
                if (!entry) m_retainedUnresolved++;
                continue;
            }
            const RHI::GPUBuffer* vb = meshLib->GetVertexBuffer(ref.libHandle);
            const RHI::GPUBuffer* ib = meshLib->GetIndexBuffer(ref.libHandle);
            if (!vb || !ib) { m_retainedUnresolved++; continue; }
            const uint32_t stride = meshLib->GetVertexStride(ref.libHandle);

            uint64_t vbVA = BufferGPUVA(gfx, *vb);
            uint64_t ibVA = BufferGPUVA(gfx, *ib);
            if (!vbVA || !ibVA) { m_retainedUnresolved++; continue; }

            CacheKey k{};
            k.sourceKind = 0;
            k.libOrSlot  = (uint64_t(ref.libHandle.Generation()) << 32) | ref.libHandle.Index();
            k.meshId     = ref.meshId;

            Pending p{};
            p.key            = k;
            p.worldMatrix    = gt->matrix;
            // DXR reads vertex N at `positionVA + N * stride`, and MeshLibrary IBs hold library-absolute
            // indices (0..libTotal-1). So positionVA must be the library VB base, not offset by
            // entry->vertexStart (that double-counts the offset -> out-of-bounds read -> corrupt BLAS ->
            // DEVICE_HUNG TDR), and vertexCount must cover the largest index: vertexStart + vertexCount.
            p.positionVA     = vbVA;
            p.positionStride = stride;
            p.interleavedTangent = meshLib->GetHasTangent(ref.libHandle);
            p.vertexCount    = entry->vertexStart + entry->vertexCount;
            p.indexVA        = ibVA + uint64_t(entry->indexStart) * kEngineIndexBytes;
            p.indexCount     = entry->indexCount;
            p.indexFormat    = kEngineIndexFormat;
            fillMaterialProxy(e, p);
            // Bindless VB/IB slots: the hit shaders re-fetch positions through them for the face normal.
            // Invalid (0xFFFFFFFFu, which closest-hit detects) until the MeshManager registers the library
            // on its first RegisterMeshLibMesh; BuildRenderScene runs before Gather, so in normal play they
            // are live here.
            p.vbBindlessIdx = RHI::kInvalidBufferIndex;
            p.ibBindlessIdx = RHI::kInvalidBufferIndex;
            if (meshMgr)
                meshMgr->GetLibBindlessSlots(ref.libHandle.Index(),
                                             p.vbBindlessIdx, p.ibBindlessIdx);
#if DDGI_GT_DIAGNOSTICS
            // Diagnostic: instances whose bindless VB/IB never resolved take
            // DDGIRayTrace's cheap fallback hit path (no attribute fetch,
            // -rayDir normal). Counted per gather for the SceneAS status line.
            if (p.vbBindlessIdx == RHI::kInvalidBufferIndex ||
                p.ibBindlessIdx == RHI::kInvalidBufferIndex)
                m_bindlessInvalidCount++;
#endif
            p.vertexStart = entry->vertexStart;
            p.indexStart  = entry->indexStart;
            p.instanceMask = instanceMask(e);
            p.entity       = e;
            pending.push_back(p);
        }
    }

    // ---- MeshHandle path (procedural primitives: Cube/Sphere/Cone/Plane/Torus) ----
    // MeshHandle holds a slot in MeshManager's primitive pool (bounded by PrimitiveMeshType::Count),
    // uploaded by MeshManager::InitPrimitives at startup and read via GetPrimitive(). Primitives use
    // uint16 indices, so the index format travels in Pending, unlike the MeshLibrary path's R32_UINT.
    if (pMesh && meshMgr)
    {
        ZoneScopedN("DDGI.Gather");
        PROF_SCOPE("DDGI.GatherStatic");
        constexpr RHI::Format kPrimIndexFormat = RHI::Format::R16_UINT;
        constexpr uint32_t    kPrimIndexBytes  = 2;
        constexpr uint32_t    kPrimPosStride   = sizeof(float) * 3;

        const auto& ents = pMesh->Entities();
        const auto& data = pMesh->Data();
        const uint32_t maxPrim = static_cast<uint32_t>(PrimitiveMeshType::Count);
        for (size_t i = 0; i < ents.size(); ++i)
        {
            Entity e = ents[i];
            if (!world.IsAlive(e)) continue;
            if (pSkin && pSkin->Get(e)) continue; // skinned -> deformable path below
            if (structurallyHidden(e)) continue;

            const ::MeshHandle& mh = data[i];
            if (!mh.IsValid() || mh.gpuMeshID >= maxPrim) continue;

            const GlobalTransform* gt = pGT->Get(e);
            if (!gt) continue;

            const auto& prim = meshMgr->GetPrimitive(static_cast<int>(mh.gpuMeshID));
            if (!prim.posBuffer.IsValid() || !prim.indexBuffer.IsValid()) continue;
            if (prim.indexCount < 3) continue;

            uint64_t posVA = BufferGPUVA(gfx, prim.posBuffer);
            uint64_t idxVA = BufferGPUVA(gfx, prim.indexBuffer);
            if (!posVA || !idxVA) continue;

            // Vertex count from the position buffer's byte size: primitives are raw float3 streams.
            const uint32_t vertexCount = static_cast<uint32_t>(prim.posBuffer.desc.size / kPrimPosStride);

            CacheKey k{};
            k.sourceKind = 1;
            k.libOrSlot  = mh.gpuMeshID; // primitive index identifies the BLAS uniquely
            k.meshId     = 0;

            Pending p{};
            p.key            = k;
            p.worldMatrix    = gt->matrix;
            p.positionVA     = posVA;
            p.positionStride = kPrimPosStride;
            p.vertexCount    = vertexCount;
            p.indexVA        = idxVA;
            p.indexCount     = prim.indexCount;
            p.indexFormat    = kPrimIndexFormat;
            fillMaterialProxy(e, p);
            // De-interleaved stream slots (UploadMesh registers pos/normal/uv/ib in the bindless heap and
            // keeps the slots on GPUMesh), so primitive hits fetch real normals and UVs; without them the
            // trace falls back to an anti-ray normal and an RTR hit shades near-black.
            p.vbBindlessIdx = RHI::kInvalidBufferIndex;
            p.ibBindlessIdx = RHI::kInvalidBufferIndex;
            if (prim.posBindlessIdx    != RHI::kInvalidBufferIndex &&
                prim.normalBindlessIdx != RHI::kInvalidBufferIndex &&
                prim.ibBindlessIdx     != RHI::kInvalidBufferIndex)
            {
                p.vbBindlessIdx = prim.posBindlessIdx;   // position stream (stride 12)
                p.ibBindlessIdx = prim.ibBindlessIdx;
                p.normalBufIdx  = prim.normalBindlessIdx;
                p.uvBufIdx      = prim.uvBindlessIdx;    // may stay invalid (uv -> 0,0)
                p.indexBytes    = kPrimIndexBytes;       // R16_UINT
            }
            p.vertexStart   = 0;
            p.indexStart    = 0;
            p.instanceMask  = instanceMask(e);
            p.entity        = e;
            pending.push_back(p);
        }
    }

    // ---- Snapshot the freshly gathered static prefix for the patch path ----
    m_retainedStaticCount = static_cast<uint32_t>(pending.size());
    m_retainedStaticBlob.resize(size_t(m_retainedStaticCount) * sizeof(Pending));
    if (m_retainedStaticCount)
        std::memcpy(m_retainedStaticBlob.data(), pending.data(),
                    m_retainedStaticBlob.size());
    std::fill(m_retainedEntityToIdx.begin(), m_retainedEntityToIdx.end(), 0xFFFFFFFFu);
    for (uint32_t i = 0; i < m_retainedStaticCount; ++i)
    {
        const uint32_t id = static_cast<uint32_t>(pending[i].entity);
        if (id >= m_retainedEntityToIdx.size())
            m_retainedEntityToIdx.resize(size_t(id) + 1, 0xFFFFFFFFu);
        m_retainedEntityToIdx[id] = i;
    }
    m_retainedValid = m_retainedGather;
    // Not bound here: BuildOrRefit binds (fused mode bound inside fillMaterialProxy already), so
    // outside fused mode the blob is PRE-binding. Record the Toon/Unlit rows; this buffer has no
    // page-bound row yet.
    m_bufMirror[m_gatherWrite].pageRows.clear();
    m_wantsSlotRows.clear();
    for (uint32_t i = 0; i < m_retainedStaticCount; ++i)
        if (pending[i].hasMaterial && pending[i].wantsMatSlot) m_wantsSlotRows.push_back(i);
    // The blob was just copied from pending, so the mirror holds by construction
    // (m_pendingMirrorsBlob).
    m_pendingMirrorsBlob = true;
    // A new blob generation: this buffer is its first mirror (its masks are in the blob itself);
    // the other buffer mirrors nothing.
    ++m_blobGen;
    {
        BufMirror& bm = m_bufMirror[m_gatherWrite];
        bm.valid   = true;
        bm.blobGen = m_blobGen;
        bm.serial  = m_gatherSerial;
        bm.maskRows.clear();
    }
    m_prevGatherDirty.clear();
    } // end full-gather path

    // ---- -ddgiblobverify ----
    // The blob is the PRE-binding half: no record in it may carry a field only BindProxy writes (a
    // set surfaceCache or a non-zero matSlot). One that does is a POST-binding record back in the
    // blob; an epoch-move re-bind would start from it and an evicted page would leave its
    // neutralised range and page rect behind. matSlot is the sensitive probe (any Toon/Unlit row
    // that emitted this frame has one), surfaceCache the harmful one.
    if (s_blobVerify && !s_bindFused)
    {
        static uint64_t s_checks = 0, s_violations = 0, s_worstFrame = 0;
        const Pending* blob = reinterpret_cast<const Pending*>(m_retainedStaticBlob.data());
        uint64_t bad = 0;
        for (uint32_t i = 0; i < m_retainedStaticCount; ++i)
            if (blob[i].surfaceCache || blob[i].matSlot != 0u) ++bad;
        ++s_checks;
        s_violations += bad;
        if (bad > s_worstFrame) s_worstFrame = bad;
        if (s_checks % 60 == 0)
            LOG_INFO("[DDGIBLOB] checks=%llu records=%u violations=%llu worstFrame=%llu fullcopy=%d",
                     static_cast<unsigned long long>(s_checks), m_retainedStaticCount,
                     static_cast<unsigned long long>(s_violations),
                     static_cast<unsigned long long>(s_worstFrame), s_blobFullCopy ? 1 : 0);
    }
    // ---- Per-buffer mirror tally ----
    // With -ddgimirrorverify: every 60 gathers, log mirror hits, replays, verify mismatches and
    // the binding counts.
    if (s_mirrorVerify && m_gatherSerial % 60 == 0)
        LOG_INFO("[DDGIMIRROR] gathers=%llu hits=%llu replays=%llu verified=%llu "
                 "mismatchRows=%llu rows=%u off=%d dirtyIn=%zu dirtyRows=%zu "
                 "wantsRows=%zu pageRows=%zu pagesLive=%d bindVerified=%llu "
                 "bindMismatchRows=%llu bindRows=%llu",
                 static_cast<unsigned long long>(m_gatherSerial),
                 static_cast<unsigned long long>(m_mirrorHits),
                 static_cast<unsigned long long>(m_mirrorReplays),
                 static_cast<unsigned long long>(m_mirrorVerified),
                 static_cast<unsigned long long>(m_mirrorMismatchRows),
                 m_retainedStaticCount, s_mirrorOff ? 1 : 0,
                 sceneDiff.dirtyXform ? sceneDiff.dirtyXform->size() : size_t(0),
                 m_prevGatherDirty.size(), m_wantsSlotRows.size(),
                 m_bufMirror[m_gatherWrite].pageRows.size(),
                 (surfaceCache && surfaceCache->IsEnabled() && surfaceCache->AtlasReady()) ? 1 : 0,
                 static_cast<unsigned long long>(m_bindVerified.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(m_bindMismatchRows.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(m_bindRowsBound.load(std::memory_order_relaxed)));

    // ---- Skinned-mesh path (per-frame deformable BLAS; never the DDGI bit) ----
    // Geometry = this frame's SkinnedVertexRing slice (object-space post-skin positions, SoA float3
    // stride 12, morphs included) + the mesh's static uint32 IB. Its indices are 0-based, unlike
    // MeshLibrary's library-absolute ones, so positionVA IS offset to the slice base. Inclusion is
    // gated on the actual skin-job list, not on SkinningOutputComponent: offsets can look valid
    // while the dispatch was dropped (anim-culled, kMaxSkinJobs overflow, first frame after spawn),
    // and a BLAS over an unwritten ring region hangs DXR traversal (TDR). Anim-culled characters
    // therefore drop out of RT, as they do in raster.
    uint32_t skinnedCount = 0;
    bool deformHashed = false;   // every skinned row carries its deformation hash
    if (includeSkinned && skin && skin->IsInitialised() && pSkin)
    {
        ZoneScopedN("DDGI.Gather");
        PROF_SCOPE("DDGI.GatherSkinned");
        auto* pSkinOut = world.GetPool<SkinningOutputComponent>();
        SkinnedVertexRing& ring = skin->GetVertexRing();
        const uint64_t ringPosVA = BufferGPUVA(gfx, ring.UploadPosBuffer());
        const uint32_t ringPosIdx = ring.UploadPosBindlessIdx();
        const uint32_t ringNrmIdx = ring.UploadNrmBindlessIdx();

        // Entities whose skin dispatch is recorded this frame. Must replicate SkinningPass::Execute's
        // cap exactly (the first kMaxSkinJobs jobs with vertexCount > 0, in list order): a job past
        // the cap is dropped there and its ring slice stays unwritten.
        std::unordered_set<Entity> skinnedThisFrame;
        // The path tracer's deformation hash per entity (SetDeformFingerprintWanted): what the skin
        // dispatch consumes - the skeleton's skinning matrices and the job's morph weights - so an
        // unchanged pose hashes the same whichever ring slot it lands in.
        std::unordered_map<Entity, uint64_t> deformByEntity;
        const bool hashDeform = m_deformWanted.load(std::memory_order_relaxed);
        bool allDeformHashed = hashDeform;
        {
            // Write side: Gather (game half) runs in the frame BuildSkinJobs filled, before the
            // publish.
            const auto& jobs = skin->SkinJobsWrite();
            skinnedThisFrame.reserve(std::min(jobs.size(), (size_t)SkinningPass::kMaxSkinJobs));
            const PoseRingBuffer& poseRing = skin->GetPoseRingBuffer();
            const std::vector<float>& morphPool = skin->MorphPoolWrite();
            // A skeleton's palette is hashed once, whatever number of sections skin from it.
            std::unordered_map<uint32_t, uint64_t> paletteByOffset;
            auto* pSkelComp = world.GetPool<SkeletonComponent>();
            auto* pSkelRef = world.GetPool<SkeletonRef>();
            uint32_t dispatched = 0;
            for (const SkinDispatchDesc& j : jobs)
            {
                if (j.vertexCount == 0) continue;
                if (dispatched >= SkinningPass::kMaxSkinJobs) break;
                skinnedThisFrame.insert(j.entity);
                ++dispatched;
                if (!hashDeform) continue;
                Entity skelEnt = j.entity;
                if (const SkeletonRef* ref = pSkelRef ? pSkelRef->Get(j.entity) : nullptr)
                    if (ref->entity != NullEntity) skelEnt = ref->entity;
                const SkeletonComponent* skel = pSkelComp ? pSkelComp->Get(skelEnt) : nullptr;
                const DirectX::XMFLOAT4X4* palette = j.poseByteOffset != ~0u ? poseRing.ReadMapped(j.poseByteOffset) : nullptr;
                if (!skel || !palette || skel->boneCount == 0) { allDeformHashed = false; continue; }
                auto [pal, freshPalette] = paletteByOffset.try_emplace(j.poseByteOffset, 0);
                if (freshPalette)
                    pal->second = Sys::WordHash64(palette, size_t(skel->boneCount) * sizeof(DirectX::XMFLOAT4X4));
                uint64_t h = pal->second;
                if (j.morphCount && size_t(j.morphWeightOffset) + j.morphCount <= morphPool.size())
                    h = Sys::WordHashCombine(h, Sys::WordHash64(morphPool.data() + j.morphWeightOffset,
                                                                size_t(j.morphCount) * sizeof(float)));
                h = Sys::WordHashCombine(h, j.vertexCount);
                auto [it, fresh] = deformByEntity.try_emplace(j.entity, h);
                if (!fresh) it->second = Sys::WordHashCombine(it->second, h);   // a second section
            }
        }

        if (pSkinOut && ringPosVA != 0 &&
            ringPosIdx != RHI::kInvalidBufferIndex &&
            ringNrmIdx != RHI::kInvalidBufferIndex &&
            !skinnedThisFrame.empty())
        {
            const auto& ents = pSkin->Entities();
            const auto& data = pSkin->Data();
            for (size_t i = 0; i < ents.size(); ++i)
            {
                Entity e = ents[i];
                if (!world.IsAlive(e)) continue;
                if (skinnedThisFrame.find(e) == skinnedThisFrame.end()) continue;

                const MeshSkinnedComponent& sk = data[i];
                if (sk.vertexCount == 0 || sk.indexCount < 3) continue;
                if (!sk.indexBuffer.IsValid()) continue;

                const SkinningOutputComponent* so = pSkinOut->Get(e);
                if (!so || so->poseByteOffset == ~0u) continue;

                const GlobalTransform* gt = pGT->Get(e);
                if (!gt) continue;

                // Skinned instances never carry the DDGI bit (bit 2). Bit 1 follows the statics'
                // Visibility / Material gates (characters cast RT sun shadows); bit 3 stays so a
                // character shows in a window.
                const uint8_t skinnedMask = instanceMask(e) & 0x0Bu;
                // Skip only when nobody can see this section - neither a reflection/AO ray (bit 0) nor
                // an RT sun-shadow ray (bit 1): a main-pass-hidden shadow caster keeps casting, as
                // statics do.
                if (skinnedMask == 0u) continue;
                if (structurallyHidden(e)) continue;

                const uint64_t ibVA = BufferGPUVA(gfx, sk.indexBuffer);
                if (!ibVA) continue;

                Pending p{};
                p.skinned        = true;
                p.entity         = e;
                // Skin.cs outputs MESH-LOCAL positions (AfterimageSystem relies on it too), so the
                // world matrix goes in the TLAS instance transform, as for static meshes.
                p.worldMatrix    = gt->matrix;
                p.positionVA     = ringPosVA + so->outPosByteOffset;
                p.positionStride = 12;
                p.vertexCount    = sk.vertexCount;
                p.indexVA        = ibVA;
                p.indexCount     = sk.indexCount;
                p.indexFormat    = kEngineIndexFormat;
                fillMaterialProxy(e, p);
                // Bound in BuildOrRefit like the static rows. Hit shading fetches in de-interleaved mode
                // over the ring streams; the *Start fields rebase into the shared arena (see
                // DDGIInstanceData).
                p.vbBindlessIdx  = ringPosIdx;
                p.normalBufIdx   = ringNrmIdx;
                p.ibBindlessIdx  = sk.baseMeshDesc.indexBufferIndex;
                p.uvBufIdx       = sk.baseMeshDesc.uv0.bufferIndex;
                p.indexBytes     = 4;
                p.vertexStart    = so->outPosByteOffset / 12u;
                p.nrmStart       = so->outNrmByteOffset / 12u;
                p.uvStart        = sk.baseMeshDesc.uv0.byteOffset / 8u;
                p.indexStart     = sk.baseMeshDesc.indexByteOffset / 4u;
                p.instanceMask   = skinnedMask;
                if (hashDeform)
                {
                    const auto deform = deformByEntity.find(e);
                    if (deform != deformByEntity.end()) p.deformHash = deform->second;
                    else allDeformHashed = false;
                }
                pending.push_back(p);
                ++skinnedCount;
            }
        }
        deformHashed = allDeformHashed;
    }

    // Two one-time reports: the skinned half has no other observable signal, and "no skinned rows,
    // the scene has none" reads like "the skinned gather is broken" in packs that do not force RT on
    // (tests/profiles/rt.json). The first gather runs before anything spawned, hence the second
    // report when the skinned path first produces rows.
    static bool s_reportedFirst = false;
    if (!s_reportedFirst)
    {
        s_reportedFirst = true;
        LOG_INFO("[SPLITAUDIT][DDGIGATHER] first: pending=%zu skinned=%u "
                 "includeSkinned=%d usePatch=%d", pending.size(), skinnedCount,
                 (int)includeSkinned, (int)usePatch);
    }
    static bool s_reportedSkinned = false;
    if (!s_reportedSkinned && skinnedCount > 0)
    {
        s_reportedSkinned = true;
        LOG_INFO("[SPLITAUDIT][DDGIGATHER] skinned path live: pending=%zu skinned=%u",
                 pending.size(), skinnedCount);
    }

    // Run after retained-blob replay: no raster visibility/epoch dependency.
    // The write buffer drops its share of the cached table first, so the only
    // other holder can be the read buffer the render half is consuming.
    GatherWrite().pathMaterials.reset();
    GatherWrite().pathMaterialHash = 0;
    const auto& primaryCmd = Sys::CommandLine::Get();
    // Both path-trace render paths (PathTrace, PathTraceRealtime) shade the vertices after the
    // primary hit from this table, and GBuffer visibility cannot tell which materials a reflected
    // ray reaches, so the table holds a record for every gathered row.
    const bool pathWanted = rendererLookup &&
        Renderer::IsPathTrace(rendererLookup->GetRenderPath());
    if (!pathWanted)
    {
        m_pathMatValid = false;
        m_pathMatCache.reset();
    }
    else
    {
        // pathMaterialIndex is the row in gather order (BuildOrRefit's erase_if moves rows, so it
        // travels in the row). Rewritten on every PathTrace frame, mirror hit or not, so a mirrored
        // prefix holds exactly what a blob replay plus this loop produces and the mirror stays valid.
        uint64_t sig = Sys::WordHash64(nullptr, 0);
        for (uint32_t i = 0; i < pending.size(); ++i)
        {
            pending[i].pathMaterialIndex = i;
            sig = Sys::WordHashCombine(sig, static_cast<uint32_t>(pending[i].entity));
        }
        const uint64_t texRevision = rendererLookup->GetTextureContentRevision();
        const uint32_t runtimeEpoch = rendererLookup->MaterialRuntimeEpoch();
        const uint32_t shaderEpoch = rendererLookup->GetMaterialShaderReloadEpoch();
        const bool stampContinuous = sceneDiff.valid && sceneDiff.frameStamp == m_pathMatLastStamp + 1;
        m_pathMatLastStamp = sceneDiff.valid ? sceneDiff.frameStamp : 0;
        static const bool s_pathMatAlways = primaryCmd.Has("-pt_material_rebuild_every_frame");
        const bool full = s_pathMatAlways || !m_pathMatValid || !m_pathMatCache ||
            sig != m_pathMatEntitySig || m_pathMatCache->size() != pending.size() ||
            !stampContinuous || sceneDiff.matTexUnsettled || !sceneDiff.matDirty ||
            texRevision != m_pathMatTexRevision || runtimeEpoch != m_pathMatRuntimeEpoch ||
            shaderEpoch != m_pathMatShaderEpoch;
        // One record per entity: every row of an entity reads the same
        // MaterialComponent (fillMaterialProxy), so its rows share the bytes.
        auto buildRecord = [&](Entity e, Resource::MaterialGPUData* table, uint32_t row) {
            const auto* source = pMat ? pMat->Get(e) : nullptr;
            // The packer refreshes linear caches; keep async raster emission's
            // MaterialComponent untouched by packing a private copy.
            MaterialComponent local;
            if (source) local = *source;
            const auto runtime = rendererLookup->ResolveMaterialRuntime(e);
            table[row] = {};
            const auto* reflection = source ? rendererLookup->GetCustomShaderReflection(
                rendererLookup->LookupMaterialCustomPSID(local)) : nullptr;
            Renderer::WriteMaterialSlot(table, row, source ? &local : nullptr, runtime, reflection);
        };
        uint32_t rewritten = 0;
        if (full)
        {
            std::vector<Resource::MaterialGPUData> rebuilt(pending.size());
            m_pathMatRowEntity.resize(pending.size());
            std::fill(m_pathMatEntityRow.begin(), m_pathMatEntityRow.end(), ~0u);
            for (uint32_t i = 0; i < pending.size(); ++i)
            {
                const uint32_t id = static_cast<uint32_t>(pending[i].entity);
                m_pathMatRowEntity[i] = id;
                if (id >= m_pathMatEntityRow.size()) m_pathMatEntityRow.resize(size_t(id) + 1, ~0u);
                if (m_pathMatEntityRow[id] != ~0u) { rebuilt[i] = rebuilt[m_pathMatEntityRow[id]]; continue; }
                m_pathMatEntityRow[id] = i;
                buildRecord(pending[i].entity, rebuilt.data(), i);
                ++rewritten;
            }
            const bool same = m_pathMatCache && m_pathMatCache->size() == rebuilt.size() &&
                std::memcmp(m_pathMatCache->data(), rebuilt.data(),
                            rebuilt.size() * sizeof(Resource::MaterialGPUData)) == 0;
            if (!same)
            {
                // A fresh table: the old one may still be read by the render half.
                m_pathMatCache = std::make_shared<std::vector<Resource::MaterialGPUData>>(std::move(rebuilt));
                ++m_pathMatRevision;
            }
            m_pathMatEntitySig = sig;
            m_pathMatTexRevision = texRevision;
            m_pathMatRuntimeEpoch = runtimeEpoch;
            m_pathMatShaderEpoch = shaderEpoch;
            m_pathMatValid = true;
        }
        else if (!sceneDiff.matDirty->empty())
        {
            bool copied = false;
            Resource::MaterialGPUData scratch[1];
            for (Entity e : *sceneDiff.matDirty)
            {
                const uint32_t id = static_cast<uint32_t>(e);
                if (id >= m_pathMatEntityRow.size() || m_pathMatEntityRow[id] == ~0u) continue;
                const uint32_t first = m_pathMatEntityRow[id];
                buildRecord(e, scratch, 0);
                ++rewritten;
                if (std::memcmp(&(*m_pathMatCache)[first], &scratch[0], sizeof(scratch[0])) == 0) continue;
                // Copy-on-write once per frame: the read buffer may hold the
                // previous table while the render half uploads it.
                if (!copied)
                {
                    m_pathMatCache = std::make_shared<std::vector<Resource::MaterialGPUData>>(*m_pathMatCache);
                    copied = true;
                }
                auto& table = *m_pathMatCache;
                for (uint32_t row = first; row < table.size(); ++row)
                    if (m_pathMatRowEntity[row] == id) table[row] = scratch[0];
            }
            if (copied) ++m_pathMatRevision;
        }
        GatherWrite().pathMaterials = m_pathMatCache;
        // Never 0 while a table is published: 0 means "no table" downstream.
        GatherWrite().pathMaterialHash = m_pathMatRevision | (uint64_t{1} << 63);
#if WITH_HARNESS
        static uint64_t s_publicationFrame = 0;
        if (primaryCmd.Has("-pt_raycounts") && ++s_publicationFrame % 60 == 0)
        {
            uint32_t rasterMissing = 0;
            for (const auto& p : pending)
                rasterMissing += rendererLookup->GetEntityMaterialSlot(p.entity) >= rendererLookup->GetMaterialSlotCapacity();
            const size_t records = m_pathMatCache ? m_pathMatCache->size() : size_t(0);
            LOG_INFO("[PTMATERIALPUBLISH] pending=%zu records=%zu rasterMissing=%u bytes=%zu",
                pending.size(), records, rasterMissing, records * sizeof(Resource::MaterialGPUData));
        }
        if (primaryCmd.Has("-pt_material_log"))
            LOG_INFO("[PTMATERIALCACHE] full=%u rewritten=%u revision=%llu rows=%zu dirty=%zu",
                full ? 1u : 0u, rewritten, static_cast<unsigned long long>(m_pathMatRevision),
                pending.size(), sceneDiff.matDirty ? sceneDiff.matDirty->size() : size_t(0));
#endif
    }

    GatherWrite().skinnedCount    = skinnedCount;
    GatherWrite().deformHashed    = deformHashed;
    GatherWrite().usePatch        = usePatch;
    GatherWrite().anyDissolve     = anyDissolve;
    GatherWrite().dirtyXformEmpty = !sceneDiff.dirtyXform || sceneDiff.dirtyXform->empty();
    GatherWrite().valid           = true;

#if WITH_HARNESS
    // -ddgiproxyverify's census is ReportProxySlotCensus, run by BuildOrRefit next to the binding it
    // reads.
#endif

    // Hand this frame's gather to the render half and move to the other buffer.
    PublishGather();
}

// ==== BUILD half: runs on the render side ====
void DDGISceneAS::ReportProxySlotCensus(const std::vector<Pending>& pending, bool usePatch)
{
#if WITH_HARNESS
    // ---- -ddgiproxyverify[=N]: the RT/DDGI material-proxy slot oracle ----
    // Every N gathers (default 60). Reads the rows about to be published, not fillMaterialProxy: on
    // a static scene the retained gather replays the blob and fillMaterialProxy does not run, so an
    // instrument there would go quiet exactly while the TLAS shades from a latched proxy. `pending`
    // is what BuildOrRefit packs into rtFlags (`p.matSlot << 16`).
    // Only Toon and Unlit hits need the raster's MaterialGPUData record (toon knobs;
    // exposureCompensation), reached through Renderer::GetEntityMaterialSlot, which is epoch-gated
    // on this frame's emit nest (under -brslane the nest has not run when Gather does). `knobs=` is
    // that bit per entity, `missing=` / `missingTotal=` the tally. matSlot comes from BindProxy and
    // the slot ResolveBatchPlan or the emit nest stamped, so this measures rather than restates.
    // Standard-model instances want no slot and are not counted; the fixture of
    // tests/scenarios/ddgi_proxy_slot carries one as the control.
    {
        static const int s_proxyEvery = []{
            const auto& cl = Sys::CommandLine::Get();
            if (!cl.Has("-ddgiproxyverify")) return 0;
            const int n = cl.GetInt("-ddgiproxyverify", 60);
            return n > 0 ? n : 60;
        }();
        if (s_proxyEvery > 0)
        {
            static uint64_t s_proxyGathers   = 0;
            static uint32_t s_proxySamples   = 0;
            static uint32_t s_proxyMissing   = 0;
            ++s_proxyGathers;
            if (s_proxyGathers % static_cast<uint64_t>(s_proxyEvery) == 0)
            {
                constexpr uint32_t kMaxListed = 8;   // a fixture has 2; a level has thousands
                uint32_t wants = 0, knobs = 0, listed = 0;
                for (const Pending& p : pending)
                {
                    const bool unlit = static_cast<int>(p.emissive.w + 0.5f) ==
                                       static_cast<int>(ShadingModel::Unlit);
                    if (!p.npr && !unlit) continue;
                    ++wants;
                    if (p.matSlot != 0) ++knobs;
                    if (listed++ < kMaxListed)
                        // slot is the PACKED value (0 = none), so it is printed
                        // back as the real slot index or -1 - the encoding the
                        // hit shader decodes, not a second convention.
                        LOG_INFO("[DDGI_PROXY_SLOT] gather=%llu entity=%u model=%s "
                                 "slot=%d knobs=%u",
                                 static_cast<unsigned long long>(s_proxyGathers),
                                 static_cast<uint32_t>(p.entity),
                                 p.npr ? "Toon" : "Unlit",
                                 static_cast<int>(p.matSlot) - 1,
                                 p.matSlot != 0 ? 1u : 0u);
                }
                s_proxyMissing += (wants - knobs);
                ++s_proxySamples;
                LOG_INFO("[DDGI_PROXY_SLOT] census gather=%llu instances=%zu wants=%u "
                         "knobs=%u missing=%u samples=%u missingTotal=%u patch=%u",
                         static_cast<unsigned long long>(s_proxyGathers),
                         pending.size(), wants, knobs, wants - knobs,
                         s_proxySamples, s_proxyMissing, usePatch ? 1u : 0u);
            }
        }
    }
#else
    (void)pending; (void)usePatch;
#endif
}

// BuildOrRefit's binding pass over the published buffer @p bufIdx. Runs on the thread that
// records the AS, after this frame's PlanEmit and emit nest, so the material slots and the
// surface-cache mappings it reads are this frame's wherever the plan ran. Only four kinds of row
// can bind differently from what the buffer holds (see m_bufMirror): a row a page bound last
// time (restarted from the PRE-binding record, keeping the fields Gather wrote after the blob),
// a Toon/Unlit static row, a static row whose entity MapEntity stamped this frame, and the
// skinned tail (fresh every frame). Every other row already is the blob's plus matSlot 0 - what
// a full bind gives; -ddgimirrorverify compares against exactly that.
void DDGISceneAS::BindGatheredRows(std::vector<Pending>& rows, uint32_t bufIdx)
{
    ZoneScopedN("DDGI.BindLate");
    BufMirror& bm = m_bufMirror[bufIdx];
    const Pending* blob = reinterpret_cast<const Pending*>(m_retainedStaticBlob.data());
    const uint32_t nStatic = std::min<uint32_t>(m_retainedStaticCount,
                                                static_cast<uint32_t>(rows.size()));
    uint64_t bound = 0;
    for (uint32_t idx : bm.pageRows)
    {
        if (idx >= nStatic) continue;
        Pending& r = rows[idx];
        // Gather wrote these after the blob: the dissolve mask (patch frames)
        // and the path-material index (PathTrace frames). The matrix is the
        // blob's already - Gather patches both.
        const uint8_t  mask = r.instanceMask;
        const uint32_t pmi  = r.pathMaterialIndex;
        std::memcpy(&r, &blob[idx], sizeof(Pending));
        r.instanceMask      = mask;
        r.pathMaterialIndex = pmi;
    }
    bm.pageRows.clear();
    for (uint32_t idx : m_wantsSlotRows)
        if (idx < nStatic) { BindProxy(rows[idx], rows[idx].entity); ++bound; }
    if (m_bindSurfaceCache)
        for (uint32_t id : m_bindSurfaceCache->MappedThisFrame())
        {
            if (id >= m_retainedEntityToIdx.size()) continue;
            const uint32_t idx = m_retainedEntityToIdx[id];
            if (idx == 0xFFFFFFFFu || idx >= nStatic) continue;
            BindProxy(rows[idx], rows[idx].entity);
            ++bound;
            if (rows[idx].surfaceCache) bm.pageRows.push_back(idx);
        }
    for (size_t i = nStatic; i < rows.size(); ++i) { BindProxy(rows[i], rows[i].entity); ++bound; }
    m_bindRowsBound.store(bound, std::memory_order_relaxed);

    static const bool s_verify = Sys::CommandLine::Get().Has("-ddgimirrorverify");
    if (s_verify)
    {
        // The full bind of the blob, with Gather's post-blob fields taken from
        // the row (Gather's own verify checks those against the World).
        uint64_t bad = 0;
        Pending ref;
        for (uint32_t i = 0; i < nStatic; ++i)
        {
            std::memcpy(&ref, &blob[i], sizeof(Pending));
            ref.instanceMask      = rows[i].instanceMask;
            ref.pathMaterialIndex = rows[i].pathMaterialIndex;
            BindProxy(ref, ref.entity);
            if (std::memcmp(&ref, &rows[i], sizeof(Pending)) != 0) ++bad;
        }
        m_bindVerified.fetch_add(1, std::memory_order_relaxed);
        m_bindMismatchRows.fetch_add(bad, std::memory_order_relaxed);
    }
}

// Consumes what Gather produced. No World, no pools.
uint64_t
DDGISceneAS::BuildOrRefit(IGraphicsDevice& gfx,
                          RHI::CommandList cmd,
                          float            timeSec,
                          bool             allowSkinned)
{
    ZoneScopedN("DDGI.BuildOrRefit");

    m_pathMaterialsReady = false;
    m_giEmitterCount = 0;
    if (!gfx.GetCapabilities().raytracing || !cmd.IsValid() || !GatherRead().valid) return 0;
    // Consume-once: a caller that reaches the build without a matching Gather this frame gets 0,
    // not a TLAS of the previous frame's rows.
    GatherRead().valid = false;

    auto& pending = GatherRead().pending;

    // Gather admits skinned rows on the TLAS demand alone; allowSkinned drops them here when the
    // command lists that feed them (demoteCL, m_frameSkinCL) do not exist - Render() creates those
    // after Gather. skinnedCount is recomputed because it feeds dynamicPresent and the
    // unchanged-bytes gate.
    uint32_t skinnedCount = GatherRead().skinnedCount;
    if (!allowSkinned && skinnedCount > 0)
    {
        pending.erase(std::remove_if(pending.begin(), pending.end(),
                                     [](const Pending& p) { return p.skinned; }),
                      pending.end());
        skinnedCount = 0;
    }
    m_transmissiveCount = static_cast<uint32_t>(
        std::count_if(pending.begin(), pending.end(), [](const Pending& p) { return p.transmissive; }));

    // Bind here, after this frame's plan; -ddgibindfused bound inside Gather's fillMaterialProxy
    // already.
    static const bool s_bindFusedHere = Sys::CommandLine::Get().Has("-ddgibindfused");
    if (!s_bindFusedHere) BindGatheredRows(pending, m_gatherRead);
    ReportProxySlotCensus(pending, GatherRead().usePatch);


    // Nested GPU timestamps (AS.*) so acceleration-structure work is not booked as probe-trace
    // cost under the enclosing "DDGI" region.
    const bool tsOk = cmd.IsValid();
    auto asBegin = [&](const char* name) -> uint32_t {
        return tsOk ? gfx.BeginGPUTimestamp(cmd, name) : 0xFFFFFFFFu;
    };
    auto asEnd = [&](uint32_t r) {
        // Not gated on r: BeginGPUTimestamp opened a capture marker even when
        // the profiler handed back no region, and EndGPUTimestamp closes it.
        if (tsOk) gfx.EndGPUTimestamp(cmd, r);
    };

    // Advances once per real build/refit; drives the BLAS-compaction readback
    // timing (see m_buildCounter / kCompactReadyDelay below).
    ++m_buildCounter;

    // BEFORE the prune and the BLAS reuse test below - a destroy recorded since
    // the last build has to land before anything decides a BLAS can be refit.
    ApplyDestroyed();

    // Prune skinned entries not gathered recently (entity destroyed, mesh component removed,
    // anim-culled for a while, RT off). Runs before the empty-pending early-out so the pool drains
    // on empty scenes too. Entries gathered this call still carry last call's counter here (the
    // build loop refreshes it); the kFrameCount+1 grace window covers that and any in-flight TLAS
    // still referencing the BLAS VA, and DestroyAccelerationStructure defers the GPU release.
    for (auto it = m_skinnedBlas.begin(); it != m_skinnedBlas.end(); )
    {
        if (it->second.lastSeenCounter + kFrameCount + 1 < m_buildCounter)
        {
            if (it->second.blas.Valid())
                gfx.DestroyAccelerationStructure(it->second.blas.as);
            it = m_skinnedBlas.erase(it);
        }
        else
            ++it;
    }

    if (pending.empty()) return 0;

    // ---- Phase B: build missing BLAS ----
    // One BLAS per unique mesh key; blasScratchMax tracks the largest per-BLAS scratch, separate from
    // the TLAS scratch. Static builds are budgeted per frame: thousands of unique meshes (Bistro
    // scale) in one command list - or the allocation storm alone - can hit the 2 s TDR on the first
    // frame. Keys over budget wait for later frames, and their instances skip the TLAS until the BLAS
    // exists. The cap is conservative.
    constexpr uint32_t kMaxBLASBuildsPerFrame = 16;
    uint64_t blasScratchMax = 0;
    {
        // Group the "build needed" rows by key so duplicates do not repeat size queries and barriers.
        // Skipped on a clean patch frame with no outstanding static BLAS: usePatch requires no adds (or
        // only skinned adds, which this scan never sees) and m_retainedUnresolved == 0, so static
        // membership cannot have changed, and m_blasCache only shrinks via PruneStaleBLAS, which runs
        // from OnWorldClear after m_retainedValid is false. The scan would find nothing (a ~7000-bucket
        // table and ~14K lookups per frame).
        const bool skipStaticBlasScan = GatherRead().usePatch && m_staticBlasAllResolved;
        std::unordered_map<CacheKey, RHI::ASTriangles, CacheKeyHash> needBuild;
        if (!skipStaticBlasScan)
        {
        needBuild.reserve(pending.size());

        for (const Pending& p : pending)
        {
            // Skinned rows have no CacheKey (value-init {0,0,0}): letting them through would build a
            // phantom static BLAS from the transient ring slice and later crash PruneStaleBLAS on the
            // generation-0 handle it rebuilds from that key.
            if (p.skinned) continue;
            if (m_blasCache.find(p.key) != m_blasCache.end()) continue;
            if (needBuild.find(p.key)   != needBuild.end())   continue;

            RHI::ASTriangles g{};
            g.positionAddress = p.positionVA;
            g.positionStride = p.positionStride;
            g.vertexCount    = p.vertexCount;
            g.indexAddress   = p.indexVA;
            g.indexCount     = p.indexCount;
            g.indexFormat    = p.indexFormat; // R32_UINT (lib) or R16_UINT (primitives)
            g.opaque         = true;
            needBuild.emplace(p.key, g);
        }
        // Latch ONLY the "nothing outstanding" case. A non-empty scan means the
        // 16-per-frame budget may defer some, and a build can fail outright, so
        // stay pessimistic and rescan next frame.
        m_staticBlasAllResolved = needBuild.empty();
        }
        const uint32_t totalNeeded = (uint32_t)needBuild.size();
        const uint32_t toBuild     = std::min(totalNeeded, kMaxBLASBuildsPerFrame);
        if (totalNeeded > kMaxBLASBuildsPerFrame)
        {
            static uint32_t s_logCounter = 0;
            if ((++s_logCounter % 60u) == 0)
                LOG_INFO("DDGISceneAS: %u BLAS pending; building %u this frame "
                         "(budget %u, deferring %u to next frame)",
                         totalNeeded, toBuild, kMaxBLASBuildsPerFrame,
                         totalNeeded - toBuild);
        }

        // Size scratch once, for the BLAS maximum and the TLAS together: reallocating m_scratch after AS
        // builds are recorded frees a resource the GPU still uses -> invalid GPU VA -> TDR.
        uint32_t sizedSoFar = 0;
        for (const auto& [k, g] : needBuild)
        {
            if (sizedSoFar++ >= toBuild) break;
            uint64_t resSz = 0, scratchSz = 0;
            // Same flags as the build (ALLOW_COMPACTION), or the sizes do not describe the built AS.
            if (!QueryBLASSize(gfx, g, kStaticBLASFlags, resSz, scratchSz)) continue;
            blasScratchMax = std::max<uint64_t>(blasScratchMax, scratchSz);
        }
        // Skinned BLAS are rebuilt or refit in place each frame, so every scheduled skinned row needs
        // scratch, not only new ones. Result buffers are allocated here (no GPU work recorded yet) and
        // reused across frames: prebuild sizes depend only on the vertex/index counts. Flags must match
        // the build loop below.
        // Each skinned build gets its own scratch region, so the builds need no scratch UAV barrier
        // between them (one shared region cost N pipeline drains - milliseconds of idle for ~0.2 ms of
        // build work at ~35 sections). Pass C decides refit-vs-rebuild per entry, because the region
        // size depends on it (update scratch << build scratch).
        constexpr uint32_t kSkinnedRebuildInterval = 16; // frames between forced rebuilds
        // Budget for skinned builds, separate from kMaxBLASBuildsPerFrame (cold-path build-once
        // entries). A built section over budget keeps last frame's BLAS (a stale pose, still in the
        // TLAS); never-built sections are exempt (Pass B).
        constexpr uint32_t kMaxSkinnedBLASBuildsPerFrame = 64;

        // ---- Pass A: give every skinned entry a correctly sized BLAS ----
        // Runs over all skinned rows regardless of the budget: allocation records no GPU work, and the
        // scheduler below needs accurate `builtOnce` / `updatable` for every entry.
        // Fail closed on either failure: an entry left with last frame's scratchOffset / performUpdate
        // would overlap re-packed scratch regions (the build loop has no per-build barriers), and a
        // counts-changed entry would be built with the new counts into the old, possibly smaller, buffer
        // as an update - undefined in DXR across an IndexCount change. Resetting costs one frame of that
        // section being absent and cannot corrupt.
        std::vector<uint32_t> skinnedIdx;
        skinnedIdx.reserve(skinnedCount);
        for (uint32_t i = 0; i < (uint32_t)pending.size(); ++i)
        {
            Pending& p = pending[i];
            if (!p.skinned) continue;
            SkinnedBLAS& entry = m_skinnedBlas[p.entity];
            if (!entry.blas.Valid() ||
                entry.vertexCount != p.vertexCount || entry.indexCount != p.indexCount)
            {
                auto failClosed = [&entry]()
                {
                    entry.builtOnce        = false;
                    entry.performUpdate    = false;
                    entry.scratchOffset    = 0;
                    entry.vertexCount      = 0;   // force a full re-query next frame
                    entry.indexCount       = 0;
                };

                RHI::ASTriangles g{};
                g.positionAddress = p.positionVA;
                g.positionStride = p.positionStride;
                g.vertexCount    = p.vertexCount;
                g.indexAddress   = p.indexVA;
                g.indexCount     = p.indexCount;
                g.indexFormat    = p.indexFormat;
                g.opaque         = true;
                uint64_t resSz = 0, scratchSz = 0, updSz = 0;
                // ALLOW_UPDATE so this BLAS can be refit later; BuildBLAS below must pass the same
                // flags or these sizes do not describe the built AS.
                if (!QueryBLASSize(gfx, g, kSkinnedBLASFlags, resSz, scratchSz, &updSz))
                { failClosed(); continue; }
                SceneBLAS fresh;
                if (!gfx.CreateAccelerationStructure(resSz, RHI::ASMemory::RESULT, fresh.as))
                { failClosed(); continue; }
                // Mesh re-registered with different counts: the old resource may back an in-flight
                // TLAS - defer, do not drop.
                if (entry.blas.Valid())
                    gfx.DestroyAccelerationStructure(entry.blas.as);
                entry.blas              = fresh;
                entry.resultSize        = resSz;
                entry.scratchSize       = scratchSz;
                entry.updateScratchSize = updSz;
                // updSz == 0 means the driver reports no refit scratch for this
                // geometry; treat that as "not refittable" and always rebuild
                // rather than guessing a size.
                entry.updatable         = (updSz > 0);
                entry.vertexCount       = p.vertexCount;
                entry.indexCount        = p.indexCount;
                entry.builtOnce         = false;
                entry.framesSinceRebuild = 0;
            }
            if (entry.blas.Valid()) skinnedIdx.push_back(i);
        }

        // ---- Pass B: choose which entries build this frame ----
        // Two tiers, because they fail differently:
        //   * never-built entries are not in the TLAS at all (the instance loop drops !builtOnce), so
        //     they are exempt from the budget - a new character must not wait ceil(N/64) frames;
        //   * built entries only go stale, so they rotate through the remaining budget; the cursor makes
        //     it a latency cap, not a permanent exclusion of the list's tail.
        std::vector<uint32_t> buildOrder;
        buildOrder.reserve(skinnedIdx.size());
        for (uint32_t idx : skinnedIdx)
            if (!m_skinnedBlas[pending[idx].entity].builtOnce)
                buildOrder.push_back(idx);
        const uint32_t freshCount = (uint32_t)buildOrder.size();
        if (!skinnedIdx.empty() && freshCount < kMaxSkinnedBLASBuildsPerFrame)
        {
            const uint32_t n     = (uint32_t)skinnedIdx.size();
            const uint32_t slots = kMaxSkinnedBLASBuildsPerFrame - freshCount;
            const uint32_t start = m_skinnedBuildCursor % n;
            uint32_t taken = 0;
            for (uint32_t k = 0; k < n && taken < slots; ++k)
            {
                const uint32_t idx = skinnedIdx[(start + k) % n];
                if (!m_skinnedBlas[pending[idx].entity].builtOnce) continue; // already queued above
                buildOrder.push_back(idx);
                ++taken;
            }
            m_skinnedBuildCursor = (start + taken) % n;
        }

        // ---- Pass C: size scratch for exactly the entries that build ----
        // Sizing only the scheduled set also shrinks the scratch buffer in scenes over the budget.
        uint64_t skinnedScratchTotal = 0;
        for (uint32_t idx : buildOrder)
        {
            const Pending& p   = pending[idx];
            SkinnedBLAS&   entry = m_skinnedBlas[p.entity];
            // Refit unless this is the first build, the BLAS is not refittable,
            // or the periodic quality rebuild is due. The interval is staggered
            // per entity so ~1/16 of the sections rebuild each frame instead of
            // all of them landing on the same frame as a spike.
            const uint32_t phase =
                (uint32_t)((static_cast<uint64_t>(p.entity) * 2654435761ull) % kSkinnedRebuildInterval);
            const bool rebuildDue =
                (entry.framesSinceRebuild >= kSkinnedRebuildInterval) &&
                ((m_buildCounter % kSkinnedRebuildInterval) == phase);
            entry.performUpdate = entry.builtOnce && entry.updatable && !rebuildDue;

            const uint64_t need = entry.performUpdate ? entry.updateScratchSize
                                                      : entry.scratchSize;
            entry.scratchOffset  = skinnedScratchTotal;
            skinnedScratchTotal += AlignTo(need,
                RHI::kASAlignment);
        }
        // The TLAS scratch too: size it before any AS work is recorded.
        EnsureTLAS(gfx, (uint32_t)pending.size());
        // Static BLAS builds share one region (budgeted to 16/frame, cold-path), so the buffer holds
        // max(staticMax, tlas) for those plus the whole sub-allocated skinned block laid out after them.
        const uint64_t sharedRegion = std::max<uint64_t>(blasScratchMax, m_tlas.scratchSize);
        const uint64_t skinnedBase  = AlignTo(sharedRegion,
            RHI::kASAlignment);
        const uint64_t totalScratchNeeded = skinnedBase + skinnedScratchTotal;
        if (totalScratchNeeded > 0) EnsureScratch(gfx, totalScratchNeeded);

        // A BLAS built this call has its COMPACTED_SIZE readback GPU-ready after
        // kFrameCount more BuildOrRefit calls (the slot fence in BeginFrame
        // guarantees this frame's compute work has completed by then). +1 for
        // margin. State 2 (below) reads it and swaps in a shrunk BLAS.
        constexpr uint64_t kCompactReadyDelay = kFrameCount + 1;

        uint32_t builtThisFrame = 0;
        const uint32_t tsStatic = asBegin("AS.StaticBLAS");
        for (const auto& [k, g] : needBuild)
        {
            if (builtThisFrame >= toBuild) break;
            uint64_t resSz = 0, scratchSz = 0;
            if (!QueryBLASSize(gfx, g, kStaticBLASFlags, resSz, scratchSz)) continue;
            // Fail closed if this entry's scratch was never folded into blasScratchMax: the sizing loop
            // advances its counter unconditionally (`sizedSoFar++ >= toBuild`) while this loop advances
            // builtThisFrame only on success, so a Query/Allocate failure lets this loop walk past the
            // sized prefix. An oversized static build at offset 0 would run into the skinned regions from
            // `skinnedBase`, which are written concurrently (no per-build barriers) -> corrupt geometry,
            // then a DispatchRays hang. Skipping is safe: the key stays out of m_blasCache, so a later
            // frame's scan sizes it.
            if (scratchSz > blasScratchMax)
            {
                static uint32_t s_unsizedLogCounter = 0;
                if ((++s_unsizedLogCounter % 60u) == 0)
                    LOG_WARNING("DDGISceneAS: skipped a BLAS build whose scratch (%llu B) exceeds "
                                "this frame's sized maximum (%llu B) — sizing/build loop diverged, "
                                "likely an AllocateBLAS failure earlier in the batch",
                                (unsigned long long)scratchSz, (unsigned long long)blasScratchMax);
                continue;
            }
            SceneBLAS blas;
            if (!gfx.CreateAccelerationStructure(resSz, RHI::ASMemory::RESULT, blas.as)) continue;
            gfx.BuildBLAS(cmd, &g, 1, kStaticBLASFlags, blas.as, m_scratch, 0);
            // Two UAV barriers: the BLAS result (the TLAS build and the size query read it) and the
            // scratch buffer (the next static build reuses offset 0 and must not race this one's
            // writes - corrupted geometry, then a GPU hang).
            const RHI::GPUBarrier built[2] = { RHI::GPUBarrier::Memory(&blas.as),
                                               RHI::GPUBarrier::Memory(&m_scratch) };
            gfx.PushBarriers(built, 2, cmd);
            // Queue the compacted-size readback; State 2 shrinks the BLAS a few frames later. A
            // failed query leaves compactState = None -> it stays full size.
            if (gfx.QueryCompactedSize(cmd, blas.as))
                blas.compactState = SceneBLAS::CompactState::AwaitingSize;
            blas.compactReadyCounter = m_buildCounter + kCompactReadyDelay;
            // Only a BLAS that actually queued a readback can ever be
            // AwaitingSize, so this is the complete set the pass must examine.
            if (blas.compactState == SceneBLAS::CompactState::AwaitingSize)
                m_blasCompactPending.push_back(k);
            // A handle does not release itself: one that did not go in (needBuild
            // dedupes keys, so this cannot happen) is destroyed, not leaked.
            if (!m_blasCache.emplace(k, blas).second)
                gfx.DestroyAccelerationStructure(blas.as);
            builtThisFrame++;
        }
        asEnd(tsStatic);

        // ---- Skinned BLAS rebuild, in place ----
        // PREFER_FAST_BUILD, no compaction (the compaction machine assumes build-once). Rebuilding into
        // the same result buffer is safe against earlier frames' traces: this CL executes after ddgiCL's
        // demote-fence wait, which the graphics queue serialises behind frame N-1's RTR trace.
        // This loop executes Pass B's `buildOrder`; unscheduled entries keep last frame's BLAS (a
        // 1-frame-stale pose over valid geometry, still in the TLAS).
        // Refresh lastSeenCounter for every gathered entry, not only the scheduled ones, or the prune
        // sweep releases a section that is waiting its turn.
        for (const Pending& p : pending)
        {
            if (!p.skinned) continue;
            auto it = m_skinnedBlas.find(p.entity);
            if (it != m_skinnedBlas.end()) it->second.lastSeenCounter = m_buildCounter;
        }

        uint32_t skinnedBuilt = 0;
        const uint32_t tsSkinned = asBegin("AS.SkinnedBLAS");
        for (uint32_t idx : buildOrder)
        {
            const Pending& p = pending[idx];
            auto it = m_skinnedBlas.find(p.entity);
            if (it == m_skinnedBlas.end()) continue;
            SkinnedBLAS& entry = it->second;
            if (!entry.blas.Valid()) continue;

            RHI::ASTriangles g{};
            g.positionAddress = p.positionVA;
            g.positionStride = p.positionStride;
            g.vertexCount    = p.vertexCount;
            g.indexAddress   = p.indexVA;
            g.indexCount     = p.indexCount;
            g.indexFormat    = p.indexFormat;
            g.opaque         = true;
            // No barrier per build: every skinned build owns a private scratch region, so the
            // builds pipeline freely; one UAV barrier after the loop covers all results before the
            // TLAS build.
            gfx.BuildBLAS(cmd, &g, 1,
                          entry.performUpdate ? kSkinnedBLASFlags | RHI::ASBuildFlags::PERFORM_UPDATE
                                              : kSkinnedBLASFlags,
                          entry.blas.as, m_scratch, skinnedBase + entry.scratchOffset);
            entry.framesSinceRebuild = entry.performUpdate
                                     ? entry.framesSinceRebuild + 1u : 0u;
            entry.builtOnce = true;
            entry.builtDeform = p.deformHash;
            ++skinnedBuilt;
        }
        // One covering UAV barrier for the batch: the null-resource form covers every BLAS result
        // and the scratch buffer, with one pipeline drain instead of N.
        if (skinnedBuilt > 0)
            gfx.PushBarrier(RHI::GPUBarrier::Memory(), cmd);
        asEnd(tsSkinned);
        if (skinnedCount > kMaxSkinnedBLASBuildsPerFrame)
        {
            // A deferred section pairs an older BLAS with this frame's stream bases: hit shading
            // reads the current pose over slightly stale triangles. Cosmetic (both are valid memory);
            // the rotation refreshes every section within ceil(N / budget) frames, the staleness
            // bound this log reports.
            static uint32_t s_skinLogCounter = 0;
            if ((++s_skinLogCounter % 60u) == 0)
                LOG_WARNING("DDGISceneAS: %u skinned sections gathered, budget %u/frame "
                            "— rotating; worst-case pose staleness %u frames",
                            skinnedCount, kMaxSkinnedBLASBuildsPerFrame,
                            (skinnedCount + kMaxSkinnedBLASBuildsPerFrame - 1u)
                                / kMaxSkinnedBLASBuildsPerFrame);
        }
    }

    // ---- State 2: compact BLAS whose COMPACTED_SIZE readback is GPU-ready ----
    // Runs before the TLAS build so a swapped (shrunk) BLAS VA reaches this frame's instances. The
    // COMPACT copy needs no scratch, so the m_scratch sizing above is untouched. Budgeted like the
    // build loop (TDR).
    bool blasSwapped = false;
    const uint32_t tsCompact = asBegin("AS.Compact");
    {
        constexpr uint32_t kMaxBLASCompactionsPerFrame = 16;
        uint32_t compactedThisFrame = 0;
        // Walk the pending list, not the whole cache. Entries that finish (or
        // whose BLAS is gone) are swap-erased; anything still awaiting its
        // readback, or deferred by the per-frame budget, stays for next frame.
        for (size_t pi = 0; pi < m_blasCompactPending.size(); )
        {
            if (compactedThisFrame >= kMaxBLASCompactionsPerFrame) break;

            const CacheKey k = m_blasCompactPending[pi];
            auto cacheIt = m_blasCache.find(k);
            if (cacheIt == m_blasCache.end())
            {   // BLAS released (world clear / prune): drop the pending entry.
                m_blasCompactPending[pi] = m_blasCompactPending.back();
                m_blasCompactPending.pop_back();
                continue;
            }
            SceneBLAS& blas = cacheIt->second;

            if (blas.compactState != SceneBLAS::CompactState::AwaitingSize)
            {   // Already resolved by some other path: stop tracking it.
                m_blasCompactPending[pi] = m_blasCompactPending.back();
                m_blasCompactPending.pop_back();
                continue;
            }
            if (m_buildCounter < blas.compactReadyCounter) { ++pi; continue; } // readback not GPU-ready yet

            // This entry is about to reach Done on every path below.
            m_blasCompactPending[pi] = m_blasCompactPending.back();
            m_blasCompactPending.pop_back();

            // The query's size buffers are released whatever the outcome
            // (slot-based defer: still referenced by the recent emit/copy CL).
            const uint64_t compactedSize = gfx.ReadCompactedSize(blas.as);

            if (compactedSize > 0 && compactedSize < blas.as.sizeBytes)
            {
                RHI::AccelerationStructure compacted;
                if (gfx.CompactAccelerationStructure(cmd, blas.as, compactedSize, compacted))
                {
                    // The TLAS build that reads this compacted BLAS this same frame
                    // must wait for the copy to finish.
                    gfx.PushBarrier(RHI::GPUBarrier::Memory(&compacted), cmd);
                    // The original is still referenced by in-flight TLASes (<= kFrameCount frames)
                    // and by this frame's compaction copy; the slot-based defer outlives both.
                    gfx.DestroyAccelerationStructure(blas.as);
                    blas.as           = compacted;
                    blas.compactState = SceneBLAS::CompactState::Done;
                    blasSwapped       = true;
                    compactedThisFrame++;
                    continue;
                }
            }
            // No savings (or compaction unavailable) -> keep the original, stop retrying.
            blas.compactState = SceneBLAS::CompactState::Done;
        }
    }

    asEnd(tsCompact);

    // ---- Phase C: build the TLAS + per-instance material buffer ----
    const uint32_t tsTlas = asBegin("AS.TLAS");
    EnsureMaterialBuffer(gfx, (uint32_t)pending.size());
    EnsureAlphaBuffer   (gfx, (uint32_t)pending.size());
    if (!m_tlas.as.IsValid()) { asEnd(tsTlas); return 0; }
    // Do not EnsureScratch here: scratch was sized in Phase B for max(BLAS, TLAS), and a
    // reallocation would free the buffer the BLAS builds queued on `cmd` still reference.

    // The job-split preset (-jobdefaultsoff) decides whether the records are built by jobs (adaptive
    // chunks) and the BLAS addresses resolved by jobs; off, one serial loop does both.
    const bool recordJobs = Sys::JobSplittingDefaultsEnabled();
    const bool resolveJobs = recordJobs;
    bool verifyRecords = false;
#if WITH_HARNESS
    verifyRecords = Sys::CommandLine::Get().Has("-asrecordverify");
#endif
    auto& instances = m_instancesScratch;
    // m_instancesScratch persists across frames (no ~500 KB alloc/free per frame at 7K instances).
    // The fingerprint below hashes these raw bytes, padding included, so every record must be fully
    // rewritten from a zeroed value each frame: the serial path clears and push_backs
    // value-initialised records, graph workers memcpy whole memset-zeroed ones (keeping the size on
    // stable frames skips a redundant resize init). Stale tail bytes would silently break the
    // TLAS-reuse gate - a full ~7000-instance rebuild every frame that shows only as compute-queue
    // time.
    if (!recordJobs || verifyRecords) instances.clear();
    instances.reserve(pending.size());
    const uint32_t frameSlot = gfx.GetFrameIndex();
    const std::vector<Resource::MaterialGPUData>* pathMaterials = GatherRead().pathMaterials.get();
    m_pathMaterialHash = GatherRead().pathMaterialHash;
    // The frame fence has retired this slot. Other slots remain untouched.
    if (!pathMaterials || pathMaterials->empty())
    {
        ReleasePathMaterials(gfx);
    }
    else if (m_pathMaterialCapacity[frameSlot] < pending.size() || !m_pathMaterialMapped[frameSlot])
    {
        m_pathMatSlotRevision[frameSlot] = 0; // the new buffer holds nothing yet
        if (m_pathMaterialMapped[frameSlot]) gfx.UnmapBuffer(m_pathMaterials[frameSlot]);
        if (m_pathMaterials[frameSlot].IsValid()) gfx.DestroyBuffer(m_pathMaterials[frameSlot]);
        m_pathMaterialMapped[frameSlot] = nullptr; m_pathMaterialCapacity[frameSlot] = 0;
        RHI::GPUBufferDesc desc{};
        desc.size = uint64_t(pending.size()) * sizeof(Resource::MaterialGPUData);
        desc.stride = sizeof(Resource::MaterialGPUData);
        desc.usage = RHI::Usage::UPLOAD;
        desc.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        desc.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        if (gfx.CreateBuffer(desc, m_pathMaterials[frameSlot]))
        {
            m_pathMaterialMapped[frameSlot] = static_cast<Resource::MaterialGPUData*>(gfx.MapBuffer(m_pathMaterials[frameSlot]));
            if (m_pathMaterialMapped[frameSlot]) m_pathMaterialCapacity[frameSlot] = uint32_t(pending.size());
        }
    }
    m_pathMaterialsReady = pathMaterials && !pathMaterials->empty() && m_pathMaterialMapped[frameSlot] &&
        m_pathMaterialCapacity[frameSlot] >= pending.size();
    // A slot that already holds this revision skips every InstanceID whose row did not move. A new
    // revision forgets the whole map first: an instance absent this frame must not keep a row
    // recorded under the old one.
    std::vector<uint32_t>& slotRows = m_pathMatSlotRow[frameSlot];
    const bool slotCurrent = m_pathMaterialsReady && m_pathMatSlotRevision[frameSlot] == m_pathMaterialHash;
    if (m_pathMaterialsReady)
    {
        if (!slotCurrent) slotRows.assign(pending.size(), ~0u);
        else if (slotRows.size() < pending.size()) slotRows.resize(pending.size(), ~0u);
        m_pathMatSlotRevision[frameSlot] = m_pathMaterialHash;
    }
    auto publishPathMaterial = [&](const Pending& p, uint32_t instanceID) {
        if (!m_pathMaterialsReady || instanceID >= slotRows.size()) return;
        if (slotRows[instanceID] == p.pathMaterialIndex) return;
        slotRows[instanceID] = p.pathMaterialIndex;
        m_pathMaterialMapped[frameSlot][instanceID] = (*pathMaterials)[p.pathMaterialIndex];
    };
    // STAGING, not the bound table: matDst/alphaDst point at this frame slot's UPLOAD buffer, which
    // receives only changed records. The shaders read the DEFAULT tables (m_matBufferGPU /
    // m_alphaBufferGPU); the CopyBufferRegion loop after this one moves the dirty spans across.
    // Staging is indexed by instIdx (src offset == dst offset). A missing DEFAULT table nulls the
    // staging pointer, so the "no dst => do not publish" guard below also keeps instances whose
    // records never reach the GPU out of the TLAS.
    auto* matDst = (frameSlot < kFrameCount && m_matBufferGPU.IsValid())
        ? static_cast<DDGIInstanceData*>(m_matMapped[frameSlot])
        : nullptr;
    auto* alphaDst = (frameSlot < kFrameCount && m_alphaBufferGPU.IsValid())
        ? static_cast<RTAlphaRecord*>(m_alphaMapped[frameSlot])
        : nullptr;
    auto* matMaster   = reinterpret_cast<DDGIInstanceData*>(m_matMaster.data());
    auto* alphaMaster = reinterpret_cast<RTAlphaRecord*>(m_alphaMaster.data());
    uint32_t alphaClipCount = 0;

    // Covers the instance/material/alpha record loop through the TLAS
    // instance upload + build record; DDGI.Fingerprint nests inside.
    // ZoneNamedN (not ZoneScopedN): same scope as the function-top zone, so
    // the fixed ___tracy_scoped_zone variable would collide under TRACY_ENABLE.
    ZoneNamedN(zoneWriteInstances, "DDGI.WriteInstances", true);

    // Append record @p idx to the open dirty span, or open a new one. The record is copied out of
    // the MASTER (the caller updates the master first), which makes the gap merge legal: for every
    // index the caller skipped, master == what the DEFAULT table holds, so re-copying it is a no-op
    // that saves a CopyBufferRegion.
    // Slots follow emit order (idx is the TLAS instance index, +1 per published instance), so a cold
    // frame's writes are dense and coalesce into a few CopyBufferRegions, as
    // Renderer::StageMaterialSlot relies on too.
    auto stageRecord = [](std::vector<RecordCopyRange>& ranges,
                          uint8_t* staging, const uint8_t* master,
                          uint32_t idx, size_t stride)
    {
        // Gap tolerance: bridging up to this many clean records is cheaper than
        // a second CopyBufferRegion (each carries fixed driver-side cost).
        constexpr uint32_t kGapMerge  = 8;
        // Hard cap on copies per table per frame: past it the gap merge is unbounded, so
        // pathological scatter degenerates to one table-wide copy.
        constexpr size_t   kMaxRanges = 64;
        if (!ranges.empty())
        {
            RecordCopyRange& last = ranges.back();
            const uint32_t end = last.first + last.count;
            if (idx >= end)
            {
                const uint32_t gap = idx - end;
                if (gap <= kGapMerge || ranges.size() >= kMaxRanges)
                {
                    std::memcpy(staging + size_t(end) * stride,
                                master  + size_t(end) * stride,
                                size_t(gap + 1) * stride);
                    last.count += gap + 1;
                    return;
                }
            }
        }
        std::memcpy(staging + size_t(idx) * stride,
                    master  + size_t(idx) * stride, stride);
        ranges.push_back({ idx, 1u });
    };
    // Buffer (re)creation, growth or a world clear left the masters not describing the DEFAULT
    // tables: stage everything once this frame.
    const bool uploadAll = m_recordUploadAll;

    // Wind foliage re-shears its transform from timeSec every frame, so those instance bytes really
    // change and the fingerprint schedules their refit. Counted so the static-scene fingerprint reuse
    // can refuse to engage while any wind instance exists.
    uint32_t windInstanceCount = 0;

    // Pure CPU record construction (no World reads, allocators, descriptors or CL writes), so graph
    // workers can run it. The serial loop below is the independent reference (-asrecordverify
    // compares the two).
    struct PreparedRecord {
        SceneASInstance instance;
        DDGIInstanceData material;
        RTAlphaRecord alpha;
        uint8_t flags;
    };
    // Phase C only: builds, compaction and cache mutation have finished. Nothing is written back into
    // the published GatherRead payload or retained across calls.
    auto lookupStaticVA = [&](const CacheKey& key) -> uint64_t {
        const auto it = m_blasCache.find(key);
        return it == m_blasCache.end() ? 0 : it->second.GPUAddress();
    };
    auto prepareRecord = [timeSec, &publishPathMaterial](const Pending& p, uint64_t blasVA, uint32_t instIdx) {
        PreparedRecord result;
        static_assert(std::is_trivially_copyable_v<PreparedRecord>);
        std::memset(&result, 0, sizeof(result));
        auto& inst = result.instance;
        // Wind foliage: shear the instance so ray tracing sees the sway the vertex shader draws. The
        // main bend is linear in height, an affine shear, so this is exact (FoliageWind.h has the
        // derivation and the per-vertex flutter it omits). Done here, not in the gather, so it is
        // recomputed from `timeSec` every frame without dirty tracking (patch frames reuse the blob's
        // worldMatrix); the changed bytes change the fingerprint, which schedules the refit - so a
        // scene with swaying foliage never takes the static-scene fast path.
        if (p.wind.Enabled())
        {
            result.flags |= 4u;
            const XMFLOAT4X4 swayed =
                FoliageWind::ApplyToWorld(p.worldMatrix, p.wind, timeSec);
            ToInstanceTransform(swayed, inst.transform);
        }
        else
        {
            ToInstanceTransform(p.worldMatrix, inst.transform);
        }
        publishPathMaterial(p, instIdx);
        inst.instanceID    = instIdx; // Closest-hit reads g_DDGIInstances[InstanceID()]
        // Mask bits: 0 RTR/RTAO, 1 RT-shadow caster, 2 DDGI, 3 front-layer RTR (see instanceMask).
        inst.instanceMask  = p.instanceMask;
        inst.hitGroupIndex = 0;
        inst.flags         = 0;
        // Mirrored transforms (negative determinant) need no facing flag: ray tracing decides facing in
        // object space, so the instance transform never flips it (raster decides after the transform,
        // hence its per-draw frontCounterClockwise). TRIANGLE_FRONT_COUNTERCLOCKWISE on them would turn
        // them inside out to every ray. DDGI decides facing from vertex normals. The path tracer culls
        // back faces in fixed function (RAY_FLAG_CULL_BACK_FACING_TRIANGLES); a double-sided material
        // keeps both faces. No other ray culls.
        // A Transmissive solid is left through its back faces: no ray may cull them.
        if (p.doubleSided || p.transmissive)
            inst.flags |= RHI::ASInstanceFlag::TRIANGLE_CULL_DISABLE;
        // Alpha cutouts become non-opaque at the instance level, never per geometry: the BLAS cache is
        // keyed by mesh only, so a geometry flag would leak to every entity sharing the mesh, and the
        // skinned prebuild query only re-runs on a count change. Instance flags override the geometry
        // OPAQUE flag, and a ray's RAY_FLAG_FORCE_OPAQUE overrides them in turn (RTAO and DDGI keep it).
        // Clearing p.alphaClip in fillMaterialProxy is the CPU kill switch; a shader-side gate could not
        // restore fixed-function traversal. The fingerprint hashes `flags` too, so there is no
        // dirty-skip hazard.
        // Blend occluders take the same route: without FORCE_NON_OPAQUE traversal commits the glass and
        // the shader cannot accumulate transmittance. The `blendOpacity > 1/256` gate keeps Additive /
        // Multiply (opacity 0, still traced by RTR/RTAO through bit 0x01) out of a candidate callback
        // that cannot change the result.
        // A Transmissive one always: a clear pane's opacity can round to 0, and the path tracer's
        // rays must reach its candidate test to take its transmittance.
        const bool blendNonOpaque =
            (p.blendShadow && p.blendOpacity > (1.0f / 256.0f)) || p.transmissive;
        if (p.alphaClip || blendNonOpaque)
        {
            inst.flags |= RHI::ASInstanceFlag::FORCE_NON_OPAQUE;
            result.flags |= 8u;
        }
        inst.blasVA        = blasVA;
        inst.triangleCount = p.indexCount / 3u;
            auto& d = result.material;
            d.baseColor[0]    = p.baseColor.x;
            d.baseColor[1]    = p.baseColor.y;
            d.baseColor[2]    = p.baseColor.z;
            d.baseColor[3]    = p.baseColor.w;
            d.emissive[0]     = p.emissive.x;
            d.emissive[1]     = p.emissive.y;
            d.emissive[2]     = p.emissive.z;
            d.emissive[3]     = p.emissive.w;
            d.vbBindlessIdx   = p.vbBindlessIdx;
            d.ibBindlessIdx   = p.ibBindlessIdx;
            d.vertexStart     = p.vertexStart;
            d.indexStart      = p.indexStart;
            d.emissiveTexIdx  = p.emissiveTexIdx;
            // Same stride the BLAS build consumed: the attribute fetch must walk the VB
            // identically (see DDGIInstanceData::vertexStride).
            d.vertexStride    = p.positionStride ? p.positionStride
                                                 : kEngineVertexStride;
            d.normalBufIdx    = p.normalBufIdx;
            d.uvBufIdx        = p.uvBufIdx;
            d.albedoTexIdx    = p.albedoTexIdx;
            d.surfaceTexIdx   = p.surfaceTexIdx;
            d.roughnessMin    = p.roughnessMin;
            d.roughnessMax    = p.roughnessMax;
            d.metalnessMin    = p.metalnessMin;
            d.metalnessMax    = p.metalnessMax;
            d.reflectance     = p.reflectance;
            d.indexBytes      = p.indexBytes;
            d.nrmStart        = p.nrmStart;
            d.uvStart         = p.uvStart;
            d.rtFlags         = (p.skinned        ? kRTInstFlagSkinned        : 0u)
                              | (p.proxyUntrusted ? kRTInstFlagProxyUntrusted : 0u)
                              | (p.surfaceCache   ? kRTInstFlagSurfaceCache   : 0u)
                              | (p.doubleSided    ? kRTInstFlagDoubleSided    : 0u)
                              | (p.npr            ? kRTInstFlagNPR            : 0u)
                              | (p.albedoRaw      ? kRTInstFlagAlbedoRaw      : 0u)
                              | (p.outline        ? kRTInstFlagOutline        : 0u)
                              | (p.interleavedTangent ? kRTInstFlagInterleavedTangent : 0u)
                              | (IsGIEmitter(p.instanceMask, p.blendShadow, p.skinned, p.emissive, p.baseColor) ? kRTInstFlagGIEmitter : 0u)
                              | (p.transmissive   ? kRTInstFlagTransmissive   : 0u)
                              | (p.matSlot << 16);      // material slot + 1, 0 = none
            d.normalTexIdx    = p.normalTexIdx;
            d.uvTilingOffset[0] = p.uvTilingOffset[0];
            d.uvTilingOffset[1] = p.uvTilingOffset[1];
            d.uvTilingOffset[2] = p.uvTilingOffset[2];
            d.uvTilingOffset[3] = p.uvTilingOffset[3];
            auto& a = result.alpha;
            a.flags = (p.alphaClip   ? kRTAlphaFlagAlphaClip   : 0u)
                    | (p.doubleSided ? kRTAlphaFlagDoubleSided : 0u)
                    | (p.normalBufIdx != RHI::kInvalidBufferIndex
                                     ? kRTAlphaFlagDeinterleaved : 0u)
                    | (p.alphaFromRed ? kRTAlphaFlagOpacityR   : 0u)
                    | (p.blendShadow  ? kRTAlphaFlagBlend | p.shadowTint : 0u);
            // One slot, two meanings, discriminated by kRTAlphaFlagBlend (the
            // two flags are mutually exclusive by construction in
            // fillMaterialProxy): clip threshold for a cutout, OPACITY for a
            // translucent occluder.
            a.alphaRef        = p.blendShadow ? p.blendOpacity : p.alphaRef;
            // This index feeds only the candidate alpha test - a Masked material points it at its
            // OPACITYMAP (.r, kRTAlphaFlagOpacityR). Hit shading keeps DDGIInstanceData::albedoTexIdx.
            a.albedoTexIdx    = p.alphaTexIdx;
            a.vbBindlessIdx   = p.vbBindlessIdx;
            a.ibBindlessIdx   = p.ibBindlessIdx;
            a.uvBufIdx        = p.uvBufIdx;
            a.indexStart      = p.indexStart;
            a.uvStart         = p.uvStart;
            // Same stride and fallback as d.vertexStride above - keep the two identical.
            a.vertexStride    = p.positionStride ? p.positionStride
                                                 : kEngineVertexStride;
            a.indexBytes      = p.indexBytes;
        return result;
    };
    ZoneNamedN(recordGraphZone, "DDGI.Records.Graph", recordJobs);
    ZoneNamedN(recordSerialZone, "DDGI.Records.Serial", !recordJobs);
    // Verification saves original master + staging bytes, executes the original loop,
    // restores exactly that input, then compares the candidate's complete output.
    struct RecordState {
        std::vector<uint8_t> materialMaster, alphaMaster, materialStaging, alphaStaging;
        std::vector<SceneASInstance> instances;
        std::vector<RecordCopyRange> materialRanges, alphaRanges;
        uint32_t wind = 0, clip = 0;
    };
    auto saveState = [&] {
        RecordState state;
        const auto copyBytes = [](const void* ptr, size_t n) {
            std::vector<uint8_t> bytes;
            if (ptr && n) { bytes.resize(n); std::memcpy(bytes.data(), ptr, n); }
            return bytes;
        };
        state.materialMaster = copyBytes(matMaster, m_matMaster.size());
        state.alphaMaster = copyBytes(alphaMaster, m_alphaMaster.size());
        state.materialStaging = copyBytes(matDst, matDst ? (std::min)(pending.size(),size_t(m_matCapacity))*sizeof(DDGIInstanceData) : 0);
        state.alphaStaging = copyBytes(alphaDst, alphaDst ? (std::min)(pending.size(),size_t(m_alphaCapacity))*sizeof(RTAlphaRecord) : 0);
        state.instances = instances;
        state.materialRanges = m_matCopyRanges; state.alphaRanges = m_alphaCopyRanges;
        state.wind = windInstanceCount; state.clip = alphaClipCount;
        return state;
    };
    auto restoreState = [&](const RecordState& state) {
        const auto copy = [](void* dst, const std::vector<uint8_t>& bytes) {
            if (!bytes.empty()) std::memcpy(dst, bytes.data(), bytes.size());
        };
        copy(matMaster, state.materialMaster); copy(alphaMaster, state.alphaMaster);
        copy(matDst, state.materialStaging); copy(alphaDst, state.alphaStaging);
        instances = state.instances;
        m_matCopyRanges = state.materialRanges; m_alphaCopyRanges = state.alphaRanges;
        windInstanceCount = state.wind; alphaClipCount = state.clip;
    };
    std::optional<RecordState> before, expected;
    if (recordJobs && verifyRecords) before.emplace(saveState());
    if (!recordJobs || verifyRecords) {
    for (Pending& p : pending)
    {
        uint64_t blasVA = 0;
        if (p.skinned)
        {
            // builtOnce gate: an allocated but never-built BLAS is undefined memory, and
            // referencing it from the TLAS hangs traversal. Skinned rows are rebuilt each frame
            // and few, so they get no VA memo.
            auto it = m_skinnedBlas.find(p.entity);
            if (it == m_skinnedBlas.end() || !it->second.builtOnce) continue;
            blasVA = it->second.blas.GPUAddress();
        }
        else
        {
            // No per-row VA cache on Pending: it would write back into a payload the game half refills
            // while the render half reads it.
            blasVA = lookupStaticVA(p.key);
        }
        if (!blasVA) continue;

        const uint32_t instIdx = (uint32_t)instances.size();

        // A TLAS instance whose InstanceID() has no material/alpha record is unreadable garbage for every
        // InstanceID()-indexed fetch: never publish one. The records are read per traversal candidate,
        // where a stale flags word could send albedoTexIdx / vb / ib into the bindless heaps.
        // EnsureMaterialBuffer / EnsureAlphaBuffer(pending.size()) make the capacity test always true in
        // practice; a null dst means buffer creation failed, and an empty TLAS (RT off this frame) is
        // the right outcome.
        if (!matDst || !alphaDst || instIdx >= m_matCapacity || instIdx >= m_alphaCapacity)
            continue;

        SceneASInstance inst{};
        // Wind foliage: shear the instance transform (see prepareRecord).
        if (p.wind.Enabled())
        {
            ++windInstanceCount;
            const XMFLOAT4X4 swayed =
                FoliageWind::ApplyToWorld(p.worldMatrix, p.wind, timeSec);
            ToInstanceTransform(swayed, inst.transform);
        }
        else
        {
            ToInstanceTransform(p.worldMatrix, inst.transform);
        }
        publishPathMaterial(p, instIdx);
        inst.instanceID    = instIdx; // Closest-hit reads g_DDGIInstances[InstanceID()]
        // Mask bits: 0 RTR/RTAO, 1 RT-shadow caster, 2 DDGI, 3 front-layer RTR (see instanceMask).
        inst.instanceMask  = p.instanceMask;
        inst.hitGroupIndex = 0;
        inst.flags         = 0;
        // Mirrored transforms need no facing flag; a double-sided material disables culling (see
        // prepareRecord).
        if (p.doubleSided || p.transmissive)
            inst.flags |= RHI::ASInstanceFlag::TRIANGLE_CULL_DISABLE;
        // Alpha cutouts and blend occluders are non-opaque at the instance level (see prepareRecord).
        const bool blendNonOpaque =
            (p.blendShadow && p.blendOpacity > (1.0f / 256.0f)) || p.transmissive;
        if (p.alphaClip || blendNonOpaque)
        {
            inst.flags |= RHI::ASInstanceFlag::FORCE_NON_OPAQUE;
            ++alphaClipCount;
        }
        inst.blasVA        = blasVA;
        inst.triangleCount = p.indexCount / 3u;
        instances.push_back(inst);

        {
            DDGIInstanceData d{};
            d.baseColor[0]    = p.baseColor.x;
            d.baseColor[1]    = p.baseColor.y;
            d.baseColor[2]    = p.baseColor.z;
            d.baseColor[3]    = p.baseColor.w;
            d.emissive[0]     = p.emissive.x;
            d.emissive[1]     = p.emissive.y;
            d.emissive[2]     = p.emissive.z;
            d.emissive[3]     = p.emissive.w;
            d.vbBindlessIdx   = p.vbBindlessIdx;
            d.ibBindlessIdx   = p.ibBindlessIdx;
            d.vertexStart     = p.vertexStart;
            d.indexStart      = p.indexStart;
            d.emissiveTexIdx  = p.emissiveTexIdx;
            // Same stride the BLAS build consumed (see prepareRecord).
            d.vertexStride    = p.positionStride ? p.positionStride
                                                 : kEngineVertexStride;
            d.normalBufIdx    = p.normalBufIdx;
            d.uvBufIdx        = p.uvBufIdx;
            d.albedoTexIdx    = p.albedoTexIdx;
            d.surfaceTexIdx   = p.surfaceTexIdx;
            d.roughnessMin    = p.roughnessMin;
            d.roughnessMax    = p.roughnessMax;
            d.metalnessMin    = p.metalnessMin;
            d.metalnessMax    = p.metalnessMax;
            d.reflectance     = p.reflectance;
            d.indexBytes      = p.indexBytes;
            d.nrmStart        = p.nrmStart;
            d.uvStart         = p.uvStart;
            d.rtFlags         = (p.skinned        ? kRTInstFlagSkinned        : 0u)
                              | (p.proxyUntrusted ? kRTInstFlagProxyUntrusted : 0u)
                              | (p.surfaceCache   ? kRTInstFlagSurfaceCache   : 0u)
                              | (p.doubleSided    ? kRTInstFlagDoubleSided    : 0u)
                              | (p.npr            ? kRTInstFlagNPR            : 0u)
                              | (p.albedoRaw      ? kRTInstFlagAlbedoRaw      : 0u)
                              | (p.outline        ? kRTInstFlagOutline        : 0u)
                              | (p.interleavedTangent ? kRTInstFlagInterleavedTangent : 0u)
                              | (IsGIEmitter(p.instanceMask, p.blendShadow, p.skinned, p.emissive, p.baseColor) ? kRTInstFlagGIEmitter : 0u)
                              | (p.transmissive   ? kRTInstFlagTransmissive   : 0u)
                              | (p.matSlot << 16);      // material slot + 1, 0 = none
            d.normalTexIdx    = p.normalTexIdx;
            d.uvTilingOffset[0] = p.uvTilingOffset[0];
            d.uvTilingOffset[1] = p.uvTilingOffset[1];
            d.uvTilingOffset[2] = p.uvTilingOffset[2];
            d.uvTilingOffset[3] = p.uvTilingOffset[3];
            // Dirty compare against the CPU master: 128 bytes of 4-byte fields, no padding, so memcmp
            // is exact. A static scene stages zero bytes of write-combined memory here.
            if (uploadAll || std::memcmp(&matMaster[instIdx], &d, sizeof(d)) != 0)
            {
                matMaster[instIdx] = d;
                stageRecord(m_matCopyRanges,
                            reinterpret_cast<uint8_t*>(matDst),
                            reinterpret_cast<const uint8_t*>(matMaster),
                            instIdx, sizeof(DDGIInstanceData));
            }

            // Alpha-cutout record (40 B, same index space). kRTAlphaFlagDeinterleaved is the
            // discriminator: DDGIInstanceData encodes de-interleaved mode as `normalBufIdx !=
            // kInvalidBufferIndex`, but this record has no normalBufIdx, and "de-interleaved without a
            // UV stream" must not read as interleaved.
            RTAlphaRecord a{};
            a.flags = (p.alphaClip   ? kRTAlphaFlagAlphaClip   : 0u)
                    | (p.doubleSided ? kRTAlphaFlagDoubleSided : 0u)
                    | (p.normalBufIdx != RHI::kInvalidBufferIndex
                                     ? kRTAlphaFlagDeinterleaved : 0u)
                    | (p.alphaFromRed ? kRTAlphaFlagOpacityR   : 0u)
                    | (p.blendShadow  ? kRTAlphaFlagBlend | p.shadowTint : 0u);
            // One slot, two meanings (see prepareRecord): clip threshold or opacity.
            a.alphaRef        = p.blendShadow ? p.blendOpacity : p.alphaRef;
            // The candidate alpha test's texture only (see prepareRecord).
            a.albedoTexIdx    = p.alphaTexIdx;
            a.vbBindlessIdx   = p.vbBindlessIdx;
            a.ibBindlessIdx   = p.ibBindlessIdx;
            a.uvBufIdx        = p.uvBufIdx;
            a.indexStart      = p.indexStart;
            a.uvStart         = p.uvStart;
            // Same stride and fallback as d.vertexStride above - keep the two identical.
            a.vertexStride    = p.positionStride ? p.positionStride
                                                 : kEngineVertexStride;
            a.indexBytes      = p.indexBytes;
            // Same dirty compare as the instance record (40 bytes of 4-byte fields, no padding), with
            // its own span list: a shading edit dirties the 128-byte record and leaves this one
            // byte-identical, and a cutout toggle the reverse.
            if (uploadAll || std::memcmp(&alphaMaster[instIdx], &a, sizeof(a)) != 0)
            {
                alphaMaster[instIdx] = a;
                stageRecord(m_alphaCopyRanges,
                            reinterpret_cast<uint8_t*>(alphaDst),
                            reinterpret_cast<const uint8_t*>(alphaMaster),
                            instIdx, sizeof(RTAlphaRecord));
            }
        }
    }
    }
    if (recordJobs && verifyRecords) {
        expected.emplace(saveState()); restoreState(*before);
    }
    if (recordJobs) {
        struct Input { const Pending* pending; uint64_t blas; };
        static thread_local std::vector<Input> t_inputScratch;
        static thread_local std::vector<uint8_t> t_dirtyScratch;
        auto& inputs = t_inputScratch; auto& dirty = t_dirtyScratch;
        uint32_t count = 0, lanes = 1, chunks = 0;
        auto fillRange = [&](uint32_t begin, uint32_t end) {
            ZoneScopedN("DDGI.Records.Chunk");
            for (uint32_t idx = begin; idx < end; ++idx) {
                const auto record = prepareRecord(*inputs[idx].pending, inputs[idx].blas, idx);
                std::memcpy(&instances[idx], &record.instance, sizeof(record.instance));
                uint8_t flags = record.flags;
                if (uploadAll || std::memcmp(&matMaster[idx], &record.material, sizeof(record.material)) != 0) {
                    matMaster[idx] = record.material; flags |= 1u;
                }
                if (uploadAll || std::memcmp(&alphaMaster[idx], &record.alpha, sizeof(record.alpha)) != 0) {
                    alphaMaster[idx] = record.alpha; flags |= 2u;
                }
                dirty[idx] = flags;
            }
        };
        inputs.clear(); inputs.reserve(pending.size());
        // Cache mutation/build and validity checks finish before publication.
        if (matDst && alphaDst) {
            ZoneScopedN("DDGI.Records.Resolve");
            // Phase C follows all BLAS builds/compactions. Both maps and their
            // resource lifetimes are frozen until this owner call returns.
            // Jobs only query immutable resource VAs; no GPU commands, cache
            // mutation or COM ownership transfers occur here.
            const bool parallelResolve = resolveJobs && pending.size() >= 2048 &&
                TaskSystem::Get().GetWorkerCount() > 1;
            static thread_local std::vector<uint64_t> t_resolvedScratch;
            if (parallelResolve) {
                t_resolvedScratch.resize(pending.size());
                auto* addresses = t_resolvedScratch.data();
                const auto* sources = pending.data();
                const auto& staticCache = m_blasCache;
                const auto& skinnedCache = m_skinnedBlas;
                const uint32_t resolveLanes = static_cast<uint32_t>((std::min)(
                    size_t(TaskSystem::Get().GetWorkerCount()), pending.size() / 1024));
                TaskSystem::Get().ParallelFor(0, resolveLanes, [&](uint32_t lane) {
                    ZoneScopedN("DDGI.Resolve.LookupChunk");
                    const size_t lo = pending.size() * lane / resolveLanes;
                    const size_t hi = pending.size() * (lane + 1) / resolveLanes;
                    for (size_t i = lo; i < hi; ++i) {
                        const auto& p = sources[i];
                        uint64_t va = 0;
                        if (p.skinned) {
                            const auto it = skinnedCache.find(p.entity);
                            if (it != skinnedCache.end() && it->second.builtOnce) va = it->second.blas.GPUAddress();
                        } else {
                            const auto it = staticCache.find(p.key);
                            if (it != staticCache.end()) va = it->second.GPUAddress();
                        }
                        addresses[i] = va;
                    }
                });
            }
            ZoneNamedN(resolveCompactZone, "DDGI.Resolve.Compact", parallelResolve);
            for (size_t pendingIndex = 0; pendingIndex < pending.size(); ++pendingIndex) {
                const Pending& p = pending[pendingIndex];
                uint64_t va = 0;
                if (parallelResolve) va = t_resolvedScratch[pendingIndex];
                else if (p.skinned) {
                    const auto it = m_skinnedBlas.find(p.entity);
                    if (it != m_skinnedBlas.end() && it->second.builtOnce) va = it->second.blas.GPUAddress();
                } else {
                    va = lookupStaticVA(p.key);
                }
                if (va && inputs.size() < m_matCapacity && inputs.size() < m_alphaCapacity)
                    inputs.push_back({&p, va});
            }
            if (verifyRecords && resolveJobs)
                LOG_INFO("[AS_RESOLVE_JOBS] pending=%zu valid=%zu parallel=%u", pending.size(), inputs.size(), unsigned(parallelResolve));
        }
        instances.resize(inputs.size()); dirty.resize(inputs.size());
        count = static_cast<uint32_t>(inputs.size());
        const uint32_t workers = TaskSystem::Get().GetWorkerCount();
        const auto plan = Sys::CoarseTaskPlan::Make(count, workers);
        lanes = plan.lanes; chunks = plan.chunks;
        auto fill = [&](uint32_t chunk) {
            fillRange(plan.Begin(count, chunk), plan.End(count, chunk));
        };
        std::atomic<uint32_t> nextChunk{0};
        auto runLane = [&](uint32_t lane) {
            if (lanes == 1) { fill(lane); return; }
            // Only coarse chunk claims are atomic, never individual records.
            for (;;) {
                const uint32_t chunk = nextChunk.fetch_add(1, std::memory_order_relaxed);
                if (chunk >= plan.chunks) break;
                fill(chunk);
            }
        };
        Sys::CpuTaskScope scope(lanes - 1);
        for (uint32_t lane=1; lane<lanes; ++lane) scope.Submit([&,lane] { runLane(lane); });
        runLane(0); scope.Join();
        // All master updates are visible before owner gap-merging copies. Stable
        // instance order preserves InstanceID and byte-identical fingerprints.
        for (uint32_t idx=0; idx<count; ++idx) {
            const uint8_t flags = dirty[idx];
            windInstanceCount += (flags & 4u) != 0; alphaClipCount += (flags & 8u) != 0;
            if (flags & 1u) stageRecord(m_matCopyRanges, reinterpret_cast<uint8_t*>(matDst),
                reinterpret_cast<const uint8_t*>(matMaster), idx, sizeof(DDGIInstanceData));
            if (flags & 2u) stageRecord(m_alphaCopyRanges, reinterpret_cast<uint8_t*>(alphaDst),
                reinterpret_cast<const uint8_t*>(alphaMaster), idx, sizeof(RTAlphaRecord));
        }
        if (expected) {
            const auto actual = saveState();
            const auto equalBytes = [](const auto& a, const auto& b) {
                return a.size() == b.size() && (a.empty() || std::memcmp(a.data(),b.data(),a.size()*sizeof(a[0])) == 0);
            };
            const bool match = equalBytes(expected->instances,actual.instances)
                && expected->materialMaster==actual.materialMaster && expected->alphaMaster==actual.alphaMaster
                && expected->materialStaging==actual.materialStaging && expected->alphaStaging==actual.alphaStaging
                && equalBytes(expected->materialRanges,actual.materialRanges) && equalBytes(expected->alphaRanges,actual.alphaRanges)
                && expected->wind==actual.wind && expected->clip==actual.clip;
            ENGINE_CHECK(match, "scene-AS record graph differs from original serial loop");
            LOG_INFO("[AS_RECORD_ORACLE] instances=%u lanes=%u wind=%u alpha=%u match=%u chunks=%u adaptive=1", count,lanes,windInstanceCount,alphaClipCount,unsigned(match),chunks);
        }
    }
    m_alphaRecordCount = (uint32_t)instances.size();

    // ---- Flush this frame's dirty spans into the resident DEFAULT tables ----
    // Recorded on this function's command list (asCL, COMPUTE queue), ahead of the TLAS build and so
    // of every consumer: ddgiCL waits on asCL, and the graph's RTR/RTShadow/RTAO passes wait on
    // promoteCL <- ddgiCL.
    // This must stay before the fingerprint gate below: that gate may return early reusing last
    // frame's TLAS, but it only proves the instance descs unchanged - a material edit changes the
    // records with identical transforms and BLAS VAs, and skipping the copies would strand it on
    // the CPU.
    // recordsRewritten: whether any instance record was (re)written this frame, for the GI emitter
    // table's reuse test (the spans are cleared here).
    const bool recordsRewritten = uploadAll || !m_matCopyRanges.empty();
    {
        const bool haveMat   = m_matBufferGPU.IsValid()   && !m_matCopyRanges.empty();
        const bool haveAlpha = m_alphaBufferGPU.IsValid() && !m_alphaCopyRanges.empty();
        if ((haveMat || haveAlpha) && cmd.IsValid())
        {
            uint64_t copiedBytes = 0;
            if (haveMat)
                for (const RecordCopyRange& r : m_matCopyRanges)
                {
                    const uint64_t off   = uint64_t(r.first) * sizeof(DDGIInstanceData);
                    const uint64_t bytes = uint64_t(r.count) * sizeof(DDGIInstanceData);
                    gfx.CopyBufferRegion(m_matBuffer[frameSlot], off,
                                         m_matBufferGPU,         off, bytes, cmd);
                    copiedBytes += bytes;
                }
            if (haveAlpha)
                for (const RecordCopyRange& r : m_alphaCopyRanges)
                {
                    const uint64_t off   = uint64_t(r.first) * sizeof(RTAlphaRecord);
                    const uint64_t bytes = uint64_t(r.count) * sizeof(RTAlphaRecord);
                    gfx.CopyBufferRegion(m_alphaBuffer[frameSlot], off,
                                         m_alphaBufferGPU,         off, bytes, cmd);
                    copiedBytes += bytes;
                }

            // Both tables decayed to COMMON at the last ECL boundary and the copy implicitly promoted
            // them to COPY_DEST; put them back into the shader-read state before the traces.
            // SHADER_RESOURCE_COMPUTE (bare NON_PIXEL_SHADER_RESOURCE) is mandatory, not a tightening:
            // on this compute-queue list ResourceState::SHADER_RESOURCE widens to PIXEL|NON_PIXEL, a
            // pixel-shader state is illegal there and the debug layer removes the device. Every
            // consumer is a compute dispatch or DispatchRays. Never OR two states together (the
            // translator switches on one value; an OR falls through to COMMON).
            RHI::GPUBarrier post[2];
            uint32_t n = 0;
            if (haveMat)
                post[n++] = RHI::GPUBarrier::Buffer(&m_matBufferGPU,
                                RHI::ResourceState::COPY_DST,
                                RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
            if (haveAlpha)
                post[n++] = RHI::GPUBarrier::Buffer(&m_alphaBufferGPU,
                                RHI::ResourceState::COPY_DST,
                                RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
            gfx.PushBarriers(post, n, cmd);

            // High-water only: this must never log per frame.
            const uint32_t copiedKB = (uint32_t)(copiedBytes / 1024ull);
            if (copiedKB > m_recordCopyPeak)
            {
                m_recordCopyPeak = copiedKB;
                LOG_INFO("DDGISceneAS: record dirty-upload high-water %u KB in "
                         "%zu+%zu spans", copiedKB, m_matCopyRanges.size(),
                         m_alphaCopyRanges.size());
            }
            // The masters now describe the DEFAULT tables byte-for-byte.
            m_recordUploadAll = false;
        }
        else if (haveMat || haveAlpha)
        {
            // No RHI handle => no way to record the copy (CopyBufferRegion and
            // PushBarriers are RHI::CommandList-only). Keep the staged bytes
            // from being silently lost: re-arm the full upload so the next
            // build with a real handle republishes every record.
            m_recordUploadAll = true;
            (void)ENGINE_CHECK_ONCE(false,
                "DDGISceneAS: BuildOrRefit called without an RHI command list — "
                "per-instance record tables cannot be uploaded");
        }
        // No else: zero spans means the resident tables already hold this frame's content (the
        // static-scene case: zero copies, zero barriers). m_recordUploadAll is deliberately not cleared
        // there: a frame that published no instances must not consume the flag.
        m_matCopyRanges.clear();
        m_alphaCopyRanges.clear();
    }

    // Throttled alpha-cutout telemetry: a content regression (a whole scene opting into non-opaque
    // traversal, e.g. migrated .imat with alphaRef just under 1.0) is otherwise only visible in
    // RenderDoc.
    if (alphaClipCount)
    {
        static uint32_t s_alphaLogCounter = 0;
        if ((++s_alphaLogCounter % 60u) == 0)
            LOG_DIAG(ENGINE_LOG_SCENE_AS,
                     "DDGISceneAS: %u/%zu instances are non-opaque "
                     "(alpha-clip + blend, FORCE_NON_OPAQUE)",
                     alphaClipCount, instances.size());
    }

    // ---- -tlasdigest[=N]: TLAS + record digest every N frames (default 20) ----
    // What the RT passes read, in instance order: each TLAS instance's transform / id / mask / hit
    // group / flags / triangle count (not the BLAS address, an allocation detail) plus its
    // DDGIInstanceData and RTAlphaRecord. The -brslaneverify emit digests do not cover DDGISceneAS;
    // this is the oracle that proves the late proxy binding byte-identical to -ddgibindfused.
    {
        static const int s_digestEvery = [] {
            const auto& cl = Sys::CommandLine::Get();
            if (!cl.Has("-tlasdigest")) return 0;
            const int n = cl.GetInt("-tlasdigest", 20);
            return n > 0 ? n : 20;
        }();
        static uint64_t s_digestFrames = 0;
        if (s_digestEvery > 0 && matMaster && ++s_digestFrames % static_cast<uint64_t>(s_digestEvery) == 0)
        {
            uint64_t h = 1469598103934665603ull;
            auto mix = [&h](const void* d, size_t n) {
                const auto* b = static_cast<const uint8_t*>(d);
                for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
            };
            for (const SceneASInstance& inst : instances)
            {
                mix(inst.transform, sizeof(inst.transform));
                mix(&inst.instanceID, sizeof(inst.instanceID));
                mix(&inst.instanceMask, sizeof(inst.instanceMask));
                mix(&inst.hitGroupIndex, sizeof(inst.hitGroupIndex));
                mix(&inst.flags, sizeof(inst.flags));
                mix(&inst.triangleCount, sizeof(inst.triangleCount));
                mix(&matMaster[inst.instanceID], sizeof(DDGIInstanceData));
                if (alphaMaster) mix(&alphaMaster[inst.instanceID], sizeof(RTAlphaRecord));
            }
            LOG_INFO("[TLAS_DIGEST] frame=%llu instances=%zu fnv=%016llx fused=%d epochRebinds=%llu",
                     static_cast<unsigned long long>(s_digestFrames), instances.size(),
                     static_cast<unsigned long long>(h),
                     Sys::CommandLine::Get().Has("-ddgibindfused") ? 1 : 0,
                     static_cast<unsigned long long>(g_ddgiEpochRebinds.load(std::memory_order_relaxed)));
        }
    }

    // ---- ReSTIR GI emitter table ----
    // The flagged instances in InstanceID order, from the records just written
    // (matMaster holds every published instance's current record).
    if (m_giEmittersWanted && frameSlot < kFrameCount && matMaster)
    {
        ZoneScopedN("DDGI.GIEmitters");
        auto& bytes = m_giEmitterScratch;
        // The table is a pure function of the published instances (transform, InstanceID, triangle
        // count) and their records. On a frame that provably changed neither - a patch frame with no
        // transform dirt, dissolve, wind or skinned rows, no record rewritten, every static BLAS
        // resolved, the same instance count - last frame's table stands; only a stale UPLOAD slot is
        // copied. -gi_emitters_every_frame rebuilds it every frame.
        const bool emittersUnchanged = m_giEmitterCacheValid && !recordsRewritten &&
            GatherRead().usePatch && GatherRead().dirtyXformEmpty && !GatherRead().anyDissolve &&
            skinnedCount == 0 && windInstanceCount == 0 && m_staticBlasAllResolved &&
            instances.size() == m_giEmitterCacheInstances &&
            !Sys::CommandLine::Get().Has("-gi_emitters_every_frame");
        if (!emittersUnchanged)
        {
        bytes.clear();
        for (const SceneASInstance& inst : instances)
        {
            const DDGIInstanceData& d = matMaster[inst.instanceID];
            if (!(d.rtFlags & kRTInstFlagGIEmitter) || inst.triangleCount == 0) continue;
            GIEmitterGPU e{};
            std::memcpy(e.objectToWorld, inst.transform, sizeof(e.objectToWorld));
            const XMMATRIX m(
                inst.transform[0][0], inst.transform[0][1], inst.transform[0][2], inst.transform[0][3],
                inst.transform[1][0], inst.transform[1][1], inst.transform[1][2], inst.transform[1][3],
                inst.transform[2][0], inst.transform[2][1], inst.transform[2][2], inst.transform[2][3],
                0.0f, 0.0f, 0.0f, 1.0f);
            XMVECTOR det;
            const XMMATRIX inv = XMMatrixInverse(&det, m);
            const float dv = XMVectorGetX(det);
            if (!(std::abs(dv) > 1e-24f) || !std::isfinite(dv)) continue; // degenerate: never hit
            XMFLOAT4X4 invF;
            XMStoreFloat4x4(&invF, inv);
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c)
                    e.worldToObject[r][c] = invF.m[r][c];
            e.instanceID    = inst.instanceID;
            e.triangleCount = inst.triangleCount;
            e.radiance      = GIEmitterRadiance(d.emissive, d.baseColor);
            const size_t at = bytes.size();
            bytes.resize(at + sizeof(e));
            std::memcpy(bytes.data() + at, &e, sizeof(e));
        }
        ++m_giEmitterGeneration;
        m_giEmitterCacheValid = true;
        m_giEmitterCacheInstances = instances.size();
        }
        const uint32_t n = static_cast<uint32_t>(bytes.size() / sizeof(GIEmitterGPU));
        if (n && (m_giEmitterCapacity[frameSlot] < n || !m_giEmitterMapped[frameSlot]))
        {
            // The frame fence has retired this slot (same rule as the path
            // materials above), so its pair can be replaced directly.
            if (m_giEmitterMapped[frameSlot]) gfx.UnmapBuffer(m_giEmitters[frameSlot]);
            if (m_giEmitters[frameSlot].IsValid()) gfx.DestroyBuffer(m_giEmitters[frameSlot]);
            if (m_giEmitterTable[frameSlot].IsValid()) gfx.DestroyBuffer(m_giEmitterTable[frameSlot]);
            m_giEmitterMapped[frameSlot] = nullptr; m_giEmitterCapacity[frameSlot] = 0;
            const uint32_t capacity = (std::max)(64u, n + n / 2);
            RHI::GPUBufferDesc desc{};
            desc.size = uint64_t(capacity) * sizeof(GIEmitterGPU);
            desc.stride = sizeof(GIEmitterGPU);
            desc.usage = RHI::Usage::UPLOAD;
            desc.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
            desc.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
            RHI::GPUBufferDesc table{};
            table.size = uint64_t(capacity) * 32u; // two float4 rows per emitter
            table.stride = 16u;
            table.usage = RHI::Usage::DEFAULT;
            table.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
            table.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
            if (gfx.CreateBuffer(desc, m_giEmitters[frameSlot]) &&
                gfx.CreateBuffer(table, m_giEmitterTable[frameSlot]))
            {
                m_giEmitterMapped[frameSlot] = gfx.MapBuffer(m_giEmitters[frameSlot]);
                if (m_giEmitterMapped[frameSlot]) m_giEmitterCapacity[frameSlot] = capacity;
            }
            else LOG_ERROR("DDGISceneAS: GI emitter table allocation failed (%u emitters)", n);
            m_giEmitterSlotGeneration[frameSlot] = 0; // the new buffer holds nothing yet
        }
        if (n && m_giEmitterMapped[frameSlot] && m_giEmitterCapacity[frameSlot] >= n)
        {
            if (m_giEmitterSlotGeneration[frameSlot] != m_giEmitterGeneration)
            {
                std::memcpy(m_giEmitterMapped[frameSlot], bytes.data(), bytes.size());
                m_giEmitterSlotGeneration[frameSlot] = m_giEmitterGeneration;
            }
            m_giEmitterCount = n;
        }
    }

    if (instances.empty()) { asEnd(tsTlas); return 0; }

    const uint32_t newCount = (uint32_t)instances.size();

    // ---- Dirty check: skip the TLAS write + build when nothing changed ----
    // Fingerprint (FNV-1a) the value-initialised instance descriptors (transforms + BLAS VAs). If it
    // matches last frame, no BLAS swapped and the TLAS is fresh, the existing TLAS is bit-identical to
    // a rebuild -> reuse it. The TLAS persists, so the trace keeps reading a valid structure; camera
    // motion never touches the world-space TLAS, so a static scene costs zero TLAS work.
    // Skinned BLAS change in place: identical instance bytes do not imply an identical TLAS (a
    // deformed BLAS's AABBs moved), so any skinned instance disables the skip - keep this half of the
    // guard. It is hoisted above the hash because with a skinned instance both consumers below
    // short-circuit on dynamicPresent, and the hash (~0.065 ms) would be computed for nobody.
    const bool dynamicPresent = skinnedCount > 0;

    // Static-scene fast path: when this frame provably wrote the same instance bytes as last frame,
    // reuse last frame's hash instead of re-walking ~1 MB of upload-heap memory. Provable: a patch
    // frame (same static membership and order - its only adds are skinned, and skinned rows set
    // dynamicPresent; no material/alpha change by usePatch's terms), zero transform dirt, no
    // dissolve animation, every static BLAS VA resolved now and last frame (an async build landing
    // changes bytes), and no wind (wind bytes change by design).
    const bool bytesProvablyUnchanged =
        GatherRead().usePatch && m_lastHashValid &&
        m_staticBlasAllResolved && m_staticBlasResolvedPrev &&
        windInstanceCount == 0 && !GatherRead().anyDissolve &&
        GatherRead().dirtyXformEmpty;
    if (m_staticBlasAllResolved && !m_staticBlasResolvedPrev)
    {
        const uint32_t epoch = m_staticGeometryEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
        LOG_INFO("[DDGI_AS] static geometry complete epoch=%u instances=%zu", epoch, instances.size());
    }
    m_staticBlasResolvedPrev = m_staticBlasAllResolved;

    const bool needFingerprint = !dynamicPresent && !bytesProvablyUnchanged;
    // ContentFingerprint with skinned content: the instance bytes are hashed for the path tracer
    // alone (the TLAS skip below still ignores them), only while every skinned row was hashed.
    const bool contentWithSkinned = dynamicPresent && GatherRead().deformHashed;

    uint64_t fp = 1469598103934665603ull; // FNV-1a offset basis
    if (!dynamicPresent && bytesProvablyUnchanged)
        fp = m_lastInstanceHash;          // verbatim reuse -> the TLAS gate sees "unchanged"
    if (needFingerprint || contentWithSkinned)
    {
        ZoneScopedN("DDGI.Fingerprint");
        // 64-bit-word FNV-1a in four interleaved lanes, folded at the end: same constants, 8 bytes per
        // step, four independent xor->imul chains so the multiplier stays pipelined (one ~64K-step
        // serial chain ran at imul latency). The value is only compared with this function's own output
        // last frame, so changing the mixing costs at most one extra full build. memcpy keeps the word
        // loads alignment-safe.
        const auto*  bytes = reinterpret_cast<const uint8_t*>(instances.data());
        const size_t nb    = static_cast<size_t>(newCount) * sizeof(SceneASInstance);
        uint64_t h0 = fp, h1 = 0x9E3779B97F4A7C15ull,
                 h2 = 0xC2B2AE3D27D4EB4Full, h3 = 0x165667B19E3779F9ull;
        size_t i = 0;
        for (; i + 32 <= nb; i += 32)
        {
            uint64_t w0, w1, w2, w3;
            std::memcpy(&w0, bytes + i,      sizeof(w0));
            std::memcpy(&w1, bytes + i + 8,  sizeof(w1));
            std::memcpy(&w2, bytes + i + 16, sizeof(w2));
            std::memcpy(&w3, bytes + i + 24, sizeof(w3));
            h0 ^= w0; h0 *= 1099511628211ull;
            h1 ^= w1; h1 *= 1099511628211ull;
            h2 ^= w2; h2 *= 1099511628211ull;
            h3 ^= w3; h3 *= 1099511628211ull;
        }
        fp = h0 ^ (h1 + 0x9E3779B97F4A7C15ull + (h0 << 6) + (h0 >> 2));
        fp ^= h2 + 0x9E3779B97F4A7C15ull + (fp << 6) + (fp >> 2);
        fp ^= h3 + 0x9E3779B97F4A7C15ull + (fp << 6) + (fp >> 2);
        for (; i + 8 <= nb; i += 8)
        {
            uint64_t w;
            std::memcpy(&w, bytes + i, sizeof(w));
            fp ^= w; fp *= 1099511628211ull;
        }
        for (; i < nb; ++i) { fp ^= bytes[i]; fp *= 1099511628211ull; }
    }

    // ---- Diagnostic: periodic AS state dump ----
    // Whether a previous scene's TLAS/BLAS were freed cannot be read off the code: the BLAS cache
    // survives a world reload by design (OnWorldClear), and PruneStaleBLAS frees only entries whose
    // MeshLibrary slot died, one world late (two-stage meshlib release). A leak shows as a step in
    // blasCache / instances that never comes down; correct reuse keeps blasCache flat. Every 120th
    // build (~2 s): the log is append-only.
    {
        static uint32_t s_asLogTick = 0;
        if ((++s_asLogTick % 120u) == 0)
        {
#if DDGI_GT_DIAGNOSTICS
            LOG_DIAG(ENGINE_LOG_SCENE_AS,
                     "SceneAS: instances=%u (skinned=%u) blasCache=%zu skinnedBlas=%zu "
                     "tlasCap=%u tlasFresh=%d blasSwapped=%d bindlessInvalid=%u emissiveTex=%u "
                     "emissiveDistinct=%zu",
                     newCount, skinnedCount, m_blasCache.size(), m_skinnedBlas.size(),
                     m_tlasCapacity, m_tlasFresh ? 1 : 0, blasSwapped ? 1 : 0,
                     m_bindlessInvalidCount, m_emissiveTexCount, m_emissiveTexDistinct.size());
#else
            LOG_DIAG(ENGINE_LOG_SCENE_AS,
                     "SceneAS: instances=%u (skinned=%u) blasCache=%zu skinnedBlas=%zu "
                     "tlasCap=%u tlasFresh=%d blasSwapped=%d",
                     newCount, skinnedCount, m_blasCache.size(), m_skinnedBlas.size(),
                     m_tlasCapacity, m_tlasFresh ? 1 : 0, blasSwapped ? 1 : 0);
#endif
        }
    }

    uint64_t deform = 0;
    bool blasLags = false;   // a skinned BLAS in the TLAS still holds an earlier pose (the build budget)
    if (contentWithSkinned)
        for (const Pending& p : pending)
        {
            if (!p.skinned) continue;
            deform = Sys::WordHashCombine(deform, p.deformHash);
            const auto it = m_skinnedBlas.find(p.entity);
            if (it != m_skinnedBlas.end() && it->second.builtOnce && it->second.builtDeform != p.deformHash)
                blasLags = true;
        }
    m_contentHash = contentWithSkinned ? Sys::WordHashCombine(fp, deform) : fp;
    m_contentHashValid = !dynamicPresent || (contentWithSkinned && !blasLags);
    // -pt_stability_log: which half of ContentFingerprint moved - the instance descs (a transform,
    // foliage wind's shear, a BLAS swap) or a skinned pose.
    static const bool s_stabilityLog = Sys::CommandLine::Get().Has("-pt_stability_log");
    if (s_stabilityLog && m_contentHashValid)
    {
        static uint64_t s_prevInstances = 0, s_prevDeform = 0;
        if (fp != s_prevInstances || deform != s_prevDeform)
            LOG_INFO("[PT_CONTENT] instances=%u deform=%u wind=%u skinned=%u",
                     fp != s_prevInstances ? 1u : 0u, deform != s_prevDeform ? 1u : 0u, windInstanceCount, skinnedCount);
        s_prevInstances = fp; s_prevDeform = deform;
    }

    if (m_tlasFresh && !blasSwapped && !dynamicPresent &&
        newCount == m_lastInstanceCount &&
        m_lastHashValid && fp == m_lastInstanceHash)
    {
        asEnd(tsTlas);
        return m_tlas.GPUAddress(); // unchanged - reuse the existing TLAS
    }

    // PERFORM_UPDATE requires the same instance count and the same BLAS pointers; otherwise DXR reads
    // garbage and hangs the GPU. A compaction swap changes a BLAS VA, so a swap frame always
    // rebuilds.
    // Skinned content (dynamicPresent) may refit: the DXR spec requires a TLAS over a rebuilt or
    // updated BLAS to be rebuilt OR updated before tracing, and PERFORM_UPDATE suffices. Deforming
    // content is what refits are for, and its builds run every frame, so the quality-rebuild
    // interval below engages.
    // Otherwise fp equality is required: a warm-cache world builds its first TLAS on the load frame
    // from not-yet-propagated transforms, and the next frame's corrected content arrives with the
    // same count. A refit would keep the stale tree topology (correct hits, ~10x traversal cost),
    // and the fingerprint skip would freeze it since no build runs again. Refit quality degrades
    // over time, hence the periodic full rebuild.
    const bool canUpdateBase = m_tlasFresh && (newCount == m_lastInstanceCount) &&
                               !blasSwapped &&
                               (dynamicPresent ||
                                (m_lastHashValid && fp == m_lastInstanceHash));
    constexpr uint32_t kTLASRebuildInterval = 64;   // consecutive refits before a quality rebuild
    if (canUpdateBase) ++m_tlasRefitCount; else m_tlasRefitCount = 0;
    const bool qualityRebuildDue = m_tlasRefitCount > kTLASRebuildInterval;
    if (qualityRebuildDue) m_tlasRefitCount = 0;
    const bool canUpdate = canUpdateBase && !qualityRebuildDue;

    // Slot-rotated staging: skinned presence forces this write every frame, and the previous
    // frame's compute-queue TLAS build may still read its own slot (SceneTLAS::kInstanceUploadSlots).
    WriteTLASInstances(gfx, instances.data(), newCount, frameSlot);
    const RHI::GPUBuffer& instanceSlot = m_tlas.instances[m_tlas.uploadSlot];
    if (m_scratch.IsValid() && instanceSlot.IsValid())
    {
        gfx.BuildTLAS(cmd, instanceSlot, m_tlas.instanceCount,
                      canUpdate ? kTLASFlags | RHI::ASBuildFlags::PERFORM_UPDATE : kTLASFlags,
                      m_tlas.as, m_scratch);
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_tlas.as), cmd);
    }
    m_tlasFresh         = true;
    m_lastInstanceCount = newCount;
    // With skinned content fp is the untouched FNV basis, or a hash made for ContentFingerprint
    // alone; either way the TLAS skip must not trust it: the first frame after the last skinned
    // instance leaves does one extra full rebuild.
    m_lastInstanceHash  = fp;
    m_lastHashValid     = needFingerprint;

    asEnd(tsTlas);
    return m_tlas.GPUAddress();
}

// ==== Per-frame accessors ====

// GetInstanceBuffer / GetInstanceSrv / GetAlphaRecordVA / GetAlphaRecordSrv return the resident
// DEFAULT tables, never this frame's staging slot.
const RHI::GPUBuffer* DDGISceneAS::GetInstanceBuffer(IGraphicsDevice& gfx) const
{
    (void)gfx;
    return m_matBufferGPU.IsValid() ? &m_matBufferGPU : nullptr;
}

void DDGISceneAS::ReleasePathMaterials(IGraphicsDevice& gfx)
{
    m_pathMaterialsReady = false;
    m_pathMaterialHash = 0;
    for (uint32_t i = 0; i < kFrameCount; ++i) { m_pathMatSlotRevision[i] = 0; m_pathMatSlotRow[i].clear(); }
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (m_pathMaterialMapped[i]) gfx.UnmapBuffer(m_pathMaterials[i]);
        if (m_pathMaterials[i].IsValid()) gfx.DestroyBuffer(m_pathMaterials[i]);
        m_pathMaterialMapped[i] = nullptr; m_pathMaterialCapacity[i] = 0;
    }
}

void DDGISceneAS::ReleaseGIEmitters(IGraphicsDevice& gfx)
{
    m_giEmitterCount = 0;
    m_giEmitterCacheValid = false;
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (m_giEmitterMapped[i]) gfx.UnmapBuffer(m_giEmitters[i]);
        if (m_giEmitters[i].IsValid()) gfx.DestroyBuffer(m_giEmitters[i]);
        if (m_giEmitterTable[i].IsValid()) gfx.DestroyBuffer(m_giEmitterTable[i]);
        m_giEmitters[i] = {}; m_giEmitterTable[i] = {};
        m_giEmitterMapped[i] = nullptr; m_giEmitterCapacity[i] = 0;
    }
}

uint64_t DDGISceneAS::GetGIEmitterSrv(IGraphicsDevice& gfx) const
{
    const uint32_t slot = gfx.GetFrameIndex();
    return m_giEmitterCount && slot < kFrameCount ? gfx.GetBufferSRVGpuHandle(m_giEmitters[slot]) : 0;
}

uint64_t DDGISceneAS::GetGIEmitterTableUav(IGraphicsDevice& gfx) const
{
    const uint32_t slot = gfx.GetFrameIndex();
    return m_giEmitterCount && slot < kFrameCount ? gfx.GetBufferUAVGpuHandle(m_giEmitterTable[slot]) : 0;
}

uint64_t DDGISceneAS::ShadingFingerprint() const
{
    const size_t count = (std::min)({ size_t(m_lastInstanceCount), m_matMaster.size() / sizeof(DDGIInstanceData),
                                      m_alphaMaster.size() / sizeof(RTAlphaRecord) });
    const auto* rows = reinterpret_cast<const DDGIInstanceData*>(m_matMaster.data());
    const auto* alphas = reinterpret_cast<const RTAlphaRecord*>(m_alphaMaster.data());
    // Runs of non-skinned rows hash in place; a skinned row hashes a copy without the fields that
    // name this frame's SkinnedVertexRing buffer and slice.
    uint64_t h = Sys::WordHash64(nullptr, 0);
    size_t runStart = 0;
    auto flushRun = [&](size_t end)
    {
        if (end <= runStart) return;
        h = Sys::WordHashCombine(h, Sys::WordHash64(rows + runStart, (end - runStart) * sizeof(DDGIInstanceData)));
        h = Sys::WordHashCombine(h, Sys::WordHash64(alphas + runStart, (end - runStart) * sizeof(RTAlphaRecord)));
    };
    for (size_t i = 0; i < count; ++i)
    {
        if (!(rows[i].rtFlags & kRTInstFlagSkinned)) continue;
        flushRun(i);
        DDGIInstanceData row = rows[i];
        row.vbBindlessIdx = row.normalBufIdx = row.vertexStart = row.nrmStart = 0;
        RTAlphaRecord alpha = alphas[i];
        alpha.vbBindlessIdx = 0;
        h = Sys::WordHashCombine(h, Sys::WordHash64(&row, sizeof(row)));
        h = Sys::WordHashCombine(h, Sys::WordHash64(&alpha, sizeof(alpha)));
        runStart = i + 1;
    }
    flushRun(count);
    return h;
}

uint64_t DDGISceneAS::GetPathMaterialSrv(IGraphicsDevice& gfx) const
{
    return m_pathMaterialsReady ? gfx.GetBufferSRVGpuHandle(m_pathMaterials[gfx.GetFrameIndex()]) : 0;
}

uint64_t DDGISceneAS::GetInstanceSrv(IGraphicsDevice& gfx) const
{
    (void)gfx;
    return m_matSrv;
}

uint64_t DDGISceneAS::GetAlphaRecordVA(IGraphicsDevice& gfx) const
{
    if (!m_alphaBufferGPU.IsValid()) return 0;
    return BufferGPUVA(gfx, m_alphaBufferGPU);
}

uint64_t DDGISceneAS::GetAlphaRecordSrv(IGraphicsDevice& gfx) const
{
    (void)gfx;
    return m_alphaSrv;
}
