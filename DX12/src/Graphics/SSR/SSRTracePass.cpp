#include "Graphics/SSR/SSRTracePass.h"
#include "Graphics/SSR/SSRRootSigSlots.h"
#include "Graphics/IGraphicsDevice.h"
#include "System/Log.h"

#include <cstring>

// SSRPass - stochastic Hi-Z ray gen / trace (pass 2). The algorithm and the per-frame
// sub-pixel jitter live in shaders/SSRTrace.cs.hlsl; this TU only packs the CB,
// dispatches and owns the textures.

using namespace SSR::RootSig;

static_assert(sizeof(SSRPass::SSRTraceCB) <= 256, "SSRTraceCB > 256");

void SSRPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::SSRTrace_CS, RHI::ShaderStage::CS,
                         "SSRTrace.cs.hlsl", "CSMain");

    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::SSRTrace_CS);
    if (!cs) { LOG_ERROR("SSRPass: SSRTrace_CS not found"); return; }
    RHI::PipelineStateDesc d{};
    d.cs = cs;
    if (!gfx.CreatePipelineState(d, m_pso))
    { LOG_ERROR("SSRPass: PSO creation failed"); return; }

    m_cb.Create(gfx, "SSRTrace.CB");

    LOG_SUCCESS("SSRPass: initialised (stochastic Hi-Z trace)");
}

void SSRPass::ReloadShaders(IGraphicsDevice& gfx)
{
    m_shaderLib.ClearCaches();
    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::SSRTrace_CS);
    if (!cs) { LOG_ERROR("SSRPass::ReloadShaders: shader missing"); return; }
    RHI::PipelineStateDesc d{}; d.cs = cs;
    if (!gfx.CreatePipelineState(d, m_pso))
        LOG_ERROR("SSRPass::ReloadShaders: PSO rebuild failed");
}

void SSRPass::ReleaseTextures()
{
    if (!m_gfx) return;
    for (RHI::Texture* t : { &m_resultTex, &m_rayDirPDFTex, &m_rayLengthTex, &m_rayClassTex })
    {
        if (t->IsValid()) m_gfx->DestroyTexture(*t);
        *t = {};
    }
    m_w = m_h = m_renderW = m_renderH = 0;
}

void SSRPass::EnsureTexture(uint32_t renderW, uint32_t renderH)
{
    if (!m_gfx || renderW == 0 || renderH == 0) return;

    // The trace runs at FULL render resolution: the half-res + sub-pixel jitter variant
    // produced visible noise without TAA and sub-pixel reflection misalignment. Re-enable
    // half-res only alongside a stable temporal denoiser that does not need TAA's jitter.
    const uint32_t traceW = renderW;
    const uint32_t traceH = renderH;

    if (renderW == m_renderW && renderH == m_renderH &&
        traceW == m_w && traceH == m_h &&
        m_resultTex.IsValid() &&
        m_rayDirPDFTex.IsValid() &&
        m_rayLengthTex.IsValid() &&
        m_rayClassTex.IsValid()) return;

    if (m_resultTex.IsValid())    m_gfx->DestroyTexture(m_resultTex);
    if (m_rayDirPDFTex.IsValid()) m_gfx->DestroyTexture(m_rayDirPDFTex);
    if (m_rayLengthTex.IsValid()) m_gfx->DestroyTexture(m_rayLengthTex);
    if (m_rayClassTex.IsValid())  m_gfx->DestroyTexture(m_rayClassTex);

    m_renderW = renderW; m_renderH = renderH;
    m_w = traceW; m_h = traceH;

    RHI::TextureDesc td{};
    td.width      = traceW;
    td.height     = traceH;
    td.format     = RHI::Format::R16G16B16A16_FLOAT;
    td.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    td.usage      = RHI::Usage::DEFAULT;
    td.layout     = RHI::ResourceState::SHADER_RESOURCE;

    if (!m_gfx->CreateTexture(td, m_resultTex))
    { LOG_ERROR("SSRPass: hit buffer create failed (%ux%u)", traceW, traceH); return; }

    if (!m_gfx->CreateTexture(td, m_rayDirPDFTex))
    { LOG_ERROR("SSRPass: rayDirPDF create failed"); return; }

    RHI::TextureDesc td2 = td;
    td2.format = RHI::Format::R16_FLOAT;
    if (!m_gfx->CreateTexture(td2, m_rayLengthTex))
    { LOG_ERROR("SSRPass: rayLength create failed"); return; }

    td2.format = RHI::Format::R8_UINT;
    if (!m_gfx->CreateTexture(td2, m_rayClassTex))
    { LOG_ERROR("SSRPass: rayClass create failed"); return; }

    m_resultState    = RHI::ResourceState::SHADER_RESOURCE;
    m_rayDirPDFState = RHI::ResourceState::SHADER_RESOURCE;
    m_rayLengthState = RHI::ResourceState::SHADER_RESOURCE;
    m_rayClassState  = RHI::ResourceState::SHADER_RESOURCE;
    LOG_INFO("SSRPass: trace outputs %ux%u (full render res) ready",
             traceW, traceH);
}

uint64_t SSRPass::GetResultSrv() const
{
    if (!m_gfx || !m_resultTex.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_resultTex);
}

