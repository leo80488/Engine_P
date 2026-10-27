// Renderer_RenderPath.cpp - which picture the Renderer makes and at what size:
// the DLSS model forwarders, the render path (raster / ReSTIR / path tracing),
// ray reconstruction and its fallback, the AA mode and the render resolution,
// the ReSTIR backends and their status, and ServiceEnableGatedResources (the
// gated passes' resources, built on enable, released on disable).

#include <optional>
#include "System/PathUtil.h"
#include "Renderer/Renderer.h"
#include "Scene/MeshAssetRegistry.h"

// engine graphics / backend
#include "Graphics/GraphicsDX12.h"
#include "Graphics/ParticleSystem.h"
#include "Graphics/TracerSystem.h"
#include "Graphics/AfterimageSystem.h"
#include "Graphics/TrailSystem.h"
#include "Graphics/SSR/SSRSubsystem.h"

// render passes
#include "RenderGraph/RenderPass/DDGIProbeDebugPass.h"
#include "RenderGraph/RenderPass/SkyboxPass.h"
#include "RenderGraph/RenderPass/AutoExposurePass.h"
#include "RenderGraph/RenderPass/NPRStabilizePass.h"
#include "RenderGraph/RenderPass/BloomPass.h"
#include "RenderGraph/RenderPass/LensFlarePass.h"
#include "RenderGraph/RenderPass/TAAPass.h"
#include "RenderGraph/RenderPass/FSR2Pass.h"
#include "RenderGraph/RenderPass/DLSSPass.h"
#include "RenderGraph/RenderPass/UpscalerReactiveMaskPass.h"
#include "RenderGraph/RenderPass/RRGuidePass.h"
#include "RenderGraph/RenderPass/FXAAPass.h"
#include "RenderGraph/RenderPass/XeGTAOPass.h"
#include "RenderGraph/RenderPass/CASPass.h"
#include "RenderGraph/RenderPass/UnderwaterPass.h"
#include "RenderGraph/RenderPass/DepthOfFieldPass.h"
#include "RenderGraph/RenderPass/StylizePass.h"
#include "RenderGraph/RenderPass/DebugTargetViewPass.h"
#include "Renderer/TextureViewService.h"
#include "Renderer/UIViewService.h"
#include "RenderGraph/RenderPass/VolumetricFogPass.h"
#include "Graphics/WaterRippleSystem.h"
#include "RenderGraph/RenderPass/ClusterPass.h"
#include "RenderGraph/RenderPass/RTReflectionPass.h"
#include "RenderGraph/RenderPass/TranslucentFrontLayerPass.h"
#include "RenderGraph/RenderPass/RTShadowPass.h"
#include "RenderGraph/RenderPass/PathTracePass.h"
#include "RenderGraph/RenderPass/RTDIPass.h"
#include "RenderGraph/RenderPass/LightSamplingPass.h"
#include "RenderGraph/RenderPass/RTGIPass.h"
#include "RenderGraph/RenderPass/RTAOPass.h"
#include "RenderGraph/RenderPass/SSGIPass.h"
#include "RenderGraph/RenderPass/SpotShadowPass.h"
#include "RenderGraph/RenderPass/PointShadowPass.h"
#include "RenderGraph/RenderPass/PerObjectShadowPass.h"
#include "RenderGraph/RenderPass/HairShadowMarkPass.h"
#include "RenderGraph/RenderPass/FaceOverlayPass.h"
#include "RenderGraph/RenderPass/BillboardFXPass.h"
#include "RenderGraph/RenderPass/HiZPass.h"
#include "RenderGraph/RenderPass/WorldUIBillboardPass.h"
#include "RenderGraph/RenderPass/CloudPass.h"
#include "RenderGraph/RenderPass/EditorGridPass.h"

// ECS components + systems
#include "ECS/ECS.h"
#include "ECS/IKRigSystem.h"
#include "ECS/HierarchyComponents.h"

// engine systems
#include "System/Log.h"
#include "System/JobDefaults.h"
#include "System/CommandLine.h"
#include "System/TaskSystem.h"
#include "System/EventBus.h"

// STL / DirectXMath
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <unordered_set>
#include <DirectXMath.h>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <execution>
#include <filesystem>
#include <cmath>

using namespace DirectX;

// CPU-side cbuffer mirrors live in Renderer.h (RendererDetail namespace); the
// layout static_asserts stay in Renderer.cpp.
using PerViewCB       = RendererDetail::PerViewCB;
using LightCB         = RendererDetail::LightCB;

namespace
{
    // -aa_fallback_taa (harness): a DLSS request the machine cannot run falls
    // back to TAA whatever the upscale ratio - the reverse arm of pack aa_fallback.
    bool AAFallbackTAAOnly()
    {
#if defined(WITH_HARNESS) && WITH_HARNESS
        static const bool s_taa = Sys::CommandLine::Get().Has("-aa_fallback_taa");
        return s_taa;
#else
        return false;
#endif
    }
}


// DLSS model forwarders. m_dlssPass is constructed unconditionally (not lazily
// on AAMode::DLSS), but the null check stays: a failed construction must
// degrade, not crash.
void Renderer::SetDLSSPreset(SL::DLSSPreset p)
{
    if (m_dlssPass) m_dlssPass->preset = p;
}

SL::DLSSPreset Renderer::GetDLSSPreset() const
{
    return m_dlssPass ? m_dlssPass->preset : SL::DLSSPreset::Default;
}

// Render path names, indexed by RenderPath ("Invalid" out of range): log tags,
// the settings row's value names, and a token ParseRenderPath accepts.
const char* Renderer::RenderPathName(RenderPath path)
{
    static constexpr const char* kNames[] = { "Raster", "ReSTIR_DI", "ReSTIR_GI", "ReSTIR_DI_GI", "PathTrace", "PathTrace_Realtime" };
    static_assert(std::size(kNames) == static_cast<size_t>(RenderPath::Count));
    const auto index = static_cast<size_t>(path);
    return index < std::size(kNames) ? kNames[index] : "Invalid";
}

bool Renderer::ParseRenderPath(const std::string& token, RenderPath& path)
{
    static constexpr const char* kTokens[] = { "raster", "restir_di", "restir_gi", "restir", "pathtrace", "pathtrace_realtime" };
    for (uint32_t i = 0; i < static_cast<uint32_t>(RenderPath::Count); ++i)
        if (token == kTokens[i] || token == RenderPathName(static_cast<RenderPath>(i)))
        { path = static_cast<RenderPath>(i); return true; }
    return false;
}

