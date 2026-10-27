#include "RenderGraph/RenderPass/AfterimageCapturePass.h"
#include "Graphics/AfterimageSystem.h"
#include "Graphics/IGraphicsDevice.h"
#include "System/Log.h"

#include <cstring>

// GPU-side per-job CB. Must match AfterimageCopy.cs.hlsl AfterimageCopyJob.
struct AfterimageCopyJobCB
{
    uint32_t srcPosElementBase;
    uint32_t srcNrmElementBase;
    uint32_t dstPosElementBase;
    uint32_t dstNrmElementBase;
    uint32_t vertexCount;
    uint32_t _pad0;
    uint32_t _pad1;
    uint32_t _pad2;
};

void AfterimageCapturePass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::AfterimageCopy_CS, RHI::ShaderStage::CS,
                          "AfterimageCopy.cs.hlsl", "AfterimageCopyCS");

    if (!CreatePipelines(gfx)) return;

    // FrameCB::Create gives each frame in flight a CONSTANT_BUFFER of kMaxJobsPerFrame x
    // kJobSlotBytes; SetComputeRootCBV binds byte offsets into it, valid because every slot is
    // 256-aligned.
    if (!m_jobCB.Create(gfx, "AfterimageCapture.JobCB"))
        LOG_ERROR("AfterimageCapturePass: failed to create job CB ring");

    LOG_SUCCESS("AfterimageCapturePass: initialised");
}

bool AfterimageCapturePass::CreatePipelines(IGraphicsDevice& gfx)
{
    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::AfterimageCopy_CS);
    if (!cs)
    {
        LOG_ERROR("AfterimageCapturePass: AfterimageCopy_CS shader not found");
        return false;
    }

    // Onto the live member: a failed rebuild keeps the old pipeline.
    RHI::PipelineStateDesc d{};
    d.cs = cs;
    if (!gfx.CreatePipelineState(d, m_pso))
    {
        LOG_ERROR("AfterimageCapturePass: compute PSO creation failed");
        return false;
    }
    return true;
}

void AfterimageCapturePass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (m_shaderLib.ClearCaches()) CreatePipelines(gfx);
}

RHI::CommandList AfterimageCapturePass::Execute(RHI::CommandList cl)
{
    if (!m_pso.IsValid() || !m_jobCB.IsValid())    return cl;
    if (!m_system || !m_system->IsInitialised())   return cl;
    // The snapshot pool is made resident lazily (AfterimageSystem::EnsurePool) and released with the
    // world. Jobs only exist once the pool does, but binding a destroyed UAV removes the device: check,
    // do not infer.
    if (!m_system->GetPoolPosBuffer().IsValid() ||
        !m_system->GetPoolNrmBuffer().IsValid()) return cl;
    const auto& jobs = m_system->GetCaptureJobs();
    if (jobs.empty()) return cl;

    auto& gfx = *m_gfx;

    // The current ring slot: a 256-aligned multi-slot CBV; the dispatches below bind byte offsets
    // into it.
    JobBufferPool* pool = m_jobCB.Current(gfx);
    if (!pool) return cl;
    uint8_t* cbBase = pool->bytes;
    const RHI::GPUBuffer& cbBuf = m_jobCB.CurrentBuffer(gfx);

    // ---- Phase 1 (CPU): pack jobs into the per-job CBV slots ----
    uint32_t totalJobs = 0;
    for (const auto& j : jobs)
    {
        if (j.vertexCount == 0) continue;
        if (totalJobs >= kMaxJobsPerFrame)
        {
            LOG_WARNING("AfterimageCapturePass: capture jobs exceed kMaxJobsPerFrame (%u) — "
                        "remaining skipped", kMaxJobsPerFrame);
            break;
        }
        auto* slot = reinterpret_cast<AfterimageCopyJobCB*>(
            cbBase + totalJobs * kJobSlotBytes);
        std::memset(slot, 0, kJobSlotBytes);
        slot->srcPosElementBase = j.srcPosElementBase;
        slot->srcNrmElementBase = j.srcNrmElementBase;
        slot->dstPosElementBase = j.dstPosElementBase;
        slot->dstNrmElementBase = j.dstNrmElementBase;
        slot->vertexCount       = j.vertexCount;
        ++totalJobs;
    }
    if (totalJobs == 0) return cl;

    // ---- Phase 2 (GPU): bind the shared UAVs once, dispatch per job ----
    gfx.BindComputePipelineState(m_pso, cl);

    gfx.SetComputeDescriptorTable(kDstPos, m_system->GetPoolPosUAV(), cl);
    gfx.SetComputeDescriptorTable(kDstNrm, m_system->GetPoolNrmUAV(), cl);

    uint32_t dispatchIdx = 0;
    for (const auto& j : jobs)
    {
        if (j.vertexCount == 0)                continue;
        if (dispatchIdx >= totalJobs)          break;

        gfx.SetComputeRootCBV(kCBSlot, cbBuf, dispatchIdx * kJobSlotBytes, cl);
        gfx.SetComputeDescriptorTable(kSrcPos, j.srcPosSrvHandle, cl);
        gfx.SetComputeDescriptorTable(kSrcNrm, j.srcNrmSrvHandle, cl);

        gfx.DispatchCompute((j.vertexCount + 63u) / 64u, 1, 1, cl);
        ++dispatchIdx;
    }

    // UAV barrier so subsequent graph passes see the copied snapshot data
    // when they read m_poolPos / m_poolNrm via bindless SRV.
    gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_system->GetPoolPosBuffer()), cl);
    gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_system->GetPoolNrmBuffer()), cl);

    return cl;
}
