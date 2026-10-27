#include "Graphics/SSR/SSRCompositePass.h"
#include "Graphics/SSR/SSRRootSigSlots.h"
#include "Graphics/IGraphicsDevice.h"
#include "System/Log.h"

#include <cstring>

// SSRCompositePass - additive blend of resolved SSR into HDR (pass 6).

using namespace SSR::RootSig;


void SSRCompositePass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::SSRComposite_CS, RHI::ShaderStage::CS,
                         "SSRComposite.cs.hlsl", "CSMain");

    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::SSRComposite_CS);
    if (!cs) { LOG_ERROR("SSRCompositePass: SSRComposite_CS not found"); return; }
    RHI::PipelineStateDesc d{};
    d.cs = cs;
    if (!gfx.CreatePipelineState(d, m_pso))
    { LOG_ERROR("SSRCompositePass: PSO creation failed"); return; }

    m_cb.Create(gfx, "SSRComposite.CB");

    LOG_SUCCESS("SSRCompositePass: initialised");
}

void SSRCompositePass::ReloadShaders(IGraphicsDevice& gfx)
{
    m_shaderLib.ClearCaches();
    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::SSRComposite_CS);
    if (!cs) { LOG_ERROR("SSRCompositePass::ReloadShaders: shader missing"); return; }
    RHI::PipelineStateDesc d{}; d.cs = cs;
    if (!gfx.CreatePipelineState(d, m_pso))
        LOG_ERROR("SSRCompositePass::ReloadShaders: PSO rebuild failed");
}

void SSRCompositePass::Execute(RHI::CommandList cl,
                                uint32_t w, uint32_t h,
                                uint64_t albedoSrv, uint64_t normalSrv, uint64_t surfaceSrv,
                                uint64_t depthSrv,  uint64_t ssrSrv,    uint64_t hdrUav,
                                uint64_t brdfLutSrv)
{
    if (!m_pso.IsValid() || !m_cb.IsValid() || !hdrUav) return;

    auto& dx12 = *m_gfx;

    SSRCompositePass::SSRCompositeCB cb{};
    using namespace DirectX;
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(cb.invViewProj),
        XMMatrixTranspose(XMLoadFloat4x4(&m_cam.invViewProj)));
    cb.cameraPos[0] = m_cam.cameraPos.x;
    cb.cameraPos[1] = m_cam.cameraPos.y;
    cb.cameraPos[2] = m_cam.cameraPos.z;
    cb.screenW         = w;
    cb.screenH         = h;
    // Unified-reflection overlap guard: t7 carries the RTReflectionPass output
    // (.a = conf), and the shader scales the additive term by (1 - conf).
    const bool useUnified = m_unifiedConfSrv != 0;
    cb.useUnifiedConf = useUnified ? 1u : 0u;
    if (auto* slot = m_cb.Current(dx12)) *slot = cb;

    dx12.BindComputePipelineState(m_pso, cl);
    dx12.SetComputeRootCBV(kCB, m_cb.CurrentBuffer(dx12), 0, cl);
    dx12.SetComputeDescriptorTable(kSRV_T0, albedoSrv,  cl);
    dx12.SetComputeDescriptorTable(kSRV_T1, normalSrv,  cl);
    dx12.SetComputeDescriptorTable(kSRV_T2, surfaceSrv, cl);
    dx12.SetComputeDescriptorTable(kSRV_T3, depthSrv,   cl);
    dx12.SetComputeDescriptorTable(kSRV_T4, ssrSrv,     cl);
    dx12.SetComputeDescriptorTable(kSRV_T5, brdfLutSrv, cl);
    if (useUnified)
        dx12.SetComputeDescriptorTable(kSRV_T7, m_unifiedConfSrv, cl);
    dx12.SetComputeDescriptorTable(kUAV_U0, hdrUav,     cl);

    const uint32_t gx = (w + 7) / 8;
    const uint32_t gy = (h + 7) / 8;
    dx12.DispatchCompute(gx, gy, 1, cl);
}