void Renderer::SetRenderPath(RenderPath path)
{
    if (path >= RenderPath::Count) path = RenderPath::Raster;
    const auto previousRequest = m_renderPathRequested.exchange(path);
    const bool supported = m_gfx.GetCapabilities().inlineRaytracing;
    const auto effective = supported ? path : RenderPath::Raster;
    const auto previous = m_renderPath.exchange(effective);
    if (previous != effective) m_renderPathChanged.store(true, std::memory_order_release);
    if (previous != effective || previousRequest != path)
        LOG_INFO("[RENDERPATH] requested=%s effective=%s reason=%s",
                 RenderPathName(path), RenderPathName(effective), supported ? "selected" : "dxr");
}

bool Renderer::RRCameraRaysForced() const
{
    // -pt_camera_primary, -pt_area_motion and -pt_lens_radius are old spellings of
    // engine.json rows and count here as pins (DevPin). The terms stay in flag-read
    // order, so the short-circuit leaves the remaining flags unread.
    const auto& cmd = Sys::CommandLine::Get();
    return (IsDevPinned(DevPin::PTCameraPrimary) && GetPTCameraPrimary()) || cmd.Has("-pt_area_restir") ||
           cmd.Has("-pt_area_spatial") || (IsDevPinned(DevPin::PTAreaMotion) && GetPTAreaMotion()) ||
           cmd.Has("-pt_area_fractional") || cmd.Has("-pt_area_overlap") ||
           cmd.Has("-pt_area_overlap_holes") || cmd.Has("-pt_area_primary_context") ||
           cmd.Has("-pt_area_context_corrupt") || cmd.Has("-pt_area_reconnect") ||
           cmd.Has("-pt_area_radiance_cache") || cmd.Has("-pt_area_radiance_corrupt") ||
           (IsDevPinned(DevPin::PTLensRadius) && GetPTLensRadius() > 0);
}

const char* Renderer::GetRayReconstructionInertReason(bool includeRuntimeFailure) const
{
    if(!SL::StreamlineBackend::Get().IsFeatureSupported(SL::Feature::DLSS_RR))
        return SL::StreamlineBackend::Get().GetFeatureStatusText(SL::Feature::DLSS_RR);
    const RenderPath path=GetRenderPath();
    // On a ReSTIR path RR denoises the raw DI / GI (and RT reflection) signal with
    // RRGuidePass's guides; the plain raster path has no ray-traced signal to
    // denoise.
    if(path==RenderPath::Raster) return "requires a ray-traced render path (ReSTIR DI / GI or Path Trace (Real-time))";
    // The reference path tracer converges by accumulation, which RR would replace
    // with a per-frame estimate. Real-time PT is the RR path.
    if(path==RenderPath::PathTrace) return "Path Trace (Reference) accumulates instead; use Path Trace (Real-time)";
    // The editor's Area / camera-ray / lens settings do not count: they are held
    // off outside the reference path (PTCameraModesHeld). Only a command-line flag
    // forces them.
    if(path==RenderPath::PathTraceRealtime && RRCameraRaysForced())
        return "requires GBuffer primary (a -pt_camera_primary / -pt_area_* / -pt_lens_radius flag forces camera rays)";
    if(m_viewMode!=ViewMode::Lit) return "requires Lit view";
    if(FrameViewRead().camera.orthographic) return "requires perspective view";
    if(includeRuntimeFailure && m_dlssPass && !m_dlssPass->IsHealthy()) return "evaluate failed; retrying by itself (toggle Ray Reconstruction to retry now)";
    return nullptr;
}

bool Renderer::IsRayReconstructionActive() const
{
    return m_aaMode==AAMode::DLSS_RR && GetRayReconstructionInertReason(false)==nullptr;
}

bool Renderer::IsRRFallbackToDLSS() const
{
    return m_aaMode==AAMode::DLSS_RR && !IsPathTrace(GetRenderPath()) && !IsRayReconstructionActive()
        && SL::StreamlineBackend::Get().IsFeatureSupported(SL::Feature::DLSS);
}

bool Renderer::PTCameraModesHeld() const
{
    // The requested path, not the effective one: Area's reservoirs (~100 MB at
    // 1080p) and pipelines are rebuilt only when the reference is entered or left,
    // never on a ReSTIR <-> real-time switch.
    return GetRenderPathRequested()!=RenderPath::PathTrace;
}

bool Renderer::RRInputsReady() const
{
    if(!IsPathTrace(GetRenderPath()) || DLSSReadsRasterRRGuides()) return true;
    return m_pathTracePass && m_pathTracePass->RayReconstructionReady();
}

bool Renderer::DLSSReadsRasterRRGuides() const
{
    if(!IsRayReconstructionActive()) return false;
    return !IsPathTrace(GetRenderPath()) || Sys::CommandLine::Get().GetInt("-rr_raster_guides",0)>=2;
}

SL::DLSSRRModel Renderer::GetDLSSRRModel() const
{
    const auto& cmd=Sys::CommandLine::Get();
    const std::string preset=cmd.Get("-dlss_rr_preset","");
    if(preset=="d" || preset=="D") return SL::DLSSRRModel::D;
    if(preset=="e" || preset=="E") return SL::DLSSRRModel::E;
    if(preset=="f" || preset=="F") return SL::DLSSRRModel::F;
    if(!preset.empty())
    {
        static bool s_warned=false;
        if(!s_warned) { s_warned=true; LOG_WARNING("[DLSS_RR] -dlss_rr_preset=%s is not d, e or f; using the setting",preset.c_str()); }
    }
    if(cmd.Has("-dlss_rr_preset_e")) return SL::DLSSRRModel::E;
    return m_dlssRRModel;
}

bool Renderer::IsFSRResolved() const
{
    return IsFSRActive() && m_fsr2Pass && m_fsr2Pass->IsContextValid();
}

bool Renderer::IsDLSSResolved() const
{
    // RR is active only on a path it can denoise (GetRayReconstructionInertReason).
    // A frame whose RR inputs are not there yet (RRInputsReady) skips DLSS
    // rather than evaluating RR without guides.
    const bool rr = IsRayReconstructionActive();
    return IsDLSSActive() && m_dlssPass && m_dlssPass->IsContextValid() &&
        !(rr && Sys::CommandLine::Get().Has("-dlss_rr_raw")) && (!rr || RRInputsReady());
}

