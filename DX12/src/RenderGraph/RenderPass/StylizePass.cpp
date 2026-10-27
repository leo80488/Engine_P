#include "RenderGraph/RenderPass/StylizePass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RootSignatureLayout.h"
#include "RenderGraph/PassHelpers.h"
#include "System/Log.h"

// Compute root-signature slots, from RHI::Slots::Cmp (Graphics/RootSignatureLayout.h):
// S::CB = b0 space2, S::SRV_t0 = t0 space2, S::UAV_u0 = u0 space2.
namespace S = RHI::Slots::Cmp;

void StylizePass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::Stylize_CS, RHI::ShaderStage::CS,
                         "Stylize.cs.hlsl", "CSMain");

    if (!CreatePipelines(gfx)) return;

    LOG_SUCCESS("StylizePass: initialized");
}

// Onto the live member: a failed rebuild keeps the old pipeline.
bool StylizePass::CreatePipelines(IGraphicsDevice& gfx)
{
    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::Stylize_CS);
    if (!cs)
    {
        LOG_ERROR("StylizePass: Stylize_CS shader not found");
        return false;
    }

    RHI::PipelineStateDesc pd{};
    pd.cs = cs;
    if (!gfx.CreatePipelineState(pd, m_pso))
    {
        LOG_ERROR("StylizePass: PSO creation failed");
        return false;
    }
    return true;
}

void StylizePass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (m_shaderLib.ClearCaches()) CreatePipelines(gfx);
}

void StylizePass::BindView(ViewState& view)
{
    m_t = view.Slot() == 0 ? &m_mainTarget : &view.Data<ViewTarget>(this).t;
}

void StylizePass::SetViewportSize(uint32_t w, uint32_t h)
{
    if (w != m_t->vpW || h != m_t->vpH)
    {
        m_t->vpW = w;
        m_t->vpH = h;
        m_t->texDirty = true;
    }
}

void StylizePass::PrepareResources(uint32_t w, uint32_t h)
{
    SetViewportSize(w, h);
    if (m_t->texDirty)
        RebuildTextures();
}

void StylizePass::RebuildTextures()
{
    if (!m_gfx || m_t->vpW == 0 || m_t->vpH == 0) return;

    m_t->output.Release(*m_gfx);
    // The shared pool supplies the target: never allocate a private one.
    if (m_t->sharedOutput)
    {
        // This early-out saves the whole private full-res RGBA16F. It fires only because the
        // pool is bound before PrepareResources (OnPrepare's PostPool stage), enabled or not:
        // bound later, a disabled producer would reach the allocation below and strand one.
        LOG_INFO("StylizePass: using shared pool target (no private allocation)");
        m_t->texDirty = false;
        return;
    }

    // RGBA16F, SRV|UAV, created UNORDERED_ACCESS; from here its state is the device's
    // book's.
    if (!m_t->output.Rebuild(*m_gfx, m_t->vpW, m_t->vpH))
        LOG_ERROR("StylizePass: failed to create output texture (%ux%u)", m_t->vpW, m_t->vpH);
    else
        LOG_INFO("StylizePass: PRIVATE output texture %ux%u allocated (%.1f MiB) — "
                 "the shared pool was not bound", m_t->vpW, m_t->vpH,
                 double(m_t->vpW) * double(m_t->vpH) * 8.0 / (1024.0 * 1024.0));

    m_t->texDirty    = false;
}

uint64_t StylizePass::GetOutputSrvHandle() const
{
    // Publish only what this frame wrote (see m_wroteThisFrame).
    if (!m_gfx || !m_wroteThisFrame || !Out().IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(Out());
}

void StylizePass::Execute(RHI::CommandList cl)
{
    // Cleared before every early return below, so a declined frame cannot
    // publish last frame's (or another effect's) pixels downstream.
    m_wroteThisFrame = false;
    if (!m_enabled || !m_pso.IsValid()) return;
    if (!m_inputSrv || m_t->vpW == 0 || m_t->vpH == 0) return;

    if (m_t->texDirty) RebuildTextures();
    if (!Out().IsValid()) return;

    auto& gfx = *m_gfx;

    StylizeCB c{};
    c.width               = m_t->vpW;
    c.height              = m_t->vpH;
    c.kuwaharaOn          = kuwaharaOn ? 1u : 0u;
    c.posterizeOn         = posterizeOn ? 1u : 0u;
    c.halftoneOn          = halftoneOn ? 1u : 0u;
    c.ditherOn            = ditherOn ? 1u : 0u;
    c.crosshatchOn        = crosshatchOn ? 1u : 0u;
    c.kuwaharaRadius      = kuwaharaRadius;
    c.posterizeLevels     = static_cast<float>(posterizeLevels);
    c.halftoneCell        = halftoneCell;
    c.halftoneAngle       = halftoneAngle;
    c.ditherLevels        = static_cast<float>(ditherLevels);
    c.crosshatchDensity   = crosshatchDensity;
    c.crosshatchThickness = crosshatchThickness;
    c.pixelateOn          = pixelateOn ? 1u : 0u;
    c.pixelSize           = pixelSize;
    c.sourceOutline       = sourceOutline;
    if (!m_depthSrv || !m_normalSrv || !m_materialSrv) c.sourceOutline.enabled = 0;

    // The target's state is the device's book's - a pool slot's too, whichever pooled
    // producer wrote it last.
    RG::TransitionTo(gfx, cl, Out(), RHI::ResourceState::UNORDERED_ACCESS);

    // The outline's inputs are optional: a zero handle binds nothing.
    const bool wrote = RG::DispatchCS(cl, m_pso, c,
        { { S::SRV_t0, m_inputSrv }, { S::SRV_t1, m_depthSrv }, { S::SRV_t2, m_normalSrv },
          { S::SRV_t3, m_materialSrv }, { S::UAV_u0, gfx.GetTextureUAVGpuHandle(Out()) } },
        RG::Groups2D(m_t->vpW, m_t->vpH, 8));

    RG::TransitionTo(gfx, cl, Out(), RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
    m_wroteThisFrame = wrote;
}
