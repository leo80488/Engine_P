#include "Graphics/SSR/SSRResolvePass.h"
#include "Graphics/SSR/SSRRootSigSlots.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "System/Log.h"

#include <cstring>

// SSRResolvePass - spatial BRDF reweight (pass 3) and the HDR snapshot copy.

using namespace SSR::RootSig;

static_assert(sizeof(SSRResolvePass::SSRResolveCB) <= 256, "SSRResolveCB > 256");

void SSRResolvePass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::SSRResolve_CS, RHI::ShaderStage::CS,
                         "SSRResolve.cs.hlsl", "CSMain");

    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::SSRResolve_CS);
    if (!cs) { LOG_ERROR("SSRResolvePass: SSRResolve_CS not found"); return; }
    RHI::PipelineStateDesc d{};
    d.cs = cs;
    if (!gfx.CreatePipelineState(d, m_pso))
    { LOG_ERROR("SSRResolvePass: PSO creation failed"); return; }

    m_cb.Create(gfx, "SSRResolve.CB");

    LOG_SUCCESS("SSRResolvePass: initialised");
}

void SSRResolvePass::ReloadShaders(IGraphicsDevice& gfx)
{
    m_shaderLib.ClearCaches();
    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::SSRResolve_CS);
    if (!cs) { LOG_ERROR("SSRResolvePass::ReloadShaders: shader missing"); return; }
    RHI::PipelineStateDesc d{}; d.cs = cs;
    if (!gfx.CreatePipelineState(d, m_pso))
        LOG_ERROR("SSRResolvePass::ReloadShaders: PSO rebuild failed");
}

void SSRResolvePass::ReleaseTextures()
{
    if (!m_gfx) return;
    for (RHI::Texture* t : { &m_snapshotTex, &m_colorTex, &m_varianceTex, &m_reprojDepthTex })
    {
        if (t->IsValid()) m_gfx->DestroyTexture(*t);
        *t = {};
    }
    m_w = m_h = m_renderW = m_renderH = 0;
}

void SSRResolvePass::EnsureTextures(uint32_t renderW, uint32_t renderH,
                                    ID3D12Heap* aliasHeap,
                                    uint64_t offSnapshot, uint64_t offColor)
{
    if (!m_gfx || renderW == 0 || renderH == 0) return;

    // The trace runs at full render res (see SSRTracePass).
    const uint32_t traceW = renderW;
    const uint32_t traceH = renderH;

    if (renderW == m_renderW && renderH == m_renderH &&
        traceW == m_w && traceH == m_h &&
        m_snapshotTex.IsValid() &&
        m_colorTex.IsValid() &&
        m_varianceTex.IsValid() &&
        m_reprojDepthTex.IsValid())
        return;

    if (m_snapshotTex.IsValid())    m_gfx->DestroyTexture(m_snapshotTex);
    if (m_colorTex.IsValid())       m_gfx->DestroyTexture(m_colorTex);
    if (m_varianceTex.IsValid())    m_gfx->DestroyTexture(m_varianceTex);
    if (m_reprojDepthTex.IsValid()) m_gfx->DestroyTexture(m_reprojDepthTex);

    m_renderW = renderW; m_renderH = renderH;
    m_w = traceW;        m_h = traceH;
    m_snapshotState    = RHI::ResourceState::COPY_DST;
    m_colorState       = RHI::ResourceState::SHADER_RESOURCE;
    m_varianceState    = RHI::ResourceState::SHADER_RESOURCE;
    m_reprojDepthState = RHI::ResourceState::SHADER_RESOURCE;

    auto& dx12 = static_cast<GraphicsDX12&>(*m_gfx);

    // Snapshot: full render res copy of HDR, the pyramid's mip 0.
    RHI::TextureDesc snap{};
    snap.width  = renderW; snap.height = renderH;
    snap.format = RHI::Format::R16G16B16A16_FLOAT;
    snap.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
    snap.usage  = RHI::Usage::DEFAULT;
    snap.layout = RHI::ResourceState::COPY_DST;
    // Placed (aliased with color) when a heap is supplied, else committed.
    const bool snapOk = aliasHeap
        ? dx12.CreateTexturePlaced(snap, aliasHeap, offSnapshot, m_snapshotTex)
        : m_gfx->CreateTexture(snap, m_snapshotTex);
    if (!snapOk) LOG_ERROR("SSRResolvePass: snapshot create failed");

    // Resolved color, variance, reprojDepth: full render res.
    RHI::TextureDesc rgba{};
    rgba.width  = traceW; rgba.height = traceH;
    rgba.format = RHI::Format::R16G16B16A16_FLOAT;
    rgba.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    rgba.usage  = RHI::Usage::DEFAULT;
    rgba.layout = RHI::ResourceState::SHADER_RESOURCE;
    // color aliases snapshot's heap bytes (same size/format) when placed.
    const bool colorOk = aliasHeap
        ? dx12.CreateTexturePlaced(rgba, aliasHeap, offColor, m_colorTex)
        : m_gfx->CreateTexture(rgba, m_colorTex);
    if (!colorOk) LOG_ERROR("SSRResolvePass: color create failed");

    RHI::TextureDesc r16{};
    r16.width  = traceW; r16.height = traceH;
    r16.format = RHI::Format::R16_FLOAT;
    r16.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    r16.usage  = RHI::Usage::DEFAULT;
    r16.layout = RHI::ResourceState::SHADER_RESOURCE;
    if (!m_gfx->CreateTexture(r16, m_varianceTex))
        LOG_ERROR("SSRResolvePass: variance create failed");
    if (!m_gfx->CreateTexture(r16, m_reprojDepthTex))
        LOG_ERROR("SSRResolvePass: reprojDepth create failed");

    LOG_INFO("SSRResolvePass: resolve %ux%u (full render res) ready",
             traceW, traceH);
}

