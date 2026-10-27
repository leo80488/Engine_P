#include "RenderGraph/RenderPass/UnderwaterPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RootSignatureLayout.h"
#include "RenderGraph/PassHelpers.h"
#include "System/Log.h"

// Compute root-signature slots (shared by every compute pass, space2), from
// RHI::Slots::Cmp (Graphics/RootSignatureLayout.h): S::CB = b0, S::SRV_t0 = t0,
// S::UAV_u0 = u0.
namespace S = RHI::Slots::Cmp;

void UnderwaterPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::Underwater_CS, RHI::ShaderStage::CS,
                         "Underwater.cs.hlsl", "CSMain");

    if (!CreatePipelines(gfx)) return;

    LOG_SUCCESS("UnderwaterPass: initialized");
}

// Creates into the live member; a failed rebuild keeps the old pipeline.
bool UnderwaterPass::CreatePipelines(IGraphicsDevice& gfx)
{
    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::Underwater_CS);
    if (!cs)
    {
        LOG_ERROR("UnderwaterPass: Underwater_CS shader not found");
        return false;
    }

    RHI::PipelineStateDesc pd{};
    pd.cs = cs;
    if (!gfx.CreatePipelineState(pd, m_pso))
    {
        LOG_ERROR("UnderwaterPass: PSO creation failed");
        return false;
    }
    return true;
}

void UnderwaterPass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (m_shaderLib.ClearCaches()) CreatePipelines(gfx);
}

void UnderwaterPass::BindView(ViewState& view)
{
    m_t = view.Slot() == 0 ? &m_mainTarget : &view.Data<ViewTarget>(this).t;
}

void UnderwaterPass::SetViewportSize(uint32_t w, uint32_t h)
{
    if (w != m_t->vpW || h != m_t->vpH)
    {
        m_t->vpW = w;
        m_t->vpH = h;
        m_t->texDirty = true;
    }
}

void UnderwaterPass::PrepareResources(uint32_t w, uint32_t h)
{
    SetViewportSize(w, h);
    if (m_t->texDirty)
        RebuildTextures();
}

void UnderwaterPass::RebuildTextures()
{
    if (!m_gfx || m_t->vpW == 0 || m_t->vpH == 0) return;

    m_t->output.Release(*m_gfx);
    // The shared pool supplies the target: never allocate a private one.
    if (m_t->sharedOutput)
    {
        // This early-out saves a full-res RGBA16F target; it works because the Renderer
        // binds the pool (PostPool stage) before PrepareResources.
        LOG_INFO("UnderwaterPass: using shared pool target (no private allocation)");
        m_t->texDirty = false;
        return;
    }

    // RGBA16F, SRV|UAV, created UNORDERED_ACCESS; from here its state is the device's
    // book's.
    if (!m_t->output.Rebuild(*m_gfx, m_t->vpW, m_t->vpH))
        LOG_ERROR("UnderwaterPass: failed to create output texture (%ux%u)", m_t->vpW, m_t->vpH);
    else
        LOG_INFO("UnderwaterPass: PRIVATE output texture %ux%u allocated (%.1f MiB) — "
                 "the shared pool was not bound", m_t->vpW, m_t->vpH,
                 double(m_t->vpW) * double(m_t->vpH) * 8.0 / (1024.0 * 1024.0));

    m_t->texDirty    = false;
}

uint64_t UnderwaterPass::GetOutputSrvHandle() const
{
    // Publish only what this frame wrote (see m_wroteThisFrame).
    if (!m_gfx || !m_wroteThisFrame || !Out().IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(Out());
}

void UnderwaterPass::Execute(RHI::CommandList cl)
{
    // Cleared before every early return below, so a declined frame cannot
    // publish last frame's (or another effect's) pixels downstream.
    m_wroteThisFrame = false;
    if (!m_enabled || !m_pso.IsValid()) return;
    if (!m_inputSrv || m_t->vpW == 0 || m_t->vpH == 0) return;

    if (m_t->texDirty) RebuildTextures();
    if (!Out().IsValid()) return;

    auto& gfx = *m_gfx;

    UnderwaterCB c{};
    c.width      = m_t->vpW;
    c.height     = m_t->vpH;
    c.time       = m_time;
    c.strength   = strength;
    c.scale      = scale;
    c.speed      = speed;
    c.tintAmount = tintAmount;
    c.tint[0]    = tint.x;
    c.tint[1]    = tint.y;
    c.tint[2]    = tint.z;

    // The target's state is the device's book's - a pool slot's too, whichever pooled
    // producer wrote it last.
    RG::TransitionTo(gfx, cl, Out(), RHI::ResourceState::UNORDERED_ACCESS);

    const bool wrote = RG::DispatchCS(cl, m_pso, c,
        { { S::SRV_t0, m_inputSrv }, { S::UAV_u0, gfx.GetTextureUAVGpuHandle(Out()) } },
        RG::Groups2D(m_t->vpW, m_t->vpH, 8));

    RG::TransitionTo(gfx, cl, Out(), RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
    m_wroteThisFrame = wrote;
}
