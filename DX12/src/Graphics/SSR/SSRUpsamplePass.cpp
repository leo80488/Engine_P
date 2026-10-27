#include "Graphics/SSR/SSRUpsamplePass.h"
#include "Graphics/SSR/SSRRootSigSlots.h"
#include "Graphics/IGraphicsDevice.h"
#include "System/Log.h"

#include <cstring>

// SSRUpsamplePass - variance-driven bilateral blur (pass 5): reads temporal color +
// variance by UV (SampleLevel), writes the render-res reflection texture.

using namespace SSR::RootSig;

static_assert(sizeof(SSRUpsamplePass::SSRUpsampleCB) <= 256, "SSRUpsampleCB > 256");
// The Execute loop routes temporal -> ping -> color; with more than 2 iterations the
// middle passes would read and write m_pingTex simultaneously. Add real ping-pong
// before raising this.
static_assert(SSRUpsamplePass::kIterations == 2, "Execute chain assumes 2 iterations");

void SSRUpsamplePass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;
    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::SSRUpsample_CS, RHI::ShaderStage::CS,
                         "SSRUpsample.cs.hlsl", "CSMain");

    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::SSRUpsample_CS);
    if (!cs) { LOG_ERROR("SSRUpsamplePass: shader missing"); return; }
    RHI::PipelineStateDesc d{}; d.cs = cs;
    if (!gfx.CreatePipelineState(d, m_pso))
    { LOG_ERROR("SSRUpsamplePass: PSO failed"); return; }

    m_cb.Create(gfx, "SSRUpsample.CB");

    LOG_SUCCESS("SSRUpsamplePass: initialised");
}

void SSRUpsamplePass::ReloadShaders(IGraphicsDevice& gfx)
{
    m_shaderLib.ClearCaches();
    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::SSRUpsample_CS);
    if (!cs) { LOG_ERROR("SSRUpsamplePass::ReloadShaders: shader missing"); return; }
    RHI::PipelineStateDesc d{}; d.cs = cs;
    if (!gfx.CreatePipelineState(d, m_pso))
        LOG_ERROR("SSRUpsamplePass::ReloadShaders: PSO rebuild failed");
}

void SSRUpsamplePass::ReleaseTextures()
{
    if (!m_gfx) return;
    for (RHI::Texture* t : { &m_colorTex, &m_pingTex })
    {
        if (t->IsValid()) m_gfx->DestroyTexture(*t);
        *t = {};
    }
    m_w = m_h = 0;
}

void SSRUpsamplePass::EnsureTexture(uint32_t w, uint32_t h)
{
    if (!m_gfx || w == 0 || h == 0) return;
    if (w == m_w && h == m_h && m_colorTex.IsValid() && m_pingTex.IsValid()) return;

    if (m_colorTex.IsValid()) m_gfx->DestroyTexture(m_colorTex);
    if (m_pingTex.IsValid())  m_gfx->DestroyTexture(m_pingTex);
    m_w = w; m_h = h;

    RHI::TextureDesc td{};
    td.width = w; td.height = h;
    td.format = RHI::Format::R16G16B16A16_FLOAT;
    td.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    td.usage = RHI::Usage::DEFAULT;
    td.layout = RHI::ResourceState::SHADER_RESOURCE;
    if (!m_gfx->CreateTexture(td, m_colorTex))
        LOG_ERROR("SSRUpsamplePass: output create failed");
    if (!m_gfx->CreateTexture(td, m_pingTex))
    {
        LOG_ERROR("SSRUpsamplePass: ping scratch create failed");
        // Keep the pair atomic: with a valid color SRV but no ping, Execute
        // would early-out forever while LightingPass / WaterPass / composite
        // keep sampling a frozen texture. A 0 SRV makes SSRSubsystem fall back
        // to the raw trace result instead.
        if (m_colorTex.IsValid()) m_gfx->DestroyTexture(m_colorTex);
    }
    LOG_INFO("SSRUpsamplePass: buffers %ux%u ready (%u a-trous iterations)",
             w, h, kIterations);
}

uint64_t SSRUpsamplePass::GetColorSrv() const
{
    if (!m_gfx || !m_colorTex.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_colorTex);
}

void SSRUpsamplePass::Execute(RHI::CommandList cl,
                              uint32_t w, uint32_t h,
                              uint64_t temporalSrv, uint64_t varianceSrv,
                              uint64_t depthSrv, uint64_t normalSrv, uint64_t surfaceSrv)
{
    if (!m_pso.IsValid() || !m_cb.IsValid()) return;
    if (w != m_w || h != m_h) return;
    if (!temporalSrv || !varianceSrv) return;
    if (!m_colorTex.IsValid() || !m_pingTex.IsValid()) return;

    auto& dx12 = *m_gfx;

    auto* mapped = m_cb.Current(dx12);
    if (!mapped) return;
    uint8_t* cbBase = mapped->bytes;
    const RHI::GPUBuffer& cbBuf = m_cb.CurrentBuffer(dx12);

    SSRUpsamplePass::SSRUpsampleCB cb{};
    using namespace DirectX;
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.invViewProj),
        XMMatrixTranspose(XMLoadFloat4x4(&m_cam.invViewProj)));
    cb.screenW = w; cb.screenH = h;
    cb.invScreenW = 1.0f / float(w);
    cb.invScreenH = 1.0f / float(h);
    cb.nearZ = m_cam.nearZ;
    cb.farZ  = m_cam.farZ;

    dx12.BindComputePipelineState(m_pso, cl);
    dx12.SetComputeDescriptorTable(kSRV_T1, varianceSrv, cl);
    dx12.SetComputeDescriptorTable(kSRV_T2, depthSrv,    cl);
    dx12.SetComputeDescriptorTable(kSRV_T3, normalSrv,   cl);
    dx12.SetComputeDescriptorTable(kSRV_T4, surfaceSrv,  cl);

    const uint32_t gx = (w + 7) / 8;
    const uint32_t gy = (h + 7) / 8;

    // A-trous iterations: temporal -> ping -> color. Iteration i taps with
    // stepSize = 1 << i; each writes its own 256-byte CB slot (same-CL dispatches sharing
    // one slot would all read the LAST CPU write).
    for (uint32_t i = 0; i < kIterations; ++i)
    {
        cb.stepSize = 1u << i;
        std::memcpy(cbBase + i * kCBStride, &cb, sizeof(cb));

        const bool     last   = (i == kIterations - 1);
        RHI::Texture&  dstTex = last ? m_colorTex : m_pingTex;
        const uint64_t srcSrv = (i == 0)
            ? temporalSrv
            : m_gfx->GetTextureSRVGpuHandle(m_pingTex);

        dx12.PushBarrier(RHI::GPUBarrier::Image(
            &dstTex, RHI::ResourceState::SHADER_RESOURCE,
            RHI::ResourceState::UNORDERED_ACCESS), cl);

        dx12.SetComputeRootCBV(kCB, cbBuf, i * kCBStride, cl);
        dx12.SetComputeDescriptorTable(kSRV_T0, srcSrv, cl);
        dx12.SetComputeDescriptorTable(kUAV_U0,
            m_gfx->GetTextureUAVGpuHandle(dstTex), cl);

        dx12.DispatchCompute(gx, gy, 1, cl);

        dx12.PushBarrier(RHI::GPUBarrier::Image(
            &dstTex, RHI::ResourceState::UNORDERED_ACCESS,
            RHI::ResourceState::SHADER_RESOURCE), cl);
    }
}
