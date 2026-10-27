#include "PostProcess/BuiltinPostProcessEffects.h"
#include "PostProcess/ResolvedPostProcessSettings.h"

#include "Graphics/IGraphicsDevice.h"
#include "RenderGraph/RenderPass/CASPass.h"
#include "RenderGraph/RenderPass/AutoExposurePass.h"
#include "RenderGraph/RenderPass/BloomPass.h"
#include "RenderGraph/RenderPass/LensFlarePass.h"
#include "RenderGraph/RenderPass/ToneMapPass.h"
#include "RenderGraph/RenderPass/UnderwaterPass.h"
#include "RenderGraph/RenderPass/DepthOfFieldPass.h"
#include "RenderGraph/RenderPass/StylizePass.h"
#include "RenderGraph/RenderPass/LumaReducePass.h"
#include "Renderer/FrameResources.h"
#include "Renderer/FrameView.h"
#include "System/CommandLine.h"

#include <DirectXMath.h>
#include <cmath>
#include <algorithm>

namespace PostProcess
{

// Pooled-target forwarders (see IEffect::SetSharedOutput).
void DepthOfFieldEffect::SetSharedOutput(const RHI::Texture* t)
{ if (m_pass) m_pass->SetSharedOutput(t); }

void CASEffect::SetSharedOutput(const RHI::Texture* t)
{ if (m_pass) m_pass->SetSharedOutput(t); }

void UnderwaterEffect::SetSharedOutput(const RHI::Texture* t)
{ if (m_pass) m_pass->SetSharedOutput(t); }

void StylizeEffect::SetSharedOutput(const RHI::Texture* t)
{ if (m_pass) m_pass->SetSharedOutput(t); }

// ==== EnergyAfterAA - the harness's LumaReduce on the resolved image ====
bool EnergyAfterAAEffect::IsEnabled(const Context& ctx) const
{
    return m_pass && ctx.view && Sys::CommandLine::Get().Has("-energy_after_aa");
}

void EnergyAfterAAEffect::Execute(Context& ctx)
{
    // The resolved image at the chain's size: the display size once an
    // upscaler resolved, the render size otherwise.
    m_pass->SetSource(ctx.gfx->DescriptorHeapIndexFromGpuHandle(ctx.hdrSrv),
                      ctx.viewportWidth, ctx.viewportHeight, ctx.view->currentFrame);
    m_pass->Execute(ctx.cl);
}

// ==== DepthOfField - focus-distance CoC blur (HDR producer; needs depth) ====
bool DepthOfFieldEffect::IsEnabled(const Context& ctx) const
{
    if (!m_pass || !ctx.resolved || ctx.hdrSrv == 0 || ctx.depthSrv == 0) return false;
    return ctx.resolved->dof.enabled && !ctx.skipDepthOfField;
}

void DepthOfFieldEffect::Execute(Context& ctx)
{
    const auto& p = ctx.resolved->dof;

    m_pass->SetEnabled(p.enabled);
    m_pass->focusDistance   = p.focusDistance;
    m_pass->focusRange      = p.focusRange;
    m_pass->transitionRange = p.transitionRange;
    m_pass->maxRadius       = p.maxRadius;

    m_pass->SetInputSrv(ctx.hdrSrv);
    m_pass->SetDepthSrv(ctx.depthSrv);
    m_pass->SetCameraPlanes(ctx.cameraNear, ctx.cameraFar);
    m_pass->SetViewportSize(ctx.viewportWidth, ctx.viewportHeight);
    m_pass->Execute(ctx.cl);

    if (const uint64_t out = m_pass->GetOutputSrvHandle())
        ctx.hdrSrv = out;
}

// ==== CAS - AMD FidelityFX Contrast Adaptive Sharpening (HDR) ====
bool CASEffect::IsEnabled(const Context& ctx) const
{
    if (!m_pass || !ctx.resolved || ctx.hdrSrv == 0) return false;
    return ctx.resolved->cas.enabled;
}

void CASEffect::Execute(Context& ctx)
{
    const auto& p = ctx.resolved->cas;

    m_pass->SetEnabled(p.enabled);
    m_pass->sharpness = p.sharpness;  // public field, not a setter

    m_pass->SetInputSrv(ctx.hdrSrv);
    m_pass->SetViewportSize(ctx.viewportWidth, ctx.viewportHeight);
    m_pass->Execute(ctx.cl);

    if (const uint64_t out = m_pass->GetOutputSrvHandle())
        ctx.hdrSrv = out;
}

// ==== AutoExposure - histogram EV (always runs) ====
AutoExposureEffect::AutoExposureEffect(AutoExposurePass* pass) : m_pass(pass)
{
#if defined(WITH_HARNESS) && WITH_HARNESS
    m_late = Sys::CommandLine::Get().Has("-ae_late");
#endif
}

bool AutoExposureEffect::IsEnabled(const Context& ctx) const
{
    // Runs every frame even when auto-exposure is disabled: Execute() then uploads
    // manualExposure into the exposure buffer, so Tonemapping never reads stale data.
    return m_pass != nullptr && ctx.resolved != nullptr;
}

void AutoExposureEffect::Execute(Context& ctx)
{
    const auto& p = ctx.resolved->autoExposure;

    m_pass->SetEnabled(p.enabled);
    m_pass->SetManualExposure(p.manualExposure);
#if defined(WITH_HARNESS) && WITH_HARNESS
    // -ae_legacy_bias: the bias before the bounds, none on the manual exposure (a reverse arm).
    static const bool s_legacyBias = Sys::CommandLine::Get().Has("-ae_legacy_bias");
    m_pass->SetLegacyBias(s_legacyBias);
    // -ae_hold_off: nothing holds the exposure - the freeze and the lock are ignored (a reverse arm).
    static const bool s_holdOff = Sys::CommandLine::Get().Has("-ae_hold_off");
    m_pass->SetHoldOff(s_holdOff);
#endif
    m_pass->SetFrozen(ctx.worldFrozen);
#if defined(WITH_HARNESS) && WITH_HARNESS
    // -ae_snap_nowait: a snap does not wait for the scene to settle (a reverse arm).
    static const bool s_snapNoWait = Sys::CommandLine::Get().Has("-ae_snap_nowait");
    m_pass->SetSceneSettled(ctx.sceneSettled, !s_snapNoWait);
#else
    m_pass->SetSceneSettled(ctx.sceneSettled, true);
#endif
    if (!ctx.secondaryView && m_pass->ConsumeMeterRequest()) m_pass->Snap();

    // Sanitize the authored ranges. Every one of these pairs is blended
    // INDEPENDENTLY by the volume resolve, so a volume that overrides only one
    // half can invert a pair the profile author never inverted. An inverted
    // log-luma range makes the meter read the scene backwards; an equal one
    // divides by zero in the pass and locks exposure on a constant forever.
    const float logLo = std::min(p.minLogLuma, p.maxLogLuma);
    const float logHi = std::max(logLo + 0.1f, std::max(p.minLogLuma, p.maxLogLuma));
    m_pass->SetLuminanceRange(logLo, logHi);

    // lowPercent > highPercent empties the metering band entirely, which pins
    // exposure at the minLogLuma fallback rather than metering anything.
    const float pctLo = std::min(p.lowPercent, p.highPercent);
    const float pctHi = std::max(p.lowPercent, p.highPercent);
    m_pass->SetPercentiles(pctLo, pctHi);

    const float expLo = std::max(1e-4f, std::min(p.minExposure, p.maxExposure));
    const float expHi = std::max(expLo, std::max(p.minExposure, p.maxExposure));
    m_pass->SetMinExposure(expLo);
    m_pass->SetMaxExposure(expHi);

    m_pass->SetEVBias(p.evBias);
    m_pass->SetKeyValue(p.keyValue);
    m_pass->SetMeteringInputs(ctx.normalSrv, ctx.depthSrv, ctx.materialSrv,
                              ctx.metaWidth, ctx.metaHeight, ctx.materialCount);
    m_pass->SetMeteringWeights(p.centerWeight, p.skyWeight,
                               p.nprWeight, p.highlightWeight);

    // Frame-rate-independent exponential adaptation: rate = 1 - exp(-dt / tau), the
    // one-pole smoothing coefficient; tau is user-tunable. Two rates: the pass adapts
    // in stops and picks one by direction (to a brighter / a darker scene).
    const auto rateFor = [dt = ctx.deltaTime](float tau) {
        const float t = (tau > 0.0f) ? tau : 1.0e-4f;
        return (dt > 0.0f) ? 1.0f - std::expf(-dt / t) : 0.0f;
    };
    m_pass->SetAdaptationRate(rateFor(p.adaptationTau),
                              rateFor(p.adaptationTauDark));

    // Meter the PRE-NPRStabilize scene, never the running hdrSrv: NPRStabilize
    // scales exposure-protected NPR pixels by rcp(exposure), so metering its
    // output closes a positive feedback loop. Falls back to hdrSrv only when
    // the Renderer did not supply a metering source (tools / capture paths).
    const bool haveMeter = (ctx.meterSrv != 0 && ctx.meterWidth && ctx.meterHeight);
    m_pass->SetHdrSrvHandle(haveMeter ? ctx.meterSrv : ctx.hdrSrv);
    m_pass->SetViewportSize(haveMeter ? ctx.meterWidth  : ctx.viewportWidth,
                            haveMeter ? ctx.meterHeight : ctx.viewportHeight);
    m_pass->Execute(ctx.cl);

    if (ctx.gfx)
        ctx.resources.exposure = ctx.gfx->GetBufferSRVGpuHandle(m_pass->GetExposureBuffer());
    ctx.resources.exposureTex = m_pass->GetExposureTexture();
}

// ==== Bloom - Sledgehammer downsample / upsample chain ====
bool BloomEffect::IsEnabled(const Context& ctx) const
{
    // Bloom always produces its chain; the composite weight (bloom.strength)
    // is applied downstream in Tonemapping, so gate only on input availability.
    return m_pass != nullptr && ctx.hdrSrv != 0;
}

void BloomEffect::Execute(Context& ctx)
{
    m_pass->SetHdrSrvHandle(ctx.hdrSrv);
    m_pass->SetViewportSize(ctx.viewportWidth, ctx.viewportHeight);
    m_pass->Execute(ctx.cl);

    ctx.resources.bloom = m_pass->GetBloomSrvHandle();
}

// ==== LensFlare - procedural directional-light flare (HDR additive) ====
bool LensFlareEffect::IsEnabled(const Context& /*ctx*/) const
{
    // Always runs when registered: the pass early-outs to a black texture when it
    // is disabled or the sun is behind the camera, so Tonemapping can bind its SRV
    // unconditionally.
    return m_pass != nullptr;
}

void LensFlareEffect::Execute(Context& ctx)
{
    // The sun on screen. fr.sunDirection points FROM the light (the direction light
    // travels), so the direction to the sun is its negation; a far point along it is
    // projected with the un-jittered view-projection. Without depth the pass keeps
    // last frame's sun.
    if (ctx.depthSrv && ctx.view && ctx.frame)
    {
        using namespace DirectX;
        const FrameView&      fv = *ctx.view;
        const FrameResources& fr = *ctx.frame;
        XMFLOAT3 sunDirWS{ 0.f, 1.f, 0.f };
        XMFLOAT3 sunColor{ 1.f, 1.f, 1.f };
        if (fr.lightCB)
        {
            sunDirWS = { -fr.sunDirection.x, -fr.sunDirection.y, -fr.sunDirection.z };
            sunColor = fr.sunColor;
        }

        const XMFLOAT3& fwd = fv.view.cameraForward;
        const float dotFwd  = sunDirWS.x * fwd.x + sunDirWS.y * fwd.y
                            + sunDirWS.z * fwd.z;
        bool sunBehind = (dotFwd <= 0.05f);

        XMFLOAT2 sunUV{ 0.5f, 0.5f };
        if (!sunBehind)
        {
            const XMFLOAT3& cp = fv.view.cameraPosition;
            XMVECTOR wp = XMVectorSet(
                cp.x + sunDirWS.x * 1.0e5f,
                cp.y + sunDirWS.y * 1.0e5f,
                cp.z + sunDirWS.z * 1.0e5f,
                1.0f);
            XMMATRIX vp = XMLoadFloat4x4(&fv.view.viewProjMatrixNoJitter);
            XMVECTOR clip = XMVector4Transform(wp, vp);
            XMFLOAT4 c; XMStoreFloat4(&c, clip);
            if (c.w > 1.0e-4f)
            {
                sunUV.x =  c.x / c.w * 0.5f + 0.5f;
                sunUV.y = -c.y / c.w * 0.5f + 0.5f;
            }
            else
            {
                sunBehind = true;
            }
        }

        // Off when the sun is below the horizon: the time of day's moon lights the
        // scene (LightCB then tracks the moon, whose direction is above the horizon at
        // night), or the light points down (~1 deg above the horizon or lower) - a
        // manually authored sun too.
        bool sunBelowHorizon = fr.moonActive;
        if (sunDirWS.y < 0.02f)
            sunBelowHorizon = true;

        m_pass->SetEnabled(!sunBelowHorizon);
        m_pass->SetSun(sunUV, sunBehind, sunColor);
        m_pass->SetDepthSrvHandle(ctx.depthSrv);
        // Depth is always render resolution; the flare composites into the HDR domain
        // the chain runs at (render resolution on the TAA path, display resolution once an
        // upscaler resolved) - its viewport, set below.
        m_pass->SetDepthSourceSize(fv.renderWidth, fv.renderHeight);
        // Clouds do not write depth - their transmittance lets overcast kill the flare.
        // 0 when they did not render.
        m_pass->SetCloudSrvHandle(fr.cloudSrv);
    }

    if (ctx.resolved)
    {
        const auto& p = ctx.resolved->lensFlare;
        // AND the authored master toggle with the horizon gate set above (last frame's
        // when there was no depth).
        m_pass->SetEnabled(p.enabled && m_pass->IsEnabled());
        m_pass->SetIntensity(p.intensity);
        m_pass->SetGhostCount(p.ghostCount);
        m_pass->SetGhostDispersal(p.ghostDispersal);
        m_pass->SetHaloWidth(p.haloWidth);
        m_pass->SetStreakLength(p.streakLength);
        m_pass->SetChromaticOffset(p.chromaticOffset);
    }

    m_pass->SetViewportSize(ctx.viewportWidth, ctx.viewportHeight);
    m_pass->Execute(ctx.cl);

    ctx.resources.lensFlare = m_pass->GetSrvHandle();
}

// ==== Underwater - screen distortion + water tint (HDR producer) ====
bool UnderwaterEffect::IsEnabled(const Context& ctx) const
{
    if (!m_pass || !ctx.resolved || ctx.hdrSrv == 0) return false;
    return ctx.resolved->underwater.enabled;
}

void UnderwaterEffect::Execute(Context& ctx)
{
    const auto& p = ctx.resolved->underwater;

    m_pass->SetEnabled(p.enabled);
    m_pass->strength   = p.strength;
    m_pass->scale      = p.scale;
    m_pass->speed      = p.speed;
    m_pass->tint       = p.tint;
    m_pass->tintAmount = p.tintAmount;
    if (!ctx.secondaryView) m_pass->AddTime(ctx.deltaTime);   // animate the wobble, once a frame

    m_pass->SetInputSrv(ctx.hdrSrv);
    m_pass->SetViewportSize(ctx.viewportWidth, ctx.viewportHeight);
    m_pass->Execute(ctx.cl);

    if (const uint64_t out = m_pass->GetOutputSrvHandle())
        ctx.hdrSrv = out;
}

// ==== Stylize - NPR filters and the source outline (HDR producer) ====
bool StylizeEffect::IsEnabled(const Context& ctx) const
{
    if (!m_pass || !ctx.resolved || ctx.hdrSrv == 0) return false;
    const auto& r = *ctx.resolved;
    return r.pixelate.enabled  || r.kuwahara.enabled || r.posterize.enabled
        || r.halftone.enabled  || r.dither.enabled   || r.crosshatch.enabled
        || (r.sourceOutline.enabled && ctx.depthSrv && ctx.normalSrv && ctx.materialSrv);
}

void StylizeEffect::Execute(Context& ctx)
{
    const auto& r = *ctx.resolved;

    m_pass->pixelateOn          = r.pixelate.enabled;
    m_pass->pixelSize           = r.pixelate.size;
    m_pass->kuwaharaOn          = r.kuwahara.enabled;
    m_pass->kuwaharaRadius      = r.kuwahara.radius;
    m_pass->posterizeOn         = r.posterize.enabled;
    m_pass->posterizeLevels     = r.posterize.levels;
    m_pass->halftoneOn          = r.halftone.enabled;
    m_pass->halftoneCell        = r.halftone.cellSize;
    m_pass->halftoneAngle       = r.halftone.angle;
    m_pass->ditherOn            = r.dither.enabled;
    m_pass->ditherLevels        = r.dither.levels;
    m_pass->crosshatchOn        = r.crosshatch.enabled;
    m_pass->crosshatchDensity   = r.crosshatch.density;
    m_pass->crosshatchThickness = r.crosshatch.thickness;
    auto& s = m_pass->sourceOutline;
    const auto& authored = r.sourceOutline;
    s.enabled = authored.enabled ? 1u : 0u;
    s.depthWidth = authored.depthWidth; s.normalWidth = authored.normalWidth;
    s.depthExponent = authored.depthExponent; s.normalExponent = authored.normalExponent;
    s.distancePower = authored.distancePower; s.depthMax = authored.depthMax;
    s.highlightU = authored.highlightU; s.highlightV = authored.highlightV;
    s.outlineColor = authored.outlineColor; s.highlightColor = authored.highlightColor;
    s.nearZ = ctx.cameraNear; s.farZ = ctx.cameraFar; s.materialCount = ctx.materialCount;
    m_pass->SetGeometryInputs(ctx.depthSrv, ctx.normalSrv, ctx.materialSrv);
    m_pass->SetEnabled(true);   // IsEnabled() already verified at least one feature

    m_pass->SetInputSrv(ctx.hdrSrv);
    m_pass->SetViewportSize(ctx.viewportWidth, ctx.viewportHeight);
    m_pass->Execute(ctx.cl);

    if (const uint64_t out = m_pass->GetOutputSrvHandle())
        ctx.hdrSrv = out;
}

// ==== Tonemapping - ACES + baked 3D LUT color grading (HDR scene -> the composition buffer) ====
bool ToneMapEffect::IsEnabled(const Context& ctx) const
{
    // Always runs: it writes the composition buffer every later stage draws into.
    return m_pass != nullptr && ctx.resolved != nullptr;
}

void ToneMapEffect::Execute(Context& ctx)
{
    const auto& cg = ctx.resolved->colorGrading;

#if defined(WITH_HARNESS) && WITH_HARNESS
    // -tonemap_inject_nonfinite=<n>: the first n columns of the tone map's input read NaN and the
    // next n +Inf (the guard turns them black and white); -tonemap_unguarded drops the guard and
    // -tonemap_no_dither the SDR output dither.
    static const ToneMapPass::DebugSwitches s_debug{
        static_cast<uint32_t>((std::max)(0, Sys::CommandLine::Get().GetInt("-tonemap_inject_nonfinite", 0))),
        Sys::CommandLine::Get().Has("-tonemap_unguarded"),
        Sys::CommandLine::Get().Has("-tonemap_no_dither") };
    m_pass->SetDebugSwitches(s_debug);
#endif

    m_pass->SetBloomStrength(ctx.resolved->bloom.strength);
    m_pass->SetColorGradingEnabled(cg.enabled);
    // Cheap when the value is unchanged (an equality check gates the LUT-dirty
    // flag), so pushing it every frame is safe.
    ColorGradingParams grading = ToColorGradingParams(cg);
#if defined(WITH_HARNESS) && WITH_HARNESS
    // -grading_legacy_maths: the old tint sign and grading exposure (a reverse arm).
    static const bool s_legacyGrading = Sys::CommandLine::Get().Has("-grading_legacy_maths");
    grading.debugFlags = s_legacyGrading ? 1u : 0u;
#endif
    m_pass->SetColorGradingParams(grading);
    const auto& u = ctx.resolved->ueColorGrading;
    UEToneMapParams ue;
    ue.shadowsSaturation = { u.shadowsSaturation.x,u.shadowsSaturation.y,u.shadowsSaturation.z,0.f };
    ue.shadowsContrast = { u.shadowsContrast.x,u.shadowsContrast.y,u.shadowsContrast.z,0.f };
    ue.shadowsGamma = { u.shadowsGamma.x,u.shadowsGamma.y,u.shadowsGamma.z,0.f };
    ue.shadowsGain = { u.shadowsGain.x,u.shadowsGain.y,u.shadowsGain.z,0.f };
    ue.shadowsOffset = { u.shadowsOffset.x,u.shadowsOffset.y,u.shadowsOffset.z,0.f };
    ue.midtonesSaturation = { u.midtonesSaturation.x,u.midtonesSaturation.y,u.midtonesSaturation.z,0.f };
    ue.midtonesContrast = { u.midtonesContrast.x,u.midtonesContrast.y,u.midtonesContrast.z,0.f };
    ue.midtonesGamma = { u.midtonesGamma.x,u.midtonesGamma.y,u.midtonesGamma.z,0.f };
    ue.midtonesGain = { u.midtonesGain.x,u.midtonesGain.y,u.midtonesGain.z,0.f };
    ue.midtonesOffset = { u.midtonesOffset.x,u.midtonesOffset.y,u.midtonesOffset.z,0.f };
    ue.highlightsSaturation = { u.highlightsSaturation.x,u.highlightsSaturation.y,u.highlightsSaturation.z,0.f };
    ue.highlightsContrast = { u.highlightsContrast.x,u.highlightsContrast.y,u.highlightsContrast.z,0.f };
    ue.highlightsGamma = { u.highlightsGamma.x,u.highlightsGamma.y,u.highlightsGamma.z,0.f };
    ue.highlightsGain = { u.highlightsGain.x,u.highlightsGain.y,u.highlightsGain.z,0.f };
    ue.highlightsOffset = { u.highlightsOffset.x,u.highlightsOffset.y,u.highlightsOffset.z,0.f };
    ue.ranges = { u.shadowsMax,u.highlightsMin,u.highlightsMax,0.f };
    ue.film = { u.filmSlope,u.filmToe,u.filmShoulder,u.filmBlackClip };
    ue.controls = { u.enabled ? 1.f : 0.f,u.filmWhiteClip,cg.vignetteStrength,0.f };
    m_pass->SetUEColorGradingParams(ue);

    m_pass->SetHdrSrvHandle(ctx.hdrSrv);
    m_pass->SetBloomSrvHandle(ctx.resources.bloom);
    m_pass->SetExposureSrvHandle(ctx.resources.exposure);
    m_pass->SetLensFlareSrvHandle(ctx.resources.lensFlare);
    // No SetViewportSize: the pass's OnPrepare already sized its output
    // (EnsureTexture) before the chain records.
    m_pass->Execute(ctx.cl);

    // The composition buffer and its live format, for the PostTonemap effects.
    ctx.resources.tonemapped       = m_pass->GetFinalOutputTexture();
    ctx.resources.tonemappedFormat = m_pass->GetFinalOutputFormat();
}

} // namespace PostProcess
