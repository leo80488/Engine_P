#include "Graphics/SSR/SSRTemporalPass.h"
#include "Graphics/SSR/SSRRootSigSlots.h"
#include "Graphics/IGraphicsDevice.h"
#include "System/Check.h"
#include "System/Log.h"

#include <cstring>

// SSRTemporalPass - dual-reprojection history accumulation (pass 4). Velocity
// reprojection and reflection-hit reprojection compete; the candidate whose history
// luminance better matches the current pixel wins. History is then clamped to a YCoCg
// AABB to suppress ghosting.

using namespace SSR::RootSig;

static_assert(sizeof(SSRTemporalPass::SSRTemporalCB) <= 256, "SSRTemporalCB > 256");

void SSRTemporalPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;
    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::SSRTemporal_CS, RHI::ShaderStage::CS,
                         "SSRTemporal.cs.hlsl", "CSMain");

    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::SSRTemporal_CS);
    if (!cs) { LOG_ERROR("SSRTemporalPass: shader missing"); return; }
    RHI::PipelineStateDesc d{}; d.cs = cs;
    if (!gfx.CreatePipelineState(d, m_pso))
    { LOG_ERROR("SSRTemporalPass: PSO failed"); return; }

    m_cb.Create(gfx, "SSRTemporal.CB");

    LOG_SUCCESS("SSRTemporalPass: initialised");
}

void SSRTemporalPass::ReloadShaders(IGraphicsDevice& gfx)
{
    m_shaderLib.ClearCaches();
    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::SSRTemporal_CS);
    if (!cs) { LOG_ERROR("SSRTemporalPass::ReloadShaders: shader missing"); return; }
    RHI::PipelineStateDesc d{}; d.cs = cs;
    if (!gfx.CreatePipelineState(d, m_pso))
        LOG_ERROR("SSRTemporalPass::ReloadShaders: PSO rebuild failed");
    // Wipe history so the first post-reload frame doesn't blend a stale
    // accumulator from before the shader change.
    MarkReset();
}

void SSRTemporalPass::BindView(ViewState& view)
{
    m_hist = &view.History<History>(this);
}

void SSRTemporalPass::History::Release(IGraphicsDevice& gfx)
{
    for (RHI::Texture* set : { color, variance, depthHistory })
        for (int i = 0; i < 2; ++i)
        {
            if (set[i].IsValid()) gfx.DestroyTexture(set[i]);
            set[i] = {};
        }
}

void SSRTemporalPass::EnsureTextures(uint32_t renderW, uint32_t renderH)
{
    if (!m_gfx || renderW == 0 || renderH == 0) return;
    if (!ENGINE_CHECK_ONCE(m_hist != nullptr, "SSRTemporalPass: no view bound (PassSet::RegisterView)"))
        return;
    History& h = *m_hist;

    // The trace runs at full render res (see SSRTracePass).
    const uint32_t traceW = renderW;
    const uint32_t traceH = renderH;

    if (renderW == m_renderW && renderH == m_renderH &&
        traceW == m_w && traceH == m_h &&
        h.color[0].IsValid() && h.color[1].IsValid() &&
        h.variance[0].IsValid() && h.variance[1].IsValid() &&
        h.depthHistory[0].IsValid() && h.depthHistory[1].IsValid())
        return;

    for (auto& t : h.color)        if (t.IsValid()) m_gfx->DestroyTexture(t);
    for (auto& t : h.variance)     if (t.IsValid()) m_gfx->DestroyTexture(t);
    for (auto& t : h.depthHistory) if (t.IsValid()) m_gfx->DestroyTexture(t);

    m_renderW = renderW; m_renderH = renderH;
    m_w = traceW;        m_h = traceH;
    h.writeIdx = 0;
    h.reset    = true;

    RHI::TextureDesc rgba{};
    rgba.width = traceW; rgba.height = traceH;
    rgba.format = RHI::Format::R16G16B16A16_FLOAT;
    rgba.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    rgba.usage = RHI::Usage::DEFAULT;
    rgba.layout = RHI::ResourceState::SHADER_RESOURCE;

    // .x = accumulated variance, .y = accumulation count N: the shader's TAA-independent
    // blend weight and clip-box width both derive from N.
    RHI::TextureDesc varDesc = rgba;
    varDesc.format = RHI::Format::R16G16_FLOAT;

    for (int i = 0; i < 2; ++i)
    {
        if (!m_gfx->CreateTexture(rgba, h.color[i]))
            LOG_ERROR("SSRTemporalPass: color[%d] create failed", i);
        if (!m_gfx->CreateTexture(varDesc, h.variance[i]))
            LOG_ERROR("SSRTemporalPass: variance[%d] create failed", i);
    }

    // R32_FLOAT to match the NDC depth precision the shader expects.
    RHI::TextureDesc depth = rgba;
    depth.format = RHI::Format::R32_FLOAT;
    for (int i = 0; i < 2; ++i)
    {
        if (!m_gfx->CreateTexture(depth, h.depthHistory[i]))
            LOG_ERROR("SSRTemporalPass: depthHistory[%d] create failed", i);
    }

    LOG_INFO("SSRTemporalPass: temporal buffers %ux%u (full render res) ready",
             traceW, traceH);
}