bool Renderer::IsRayReconstructionResolved() const
{
    return IsRayReconstructionActive() && !Sys::CommandLine::Get().Has("-dlss_rr_raw") && m_dlssPass && m_dlssPass->IsContextValid() &&
        (!IsPathTrace(GetRenderPath()) || (m_pathTracePass && m_pathTracePass->IsActive())) && RRInputsReady();
}

Renderer::AAMode Renderer::GetAAMode() const
{
    if(m_aaMode==AAMode::DLSS_RR && GetRayReconstructionInertReason())
    {
        if(IsPathTrace(GetRenderPath())) return AAMode::None;
        return SL::StreamlineBackend::Get().IsFeatureSupported(SL::Feature::DLSS)?AAMode::DLSS:AAMode::TAA;
    }
    return m_aaMode;
}

void Renderer::SetAAMode(AAMode m)
{
    // Record the ask before any demotion, so persistence can save what the user
    // wanted rather than what this machine could deliver.
    m_aaModeRequested = m;

    // Refuse DLSS on a machine that cannot run it rather than leave the renderer
    // in a mode whose pass never produces an image. This is the last line of
    // defence (the editor greys the option out), for e.g. a settings.json copied
    // from an RTX machine to a non-NVIDIA one. The stand-in is FSR at the player's
    // upscale ratio (antialiasing.fsrRenderScale), so the frame keeps the render
    // size it asked for; Native (DLAA) has no upscaling to keep and takes TAA
    // (-aa_fallback_taa: always TAA).
    // NoteAAFallback logs the demotion once the frame's settings have all applied:
    // a boot decides it twice (the AA row before the ratio's).
    const SL::StreamlineBackend& sl = SL::StreamlineBackend::Get();
    const AAMode standIn = (m_upscaleMode != UpscaleMode::Native && !AAFallbackTAAOnly()) ? AAMode::FSR : AAMode::TAA;
    if (m == AAMode::DLSS && !sl.IsFeatureSupported(SL::Feature::DLSS))
        m = standIn;
    // RR is an option of DLSS (IsRRFallbackToDLSS): without its plugin, DLSS
    // Super Resolution runs where it can, the stand-in above where it cannot.
    if(m==AAMode::DLSS_RR && !sl.IsFeatureSupported(SL::Feature::DLSS_RR))
        m=sl.IsFeatureSupported(SL::Feature::DLSS)?AAMode::DLSS:standIn;
    if(m_aaMode!=m && m_dlssPass) m_dlssPass->SetReset(true);
    m_aaMode = m;
    UpdateAAEnabled();
}

void Renderer::NoteAAFallback()
{
    if (m_aaModeRequested == m_aaNotedRequested && m_aaMode == m_aaNotedEffective) return;
    m_aaNotedRequested = m_aaModeRequested;
    m_aaNotedEffective = m_aaMode;
    if (m_aaModeRequested == m_aaMode) return;
    const SL::StreamlineBackend& sl = SL::StreamlineBackend::Get();
    const char* standIn = m_aaMode == AAMode::FSR  ? "FSR at the same upscale ratio"
                        : m_aaMode == AAMode::DLSS ? "DLSS" : "TAA";
    if (m_aaModeRequested == AAMode::DLSS)
        LOG_WARNING("Renderer: DLSS requested but unavailable (%s) — using %s",
                    sl.GetFeatureStatusText(SL::Feature::DLSS), standIn);
    else if (m_aaModeRequested == AAMode::DLSS_RR)
        LOG_WARNING("DLSS-RR unavailable (%s); preserving request, using %s/PT accumulation",
                    sl.GetFeatureStatusText(SL::Feature::DLSS_RR), standIn);
}

void Renderer::SetUpscaleMode(UpscaleMode m)
{
    m_upscaleMode = m;
    // A demoted DLSS request's stand-in depends on the ratio (FSR, or TAA at
    // Native), and the AA row applies before this one: decide it again with the
    // current ratio.
    if (m_aaModeRequested != m_aaMode) SetAAMode(m_aaModeRequested);
}

// The render size for a display size: the upscaler's own query when DLSS / RR
// runs, the shared ratio table otherwise.
void Renderer::ComputeRenderResolution(uint32_t displayW, uint32_t displayH,
                                       uint32_t& outRenderW, uint32_t& outRenderH) const
{
    // DLSS validates the render extent against its own per-mode table, so its own
    // query is the authority; the shared ratio table is only close, and close is
    // rejected.
    // RR, DLSS and FSR come from this frame's plan (GamePlan, from the intent
    // CaptureFrameIntent took); the render half decides them from the same intent.
    const FramePlan::Decisions& plan = m_gamePlan;
    if(plan.rr)
    {
        // RR upscales from its own table (the path tracer traces at the upscaler's
        // input resolution); Native is DLAA (1:1). Without an answer, native size.
        SL::DLSSOptimalSettings s{};
        if(SL::StreamlineBackend::Get().GetDLSSDOptimalSettings(GetDLSSQualityMode(),displayW,displayH,s))
        { outRenderW=s.renderW; outRenderH=s.renderH; return; }
        outRenderW=displayW; outRenderH=displayH; return;
    }
    if (plan.dlssWanted)
    {
        SL::DLSSOptimalSettings s{};
        if (SL::StreamlineBackend::Get().GetDLSSOptimalSettings(
                GetDLSSQualityMode(), displayW, displayH, s))
        {
            outRenderW = s.renderW;
            outRenderH = s.renderH;
            return;
        }
        // Query failed (no SDK / unsupported GPU): fall through to the table so this
        // always answers; the DLSS pass declines to run and the frame resolves through
        // TAA at the selected ratio's size.
    }
    // GetEffectiveUpscaleMode, from the plan: the ratio applies only while an
    // upscaler is selected and applicable.
    ::ComputeRenderSize(displayW, displayH, (plan.fsr || plan.dlssWanted) ? m_upscaleMode : UpscaleMode::Native,
                        outRenderW, outRenderH);
}

// Global view mode (Lit / Unlit / Wireframe / debug views). The per-frame
// plumbing - LightCB.viewMode, FILL_MODE_WIREFRAME on the geometry passes,
// background-pass suppression - is applied in Render_BindFrameResources from
// m_viewMode; here only the AA enable is refreshed, since some modes force it
// off.
void Renderer::SetViewMode(ViewMode m)
{
    m_viewMode = m;
    UpdateAAEnabled();
}

