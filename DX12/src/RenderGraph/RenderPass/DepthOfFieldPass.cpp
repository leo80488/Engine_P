#include "RenderGraph/RenderPass/DepthOfFieldPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RootSignatureLayout.h"
#include "RenderGraph/PassHelpers.h"
#include "System/Log.h"

// Compute root-signature slots (shared compute layout, space2; RHI::Slots::Cmp in
// Graphics/RootSignatureLayout.h): SRV_t0 = HDR colour, SRV_t1 = depth,
// UAV_u0 = output.
namespace S = RHI::Slots::Cmp;

void DepthOfFieldPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::DepthOfField_CS, RHI::ShaderStage::CS,
                         "DepthOfField.cs.hlsl", "CSMain");

    if (!CreatePipelines(gfx)) return;

    LOG_SUCCESS("DepthOfFieldPass: initialized");
}

// Onto the live member: a failed rebuild keeps the old pipeline.
bool DepthOfFieldPass::CreatePipelines(IGraphicsDevice& gfx)
{
    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::DepthOfField_CS);
    if (!cs)
    {
        LOG_ERROR("DepthOfFieldPass: DepthOfField_CS shader not found");
        return false;
    }

    RHI::PipelineStateDesc pd{};
    pd.cs = cs;
    if (!gfx.CreatePipelineState(pd, m_pso))
    {
        LOG_ERROR("DepthOfFieldPass: PSO creation failed");
        return false;
    }
    return true;
}

void DepthOfFieldPass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (m_shaderLib.ClearCaches()) CreatePipelines(gfx);
}

void DepthOfFieldPass::BindView(ViewState& view)
{
    m_t = view.Slot() == 0 ? &m_mainTarget : &view.Data<ViewTarget>(this).t;
}

void DepthOfFieldPass::SetViewportSize(uint32_t w, uint32_t h)
{
    if (w != m_t->vpW || h != m_t->vpH)
    {
        m_t->vpW = w;
        m_t->vpH = h;
        m_t->texDirty = true;
    }
}

void DepthOfFieldPass::PrepareResources(uint32_t w, uint32_t h)
{
    SetViewportSize(w, h);
    if (m_t->texDirty)
        RebuildTextures();
}

void DepthOfFieldPass::RebuildTextures()
{
    if (!m_gfx || m_t->vpW == 0 || m_t->vpH == 0) return;

    m_t->output.Release(*m_gfx);
    // The shared pool supplies the target: never allocate a private one.
    if (m_t->sharedOutput)
    {
        // This holds only because the Renderer binds the pool (PostPool stage) BEFORE
        // PrepareResources: bound later, a disabled producer would reach the allocation below
        // and strand a full-res RGBA16F.
        LOG_INFO("DepthOfFieldPass: using shared pool target (no private allocation)");
        m_t->texDirty = false;
        return;
    }

    // RGBA16F, SRV|UAV, created UNORDERED_ACCESS; its state from here is the device's
    // book's.
    if (!m_t->output.Rebuild(*m_gfx, m_t->vpW, m_t->vpH))
        LOG_ERROR("DepthOfFieldPass: failed to create output texture (%ux%u)", m_t->vpW, m_t->vpH);
    else
        LOG_INFO("DepthOfFieldPass: PRIVATE output texture %ux%u allocated (%.1f MiB) — "
                 "the shared pool was not bound", m_t->vpW, m_t->vpH,
                 double(m_t->vpW) * double(m_t->vpH) * 8.0 / (1024.0 * 1024.0));

    m_t->texDirty    = false;
}

uint64_t DepthOfFieldPass::GetOutputSrvHandle() const
{
    // Publish only what this frame actually wrote (see m_wroteThisFrame).
    if (!m_gfx || !m_wroteThisFrame || !Out().IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(Out());
}

void DepthOfFieldPass::Execute(RHI::CommandList cl)
{
    // Cleared before every early return below, so a declined frame cannot
    // publish last frame's (or another effect's) pixels downstream.
    m_wroteThisFrame = false;
    if (!m_enabled || !m_pso.IsValid()) return;
    if (!m_inputSrv || !m_depthSrv || m_t->vpW == 0 || m_t->vpH == 0) return;

    if (m_t->texDirty) RebuildTextures();
    if (!Out().IsValid()) return;

    auto& gfx = *m_gfx;

    DoFCB c{};
    c.width           = m_t->vpW;
    c.height          = m_t->vpH;
    c.nearZ           = m_near;
    c.farZ            = m_far;
    c.focusDistance   = focusDistance;
    c.focusRange      = focusRange;
    c.transitionRange = transitionRange;
    c.maxRadius       = maxRadius;

    // The target's state is the device's book's - a pool slot's too, whichever pooled
    // producer wrote it last.
    RG::TransitionTo(gfx, cl, Out(), RHI::ResourceState::UNORDERED_ACCESS);

    const bool wrote = RG::DispatchCS(cl, m_pso, c,
        { { S::SRV_t0, m_inputSrv }, { S::SRV_t1, m_depthSrv },
          { S::UAV_u0, gfx.GetTextureUAVGpuHandle(Out()) } },
        RG::Groups2D(m_t->vpW, m_t->vpH, 8));

    RG::TransitionTo(gfx, cl, Out(), RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
    m_wroteThisFrame = wrote;
}
