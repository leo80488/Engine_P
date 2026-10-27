#include "RenderGraph/RenderPass/FXAAPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RootSignatureLayout.h"
#include "RenderGraph/PassHelpers.h"
#include "System/Log.h"
#include "System/Check.h"
#include <cstring>

// Compute root-signature slots (RHI::Slots::Cmp, Graphics/RootSignatureLayout.h):
// input SRV at S::SRV_t0 (t0 space2), output UAV at S::UAV_u0 (u0 space2).
namespace S = RHI::Slots::Cmp;

// Gives this instance a unique name (GetName() says what RenderGraph keys on it).
void FXAAPass::SetNameSuffix(const char* suffix)
{
    // An empty suffix would silently reproduce the main instance's name - the failure a
    // unique name prevents (first-match-wins timestamp scan, one shared external-wait key,
    // one merged PIX region). Leave the name alone and report rather than manufacture a
    // duplicate.
    if (!ENGINE_CHECK(suffix && *suffix,
                      "FXAAPass::SetNameSuffix: empty suffix would duplicate '%s'",
                      kBaseName))
        return;
    m_name = kBaseName;
    m_name += '.';
    m_name += suffix;
}

void FXAAPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::FXAA_CS, RHI::ShaderStage::CS, "FXAA.cs.hlsl", "CSMain");

    if (!CreatePipelines(gfx)) return;


    LOG_SUCCESS("FXAAPass: initialized");
}

// Onto the live member: a failed rebuild keeps the old pipeline.
bool FXAAPass::CreatePipelines(IGraphicsDevice& gfx)
{
    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::FXAA_CS);
    if (!cs)
    {
        LOG_ERROR("FXAAPass: FXAA_CS shader not found");
        return false;
    }

    RHI::PipelineStateDesc pd{};
    pd.cs = cs;
    if (!gfx.CreatePipelineState(pd, m_pso))
    {
        LOG_ERROR("FXAAPass: PSO creation failed");
        return false;
    }
    return true;
}

void FXAAPass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (m_shaderLib.ClearCaches()) CreatePipelines(gfx);
}

void FXAAPass::BindView(ViewState& view)
{
    m_t = view.Slot() == 0 ? &m_mainTarget : &view.Data<ViewTarget>(this).t;
}

uint64_t FXAAPass::GetResolvedSrvHandle() const
{
    if (!m_gfx || !m_t->output.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_t->output.Tex());
}

void FXAAPass::RebuildBuffers()
{
    if (!m_gfx || m_t->vpW == 0 || m_t->vpH == 0) return;

    // RGBA16F, SRV|UAV, destroyed and made again, created UNORDERED_ACCESS; its state
    // from here is the device's book's.
    if (!m_t->output.Rebuild(*m_gfx, m_t->vpW, m_t->vpH))
        LOG_ERROR("FXAAPass: failed to create output buffer");
    m_t->lastVpW     = m_t->vpW;
    m_t->lastVpH     = m_t->vpH;

    LOG_INFO("FXAAPass: rebuilt output buffer %ux%u", m_t->vpW, m_t->vpH);
}

void FXAAPass::PrepareResources(uint32_t w, uint32_t h)
{
    SetViewportSize(w, h);
    if (m_t->vpW != m_t->lastVpW || m_t->vpH != m_t->lastVpH)
        RebuildBuffers();
}

void FXAAPass::ReleaseResources()
{
    if (!m_t->output.IsValid()) return;
    m_t->output.Release(*m_gfx);
    // Clear the size stamp too, or the next PrepareResources at the SAME resolution sees
    // "unchanged" and never rebuilds: the pass would run against an invalid handle for as
    // long as the mode stays selected.
    m_t->lastVpW = 0;
    m_t->lastVpH = 0;
    LOG_INFO("FXAAPass: output buffer released (AA mode cannot run FXAA)");
}

RHI::CommandList FXAAPass::Execute(RHI::CommandList cl)
{
    if (!m_enabled) return cl;
    if (!m_pso.IsValid() || m_t->vpW == 0 || m_t->vpH == 0) return cl;
    if (!m_inputSrvHandle) return cl;

    if (m_t->vpW != m_t->lastVpW || m_t->vpH != m_t->lastVpH)
        RebuildBuffers();
    if (!m_t->output.IsValid()) return cl;

    auto& gfx = *m_gfx;

    FXAACB c{};
    c.width                   = m_t->vpW;
    c.height                  = m_t->vpH;
    c.qualitySubpix           = qualitySubpix;
    c.qualityEdgeThreshold    = qualityEdgeThreshold;
    c.qualityEdgeThresholdMin = qualityEdgeThresholdMin;

    // Output -> UAV (its state is the device's book's)
    RG::TransitionTo(gfx, cl, m_t->output.Tex(), RHI::ResourceState::UNORDERED_ACCESS);

    // Dispatch (a zero output UAV binds nothing)
    RG::DispatchCS(cl, m_pso, c,
        { { S::SRV_t0, m_inputSrvHandle }, { S::UAV_u0, gfx.GetTextureUAVGpuHandle(m_t->output.Tex()) } },
        RG::Groups2D(m_t->vpW, m_t->vpH, 8));

    // Output -> SRV for downstream reads
    RG::TransitionTo(gfx, cl, m_t->output.Tex(), RHI::ResourceState::SHADER_RESOURCE_COMPUTE);

    return cl;
}