// Single source of truth for the TAA / FXAA enable: derived from m_aaMode,
// forced off on the path-trace paths and in Wireframe (thin lines ghost under
// temporal AA, and dropping TAA also drops projection jitter, which a stable
// wireframe wants).
void Renderer::UpdateAAEnabled()
{
    // Lighting-debug and GBuffer-debug views force AA off too: temporal
    // accumulation would blend an isolated bucket with previous frames and make "is
    // this pixel exactly zero?" (the red-sun leak test's question) unanswerable.
    // Dropping TAA also drops jitter, so a locked-camera capture compares
    // bit-for-bit.
    const bool wire   = (m_viewMode == ViewMode::Wireframe);
    const bool noAA   = IsPathTrace(GetRenderPath()) || wire || IsLightingDebugView(m_viewMode)
                             || IsGBufferDebugView(m_viewMode);
    const bool taaOn  = !noAA && ((m_aaMode == AAMode::TAA) || (m_aaMode == AAMode::FXAA_TAA) || (m_aaMode==AAMode::DLSS_RR && !IsRayReconstructionActive() && !IsRRFallbackToDLSS()));
    const bool fxaaOn = !noAA && ((m_aaMode == AAMode::FXAA) || (m_aaMode == AAMode::FXAA_TAA));
    if (m_taaPass)  m_taaPass->SetEnabled(taaOn);
    if (m_fxaaPass) m_fxaaPass->SetEnabled(fxaaOn);
}

// ReSTIR backend per stage (DI / GI / PT). A pair ReSTIR::IsLegal rejects is
// ignored.
void Renderer::SetRestirBackend(ReSTIR::Stage stage, ReSTIR::Backend backend)
{
    if (!ReSTIR::IsLegal(stage, backend)) return;
    const auto value = static_cast<uint8_t>(backend);
    switch (stage)
    {
    case ReSTIR::Stage::DI: m_restirBackendDI.store(value); break;
    case ReSTIR::Stage::GI: m_restirBackendGI.store(value); break;
    case ReSTIR::Stage::PT: m_restirBackendPT.store(value); break;
    default: break;
    }
}

ReSTIR::Backend Renderer::GetRestirBackend(ReSTIR::Stage stage) const
{
    switch (stage)
    {
    case ReSTIR::Stage::DI: return static_cast<ReSTIR::Backend>(m_restirBackendDI.load());
    case ReSTIR::Stage::GI: return static_cast<ReSTIR::Backend>(m_restirBackendGI.load());
    case ReSTIR::Stage::PT: return static_cast<ReSTIR::Backend>(m_restirBackendPT.load());
    default:                return ReSTIR::Backend::Engine;
    }
}

ReSTIR::BackendStatus Renderer::GetRestirBackendStatus(ReSTIR::Stage stage) const
{
    const auto i = static_cast<size_t>(stage);
    if (i >= static_cast<size_t>(ReSTIR::Stage::Count)) return {};
    std::lock_guard<std::mutex> lock(m_restirStatusMutex);
    return m_restirStatus[i];
}

Renderer::GISharcStatus Renderer::GetRestirGISharcStatus() const
{
    std::lock_guard<std::mutex> lock(m_restirStatusMutex);
    return m_giSharcStatus;
}

const char* Renderer::GetRestirDIEffectiveDenoiser() const
{
    std::lock_guard<std::mutex> lock(m_restirStatusMutex);
    return m_restirDiDenoiserEffective;
}

const char* Renderer::GetRestirGIEffectiveDenoiser() const
{
    std::lock_guard<std::mutex> lock(m_restirStatusMutex);
    return m_restirGiDenoiserEffective;
}

void Renderer::PublishRestirStatus()
{
    ReSTIR::BackendStatus next[static_cast<size_t>(ReSTIR::Stage::Count)];
    const RG::RenderPass* passes[] = { m_rtDiPass, m_rtGiPass, m_pathTracePass };
    for (size_t i = 0; i < std::size(next); ++i)
    {
        const auto stage = static_cast<ReSTIR::Stage>(i);
        if (stage == ReSTIR::Stage::DI && m_rtDiPass) next[i] = m_rtDiPass->GetBackendStatus();
        else if (stage == ReSTIR::Stage::GI && m_rtGiPass) next[i] = m_rtGiPass->GetBackendStatus();
        else if (stage == ReSTIR::Stage::PT && m_pathTracePass) next[i] = m_pathTracePass->GetBackendStatus();
        if (!passes[i])
        {
            // No pass (no inline ray tracing): nothing runs; report the request.
            auto& s = next[i];
            s.requested = GetRestirBackend(stage);
            s.requestedName = ReSTIR::BackendName(s.requested);
            s.effective = ReSTIR::DefaultBackend(stage);
            s.effectiveName = ReSTIR::BackendName(s.effective);
            s.reason = "no inline ray tracing";
        }
    }
    // GI's SHaRC cache, for caps().restir.gi.sharc.
    GISharcStatus sharc;
    sharc.requested = GetRestirGISharc();
    sharc.mode = GetRestirGISharcMode();
    sharc.reason = m_rtGiPass ? "off" : "no inline ray tracing";
    if (m_rtGiPass)
    {
        const RTGIPass::SharcStatus s = m_rtGiPass->GetSharcStatus();
        sharc.requested = s.requested; sharc.effective = s.effective; sharc.reason = s.reason;
        sharc.mode = s.mode; sharc.bytes = s.bytes; sharc.clears = s.clears;
    }
    // The denoisers the shells run, for the editor.
    const char* diDenoiser = m_rtDiPass ? m_rtDiPass->EffectiveDenoiserName() : "none";
    const char* giDenoiser = m_rtGiPass ? m_rtGiPass->EffectiveDenoiserName() : "none";
    bool switched = false;
    {
        std::lock_guard<std::mutex> lock(m_restirStatusMutex);
        m_restirDiDenoiserEffective = diDenoiser;
        m_restirGiDenoiserEffective = giDenoiser;
        for (size_t i = 0; i < std::size(next); ++i)
        {
            if (m_restirStatusPublished && std::strcmp(m_restirStatus[i].effectiveName, next[i].effectiveName) != 0)
                switched = true;
            m_restirStatus[i] = std::move(next[i]);
        }
        m_restirStatusPublished = true;
        m_giSharcStatus = sharc;
    }
    if (switched) m_restirBackendSwitched.store(true, std::memory_order_release);
    const auto compare = static_cast<uint8_t>(GetRestirCompare());
    if (compare != m_restirCompareLogged)
    {
        m_restirCompareLogged = compare;
        LOG_INFO("[RESTIR_COMPARE] requested=%s effective=%s reason=%s",
            ReSTIR::CompareName(GetRestirCompare()), ReSTIR::CompareName(GetRestirCompareEffective()), GetRestirCompareReason());
    }
}