uint64_t SSRResolvePass::GetSnapshotSrv() const
{
    if (!m_gfx || !m_snapshotTex.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_snapshotTex);
}

uint64_t SSRResolvePass::GetColorSrv() const
{
    if (!m_gfx || !m_colorTex.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_colorTex);
}

uint64_t SSRResolvePass::GetVarianceSrv() const
{
    if (!m_gfx || !m_varianceTex.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_varianceTex);
}

uint64_t SSRResolvePass::GetReprojDepthSrv() const
{
    if (!m_gfx || !m_reprojDepthTex.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_reprojDepthTex);
}

void SSRResolvePass::PreSnapshotCopy(RHI::CommandList cl)
{
    if (!m_gfx || !m_snapshotTex.IsValid()) return;
    if (m_snapshotState == RHI::ResourceState::COPY_DST) return;
    auto& dx12 = *m_gfx;
    dx12.PushBarrier(RHI::GPUBarrier::Image(
        &m_snapshotTex, m_snapshotState,
        RHI::ResourceState::COPY_DST), cl);
    m_snapshotState = RHI::ResourceState::COPY_DST;
}

void SSRResolvePass::PostSnapshotCopy(RHI::CommandList cl)
{
    if (!m_gfx || !m_snapshotTex.IsValid()) return;
    if (m_snapshotState == RHI::ResourceState::SHADER_RESOURCE) return;
    auto& dx12 = *m_gfx;
    dx12.PushBarrier(RHI::GPUBarrier::Image(
        &m_snapshotTex, m_snapshotState,
        RHI::ResourceState::SHADER_RESOURCE), cl);
    m_snapshotState = RHI::ResourceState::SHADER_RESOURCE;
}

