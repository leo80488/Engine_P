#include "RenderGraph/RenderPass/UpscalerReactiveMaskPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RootSignatureLayout.h"
#include "Graphics/ShadingStencil.h"
#include "System/Log.h"
#include "System/Check.h"

// Shared compute root-signature slots (space2) from RHI::Slots::Cmp:
// S::CB = b0, S::SRV_t0 = t0, S::UAV_u0 = u0.
namespace S = RHI::Slots::Cmp;

namespace
{
    // The stencil bits TAA weakens history on (Graphics/ShadingStencil.h).
    constexpr uint32_t kParticleTemporalBit = ShadingStencil::kTaaExclude;   // 0x40
    constexpr uint32_t kOutlineTemporalBit  = ShadingStencil::kOutlineRim;   // 0x80
}

void UpscalerReactiveMaskPass::Init(IGraphicsDevice& gfx)
{
    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::UpscalerReactiveMask_CS, RHI::ShaderStage::CS,
                         "UpscalerReactiveMask.cs.hlsl", "CSMain");
    if (CreatePipelines(gfx))
        LOG_SUCCESS("UpscalerReactiveMaskPass: initialized");
}

// Creates into the live member; a failed rebuild keeps the old pipeline.
bool UpscalerReactiveMaskPass::CreatePipelines(IGraphicsDevice& gfx)
{
    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::UpscalerReactiveMask_CS);
    if (!cs)
    {
        LOG_ERROR("UpscalerReactiveMaskPass: UpscalerReactiveMask_CS shader not found");
        return false;
    }
    RHI::PipelineStateDesc pd{};
    pd.cs = cs;
    if (!gfx.CreatePipelineState(pd, m_pso))
    {
        LOG_ERROR("UpscalerReactiveMaskPass: PSO creation failed");
        return false;
    }
    return true;
}

void UpscalerReactiveMaskPass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (m_shaderLib.ClearCaches()) CreatePipelines(gfx);
}

void UpscalerReactiveMaskPass::Prepare(IGraphicsDevice& gfx, bool wanted)
{
    m_wanted = wanted;
    m_written.store(false, std::memory_order_relaxed);
    if (!wanted) { Release(gfx); return; }
    const uint32_t w = gfx.GetRenderWidth(), h = gfx.GetRenderHeight();
    if (w == 0 || h == 0) return;
    if (m_tex.IsValid() && w == m_w && h == m_h) return;
    Release(gfx);
    RHI::TextureDesc td{};
    td.width      = w;
    td.height     = h;
    td.format     = RHI::Format::R8_UNORM;
    td.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    td.usage      = RHI::Usage::DEFAULT;
    td.layout     = RHI::ResourceState::UNORDERED_ACCESS;
    td.debugName = "Upscaler.ReactiveMask";
    if (!gfx.CreateTexture(td, m_tex))
    {
        LOG_ERROR("UpscalerReactiveMaskPass: allocation failed (%ux%u)", w, h);
        return;
    }
    m_state = td.layout;
    m_w = w; m_h = h;
    m_loggedFirstDispatch = false;   // the dispatch line below, once per allocation
    LOG_INFO("[REACTIVE] allocated=1 width=%u height=%u", w, h);
}

void UpscalerReactiveMaskPass::Release(IGraphicsDevice& gfx)
{
    if (!m_tex.IsValid()) return;
    gfx.DestroyTexture(m_tex);
    m_tex = {};
    m_w = m_h = 0;
    m_written.store(false, std::memory_order_relaxed);
    LOG_INFO("[REACTIVE] released=1");
}

const RHI::Texture* UpscalerReactiveMaskPass::Execute(RHI::CommandList cl, IGraphicsDevice& gfx,
                                                      uint64_t stencilSrv, uint32_t renderW,
                                                      uint32_t renderH, float particleWeight,
                                                      float outlineWeight)
{
    if (!Ready()) return nullptr;
    // The upscaler takes the mask at its render size; a mask Prepare sized for
    // another one (a size change between the boundary and this frame) is
    // skipped rather than handed over at the wrong extent.
    if (renderW != m_w || renderH != m_h) return nullptr;
    // Without the stencil view there is nothing to mark; an all-zero mask would
    // be the same as none, so hand the upscaler none. Said once: a skipped mask
    // and a working one look alike until a particle ghosts.
    if (!ENGINE_CHECK_ONCE(stencilSrv != 0,
                           "UpscalerReactiveMaskPass: no stencil view of the depth buffer - the "
                           "upscaler gets no reactive mask, so particles that opted out of TAA ghost"))
        return nullptr;
    const uint64_t uav = gfx.GetTextureUAVGpuHandle(m_tex);
    if (uav == 0) return nullptr;

    const RHI::Constants<ReactiveMaskCB> c = AllocConstants<ReactiveMaskCB>(gfx);
    if (!c) return nullptr;
    *c.cpu = ReactiveMaskCB{ m_w, m_h, kParticleTemporalBit, kOutlineTemporalBit,
                             particleWeight, outlineWeight, { 0.0f, 0.0f } };

    if (m_state != RHI::ResourceState::UNORDERED_ACCESS)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(&m_tex, m_state, RHI::ResourceState::UNORDERED_ACCESS), cl);
        m_state = RHI::ResourceState::UNORDERED_ACCESS;
    }
    gfx.BindComputePipelineState(m_pso, cl);
    gfx.SetComputeRootCBVAddress(S::CB, c.gpu, cl);
    gfx.SetComputeDescriptorTable(S::SRV_t0, stencilSrv, cl);
    gfx.SetComputeDescriptorTable(S::UAV_u0, uav, cl);
    gfx.DispatchCompute((m_w + 7) / 8, (m_h + 7) / 8, 1, cl);
    gfx.PushBarrier(RHI::GPUBarrier::Image(&m_tex, RHI::ResourceState::UNORDERED_ACCESS,
                                           RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
    m_state = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
    m_written.store(true, std::memory_order_release);

    // Proof of life, once per allocation: "the mask ran" and "the mask was
    // never armed" are otherwise indistinguishable from the log.
    if (!m_loggedFirstDispatch)
    {
        m_loggedFirstDispatch = true;
        LOG_INFO("[REACTIVE] dispatched=1 width=%u height=%u particleBit=0x%02x particleWeight=%.3f "
                 "outlineBit=0x%02x outlineWeight=%.3f",
                 m_w, m_h, kParticleTemporalBit, particleWeight, kOutlineTemporalBit, outlineWeight);
    }
    return &m_tex;
}