void Renderer::ExtractShadowAtlasDemand(World& world)
{
    m_wantSpotAtlas = m_wantPointAtlas = false;
    const auto* lights = world.GetPool<LightData>();
    if (!lights) return;
    for (const LightData& ld : lights->Data())
    {
        if (!ld.enabled || !ld.castsShadow || ld.radius <= 0.0f) continue;
        if (ld.type == LightType::Spot)
            m_wantSpotAtlas  = m_wantSpotAtlas || (ld.spotAngle > 0.0f);
        else if (ld.type == LightType::Point)
            m_wantPointAtlas = true;
        if (m_wantSpotAtlas && m_wantPointAtlas) break;
    }
}

void Renderer::ServiceEnableGatedResources(uint32_t viewportW, uint32_t viewportH)
{
    // Called by App at the parked frame boundary, before BeginFrame (see the
    // declaration): the disable paths may hard-sync through
    // WaitIdleAndReleaseDeferred (NrdDenoiser tears its pool down at once).
    // Requests follow the intent flags, not the passes' m_active: m_active needs the
    // output SRV to exist, so gating creation on it deadlocks.
    // The passes' service hooks run in stages with the Renderer's own steps between
    // them (each stage's order is RegisterServiceOrder's): the Renderer fills each
    // pass's request from its settings and the frame's plan, and the pass applies
    // it in OnService.
    RG::ServiceContext service;
    service.gfx = &m_gfx;
    // A failed RR evaluate re-arms by itself (DLSSPass::ServiceRetry).
    m_passes.Service(RG::ServiceStage::Retry, service);
    UpdateAAEnabled();
    // The upscalers' context (FSR2Pass / DLSSPass::OnService): create or resize
    // the one Stage A (GamePlan) selects and give the other's VRAM back. Here, not
    // in UploadFrameData: the resolve below sizes the post chain and decides RR
    // from whether the context exists, so a later create would resolve the frame to
    // render size on this half and display size on the render half, and the
    // group-D residents would be rebuilt in Phase 5, where growing the alias heap
    // idles the GPU. Uses the device's render size (set by App) and App's viewport,
    // as UploadFrameData reads them; skipped on a zero-size viewport. Main thread,
    // before the compute worker kicks; DLSS's rebuild takes no Streamline frame
    // token.
    if (viewportW != 0 && viewportH != 0)
    {
        const FramePlan::Decisions& stageA = m_gamePlan;
        uint32_t renderW = m_gfx.GetRenderWidth(), renderH = m_gfx.GetRenderHeight();
        if (renderW == 0 || renderH == 0) ComputeRenderResolution(viewportW, viewportH, renderW, renderH);
        if (m_fsr2Pass) m_fsr2Pass->Request() = { stageA.fsr, renderW, renderH, viewportW, viewportH };
        if (m_dlssPass)
        {
            DLSSPass::ServiceRequest& r = m_dlssPass->Request();
            r.upscaler = stageA.dlssWanted;
            if (r.upscaler)   // the getters read their flags only on a DLSS frame
            {
                r.rr = stageA.rr;
                r.rrModel = GetDLSSRRModel();
                r.quality = GetDLSSQualityMode();
                r.renderW = renderW; r.renderH = renderH; r.displayW = viewportW; r.displayH = viewportH;
            }
        }
        m_passes.Service(RG::ServiceStage::Upscalers, service);
    }
    // This frame's plan as the game half has it: the intent CaptureFrameIntent
    // took and the resolve at this frame's sizes (after ServiceRetry and the
    // upscalers above, so DLSS's health and both contexts are this frame's).
    const FramePlan::Intent& intent = FrameViewWrite().planIntent;
    const FramePlan::Decisions resolve = PlanGameResolve(viewportW, viewportH);
    // The budget ladder's last stage renders a ReSTIR path as Raster
    // (GetRenderPath); the passes keep their working set (retain) for its recovery
    // probe. Any other "not wanted" releases it.
    FramePlan::State ladder;
    ladder.budgetRaster = m_restirBudgetStage.load(std::memory_order_relaxed) == 4;
    const RenderPath effectivePath = static_cast<RenderPath>(FramePlan::EffectivePath(intent, ladder));
    const RenderPath ladderPath = static_cast<RenderPath>(intent.path);
    const bool ptPath = IsPathTrace(effectivePath);
    if (!ptPath)
        m_ddgiSceneAS.ReleasePathMaterials(m_gfx);
    // The path tracer's whole request; its direct light is serviced first
    // (ServiceStage::DirectLight), the rest after ReSTIR DI / GI. The reference path
    // tracer's camera-ray modes and film jitter (which would send the real-time
    // path's auto to the engine path tracer) are held off on every other path
    // (PTCameraModesHeld): the settings stay and return with Path Trace
    // (Reference); a command-line pin (DevPin) is not held. The path picks the
    // core: real-time = RTXDI's ReSTIR PT with the SHaRC preset, reference = the
    // engine path tracer. On this path the glass's path-traced reflection is the
    // forward reflection fold's front tier (PathTracePass::SetFrontLayer); toon
    // receivers keep their model and take its traced ambient and sun visibility
    // (SetNprLighting).
    if (m_pathTracePass)
    {
        const bool held = PTCameraModesHeld();
        PathTracePass::ServiceRequest& r = m_pathTracePass->Request();
        r.directLight = GetPTDirectLight();
        r.areaMode = held ? 0u : GetPTAreaMode();
        r.areaMotion = GetPTAreaMotion();
        r.areaHistory = GetPTAreaHistory();
        r.areaSpatialK = GetPTAreaSpatialK();
        r.areaSpatialRadius = GetPTAreaSpatialRadius();
        r.areaReconnect = GetPTAreaReconnect();
        r.areaMotionPinned = IsDevPinned(DevPin::PTAreaMotion);
        r.rrRuns = resolve.rrRuns;
        r.realtime = effectivePath == RenderPath::PathTraceRealtime;
        r.maxBounces = GetPTMaxBounces();
        r.reference = GetPTReferenceArm();
        r.bouncesPinned = IsDevPinned(DevPin::PTMaxBounces);
        r.referencePinned = IsDevPinned(DevPin::PTReferenceArm);
        r.cameraPrimary = (!held || IsDevPinned(DevPin::PTCameraPrimary)) && GetPTCameraPrimary();
        r.filmJitter = (!held || IsDevPinned(DevPin::PTFilmJitter)) && GetPTFilmJitter();
        r.primaryRayCone = GetPTPrimaryRayCone();
        r.lensRadius = held && !IsDevPinned(DevPin::PTLensRadius) ? 0.f : GetPTLensRadius();
        r.focusDistance = GetPTFocusDistance();
        r.backend = GetRestirBackend(ReSTIR::Stage::PT);
        r.frontReflection = ptPath && m_frontLayerPass && m_transparentPass && intent.transparentReflections;
        r.nprLighting = ptPath;
        r.denoise = effectivePath == RenderPath::PathTrace && GetPTDenoise();
        r.denoiseFadeSpp = GetPTDenoiseFadeSpp();
        r.wanted = ptPath;
    }
    // The direct-light stage first: ptWantsDI below reads its effective value, so
    // this frame decides on the setting it applies - at boot too, where
    // -rtxdi_pt_di / -rtxdi_pt_unified (old spellings of raytracing.ptDirectLight)
    // reach the pass only here. A change remakes the core.
    m_passes.Service(RG::ServiceStage::DirectLight, service);
    // The PT's rtxdi mode composites the primary's local lights from ReSTIR DI (as
    // RTXDI's own sample does), so the DI pass runs on the path-trace path too -
    // unless -rtxdi_pt_di=0 gives the primary the core's own next-event sample. An
    // auto request that resolved to rtxdi last frame counts; a frame without the DI
    // result is still exact (the core's own next-event sample).
    const ReSTIR::Backend ptRequest = GetRestirBackend(ReSTIR::Stage::PT);
    const bool ptWantsDI = IsPathTrace(effectivePath) &&
        (ptRequest == ReSTIR::Backend::Rtxdi ||
         (ptRequest == ReSTIR::Backend::Auto && m_pathTracePass &&
          m_pathTracePass->GetResolvedBackend() == ReSTIR::Backend::Rtxdi)) &&
        IsRestirBackendAvailable(ReSTIR::Stage::PT, ReSTIR::Backend::Rtxdi) && PTRtxdi::UsesRestirDI();
    auto diOn = [ptWantsDI](RenderPath p) { return p == RenderPath::ReSTIR_DI || p == RenderPath::ReSTIR_DI_GI || ptWantsDI; };
    auto giOn = [](RenderPath p) { return p == RenderPath::ReSTIR_GI || p == RenderPath::ReSTIR_DI_GI; };
    // Ray reconstruction denoises the ReSTIR DI / GI signal itself: the shells
    // skip NRD and keep their configured resolution, half by default (RTDIPass /
    // RTGIPass::SetRayReconstruction). Not after an evaluate failure: NRD comes
    // back.
    const bool rrRestir = !ptPath && resolve.rrRuns;
    // On the path-trace path ReSTIR DI is the path tracer's input: RR (or, without
    // it, progressive accumulation) denoises the composite, so the DI shell hands
    // over its raw estimate at full resolution with the light distance RR's
    // specular guide needs (the shell's RR mode). With NRD and the half-resolution
    // upsample in front of RR, a spot-lit floor boiled 2-3x more than with the
    // core's own next-event sample - which RR in turn over-brightens (+22% against
    // the reference, where raw DI lands within 1.1%).
    const bool diRaw = rrRestir || ptWantsDI;
    if (m_rtDiPass)
    {
        RTDIPass::Settings s;
        s.distance = GetDIRayTMax(); s.spatialPasses = GetRestirDISpatialPasses(); s.spatialRadius = GetRestirDISpatialRadius();
        s.spatialK = GetRestirDIK(); s.maxM = GetRestirDIMCap(); s.maxAge = GetRestirDIAgeCap(); s.reuse = GetRestirDIReuse();
        s.candidates = GetRestirDICandidates(); s.fullResolution = GetRestirDIFullRes();
        s.specularAntiFirefly = GetRestirDISpecAntiFirefly();
        s.denoiser = GetRestirDIDenoiser();
        // Which of them are command-line values (DevPin).
        s.reusePinned = IsDevPinned(DevPin::RestirDiReuse);
        s.spatialPassesPinned = IsDevPinned(DevPin::RestirDiSpatialPasses);
        s.spatialRadiusPinned = IsDevPinned(DevPin::RestirDiSpatialRadius);
        s.ageCapPinned = IsDevPinned(DevPin::RestirDiAgeCap);
        RTDIPass::ServiceRequest& r = m_rtDiPass->Request();
        r.settings = s;
        r.backend = GetRestirBackend(ReSTIR::Stage::DI);
        r.raw = diRaw;
        r.wanted = diOn(effectivePath);
        r.retain = diOn(ladderPath);
        // Compare mode: the shadow instance runs the other core (the board says which
        // the main pass runs).
        if (m_rtDiShadowPass)
        {
            m_restirCompareDiActive = GetRestirCompareEffective() == ReSTIR::Compare::DI && diOn(effectivePath);
            RTDIPass::ServiceRequest& sr = m_rtDiShadowPass->Request();
            sr.settings = s;
            sr.wanted = m_restirCompareDiActive;
        }
    }
    if (m_rtGiPass)
    {
        const bool giPath = giOn(effectivePath);
        RTGIPass::Settings s;
        s.maxDistance = GetRestirGIMaxDistance(); s.tier = GetRestirGITier(); s.maxAge = GetRestirGIAgeCap();
        s.reuse = GetRestirGIReuse(); s.spatialRadius = GetRestirGISpatialRadius(); s.spatial = GetRestirGISpatial();
        s.fullResolution = GetRestirGIFullRes(); s.initialCandidates = GetRestirGIInitialCandidates();
        s.emitterCandidates = GetRestirGIEmitterCandidates(); s.hitExactLights = GetRestirGIHitExactLights();
        s.sharc = GetRestirGISharc(); s.sharcMode = GetRestirGISharcMode();
        s.denoiser = GetRestirGIDenoiser();
        // Which of them are command-line values (DevPin).
        s.spatialRadiusPinned = IsDevPinned(DevPin::RestirGiSpatialRadius);
        s.sharcPinned = IsDevPinned(DevPin::RestirGiSharc);
        RTGIPass::ServiceRequest& r = m_rtGiPass->Request();
        r.settings = s;
        r.backend = GetRestirBackend(ReSTIR::Stage::GI);
        r.raw = rrRestir;
        r.wanted = giPath;
        r.retain = giOn(ladderPath);
        // Compare mode: the shadow instance runs the other core.
        if (m_rtGiShadowPass)
        {
            m_restirCompareGiActive = GetRestirCompareEffective() == ReSTIR::Compare::GI && giPath;
            RTGIPass::ServiceRequest& sr = m_rtGiShadowPass->Request();
            sr.settings = s;
            sr.wanted = m_restirCompareGiActive;
        }
    }
    m_passes.Service(RG::ServiceStage::Restir, service);
    if (m_rtGiPass)
    {
        // The GI emitter table (emitter sampling) is built only for the GI paths.
        const bool giPath = giOn(effectivePath);
        m_ddgiSceneAS.SetGIEmittersWanted(giPath);
        if (!giPath) m_ddgiSceneAS.ReleaseGIEmitters(m_gfx);
    }
    // The path tracer's service (its request was filled above).
    m_passes.Service(RG::ServiceStage::PathTrace, service);
    // The static BLAS set has just completed (a world's first burst, ~16 a frame,
    // or static meshes that arrived later): the SHaRC caches forget the light their
    // paths took through slabs not yet in the TLAS - otherwise the multi-bounce
    // cache kept another floor's light for ~400 frames after a load. Serviced by
    // the path tracer, then ReSTIR GI's main pass (their OnService).
    if (const uint32_t epoch = m_ddgiSceneAS.StaticGeometryEpoch(); epoch != m_staticGeometryEpochSeen)
    {
        m_staticGeometryEpochSeen = epoch;
        m_passes.Service(RG::ServiceStage::StaticGeometry, service);
    }
    // Lights v2 rings, created the first frame a consumer is wanted - after the
    // passes above, so those passes' working sets keep the descriptor indices they
    // would have without the rings. The DI history follows its lights through the key
    // map (and the previous list, which the RTXDI core reads); the path tracer samples
    // its next-event lights from the power alias table.
    if (m_clusterPass)
    {
        ClusterPass::ServiceRequest& r = m_clusterPass->Request();
        // The ray-traced fog's reservoirs follow their lights through the key map too.
        r.mapAndPrevious = diOn(effectivePath) || diOn(ladderPath) ||
                           (m_volFogPass && VolumetricFogPass::RtMode() != 0);
        r.alias = IsPathTrace(effectivePath);
    }
    // The PDF texture + RIS buffer: wanted while the DI core draws RTXDI
    // candidates (rtxdi / hybrid; the board), or forced by -light_tables.
    if (m_lightSamplingPass)
    {
        static const bool s_lightTables = Sys::CommandLine::Get().Has("-light_tables");
        m_lightSamplingPass->Request().forced = s_lightTables;
    }
    m_passes.Service(RG::ServiceStage::Lights, service);
    PublishRestirStatus();
    // A ReSTIR history reset requested at this service point resets RR's history
    // too, in this frame's Streamline constants (DLSSPass::OnService).
    if (m_dlssPass) m_dlssPass->Request().restirReset = !ptPath && resolve.rr;
    m_passes.Service(RG::ServiceStage::RRReset, service);
    // The hybrid reflection pass's hardware tier (one inline-RayQuery PSO, ~0.85 s
    // to create) is built the first frame RT reflections are wanted; its
    // screen-space tier (SSR) is built at Init. rtrActive / frontActive wait for
    // HasHWTier. Under RR the reflections are denoised by RR too: REBLUR off, the
    // raw trace and its hit distance kept for RRGuidePass (WorkingSets, after both
    // tiers).
    {
        const bool hwTier = intent.rtReflections && !ptPath;
        if (m_rtReflectionPass)
        {
            m_rtReflectionPass->Request().hwTier = hwTier;
            m_rtReflectionPass->Request().rr = rrRestir;
        }
        if (m_rtReflectionFrontPass) m_rtReflectionFrontPass->Request().hwTier = hwTier;
    }
    m_passes.Service(RG::ServiceStage::RTReflectionTier, service);
    // The colour before transparency, on either path while RR runs
    // (-dlss_rr_no_color_before_transparency: the A/B arm without it).
    if (m_rrColorSnapshot)
        m_rrColorSnapshot->Request().wanted =
            resolve.rrRuns && !Sys::CommandLine::Get().Has("-dlss_rr_no_color_before_transparency");
    // The reactive mask, while FSR 2 or DLSS SR really runs (resolved: a selected
    // upscaler that did not come up reads nothing) - not DLSS-RR, which is not
    // given it (UpscalerReactiveMaskPass.h).
    // -reactive_off (harness): the A/B arm without it.
    if (m_reactiveMaskPass)
    {
        bool off = false;
#if defined(WITH_HARNESS) && WITH_HARNESS
        static const bool s_off = Sys::CommandLine::Get().Has("-reactive_off");
        off = s_off;
#endif
        const bool upscalerReads = resolve.fsrResolved || (resolve.dlssResolved && !resolve.rr);
        m_reactiveMaskPass->Request().wanted = upscalerReads && !off;
    }
    // The working sets follow the intent's switches (the render half's RT
    // budget watchdog writes them back on its frame; the next intent has it).
    if (m_rtaoPass) m_rtaoPass->Request().wanted = intent.ssao && intent.rtao;
    if (m_ssgiPass)
    {
        SSGIPass::ServiceRequest& r = m_ssgiPass->Request();
        r.reservoir = m_ddgiSettings.ssgiReservoirEnabled;
        r.wanted = m_ddgiSettings.ssgiEnabled;
        r.hiZOnly = giOn(effectivePath);
    }
    if (m_rtShadowPass) m_rtShadowPass->Request().wanted = intent.rtShadows;
    // The debug target view's pipeline: built the first frame a target is
    // asked for (Renderer/DebugTargets.h), kept once built.
    if (m_debugTargetPass)
    {
        DebugTargetViewPass::ServiceRequest& r = m_debugTargetPass->Request();
        // The pass capture's picture is painted with its shader too.
        r.wanted = m_debugTargetWanted.load(std::memory_order_acquire) || PassCapturePictureWanted();
        // The debug targets made this frame - the shown one's producer and the ones a
        // capture asked for - with textures at this frame's render size; the render
        // half reads which from the FrameView.
        r.produce = DebugTargetProduceMask();
        r.renderW = m_gfx.GetRenderWidth();
        r.renderH = m_gfx.GetRenderHeight();
        r.materialCapacity = GetMaterialSlotCapacity();   // the material cost table's slots
        FrameViewWrite().debugProduce = r.produce;
    }
    else
        FrameViewWrite().debugProduce = 0;
    m_passes.Service(RG::ServiceStage::WorkingSets, service);
    // debugShade: the material pixel shaders count into the debug targets'
    // counters this frame once a producer that reads one runs and its counter
    // exists (made by the OnService above).
    FrameViewWrite().debugShade =
        m_debugTargetPass && m_debugTargetPass->DebugShadeReady(FrameViewWrite().debugProduce);
    // Texture Viewer pictures: apply the latched requests / releases, (re)make
    // their targets, build the pipeline the first time one is live. A no-op
    // with no view.
    if (m_textureViews) m_textureViews->ServiceFrameBoundary();
    // UI Editor designer pictures: the same latch / target / first-use pipeline
    // shape. Must precede the editor's UI build (a fresh target's SRV is what
    // ImGui::Image captures this frame) - it does: this is the serial window.
    if (m_uiViews) m_uiViews->ServiceFrameBoundary();
    // The pass capture (Renderer/PassCapture.h): apply the latched request, make
    // its copy and picture, arm the graph. Nothing asked: nothing armed.
    ServicePassCapture();
    if (m_volFogPass)
    {
        const uint8_t bits = m_volFogDemandBits.load(std::memory_order_relaxed);
        VolumetricFogPass::FogDemand d;
        d.sceneOwned   = (bits & kFogDemandOwned)   != 0;
        d.sceneEnabled = (bits & kFogDemandEnabled) != 0;
        d.sceneGodRays = (bits & kFogDemandGodRays) != 0;
        d.anyLocal     = (bits & kFogDemandLocal)   != 0;
        // The ray-traced lighting's reservoirs and grids while the game half's plan gives it the
        // fog (stage A: its row not off, and under auto a frame that traces rays); the render
        // half's stage B decides each frame whether it runs. A raster frame's fog allocates none.
        d.rt           = VolumetricFogPass::RtMode() != 0 && m_gamePlan.fogOwner != FramePlan::FogOwner::Raster;
        m_volFogPass->Request().demand = d;
    }
    // Then the residents: the froxels, the star cubemap (48 MiB,
    // SkyboxPass::OnService), the group-D post residents and the shadow atlases, in
    // that order.

    // Group-D residents can grow the shared alias heap: size them before any frame
    // commands are recorded, while the heap's retirement fence is legal - even when
    // no RT pass provisioned the arena. Use the plan's resolve at this frame's
    // viewport (PlanGameResolve), not GetWidth/Height: the editor's viewport is
    // smaller than the swap chain, and the window size would rebuild these passes
    // every frame with FSR / DLSS on. FrameView is published later, so the viewport
    // comes from App.
    const uint32_t postW = resolve.postW;
    const uint32_t postH = resolve.postH;
    if (m_bloomPass) m_bloomPass->Request() = { postW, postH };
    if (m_nprStabilizePass) m_nprStabilizePass->Request() = { m_gfx.GetRenderWidth(), m_gfx.GetRenderHeight() };
    if (m_lensFlarePass) m_lensFlarePass->Request() = { postW, postH };

    // Spot (256 MiB base) + point (96 MiB) shadow atlases, from a direct LightData pool
    // scan (ExtractShadowAtlasDemand). The obvious sources - last frame's
    // activeCasters, m_spotCullCount, the caster high-water - are computed in
    // BuildScene_UploadLights blocks gated on atlas residency, so deriving demand
    // from them deadlocks: no atlas -> 0 casters -> not wanted -> never allocated.
    // The scan mirrors BuildScene_UploadLights' qualifier tests (type, castsShadow,
    // radius, spotAngle for spots) and ignores the kMaxCasters cap, visibility and
    // the per-frame light gather: over-approximating costs at worst an unsampled
    // atlas, under-approximating a missing shadow.
    // At most one frame of lag: the demand is read at the top of the game half, after
    // the frame's simulation, so only a caster added later in that game half (the
    // editor UI) allocates on the next frame; until then it renders without its
    // shadow rather than with a wrong one (BuildScene_UploadLights withholds
    // shadowSliceIdx = 0xFFFFFFFF and every shader takes its no-shadow branch).
    if (m_spotShadowPass || m_pointShadowPass)
    {
        // Demand comes from ExtractShadowAtlasDemand, called on the game half just
        // before this, so this function, which may hard-sync, touches no World.
        const bool wantSpot  = m_wantSpotAtlas;
        const bool wantPoint = m_wantPointAtlas;
        // Ensure* only allocates: !wanted is never a release. Release is per-world
        // sticky (OnWorldClear), so a light toggled off and on, or a momentarily
        // light-less world, cannot churn the atlases.
        if (m_spotShadowPass)  m_spotShadowPass->Request().wanted = wantSpot;
        if (m_pointShadowPass) m_pointShadowPass->Request().wanted = wantPoint;
    }
    m_passes.Service(RG::ServiceStage::Residents, service);
}

