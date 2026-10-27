#include "RenderGraph/RenderPass/CASPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RootSignatureLayout.h"
#include "RenderGraph/PassHelpers.h"
#include "System/Log.h"
#include <cstring>

// Compute root-signature slots shared by every compute pass, from RHI::Slots::Cmp
// (Graphics/RootSignatureLayout.h): S::CB = b0 space2, S::SRV_t0 = t0 space2, S::UAV_u0 = u0 space2.
namespace S = RHI::Slots::Cmp;

void CASPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::CAS_CS, RHI::ShaderStage::CS,
                         "CAS.cs.hlsl", "CSMain");

    if (!CreatePipelines(gfx)) return;

    LOG_SUCCESS("CASPass: initialized");
}

// Onto the live member: a failed rebuild keeps the old pipeline.
bool CASPass::CreatePipelines(IGraphicsDevice& gfx)
{
    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::CAS_CS);
    if (!cs)
    {
        LOG_ERROR("CASPass: CAS_CS shader not found");
        return false;
    }

    RHI::PipelineStateDesc pd{};
    pd.cs = cs;
    if (!gfx.CreatePipelineState(pd, m_pso))
    {
        LOG_ERROR("CASPass: PSO creation failed");
        return false;
    }
    return true;
}

void CASPass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (m_shaderLib.ClearCaches()) CreatePipelines(gfx);
}

void CASPass::BindView(ViewState& view)
{
    m_t = view.Slot() == 0 ? &m_mainTarget : &view.Data<ViewTarget>(this).t;
}

void CASPass::SetViewportSize(uint32_t w, uint32_t h)
{
    if (w != m_t->vpW || h != m_t->vpH)
    {
        m_t->vpW = w;
        m_t->vpH = h;
        m_t->texDirty = true;
    }
}

void CASPass::PrepareResources(uint32_t w, uint32_t h)
{
    SetViewportSize(w, h);
    if (m_t->texDirty)
        RebuildTextures();
}

void CASPass::RebuildTextures()
{
    if (!m_gfx || m_t->vpW == 0 || m_t->vpH == 0) return;

    m_t->output.Release(*m_gfx);
    // The shared pool supplies the target: never allocate a private one.
    if (m_t->sharedOutput)
    {
        // This early-out relies on the Renderer binding the pool before PrepareResources. Bound any later
        // (e.g. only for enabled effects at execute time), a disabled producer reaches the allocation
        // below and strands a private full-res RGBA16F.
        LOG_INFO("CASPass: using shared pool target (no private allocation)");
        m_t->texDirty = false;
        return;
    }

    // RGBA16F, SRV | UAV, created UNORDERED_ACCESS; from here its state is the device's book's.
    if (!m_t->output.Rebuild(*m_gfx, m_t->vpW, m_t->vpH))
        LOG_ERROR("CASPass: failed to create output texture (%ux%u)", m_t->vpW, m_t->vpH);
    else
        LOG_INFO("CASPass: PRIVATE output texture %ux%u allocated (%.1f MiB) — "
                 "the shared pool was not bound", m_t->vpW, m_t->vpH,
                 double(m_t->vpW) * double(m_t->vpH) * 8.0 / (1024.0 * 1024.0));

    m_t->texDirty    = false;
}

uint64_t CASPass::GetOutputSrvHandle() const
{
    // Publish only what this frame wrote (m_wroteThisFrame).
    if (!m_gfx || !m_wroteThisFrame || !Out().IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(Out());
}

void CASPass::Execute(RHI::CommandList cl)
{
    // Cleared before every early return below, so a declined frame cannot
    // publish last frame's (or another effect's) pixels downstream.
    m_wroteThisFrame = false;
    if (!m_enabled || !m_pso.IsValid()) return;
    if (!m_inputSrv || m_t->vpW == 0 || m_t->vpH == 0) return;

    if (m_t->texDirty) RebuildTextures();
    if (!Out().IsValid()) return;

    auto& gfx = *m_gfx;

    CASCB c{};
    c.width     = m_t->vpW;
    c.height    = m_t->vpH;
    c.sharpness = sharpness;

    // ---- Transition output to UAV ----
    // The target's state is the device's book's, a pool slot's too, whichever pooled producer wrote it
    // last.
    RG::TransitionTo(gfx, cl, Out(), RHI::ResourceState::UNORDERED_ACCESS);

    // ---- Dispatch ----
    const bool wrote = RG::DispatchCS(cl, m_pso, c,
        { { S::SRV_t0, m_inputSrv }, { S::UAV_u0, gfx.GetTextureUAVGpuHandle(Out()) } },
        RG::Groups2D(m_t->vpW, m_t->vpH, 8));

    // ---- Transition output -> SRV for the downstream tone map / bloom ----
    RG::TransitionTo(gfx, cl, Out(), RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
    m_wroteThisFrame = wrote;
}
