#include "RenderGraph/RenderPass/LensFlarePass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RootSignatureLayout.h"
#include "System/Check.h"
#include "System/Log.h"
#include <cstring>

// Compute root-signature slots - RHI::Slots::Cmp (Graphics/RootSignatureLayout.h)
// is the single source of truth. This pass binds S::SRV_t0 = depth,
// S::SRV_t1 = cloud transmittance, S::UAV_u0 = output flare.
namespace S = RHI::Slots::Cmp;

void LensFlarePass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::LensFlare_CS, RHI::ShaderStage::CS,
                         "LensFlare.cs.hlsl", "CSMain");

    if (!CreatePipelines(gfx)) return;

    m_cb.Create(gfx, "LensFlare.CB");

    LOG_SUCCESS("LensFlarePass: initialized");
}

// Onto the live member: a failed rebuild keeps the old pipeline.
bool LensFlarePass::CreatePipelines(IGraphicsDevice& gfx)
{
    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::LensFlare_CS);
    if (!cs) { LOG_ERROR("LensFlarePass: LensFlare_CS not found"); return false; }

    RHI::PipelineStateDesc d{};
    d.cs = cs;
    if (!gfx.CreatePipelineState(d, m_pso))
    { LOG_ERROR("LensFlarePass: PSO failed"); return false; }
    return true;
}

void LensFlarePass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (m_shaderLib.ClearCaches()) CreatePipelines(gfx);
}

void LensFlarePass::DestroyTexture()
{
    if (m_gfx && m_t->texture.IsValid())
        m_gfx->DestroyTexture(m_t->texture);
}

void LensFlarePass::ViewTarget::Release(IGraphicsDevice& gfx)
{
    if (t.texture.IsValid()) gfx.DestroyTexture(t.texture);
    t = Target{};
}

void LensFlarePass::BindView(ViewState& view)
{
    m_hist = &view.History<History>(this);
    m_t    = view.Slot() == 0 ? &m_mainTarget : &view.Data<ViewTarget>(this).t;
}

void LensFlarePass::History::Release(IGraphicsDevice& gfx)
{
    if (occlusion.IsValid()) gfx.DestroyTexture(occlusion);
    occlusion = {};
    valid     = false;
}

// The occlusion history is 1x1 and viewport-INDEPENDENT, so it is created once
// and deliberately NOT rebuilt on resize - only the validity flag resets, which
// re-seeds the EMA from the first post-resize frame instead of blending against a
// value measured at a different sun position.
void LensFlarePass::EnsureHistory()
{
    if (!m_gfx) return;
    if (!ENGINE_CHECK_ONCE(m_hist != nullptr,
                           "LensFlarePass: no view bound (PassSet::BindView) - no occlusion history"))
        return;
    if (m_hist->occlusion.IsValid()) return;

    RHI::TextureDesc td{};
    td.width      = 1;
    td.height     = 1;
    td.format     = RHI::Format::R32_FLOAT;
    td.bindFlags = RHI::BindFlag::UNORDERED_ACCESS;
    td.usage      = RHI::Usage::DEFAULT;
    td.layout     = RHI::ResourceState::UNORDERED_ACCESS;
    td.debugName = "LensFlarePass.OcclusionHistory";

    if (!m_gfx->CreateTexture(td, m_hist->occlusion))
        LOG_ERROR("LensFlarePass: occlusion history texture failed");
    m_hist->valid = false;
}

void LensFlarePass::RebuildTexture()
{
    if (!m_gfx) return;
    DestroyTexture();

    m_t->texW = (m_t->vpW + 1) / 2;
    m_t->texH = (m_t->vpH + 1) / 2;
    if (m_t->texW < 1) m_t->texW = 1;
    if (m_t->texH < 1) m_t->texH = 1;

    RHI::TextureDesc td{};
    td.width      = m_t->texW;
    td.height     = m_t->texH;
    td.format     = RHI::Format::R16G16B16A16_FLOAT;
    td.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    td.usage      = RHI::Usage::DEFAULT;
    td.layout     = RHI::ResourceState::UNORDERED_ACCESS;
    td.debugName = "LensFlarePass.Output";

    if (!m_gfx->CreateTexture(td, m_t->texture))
        LOG_ERROR("LensFlarePass: failed to create output texture");

    m_t->textureState = RHI::ResourceState::UNORDERED_ACCESS;
    m_t->lastVpW = m_t->vpW;
    m_t->lastVpH = m_t->vpH;
    LOG_INFO("LensFlarePass: output texture %ux%u", m_t->texW, m_t->texH);
}

void LensFlarePass::PrepareResources(uint32_t w, uint32_t h)
{
    SetViewportSize(w, h);
    EnsureHistory();
    if (m_t->vpW != m_t->lastVpW || m_t->vpH != m_t->lastVpH)
    {
        RebuildTexture();
        // Sun screen position moves with the projection; blending the new
        // occlusion against a pre-resize sample would fade in from the wrong value.
        if (m_hist) m_hist->valid = false;
    }
}