uint32_t Renderer::GetPathTraceSamples() const
{ return m_pathTracePass ? m_pathTracePass->GetPublishedSamples() : 0; }

float Renderer::BeginWorldClock(float dt)
{
    const bool frozen = m_ptFreezeTime.load() && GetRenderPath() == RenderPath::PathTrace;
    if (frozen != m_worldClockFrozen.load(std::memory_order_relaxed))
        LOG_INFO("[PT_FREEZE] frozen=%u", frozen ? 1u : 0u);
    m_worldClockFrozen.store(frozen, std::memory_order_relaxed);
    float worldDt = dt;
    m_frameWorldStepped = false;
    if (frozen)
    {
        worldDt = 0.f;
        const float step = m_worldStepSeconds.exchange(0.f);
        if (step > 0.f)
        {
            worldDt = step;
            m_frameWorldStepped = true;
            const uint32_t n = m_worldStepsDone.fetch_add(1, std::memory_order_relaxed) + 1;
            LOG_INFO("[PT_FREEZE] step=%u seconds=%.4f", n, step);
        }
    }
    else
    {
        m_worldStepSeconds.store(0.f);   // a step asked for while not frozen is dropped
    }
    m_frameWorldDt = worldDt;
    m_frameWorldDtSet = true;
    return worldDt;
}

bool Renderer::RequestWorldStep(float seconds)
{
    if (!m_worldClockFrozen.load(std::memory_order_relaxed) || !(seconds > 0.f) || !(seconds <= 1.f))
        return false;
    m_worldStepSeconds.store(seconds);
    return true;
}
uint32_t Renderer::GetPathTraceResets() const
{ return m_pathTracePass ? m_pathTracePass->GetPublishedResets() : 0; }
