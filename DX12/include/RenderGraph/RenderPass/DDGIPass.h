#pragma once

// DDGIPass - every per-frame DDGI dispatch of one volume (the Renderer builds the DDGI TLAS
// first):
//   1. adaptive ray count: PrepareRayCount, RayAllocation (also compacts the active-probe
//      list), FinalizeIndirect (the dispatch args);
//   2. the trace: inline RayQuery (compute, DXR 1.1 - no state object, no DispatchRays),
//      dispatched indirectly with that ray budget, writing per-probe rays into the volume's
//      RayData buffer;
//   2.5 probe relocation and classification;
//   3-4. relight of the irradiance and depth atlases (EMA blend), one group per active probe;
//   5-6. the 1-texel border of both atlases.
// Standalone pass (manual Execute): the Renderer calls it per active volume after the manager
// uploaded this frame's constants, on the async compute queue when it has one (as SkinningPass
// / ParticleSimPass do).

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "RenderGraph/PassLifecycle.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsStruct.h"

#include <cstdint>

namespace DDGI { class DDGIVolumeManager; }

class DDGIPass : public RG::PassLifecycle
{
public:
    // Creates the root signature, compiles the shaders and builds the PSOs. Returns false
    // (logged) when the device lacks inline RayQuery (DXR 1.1); skip Execute then.
    bool Init(IGraphicsDevice& gfx);
    void Shutdown(IGraphicsDevice& gfx) override;

    // Recompiles the shaders and rebuilds the PSOs (hot reload). Call between frames, with the
    // GPU idle.
    bool ReloadPipelines(IGraphicsDevice& gfx);   // false: not ready, or a pipeline failed
    void ReloadShaders(IGraphicsDevice& gfx) override { ReloadPipelines(gfx); }
    uint32_t HandledEvents() const override { return kReloadShaders; }   // events from Renderer::PassSet

    // Records one volume. The TLAS GPU VA is passed in so the pass does not care how the AS is
    // built: the Renderer builds one DDGI-only TLAS shared by every volume (same static geometry).
    // The caller must have:
    //   - uploaded the volume constants this frame (DDGIVolumeManager::Tick);
    //   - built / refit the DDGI TLAS;
    //   - a graphics or compute command list (inline RayQuery works on either).
    // @p matBufVA is the GPU VA of the per-instance data (DDGIInstanceData) DDGISceneAS publishes;
    // the trace's hit shading indexes it with the committed instance ID, 0 -> grey hits.
    // @p onComputeQueue: the list targets the compute queue, so shader-read states are NON_PIXEL
    // only.
    void Execute(IGraphicsDevice& gfx,
                 RHI::CommandList cmd,
                 DDGI::DDGIVolumeManager& mgr,
                 uint32_t volumeSlot,
                 uint64_t tlasVA,
                 uint64_t skyIBLSrv,
                 uint64_t matBufVA,
                 uint64_t bindlessBufferTable,
                 uint64_t lightsSrvHandle,
                 bool     onComputeQueue = false);

    bool IsReady() const { return m_ready; }

private:
    bool BuildRootSignature(IGraphicsDevice& gfx);
    bool CreateCSPipelines(IGraphicsDevice& gfx);
    void DestroyPipelines(IGraphicsDevice& gfx);

    // The DDGI-only compute root signature:
    //   [0] ROOT_CBV          b0 space0  (DDGIVolumeGPU)
    //   [1] ROOT_SRV          t0 space0  (TLAS)
    //   [2] DESC_TABLE 1 SRV  t1 space0  (sky IBL)
    //   [3] DESC_TABLE 1 UAV  u0 space0  (atlas / ray data / probe data - per dispatch)
    //   [4] DESC_TABLE 1 SRV  t2 space0  (ray data - relocation and relight read path)
    //   [5] ROOT_SRV          t3 space0  (per-instance data - DDGISceneAS)
    //   [6] DESC_TABLE MeshDescriptorHeap::kMaxBuffers SRV t0 space1
    //                                    (bindless ByteAddressBuffer g_DDGIBuffers[] - VB/IB
    //                                    lookup at hit time for the face normal)
    //   [7] DESC_TABLE 1 UAV  u1 space0  (variance buffer - irradiance relight only)
    //   [8] DESC_TABLE 1 SRV  t4 space0  (irradiance atlas - multi-bounce trace read)
    //   [9] DESC_TABLE 1 SRV  t5 space0  (depth atlas - multi-bounce trace read)
    //   [10] DESC_TABLE 1 SRV t6 space0  (probe data - trace and PrepareRayCount)
    //   [11] DESC_TABLE 1 SRV t7 space0  (cluster GPULight buffer - direct light loop)
    //   [12] DESC_TABLE 1 UAV u2 space0  (per-probe adaptive ray count)
    //   [13] DESC_TABLE 1 UAV u3 space0  (ray allocation - total + descriptors)
    //   [14] DESC_TABLE 1 UAV u4 space0  (dispatch args - written by FinalizeIndirect, read by
    //                                    DispatchComputeIndirect in INDIRECT_ARGUMENT state)
    //   [15] DESC_TABLE kMaxBindlessTextures SRV t0 space2 (bindless g_AllTextures[])
    //   static sampler s0 space0  (linear wrap)
    RHI::PipelineLayout m_layout;

    // Trace CS: inline RayQuery (DXR 1.1), dispatched indirectly
    // (IGraphicsDevice::DispatchComputeIndirect) from the dispatch-args buffer.
    RHI::PipelineState m_tracePSO;

    // Adaptive ray-count + indirect-args pipeline.
    RHI::PipelineState m_prepareRayCountPSO;
    RHI::PipelineState m_rayAllocationPSO;
    RHI::PipelineState m_finalizeIndirectPSO;

    // Probe relocation and classification (DDGIProbeRelocate.cs.hlsl): pushes probes out of
    // nearby surfaces through DDGIProbeData.offset (an EMA-filtered push), flags buried probes
    // INACTIVE and updates the always-on sky visibility. Always dispatched; the volume's flags
    // (enableRelocation, enableClassification) gate the first two in the shader.
    RHI::PipelineState m_relocatePSO;

    // Compute PSOs of DDGIRelight.cs.hlsl + DDGIBorderUpdate.cs.hlsl, one irradiance and one
    // depth variant each.
    RHI::PipelineState m_relightIrradiancePSO;
    RHI::PipelineState m_relightDepthPSO;
    RHI::PipelineState m_borderIrradiancePSO;
    RHI::PipelineState m_borderDepthPSO;

    bool m_ready = false;
};
