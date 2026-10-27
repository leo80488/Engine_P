#include "RenderGraph/RenderPass/DDGIPass.h"
#include "Graphics/DDGIVolumeManager.h"
#include "Graphics/GraphicsDX12.h"
#include "Graphics/PipelineLayoutBuilder.h"
#include "Graphics/MeshDescriptorHeap.h"   // kMaxBuffers: the root signature's bindless table size
#include "Graphics/DxcCompiler.h"
#include "Graphics/ShaderDiskCache.h"
#include "System/Log.h"

#include <vector>
#include <string>
#include <iterator>   // std::size: pins kDDGIStepNames against DDGI::kMaxVolumes

namespace
{
// The DDGI root signature's parameters, named and typed here (the register map is in
// DDGIPass.h).
constexpr RHI::Slots::CmpParam kCB              { 0 };    // b0 root CBV: the volume CB
constexpr RHI::Slots::CmpParam kTLAS            { 1 };    // t0 root SRV: the TLAS
constexpr RHI::Slots::CmpParam kSkyIBL          { 2 };    // t1: the sky IBL cube
constexpr RHI::Slots::CmpParam kOutputUav       { 3 };    // u0: this dispatch's output (ray data, an atlas, probe data)
constexpr RHI::Slots::CmpParam kRayDataSrv      { 4 };    // t2: the ray data
constexpr RHI::Slots::CmpParam kInstanceData    { 5 };    // t3 root SRV: the per-instance data
constexpr RHI::Slots::CmpParam kBindlessBuffers { 6 };    // t0.. space1: the bindless buffers
constexpr RHI::Slots::CmpParam kVarianceUav     { 7 };    // u1: the variance buffer (irradiance relight)
constexpr RHI::Slots::CmpParam kIrradianceSrv   { 8 };    // t4: the irradiance atlas (multi-bounce)
constexpr RHI::Slots::CmpParam kDepthSrv        { 9 };    // t5: the depth atlas (multi-bounce)
constexpr RHI::Slots::CmpParam kProbeDataSrv    { 10 };   // t6: the probe data
constexpr RHI::Slots::CmpParam kLightsSrv       { 11 };   // t7: the cluster's GPULight buffer
constexpr RHI::Slots::CmpParam kRayCountUav     { 12 };   // u2: the per-probe ray count
constexpr RHI::Slots::CmpParam kRayAllocUav     { 13 };   // u3: the ray allocation
constexpr RHI::Slots::CmpParam kDispatchArgsUav { 14 };   // u4: the dispatch args
constexpr RHI::Slots::CmpParam kBindlessTextures{ 15 };   // t0.. space2: the bindless textures
constexpr uint32_t kRootParamCount = 16;

// ==== Per-slot GPU timestamp labels ====
// The labels carry the volume slot, so a multi-volume scene can tell which volume cost what.
// They MUST be static string literals: GPUProfiler stores the `const char*` it is given
// (regionNames[]) and the Tracy zone holds the pointer until it resolves, so a per-call
// snprintf buffer would dangle.
enum DDGIStep : uint32_t
{
    kStepPrepare = 0,
    kStepRayAlloc,
    kStepFinalize,
    kStepTrace,
    kStepRelocate,
    kStepRelightIrr,
    kStepRelightDep,
    kStepBorder,
    kStepCount
};

#define DDGI_STEP_ROW(v)                 \
    { "DDGI.V" #v ".PrepareRayCount",    \
      "DDGI.V" #v ".RayAllocation",      \
      "DDGI.V" #v ".FinalizeIndirect",   \
      "DDGI.V" #v ".Trace",              \
      "DDGI.V" #v ".Relocate",           \
      "DDGI.V" #v ".Relight.Irr",        \
      "DDGI.V" #v ".Relight.Dep",        \
      "DDGI.V" #v ".Border" }

const char* const kDDGIStepNames[][kStepCount] = {
    DDGI_STEP_ROW(0), DDGI_STEP_ROW(1), DDGI_STEP_ROW(2), DDGI_STEP_ROW(3),
    DDGI_STEP_ROW(4), DDGI_STEP_ROW(5), DDGI_STEP_ROW(6), DDGI_STEP_ROW(7),
};
#undef DDGI_STEP_ROW

// Raising DDGI::kMaxVolumes must add rows here, or Execute indexes past the end of this
// table.
static_assert(std::size(kDDGIStepNames) == DDGI::kMaxVolumes,
              "kDDGIStepNames needs one row per DDGI volume slot");

// Shader compiles go through the shared disk cache (Graphics/ShaderDiskCache.h):
// ShaderLibrary's .ishdr cache in shaders/shader_cache_dxil/, keyed on path + entry + stage +
// defines + compile flags and invalidated by the mtime of the source or ANY transitive
// #include. An uncached compile would pay a full DXC run on every boot.
std::vector<uint8_t> CompileShaderFile(const std::string& path,
                                       RHI::ShaderStage stage,
                                       const std::vector<DxcCompiler::Define>& defines = {},
                                       const std::string& entry = "main")
{
    // The defines are part of the cache key: DDGIRelight.cs.hlsl and DDGIBorderUpdate.cs.hlsl
    // are each compiled TWICE from one source, differing only in defines, and a key without them
    // would let the variants overwrite each other's .ishdr (the irradiance pass would get the
    // depth shader). See ShaderDiskCache::HashDefines.
    return ShaderDiskCache::LoadOrCompile(path, entry, stage, defines, "DDGIPass");
}
} // namespace

// ==== Init / Shutdown ====

bool DDGIPass::Init(IGraphicsDevice& gfx)
{
    if (!gfx.GetCapabilities().inlineRaytracing)
    {
        LOG_INFO("DDGIPass: device does not support inline RayQuery (DXR 1.1) — pass disabled");
        m_ready = false;
        return false;
    }

    if (!BuildRootSignature(gfx))    return false;
    if (!CreateCSPipelines(gfx))     return false;
    m_ready = true;
    LOG_INFO("DDGIPass: initialised (inline RayQuery CS path)");
    return true;
}

void DDGIPass::DestroyPipelines(IGraphicsDevice& gfx)
{
    for (RHI::PipelineState* pso : { &m_tracePSO, &m_prepareRayCountPSO, &m_rayAllocationPSO,
                                     &m_finalizeIndirectPSO, &m_relocatePSO, &m_relightIrradiancePSO,
                                     &m_relightDepthPSO, &m_borderIrradiancePSO, &m_borderDepthPSO })
        gfx.DestroyPipelineState(*pso);
}

void DDGIPass::Shutdown(IGraphicsDevice& gfx)
{
    DestroyPipelines(gfx);
    gfx.DestroyPipelineLayout(m_layout);
    m_ready = false;
}

bool DDGIPass::ReloadPipelines(IGraphicsDevice& gfx)
{
    if (!m_ready) return false;
    DestroyPipelines(gfx);
    if (!CreateCSPipelines(gfx))  return false;
    return true;
}

// ==== Root signature ====

bool DDGIPass::BuildRootSignature(IGraphicsDevice& gfx)
{
    // The layout (DDGI-only; the register map is DDGIPass.h's):
    RHI::PipelineLayoutBuilder b("DDGI");
    b.RootCBV(kCB, 0)                                      // b0: the volume CB
     .RootSRV(kTLAS, 0)                                    // t0: the TLAS
     .Table(kSkyIBL,     { RHI::SrvRange(1) })             // t1: the sky IBL cube
     .Table(kOutputUav,  { RHI::UavRange(0) })             // u0: atlas / ray data / probe data
     .Table(kRayDataSrv, { RHI::SrvRange(2) })             // t2: the ray data
     // Per-instance data (StructuredBuffer<DDGIInstanceData>) by raw GPU VA.
     .RootSRV(kInstanceData, 3)                            // t3
     // Bindless ByteAddressBuffer table - the engine's convention. Closest-hit
     // indexes it via DDGIInstanceData::vbBindlessIdx / ibBindlessIdx to fetch
     // positions for the geometric face normal; the shader sees this size as
     // g_DDGIBuffers[N] (DDGIRayTrace.cs.hlsl).
     .Table(kBindlessBuffers, { RHI::SrvRange(0, MeshDescriptorHeap::kMaxBuffers, 1) })
     // u1: the variance buffer - only the irradiance relight reads it; the
     // other dispatches keep the binding.
     .Table(kVarianceUav, { RHI::UavRange(1) })
     // t4..t6: the multi-bounce SRVs (irradiance atlas, depth atlas, probe
     // data), read by the trace; t6 also by PrepareRayCount.
     .Table(kIrradianceSrv, { RHI::SrvRange(4) })
     .Table(kDepthSrv,      { RHI::SrvRange(5) })
     .Table(kProbeDataSrv,  { RHI::SrvRange(6) })
     .Table(kLightsSrv,     { RHI::SrvRange(7) })      // t7: the cluster's GPULight buffer
     // u2..u4: the adaptive ray dispatch - ray count, ray allocation, dispatch args.
     .Table(kRayCountUav,     { RHI::UavRange(2) })
     .Table(kRayAllocUav,     { RHI::UavRange(3) })
     .Table(kDispatchArgsUav, { RHI::UavRange(4) })
     // The engine-wide bindless textures (g_AllTextures[], t0 space2): emissive
     // at the closest hit.
     .Table(kBindlessTextures, { RHI::SrvRange(0, GraphicsDX12::kMaxBindlessTextures, 2) });
    // s0 - linear WRAP. Keep MaxLOD at FLT_MAX (SamplerDesc's default): a zero MaxLOD also
    // clamps SampleLevel, so DDGIRayTrace.cs's deliberate mip-4 bounce-albedo / emissive fetches
    // (DDGI_BOUNCE_ALBEDO_MIP) would run at mip 0.
    RHI::SamplerDesc linearWrap{};
    linearWrap.maxAnisotropy = 1;
    b.Sampler(linearWrap, 0);
    b.Flags(RHI::LayoutFlags::HEAP_INDEXED_RESOURCES);
    if (!gfx.CreatePipelineLayout(b.Desc(), m_layout))
    {
        LOG_ERROR("DDGIPass: pipeline layout failed");
        return false;
    }
    return true;
}

// ==== Compute PSOs (ray count, trace, relocation, relight, border) ====

bool DDGIPass::CreateCSPipelines(IGraphicsDevice& gfx)
{
    // Every DDGI shader includes DDGICommon.hlsli, which #errors when its DDGI_MAX_VOLUMES
    // disagrees with this value: DDGIVolumeManager.h static_asserts against
    // RHI::Slots::Gfx::DDGIMaxVolumes but cannot reach HLSL, and a mismatch would compile clean
    // and leave the extra volumes invisible. Injected in buildCS so a new DDGI shader cannot
    // miss it.
    const std::string kMaxVolumesStr = std::to_string(DDGI::kMaxVolumes);

    auto buildCS = [&](const std::string& path,
                       const std::vector<DxcCompiler::Define>& defines,
                       RHI::PipelineState& outPSO) -> bool
    {
        std::vector<DxcCompiler::Define> allDefines = defines;
        allDefines.push_back(DxcCompiler::Define{ "DDGI_MAX_VOLUMES_EXPECTED",
                                                  kMaxVolumesStr });

        const auto dxil = CompileShaderFile(path, RHI::ShaderStage::CS, allDefines, "main");
        if (dxil.empty()) return false;

        RHI::Shader cs;
        RHI::PipelineStateDesc pd{};
        pd.cs = &cs; pd.layout = &m_layout;
        const bool ok = gfx.CreateShader(RHI::ShaderStage::CS, dxil.data(), dxil.size(), cs) &&
                        gfx.CreatePipelineState(pd, outPSO);
        gfx.DestroyShader(cs);   // the PSO keeps its own copy of the DXIL
        if (!ok) LOG_ERROR("DDGIPass: pipeline failed for %s", path.c_str());
        return ok;
    };

    using D = DxcCompiler::Define;
    const std::vector<D> defIrr = { D{"DDGI_RELIGHT_TARGET_IRRADIANCE", "1"},
                                    D{"DDGI_RELIGHT_TARGET_DEPTH",      "0"} };
    const std::vector<D> defDep = { D{"DDGI_RELIGHT_TARGET_IRRADIANCE", "0"},
                                    D{"DDGI_RELIGHT_TARGET_DEPTH",      "1"} };

    if (!buildCS("shaders/DDGIRayTrace.cs.hlsl",          {}, m_tracePSO))             return false;
    if (!buildCS("shaders/DDGIPrepareRayCount.cs.hlsl",   {}, m_prepareRayCountPSO))   return false;
    if (!buildCS("shaders/DDGIRayAllocation.cs.hlsl",     {}, m_rayAllocationPSO))     return false;
    if (!buildCS("shaders/DDGIFinalizeIndirect.cs.hlsl",  {}, m_finalizeIndirectPSO))  return false;
    if (!buildCS("shaders/DDGIProbeRelocate.cs.hlsl",     {}, m_relocatePSO))          return false;
    if (!buildCS("shaders/DDGIRelight.cs.hlsl",      defIrr, m_relightIrradiancePSO)) return false;
    if (!buildCS("shaders/DDGIRelight.cs.hlsl",      defDep, m_relightDepthPSO))      return false;
    if (!buildCS("shaders/DDGIBorderUpdate.cs.hlsl", defIrr, m_borderIrradiancePSO))  return false;
    if (!buildCS("shaders/DDGIBorderUpdate.cs.hlsl", defDep, m_borderDepthPSO))       return false;
    // The trace and the per-probe kernels dispatch indirectly
    // (IGraphicsDevice::DispatchComputeIndirect) from the dispatch-args buffer FinalizeIndirect
    // writes.
    return true;
}

// ==== Per-frame execute ====

void DDGIPass::Execute(IGraphicsDevice& gfx,
                       RHI::CommandList cmd,
                       DDGI::DDGIVolumeManager& mgr,
                       uint32_t volumeSlot,
                       uint64_t tlasVA,
                       uint64_t skyIBLSrv,
                       uint64_t matBufVA,
                       uint64_t bindlessBufferTable,
                       uint64_t lightsSrvHandle,
                       bool     onComputeQueue)
{
    if (!m_ready) return;
    if (mgr.GetProbeCount(volumeSlot) == 0) return;

    const uint32_t probeCount   = mgr.GetProbeCount(volumeSlot);
    const uint32_t raysPerProbe = mgr.GetRaysPerProbe(volumeSlot);
    if (probeCount == 0 || raysPerProbe == 0) return;

    // Our layout, with the first dispatch's PSO. Every later stage binds its
    // own PSO under the same layout, so the root arguments set here carry
    // over (D3D12 invalidates them only when the root signature changes).
    gfx.BindComputePipelineState(m_prepareRayCountPSO, cmd);

    // CBV - the volume CB, this frame's ring slot (Tick() wrote the matching index).
    {
        const RHI::GPUBuffer* cb = mgr.GetVolumeCB(gfx, volumeSlot);
        if (!cb || !cb->IsValid())
        {
            LOG_INFO("DDGIPass: skipping volume %u — CB not allocated", volumeSlot);
            return;
        }
        gfx.SetComputeRootCBV(kCB, *cb, 0, cmd);
    }

    gfx.SetComputeRootSRVAddress(kTLAS, tlasVA, cmd);

    if (skyIBLSrv != 0)
        gfx.SetComputeDescriptorTable(kSkyIBL, skyIBLSrv, cmd);

    // Per-instance data (t3 space0): the trace's hit shading; the other kernels ignore it.
    if (matBufVA != 0)
        gfx.SetComputeRootSRVAddress(kInstanceData, matBufVA, cmd);

    // Bindless ByteAddressBuffer table (t0 space1): the trace fetches vertex positions for the
    // geometric face normal. The other kernels ignore it; keeping the root signature satisfied is
    // cheap.
    if (bindlessBufferTable != 0)
        gfx.SetComputeDescriptorTable(kBindlessBuffers, bindlessBufferTable, cmd);

    // Cluster GPULight buffer (t7): the trace picks a random light per ray. 0 is fine: the trace
    // skips direct light when g_Vol.lightCount == 0.
    if (lightsSrvHandle != 0)
        gfx.SetComputeDescriptorTable(kLightsSrv, lightsSrvHandle, cmd);

    // UAVs that persist across every dispatch of this Execute.
    if (uint64_t v = mgr.GetVarianceUav(volumeSlot))
        gfx.SetComputeDescriptorTable(kVarianceUav, v, cmd);
    if (uint64_t rc = mgr.GetRayCountUav(volumeSlot))
        gfx.SetComputeDescriptorTable(kRayCountUav, rc, cmd);
    if (uint64_t ra = mgr.GetRayAllocUav(volumeSlot))
        gfx.SetComputeDescriptorTable(kRayAllocUav, ra, cmd);
    if (uint64_t da = mgr.GetDispatchArgsUav(volumeSlot))
        gfx.SetComputeDescriptorTable(kDispatchArgsUav, da, cmd);

    // Probe-data SRV (t6), bound before the first dispatch: DDGIPrepareRayCount reads `state` to
    // zero the ray budget of INACTIVE probes (the trace binds it again). Legal: the buffer stays
    // in kSHReadState until the UAV window around the relocate dispatch.
    if (uint64_t pdSrvEarly = mgr.GetProbeDataSrv(volumeSlot))
        gfx.SetComputeDescriptorTable(kProbeDataSrv, pdSrvEarly, cmd);

    // Bindless textures (t0 space2): the trace samples emissive via
    // DDGIInstanceData.emissiveTexIdx.
    if (const uint64_t h = gfx.GetBindlessTextureTableGpuHandle())
        gfx.SetComputeDescriptorTable(kBindlessTextures, h, cmd);

    // Per-resource UAV barriers: each names the one resource it orders. A device-wide UAV
    // barrier (8 per volume per frame) would serialize every DDGI stage against every other and
    // against unrelated compute work on the queue. Resolved once here, since they repeat.
    const RHI::GPUBuffer* rayCountGB = mgr.GetRayCountBuffer(volumeSlot);
    const RHI::GPUBuffer* rayAllocGB = mgr.GetRayAllocBuffer(volumeSlot);
    const RHI::GPUBuffer* rayDataGB  = mgr.GetRayDataBuffer (volumeSlot);
    const RHI::GPUBuffer* argsGB = mgr.GetDispatchArgsBuffer(volumeSlot);
    const bool            haveArgs = argsGB && argsGB->IsValid();
    const RHI::Texture* depthTex = mgr.GetDepthAtlasTexture(volumeSlot);

    // Fail safe: a missing resource (a half-allocated slot, which the caller rules out) falls
    // back to a device-wide barrier rather than none - a missing UAV barrier is a race, a
    // redundant one only slow.
    auto uavBarrierOn = [&](const RHI::GPUResource* res) {
        gfx.PushBarrier(RHI::GPUBarrier::Memory(res && res->IsValid() ? res : nullptr), cmd);   // null == device-wide
    };

    // Per-step GPU timestamps, nested under the Renderer's "DDGI" region; the names are per
    // volume slot (kDDGIStepNames).
    auto pBegin = [&](DDGIStep step) -> uint32_t {
        return gfx.BeginGPUTimestamp(cmd, kDDGIStepNames[volumeSlot][step]);
    };
    auto pEnd = [&](uint32_t r) {
        gfx.EndGPUTimestamp(cmd, r);
    };

    // ---- 1a. Prepare the per-probe ray count from variance ----
    {
        uint32_t r = pBegin(kStepPrepare);
        gfx.BindComputePipelineState(m_prepareRayCountPSO, cmd);
        const uint32_t groups = (probeCount + 63u) / 64u;
        gfx.DispatchCompute(groups, 1, 1, cmd);
        uavBarrierOn(rayCountGB);    // -> read by RayAllocation below
        pEnd(r);
    }

    // ---- 1b. Pack the ray descriptors (atomic InterlockedAdd) ----
    {
        uint32_t r = pBegin(kStepRayAlloc);
        gfx.BindComputePipelineState(m_rayAllocationPSO, cmd);
        const uint32_t groups = (probeCount + 63u) / 64u;
        gfx.DispatchCompute(groups, 1, 1, cmd);
        // TWO resources, both load-bearing:
        //   rayAlloc - the packed ray descriptors, read by FinalizeIndirect and the trace;
        //   rayCount - the per-probe counts plus the compacted active-probe list and its length:
        //              FinalizeIndirect sizes the per-probe dispatch from it, the relights map
        //              SV_GroupID.x -> probeIdx through it, Border reads the counts.
        // The rayCount half is easy to lose: this kernel's obvious output is rayAlloc.
        {
            const RHI::GPUBarrier b[2] = {
                RHI::GPUBarrier::Memory(rayAllocGB && rayAllocGB->IsValid() ? rayAllocGB : nullptr),
                RHI::GPUBarrier::Memory(rayCountGB && rayCountGB->IsValid() ? rayCountGB : nullptr) };
            gfx.PushBarriers(b, 2, cmd);
        }
        pEnd(r);
    }

    // ---- 1c. Finalize the indirect dispatch args ----
    {
        uint32_t r = pBegin(kStepFinalize);
        gfx.BindComputePipelineState(m_finalizeIndirectPSO, cmd);
        gfx.DispatchCompute(1, 1, 1, cmd);
        uavBarrierOn(argsGB);        // -> the INDIRECT_ARGUMENT transition below
        pEnd(r);
    }

    // The trace reads the args buffer in INDIRECT_ARGUMENT state. rayAlloc stays UAV: the trace
    // reads its descriptors through a UAV.
    if (haveArgs)
        gfx.PushBarrier(RHI::GPUBarrier::Buffer(argsGB, RHI::ResourceState::UNORDERED_ACCESS,
                                                RHI::ResourceState::INDIRECT_ARGUMENT), cmd);

    // Depth atlas on entry: SRV (last frame's lighting read) or UAV (first frame after
    // creation). The trace samples last frame's depth atlas for multi-bounce Chebyshev
    // visibility, so it needs SRV; on a compute queue that is NON_PIXEL only (the only SRV state
    // valid on a compute list).
    mgr.TransitionVolumeAtlases(gfx, cmd, volumeSlot,
        DDGI::DDGIVolumeManager::AtlasState::SRV, onComputeQueue);

    // Irradiance atlas (octahedral texture): kept in kSHReadState at every Execute boundary and
    // UAV only around the relight. The validator tracks the last EXPLICIT state, and a texture
    // has no implicit decay. kSHReadState is NON_PIXEL | PIXEL on the graphics queue (LightingPass
    // reads it in the pixel shader) and NON_PIXEL alone on the compute queue (PIXEL is invalid
    // there). With DDGI on the compute queue, DDGIVolumeManager::DemoteForComputeQueue narrows it
    // before the DDGI list and the Renderer promotes it on the graphics queue
    // (PromoteAtlasesForGraphicsQueue) before LightingPass; the cross-queue fence orders them. It
    // is created in SHADER_RESOURCE, so frame 1 starts in that state too.
    const RHI::Texture* shGB  = mgr.GetIrradianceAtlasTexture(volumeSlot);
    const bool          haveSH = shGB && shGB->IsValid();
    // NON_PIXEL alone on the compute queue, NON_PIXEL | PIXEL on graphics.
    const RHI::ResourceState kSHReadState = onComputeQueue
        ? RHI::ResourceState::SHADER_RESOURCE_COMPUTE
        : RHI::ResourceState::SHADER_RESOURCE;
    // No barrier here: the atlas is already in kSHReadState (from creation or the previous
    // Execute's end barrier), which the trace's t4 SRV read accepts.

    // ---- 2. Trace (ExecuteIndirect) ----
    {
        uint32_t r = pBegin(kStepTrace);
        uint64_t rayUav = mgr.GetRayDataUav(volumeSlot);
        if (rayUav)
            gfx.SetComputeDescriptorTable(kOutputUav, rayUav, cmd);

        // Multi-bounce SRVs: this volume's irradiance atlas, depth atlas and probe data.
        if (uint64_t shSrv = mgr.GetIrradianceAtlasSrv(volumeSlot))
            gfx.SetComputeDescriptorTable(kIrradianceSrv, shSrv, cmd);
        if (uint64_t dep = mgr.GetDepthAtlasSrv(volumeSlot))
            gfx.SetComputeDescriptorTable(kDepthSrv, dep, cmd);
        if (uint64_t pd  = mgr.GetProbeDataSrv(volumeSlot))
            gfx.SetComputeDescriptorTable(kProbeDataSrv, pd, cmd);

        gfx.BindComputePipelineState(m_tracePSO, cmd);
        if (haveArgs)
            gfx.DispatchComputeIndirect(*argsGB, 0, cmd);
        uavBarrierOn(rayDataGB);     // -> read by Relocate + both Relight paths
        pEnd(r);
    }

    // The args buffer STAYS in INDIRECT_ARGUMENT until the end of Execute: Relight.Irr and
    // Relight.Dep consume its second argument slot (byte offset 16). FinalizeIndirect, its only
    // writer, already ran, so holding the state is free and saves two barriers per volume.

    // Trace consumed the depth atlas as SRV; relight + border need it as UAV.
    mgr.TransitionVolumeAtlases(gfx, cmd, volumeSlot,
        DDGI::DDGIVolumeManager::AtlasState::UAV);

    // Irradiance atlas: the trace read it via t4 (kSHReadState); the relight writes it via u0.
    if (haveSH)
        gfx.PushBarrier(RHI::GPUBarrier::Image(shGB, kSHReadState, RHI::ResourceState::UNORDERED_ACCESS), cmd);

    // ---- 2.5. Probe relocation and classification ----
    // ProbeData stays in kSHReadState between Execute calls (like the irradiance atlas), so
    // LightingPass samples it without a per-frame transition; it is UAV only around this
    // dispatch. The shader reads g_RayData (t2) and g_RayCount (u2) and writes ProbeData (u0):
    // the always-on sky visibility, plus an EMA-filtered push out of nearby surfaces and the
    // INACTIVE classification, gated by the volume's flags (bit 1 enableRelocation, bit 2
    // enableClassification) - so the dispatch always runs.
    const RHI::GPUBuffer* pdGB   = mgr.GetProbeDataBuffer(volumeSlot);
    const bool            havePD = pdGB && pdGB->IsValid();
    if (havePD)
        gfx.PushBarrier(RHI::GPUBarrier::Buffer(pdGB, kSHReadState, RHI::ResourceState::UNORDERED_ACCESS), cmd);

    if (uint64_t pdUav = mgr.GetProbeDataUav(volumeSlot))
    {
        uint32_t r = pBegin(kStepRelocate);
        gfx.BindComputePipelineState(m_relocatePSO, cmd);
        gfx.SetComputeDescriptorTable(kOutputUav, pdUav, cmd);
        if (uint64_t raySrv = mgr.GetRayDataSrv(volumeSlot))
            gfx.SetComputeDescriptorTable(kRayDataSrv, raySrv, cmd);
        const uint32_t groups = (probeCount + 63u) / 64u;
        gfx.DispatchCompute(groups, 1, 1, cmd);
        uavBarrierOn(pdGB);          // -> the UAV->kSHReadState restore below
        pEnd(r);
    }

    // Restore ProbeData to kSHReadState so LightingPass + next-frame trace's
    // multi-bounce SRV read just work without further transitions.
    if (havePD)
        gfx.PushBarrier(RHI::GPUBarrier::Buffer(pdGB, RHI::ResourceState::UNORDERED_ACCESS, kSHReadState), cmd);

    // ---- 3 + 4. Relight: irradiance, then depth ----
    // The variance / ray count / ray alloc bindings persist from the trace setup; each dispatch
    // swaps the atlas UAV and the ray SRV. runRelight's barrierRes names the resource whose
    // writes the NEXT recorded command must observe (null when a later state transition orders
    // them); see the two call sites.
    // kPerProbeArgsOffset: byte offset of the per-probe D3D12_DISPATCH_ARGUMENTS in the
    // dispatch-args buffer (slot 0 = trace); mirrors g_DispatchArgs[4..6] in
    // DDGIFinalizeIndirect.cs.hlsl.
    constexpr uint64_t kPerProbeArgsOffset = 16;

    // One thread group per ACTIVE probe, dispatched indirectly: the group count comes from the
    // compacted list DDGIRayAllocation built, so groups that would return at once (inactive
    // probes, probes outside this frame's update subset) are never launched.
    auto dispatchPerProbeIndirect = [&]()
    {
        if (haveArgs)
            gfx.DispatchComputeIndirect(*argsGB, kPerProbeArgsOffset, cmd);
    };

    auto runRelight = [&](const RHI::PipelineState& pso,
                          uint64_t atlasUav, uint64_t raySrv,
                          const RHI::GPUResource* barrierRes)
    {
        gfx.BindComputePipelineState(pso, cmd);
        if (raySrv)   gfx.SetComputeDescriptorTable(kRayDataSrv, raySrv, cmd);
        if (atlasUav) gfx.SetComputeDescriptorTable(kOutputUav, atlasUav, cmd);
        dispatchPerProbeIndirect();

        if (barrierRes && barrierRes->IsValid())
            gfx.PushBarrier(RHI::GPUBarrier::Memory(barrierRes), cmd);
    };
    // Relight.Irr writes the octahedral RGBA16F irradiance atlas and needs its barrier:
    // Border.Irr reads the interior texels this dispatch wrote to mirror them into the border
    // ring. Every barrier names its resource, so each new cross-dispatch data flow must be
    // checked by hand; a missing one is a race, not a stall. The variance buffer needs none: its
    // only reader is next frame's PrepareRayCount, ordered by the frame fence.
    {
        uint32_t r = pBegin(kStepRelightIrr);
        runRelight(m_relightIrradiancePSO,
                   mgr.GetIrradianceAtlasUav(volumeSlot),
                   mgr.GetRayDataSrv(volumeSlot),
                   /*barrierRes=*/shGB);
        pEnd(r);
    }
    // Required too: Border reads the interior texels of the depth atlas this dispatch wrote.
    {
        uint32_t r = pBegin(kStepRelightDep);
        runRelight(m_relightDepthPSO,
                   mgr.GetDepthAtlasUav(volumeSlot),
                   mgr.GetRayDataSrv(volumeSlot),
                   /*barrierRes=*/depthTex);
        pEnd(r);
    }

    // ---- 5 + 6. Border update ----
    auto runBorder = [&](const RHI::PipelineState& pso, uint64_t atlasUav,
                         const RHI::Texture* atlasRes)
    {
        gfx.BindComputePipelineState(pso, cmd);
        if (atlasUav) gfx.SetComputeDescriptorTable(kOutputUav, atlasUav, cmd);
        // DIRECT dispatch, one group per probe, not the indirect active-probe path of the relights:
        // this kernel is latency-bound, so the skipped-probe groups buy occupancy for free and
        // probeIdx == SV_GroupID.x keeps consecutive groups on consecutive atlas tiles (the compacted
        // list measured 4x slower; DDGIBorderUpdate.cs.hlsl has the details).
        gfx.DispatchCompute(probeCount, 1, 1, cmd);

        // The consumers (the caller's TransitionVolumeAtlases UAV -> SRV for the depth atlas, the
        // UAV -> kSHReadState transition at the end of Execute for the irradiance atlas) would each
        // order these writes, but neither is issued here, so keep this barrier on the atlas THIS call
        // wrote. The atlas is a parameter because both border dispatches use this lambda.
        uavBarrierOn(atlasRes);
    };
    // Both atlases need border replication: skipping it leaves the 1-texel ring at its
    // creation-time zero, and every bilinear tap near a tile edge blends that zero in - dark
    // seams in a grid pattern, worst in the -Z hemisphere (the octahedral square's outer ring).
    {
        uint32_t r = pBegin(kStepBorder);
        runBorder(m_borderIrradiancePSO, mgr.GetIrradianceAtlasUav(volumeSlot), shGB);
        runBorder(m_borderDepthPSO,      mgr.GetDepthAtlasUav(volumeSlot),      depthTex);
        pEnd(r);
    }

    // Args buffer back to UAV for next frame's FinalizeIndirect; deferred to here because both
    // relights ExecuteIndirect from it.
    if (haveArgs)
        gfx.PushBarrier(RHI::GPUBarrier::Buffer(argsGB, RHI::ResourceState::INDIRECT_ARGUMENT,
                                                RHI::ResourceState::UNORDERED_ACCESS), cmd);

    // Irradiance atlas back to kSHReadState, so LightingPass reads it without a transition and
    // the next Execute's after-trace barrier finds the StateBefore it expects. Without it the
    // validator carries UAV into the next frame, and that frame's UAV -> UAV mismatch hangs the
    // device.
    if (haveSH)
        gfx.PushBarrier(RHI::GPUBarrier::Image(shGB, RHI::ResourceState::UNORDERED_ACCESS, kSHReadState), cmd);

#if DDGI_GT_DIAGNOSTICS
    // Diagnostic ground-truth readback: a no-op unless DDGIVolumeManager::Tick armed this slot
    // (~every 600 volume frames). Must run after every DDGI dispatch, so the copied ray / probe
    // data is this frame's final state; relies on the states this function leaves (rayData /
    // rayAlloc in UAV, probeData back in its shader-read state).
    mgr.RecordDebugReadback(gfx, cmd, volumeSlot, onComputeQueue);
#endif
    // Runtime probe watch (Engine.DDGIWatchProbe): same placement and state contract as the
    // readback above; a no-op unless a watch is set.
    mgr.RecordProbeWatch(gfx, cmd, volumeSlot, onComputeQueue);
}