void SSRResolvePass::Execute(RHI::CommandList cl,
                             uint32_t traceW, uint32_t traceH,
                             uint64_t normalSrv, uint64_t surfaceSrv, uint64_t depthSrv,
                             uint64_t hitBufferSrv, uint64_t rayDirPDFSrv,
                             uint64_t rayLengthSrv, uint64_t rayClassSrv)
{
    if (!m_pso.IsValid() || !m_cb.IsValid()) return;
    if (traceW != m_w || traceH != m_h) return;
    if (!hitBufferSrv || !rayDirPDFSrv) return;

    auto& dx12 = *m_gfx;

    SSRResolvePass::SSRResolveCB cb{};
    using namespace DirectX;
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.invViewProj),
        XMMatrixTranspose(XMLoadFloat4x4(&m_cam.invViewProj)));
    cb.cameraPos[0] = m_cam.cameraPos.x;
    cb.cameraPos[1] = m_cam.cameraPos.y;
    cb.cameraPos[2] = m_cam.cameraPos.z;
    cb.traceW       = traceW;
    cb.traceH       = traceH;
    cb.invTraceW    = 1.0f / float(traceW);
    cb.invTraceH    = 1.0f / float(traceH);
    cb.nearZ        = m_cam.nearZ;
    cb.farZ         = m_cam.farZ;
    cb.frameIndex   = m_frameIndex;
    cb.fireflyCap   = m_fireflyCap;
    cb.renderW      = m_renderW;
    cb.renderH      = m_renderH;
    if (auto* slot = m_cb.Current(dx12)) *slot = cb;

    auto toUAV = [&](RHI::Texture& tex, RHI::ResourceState& state) {
        if (state != RHI::ResourceState::UNORDERED_ACCESS)
        {
            dx12.PushBarrier(RHI::GPUBarrier::Image(
                &tex, state, RHI::ResourceState::UNORDERED_ACCESS), cl);
            state = RHI::ResourceState::UNORDERED_ACCESS;
        }
    };
    auto toSR = [&](RHI::Texture& tex, RHI::ResourceState& state) {
        if (state != RHI::ResourceState::SHADER_RESOURCE)
        {
            dx12.PushBarrier(RHI::GPUBarrier::Image(
                &tex, state, RHI::ResourceState::SHADER_RESOURCE), cl);
            state = RHI::ResourceState::SHADER_RESOURCE;
        }
    };

    toUAV(m_colorTex,       m_colorState);
    toUAV(m_varianceTex,    m_varianceState);
    toUAV(m_reprojDepthTex, m_reprojDepthState);

    dx12.BindComputePipelineState(m_pso, cl);
    dx12.SetComputeRootCBV(kCB, m_cb.CurrentBuffer(dx12), 0, cl);
    dx12.SetComputeDescriptorTable(kSRV_T0, normalSrv,     cl);
    dx12.SetComputeDescriptorTable(kSRV_T1, surfaceSrv,    cl);
    dx12.SetComputeDescriptorTable(kSRV_T2, depthSrv,      cl);
    dx12.SetComputeDescriptorTable(kSRV_T3, hitBufferSrv,  cl);
    dx12.SetComputeDescriptorTable(kSRV_T4, rayDirPDFSrv,  cl);
    if (rayLengthSrv)
        dx12.SetComputeDescriptorTable(kSRV_T5, rayLengthSrv, cl);
    if (rayClassSrv)
        dx12.SetComputeDescriptorTable(kSRV_T6, rayClassSrv, cl);
    dx12.SetComputeDescriptorTable(kUAV_U0,
        m_gfx->GetTextureUAVGpuHandle(m_colorTex), cl);
    dx12.SetComputeDescriptorTable(kUAV_U1,
        m_gfx->GetTextureUAVGpuHandle(m_varianceTex), cl);
    dx12.SetComputeDescriptorTable(kUAV_U2,
        m_gfx->GetTextureUAVGpuHandle(m_reprojDepthTex), cl);

    const uint32_t gx = (traceW + 7) / 8;
    const uint32_t gy = (traceH + 7) / 8;
    dx12.DispatchCompute(gx, gy, 1, cl);

    toSR(m_colorTex,       m_colorState);
    toSR(m_varianceTex,    m_varianceState);
    toSR(m_reprojDepthTex, m_reprojDepthState);
}