uint64_t LensFlarePass::GetSrvHandle() const
{
    // Publish only what this frame actually wrote - see m_wroteThisFrame.
    if (!m_gfx || !m_wroteThisFrame || !m_t->texture.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_t->texture);
}

RHI::CommandList LensFlarePass::Execute(RHI::CommandList cl)
{
    // Cleared before every early return, so a declined frame cannot publish
    // somebody else's pixels through GetSrvHandle().
    m_wroteThisFrame = false;
    if (!m_pso.IsValid())   return cl;
    if (m_t->vpW == 0 || m_t->vpH == 0) return cl;

    if (m_t->vpW != m_t->lastVpW || m_t->vpH != m_t->lastVpH)
        RebuildTexture();
    if (!m_t->texture.IsValid()) return cl;

    auto& gfx = *m_gfx;

    // Texture left as SHADER_RESOURCE_COMPUTE last frame (consumed by ToneMap)
    // -> flip back to UAV before writing.
    if (m_t->textureState != RHI::ResourceState::UNORDERED_ACCESS)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(
            &m_t->texture, m_t->textureState,
            RHI::ResourceState::UNORDERED_ACCESS), cl);
        m_t->textureState = RHI::ResourceState::UNORDERED_ACCESS;
    }

    if (auto* slot = m_cb.Current(gfx))
    {
        LensFlarePass::LensFlareCB cb{};
        cb.dstWidth        = m_t->texW;
        cb.dstHeight       = m_t->texH;
        cb.srcDepthWidth   = m_depthW ? m_depthW : m_t->vpW;
        cb.srcDepthHeight  = m_depthH ? m_depthH : m_t->vpH;
        cb.enabled         = (m_enabled && m_depthSrv != 0) ? 1u : 0u;
        cb.intensity       = m_intensity;
        cb.sunBehind       = m_sunBehind ? 1.0f : 0.0f;
        cb.chromaticOffset = m_chromaticOffset;
        cb.sunUV[0]        = m_sunUV.x;
        cb.sunUV[1]        = m_sunUV.y;
        cb.haloWidth       = m_haloWidth;
        cb.streakLength    = m_streakLength;
        cb.sunColor[0]     = m_sunColor.x;
        cb.sunColor[1]     = m_sunColor.y;
        cb.sunColor[2]     = m_sunColor.z;
        cb.ghostDispersal  = m_ghostDispersal;
        cb.ghostCount      = (m_ghostCount > 8u) ? 8u : m_ghostCount;
        cb.streakWidth     = 0.014f;
        cb.occlusionRadius = 0.006f;
        cb.cloudValid      = (m_cloudSrv != 0) ? 1u : 0u;
        cb.apertureBlades   = m_apertureBlades;
        cb.apertureRotation = m_apertureRotation;
        cb.occlusionSmooth  = m_occlusionSmooth;
        cb.historyValid     = (m_hist && m_hist->valid) ? 1u : 0u;
        cb.streakStrength   = m_streakStrength;
        cb.ghostBrightness  = m_ghostBrightness;
        cb.glareStrength    = m_glareStrength;
        cb.pad0             = 0.0f;
        *slot = cb;
    }

    gfx.BindComputePipelineState(m_pso, cl);
    gfx.SetComputeRootCBV(S::CB, m_cb.CurrentBuffer(gfx), cl);
    if (m_depthSrv) gfx.SetComputeDescriptorTable(S::SRV_t0, m_depthSrv, cl);
    // The shader branches on cloudValid; bind depth as a harmless dummy when
    // clouds didn't render so the statically-declared t1 table is never unset.
    if (m_cloudSrv || m_depthSrv)
        gfx.SetComputeDescriptorTable(S::SRV_t1, m_cloudSrv ? m_cloudSrv : m_depthSrv, cl);
    gfx.SetComputeDescriptorTable(S::UAV_u0, gfx.GetTextureUAVGpuHandle(m_t->texture), cl);
    // Persistent 1x1 occlusion history. NEVER cleared and NEVER transitioned out of
    // UNORDERED_ACCESS - it must survive across frames for the EMA that stops the
    // flare strobing when the sun crosses foliage. The shader reads and writes the
    // same texel from different threads within a dispatch, safe only because every
    // thread computes the identical value from last frame's history (see the note at
    // the shader's read site).
    if (m_hist && m_hist->occlusion.IsValid())
        gfx.SetComputeDescriptorTable(S::UAV_u1, gfx.GetTextureUAVGpuHandle(m_hist->occlusion), cl);

    gfx.DispatchCompute((m_t->texW + 7) / 8, (m_t->texH + 7) / 8, 1, cl);
    gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_t->texture), cl);
    // From here the history holds a real value, so the shader may blend against
    // it instead of seeding from this frame's raw occlusion.
    if (m_hist) m_hist->valid = true;

    // Hand off to ToneMap as SRV.
    gfx.PushBarrier(RHI::GPUBarrier::Image(
        &m_t->texture, RHI::ResourceState::UNORDERED_ACCESS,
        RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
    m_t->textureState   = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
    m_wroteThisFrame = true;

    return cl;
}
