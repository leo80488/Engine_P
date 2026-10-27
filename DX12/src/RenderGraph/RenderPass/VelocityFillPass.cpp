#include "RenderGraph/RenderPass/VelocityFillPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RootSignatureLayout.h"
#include "RenderGraph/PassHelpers.h"
#include "System/Log.h"
#include "System/Check.h"

// Shared compute root-signature slots (space2) from RHI::Slots::Cmp:
// S::CB = b0, S::SRV_t0 = t0, S::UAV_u0 = u0.
namespace S = RHI::Slots::Cmp;

void VelocityFillPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::VelocityFill_CS, RHI::ShaderStage::CS,
                         "VelocityFill.cs.hlsl", "CSMain");

    if (!CreatePipelines(gfx)) return;

    LOG_SUCCESS("VelocityFillPass: initialized");
}

// Creates into the live member; a failed rebuild keeps the old pipeline.
bool VelocityFillPass::CreatePipelines(IGraphicsDevice& gfx)
{
    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::VelocityFill_CS);
    if (!cs)
    {
        LOG_ERROR("VelocityFillPass: VelocityFill_CS shader not found");
        return false;
    }

    RHI::PipelineStateDesc pd{};
    pd.cs = cs;
    if (!gfx.CreatePipelineState(pd, m_pso))
    {
        LOG_ERROR("VelocityFillPass: PSO creation failed");
        return false;
    }
    return true;
}

void VelocityFillPass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (m_shaderLib.ClearCaches()) CreatePipelines(gfx);
}

void VelocityFillPass::Execute(RHI::CommandList cl, uint64_t depthSrv, uint64_t velocityUav)
{
    if (!m_enabled || !m_pso.IsValid()) return;
    if (m_vpW == 0 || m_vpH == 0) return;

    // A missing descriptor would dispatch against an unbound slot. Declining is safe
    // (the sky keeps zero velocity) but invisible - a skipped fill and a working one
    // look alike until the camera moves and the sky ghosts - so say so, once.
    if (!ENGINE_CHECK_ONCE(depthSrv != 0 && velocityUav != 0,
                           "VelocityFillPass: missing descriptor (depthSrv=%llu velocityUav=%llu) "
                           "- sky motion vectors will stay zero and the sky will ghost under "
                           "camera motion with FSR2/DLSS",
                           static_cast<unsigned long long>(depthSrv),
                           static_cast<unsigned long long>(velocityUav)))
        return;

    // Proof of life, once: otherwise "the fill ran" and "the fill was never armed"
    // are indistinguishable in the log.
    if (!m_loggedFirstDispatch)
    {
        m_loggedFirstDispatch = true;
        LOG_INFO("VelocityFillPass: filling sky motion vectors at %ux%u", m_vpW, m_vpH);
    }

    VelocityFillCB c{};
    c.invViewProjNoJitter = m_invViewProjNoJitter;
    c.prevViewProj        = m_prevViewProj;
    c.width               = m_vpW;
    c.height              = m_vpH;
    RG::DispatchCS(cl, m_pso, c, { { S::SRV_t0, depthSrv }, { S::UAV_u0, velocityUav } },
                   RG::Groups2D(m_vpW, m_vpH, 8));
}