uint64_t SSRPass::GetRayDirPDFSrv() const
{
    if (!m_gfx || !m_rayDirPDFTex.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_rayDirPDFTex);
}

uint64_t SSRPass::GetRayLengthSrv() const
{
    if (!m_gfx || !m_rayLengthTex.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_rayLengthTex);
}

uint64_t SSRPass::GetRayClassSrv() const
{
    if (!m_gfx || !m_rayClassTex.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_rayClassTex);
}

void SSRPass::Execute(RHI::CommandList cl,
                      uint64_t normalSrv,
                      uint64_t surfaceSrv,
                      uint64_t depthSrv,
                      uint64_t depthHierSrv,
                      uint64_t hdrPyramidSrv,
                      uint64_t velocitySrv,
                      uint64_t skyRadianceSrv,
                      uint32_t skyRadianceMips,
                      bool hasSkyRadiance)
{
    if (!m_pso.IsValid() || !m_resultTex.IsValid() || !m_cb.IsValid()) return;
    if (!normalSrv || !surfaceSrv || !depthSrv || !depthHierSrv) return;
    if (!hdrPyramidSrv) return;

    auto& dx12 = *m_gfx;

    SSRPass::SSRTraceCB cb{};
    using namespace DirectX;
    auto storeT = [](float out[16], const XMFLOAT4X4& m) {
        XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(out),
            XMMatrixTranspose(XMLoadFloat4x4(&m)));
    };
    storeT(cb.viewProj,    m_cam.viewProj);
    storeT(cb.invViewProj, m_cam.invViewProj);
    cb.cameraPos[0] = m_cam.cameraPos.x;
    cb.cameraPos[1] = m_cam.cameraPos.y;
    cb.cameraPos[2] = m_cam.cameraPos.z;
    cb.nearZ           = m_cam.nearZ;
    cb.farZ            = m_cam.farZ;
    cb.renderW         = m_renderW;
    cb.renderH         = m_renderH;
    cb.traceW          = m_w;
    cb.traceH          = m_h;
    cb.hizMipCount     = m_hizMipCount ? m_hizMipCount : 1;
    cb.maxRayLength       = m_maxRayLength;
    cb.roughnessCutoff    = m_roughnessCutoff;
    cb.frameIndex         = m_frameIndex;
    cb.traceThickness     = m_traceThickness;
    cb.hizMostDetailedLvl = m_hizMostDetailed;
    cb.coneMipMax         = m_coneMipMax;
    cb.depthBiasFactor    = m_depthBiasFactor;
    cb.finishLinearSteps  = m_finishLinearSteps;
    cb.skyRadianceMips    = skyRadianceMips ? skyRadianceMips : 1u;
    cb.hasSkyRadiance     = hasSkyRadiance ? 1u : 0u;
    cb.roughFadeStart     = (m_roughnessCutoff > 0.15f)
                          ? (m_roughnessCutoff - 0.15f) : 0.0f;
    if (auto* slot = m_cb.Current(dx12)) *slot = cb;

    auto toUAV = [&](RHI::Texture& tex, RHI::ResourceState& state) {
        if (state != RHI::ResourceState::UNORDERED_ACCESS)
        {
            dx12.PushBarrier(RHI::GPUBarrier::Image(
                &tex, state, RHI::ResourceState::UNORDERED_ACCESS), cl);
            state = RHI::ResourceState::UNORDERED_ACCESS;
        }
    };
    toUAV(m_resultTex,    m_resultState);
    toUAV(m_rayDirPDFTex, m_rayDirPDFState);
    toUAV(m_rayLengthTex, m_rayLengthState);
    toUAV(m_rayClassTex,  m_rayClassState);

    dx12.BindComputePipelineState(m_pso, cl);
    dx12.SetComputeRootCBV(kCB, m_cb.CurrentBuffer(dx12), 0, cl);
    dx12.SetComputeDescriptorTable(kSRV_T0, normalSrv,     cl);
    dx12.SetComputeDescriptorTable(kSRV_T1, surfaceSrv,    cl);
    dx12.SetComputeDescriptorTable(kSRV_T2, depthSrv,      cl);
    dx12.SetComputeDescriptorTable(kSRV_T3, depthHierSrv,  cl);
    dx12.SetComputeDescriptorTable(kSRV_T4, hdrPyramidSrv, cl);
    if (skyRadianceSrv)
        dx12.SetComputeDescriptorTable(kSRV_T5, skyRadianceSrv, cl);
    if (velocitySrv)
        dx12.SetComputeDescriptorTable(kSRV_T7, velocitySrv, cl);
    dx12.SetComputeDescriptorTable(kUAV_U0,
        m_gfx->GetTextureUAVGpuHandle(m_resultTex), cl);
    dx12.SetComputeDescriptorTable(kUAV_U1,
        m_gfx->GetTextureUAVGpuHandle(m_rayDirPDFTex), cl);
    dx12.SetComputeDescriptorTable(kUAV_U2,
        m_gfx->GetTextureUAVGpuHandle(m_rayLengthTex), cl);
    dx12.SetComputeDescriptorTable(kUAV_U3,
        m_gfx->GetTextureUAVGpuHandle(m_rayClassTex), cl);

    const uint32_t gx = (m_w + 7) / 8;
    const uint32_t gy = (m_h + 7) / 8;
    dx12.DispatchCompute(gx, gy, 1, cl);
}