uint64_t SSRTemporalPass::GetColorSrv() const
{
    if (!m_gfx || !m_hist || !m_hist->color[m_hist->writeIdx].IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_hist->color[m_hist->writeIdx]);
}

uint64_t SSRTemporalPass::GetVarianceSrv() const
{
    if (!m_gfx || !m_hist || !m_hist->variance[m_hist->writeIdx].IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_hist->variance[m_hist->writeIdx]);
}

const RHI::Texture* SSRTemporalPass::GetColorTexture() const
{
    return m_hist ? &m_hist->color[m_hist->writeIdx] : nullptr;
}
const RHI::Texture* SSRTemporalPass::GetVarianceTexture() const
{
    return m_hist ? &m_hist->variance[m_hist->writeIdx] : nullptr;
}

void SSRTemporalPass::Execute(RHI::CommandList cl,
                              uint32_t traceW, uint32_t traceH,
                              uint64_t colorCurrentSrv,
                              uint64_t varianceCurrentSrv,
                              uint64_t reprojDepthSrv,
                              uint64_t velocitySrv,
                              uint64_t depthSrv)
{
    if (!m_pso.IsValid() || !m_cb.IsValid() || !m_hist) return;
    if (traceW != m_w || traceH != m_h) return;
    if (!colorCurrentSrv) return;
    History& h = *m_hist;
    // The chain runs only in FullFold / FullAdditive; on the PyramidOnly frames between,
    // this history is neither updated nor reset, so after a gap it holds a frame long gone:
    // restart it.
    if (h.lastFrame != ~0ull && h.lastFrame + 1 != m_renderFrame)
    {
        if (!h.reset)
            LOG_INFO("[SSR] temporal history restarts after a gap: frame=%llu last=%llu",
                     static_cast<unsigned long long>(m_renderFrame), static_cast<unsigned long long>(h.lastFrame));
        h.reset = true;
    }
    h.lastFrame = m_renderFrame;

    auto& dx12 = *m_gfx;
    // Advance the ping-pong BEFORE binding. The upsample pass (and the SSR debug window)
    // call Get*Srv() right after Execute and must receive the buffer written THIS frame.
    // Flipping at the END of Execute would hand them the history buffer: a reflection
    // accumulated under the PREVIOUS camera pose, composited without reprojection, detaches
    // from its contact line by one frame of screen motion whenever the camera moves
    // (invisible while it is stationary).
    h.writeIdx = 1u - h.writeIdx;
    const uint32_t readIdx = 1u - h.writeIdx;

    SSRTemporalPass::SSRTemporalCB cb{};
    using namespace DirectX;
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.invViewProj),
        XMMatrixTranspose(XMLoadFloat4x4(&m_cam.invViewProj)));
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.prevViewProj),
        XMMatrixTranspose(XMLoadFloat4x4(&m_cam.prevViewProj)));
    cb.traceW = traceW; cb.traceH = traceH;
    cb.invTraceW = 1.0f / float(traceW);
    cb.invTraceH = 1.0f / float(traceH);
    cb.resetHistory = h.reset ? 1u : 0u;
    cb.nearZ = m_cam.nearZ;
    cb.farZ  = m_cam.farZ;
    cb.renderW = m_renderW;
    cb.renderH = m_renderH;
    cb.frameIndex = m_frameIndex;
    if (auto* slot = m_cb.Current(dx12)) *slot = cb;

    dx12.PushBarrier(RHI::GPUBarrier::Image(
        &h.color[h.writeIdx], RHI::ResourceState::SHADER_RESOURCE,
        RHI::ResourceState::UNORDERED_ACCESS), cl);
    dx12.PushBarrier(RHI::GPUBarrier::Image(
        &h.variance[h.writeIdx], RHI::ResourceState::SHADER_RESOURCE,
        RHI::ResourceState::UNORDERED_ACCESS), cl);
    dx12.PushBarrier(RHI::GPUBarrier::Image(
        &h.depthHistory[h.writeIdx], RHI::ResourceState::SHADER_RESOURCE,
        RHI::ResourceState::UNORDERED_ACCESS), cl);

    dx12.BindComputePipelineState(m_pso, cl);
    dx12.SetComputeRootCBV(kCB, m_cb.CurrentBuffer(dx12), 0, cl);
    dx12.SetComputeDescriptorTable(kSRV_T0, colorCurrentSrv,    cl);
    dx12.SetComputeDescriptorTable(kSRV_T1,
        m_gfx->GetTextureSRVGpuHandle(h.color[readIdx]),    cl);
    dx12.SetComputeDescriptorTable(kSRV_T2, varianceCurrentSrv, cl);
    dx12.SetComputeDescriptorTable(kSRV_T3,
        m_gfx->GetTextureSRVGpuHandle(h.variance[readIdx]), cl);
    dx12.SetComputeDescriptorTable(kSRV_T4, reprojDepthSrv,     cl);
    if (velocitySrv)
        dx12.SetComputeDescriptorTable(kSRV_T5, velocitySrv,    cl);
    if (depthSrv)
        dx12.SetComputeDescriptorTable(kSRV_T6, depthSrv,       cl);
    dx12.SetComputeDescriptorTable(kSRV_T7,
        m_gfx->GetTextureSRVGpuHandle(h.depthHistory[readIdx]), cl);

    dx12.SetComputeDescriptorTable(kUAV_U0,
        m_gfx->GetTextureUAVGpuHandle(h.color[h.writeIdx]), cl);
    dx12.SetComputeDescriptorTable(kUAV_U1,
        m_gfx->GetTextureUAVGpuHandle(h.variance[h.writeIdx]), cl);
    dx12.SetComputeDescriptorTable(kUAV_U2,
        m_gfx->GetTextureUAVGpuHandle(h.depthHistory[h.writeIdx]), cl);

    const uint32_t gx = (traceW + 7) / 8;
    const uint32_t gy = (traceH + 7) / 8;
    dx12.DispatchCompute(gx, gy, 1, cl);

    dx12.PushBarrier(RHI::GPUBarrier::Image(
        &h.color[h.writeIdx], RHI::ResourceState::UNORDERED_ACCESS,
        RHI::ResourceState::SHADER_RESOURCE), cl);
    dx12.PushBarrier(RHI::GPUBarrier::Image(
        &h.variance[h.writeIdx], RHI::ResourceState::UNORDERED_ACCESS,
        RHI::ResourceState::SHADER_RESOURCE), cl);
    dx12.PushBarrier(RHI::GPUBarrier::Image(
        &h.depthHistory[h.writeIdx], RHI::ResourceState::UNORDERED_ACCESS,
        RHI::ResourceState::SHADER_RESOURCE), cl);

    h.reset = false;
}
