// Renderer_RenderAsync.cpp - Renderer::RenderAsync, the frame's render
// coroutine: pass setup, the render graph's execute and the submits after it;
// also the frame plan's state reads (ReadPlanState, PlanStage, PlanGame*) and
// the frame capture. Uses no file-local symbol of Renderer.cpp, only the
// RendererDetail cbuffer aliases every Renderer satellite repeats.

#include "Diagnostics/Profiler.h"
#include <optional>
#include "System/Check.h"
#include "System/PathUtil.h"
#include "System/WordHash.h"
#include "Renderer/Renderer.h"
#include "Scene/MeshAssetRegistry.h"
#include "Resource/MeshLibIdentity.h"

// engine graphics / backend
#include "Graphics/GraphicsDX12.h"
#include "Graphics/DepthTrace.h"
#include "Graphics/ParticleSystem.h"
#include "Graphics/TracerSystem.h"
#include "Graphics/BeamSystem.h"
#include "Graphics/AfterimageSystem.h"
#include "Graphics/TrailSystem.h"
#include "Graphics/SSR/SSRSubsystem.h"

// render passes
#include "Renderer/Preview/PreviewRenderer.h"
#include "RenderGraph/RenderPass/DDGIPass.h"
#include "RenderGraph/RenderPass/DDGIProbeDebugPass.h"
#include "RenderGraph/RenderPass/GBufferPass.h"
#include "RenderGraph/RenderPass/SurfaceCacheBakePass.h"
#include "RenderGraph/RenderPass/WaterPass.h"
#include "RenderGraph/RenderPass/WaterSurfacePass.h"
#include "RenderGraph/RenderPass/LightingPass.h"
#include "RenderGraph/RenderPass/TransparentPass.h"
#include "RenderGraph/RenderPass/SkyboxPass.h"
#include "RenderGraph/RenderPass/ShadowPass.h"
#include "RenderGraph/RenderPass/AutoExposurePass.h"
#include "RenderGraph/RenderPass/NPRStabilizePass.h"
#include "RenderGraph/RenderPass/BloomPass.h"
#include "RenderGraph/RenderPass/LensFlarePass.h"
#include "RenderGraph/RenderPass/ToneMapPass.h"
#include "RenderGraph/RenderPass/LumaReducePass.h"
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
#include "RenderGraph/RenderPass/SkyIBLPass.h"
#include "RenderGraph/RenderPass/VolumetricFogPass.h"
#include "Graphics/WaterRippleSystem.h"
#include "RenderGraph/RenderPass/ClusterPass.h"
#include "RenderGraph/RenderPass/DecalPass.h"
#include "RenderGraph/RenderPass/RTReflectionPass.h"
#include "RenderGraph/RenderPass/TranslucentFrontLayerPass.h"
#include "RenderGraph/RenderPass/RTShadowPass.h"
#include "RenderGraph/RenderPass/PathTracePass.h"
#include "RenderGraph/RenderPass/RTDIPass.h"
#include "RenderGraph/RenderPass/LightSamplingPass.h"
#include "RenderGraph/RenderPass/RtxdiProbe.h"
#include "RenderGraph/RenderPass/RTGIPass.h"
#include "RenderGraph/RenderPass/RTAOPass.h"
#include "RenderGraph/RenderPass/SSGIPass.h"
#include "RenderGraph/RenderPass/SpotShadowPass.h"
#include "RenderGraph/RenderPass/PointShadowPass.h"
#include "RenderGraph/RenderPass/PerObjectShadowPass.h"
#include "RenderGraph/RenderPass/HairShadowMarkPass.h"
#include "RenderGraph/RenderPass/FaceOverlayPass.h"
#include "RenderGraph/RenderPass/ParticlePasses.h"
#include "RenderGraph/RenderPass/TracerPasses.h"
#include "RenderGraph/RenderPass/BeamSimPass.h"
#include "RenderGraph/RenderPass/TrailPasses.h"
#include "RenderGraph/RenderPass/BillboardFXPass.h"
#include "RenderGraph/RenderPass/CullingPass.h"
#include "RenderGraph/RenderPass/HiZPass.h"
#include "RenderGraph/RenderPass/DebugWirePass.h"
#include "RenderGraph/RenderPass/UIPass.h"
#include "RenderGraph/RenderPass/WorldUIBillboardPass.h"
#include "RenderGraph/RenderPass/DebugIconPass.h"
#include "RenderGraph/RenderPass/CloudPass.h"
#include "RenderGraph/RenderPass/EditorGridPass.h"

// post-process
#include "PostProcess/PostChain.h"
#include "PostProcess/BuiltinPostProcessEffects.h"
#include "PostProcess/PostProcessRuntime.h"

// ECS components + systems
#include "ECS/ECS.h"
#include "ECS/IKRigSystem.h"
#include "ECS/Components.h"
#include "ECS/HierarchyComponents.h"

// resource system
#include "Resource/MaterialSerializer.h"

// engine systems
#include "System/Log.h"
#include "System/BootTimeline.h"
#include "System/JobDefaults.h"
#include "System/CommandLine.h"   // -phaselog_every gate for [RENDERZONE]
#include "System/Check.h"     // ENGINE_CHECK_ONCE tripwires
#include "RenderGraph/FrameCapture.h"   // ArmFrameCapture / PublishFrameCapture
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
#include <cstdlib>            // _dupenv_s / atoi - DX12_FWDREFL_DEBUG
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
// FramePlan (Renderer/FramePlan.h) takes the render path and the AA mode by
// value; these asserts keep its enums in step with Renderer's.
using RP = Renderer::RenderPath;
using FP = FramePlan::Path;
static_assert(uint32_t(RP::Raster) == uint32_t(FP::Raster) && uint32_t(RP::ReSTIR_DI) == uint32_t(FP::ReSTIR_DI) &&
              uint32_t(RP::ReSTIR_GI) == uint32_t(FP::ReSTIR_GI) && uint32_t(RP::ReSTIR_DI_GI) == uint32_t(FP::ReSTIR_DI_GI) &&
              uint32_t(RP::PathTrace) == uint32_t(FP::PathTrace) &&
              uint32_t(RP::PathTraceRealtime) == uint32_t(FP::PathTraceRealtime) && uint32_t(RP::Count) == 6,
              "FramePlan::Path mirrors Renderer::RenderPath");
using AM = Renderer::AAMode;
using FA = FramePlan::AA;
static_assert(uint32_t(AM::None) == uint32_t(FA::None) && uint32_t(AM::FXAA) == uint32_t(FA::FXAA) &&
              uint32_t(AM::TAA) == uint32_t(FA::TAA) && uint32_t(AM::FXAA_TAA) == uint32_t(FA::FXAA_TAA) &&
              uint32_t(AM::FSR) == uint32_t(FA::FSR) && uint32_t(AM::DLSS) == uint32_t(FA::DLSS) &&
              uint32_t(AM::DLSS_RR) == uint32_t(FA::DLSS_RR) && uint32_t(AM::Count) == 7,
              "FramePlan::AA mirrors Renderer::AAMode");
}

// What exists and is ready for the frame @p view describes (Renderer/FramePlan.h)
// - no side effects, so the game half reads it too (GamePlan, PlanGameStageA,
// PlanGameResolve). @p renderHalf picks the LightCB slot: the frame being
// rendered (Current) or the frame the game half builds (Upload).
FramePlan::State Renderer::ReadPlanState(const FrameView& view, bool renderHalf)
{
    FramePlan::State s;
    s.budgetRaster = m_restirBudgetStage.load(std::memory_order_relaxed) == 4;
    s.ortho        = view.camera.orthographic;
    // Sun gate: lightColor is zeroed every frame when no directional light
    // exists; lightDir is deliberately left stale (never test it).
    if (const auto* lb = renderHalf ? m_lightCB.Current(m_gfx) : m_lightCB.Upload(m_gfx))
        s.sun = (lb->lightColor[0] + lb->lightColor[1] + lb->lightColor[2]) > 0.0f;
    s.ddgiEnabled  = m_ddgiSettings.ddgiEnabled;
    s.ssgiEnabled  = m_ddgiSettings.ssgiEnabled;
    s.ddgiReady    = m_ddgiReady && m_ddgiPass;
    s.ddgiVolumes  = static_cast<uint32_t>(m_ddgiMgr.GetActiveVolumeCount());
    s.refl = m_rtReflectionPass != nullptr;
    if (m_rtReflectionPass)
    {
        s.reflReady   = m_rtReflectionPass->IsReady();
        s.reflHwTier  = m_rtReflectionPass->HasHWTier();
        s.reflUnified = m_rtReflectionPass->IsUnifiedAvailable();
    }
    s.front           = m_rtReflectionFrontPass && m_frontLayerPass;
    s.transparentPass = m_transparentPass != nullptr;
    if (m_rtReflectionFrontPass)
        s.frontReady = m_rtReflectionFrontPass->IsReady() && m_rtReflectionFrontPass->HasHWTier() &&
                       m_rtReflectionFrontPass->IsUnifiedAvailable();
    s.rtShadow = m_rtShadowPass != nullptr;
    if (m_rtShadowPass)
    {
        s.rtShadowReady  = m_rtShadowPass->IsReady();
        s.rtShadowOutput = m_rtShadowPass->GetUpcomingOutputSrv() != 0;
    }
    s.rtao = m_rtaoPass != nullptr;
    if (m_rtaoPass)
    {
        s.rtaoReady  = m_rtaoPass->IsReady();
        s.rtaoOutput = m_rtaoPass->GetUpcomingOutputSrv() != 0;
    }
    s.ptPass = m_pathTracePass != nullptr;
    if (m_pathTracePass)
    {
        s.ptInert         = m_pathTracePass->GetInertReason() != nullptr;
        s.ptOutput        = m_pathTracePass->GetUpcomingOutputSrv() != 0;
        s.ptCameraPrimary = m_pathTracePass->UsesCameraPrimary();
        s.ptRestirDI      = m_pathTracePass->UsesRtxdi() && PTRtxdi::UsesRestirDI();
    }
    s.diPass = m_rtDiPass != nullptr;
    if (m_rtDiPass)
    {
        s.diInert  = m_rtDiPass->GetInertReason() != nullptr;
        s.diOutput = m_rtDiPass->GetUpcomingOutputSrv() != 0;
    }
    s.giPass = m_rtGiPass != nullptr;
    if (m_rtGiPass)
    {
        s.giInert  = m_rtGiPass->GetInertReason() != nullptr;
        s.giOutput = m_rtGiPass->GetUpcomingOutputSrv() != 0;
    }
    s.ssgiPass = m_ssgiPass != nullptr;
    if (m_ssgiPass)
        s.ssgiUsable = m_ssgiPass->GetInertReason() == nullptr && m_ssgiPass->GetUpcomingOutputSrv() != 0;
    s.xegtaoPass = m_xegtaoPass != nullptr;
    s.post = m_taaPass || m_autoExposurePass || m_bloomPass || m_toneMapPass;
    if (s.post)
    {
        s.slRR   = SL::StreamlineBackend::Get().IsFeatureSupported(SL::Feature::DLSS_RR);
        s.slDLSS = SL::StreamlineBackend::Get().IsFeatureSupported(SL::Feature::DLSS);
        // The flags are read under GetRayReconstructionInertReason's own
        // conditions, so run.json's unreadFlags does not change.
        s.rrForcedCameraRays = s.slRR &&
            FramePlan::EffectivePath(view.planIntent, s) == FramePlan::Path::PathTraceRealtime && RRCameraRaysForced();
        s.fsrContext  = m_fsr2Pass && m_fsr2Pass->IsContextValid();
        s.dlssContext = m_dlssPass && m_dlssPass->IsContextValid();
        s.dlssHealthy = m_dlssPass && m_dlssPass->IsHealthy();
        s.renderW     = view.renderWidth;
        s.renderH     = view.renderHeight;
        s.viewportW   = view.vpWidth;
        s.viewportH   = view.vpHeight;
        s.rrGuidePass = m_features.Has("RRGuide");
    }
    // The raster systems' readers on the path-tracing path (FramePlan.h).
    // Render_BindFrameResources counted the draws for this frame on the render
    // half; the game half reads the last frame's (see State). Every one is an
    // over-approximation where it is not exact - a reader counted that does not
    // sample costs a system that runs, one missed would sample a stale one.
    s.translucentDraws = m_transparentPass && m_transparentPass->IsActive();
    s.waterDraws = (m_waterPass && m_waterPass->HasWater()) || (m_waterSurfacePass && m_waterSurfacePass->HasWater());
    s.toonDraws = m_nprDrawCount > 0;
    s.ptToonLighting = m_pathTracePass && !m_pathTracePass->UsesCameraPrimary() && m_pathTracePass->NprLightingReady();
    s.particles = m_particleRenderPass && m_particleRenderPass->IsActive();
    s.fog = m_volFogPass && m_volFogPass->WantsFog();
    s.faceOverlay = m_faceOverlayPass && m_faceOverlayPass->IsActive();
    // The game half armed these for this frame (ExtractPreviewRequests,
    // ExtractProbeBakeRequest, BuildScene_UpdateDDGI's debug slot lane).
    s.previewWorldViews = m_planPreviewWorldViews > 0;
    s.probeBake = m_probeBakeReq.valid;
    s.ddgiDebug = m_ddgiProbeDebugPass && m_ddgiProbeDebugPass->enabled && m_ddgiProbeDebugPass->HasDebugVolumes();
    // Render_BindFrameResources uploaded this frame's medium on the render half; a game-half
    // read would race it, and no decision the game half reads depends on it.
    s.ptMedia = renderHalf && m_pathTracePass && m_pathTracePass->MediaEnabled() && m_mediumIdx != 0;
    s.fogRtMode = m_volFogPass ? VolumetricFogPass::RtMode() : 0;
    s.fogRtUsable = m_volFogPass && m_volFogPass->RtUsable();
#ifdef WITH_HARNESS
    // -frameplan_pt_inert (pack frame_plan_fallback): the plan believes the path
    // tracer's core is not usable - a path-tracing path falls back; the pass
    // itself is untouched. -frameplan_legacy_pt (State::legacyPtRule; the fallback
    // and veto packs' reverse arms): instead of the path tracer owning the frame,
    // the path-tracing path excludes the reflections and RTAO, and the path tracer
    // running excludes XeGTAO and SSGI. -frameplan_no_pt_readers
    // (State::ignorePtReaders; frame_plan_pt_readers' reverse arm): the raster
    // systems run on the path-tracing path whether anything reads them or not.
    static const bool s_ptInert   = Sys::CommandLine::Get().Has("-frameplan_pt_inert");
    static const bool s_legacy    = Sys::CommandLine::Get().Has("-frameplan_legacy_pt");
    static const bool s_noReaders = Sys::CommandLine::Get().Has("-frameplan_no_pt_readers");
    if (s_ptInert) s.ptInert = true;
    s.legacyPtRule    = s_legacy;
    s.ignorePtReaders = s_noReaders;
#endif
    return s;
}

// The render half's read, once, at its top - after the ReSTIR budget ladder
// moved for this frame (its last stage is part of the state).
void Renderer::CaptureFramePlanState()
{
    // Apply the NRD switch before ReadPlanState reads the reflection pass's
    // availability (IsUnifiedAvailable depends on it).
    if (m_rtReflectionPass) m_rtReflectionPass->SetUseNRD(m_rtrUseNRD);
    ENGINE_CHECK_ONCE(FrameViewRead().planIntent.captured,
        "[FRAMEPLAN] a frame reached the render half without Renderer::CaptureFrameIntent - the plan reads a "
        "default intent (Raster, every feature off); call it on the game half right after RenderPhaseBarrier");
    m_planState = ReadPlanState(FrameViewRead(), /*renderHalf=*/true);
}

void Renderer::PlanStage(FramePlan::Stage stage)
{
    m_planFacts.stage = stage;
    FramePlan::Intent intent = FrameViewRead().planIntent;
#ifdef WITH_HARNESS
    // -frameplan_break=<n> (the frame_plan packs' reverse arm): from the n-th
    // planned frame on, the plan believes RT reflections are switched off - the
    // packs' steady-state lines must catch rtr=0 / front=0.
    static const int s_break = Sys::CommandLine::Get().GetInt("-frameplan_break", -1);
    if (s_break >= 0 && m_planFrames + 1 >= static_cast<uint64_t>(s_break))
        intent.rtReflections = false;
#endif
    m_plan = FramePlan::PlanFrame(intent, m_planState, m_planFacts);
}

FramePlan::Decisions Renderer::PlanGameStageA()
{
    const FrameView& view = FrameViewWrite();
    return FramePlan::PlanFrame(view.planIntent, ReadPlanState(view, /*renderHalf=*/false), FramePlan::Facts{});
}

// The game half's resolve (stage C). The FrameView's sizes are
// UploadFrameData's, which runs later in this game half; the service sizes the
// post resources from THIS frame's viewport (App's) and render size (the
// device's, which App set).
FramePlan::Decisions Renderer::PlanGameResolve(uint32_t viewportW, uint32_t viewportH)
{
    const FrameView& view = FrameViewWrite();
    FramePlan::State s = ReadPlanState(view, /*renderHalf=*/false);
    s.viewportW = viewportW;
    s.viewportH = viewportH;
    s.renderW   = m_gfx.GetRenderWidth();
    s.renderH   = m_gfx.GetRenderHeight();
    FramePlan::Facts f;
    f.stage = FramePlan::Stage::C;
    // The flags are read only under the RR predicates' own conditions (RR
    // active; -rr_raster_guides only on a path-tracing path), so run.json's
    // unreadFlags does not change.
    const FramePlan::Decisions a = FramePlan::PlanFrame(view.planIntent, s, FramePlan::Facts{});
    const FramePlan::Path path = FramePlan::EffectivePath(view.planIntent, s);
    const bool traced = path == FramePlan::Path::PathTrace || path == FramePlan::Path::PathTraceRealtime;
    const auto& cmd = Sys::CommandLine::Get();
    f.rrRaw          = a.rr && cmd.Has("-dlss_rr_raw");
    f.rrRasterGuides = a.rr && traced ? cmd.GetInt("-rr_raster_guides", 0) : 0;
    f.ptRRReady      = m_pathTracePass && m_pathTracePass->RayReconstructionReady();
    return FramePlan::PlanFrame(view.planIntent, s, f);
}

Sys::OwnerTask Renderer::RenderAsync(Sys::OwnerTaskQueue& owner,
    std::optional<Sys::ExecutionDomain::Token> token, RHI::CommandList& result, uint32_t viewSlot)
{
    // Render needs the frame preparation published. m_laneEmitOwed covers
    // -brslane: there this thread runs PrepareFrameOnLane first, so reaching here
    // with it set means the emit half was skipped, not merely unpublished.
    if (m_pendingPublication || m_laneEmitOwed)
        throw std::logic_error("Render attempted before frame preparation publication");

    // A secondary view's call (viewSlot != 0, Renderer_Views.cpp): the per-view work below again,
    // through its camera into its own picture, inside its segment; the scene's work ran in the main
    // view's call this frame and is skipped (primary).
    const bool primary = viewSlot == 0;
    struct ViewSegmentEnd
    {
        Renderer* r    = nullptr;
        uint32_t  slot = 0;
        ~ViewSegmentEnd() { if (r) r->EndViewSegment(slot); }
    } viewSegmentEnd;
    if (!primary)
    {
        if (!BeginViewSegment(viewSlot))
        {
            result = {};
            co_return;
        }
        viewSegmentEnd.r    = this;
        viewSegmentEnd.slot = viewSlot;
    }

    if (primary)
    {
        // ReSTIR budget guard is opt-in; parent regions include their NRD children.
        // Keep the user's requested path intact while the final stage uses Raster.
        const auto& budgetCmd=Sys::CommandLine::Get();
        const float restirBudgetMs=budgetCmd.GetFloat("-restir_budget_ms",GetRestirBudgetMs());
        if((restirBudgetMs>0)!=m_restirBudgetWant)
        {
            m_restirBudgetWant=restirBudgetMs>0;
            if(m_restirBudgetWant) m_gfx.AddGPUProfilerWant();
            else
            {
                m_gfx.ReleaseGPUProfilerWant();
                m_restirBudget={};
                if(m_restirBudgetStage.exchange(0)!=0) m_renderPathChanged.store(true,std::memory_order_release);
            }
        }
        if(m_restirBackendSwitched.exchange(false,std::memory_order_acq_rel))
        {
            // A stage's effective core changed: the ladder's history measured the
            // other core, so it restarts like a path switch.
            m_restirBudget={};
            if(m_restirBudgetStage.exchange(0)!=0) m_renderPathChanged.store(true,std::memory_order_release);
        }
        if(restirBudgetMs>0)
        {
            RHI::GpuTimingView profiler; m_gfx.GetGPUTimings(profiler);
            const auto requested=m_renderPath.load(std::memory_order_relaxed);
            if(requested!=m_restirBudgetPath)
            {
                m_restirBudget={}; m_restirBudgetPath=requested;
                m_restirBudgetStage.store(0,std::memory_order_relaxed);
            }
            ++m_restirBudgetFrame;
            if(requested>=RenderPath::ReSTIR_DI && requested<=RenderPath::ReSTIR_DI_GI && profiler.hasResults)
            {
                float cost=0; bool measured=false;
                for(uint32_t i=0;i<profiler.resultCount;++i)
                {
                    const auto& r=profiler.results[i];
                    if(r.name && ((requested!=RenderPath::ReSTIR_GI && std::strcmp(r.name,"RTDI")==0)
                        || (requested!=RenderPath::ReSTIR_DI && std::strcmp(r.name,"RTGI")==0)))
                    { cost+=r.gpuMs; measured=true; }
                }
                // Deterministic harness release; production keeps the configured ceiling.
                const int releaseFrame=budgetCmd.GetInt("-restir_budget_release_frame",0);
                const float ceiling=releaseFrame>0 && m_restirBudgetFrame>=static_cast<uint64_t>(releaseFrame)
                    ? budgetCmd.GetFloat("-restir_budget_release_ms",100.f) : restirBudgetMs;
                const auto previous=m_restirBudget.stage;
                if(m_restirBudget.Observe(profiler.resultsSequence,cost,ceiling,measured))
                {
                    m_restirBudgetStage.store(m_restirBudget.stage,std::memory_order_relaxed);
                    m_renderPathChanged.store(true,std::memory_order_release);
                    LOG_INFO("[RESTIR_BUDGET] frame=%llu sequence=%llu stage=%u previous=%u emaMs=%.4f budgetMs=%.4f requested=%s effective=%s reason=%s",
                        static_cast<unsigned long long>(m_restirBudgetFrame),static_cast<unsigned long long>(profiler.resultsSequence),
                        m_restirBudget.stage,previous,m_restirBudget.ema,ceiling,RenderPathName(requested),RenderPathName(GetRenderPath()),
                        m_restirBudget.stage>previous ? "over-budget" : "recovery-probe");
                }
            }
        }
        if(m_rtDiPass) m_rtDiPass->SetBudgetStage(m_restirBudgetStage.load(std::memory_order_relaxed));
        if(m_rtDiShadowPass) m_rtDiShadowPass->SetBudgetStage(m_restirBudgetStage.load(std::memory_order_relaxed));
        if(m_rtGiPass) m_rtGiPass->SetBudgetStage(m_restirBudgetStage.load(std::memory_order_relaxed));
        if(m_rtGiShadowPass) m_rtGiShadowPass->SetBudgetStage(m_restirBudgetStage.load(std::memory_order_relaxed));

        static const int s_restirMemoryEvery=Sys::CommandLine::Get().GetInt("-restir_memory_log_every",0);
        static uint64_t s_restirMemoryFrame=0;
        if(s_restirMemoryEvery>0 && ++s_restirMemoryFrame%s_restirMemoryEvery==0)
        {
            uint64_t used=0,budget=0;
            if(m_gfx.QueryVramUsage(used,budget))
                LOG_INFO("[RESTIR_MEMORY] frame=%llu usedBytes=%llu budgetBytes=%llu arenaBytes=%llu arenaEpoch=%u requested=%s effective=%s",
                    static_cast<unsigned long long>(s_restirMemoryFrame),static_cast<unsigned long long>(used),
                    static_cast<unsigned long long>(budget),static_cast<unsigned long long>(m_transientAliasArena.Capacity()),
                    m_transientAliasArena.Epoch(),RenderPathName(GetRenderPathRequested()),RenderPathName(GetRenderPath()));
        }

        // Read resolved timestamps on the render thread (never the game thread).
        // These are older GPU frames; expose the age rather than attributing them
        // to this frame's update counts. Opt-in so normal rendering pays no scan.
        static const int s_spotGpuLogEvery = Sys::CommandLine::Get().GetInt("-spot_shadow_log_every", 0);
        static uint64_t s_spotGpuLogFrame = 0;
        if (s_spotGpuLogEvery > 0 && ++s_spotGpuLogFrame % s_spotGpuLogEvery == 0) {
            RHI::GpuTimingView profiler;
            m_gfx.GetGPUTimings(profiler);
            if (profiler.enabled && profiler.hasResults) {
                float spotMs = 0.0f, cullMs = 0.0f;
                bool foundSpot = false;
                for (uint32_t i = 0; i < profiler.resultCount; ++i) {
                    const auto& r = profiler.results[i];
                    if (!r.name) continue;
                    if (std::strcmp(r.name, "SpotShadowPass") == 0) { spotMs += r.gpuMs; foundSpot = true; }
                    if (std::strcmp(r.name, "ShadowCull") == 0) cullMs += r.gpuMs;
                }
                if (foundSpot)
                    LOG_INFO("[SPOTGPU] frame=%llu shadowMs=%.4f familyCullMs=%.4f age=%u",
                        static_cast<unsigned long long>(s_spotGpuLogFrame), spotMs, cullMs, profiler.resultsAgeFrames);
            }
        }
    }

    // The render half's Streamline token, latched ONCE for this function and
    // everything it kicks. Read here rather than carried from UploadFrameData
    // because that runs on the other half of the seam; see the member's comment.
    if (primary) m_slFrameToken = SL::StreamlineBackend::Get().RenderFrameToken();

    // Per-phase CPU times of this function, reported as [RENDERZONE] under
    // -phaselog_every. Phases are timestamped (_rzMark), not scoped: locals cross
    // every phase boundary, so wrapping phases in braces would change their
    // lifetime.
    static double s_rz[kRenderZoneCount] = {};
    static int    s_rzN = 0;
#ifdef TRACY_ENABLE
    // Sequential RAII zones preserve the lifetime of locals crossing phase boundaries.
    static const tracy::SourceLocationData kRenderStages[] = {
        { "RenderCPU.BindFrame", __FUNCTION__, __FILE__, __LINE__, 0 },
        { "RenderCPU.InstanceFlush", __FUNCTION__, __FILE__, __LINE__, 0 },
        { "RenderCPU.MaterialFlush", __FUNCTION__, __FILE__, __LINE__, 0 },
        { "RenderCPU.ComputePre", __FUNCTION__, __FILE__, __LINE__, 0 },
        { "RenderCPU.ShadowSetup", __FUNCTION__, __FILE__, __LINE__, 0 },
        { "RenderCPU.GPUCull", __FUNCTION__, __FILE__, __LINE__, 0 },
        { "RenderCPU.DDGI", __FUNCTION__, __FILE__, __LINE__, 0 },
        { "RenderCPU.ReflectSetup", __FUNCTION__, __FILE__, __LINE__, 0 },
        { "RenderCPU.ColorPasses", __FUNCTION__, __FILE__, __LINE__, 0 },
        { "RenderCPU.ShadowJoin", __FUNCTION__, __FILE__, __LINE__, 0 },
        { "RenderCPU.HiZ", __FUNCTION__, __FILE__, __LINE__, 0 },
        { "RenderCPU.PostTail", __FUNCTION__, __FILE__, __LINE__, 0 },
    };
    std::optional<tracy::ScopedZone> renderStage;
    renderStage.emplace(&kRenderStages[0], true);
#endif
    // The engine profiler's copy of the same stages - but ONLY the ones that
    // finish before the first co_await (stage 7 ends at _rzMark(7), right
    // before m_graph.ExecuteAsync is awaited). Stages 8.. span suspensions:
    // their Tracy zones read as wall periods, and the engine profiler's
    // coroutine rule would drop them, so the chain is closed instead and the
    // render half after the await is covered by the OwnerTask resume segments.
    static Diag::Prof::NameSlot s_profStages[] = {
        Diag::Prof::NameSlot{ "RenderCPU.BindFrame" },
        Diag::Prof::NameSlot{ "RenderCPU.InstanceFlush" },
        Diag::Prof::NameSlot{ "RenderCPU.MaterialFlush" },
        Diag::Prof::NameSlot{ "RenderCPU.ComputePre" },
        Diag::Prof::NameSlot{ "RenderCPU.ShadowSetup" },
        Diag::Prof::NameSlot{ "RenderCPU.GPUCull" },
        Diag::Prof::NameSlot{ "RenderCPU.DDGI" },
        Diag::Prof::NameSlot{ "RenderCPU.ReflectSetup" },
    };
    constexpr int kProfStages = static_cast<int>(sizeof(s_profStages) / sizeof(s_profStages[0]));
    Diag::Prof::StageChain profStage;
    profStage.Next(s_profStages[0]);
    auto _rzT = std::chrono::steady_clock::now();
    auto _rzMark = [&](int i) {   // s_rz is static: no capture needed
        const auto n = std::chrono::steady_clock::now();
        s_rz[i] += std::chrono::duration<double, std::milli>(n - _rzT).count();
        _rzT = n;
#ifdef TRACY_ENABLE
        renderStage.reset();
        renderStage.emplace(&kRenderStages[i + 1], true);
#endif
        if (i + 1 < kProfStages) profStage.Next(s_profStages[i + 1]);
        else                     profStage.Close();
    };

    // Report from a scope-exit guard, not from the bottom of the function: the
    // common path leaves through an early return, so a tail report would never run.
    struct RzReport
    {
        std::chrono::steady_clock::time_point& t;
        bool                                   primary;
        ~RzReport()
        {
            if (!primary) return;
            s_rz[kRenderZoneCount - 1] +=
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t).count();
            static const int s_every = []{
                const auto& cl = Sys::CommandLine::Get();
                return cl.Has("-phaselog_every") ? cl.GetInt("-phaselog_every", 60) : 0;
            }();
            if (s_every <= 0 || ++s_rzN % s_every != 0) return;
            double tot = 0.0;
            for (double v : s_rz) tot += v;
            LOG_INFO("[RENDERZONE] per frame (total %.2f ms): bindFrame %.2f instFlush %.2f "
                     "matFlush %.2f computePre %.2f shadowCL %.2f gpuCull %.2f ddgi %.2f "
                     "reflect %.2f colorPasses %.2f waitWorker0 %.2f hiZ %.2f post+tail %.2f",
                     tot / s_every,
                     s_rz[0]/s_every, s_rz[1]/s_every, s_rz[2]/s_every,  s_rz[3]/s_every,
                     s_rz[4]/s_every, s_rz[5]/s_every, s_rz[6]/s_every,  s_rz[7]/s_every,
                     s_rz[8]/s_every, s_rz[9]/s_every, s_rz[10]/s_every, s_rz[11]/s_every);
            for (double& v : s_rz) v = 0.0;
        }
    } _rzReport{ _rzT, primary };

    EnsureWorkers();
    // Drain borrowed recording work before Render unwinds on an owner failure.
    // Normal joins below still report job exceptions at the consuming boundary.
    struct RecordingDrain
    {
        RenderWorker* jobs;
        ~RecordingDrain() { for (int i = 0; i < 3; ++i) jobs[i].Shutdown(); }
    } recordingDrain{m_renderWorkers};

    // ---- Frame bindings (read-only for all workers this frame) ----
    const uint32_t frameSlot = m_gfx.GetFrameIndex();
    if (primary) Render_BindFrameResources(frameSlot);
    else         Render_BindViewResources(viewSlot, frameSlot);

    // The frame plan (Renderer/FramePlan.h): the state it reads, once, and its
    // stage A. After the frame binds: the state counts this frame's draws, which
    // the path tracer's readers need. The decision sites below read m_plan.
    if (primary)
    {
        CaptureFramePlanState();
        m_planFacts = {};
        PlanStage(FramePlan::Stage::A);
    }

    _rzMark(0);

    // ---- The frame's queued texture uploads, on a list of their own - begun
    // first and depending on nothing, so EndFrame's submit (every dependency-free
    // list first, in pool order) runs the copies before every reader of this
    // frame: the graph's UI passes, the preview, the primary list's ImGui. Must
    // stay above every other BeginCommandList in RenderAsync; the device checks the
    // order each frame.
    if (primary) m_gfx.RecordPendingStreamUploads();

    // ---- Flush this frame's dirty instance records into the DEFAULT buffer.
    // This list is created before every instance-buffer consumer (shadow / cull /
    // graph / voxel / probe-bake lists) and has no dependencies, so EndFrame's
    // topological submit keeps pool order -> the copies execute first on the
    // graphics queue. Every consumer is graphics-queue (DDGI's async compute reads
    // DDGISceneAS's own buffer); a compute-queue consumer would need
    // AddCommandListDependency on this list and the DDGI fence sync, not pool order.
    if (primary && m_retainInstances && !InstCopyRead().empty() &&
        m_instanceBufferGPU.IsValid())
    {
        RHI::CommandList instCL = m_gfx.BeginCommandList(RHI::QueueType::GRAPHICS);
        m_gfx.SetCommandListName(instCL, "InstanceCopy");
        instCL.gfx = &m_gfx;

        uint32_t copiedRecords = 0;
        for (const InstanceCopyRange& r : InstCopyRead())
        {
            m_gfx.CopyBufferRegion(
                m_instanceBuffer[r.frameSlot],
                uint64_t(r.stagingFirst) * sizeof(GPUInstanceData),
                m_instanceBufferGPU,
                uint64_t(r.dstSlot) * sizeof(GPUInstanceData),
                uint64_t(r.count) * sizeof(GPUInstanceData),
                instCL);
            copiedRecords += r.count;
        }
        InstCopyRead().clear();

        // The buffer decayed to COMMON at the last ECL boundary and the copy
        // promoted it to COPY_DEST; move it to the SRV states the draw / cull / voxel
        // reads expect, in this same submission batch. Do not OR two states here:
        // SHADER_RESOURCE is the one enumerator that maps to NON_PIXEL|PIXEL, and the
        // translator (GraphicsDX12_Translation.cpp) switches on one value, so an OR
        // falls through to COMMON. The enclosing `if` already required IsValid().
        m_gfx.PushBarrier(RHI::GPUBarrier::Buffer(&m_instanceBufferGPU,
                              RHI::ResourceState::COPY_DST,
                              RHI::ResourceState::SHADER_RESOURCE),
                          instCL);

        if (copiedRecords > m_instUploadRecordsPeak)
        {
            m_instUploadRecordsPeak = copiedRecords;
            LOG_INFO("Renderer: instance dirty-upload high-water %u records (%.1f KB)",
                     copiedRecords,
                     copiedRecords * sizeof(GPUInstanceData) / 1024.0f);
        }
    }

    _rzMark(1);

    // ---- Flush this frame's dirty material slots into the DEFAULT buffer ----
    // Same contract and point in the frame as the instance copy: this list opens
    // before shadowCL (Phase 1), the cull, graph and probe-bake lists (all
    // graphics-queue), so pool order lands the copies before the first draw that
    // reads a slot. That makes a live editor edit show the same frame: a slider
    // drag changes matHash -> new key -> new slot -> needsBaseWrite -> a dirty range
    // recorded in BuildRenderScene and drained here, one list ahead of this
    // frame's GBuffer.
    // In-flight readers are safe: slot content is immutable per key (the key folds
    // in every render-affecting field), and a slot changes meaning only at
    // reassignment, which SweepPersistentMatSlots defers by kMatSlotReuseLagEpochs
    // (4 > 3 frames in flight). So a range recorded this frame rewrites identical
    // bytes or writes a slot no in-flight frame references.
    if (primary && MaterialGpuBufferActive() &&
        (MatUploadRead().fullInit || !MatUploadRead().ranges.empty()))
    {
        constexpr uint64_t kMatStride = sizeof(Resource::MaterialGPUData);

        RHI::CommandList matCL = m_gfx.BeginCommandList(RHI::QueueType::GRAPHICS);
        m_gfx.SetCommandListName(matCL, "MaterialCopy");
        matCL.gfx = &m_gfx;

        uint32_t copiedSlots = 0;
        if (MatUploadRead().fullInit)
        {
            // One-time seed (creation / explicit reset): the whole default-PBR
            // pre-filled staging table. Any pending ranges are a subset of it.
            const uint32_t src = m_materialBufferMapped[frameSlot] ? frameSlot : 0u;
            if (m_materialBuffer[src].IsValid())
            {
                m_gfx.CopyBufferRegion(m_materialBuffer[src], 0,
                                       m_materialBufferGPU, 0,
                                       uint64_t(m_maxMaterials) * kMatStride,
                                       matCL);
                copiedSlots = m_maxMaterials;
            }
            MatUploadRead().fullInit = false;
            MatUploadRead().ranges.clear();
        }
        else
        {
            for (const MaterialCopyRange& r : MatUploadRead().ranges)
            {
                m_gfx.CopyBufferRegion(
                    m_materialBuffer[r.frameSlot], uint64_t(r.slot) * kMatStride,
                    m_materialBufferGPU,           uint64_t(r.slot) * kMatStride,
                    uint64_t(r.count) * kMatStride,
                    matCL);
                copiedSlots += r.count;
            }
            MatUploadRead().ranges.clear();
        }

        // Decayed to COMMON, promoted to COPY_DEST by the copy: back to the SRV
        // states the GBuffer / Lighting / shadow reads need, in this same batch, under
        // the instance copy's single-enumerator rule. MaterialGpuBufferActive()
        // already implies m_materialBufferGPU.IsValid().
        m_gfx.PushBarrier(RHI::GPUBarrier::Buffer(&m_materialBufferGPU,
                              RHI::ResourceState::COPY_DST,
                              RHI::ResourceState::SHADER_RESOURCE),
                          matCL);

        if (copiedSlots > m_materialCopyRecordsPeak)
        {
            m_materialCopyRecordsPeak = copiedSlots;
            LOG_INFO("Renderer: material dirty-upload high-water %u slots (%.1f KB)",
                     copiedSlots, copiedSlots * float(kMatStride) / 1024.0f);
        }
    }

    // The material cost table the debug target materialCost reads - only on the
    // frames it runs, here for the material copy's reason (a list opened before
    // every list that reads it).
    if (primary) RecordDebugCostTable(frameSlot);

    // Do not create or release the RT screen passes' or fog's enable-gated working
    // sets here: ServiceEnableGatedResources() does that at the frame boundary,
    // because the disable path may WaitIdleAndReleaseDeferred and command lists
    // are already open by this point (CanHardSyncNow refuses it).

    // The first prepare point (RG::PassLifecycle::OnPrepare; the order is
    // Renderer::RegisterPrepareOrder's): XeGTAO's working set and the RT reflection
    // passes' transient-alias upkeep, on the main thread before any worker kicks -
    // these passes record inside the graph's parallel phase, where creating
    // textures (and descriptors) would race the other recorders.
    m_passes.Prepare(RG::PrepareStage::FrameEarly, PrepareContextNow());

    // Pre-capture SRV handles on main thread before workers kick (graph won't recompile mid-frame).
    const bool needPostCapture = m_taaPass || m_xegtaoPass;
    const RHI::Texture* taaDepthTex    = needPostCapture ? m_graph.GetPhysicalTexture(m_depthHandle)    : nullptr;
    const RHI::Texture* taaSurfaceTex  = m_taaPass       ? m_graph.GetPhysicalTexture(m_surfaceHandle)  : nullptr;
    // Velocity feeds both TAA and XeGTAO's temporal reprojection: capture it
    // when either exists, so XeGTAO gets real motion vectors with TAA off.
    const RHI::Texture* taaVelocityTex = needPostCapture  ? m_graph.GetPhysicalTexture(m_velocityHandle) : nullptr;
    const RHI::Texture* normalTex      = (m_xegtaoPass || m_nprStabilizePass || m_autoExposurePass)
                                      ? m_graph.GetPhysicalTexture(m_normalHandle) : nullptr;
    const uint64_t      depthSrv       = taaDepthTex    ? m_gfx.GetTextureSRVGpuHandle(*taaDepthTex)    : 0;
    const uint64_t      surfaceSrv     = taaSurfaceTex  ? m_gfx.GetTextureSRVGpuHandle(*taaSurfaceTex)  : 0;
    const uint64_t      velocitySrv    = taaVelocityTex ? m_gfx.GetTextureSRVGpuHandle(*taaVelocityTex) : 0;
    const uint64_t      normalSrv      = normalTex      ? m_gfx.GetTextureSRVGpuHandle(*normalTex)      : 0;
    const uint64_t      materialSrv    = m_materialBufferGPU.IsValid()
                                      ? m_gfx.GetBufferSRVGpuHandle(m_materialBufferGPU) : 0;
    // Stencil-plane view of the depth buffer: TAA uses it to find the pixels
    // OutlinePass tagged and weaken their history weight. 0 when the depth format
    // has no stencil - TAA then disables the feature.
    const uint64_t      stencilSrv     = taaDepthTex    ? m_gfx.GetTextureStencilSRVGpuHandle(*taaDepthTex) : 0;

    _rzMark(2);

    // ---- Compute prepass: skinning / particle / trail / tracer / beam CS. ----
    if (primary)
    {
        m_frameSkinCL = {};
        Render_ComputePrepass();
    }

    // ==== Async-compute kickoff ====
    // The DDGI / scene-AS compute chain (asCL -> ddgiCL) waits on demoteCL, and a
    // submit batch shares one fence value, so demoteCL is created here and the
    // batch closed right after it: compute starts ~0.15 ms into the frame, behind
    // only the skinning / particle prepass instead of the whole graphics prologue,
    // and overlaps the raster work.
    // Everything the gates read (the RT / SSAO toggles, m_viewMode, the DDGI
    // volumes BuildScene_UpdateDDGI allocated) is final by this point.
    // ddgiEnabled = false skips the whole DDGI / AS scope unless an RT screen
    // effect needs the TLAS (rtScreenWantsTlas); the scope then runs for the AS
    // build only and the per-volume probe loop skips the probe work. Keep the gate
    // exact: DDGI's cost is measured by toggling it.
    // One definition, shared with the game-side gather: the plan's stage A, which
    // the gather computes from the same frame intent (PlanGameStageA), so the
    // gather and the build cannot disagree about whether the frame has an
    // acceleration structure.
    const bool ddgiVolumesActive = m_plan.ddgi;
    const bool rtScreenWantsTlas = m_plan.tlas;

    RHI::CommandList demoteCL{};
    if (primary && (ddgiVolumesActive || rtScreenWantsTlas) && m_lastWorld)
    {
        // Tiny graphics-queue list that demotes the DDGI atlases and SH / probe-data
        // buffers from PIXEL|NON_PIXEL_SHADER_RESOURCE (the previous frame's promoteCL,
        // or CreateBuffer's initial state) to NON_PIXEL_SHADER_RESOURCE only, the one
        // SR state a compute list can transition from. Without it the first
        // compute-queue DDGI barrier (StateBefore NPSR-only) trips the debug layer: the
        // recorded state holds PSR (0x80), invalid on compute. ddgiCL waits on
        // demoteCL via fence.
        demoteCL = m_gfx.BeginCommandList(RHI::QueueType::GRAPHICS);
        m_gfx.SetCommandListName(demoteCL, "Demote");
        if (demoteCL.IsValid())
        {
            demoteCL.gfx = &m_gfx;
            uint32_t dr = m_gfx.BeginGPUTimestamp(demoteCL, "DDGI.Demote");
            m_ddgiMgr.DemoteForComputeQueue(m_gfx, demoteCL);
            m_gfx.EndGPUTimestamp(demoteCL, dr);

            // Close the batch here so demoteCL's fence value covers only the prepass,
            // not the whole prologue. This is the engine's only SetSubmitBarrier call: each
            // costs an extra ExecuteCommandLists + Signal, so splitting at every
            // cross-queue source measured worse.
            if (m_asyncComputeEarlyKickoff)
                m_gfx.SetSubmitBarrier(demoteCL);
        }
    }

    _rzMark(3);

    // ---- Phase 1: Open shadow CL and build its per-frame context ----
    // A secondary view draws its own cascades (its fit, BeginViewSegment) into the shared array when
    // the frame has a CSM; otherwise it records nothing here, the main view has.
    const bool viewCsm = !primary && m_shadowPass && m_plan.csm;
    RHI::CommandList shadowCL{};
    RG::RenderContext shadowCtx;
    if (primary || viewCsm)
    {
        shadowCL = m_gfx.BeginCommandList(RHI::QueueType::GRAPHICS);
        m_gfx.SetCommandListName(shadowCL, "Shadows");
        // A secondary view's cascades overwrite the shared array the main view's lighting reads.
        if (!primary && m_viewAfter.IsValid()) m_gfx.AddCommandListDependency(shadowCL, m_viewAfter);
        shadowCL.gfx = &m_gfx;

        shadowCtx.SetDimensions(m_gfx.GetRenderWidth(), m_gfx.GetRenderHeight());
        shadowCtx.SetDrawList(&DrawPacketsRead());
        shadowCtx.SetBuffer(RG::RGSlot::InstanceBuffer,  RetainedInstancesActive()
                                                             ? m_instanceBufferGPU
                                                             : m_instanceBuffer[frameSlot]);
        shadowCtx.SetBuffer(RG::RGSlot::MeshDescriptors, m_meshMgr.GetDescriptorHeap().CurrentMeshDescBuffer());
        // Material buffer + bindless texture table required by Shadow.ps ALPHA_TEST path.
        // DEFAULT-heap buffer: ShadowPass / SpotShadowPass / PointShadowPass /
        // PerObjectShadowPass all resolve this same context binding.
        shadowCtx.SetBuffer(RG::RGSlot::MaterialBuffer,  MaterialSRVBuffer(frameSlot));
        shadowCtx.SetBindlessTableHandle(m_meshMgr.GetDescriptorHeap().GetBufferTableGpuHandle());
        shadowCL.ctx = &shadowCtx;
    }

    // ---- Phase 1.5: shadow-family retained args ----
    // Recorded on the main thread into shadowCL before the worker kick: (a) the
    // per-cascade cull dispatches must precede the pass's ExecuteIndirect in the
    // same list - shadowCL is pooled (so submitted) before the Phase 2a.75 main
    // cull list, which therefore cannot serve these views; (b) CullingPass's
    // CB-pool cursor is not thread-safe against that later main-view dispatch. The
    // latch was armed in BuildRenderScene, so classify / meta / dispatch / pass
    // agree.
    // The same recording serves SpotShadowPass / PointShadowPass, which run in the
    // graph on lists opened after shadowCL, so same-queue pool order still puts
    // the dispatches first. The working buffers end this list in
    // INDIRECT_ARGUMENT / NON_PIXEL_SHADER_RESOURCE and, being buffers, decay to
    // COMMON at any ECL boundary and re-promote on first use: the graph lists need
    // no barrier for them.
    // false = the dispatches could not be recorded: the passes must not consume the
    // stale working buffers and fall back to their arg rings this frame, which are
    // always coherent (classify ran emit-once, so a ring sees a correct superset
    // of casters).
    const bool shadowCullRecorded =
        primary && SceneEmitRead().shadowArgsFrameActive && RecordShadowFamilyCull(shadowCL, frameSlot);

    if (primary && m_shadowPass)
    {
        ShadowPass::RetainedShadowArgs ra{};
        if (shadowCullRecorded)
        {
            ra.active      = true;
            ra.countBuffer = &m_shadowGroupCounts;
            for (uint32_t v = 0; v < kShadowCullViews; ++v)
            {
                ra.workingArgs[v]  = &m_shadowWorkingArgs[v];
                ra.remapHeapIdx[v] = m_shadowRemapHeapIdx[v];
            }
            for (int g = 0; g < ShadowDrawGroup::RetainedCount; ++g)
            {
                ra.groupBase[g] = SceneEmitRead().shadowGroupBase[g];
                ra.groupCap[g]  = SceneEmitRead().shadowGroupCap[g];
            }
            // The meshlet twins' DISPATCH_MESH commands and the per-cascade
            // MeshletCullCB banks RecordShadowFamilyCull wrote this frame.
            ra.meshlet      = SceneEmitRead().shadowMeshletFrameActive;
            ra.meshletBanks = ra.meshlet ? &m_meshletShadowCB.CurrentBuffer(m_gfx) : nullptr;
            ra.frame        = static_cast<uint32_t>(FrameViewRead().currentFrame);
        }
        m_shadowPass->SetRetainedShadowArgs(ra);
    }
    // A secondary view's casters go through the arg ring: the retained cull is the main view's cascades'.
    else if (viewCsm)
        m_shadowPass->SetRetainedShadowArgs(ShadowPass::RetainedShadowArgs{});

    if (primary && m_spotShadowPass)
    {
        SpotShadowPass::RetainedShadowArgs ra{};
        // m_spotCullCount mirrors SetActiveCasterCount (same loop, same index);
        // slices past it got no dispatch, so their entries stay null and the pass
        // draws nothing there, as activeCount also does.
        if (shadowCullRecorded)
        {
            ra.active      = true;
            ra.countBuffer = &m_shadowGroupCounts;
            for (uint32_t s = 0; s < SceneEmitRead().spotCullCount; ++s)
            {
                const uint32_t view = kSpotCullViewBase + s;
                ra.workingArgs [s] = &m_shadowWorkingArgs[view];
                ra.remapHeapIdx[s] = m_shadowRemapHeapIdx[view];
                ra.countBase   [s] = view * ShadowDrawGroup::RetainedCount;
            }
            for (int g = 0; g < ShadowDrawGroup::RetainedCount; ++g)
            {
                ra.groupBase[g] = SceneEmitRead().shadowGroupBase[g];
                ra.groupCap[g]  = SceneEmitRead().shadowGroupCap[g];
            }
        }
        m_spotShadowPass->SetRetainedShadowArgs(ra);
    }

    if (primary && m_pointShadowPass)
    {
        PointShadowPass::RetainedShadowArgs ra{};
        // Indexed per cube: the pass maps face -> cube (face / kFacesPerCube).
        if (shadowCullRecorded)
        {
            ra.active      = true;
            ra.countBuffer = &m_shadowGroupCounts;
            for (uint32_t c = 0; c < SceneEmitRead().pointCullCount; ++c)
            {
                const uint32_t view = kPointCullViewBase + c;
                ra.workingArgs [c] = &m_shadowWorkingArgs[view];
                ra.remapHeapIdx[c] = m_shadowRemapHeapIdx[view];
                ra.countBase   [c] = view * ShadowDrawGroup::RetainedCount;
            }
            for (int g = 0; g < ShadowDrawGroup::RetainedCount; ++g)
            {
                ra.groupBase[g] = SceneEmitRead().shadowGroupBase[g];
                ra.groupCap[g]  = SceneEmitRead().shadowGroupCap[g];
            }
        }
        m_pointShadowPass->SetRetainedShadowArgs(ra);
    }

    // ---- Phase 2a: Kick Worker 0 (shadow) ----
    // The CSM unless the path tracer owns the frame and nothing reads it
    // (FramePlan::Decisions::csm) - skipped the way ShadowPass skips a frame
    // without casters: the array keeps what it was left with. One that no frame
    // has written yet is cleared to "no occluder" instead, for a veto frame's
    // raster fallback (FramePlan.h).
    const bool csmActive = m_plan.csm;
    if (primary || viewCsm) m_renderWorkers[0].Kick([this, shadowCL, csmActive]() mutable
    {
        if (m_shadowPass && csmActive)
        {
            uint32_t r = m_gfx.BeginGPUTimestamp(shadowCL, "ShadowPass");
            m_shadowPass->Execute(shadowCL);
            m_gfx.EndGPUTimestamp(shadowCL, r);
        }
        else if (m_shadowPass)
        {
            m_shadowPass->PrepareUnwritten(shadowCL);
        }
    });

    // ---- Phase 2a.5: Cluster lighting CS - runs when lights exist or DecalPass
    // needs AABBs. Declared outside the if so later phases see the handle. No DDGI
    // dependency: the GPULight buffer DDGI's trace samples is CPU-written into an
    // UPLOAD ring by ClusterPass::SetLights, not produced by this list (see the
    // (void)clusterCL at ddgiCL's dependencies).
    RHI::CommandList clusterCL{};
    // Every view: a secondary view culls the lights into its own grid (ClusterPass is bound to it).
    if (m_clusterPass)
    {
        clusterCL = m_gfx.BeginCommandList(RHI::QueueType::GRAPHICS);
        m_gfx.SetCommandListName(clusterCL, "ClusterPass");
        clusterCL.gfx = &m_gfx;
        uint32_t rc = m_gfx.BeginGPUTimestamp(clusterCL, "ClusterPass");
        m_clusterPass->Execute(clusterCL);

        // Piggyback decal cluster cull on this CL; apply CS in graph reads via CL chain (the bound
        // view's grid).
        if (m_decalPass)
            m_decalPass->DispatchCull(clusterCL);

        m_gfx.EndGPUTimestamp(clusterCL, rc);
    }

    _rzMark(4);

    // ---- Phase 2a.75: GPU indirect-command culling (frustum + Hi-Z) ----
    // Cull mode: copies this frame's CPU-written args into the DEFAULT-heap buffer,
    // InstanceCull.cs zeroes the commands whose every instance fails the frustum /
    // Hi-Z test, and GBufferPass draws from that buffer. Without GPU culling it
    // draws the raw upload args.
    // Compaction mode (gpuCompactionFrameActive, latched by SortAndEmit): the
    // upload args stay pristine (the CS reads them as an SRV and the GPU never
    // writes them, so no cross-frame sticky cull); survivors are compacted per
    // instance into the working remap's upper half and per group into the working
    // arg buffer, with per-group counts.
    // cullCL is a graphics-queue list created before the color graph's lists, so
    // same-queue submission order makes the args final before any ExecuteIndirect
    // consumes them (the invariant the demote / skin lists rely on too).
    const uint32_t cullCmdCount =
        std::min<uint32_t>(SceneEmitRead().indirectDrawCount, m_maxInstances);
    const bool gpuCullActive =
        primary && m_gpuCullingEnabled && m_cullingPass && m_indirectArgBuffer.IsValid() &&
        m_useIndirectDraw && !SceneEmitRead().indirectGroups.empty() && cullCmdCount > 0 &&
        m_indirectArgMapped[frameSlot];
    // Compaction latch, set by this frame's SortAndEmit: implies retained
    // instances and every compaction resource valid at build time. The per-view CB
    // already published the working remap's heap index for it.
    const bool gpuCompactFrame = primary && SceneEmitRead().gpuCompactionFrameActive;
    // Entries of this frame's pristine mapping (nonzero whenever a packet was
    // emitted in retained mode: commands imply instances). Guarded so the dispatch
    // decision and the GBufferPass binding cannot disagree.
    const uint32_t compactRemapEntries = gpuCompactFrame
        ? static_cast<uint32_t>((std::min)(InstanceRemapRead().size(),
                                           static_cast<size_t>(m_maxInstances)))
        : 0u;
    const bool gpuCompactDispatch =
        gpuCompactFrame && gpuCullActive && compactRemapEntries > 0;

    // GBufferPass indirect-draw state; ExecuteIndirect requires bindless textures.
    if (m_gbufferPass)
    {
        if (primary && m_useIndirectDraw && m_indirectArgBuffer.IsValid() && !SceneEmitRead().indirectGroups.empty())
        {
            if (gpuCompactDispatch)
                // Compaction: working args + per-group GPU counts (count = survivors).
                m_gbufferPass->SetIndirectDraw(&m_indirectArgBuffer,
                                               SceneEmitRead().indirectGroups,
                                               &m_drawCountBuffer);
            else if (gpuCompactFrame)
                // Latched pristine args carry FULL instanceCounts (the
                // wantsMain/viewMask predicates live in the CS this frame);
                // without the cull dispatch they must never reach
                // ExecuteIndirect. The CPU per-draw path checks the
                // predicates itself: correct, one frame of slower draws.
                m_gbufferPass->ClearIndirectDraw();
            else
                m_gbufferPass->SetIndirectDraw(
                    gpuCullActive ? &m_indirectArgBuffer : &m_indirectArgUpload[frameSlot],
                    SceneEmitRead().indirectGroups);
        }
        else
            m_gbufferPass->ClearIndirectDraw();
    }

    if (gpuCullActive || gpuCompactFrame)
    {
        RHI::CommandList cullCL = m_gfx.BeginCommandList(RHI::QueueType::GRAPHICS);
        m_gfx.SetCommandListName(cullCL, "GpuCulling");
        cullCL.gfx = &m_gfx;
        uint32_t cullRegion = m_gfx.BeginGPUTimestamp(cullCL, "GpuCulling");

        float frustumData[6][4];
        for (int p = 0; p < 6; ++p)
        {
            frustumData[p][0] = FrameViewRead().view.frustum[p].normal.x;
            frustumData[p][1] = FrameViewRead().view.frustum[p].normal.y;
            frustumData[p][2] = FrameViewRead().view.frustum[p].normal.z;
            frustumData[p][3] = FrameViewRead().view.frustum[p].distance;
        }
        m_cullingPass->SetFrustumPlanes(frustumData);
        // Hi-Z's screen projection must use the viewProj captured with the pyramid
        // (last frame, m_mainView.hiz.viewProj): the current one mis-taps by a frame
        // of camera motion. The frustum planes stay current; only the occlusion
        // projection pairs with the pyramid. Without a capture the current VP is
        // passed (hiZUsable is false then and the value never read).
        m_cullingPass->SetViewProj(m_mainView.hiz.framesValid > 0
                                       ? m_mainView.hiz.viewProj
                                       : FrameViewRead().view.viewProjMatrixNoJitter);

        // Hi-Z is last frame's pyramid - HiZUsableThisFrame is the gate (the
        // meshlet amplification shader reads the same answer).
        const bool hiZUsable = HiZUsableThisFrame();

        if (gpuCompactFrame)
        {
            // ---- Compaction: per-instance cull ----
            // The per-view CB published m_instanceRemapGPU's heap index this frame, so its
            // lower half must hold this frame's pristine mapping even when the dispatch is
            // skipped (empty groups): the full-count main-view draws (picking /
            // transparent / outline) resolve through it unconditionally.
            if (compactRemapEntries > 0)
                m_gfx.CopyBufferRegion(
                    m_instanceRemapBuffer[frameSlot], 0,
                    m_instanceRemapGPU, 0,
                    uint64_t(compactRemapEntries) * sizeof(uint32_t), cullCL);

            // Dispatch only when the indirect path consumes commands this frame:
            // gpuCompactDispatch false -> GBufferPass took the CPU per-draw fallback above
            // -> nothing reads the working args / counts.
            if (gpuCompactDispatch)
            {
                // Zero this frame's per-group counts (upload twin is memset 0
                // once at creation and never written after).
                const uint32_t groupCount =
                    static_cast<uint32_t>(SceneEmitRead().indirectGroups.size());
                m_gfx.CopyBufferRegion(
                    m_drawCountUpload[frameSlot], 0,
                    m_drawCountBuffer, 0,
                    uint64_t(groupCount) * sizeof(uint32_t), cullCL);

                // m_indirectArgBuffer.IsValid() is implied by gpuCullActive but not by
                // gpuCompactFrame alone, hence the explicit check. One condition decides the
                // counter zeroing, the UAV transition and passing the buffer to the dispatch:
                // if the zero-copy ran under another predicate than the transition, the CS
                // would write u3 while still COPY_DST, a buffer state mismatch the debug layer
                // does not reliably catch.
                const bool visStatsThisFrame =
                    m_visStatsEnabled && m_visStatsBuffer.IsValid() &&
                    m_visStatsZeroUpload.IsValid() &&
                    m_instanceRemapGPU.IsValid() && m_drawCountBuffer.IsValid() &&
                    m_indirectArgBuffer.IsValid();

                if (visStatsThisFrame)
                    m_gfx.CopyBufferRegion(m_visStatsZeroUpload, 0, m_visStatsBuffer, 0,
                                           kVisStatsElems * sizeof(uint32_t), cullCL);

                if (m_instanceRemapGPU.IsValid() && m_drawCountBuffer.IsValid() &&
                    m_indirectArgBuffer.IsValid())
                {
                    // Copies promoted remap/counts to COPY_DEST; the working
                    // arg buffer decayed to COMMON (nothing copies into it in
                    // this mode). Explicit hops keep the debug layer exact.
                    RHI::GPUBarrier pre[4] = {
                        RHI::GPUBarrier::Buffer(&m_instanceRemapGPU,
                            RHI::ResourceState::COPY_DST, RHI::ResourceState::UNORDERED_ACCESS),
                        RHI::GPUBarrier::Buffer(&m_drawCountBuffer,
                            RHI::ResourceState::COPY_DST, RHI::ResourceState::UNORDERED_ACCESS),
                        RHI::GPUBarrier::Buffer(&m_indirectArgBuffer,
                            RHI::ResourceState::UNDEFINED, RHI::ResourceState::UNORDERED_ACCESS),
                        {},
                    };
                    uint32_t preCount = 3;
                    if (visStatsThisFrame)
                        pre[preCount++] = RHI::GPUBarrier::Buffer(&m_visStatsBuffer,
                            RHI::ResourceState::COPY_DST, RHI::ResourceState::UNORDERED_ACCESS);
                    m_gfx.PushBarriers(pre, preCount, cullCL);
                }

                m_cullingPass->ExecuteCompact(cullCL,
                    m_instanceBufferGPU,   // latch implies retained instances
                    m_meshMgr.GetDescriptorHeap().CurrentMeshAABBBuffer(),
                    m_indirectArgBuffer,   // working args (write-only)
                    cullCmdCount,
                    hiZUsable ? m_hiZPass->GetHiZSrvHandle() : 0,
                    hiZUsable ? m_hiZPass->GetHiZValidWidth()  : 0,
                    hiZUsable ? m_hiZPass->GetHiZValidHeight() : 0,
                    hiZUsable ? m_hiZPass->GetHiZMipCount()  : 0,
                    m_gfx.GetBufferSRVGpuHandle(m_instanceRemapBuffer[frameSlot]),
                    m_gfx.GetBufferSRVGpuHandle(m_indirectArgUpload[frameSlot]),
                    m_gfx.GetBufferSRVGpuHandle(m_cmdGroupMeta[frameSlot]),
                    m_instanceRemapGPU,
                    m_drawCountBuffer,
                    m_maxInstances,         // upper-half base of the working remap
                    // nullptr when anything above failed: the CB flag then
                    // leaves u3 unbound AND unwritten, so the two can never
                    // disagree.
                    visStatsThisFrame ? &m_visStatsBuffer : nullptr);

                if (m_instanceRemapGPU.IsValid() && m_drawCountBuffer.IsValid() &&
                    m_indirectArgBuffer.IsValid())
                {
                    // Keep the UAV flush its own push: merged with the transitions into one
                    // ResourceBarrier call, D3D12 leaves their relative order unspecified.
                    RHI::GPUBarrier uavDone[4] = {
                        RHI::GPUBarrier::Memory(&m_indirectArgBuffer),
                        RHI::GPUBarrier::Memory(&m_instanceRemapGPU),
                        RHI::GPUBarrier::Memory(&m_drawCountBuffer),
                        {},
                    };
                    uint32_t uavCount = 3;
                    if (visStatsThisFrame)
                        uavDone[uavCount++] = RHI::GPUBarrier::Memory(&m_visStatsBuffer);
                    m_gfx.PushBarriers(uavDone, uavCount, cullCL);

                    // Args + counts feed ExecuteIndirect; the working remap is
                    // read by the VSes via ResourceDescriptorHeap (non-pixel).
                    const RHI::GPUBarrier post[3] = {
                        RHI::GPUBarrier::Buffer(&m_indirectArgBuffer,
                            RHI::ResourceState::UNORDERED_ACCESS, RHI::ResourceState::INDIRECT_ARGUMENT),
                        RHI::GPUBarrier::Buffer(&m_drawCountBuffer,
                            RHI::ResourceState::UNORDERED_ACCESS, RHI::ResourceState::INDIRECT_ARGUMENT),
                        RHI::GPUBarrier::Buffer(&m_instanceRemapGPU,
                            RHI::ResourceState::UNORDERED_ACCESS, RHI::ResourceState::SHADER_RESOURCE_COMPUTE),
                    };
                    m_gfx.PushBarriers(post, 3, cullCL);

                    // Post-cull survivor count -> readback ring. Appended here, not folded into
                    // the barrier arrays above: cullCL is the renderer's most state-sensitive
                    // list, so a diagnostic must not reshuffle the transitions the draws depend
                    // on. Costs one <=16 KB copy and two buffer barriers a frame.
                    if (m_visibleDrawCmdsEnabled && m_drawCountReadback[frameSlot].IsValid())
                    {
                        const uint32_t copyCount =
                            (groupCount < kDrawCountReadbackCap) ? groupCount
                                                                 : kDrawCountReadbackCap;
                        m_gfx.PushBarrier(RHI::GPUBarrier::Buffer(&m_drawCountBuffer,
                            RHI::ResourceState::INDIRECT_ARGUMENT,
                            RHI::ResourceState::COPY_SRC), cullCL);
                        m_gfx.CopyBufferRegion(
                            m_drawCountBuffer, 0,
                            m_drawCountReadback[frameSlot], 0,
                            uint64_t(copyCount) * sizeof(uint32_t), cullCL);
                        m_gfx.PushBarrier(RHI::GPUBarrier::Buffer(&m_drawCountBuffer,
                            RHI::ResourceState::COPY_SRC,
                            RHI::ResourceState::INDIRECT_ARGUMENT), cullCL);

                        m_drawCountReadbackGroups[frameSlot] = copyCount;
                        m_drawCountReadbackFrame [frameSlot] = m_frameCounterForReadback.load(std::memory_order_relaxed) + 1;
                    }

                    // Visible-geometry counters -> their own readback ring, kept apart from the
                    // block above: the two rings have different enable conditions and frame
                    // counters, and one of them exists in a shipped build. 16 bytes; the buffer is
                    // never an INDIRECT_ARGUMENT, so it hops UAV -> COPY_SRC and back.
                    if (visStatsThisFrame && m_visStatsReadback[frameSlot].IsValid())
                    {
                        m_gfx.PushBarrier(RHI::GPUBarrier::Buffer(&m_visStatsBuffer,
                            RHI::ResourceState::UNORDERED_ACCESS,
                            RHI::ResourceState::COPY_SRC), cullCL);
                        m_gfx.CopyBufferRegion(
                            m_visStatsBuffer, 0,
                            m_visStatsReadback[frameSlot], 0,
                            kVisStatsElems * sizeof(uint32_t), cullCL);
                        // Back to COPY_DST: next frame's first touch is the
                        // zero-copy, and the pre-barrier above expects it to
                        // start there.
                        m_gfx.PushBarrier(RHI::GPUBarrier::Buffer(&m_visStatsBuffer,
                            RHI::ResourceState::COPY_SRC,
                            RHI::ResourceState::COPY_DST), cullCL);

                        m_visStatsReadbackFrame[frameSlot] = m_visStatsFrameCounter.load(std::memory_order_relaxed) + 1;
                    }
                }
            }
            else if (compactRemapEntries > 0 && m_instanceRemapGPU.IsValid())
            {
                // Copy-only frame: make the pristine lower half visible to the
                // VS reads (no CS ran, upper half never referenced).
                m_gfx.PushBarrier(RHI::GPUBarrier::Buffer(&m_instanceRemapGPU,
                                      RHI::ResourceState::COPY_DST,
                                      RHI::ResourceState::SHADER_RESOURCE_COMPUTE),
                                  cullCL);
            }
        }
        else
        {
            // ---- Cull mode: copy + in-place zero-out ----
            // Upload args -> DEFAULT. The DEFAULT buffer decayed to COMMON at the last ECL
            // boundary, so the copy promotes it to COPY_DEST; the UAV hop below needs an
            // explicit transition.
            m_gfx.CopyBuffer(m_indirectArgUpload[frameSlot], m_indirectArgBuffer,
                             uint64_t(cullCmdCount) * sizeof(IndirectDrawCommand), cullCL);

            if (m_indirectArgBuffer.IsValid())
            {
                m_gfx.PushBarrier(RHI::GPUBarrier::Buffer(&m_indirectArgBuffer,
                                      RHI::ResourceState::COPY_DST,
                                      RHI::ResourceState::UNORDERED_ACCESS),
                                  cullCL);
            }

            // Under retained instances the records live at stable slots in the DEFAULT
            // buffer; the CS resolves each command's draw-local range through this frame's
            // remap SRV (t7 space2, remapEnabled in its CB).
            const bool cullRetained = RetainedInstancesActive();
            m_cullingPass->Execute(cullCL,
                cullRetained ? m_instanceBufferGPU : m_instanceBuffer[frameSlot],
                m_meshMgr.GetDescriptorHeap().CurrentMeshAABBBuffer(),
                m_indirectArgBuffer,
                cullCmdCount,
                hiZUsable ? m_hiZPass->GetHiZSrvHandle() : 0,
                hiZUsable ? m_hiZPass->GetHiZValidWidth()  : 0,
                hiZUsable ? m_hiZPass->GetHiZValidHeight() : 0,
                hiZUsable ? m_hiZPass->GetHiZMipCount()  : 0,
                cullRetained ? m_gfx.GetBufferSRVGpuHandle(m_instanceRemapBuffer[frameSlot])
                             : 0);

            if (m_indirectArgBuffer.IsValid())
            {
                // Make the zero-out writes visible, then move to the state the
                // per-group ExecuteIndirect consumes. (The ECL-boundary decay to
                // COMMON would also legalize the next access; the explicit pair
                // keeps the debug layer's view exact.)
                m_gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_indirectArgBuffer), cullCL);
                m_gfx.PushBarrier(RHI::GPUBarrier::Buffer(&m_indirectArgBuffer,
                                      RHI::ResourceState::UNORDERED_ACCESS,
                                      RHI::ResourceState::INDIRECT_ARGUMENT),
                                  cullCL);
            }
        }

        m_gfx.EndGPUTimestamp(cullCL, cullRegion);
    }

    _rzMark(5);

    // ---- Phase 2b.5: DDGI (TLAS + trace + relight) - a dependency of the color
    // graph; atlases SRV-ready.
    RHI::CommandList ddgiCL{};
    // Function scope so a consumer outside the DDGI block could depend on the
    // TLAS build directly. None does (pointing RTShadow / RTAO at it bought no
    // overlap), but the ENGINE_CHECK_ONCE below still guards the invariant such a
    // dependency needs.
    RHI::CommandList asCL{};
    // ddgiLogTick is a debug switch, off: switched on, it ticks every 60th frame
    // of s_ddgiLogCounter (~1 Hz) to log which DDGI gate failed.
    static uint32_t s_ddgiLogCounter = 0;
    const bool ddgiLogTick = false;

    uint32_t ddgiRegion = ~0u;
    // RTReflectionPass inputs exported from the DDGI scope below (consumed by
    // the Phase 2b.7 wiring block). Zero = no TLAS this frame = RTR inactive.
    uint64_t rtrTlasVA = 0, rtrInstanceVA = 0;
    // Per-instance alpha-cutout table (RTAlphaRecord, 40 B) and how many entries
    // were written this frame, shared by RTR / RTShadow / RTAO.
    uint64_t rtrAlphaVA = 0;
    uint32_t rtrAlphaCount = 0;
    uint64_t rtrBindlessTable = 0, rtrLightsSrv = 0;
    // The scene-AS build (DDGISceneAS::BuildOrRefit) does not depend on DDGI
    // volumes; only the per-volume probe work does. With RT reflections on in a
    // DDGI-less scene this scope still runs: demote / promote record no barriers
    // (no allocated slots), but promoteCL still carries the compute -> graphics
    // fence the RTR trace waits on, and the per-volume DDGIPass loop skips itself
    // (GetProbeCount(s) == 0). RTReflectionPass::IsReady() implies DXR support.
    // The gate booleans and demoteCL come from the async-compute kickoff after
    // Render_ComputePrepass; do not recompute them here, or the two sites can
    // disagree.
    if (primary && (ddgiVolumesActive || rtScreenWantsTlas) && m_lastWorld)
    {
        // DDGI runs on the COMPUTE queue - the probe update is pure compute (CS with
        // inline RayQuery) - in parallel with the graphics-queue Shadow + GBuffer +
        // SkyIBL + Decal work. LightingPass reads the SH probe buffer + depth atlas via
        // SRV; the cross-queue handoff:
        //   0. demote -> DDGI fence (the demote graphics list puts the buffers in
        //      NPSR-only)
        //   1. AS -> DDGI edge (asCL builds the TLAS the trace walks; there is no
        //      cluster -> DDGI fence, see the (void)clusterCL below)
        //   2. DDGI -> promote fence (the promote graphics list flips the atlases
        //      NPSR -> NPSR|PSR)
        //   3. promote -> LightingPass (RenderGraph::SetExternalWait)
        // ---- Phase 2b.4b: scene acceleration structure, on its own compute list.
        // The AS build is not DDGI work - it serves DDGI, RTReflection, RTShadow and
        // RTAO - so it has its own "SceneAS" GPU region (plus the AS.* sub-regions
        // inside BuildOrRefit) instead of inflating the "DDGI" one.
        // It reads this frame's skinned vertex-ring slices, so it must run after
        // skinCL: that edge is declared explicitly below, not left to the demoteCL
        // edge (free when both land in the same submit batch, correct when they do
        // not). It does not need clusterCL (the AS build never touches the light
        // buffer).
        uint64_t tlasVA = 0;
        {
            asCL = m_gfx.BeginCommandList(RHI::QueueType::COMPUTE);
            m_gfx.SetCommandListName(asCL, "SceneAS");
            if (asCL.IsValid())
            {
                asCL.gfx = &m_gfx;
                if (demoteCL.IsValid())
                    m_gfx.AddCommandListDependency(asCL, demoteCL);
                if (m_frameSkinCL.IsValid())
                    m_gfx.AddCommandListDependency(asCL, m_frameSkinCL);
                uint32_t asRegion = m_gfx.BeginGPUTimestamp(asCL, "SceneAS");
                {
                    // Live tunable (EngineSettings "features.rtShadowBlendOccluders"), read
                    // by the next Gather (game side): it decides FORCE_NON_OPAQUE on alpha-blend
                    // instances, i.e. whether they enter non-opaque traversal at all. A toggle
                    // forces one full gather and, by changing the TLAS instance bytes, one
                    // rebuild.
                    m_ddgiSceneAS.SetBlendShadowOccluders(m_rtShadowBlendOccluders);
                    // Any active RT screen feature (RTR / RTAO / RT shadows) also pulls skinned
                    // meshes in as per-frame deformable BLAS (mask bits 0/1). That needs the
                    // ordering that guarantees the ring slices are written: demoteCL for the
                    // resource states and skinCL for the vertex data. Without both the compute
                    // queue could read ring slices before skinCL's writes land - better skip
                    // skinned for a frame than build garbage BLAS (a DXR traversal hang).
                    // No World: the gather ran on the game side in Renderer::BeginFrame
                    // (m_ddgiSceneAS.Gather) right after BuildRenderScene. allowSkinned filters
                    // what it gathered; it is decided here because demoteCL and m_frameSkinCL
                    // only exist here.
                    tlasVA = m_ddgiSceneAS.BuildOrRefit(
                        m_gfx, asCL,
                        // Same clock PerViewCB.timeParams.x feeds the vertex
                        // shader: the foliage-wind TLAS shear must land on the
                        // phase the leaves are drawn at.
                        /*timeSec=*/FrameViewRead().globalTimeSec,
                        /*allowSkinned=*/rtScreenWantsTlas && demoteCL.IsValid() &&
                                         m_frameSkinCL.IsValid());
                    // -dxr_parity traces one grid of camera rays through the
                    // DXR pipeline and through inline RayQuery against this
                    // TLAS, and compares the hits.
                    if (DXRParityTest::Wanted())
                        m_dxrParity.Tick(m_gfx, asCL, tlasVA, FrameViewRead().view);
                }
                m_gfx.EndGPUTimestamp(asCL, asRegion);
            }
        }

        ddgiCL = m_gfx.BeginCommandList(RHI::QueueType::COMPUTE);
        m_gfx.SetCommandListName(ddgiCL, "DDGI");
        if (ddgiCL.IsValid())
        {
            // Load-bearing for two reasons, do not remove it: the resource-state
            // handoff (demote puts the atlases in NPSR-only), and it makes ddgiCL a
            // cross-queue waiter, which forces a batch split and keeps asCL on its own
            // fence value. If asCL and ddgiCL merged into one compute batch, a consumer
            // waiting on asCL would silently wait on ddgiCL instead.
            if (demoteCL.IsValid())
                m_gfx.AddCommandListDependency(ddgiCL, demoteCL);
            // The trace reads the TLAS asCL just built.
            if (asCL.IsValid())
                m_gfx.AddCommandListDependency(ddgiCL, asCL);
            // Nothing waits on asCL alone (RTShadow / RTAO wait on promoteCL, see their
            // SetExternalWait), but a consumer pointed at asCL needs asCL and ddgiCL in
            // different submit batches, or "wait on asCL" silently becomes "wait on
            // ddgiCL" and reads as a hardware limit instead of a wiring bug. The demoteCL
            // edge guarantees the split by making ddgiCL a cross-queue waiter. Checked
            // because the failure is invisible; report only: a merged batch makes a waiter
            // wait longer, it does not unorder anything.
            ENGINE_CHECK_ONCE(!asCL.IsValid() || !ddgiCL.IsValid() || demoteCL.IsValid(),
                "asCL/ddgiCL would share a submit batch (no demoteCL) - a consumer "
                "waiting on asCL would silently wait on ddgiCL instead");
            // Not a dependency on clusterCL: the GPULight buffer DDGI's trace samples
            // (root slot 11) is an UPLOAD-heap, persistently mapped, frame-slot-ringed
            // buffer the CPU writes in ClusterPass::SetLights. ClusterPass's list never
            // touches it, so there is no GPU producer to wait for; BeginFrame's
            // frame-slot fence orders the write like every other per-frame upload. An
            // edge would make the compute queue wait on the graphics batch holding
            // ClusterPass, most of the prologue, for nothing.
            (void)clusterCL;
            ddgiRegion = m_gfx.BeginGPUTimestamp(ddgiCL, "DDGI");
            {
                if (ddgiLogTick)
                {
                    LOG_INFO("DDGI: gate-pass volumes=%u tlasVA=0x%llX skyRadSrv=%llu",
                             m_ddgiMgr.GetActiveVolumeCount(),
                             (unsigned long long)tlasVA,
                             (unsigned long long)m_skyRadianceSrvForDDGI);
                }
                if (tlasVA == 0 && ddgiLogTick)
                    LOG_WARNING("DDGI: TLAS build returned 0 — no static geometry visible to DDGI this frame (checked MeshLibRef + procedural-MeshHandle pools).");
                if (tlasVA != 0)
                {
                    // Bind the descriptor heap: the trace reads its SRVs/UAVs through it.
                    m_gfx.BindDescriptorHeaps(ddgiCL);

                    // GPU VA of g_DDGIInstances buffer (the hit shading reads albedo + bindless slots).
                    uint64_t matVA = 0;
                    if (const RHI::GPUBuffer* mb = m_ddgiSceneAS.GetInstanceBuffer(m_gfx))
                        matVA = m_gfx.GetBufferGPUAddress(*mb);

                    // Same bindless table as rasterizer; the hit shading reads vb/ibBindless for face normals.
                    const uint64_t bindlessTable =
                        m_meshMgr.GetDescriptorHeap().GetBufferTableGpuHandle();

                    // DDGIPass manages the atlas SRV <-> UAV transitions itself and leaves
                    // them SRV for downstream.
                    const uint64_t lightsSrv = m_clusterPass
                        ? m_clusterPass->GetLightsSRVHandle() : 0ull;

                    // Export for the RTReflectionPass wiring (Phase 2b.7): RTR
                    // reuses this frame's TLAS / instance data as is.
                    rtrTlasVA        = tlasVA;
                    rtrInstanceVA    = matVA;
                    rtrAlphaVA       = m_ddgiSceneAS.GetAlphaRecordVA(m_gfx);
                    rtrAlphaCount    = m_ddgiSceneAS.GetAlphaRecordCount();
                    rtrBindlessTable = bindlessTable;
                    rtrLightsSrv     = lightsSrv;

                    // Always hand the trace a valid sky cube: the real sky
                    // radiance, or the shared black fallback in None / FlatColor
                    // (0 -> the miss path would sample a stale / unbound
                    // descriptor and bake garbage into the probes).
                    // This list runs on the compute queue, concurrently with
                    // the graph that rewrites SkyIBL's specular cube on the
                    // graphics queue, with no edge between them. When that cube
                    // is the sky, the trace samples its async copy instead,
                    // which promoteCL (ordered after this list) refreshes
                    // below: DDGI's sky is a frame late.
                    const uint64_t liveSky = m_skyRadianceSrvForDDGI ? m_skyRadianceSrvForDDGI
                                                                     : m_fallbackSkyCubeSrv;
                    const uint64_t ddgiSky = m_skyIBLPass ? m_skyIBLPass->AsyncReaderSrv(liveSky) : liveSky;
                    static bool s_ddgiSkyCopyLogged = false;
                    if (ddgiSky != liveSky && !s_ddgiSkyCopyLogged)
                    {
                        s_ddgiSkyCopyLogged = true;
                        LOG_INFO("[DDGI_SKY] DDGI samples the specular cube's async copy (frame %llu)",
                                 static_cast<unsigned long long>(FrameViewRead().currentFrame));
                    }

                    // ddgiVolumesActive (the plan's stage A) also guards
                    // m_ddgiPass: the TLAS-only arm enters this scope with DDGI
                    // possibly never initialized. It carries ddgiEnabled too,
                    // so "DDGI off + RT on" pays no probe updates while the
                    // shared TLAS is still built. Do not gate the demote /
                    // promote lists on that flag: they are per-resource,
                    // flag-gated and must keep round-tripping, or the first
                    // re-enabled frame issues a barrier whose StateBefore no
                    // longer matches what was recorded.
                    for (uint32_t s = 0; ddgiVolumesActive && s < DDGI::kMaxVolumes; ++s)
                    {
                        if (m_ddgiMgr.GetProbeCount(s) == 0) continue;
                        // Camera relevance: a Dormant volume runs only on its
                        // heartbeat frame, so volumes far from the camera cost no
                        // GPU work. MarkVolumeExecuted below keeps the temporal
                        // interleave phase aligned with the executions.
                        if (!m_ddgiMgr.ShouldExecuteVolume(s)) continue;

                        m_ddgiPass->Execute(m_gfx, ddgiCL, m_ddgiMgr, s,
                                            tlasVA,
                                            ddgiSky,
                                            matVA,
                                            bindlessTable, lightsSrv,
                                            /*onComputeQueue=*/true);

                        // Tell the manager this slot's dispatches ran, so the
                        // next Tick advances its frame counter. The counter drives
                        // the temporal-interleave and INACTIVE-probe retest
                        // phases; both desync permanently if it advances on
                        // skipped frames (DDGIVolumeManager::MarkVolumeExecuted).
                        m_ddgiMgr.MarkVolumeExecuted(s);

                        // Leave the atlases in SRV state on the compute queue;
                        // promoteCL below (graphics queue) flips them NPSR-only ->
                        // NPSR|PSR so LightingPass's PS read sees the full SR mask.
                        m_ddgiMgr.TransitionVolumeAtlases(m_gfx, ddgiCL, s,
                            DDGI::DDGIVolumeManager::AtlasState::SRV,
                            /*onComputeQueue=*/true);
                    }
                }
            }
            m_gfx.EndGPUTimestamp(ddgiCL, ddgiRegion);
        }
    }

    // ---- Phase 2b.6: DDGI atlas promote (graphics queue).
    // DDGI ran on the COMPUTE queue, which leaves the SH probe buffer + depth
    // atlas in NON_PIXEL_SHADER_RESOURCE (the only SR state legal on compute).
    // LightingPass's PS read needs the full PIXEL|NON_PIXEL mask, which can
    // only be issued on a graphics CL. promoteCL waits on the DDGI fence
    // (cross-queue) and emits the per-volume state transitions; LightingPass
    // then waits on promoteCL via SetExternalWait below.
    RHI::CommandList promoteCL{};
    if (ddgiCL.IsValid())
    {
        promoteCL = m_gfx.BeginCommandList(RHI::QueueType::GRAPHICS);
        m_gfx.SetCommandListName(promoteCL, "Promote");
        if (promoteCL.IsValid())
        {
            promoteCL.gfx = &m_gfx;
            m_gfx.AddCommandListDependency(promoteCL, ddgiCL);
            uint32_t pr = m_gfx.BeginGPUTimestamp(promoteCL, "DDGI.Promote");
            m_ddgiMgr.PromoteAtlasesForGraphicsQueue(m_gfx, promoteCL);
            m_gfx.EndGPUTimestamp(promoteCL, pr);

            // The specular cube's async copy - the cube DDGI sampled
            // above - is refreshed here, in the first graphics list
            // ordered after DDGI's; the next frame's DDGI waits on a
            // later graphics fence (demoteCL), so it sees this copy
            // complete. The faces copied are the ones the prefilter
            // wrote up to the last frame; whichever order this list and
            // SkyIBLPass take on the GPU, each leaves the cube in the
            // state it found it in.
            if (m_skyIBLPass) m_skyIBLPass->RefreshAsyncCopy(promoteCL);

            // LightingPass is the first graph pass that reads the DDGI SH
            // atlas (its PS samples the irradiance atlas at t29). A targeted
            // wait: GBuffer / Terrain / SkyIBL / SpotShadow / Decal run in
            // parallel with the compute-queue DDGI work.
            m_graph.SetExternalWait("LightingPass", promoteCL);
        }
    }
    // A secondary view's lighting reads the same atlases: it waits on the main view's promote.
    if (primary)
        m_framePromoteCL = promoteCL;
    else if (m_framePromoteCL.IsValid())
    {
        promoteCL = m_framePromoteCL;
        m_graph.SetExternalWait("LightingPass", promoteCL);
    }
    // ... and its traced passes trace the main view's acceleration structure of this frame.
    if (primary)
    {
        m_frameRt.tlasVA        = rtrTlasVA;
        m_frameRt.instanceVA    = rtrInstanceVA;
        m_frameRt.alphaVA       = rtrAlphaVA;
        m_frameRt.alphaCount    = rtrAlphaCount;
        m_frameRt.bindlessTable = rtrBindlessTable;
        m_frameRt.lightsSrv     = rtrLightsSrv;
    }
    else
    {
        rtrTlasVA        = m_frameRt.tlasVA;
        rtrInstanceVA    = m_frameRt.instanceVA;
        rtrAlphaVA       = m_frameRt.alphaVA;
        rtrAlphaCount    = m_frameRt.alphaCount;
        rtrBindlessTable = m_frameRt.bindlessTable;
        rtrLightsSrv     = m_frameRt.lightsSrv;
    }

    _rzMark(6);

    // ---- The plan's stage B (Renderer/FramePlan.h) ----
    // Everything the traced and screen-space decisions below need is known
    // now: the acceleration structure was built (or not) above, and the
    // pyramid, the GBuffer SRVs and the front layer's draws were all settled
    // before it. Planned once; each site below reads m_plan.
    if (primary)
    {
        m_planFacts.tlas       = rtrTlasVA != 0;
        m_planFacts.tlasTables = rtrInstanceVA != 0 && rtrBindlessTable != 0;
        m_planFacts.promote    = promoteCL.IsValid();
#ifdef WITH_HARNESS
        // -frameplan_veto_tlas=<n> (pack frame_plan_veto): frame n's plan sees
        // no TLAS - the path tracer is vetoed for that frame. The TLAS itself
        // is untouched.
        static const int s_vetoFrame = Sys::CommandLine::Get().GetInt("-frameplan_veto_tlas", -1);
        if (s_vetoFrame >= 0 && FrameViewRead().currentFrame == static_cast<uint64_t>(s_vetoFrame))
            m_planFacts.tlas = false;
#endif
        m_planFacts.pyramid    = m_ssrSubsystem && m_ssrSubsystem->GetSceneColorPyramidSrv() != 0;
        m_planFacts.depth      = depthSrv != 0;
        m_planFacts.normal     = normalSrv != 0;
        m_planFacts.frontDraws = m_frontLayerPass && m_frontLayerPass->HasDraws();
        PlanStage(FramePlan::Stage::B);
    }

    // ---- Phase 2b.7: unified reflection wiring. The pass itself runs inside
    // the graph between DecalPass and LightingPass (see Compile()); here it gets
    // this frame's TLAS / DDGI / light data and its wait on promoteCL. Consumed
    // by the post-graph legacy-SSR gating below.
    bool unifiedReflActive = false;
    m_frontLayerActiveThisFrame = false;
    m_ptFrontActiveThisFrame = false;
    m_ptNprActiveThisFrame = false;
    // Every view's: the pass keeps a working set per view (a secondary view's sized in its segment) and
    // a view's plan keeps the tiers where its set is up (SecondaryViewPlan), the glass's front layer
    // too. The budget guards and the path tracer's and ReSTIR shells' blocks below stay the main view's.
    if (m_rtReflectionPass)
    {
        // The bound view's SSR chain: its scene-colour pyramid of last frame.
        SSRSubsystem* const viewSSR = RecordingSSR();
        // Budget guard: a rolling EMA of the "RTReflectionPass" GPU region;
        // sustained over budget -> hard disable until the user re-enables it. Fed
        // only while the GPU profiler runs (editor Profiler window), so it is opt-in
        // (m_rtBudgetGuardEnabled, default false): otherwise RTR would switch off
        // mid-session with only a log line, depending on whether the profiler
        // happens to be open. The ray TMax / pixel-list / roughness caps are the
        // always-on safety net.
        if (primary && m_rtBudgetGuardEnabled && m_rtReflectionsEnabled)
        {
            // Returns false (EMA untouched) whenever the profiler is off, has no
            // results yet, or never timed this region.
            float rtrGpuMs = 0.0f;
            if (m_gfx.GetGPUTimestampResult(RTReflectionPass::kOpaqueName, rtrGpuMs))
            {
                // The translucent front-layer instance shares the budget:
                // disabling only the opaque trace while the glass kept tracing
                // would be the worst of both.
                float frontMs = 0.0f;
                if (m_rtReflectionFrontPass &&
                    m_gfx.GetGPUTimestampResult(RTReflectionPass::kFrontName, frontMs))
                    rtrGpuMs += frontMs;
                // Rolling-average ceiling, user-tunable (Renderer::SetRTBudgetMs
                // / settings.json). ~4 ms is the upper end of what engines spend
                // on 1080p hybrid RT reflections; a 1.5 ms ceiling trips
                // constantly on real scenes (2.4-2.9 ms). A safety net against
                // runaway cost, not a perf target.
                const float kRTRBudgetMs = m_rtBudgetMs;
                m_rtrSmoothedMs += 0.05f * (rtrGpuMs - m_rtrSmoothedMs);
                if (m_rtrSmoothedMs > kRTRBudgetMs) ++m_rtrOverBudgetFrames;
                else                                m_rtrOverBudgetFrames = 0;
                if (m_rtrOverBudgetFrames >= 300)   // ~3 s sustained at 60 fps
                {
                    LOG_WARNING("RTReflection: auto-disabled — %.2f ms avg exceeded the %.2f ms budget for ~3 s "
                                "(turn off 'RT Budget Watchdog' in Render Settings to keep it on)",
                                m_rtrSmoothedMs, kRTRBudgetMs);
                    m_rtReflectionsEnabled = false;
                    m_rtrOverBudgetFrames  = 0;
                    m_rtrSmoothedMs        = 0.0f;
                }
            }
        }

        // RTShadow budget guard: the RTR guard's EMA scheme (~3 s sustained ->
        // hard disable until the user re-enables). Fed only while the editor GPU
        // profiler runs; takes effect next frame. Shares the RTR guard's opt-in
        // switch and ms ceiling.
        if (primary && m_rtBudgetGuardEnabled && m_rtShadowsEnabled)
        {
            float rtShadowGpuMs = 0.0f;
            if (m_gfx.GetGPUTimestampResult("RTShadowPass", rtShadowGpuMs))
            {
                // NRD SIGMA is multi-pass - shares the RTR budget.
                const float kRTShadowBudgetMs = m_rtBudgetMs;
                m_rtShadowSmoothedMs += 0.05f * (rtShadowGpuMs - m_rtShadowSmoothedMs);
                if (m_rtShadowSmoothedMs > kRTShadowBudgetMs) ++m_rtShadowOverBudgetFrames;
                else                                          m_rtShadowOverBudgetFrames = 0;
                if (m_rtShadowOverBudgetFrames >= 300)   // ~3 s at 60 fps
                {
                    LOG_WARNING("RTShadow: auto-disabled — %.2f ms avg exceeded "
                                "the %.2f ms budget for ~3 s (turn off 'RT Budget "
                                "Watchdog' in Render Settings to keep it on)",
                                m_rtShadowSmoothedMs, kRTShadowBudgetMs);
                    m_rtShadowsEnabled         = false;
                    m_rtShadowOverBudgetFrames = 0;
                    m_rtShadowSmoothedMs       = 0.0f;
                }
            }
        }

        // Both tiers from the plan's stage B (Renderer/FramePlan.h). The NRD
        // toggle is applied before the plan reads the pass's availability
        // (CaptureFramePlanState): the unified pass is NRD-only, so NRD off or dead
        // -> IsUnifiedAvailable() false -> the legacy SSR FullFold chain owns t44.
        const bool rtrActive    = m_plan.rtr;
        // Screen-space tier: traced inside RTReflectionPass (FFX classify -> SW
        // Hi-Z intersect) so both tiers share one sampled lobe, one denoiser, one
        // Lighting fold. The plan's pyramid fact mirrors the pass's own screenOk
        // gate: without it an SSR-only frame with no pyramid yet (SSR just toggled
        // on / world reload) would set unifiedReflActive while Execute declines the
        // frame, and Lighting would fold a stale m_denoised.
        const bool screenActive = m_plan.ssrTier;
        const bool pyramidReady = viewSSR && viewSSR->GetSceneColorPyramidSrv() != 0;
        m_rtReflectionPass->SetActive(rtrActive || screenActive);
        // Hoisted out of the block below: the translucent front-layer instance
        // is fed a copy of the same FrameData.
        RTReflectionPass::FrameData fd{};
        if (rtrActive || screenActive)
        {
            // Live tunable (EngineSettings "features.rtReflectionsMaxRoughness").
            m_rtReflectionPass->roughnessThreshold = m_rtrMaxRoughness;
            // Live tunable (EngineSettings "features.rtReflectionsRayTMax").
            // A change also shifts what hitDist means to REBLUR (the pack
            // shader's normalization scales with it), so the accumulated
            // history is no longer comparable: drop it, as SetUseNRD does.
            if (m_rtReflectionPass->rayTMax != m_rtrRayTMax)
            {
                m_rtReflectionPass->rayTMax = m_rtrRayTMax;
                m_rtReflectionPass->ResetHistory();
            }

            // Immutable indices from this packet, including NPR front-layer
            // consumers below. Heap registration is an owner publication task.
            m_rtReflectionPass->materialBufBindless = FrameViewRead().reflectionMaterialSlot;
            m_rtReflectionPass->exposureBufBindless = FrameViewRead().reflectionExposureSlot;
            m_rtReflectionPass->sceneNprMaxBrightness =
                m_ddgiSettings.nprMaxBrightness;

            if (rtrActive)
            {
                fd.tlasVA              = rtrTlasVA;
                fd.instanceVA          = rtrInstanceVA;
                fd.alphaRecordVA       = rtrAlphaVA;
                fd.alphaInstanceCount  = rtrAlphaCount;
                fd.bindlessBufferTable = rtrBindlessTable;
                fd.lightsSrv           = rtrLightsSrv;
            }
            fd.enableScreenTrace = screenActive ? 1u : 0u;
            // fd.ssrPrevSrv stays 0: the unified trace owns every eligible pixel.
            // Last frame's lit scene-color pyramid (parked in SHADER_RESOURCE between
            // frames) is the radiance source for the screen tier AND for the RT tier's
            // screen-radiance reuse at hit points, so it is fed whenever it exists;
            // fd.enableScreenTrace alone decides whether the SW Hi-Z march runs. Do not
            // gate the pyramid on the SSR toggle: RTR-only would then shade every hit
            // through the analytic material proxy, and screen reuse is the only tier
            // that carries a custom pixel shader's real look (e.g. an NPR character).
            //   SSR only   -> screen march only (no RayQuery)
            //   RTR only   -> RayQuery only; on-screen hit points still reuse their
            //                 shaded colour, the proxy covers off-screen hits
            //   SSR + RTR  -> hybrid (march first, ray fallback)
            // 0 -> cb.hasPrevHdr = 0 and the pass binds its fallback SRV (as on the
            // first frame / without the subsystem).
            if (pyramidReady)
            {
                fd.prevHdrSrv  = viewSSR->GetSceneColorPyramidSrv();
                fd.prevHdrMips = viewSSR->GetSceneColorPyramidMips();
            }
            // Reflection probes: analytic RT hits sample the parallax-corrected probe
            // tier Lighting.ps uses, so off-screen reflected content picks up the local
            // environment colour instead of sky only (the main residual SSR / RTR colour
            // gap). The buffer SRV is the per-frame ring slot (BuildScene_UploadProbes
            // ran this frame).
            fd.probeArraySrv  = m_probeMgr.GetArraySrv();
            fd.probeBufferSrv = m_probeMgr.GetBufferSrv(m_gfx);
            fd.probeCount     = m_probeMgr.GetActiveProbeCount();
            // Hit-ambient parity: the SAME irradiance cube Lighting.ps
            // diffuse-samples (real-or-black, set by SyncSkyboxIBL) so the
            // trace's non-SH ambient branch matches the main view exactly.
            fd.skyIrrSrv      = m_skyIrradianceSrvForRTR;
            fd.ddgiVolumeBufSrv   = m_ddgiMgr.GetVolumeBufferSrv(m_gfx);
            fd.ddgiIrradianceTable   = m_ddgiMgr.GetIrradianceTableGpu();
            fd.ddgiDepthTable     = m_ddgiMgr.GetDepthTableGpu();
            fd.ddgiProbeDataTable = m_ddgiMgr.GetProbeDataTableGpu();
            // Mirror Lighting.ps's master gate (LightCB.ddgiEnabled = toggle
            // && count): the trace checks only ddgiVolumeCount > 0, so the raw
            // count would keep stale DDGI irradiance in reflections after the
            // settings toggle dropped it on screen.
            fd.ddgiVolumeCount    = m_ddgiSettings.ddgiEnabled
                                  ? m_ddgiMgr.GetActiveVolumeCount() : 0u;
            fd.lightCount         = m_clusterPass ? m_clusterPass->GetLightCount() : 0u;
            fd.worldLightGridIndex = m_clusterPass ? m_clusterPass->GetWorldLightGridIndex() : ~0u;
            fd.viewProj           = FrameViewRead().view.viewProjMatrix;                     // jittered
            fd.prevViewProj       = RecordingViewState().jitter.GetPrevViewProjJittered();   // jittered
            fd.cameraPos          = FrameViewRead().camera.position;
            // Post-TOD sun, the single sun Lighting.ps and DDGI use; the
            // indirect-lighting scales are mirrored too, so hit-shading
            // brightness matches Lighting.ps.
            if (const auto* lb = m_lightCB.Current(m_gfx))
            {
                fd.sunDirection = { lb->lightDir[0],   lb->lightDir[1],   lb->lightDir[2]   };
                fd.sunColor     = { lb->lightColor[0], lb->lightColor[1], lb->lightColor[2] };
                fd.ddgiDiffuseScale   = lb->ddgiDiffuseScale;
                fd.iblStrength        = lb->iblStrength;
                fd.iblUseSH           = lb->iblUseSH;
                fd.ddgiAONearFieldStrength = lb->ddgiAONearFieldStrength;
                // Flat ambient (None / FlatColor): the renderer zeroes
                // ambientColor in sky modes, so this mirrors Lighting.ps's no-op
                // there.
                fd.flatAmbient = { lb->ambientColor[0] * lb->ambientIntensity,
                                   lb->ambientColor[1] * lb->ambientIntensity,
                                   lb->ambientColor[2] * lb->ambientIntensity };
            }
            // Sky SH for the hit-ambient fallback: the buffer Lighting.ps
            // evaluates at t19 (SH mode only; static-skybox scenes read the
            // irradiance cube instead, which RTR does not bind -> no ambient).
            fd.skySHSrv = m_skyIBLPass ? m_skyIBLPass->GetSHSrvHandle() : 0;
            fd.nearZ      = FrameViewRead().camera.nearZ;
            fd.farZ       = FrameViewRead().camera.farZ;
            fd.frameIndex = static_cast<uint32_t>(FrameViewRead().currentFrame);
            // Sky cube (both denoiser modes: NRD miss radiance + the analytic
            // hit's indirect-specular term), the same cube + fallback DDGI's
            // miss path uses. The mip count mirrors LightCB's iblRadianceMips
            // (the prefiltered chain Lighting.ps walks with roughness *
            // (mips-1)); the 1x1 black fallback has 1 mip.
            fd.skyCubeSrv    = m_skyRadianceSrvForDDGI ? m_skyRadianceSrvForDDGI
                                                       : m_fallbackSkyCubeSrv;
            fd.skyCubeIsReal = (m_skyRadianceSrvForDDGI != 0) ? 1u : 0u;
            {
                const auto* lb2 = m_lightCB.Current(m_gfx);
                fd.skyCubeMips = (fd.skyCubeIsReal && lb2 && lb2->iblRadianceMips > 0)
                               ? lb2->iblRadianceMips : 1u;
            }
            fd.viewMatrix         = FrameViewRead().view.viewMatrix;
            fd.projMatrixNoJitter = FrameViewRead().view.projMatrixNoJitter;
            fd.jitterX = RecordingViewState().jitter.GetJitterX();
            fd.jitterY = RecordingViewState().jitter.GetJitterY();
            // ReSTIR GI as the previous frame left it (RTGI runs after this
            // pass): the environment stand-in's sky visibility and rough band -
            // only while it ran last frame and still runs now (the recording view's).
            {
                const RenderPath rp = GetRenderPath();
                const bool giPath = (rp == RenderPath::ReSTIR_GI || rp == RenderPath::ReSTIR_DI_GI)
                    && (m_viewMode == ViewMode::Lit || IsLightingDebugView(m_viewMode));
                const bool giPrev = primary ? m_giActivePrevFrame : m_secondary[viewSlot].giActivePrevFrame;
                fd.restirGiPrevSrv = (giPrev && giPath && m_rtGiPass)
                                   ? m_rtGiPass->GetUpcomingOutputSrv() : 0;
            }
            m_rtReflectionPass->SetFrameData(fd);

            // The trace reads DDGI probe data (volume / SH / depth SRVs) as well as
            // the TLAS (built on asCL), so it waits for the DDGI compute chain.
            // promoteCL and every graph pass list are graphics, so this is a same-queue
            // ordering edge with no fence; the frame's one graphics-waits-on-compute
            // fence is promoteCL <- ddgiCL. Screen-only frames touch no TLAS.
            if (rtrActive)
                m_graph.SetExternalWait("RTReflectionPass", promoteCL);

            if (m_lightingPass)
                m_lightingPass->SetRTReflection(m_rtReflectionPass->GetUpcomingOutputSrv());
            unifiedReflActive = true;
        }
        else if (m_lightingPass)
        {
            // 0 -> LightingPass binds its 1x1 black fallback (rtConf == 0).
            // Except when the post-graph legacy chain runs in FullFold mode (unified
            // pass unavailable, e.g. no DXR): it binds its upsample output at t44 after
            // the graph, and that binding must survive into next frame's LightingPass
            // recording. Until the legacy chain has produced output (GetFinalColorSrv
            // 0, e.g. the frame reflections switch modes), clear to the black fallback
            // instead of folding whatever t44 last held. The chain also skips a
            // path-traced frame with nothing reading it (m_plan.ssrChain) and binds
            // nothing then either.
            const bool legacyFoldWillBind =
                m_ssrEnabled && viewSSR &&
                m_viewMode != ViewMode::Wireframe && m_plan.ssrChain;
            if (!legacyFoldWillBind || viewSSR->GetFinalColorSrv() == 0)
                m_lightingPass->SetRTReflection(0);
        }

        // ---- Second instance: the translucent FRONT-LAYER trace ----
        // Traces from the TranslucentFrontLayerPass stamp so glass gets the real RT
        // tier (its own normal, off-screen hits, proxy-shaded hits) instead of
        // borrowing the opaque trace behind it. Same TLAS / alpha records / probes /
        // DDGI / sky as the opaque instance; the FrameData differs only in no screen
        // tier and no prev-HDR reuse, because both validate hit pixels against
        // gDepth / gNormal, the translucent buffers on this instance. Gated on the
        // RT tier being live, the forward fold on (its only consumer) and an
        // eligible translucent packet this frame; the prepass follows the same gate
        // (no consumer, no stamp). The LightCB fold before graph.Execute reads
        // m_frontLayerActiveThisFrame to name the front-layer indices or leave them
        // the Tex2D null. A secondary view's plan keeps it where its own working set is
        // up; its draws are its own (counted before this block, as the main view's).
        {
            bool frontActive = false;
            if (m_rtReflectionFrontPass && m_frontLayerPass)
            {
                // From the plan's stage B (Renderer/FramePlan.h).
                frontActive = m_plan.front && m_frontLayerPass->HasDraws() && m_frontLayerPass->EnsureTargets(m_gfx);
                // The main view's switch is also what a preload's worker reads to warm pipelines.
                if (primary) m_frontLayerPass->SetEnabled(frontActive);
                m_frontLayerPass->SetActive(frontActive);
                m_rtReflectionFrontPass->SetActive(frontActive);
                if (frontActive)
                {
                    m_rtReflectionFrontPass->roughnessThreshold = m_rtrMaxRoughness;
                    if (m_rtReflectionFrontPass->rayTMax != m_rtrRayTMax)
                    {
                        m_rtReflectionFrontPass->rayTMax = m_rtrRayTMax;
                        m_rtReflectionFrontPass->ResetHistory();
                    }
                    m_rtReflectionFrontPass->materialBufBindless   = FrameViewRead().reflectionMaterialSlot;
                    m_rtReflectionFrontPass->exposureBufBindless   = FrameViewRead().reflectionExposureSlot;
                    m_rtReflectionFrontPass->sceneNprMaxBrightness =
                        m_ddgiSettings.nprMaxBrightness;
                    RTReflectionPass::FrameData fd2 = fd;
                    fd2.enableScreenTrace = 0u;
                    fd2.prevHdrSrv        = 0;
                    fd2.prevHdrMips       = 0;
                    fd2.restirGiPrevSrv   = 0;   // a pane is not a GI receiver
                    m_rtReflectionFrontPass->SetFrameData(fd2);
                    // Same-queue ordering edge as the opaque instance's (no
                    // fence); registered under this instance's own name.
                    m_graph.SetExternalWait(RTReflectionPass::kFrontName, promoteCL);
                }
            }
            m_frontLayerActiveThisFrame = frontActive;
        }
    }
    // ---- Phase 2b.8: RT sun-shadow mask wiring ----
    // Every view's: the bound view's working set (a secondary view's sized in its segment) and its jitter.
    if (m_rtShadowPass)
    {
        // The sun's direction for the trace (the plan's sun gate is State::sun:
        // lightColor is zeroed every frame when no directional light exists;
        // lightDir is deliberately left stale - never test it).
        DirectX::XMFLOAT3 sunDir{ 0.f, -1.f, 0.f };
        if (const auto* lb = m_lightCB.Current(m_gfx))
            sunDir = { lb->lightDir[0], lb->lightDir[1], lb->lightDir[2] };
        // The plan's stage B holds every term. Its output term
        // (State::rtShadowOutput, GetUpcomingOutputSrv() != 0) is load-bearing:
        // LightCB.rtShadowEnabled makes Lighting.ps Load() the t45 mask across the
        // whole screen, so the flag must never be set without a live full-res mask
        // (the fallback bound for handle 0 is 1x1, and an out-of-bounds Load there
        // returns 0 = fully shadowed everywhere).
        const bool rtShadowActive = m_plan.rtShadow;
        m_rtShadowPass->SetActive(rtShadowActive);
        // (A camera cut reset its history at the frame's start: the view's
        // CameraCut, Render_BindFrameResources.)
        if (rtShadowActive)
        {
            RTShadowPass::FrameData fd{};
            fd.tlasVA             = rtrTlasVA;
            fd.alphaRecordVA      = rtrAlphaVA;
            // Cutout UV transform lives in the instance table, not the 40 B
            // alpha record (RTShadow.cs.hlsl RTS_ClassifyCandidate).
            fd.instanceVA         = rtrInstanceVA;
            fd.alphaInstanceCount = rtrAlphaCount;
            fd.bindlessBufferTable= rtrBindlessTable;
            fd.viewProj           = FrameViewRead().view.viewProjMatrix;   // jittered, matches the depth buffer
            fd.viewMatrix         = FrameViewRead().view.viewMatrix;        // NRD worldToView (non-jittered)
            fd.projMatrixNoJitter = FrameViewRead().view.projMatrixNoJitter;
            fd.jitterX            = RecordingViewState().jitter.GetJitterX();
            fd.jitterY            = RecordingViewState().jitter.GetJitterY();
            fd.sunDirection       = sunDir;                  // FROM-light (post-TOD)
            fd.nearZ              = FrameViewRead().camera.nearZ;
            fd.farZ               = FrameViewRead().camera.farZ;
            fd.frameIndex         = static_cast<uint32_t>(FrameViewRead().currentFrame);
            m_rtShadowPass->SetFrameData(fd);
            // Shadow rays read the TLAS (built on asCL) and no DDGI resource (RTShadow.cs
            // binds t0 TLAS, t1-t4 GBuffer, t7 alpha, t8 instances), so the promoteCL wait
            // is stricter than the data requires. Retargeting it at asCL bought no
            // cross-queue overlap and cost two extra submit batch splits (RTShadow and RTAO
            // as cross-queue waiters, one ExecuteCommandLists + Signal each, ~0.1 ms of
            // EndFrame). RT shadow / AO rays do not contend with the DDGI trace for
            // traversal units; why the overlap window does not fill needs a PIX / GPUView
            // capture.
            // Retarget, never delete: the plan's RT-shadow want is independent of RTR's
            // (FramePlan's shadowWant / rtrWant), so with RTR off this edge is the only
            // thing ordering the shadow trace after the compute-queue TLAS build -
            // deleting it is a GPU race.
            m_graph.SetExternalWait("RTShadowPass", promoteCL);
            if (m_lightingPass)
                m_lightingPass->SetRTShadowMask(m_rtShadowPass->GetUpcomingOutputSrv());
        }
        else if (m_lightingPass)
        {
            // 0 -> LightingPass binds the shared 1x1 lit fallback (min() no-op).
            m_lightingPass->SetRTShadowMask(0);
        }
        // LightCB gate, written every frame (the triple-buffered slot would
        // otherwise carry a 3-frame-old value); Lighting.ps samples t45 only on
        // frames a fresh mask exists.
        if (auto* lb = m_lightCB.Current(m_gfx))
            lb->rtShadowEnabled = rtShadowActive ? 1.0f : 0.0f;
    }

    // ---- Phase 2b.9: RTAO wiring (graph pass, graphics queue) ----
    // RTAOPass runs in the graph (after Decal, before Lighting) and, when
    // active, is the AO source: Lighting's SSAO handle points at its output and
    // XeGTAO is deactivated (Phase 2b.9b). Same frame, no one-frame latency.
    // Its rays read the asCL TLAS -> the same promoteCL wait.
    // Every view's, as the sun-shadow mask's.
    bool rtaoActive = false;
    if (m_rtaoPass)
    {
        // From the plan's stage B (Renderer/FramePlan.h).
        rtaoActive = m_plan.rtao;
        m_rtaoPass->SetActive(rtaoActive);
        // (A camera cut: the view's CameraCut at the frame's start.)
        if (rtaoActive)
        {
            RTAOPass::FrameData fd{};
            fd.tlasVA             = rtrTlasVA;
            fd.alphaRecordVA      = rtrAlphaVA;
            // Cutout UV transform lives in the instance table, not the 40 B
            // alpha record (RTAO.cs.hlsl RTAO_AlphaAccept).
            fd.instanceVA         = rtrInstanceVA;
            fd.alphaInstanceCount = rtrAlphaCount;
            fd.bindlessBufferTable= rtrBindlessTable;
            fd.viewProj           = FrameViewRead().view.viewProjMatrix;   // jittered, matches the depth buffer
            fd.viewMatrix         = FrameViewRead().view.viewMatrix;        // NRD worldToView (non-jittered)
            fd.projMatrixNoJitter = FrameViewRead().view.projMatrixNoJitter;
            fd.jitterX            = RecordingViewState().jitter.GetJitterX();
            fd.jitterY            = RecordingViewState().jitter.GetJitterY();
            fd.nearZ              = FrameViewRead().camera.nearZ;
            fd.farZ               = FrameViewRead().camera.farZ;
            fd.frameIndex         = static_cast<uint32_t>(FrameViewRead().currentFrame);
            m_rtaoPass->SetFrameData(fd);
            // As with RTShadowPass's wait: RTAO.cs binds no DDGI resource, so
            // this wait is stricter than the data requires, but retargeting it at
            // asCL bought no overlap and cost an extra submit batch split.
            m_graph.SetExternalWait("RTAOPass", promoteCL);
            // AO source swap: Lighting samples the RTAO output at gSSAO (t18)
            // instead of XeGTAO's. XeGTAO is deactivated in Phase 2b.9b below.
            if (m_lightingPass) m_lightingPass->SetSSAOHandle(m_rtaoPass->GetUpcomingOutputSrv());
        }
    }

    bool ptActive = false, diActive = false, giActive = false;
    // The ReSTIR shells and the path tracer record every view (a working set per view; a secondary view's plan
    // keeps pt / di / gi where its sets are up). The path tracer's RTXDI mode, front reflection and toon
    // lighting, the light tables and the compare shadows stay the main view's: their setters below are the
    // main view's only (a secondary view's call would reset their history).
    if (m_pathTracePass || m_rtDiPass || m_rtGiPass)
    {
        PathTracePass::FrameData fd{};
        // The three from the plan's stage B (Renderer/FramePlan.h): the path
        // tracer only while it owns the frame and the TLAS is there; the
        // ReSTIR shells also in the lighting-bucket views.
        ptActive = m_plan.pt;
        diActive = m_plan.di;
        giActive = m_plan.gi;
        // Next frame's reflection pass reads what RTGI writes this frame (each view's its own).
        if (primary) m_giActivePrevFrame = giActive;
        else m_secondary[viewSlot].giActivePrevFrame = giActive;
        fd.tlasVA = rtrTlasVA; fd.instanceVA = rtrInstanceVA; fd.alphaVA = rtrAlphaVA;
        fd.instanceCount = rtrAlphaCount; fd.buffers = rtrBindlessTable;
        fd.viewProj = FrameViewRead().view.viewProjMatrix; fd.camera = FrameViewRead().camera.position;
        fd.farZ = FrameViewRead().camera.farZ;
        fd.nearZ = FrameViewRead().camera.nearZ;
        fd.jitterX = RecordingViewState().jitter.GetJitterX(); fd.jitterY = RecordingViewState().jitter.GetJitterY();
        fd.cameraCut = (RecordingViewState().FrameBreaks() & HistoryReset::CameraCut) != 0;
        fd.contentCut = (RecordingViewState().FrameBreaks() & HistoryReset::Cuts) != 0;
        fd.view = FrameViewRead().view.viewMatrix;
        fd.projection = FrameViewRead().view.projMatrixNoJitter;
        // -pt_static_scene: reference runs accumulate through skinned TLAS
        // content whatever it does: neither a pose change, the shading records
        // nor on-screen motion (the object-motion probe) gate the path tracer. Pause the animators first (Lua
        // Animator.Pause / AnimGraph.SetTimeScale 0), or the image averages
        // the poses, and change no material during the run. Without it a
        // skinned instance restarts the image only when its pose changes
        // (DDGISceneAS::ContentFingerprint).
        static const bool s_ptStaticScene = Sys::CommandLine::Get().Has("-pt_static_scene");
        fd.fingerprint = s_ptStaticScene ? m_ddgiSceneAS.TlasFingerprint() : m_ddgiSceneAS.ContentFingerprint();
        fd.staticScene = s_ptStaticScene;
        fd.worldClockRuns = FrameViewRead().worldDeltaTime > 0.f;
        // The next Gather hashes the skinned rows' deformation for it.
        m_ddgiSceneAS.SetDeformFingerprintWanted(ptActive && !s_ptStaticScene);
        if (ptActive || diActive || giActive)
        {
            // The shading and light fingerprints gate only the PT's
            // progressive accumulation (PathTracePass::SetFrameData). RTDI and
            // RTGI never read them, and hashing the instance tables costs ~7 ms
            // of every DI / GI frame.
            if (ptActive)
            {
                fd.shadingFingerprint = s_ptStaticScene ? 0ull
                    : m_ddgiSceneAS.ShadingFingerprint() ^ m_ddgiSceneAS.PathMaterialFingerprint();
                fd.lightsFingerprint = m_clusterPass ? m_clusterPass->GetLightsFingerprint() : 0;
            }
            const auto pathMaterialSrv = m_ddgiSceneAS.GetPathMaterialSrv(m_gfx);
            const auto pathMaterialIdx = pathMaterialSrv ? m_gfx.DescriptorHeapIndexFromGpuHandle(pathMaterialSrv) : ~0u;
            fd.pathMaterialsIdx = pathMaterialIdx == ~0u ? 0 : pathMaterialIdx;
            fd.transmissive = m_ddgiSceneAS.TransmissiveInstanceCount() > 0;
            // The medium only where the plan gives the path tracer the fog (FramePlan::FogOwner):
            // elsewhere the raster fog passes draw it, once.
            if (m_plan.fogOwner == FramePlan::FogOwner::PathTracer)
            {
                fd.mediumIdx = m_mediumIdx;
                fd.sunMediumScale = m_sunMediumScale;
                fd.mediumFingerprint = Sys::WordHashCombine(m_mediumSet.Fingerprint(),
                                                            Sys::WordHash64(&fd.sunMediumScale, sizeof(float)));
            }
            // The rtxdi core's segments cross the fog whoever draws the camera's (the raster fog
            // does beside it).
            if (ptActive && m_pathTracePass && m_pathTracePass->RtxdiMediaEnabled() && m_mediumIdx != 0)
            {
                const uint32_t rule = PathTracePass::RtxdiMediaRule();
                fd.rtxdiMediumIdx = m_mediumIdx;
                if (rule == 3u && m_volFogPass) fd.rtxdiFroxel = m_volFogPass->LastLitVolume(RecordingViewState());
                fd.mediumFingerprint = Sys::WordHashCombine(m_mediumSet.Fingerprint(), Sys::WordHash64(&rule, sizeof(rule)));
            }
            fd.textureRevision = m_texSys ? m_texSys->ContentRevision() : 0;
            fd.surfaceCacheRevision = m_surfaceCache.GetPublishEpoch();
            const auto idx = rtrLightsSrv ? m_gfx.DescriptorHeapIndexFromGpuHandle(rtrLightsSrv) : ~0u;
            fd.lightsIdx = idx == ~0u ? 0 : idx;
            fd.lightCount = fd.lightsIdx && m_clusterPass ? m_clusterPass->GetLightCount() : 0;
            fd.activeLocalLightCount = fd.lightCount ? m_clusterPass->GetActiveLocalLightCount() : 0;
            fd.firstActiveLocalLightIndex = fd.lightCount ? m_clusterPass->GetFirstActiveLocalLightIndex() : 0;
            if (m_clusterPass)
            {
                // This render frame's slot of the light key map, the previous
                // light list and the light alias table.
                auto heapIndex = [&](uint64_t srv) -> uint32_t {
                    if (!srv) return 0u;
                    const uint32_t i = m_gfx.DescriptorHeapIndexFromGpuHandle(srv);
                    return i == ~0u ? 0u : i;
                };
                const ClusterPass::LightHistory history = m_clusterPass->GetLightHistory();
                fd.lightMapIdx = heapIndex(history.mapSRV);
                fd.prevLightsIdx = heapIndex(history.prevLightsSRV);
                fd.prevLightCount = history.prevCount;
                fd.lightsIdentity = history.identity;
                const ClusterPass::LightAlias alias = m_clusterPass->GetLightAlias();
                fd.lightAliasIdx = fd.lightCount ? heapIndex(alias.srv) : 0u;
                fd.lightAliasCount = fd.lightAliasIdx ? alias.count : 0u;
                if (fd.mediumIdx)
                    fd.lightMediumScaleIdx = fd.lightCount ? heapIndex(m_clusterPass->GetLightMediumScaleSRVHandle()) : 0u;
            }
            const auto materialSrv = m_gfx.GetBufferSRVGpuHandle(MaterialSRVBuffer(m_gfx.GetFrameIndex()));
            const auto materialIdx = materialSrv ? m_gfx.DescriptorHeapIndexFromGpuHandle(materialSrv) : ~0u;
            fd.materialsIdx = materialIdx == ~0u ? 0 : materialIdx;
        }
        fd.stable = m_ddgiSceneAS.ContentFingerprintValid() && m_ddgiSceneAS.StaticBlasAllResolved();
        if (s_ptStaticScene)   // see above: the TLAS fingerprint does not gate it either
            fd.stable = m_ddgiSceneAS.StaticBlasAllResolved();
        // A material sky is allowed to animate every frame without changing
        // its descriptor. It cannot share a static reference accumulation
        // while the world clock runs; it holds still when the clock does (the
        // path tracer's time freeze).
        // -pt_static_sky: reference runs accumulate through an animated material
        // sky (its light reaches a night interior only through the windows).
        if (m_skyIBLPass && m_skyIBLPass->IsMaterialSkyEnabled() && !Sys::CommandLine::Get().Has("-pt_static_sky")
            && FrameViewRead().worldDeltaTime > 0.f)
            fd.stable = false;
        // A frozen world's step (Render.ptStep) moved everything time-driven at once.
        if (FrameViewRead().worldStepped)
            fd.stable = false;
        if (!fd.stable && ptActive && Sys::CommandLine::Get().Has("-pt_stability_log")
            && FrameViewRead().currentFrame % 60 == 0)
            LOG_INFO("[PT_STABILITY] contentHash=%u blasResolved=%u materialSky=%u",
                m_ddgiSceneAS.ContentFingerprintValid() ? 1u : 0u, m_ddgiSceneAS.StaticBlasAllResolved() ? 1u : 0u,
                (m_skyIBLPass && m_skyIBLPass->IsMaterialSkyEnabled()) ? 1u : 0u);
        const auto sky = m_skyRadianceSrvForDDGI ? m_skyRadianceSrvForDDGI : m_fallbackSkyCubeSrv;
        if (sky) { const auto idx = m_gfx.DescriptorHeapIndexFromGpuHandle(sky); fd.skyIdx = idx == ~0u ? 0 : idx; }
        if (const auto* lb = m_lightCB.Current(m_gfx))
        {
            fd.sunDirection = {lb->lightDir[0],lb->lightDir[1],lb->lightDir[2]};
            fd.sunColor = {lb->lightColor[0],lb->lightColor[1],lb->lightColor[2]};
            fd.environmentScale = lb->iblStrength;
        }
        if (fd.cameraCut && (ptActive || diActive || giActive))
        {
            // Scripted camera-stack cuts do not necessarily go through the
            // editor's InvalidateTemporalHistory, so invalidate the NRD history
            // and the reservoir proposals on this render frame. The view's
            // CameraCut, applied here rather than at the frame's start: only to
            // the passes this frame's plan runs, which its stage B decides (their
            // ViewHistory::Invalidate leaves a lone CameraCut to this block). A
            // pass that does not run resets when it switches on anyway.
            const bool apply=!Sys::CommandLine::Get().Has("-restir_no_camera_cut_reset");
            if (apply)
            {
                if (ptActive && m_pathTracePass) m_pathTracePass->ResetHistory();
                if (diActive && m_rtDiPass) m_rtDiPass->ResetHistory();
                if (diActive && m_rtDiShadowPass) m_rtDiShadowPass->ResetHistory();
                if (giActive && m_rtGiPass) m_rtGiPass->ResetHistory();
                if (giActive && m_rtGiShadowPass) m_rtGiShadowPass->ResetHistory();
            }
            if (Sys::CommandLine::Get().Has("-restir_history_probe"))
                LOG_INFO("[RESTIR_CAMERA_CUT] frame=%u applied=%u di=%u gi=%u pt=%u",
                    static_cast<uint32_t>(FrameViewRead().currentFrame),apply,diActive,giActive,ptActive);
        }
        fd.diActive = diActive;
        if (m_pathTracePass) m_pathTracePass->SetFrameData(fd, ptActive);
        // The glass's own reflection on this path: the front-layer prepass
        // stamps the nearest pane for PathTracePass's FrontReflection entry
        // (the RT reflection front instance it feeds on the other paths never
        // runs here); the LightCB fold before graph.Execute names the result.
        m_ptFrontActiveThisFrame = primary && ptActive && m_frontLayerPass && m_frontLayerPass->HasDraws() &&
                                   m_pathTracePass->FrontReflectionReady() && m_frontLayerPass->EnsureTargets(m_gfx);
        if (primary && m_pathTracePass) m_pathTracePass->SetFrontReflectionActive(m_ptFrontActiveThisFrame);
        if (m_ptFrontActiveThisFrame)
        {
            m_frontLayerPass->SetEnabled(true);
            m_frontLayerPass->SetActive(true);
        }
        // Toon receivers on screen take the path tracer's ambient and sun
        // visibility (the NPR lighting variant only draws with a GBuffer
        // primary: camera-primary PT shades every pixel itself).
        m_ptNprActiveThisFrame = primary && ptActive && m_nprDrawCount > 0 && !m_pathTracePass->UsesCameraPrimary() &&
                                 m_pathTracePass->NprLightingReady();
        if (primary && m_pathTracePass) m_pathTracePass->SetNprLightingActive(m_ptNprActiveThisFrame);
        const bool cameraPrimary=ptActive && m_pathTracePass->UsesCameraPrimary();
        if(m_lightingPass) m_lightingPass->SetCameraPrimary(cameraPrimary);
        // The path tracer's camera rays shade every pixel, the
        // background included - the main view hides the environment (the
        // view Render_BindFrameResources set, every frame, so leaving the
        // camera primary shows it again).
        if(cameraPrimary)
        {
            RG::SceneView view = m_graph.View();
            view.show.HideAll(RG::kShowEnvironment);
            m_graph.SetView(view);
        }
        // The fog is the path tracer's (FramePlan::FogOwner): the raster fog passes and their forward
        // fog are hidden, and on the GBuffer primary the path tracer's camera fog is composited
        // where the height fog would be (IPathTraceFogComposite; the camera primary integrates it).
        // With the ray-traced froxels (fogRt) the volumetric fog stays, lit for the surfaces drawn
        // after it alone (VolumetricFogPass::RtTier::ForwardOnly composites nothing).
        if(m_plan.fogOwner==FramePlan::FogOwner::PathTracer)
        {
            RG::SceneView view = m_graph.View();
            view.show.HideAll(static_cast<uint32_t>(RG::ShowFlag::HeightFog) |
                              (m_plan.fogRt ? 0u : static_cast<uint32_t>(RG::ShowFlag::VolumetricFog)));
            m_graph.SetView(view);
        }
        {
            const uint32_t fogIdx = m_pathTracePass ? m_pathTracePass->GetCameraFogIndex() : 0u;
            m_features.ForEach<IPathTraceFogComposite>([fogIdx](IPathTraceFogComposite& c) { c.SetPathTraceFog(fogIdx); });
        }
        // The light tables are the world's: built on the main view's call, read by every view's DI.
        if (primary && m_lightSamplingPass)
        {
            const uint32_t lightsIdx = rtrLightsSrv ? m_gfx.DescriptorHeapIndexFromGpuHandle(rtrLightsSrv) : ~0u;
            m_lightSamplingPass->SetFrameData(lightsIdx == ~0u ? 0u : lightsIdx,
                m_clusterPass ? m_clusterPass->GetLightCount() : 0u,
                static_cast<uint32_t>(FrameViewRead().currentFrame), m_lightSamplingPass->IsWanted());
            // The RTXDI DI core samples its local lights from this frame's
            // PDF texture (0 when the tables are not built).
            if (m_rtDiPass)
            {
                const auto tables = m_lightSamplingPass->IsActive() ? m_lightSamplingPass->GetTables() : LightSamplingPass::Tables{};
                m_rtDiPass->SetLightTables(tables.pdfIdx, tables.pdfSize, tables.pdfMips);
                if (m_rtDiShadowPass) m_rtDiShadowPass->SetLightTables(tables.pdfIdx, tables.pdfSize, tables.pdfMips);
            }
        }
        if (m_rtDiPass)
            m_rtDiPass->SetTemporalAA(((m_taaPass && m_taaPass->IsEnabled()) || IsFSRActive() || IsDLSSActive())
                && !FrameViewRead().camera.orthographic);
        if (m_rtDiPass) m_rtDiPass->SetFrameData(fd, diActive,
            m_clusterPass ? m_clusterPass->GetWorldLightGridIndex() : ~0u,
            static_cast<uint32_t>(FrameViewRead().currentFrame), RecordingViewState().jitter.GetJitterX(),
            RecordingViewState().jitter.GetJitterY());
        if (primary && m_rtDiShadowPass)
        {
            m_rtDiShadowPass->SetTemporalAA(((m_taaPass && m_taaPass->IsEnabled()) || IsFSRActive() || IsDLSSActive())
                && !FrameViewRead().camera.orthographic);
            m_rtDiShadowPass->SetFrameData(fd, diActive && m_restirCompareDiActive,
                m_clusterPass ? m_clusterPass->GetWorldLightGridIndex() : ~0u,
                static_cast<uint32_t>(FrameViewRead().currentFrame), m_mainView.jitter.GetJitterX(), m_mainView.jitter.GetJitterY());
        }
        if (m_rtGiPass)
        {
            RTGIPass::FrameData gi;
            static_cast<PathTracePass::FrameData&>(gi)=fd;
            gi.volumeSrv=m_ddgiMgr.GetVolumeBufferSrv(m_gfx);
            gi.irradianceTable=m_ddgiMgr.GetIrradianceTableGpu();
            gi.depthTable=m_ddgiMgr.GetDepthTableGpu();
            gi.probeTable=m_ddgiMgr.GetProbeDataTableGpu();
            gi.volumeCount=m_ddgiSettings.ddgiEnabled?m_ddgiMgr.GetActiveVolumeCount():0u;
            gi.worldGrid=m_clusterPass?m_clusterPass->GetWorldLightGridIndex():~0u;
            gi.emitterCount=m_ddgiSceneAS.GetGIEmitterCount();
            gi.emitterSrv=gi.emitterCount?m_ddgiSceneAS.GetGIEmitterSrv(m_gfx):0;
            gi.emitterTableUav=gi.emitterCount?m_ddgiSceneAS.GetGIEmitterTableUav(m_gfx):0;
            if(!gi.emitterSrv || !gi.emitterTableUav) gi.emitterCount=0;
            m_rtGiPass->SetFrameData(gi,giActive,static_cast<uint32_t>(FrameViewRead().currentFrame),
                RecordingViewState().jitter.GetJitterX(),RecordingViewState().jitter.GetJitterY());
            if (primary && m_rtGiShadowPass)
                m_rtGiShadowPass->SetFrameData(gi,giActive && m_restirCompareGiActive,static_cast<uint32_t>(FrameViewRead().currentFrame),
                    m_mainView.jitter.GetJitterX(),m_mainView.jitter.GetJitterY());
        }
        if (giActive) m_graph.SetExternalWait("RTGI", promoteCL);
        if (diActive) m_graph.SetExternalWait("RTDI", promoteCL);
        if (ptActive) m_graph.SetExternalWait("PT.Trace", promoteCL);
    }

    // ---- The froxel fog's ray-traced lighting (FramePlan::Decisions::fogRt) ----
    // Its tier - forward-only under the path tracer's own fog, the sun and every light in the
    // froxels on the path-traced paths, beside the raymarched shafts elsewhere - and what it
    // reads: this frame's TLAS and tables, ClusterPass's lights and key map, the medium. The
    // height fog's composite then starts where the froxels end (IFroxelHeightFog).
    if (m_volFogPass)
    {
        VolumetricFogPass::RtTier tier = VolumetricFogPass::RtTier::Off;
        VolumetricFogPass::RtInputs in;
        // The medium on every frame: the raymarched shafts' sun crosses it too
        // (pass.volumetricFog.raymarchSunTransmittance).
        in.mediumIdx = m_mediumIdx;
        if (m_plan.fogRt)
        {
            tier = m_plan.fogOwner == FramePlan::FogOwner::PathTracer ? VolumetricFogPass::RtTier::ForwardOnly
                 : (m_plan.tier == FramePlan::Tier::T4 || m_plan.tier == FramePlan::Tier::Ref)
                       ? VolumetricFogPass::RtTier::Full : VolumetricFogPass::RtTier::Hybrid;
            auto heapIndex = [&](uint64_t srv) -> uint32_t {
                if (!srv) return 0u;
                const uint32_t i = m_gfx.DescriptorHeapIndexFromGpuHandle(srv);
                return i == ~0u ? 0u : i;
            };
            in.tlasVA = rtrTlasVA; in.instanceVA = rtrInstanceVA; in.alphaVA = rtrAlphaVA;
            in.buffers = rtrBindlessTable;
            in.alphaTestEnabled = rtrAlphaVA != 0 ? 1u : 0u;
            in.alphaInstanceCount = rtrAlphaCount;
            in.pathMaterialsIdx = heapIndex(m_ddgiSceneAS.GetPathMaterialSrv(m_gfx));
            in.glass = in.pathMaterialsIdx != 0 && m_ddgiSceneAS.TransmissiveInstanceCount() > 0;
            in.sunMediumScale = m_sunMediumScale;
            if (m_clusterPass)
            {
                in.lightsIdx = heapIndex(rtrLightsSrv ? rtrLightsSrv : m_clusterPass->GetLightsSRVHandle());
                in.lightCount = in.lightsIdx ? m_clusterPass->GetLightCount() : 0u;
                in.lightGridIdx = heapIndex(m_clusterPass->GetLightGridSRVHandle());
                in.lightIndexIdx = heapIndex(m_clusterPass->GetLightIndexSRVHandle());
                const ClusterPass::LightHistory history = m_clusterPass->GetLightHistory();
                in.lightMapIdx = heapIndex(history.mapSRV);
                in.prevLightCount = history.prevCount;
                in.lightMediumScaleIdx = in.lightCount ? heapIndex(m_clusterPass->GetLightMediumScaleSRVHandle()) : 0u;
            }
            in.clusterNear = FrameViewRead().camera.nearZ;
            in.clusterFar = FrameViewRead().camera.farZ;
            // The indirect light's sources: DDGI on a frame whose plan runs its volumes (the fog never
            // turns it on), the sky the path tracer's misses sample and its scale, and the path
            // tracer's SHaRC cache, which PT.Trace resolves earlier in the graph on a frame it traces.
            in.ddgi = m_plan.ddgi && m_ddgiMgr.GetActiveVolumeCount() > 0;
            in.skyIdx = heapIndex(m_skyRadianceSrvForDDGI ? m_skyRadianceSrvForDDGI : m_fallbackSkyCubeSrv);
            if (const auto* lb = m_lightCB.Current(m_gfx)) in.environmentScale = lb->iblStrength;
            if (ptActive && m_pathTracePass) in.sharc = m_pathTracePass->GetSharcLookup();
            if (promoteCL.IsValid()) m_graph.SetExternalWait("VolumetricFogPass", promoteCL);
        }
        m_volFogPass->SetRt(tier, in);
        const float range = m_volFogPass->RtRuns() ? m_volFogPass->EffectiveFroxelFar() : 0.0f;
        m_features.ForEach<IFroxelHeightFog>([range](IFroxelHeightFog& h) { h.SetFroxelHeightFog(range); });
    }

    // ---- Render features: the graph stages prepare (RenderFeature.h) ----
    // After the frame's view is settled - a debug view or an ortho camera hides
    // the environment (Render_BindFrameResources), and so does the path
    // tracer's camera primary just above - and before the forward fog
    // (WriteFogApplyCB) and the graph read what the features set.
    {
        // [RG_VIEW] - what the main view shows, one line whenever its flags
        // change (RenderGraph/SceneView.h).
        const RG::SceneView& view = m_graph.View();
        if (primary && view.show.bits != m_loggedViewShow)
        {
            m_loggedViewShow = view.show.bits;
            const std::string hidden = RG::ShowFlagList(~view.show.bits & RG::kShowAll);
            LOG_INFO("[RG_VIEW] name=%s show=0x%02x hidden=%s viewMode=%u ortho=%d", view.name, view.show.bits,
                     hidden.empty() ? "none" : hidden.c_str(), unsigned(view.viewMode), view.orthographic ? 1 : 0);
        }
        // The features prepare the frame once, for the main view; a secondary view's segment hands them
        // its frame for what their view-safe passes need of it (its camera).
        FeatureFrameContext featureCtx{ FrameViewRead(), m_plan, m_planFacts, m_gfx, view, &DrawPacketsRead() };
        if (primary)
            m_features.PrepareGraphStages(featureCtx);
        else
            m_features.PrepareView(featureCtx);
    }

    // ---- Phase 2b.10: SSGI wiring (graph pass, graphics queue) ----
    // Screen-space diffuse GI, the near-field half of the hybrid. Needs no TLAS
    // and no promoteCL wait: it marches the depth buffer and samples the
    // previous frame's scene-colour pyramid, so it has no cross-queue producer.
    bool ssgiActive = false;
    if (m_ssgiPass)
    {
        // A secondary view's working set at its size (its segment, before the graph records).
        if (!primary)
            m_ssgiPass->EnsureViewResources(FrameViewRead().renderWidth, FrameViewRead().renderHeight);
        // SSGIPass owns and always builds its own Hi-Z, so this gate has no
        // reflection dependency. GetInertReason() covers every precondition Execute
        // checks, the NRD instance included: without it ssgiActive could read true
        // while Execute returns early on a failed REBLUR_DIFFUSE resize.
        // From the plan's stage B (Renderer/FramePlan.h): not under the path
        // tracer or ReSTIR GI.
        ssgiActive = m_plan.ssgi;

        // ---- Diagnostic ----
        // Every failure mode in this chain looks like the feature being off
        // (coverage 0 renders exactly like SSGI disabled), so the log names the
        // first unmet condition, once per change. The heap index is resolved first
        // so the log can report it: the pass can execute perfectly and still be
        // invisible if Lighting.ps opens the wrong descriptor. A typed handle;
        // "no SSGI" is the Tex2D null (the shaders' BindlessNone gate), which is
        // also what the handle check returns for a slot it refuses.
        uint32_t ssgiHeapIdx = BINDLESS_NULL_TEX2D_FLOAT;
        if (ssgiActive)
            ssgiHeapIdx = m_gfx.BindlessOf<RHI::Bindless::Kind::Tex2DFloat>(
                m_ssgiPass->GetUpcomingOutputSrv(), "SSGI result").Bits();
        const char* inert = m_ssgiPass->GetInertReason();
        SSRSubsystem* const ssgiSsr = RecordingSSR();   // the bound view's pyramid
        const bool  pyramidReady = ssgiSsr && ssgiSsr->GetSceneColorPyramidSrv() != 0;
        const uint32_t sig = (m_ddgiSettings.ssgiEnabled ? 1u : 0u)
                           | (ssgiActive     ? 2u : 0u)
                           | (pyramidReady   ? 4u : 0u)
                           | (inert          ? 8u : 0u)
                           | ((m_ssgiPass->GetLastActivePixelEstimate() != 0) ? 16u : 0u)
                           | ((ssgiHeapIdx != BINDLESS_NULL_TEX2D_FLOAT) ? 32u : 0u)
                           | (m_ddgiSettings.ssgiReservoirEnabled ? 64u : 0u);
        static uint32_t s_prevSsgiSig = 0xFFFFFFFFu;
        if (primary && sig != s_prevSsgiSig)
        {
            LOG_INFO("SSGI: toggle=%s active=%s executed=%s pyramid=%s reservoir=%s heapIdx=%u%s%s",
                     m_ddgiSettings.ssgiEnabled ? "ON" : "off",
                     ssgiActive ? "yes" : "NO",
                     m_ssgiPass->GetLastActivePixelEstimate() ? "yes" : "NO",
                     pyramidReady ? "ready" : "MISSING(coverage will be 0)",
                     m_ddgiSettings.ssgiReservoirEnabled ? "0.25-rpp" : "legacy",
                     RHI::Bindless::IndexOf(ssgiHeapIdx),
                     inert ? "  INERT: " : "",
                     inert ? inert : "");
            // Separate line: the pre-flight gate above and what Execute
            // actually did are different questions; conflating them makes
            // "executed=yes" lie.
            if (const char* bail = m_ssgiPass->GetExecBailReason())
                LOG_INFO("SSGI: Execute BAILED -> %s", bail);
            s_prevSsgiSig = sig;
        }
        const bool giHiZOnly=giActive && m_ssgiPass->GetDepthHierarchySrv()!=0 && m_ssgiPass->GetDepthHierarchyMipCount()!=0;
        m_ssgiPass->SetHiZOnly(giHiZOnly);
        m_ssgiPass->SetActive(ssgiActive || giHiZOnly);
        // (A camera cut restarted its histories at the frame's start: the
        // view's CameraCut.)
        if (ssgiActive)
        {
            // Artist knobs live on the scene's IndirectLightingSettings and are
            // copied every frame: the component is re-read from the ECS pool
            // each frame, so caching them at Init would freeze them.
            m_ssgiPass->maxDistance = m_ddgiSettings.ssgiMaxDistance;
            m_ssgiPass->thickness   = m_ddgiSettings.ssgiThickness;
            m_ssgiPass->rayCount    = m_ddgiSettings.ssgiRayCount;
            m_ssgiPass->debugMode   = m_ddgiSettings.ssgiDebugMode;
            m_ssgiPass->SetReservoirEnabled(
                m_ddgiSettings.ssgiReservoirEnabled);
            m_ssgiPass->reservoirSpatialCandidates =
                m_ddgiSettings.ssgiReservoirSpatialCandidates;
            m_ssgiPass->reservoirMaxM = m_ddgiSettings.ssgiReservoirMaxM;
            m_ssgiPass->reservoirMaxAge = m_ddgiSettings.ssgiReservoirMaxAge;
            m_ssgiPass->reservoirValidationSteps =
                m_ddgiSettings.ssgiReservoirValidationSteps;

            // The bound view's camera and jitter.
            const ViewState& ssgiView = RecordingViewState();
            SSGIPass::FrameData fd{};
            fd.viewProj           = FrameViewRead().view.viewProjMatrix;      // jittered, matches the depth buffer
            fd.prevViewProj       = ssgiView.jitter.GetPrevViewProjJittered();
            fd.viewMatrix         = FrameViewRead().view.viewMatrix;          // NRD worldToView (non-jittered)
            fd.projMatrixNoJitter = FrameViewRead().view.projMatrixNoJitter;
            fd.cameraPos          = FrameViewRead().camera.position;
            fd.jitterX            = ssgiView.jitter.GetJitterX();
            fd.jitterY            = ssgiView.jitter.GetJitterY();
            fd.nearZ              = FrameViewRead().camera.nearZ;
            fd.farZ               = FrameViewRead().camera.farZ;
            fd.frameIndex         = static_cast<uint32_t>(FrameViewRead().currentFrame);
            if (ssgiSsr)
            {
                fd.prevHdrSrv    = ssgiSsr->GetSceneColorPyramidSrv();
                const uint32_t mips = ssgiSsr->GetSceneColorPyramidMips();
                fd.prevHdrMaxMip = mips > 0 ? float(mips - 1) : 0.0f;
            }
            fd.ddgiVolumeBufSrv    = m_ddgiMgr.GetVolumeBufferSrv(m_gfx);
            fd.ddgiIrradianceTable = m_ddgiMgr.GetIrradianceTableGpu();
            fd.ddgiDepthTable      = m_ddgiMgr.GetDepthTableGpu();
            fd.ddgiProbeDataTable  = m_ddgiMgr.GetProbeDataTableGpu();
            // Mirror Lighting.ps's master gate exactly (toggle && count), as
            // the reflection path does: the raw count would keep stale DDGI
            // alive after the screen dropped it.
            fd.ddgiVolumeCount     = m_ddgiSettings.ddgiEnabled
                                   ? m_ddgiMgr.GetActiveVolumeCount() : 0u;
            fd.ddgiEnabled         = m_ddgiSettings.ddgiEnabled ? 1u : 0u;
            m_ssgiPass->SetFrameData(fd);
        }

        // Graph order is SSGI -> RTReflection. When both screen-space tiers
        // are live, the latter reads the pyramid SSGI just built and removes a
        // whole full-resolution Hi-Z construction from the frame. Passing 0
        // keeps reflections independent when SSGI is disabled. Both are the bound view's.
        if (m_rtReflectionPass)
            m_rtReflectionPass->SetExternalDepthHierarchy(
                (ssgiActive || giHiZOnly) ? m_ssgiPass->GetDepthHierarchySrv() : 0,
                (ssgiActive || giHiZOnly) ? m_ssgiPass->GetDepthHierarchyMipCount() : 0);

        // LightCB fold, written every frame including the inactive branch.
        // LightCB is a triple-buffered ring: a conditional write leaves the slot
        // holding an index from kFrameCount frames ago, which under Resource Binding
        // Tier 3 names a live-but-wrong descriptor with no D3D12 validation message.
        // The Tex2D null is the "no SSGI" value.
        if (auto* lb = m_lightCB.Current(m_gfx))
        {
            lb->ssgiResultIdx = ssgiHeapIdx;
            lb->ssgiIntensity = m_ddgiSettings.ssgiIntensity;
        }
    }
    else
    {
        if (auto* lb = m_lightCB.Current(m_gfx))
        {
            lb->ssgiResultIdx = BINDLESS_NULL_TEX2D_FLOAT;   // no SSGI
            lb->ssgiIntensity = 0.0f;
        }
        if (m_rtReflectionPass)
            m_rtReflectionPass->SetExternalDepthHierarchy(0, 0);
    }

    // ---- Phase 2b.9b: XeGTAO wiring (graph pass, graphics queue) ----
    // Screen-space AO in RTAO's graph slot (after Decal -> post-decal normals,
    // before Lighting). Everything it needs is settled here: the GBuffer SRVs were
    // captured before the workers kicked and the graph has not run, so the output
    // binds the same frame. From the plan's stage B (Renderer/FramePlan.h): the
    // AO source unless the path tracer or RTAO owns gSSAO - running both would
    // burn four full-res dispatches on a result nothing reads.
    // Every view's (the bound view's working set and history).
    const bool xegtaoActive = m_plan.xegtao;
    if (m_xegtaoPass)
    {
        m_xegtaoPass->SetActive(xegtaoActive);
        if (xegtaoActive)
        {
            m_xegtaoPass->SetQualityLevel(m_aoQuality);   // features.aoQuality
            m_xegtaoPass->SetDepthSrvHandle(depthSrv);
            m_xegtaoPass->SetNormalSrvHandle(normalSrv);
            // Velocity feeds AO-only temporal reprojection; falls back to "no history" if 0.
            m_xegtaoPass->SetVelocitySrvHandle(velocitySrv);
            // XeGTAO works on the GBuffer / depth at render res; required,
            // since at non-Native the depth buffer is render-sized.
            m_xegtaoPass->SetViewportSize(FrameViewRead().renderWidth, FrameViewRead().renderHeight);
            // Pass un-jittered projection so TAA jitter doesn't drift reconstruction. A secondary
            // view takes the main view's noise index (recorded before it this frame).
            static uint32_t s_gtaoFrame = 0;
            m_xegtaoPass->SetProjectionMatrix(
                FrameViewRead().view.projMatrixNoJitter, FrameViewRead().view.viewMatrix,
                primary ? s_gtaoFrame++ : s_gtaoFrame - 1);
            // Bind the ping-pong slot Execute is about to write, not the one it
            // last wrote: the write index is chosen at the end of the previous
            // Execute, so it is known before the dispatch records, which removes
            // the one-frame AO lag.
            if (m_lightingPass)
                m_lightingPass->SetSSAOHandle(m_xegtaoPass->GetUpcomingAOSrv());
        }
    }

    // ---- Folded-to-bindless SRV indices (t24..t45) ----
    // The graphics root signature is at D3D12's 64-DWORD limit, so these SRVs
    // have no descriptor table: their shader-visible heap index rides in LightCB
    // (RootSignatureLayout.h):
    //   batch 1  t45 RT sun-shadow mask, t44 RT reflection
    //   batch 2  t43 hair shadow map, t42 per-object shadow map,
    //            t41 point-light cube shadow atlas
    //   batch 3  t28 DDGI volume buffer (the t29..t40 block bases ride inside
    //            that buffer's elements, written by DDGIVolumeManager::Tick)
    //   batch 4  t25 per-cluster probe grid, t26 probe index list
    //   batch 5  t27 SSR trace result - read only by Water.ps, rebuilt on resize
    //   batch 6  t24 reflection-probe StructuredBuffer - a 3-slot UPLOAD ring,
    //            so its value differs frame to frame
    // Lighting.ps / shadow.hlsli / Water.ps open them with
    // ResourceDescriptorHeap[idx].
    // Written in one place, right before graph.Execute (where LightingPass
    // records):
    //   * every SetRTReflection / SetRTShadowMask call for this frame's read has
    //     happened (Phase 2b.7 / 2b.8), so index and resource agree. The
    //     FullFold legacy-SSR wiring after graph.Execute targets the next frame
    //     and reaches it through this same line.
    //   * the index is recomputed from the current handle every frame, never
    //     cached: resize recreates these textures and the texture free list
    //     recycles handle ids, so a cached index can name a stranger.
    // An absent resource -> LightingPass's real 1x1 fallback SRV (black for the
    // reflection, white for the mask), or for the batch-4 pair a type-correct
    // null StructuredBuffer SRV; no sentinel needed.
    // Batches 5 and 6 (ssrResultIdx, reflProbeBufIdx) are written in their own
    // blocks below: their handles come from WaterPass and ReflectionProbeManager,
    // not LightingPass, so they must not inherit this gate.
    if (m_lightingPass)
    {
        if (auto* lb = m_lightCB.Current(m_gfx))
        {
            // ~0u means both the live handle and LightingPass's 1x1 fallback
            // were 0, reachable only if the fallback texture failed at Init (it
            // logs). ResourceDescriptorHeap[] has no null index and no bounds
            // check, so ~0u would read far outside the heap and can remove the
            // device; index 0 is always a real descriptor (wrong contents, but
            // addressable). Report once and keep rendering rather than take a TDR.
            auto resolve = [&](uint64_t srv, const char* what) -> uint32_t
            {
                const uint32_t idx = m_gfx.DescriptorHeapIndexFromGpuHandle(srv);
                if (!ENGINE_CHECK_ONCE(idx != ~0u,
                        "LightCB bindless fold: no descriptor for %s "
                        "(live and fallback SRV both null) — clamping to heap "
                        "index 0", what))
                    return 0u;
                return idx;
            };
            // A typed handle (gen << 20 | index), checked against the heap's
            // record; Lighting.ps and custom_surface.hlsli open it with
            // bindless.hlsli's BindlessTex2D<float4>.
            // -bindless_reverse asks for the WRONG kind on purpose: the CPU
            // check refuses and hands over the TexCubeArray null, which the
            // shaders' debug lane must refuse in turn - pack bindless_validate,
            // reverse arm. Read once.
            static const bool s_bindlessReverse = Sys::CommandLine::Get().Has("-bindless_reverse");
            lb->rtReflectionIdx = s_bindlessReverse
                ? m_gfx.BindlessOf<RHI::Bindless::Kind::TexCubeArrayFloat>(
                      m_lightingPass->GetRTReflectionSrv(), "RT reflection (-bindless_reverse)").Bits()
                : m_gfx.BindlessOf<RHI::Bindless::Kind::Tex2DFloat>(
                      m_lightingPass->GetRTReflectionSrv(), "RT reflection").Bits();
            // The shadow family carries typed handles too (bindless.hlsli opens
            // them); each still names LightingPass's value fallback (white mask,
            // empty atlas / map) when its pass did not run - values, not
            // "absent".
            lb->rtShadowMaskIdx = m_gfx.BindlessOf<RHI::Bindless::Kind::Tex2DFloat>(
                                      m_lightingPass->GetRTShadowMaskSrv(), "RT shadow mask").Bits();
            // Batch 2, the shadow family (point / per-object / hair, below): same
            // contract and one-place rule - every SetPointShadowAtlas /
            // SetPerObjectShadowMap / SetHairShadowMap call for this frame has run,
            // and each accessor applies its fallback.
            // The ReSTIR / path-tracer results: typed handles; "not this frame" is
            // the null of each one's kind (the shaders gate on BindlessNone).
            lb->restirDiIdx    = BINDLESS_NULL_TEX2DARRAY_FLOAT;
            lb->ptResultIdx    = lb->restirGiIdx = BINDLESS_NULL_TEX2D_FLOAT;
            lb->renderPathMode = 0u;
            if (diActive)
            {
                lb->restirDiIdx = m_gfx.BindlessOf<RHI::Bindless::Kind::Tex2DArrayFloat>(
                    m_rtDiPass->GetUpcomingOutputSrv(), "ReSTIR_DI").Bits();
                lb->renderPathMode = static_cast<uint32_t>(GetRenderPath());
            }
            if (giActive)
            {
                lb->restirGiIdx = m_gfx.BindlessOf<RHI::Bindless::Kind::Tex2DFloat>(
                    m_rtGiPass->GetUpcomingOutputSrv(), "ReSTIR_GI").Bits();
                lb->renderPathMode = static_cast<uint32_t>(GetRenderPath());
            }
            if (ptActive)
            {
                lb->ptResultIdx = m_gfx.BindlessOf<RHI::Bindless::Kind::Tex2DFloat>(
                    m_pathTracePass->GetUpcomingOutputSrv(), "PathTrace").Bits();
                // The shaders' path-traced composite (gRenderPathMode 4) on
                // either path-tracing path.
                lb->renderPathMode = static_cast<uint32_t>(RenderPath::PathTrace);
            }
            // PT-lit toon receivers: the Tex2D null unless the path tracer lit them
            // this frame (a stale texture would light a character that moved).
            lb->nprPtAmbientIdx = m_ptNprActiveThisFrame
                ? m_gfx.BindlessOf<RHI::Bindless::Kind::Tex2DFloat>(m_pathTracePass->GetNprLightingSrv(),
                                                       "path-traced toon lighting").Bits()
                : BINDLESS_NULL_TEX2D_FLOAT;
            lb->pointShadowIdx  = m_gfx.BindlessOf<RHI::Bindless::Kind::TexCubeArrayFloat>(
                                      m_lightingPass->GetPointShadowAtlasSrv(),
                                      "point-light cube shadow atlas").Bits();
            lb->perObjShadowIdx = m_gfx.BindlessOf<RHI::Bindless::Kind::Tex2DFloat>(
                                      m_lightingPass->GetPerObjectShadowSrv(), "per-object shadow map").Bits();
            lb->hairShadowIdx   = m_gfx.BindlessOf<RHI::Bindless::Kind::Tex2DFloat>(
                                      m_lightingPass->GetHairShadowSrv(), "hair shadow map").Bits();
            // Batch 3, DDGI: only the entry buffer's index lives here; the three
            // kMaxVolumes-wide descriptor-block bases (t29..t40) are stamped into
            // every VolumeGPUDesc element by DDGIVolumeManager::Tick.
            // GetVolumeBufferSrv() returns the current frame's ring slot (the
            // buffer is triple-buffered UPLOAD), hence recomputed every frame.
            // A 0 handle is not an error: DDGIVolumeManager::Init is skipped
            // without DXR, ddgiEnabled / ddgiVolumeCount are 0 then and Lighting.ps
            // never opens it; the typed handle is the null of the buffer's stride,
            // not heap 0.
            const uint64_t ddgiVolSrv = m_ddgiMgr.GetVolumeBufferSrv(m_gfx);
            lb->ddgiVolumeBufIdx = m_gfx.BindlessOf<RHI::Bindless::Kind::StructuredBuffer>(ddgiVolSrv,
                                       "DDGI volume buffer", static_cast<uint32_t>(sizeof(DDGI::VolumeGPUDesc))).Bits();
            // Batch 4, ClusterPass's per-cluster reflection-probe list (t25 grid,
            // t26 index list): the one lifetime-stable pair (DEFAULT-heap buffers
            // created in ClusterPass::Init, never recreated), re-resolved anyway -
            // "never cache a heap index" is engine-wide, and it costs one division
            // each. Absent, the accessors return LightingPass's type-correct null
            // StructuredBuffer SRVs (stride 8 / 4), not the probe cube array: the
            // shader opens these descriptors, and a dimension mismatch there is UB
            // that Resource Binding Tier 3 does not report. Typed handles, the
            // strides checked (8 / 4).
            lb->reflProbeGridIdx  = m_gfx.BindlessOf<RHI::Bindless::Kind::StructuredBuffer>(m_lightingPass->GetReflProbeGridSrv(),
                                        "reflection probe cluster grid", 8u).Bits();
            lb->reflProbeIndexIdx = m_gfx.BindlessOf<RHI::Bindless::Kind::StructuredBuffer>(m_lightingPass->GetReflProbeIndexSrv(),
                                        "reflection probe cluster index list", 4u).Bits();
        }
    }

    // ---- Batch 5: the SSR trace result (t27) ----
    // Outside the `if (m_lightingPass)` block on purpose: it comes from WaterPass
    // and only Water.ps reads it. Gated on LightingPass, a missing LightingPass
    // would freeze ssrResultIdx at what this ring slot held kFrameSlots frames
    // ago - after a resize, a freed descriptor. (BeginFrame's SSR push is not
    // gated on m_lightingPass for the same reason.)
    // WaterPass owns the handle: GetSSRResultSrv() returns the live upsample SRV
    // or that pass's own 1x1 black Texture2D, never LightingPass's probe cube
    // array (the wrong dimension for the `Texture2D<float4>` the shader opens).
    // Its descriptor is not lifetime-stable (SSRSubsystem::OnResize recreates
    // SSRUpsamplePass::m_colorTex on a render-resolution change); only ordering
    // makes the index current: BeginFrame calls OnResize and pushes the new
    // handle into WaterPass, this block re-derives the index, and only then does
    // graph.Execute record WaterPass. Nothing in between recreates the texture or
    // caches the index.
    if (auto* lb = m_lightCB.Current(m_gfx))
    {
        // A typed handle: the live result or WaterPass's 1x1 black
        // fallback; the Tex2D null with no water pass at all.
        // A secondary view's is its own SSR chain's (resized in its segment), else the black stand-in.
        uint64_t viewSsrSrv = 0;
        if (!primary && m_ssrEnabled)
            if (SSRSubsystem* ssr = m_secondary[viewSlot].ssr.get()) viewSsrSrv = ssr->GetFinalColorSrv();
        lb->ssrResultIdx = m_waterPass
            ? m_gfx.BindlessOf<RHI::Bindless::Kind::Tex2DFloat>(
                  primary ? m_waterPass->GetSSRResultSrv()
                          : (viewSsrSrv ? viewSsrSrv : m_waterPass->GetNoSSRSrv()), "SSR result").Bits()
            : m_gfx.NullOf<RHI::Bindless::Kind::Tex2DFloat>().Bits();

        // ---- Volumetric cloud shadow ----
        // The SRV handle and the projection rows are read as a pair, here, before
        // graph.Execute: CloudPass bakes both at the end of its Execute, so this
        // publishes last frame's map with last frame's basis. Do not split the two
        // reads (CloudPass::Execute's ordering note): cloud shadows would slide
        // across the ground whenever the sun rotates. Absent is the Tex2D null
        // (cloud_shadow.hlsli returns full sun for it), not 0 - heap 0 is a live
        // table entry.
        uint32_t cloudShadowIdx = m_gfx.NullOf<RHI::Bindless::Kind::Tex2DFloat>().Bits();
        // LightCB is a ring, so an un-written row still holds an older frame's
        // basis. Both consumers gate on "no map" before touching it, but a stale
        // projection in the buffer is a landmine for whoever relaxes that gate:
        // zero it, the gate is belt-and-braces.
        std::memset(lb->cloudShadowRight, 0, sizeof(lb->cloudShadowRight));
        std::memset(lb->cloudShadowUp,    0, sizeof(lb->cloudShadowUp));
        if (m_cloudPass)
        {
            const uint64_t srv = m_cloudPass->GetCloudShadowSrvHandle();
            if (srv != 0)
            {
                // A slot that is not a Tex2D float is refused (and reported)
                // by the handle check, which then returns the null: cloud
                // shadows off this frame, the rows left zero.
                const auto h = m_gfx.BindlessOf<RHI::Bindless::Kind::Tex2DFloat>(srv, "cloud shadow map");
                if (h.Index() != BINDLESS_NULL_TEX2D_FLOAT)
                {
                    cloudShadowIdx = h.Bits();
                    m_cloudPass->GetCloudShadowRows(lb->cloudShadowRight,
                                                    lb->cloudShadowUp);
                }
            }
        }
        // The null = unbound; SampleCloudShadow returns full sun, so a frame
        // with no clouds never inherits the previous scene's overcast.
        lb->cloudShadowIdx = cloudShadowIdx;

        // The volumetric raymarch reads these fields: it opens this frame's
        // LightCB from the heap (a compute signature has no root CBV for it), so
        // the shafts and the lit surfaces cannot disagree about where the shadows
        // are. The render slot's buffer, the one `lb` points into.
        if (m_volFogPass)
        {
            // A typed CBV handle; no CBV -> the CBV null.
            const uint32_t lightCbv = m_gfx.Cbv(m_lightCB.CurrentBuffer(m_gfx),
                                                "LightCB for the volumetric raymarch").Bits();
            ENGINE_CHECK_ONCE(lightCbv != BINDLESS_NULL_CBV,
                "LightCB has no CBV in the heap - the volumetric light shafts draw "
                "without cloud shadows");
            m_volFogPass->SetLightCB(lightCbv);
        }

        // ---- Forward translucent traced-reflection fold ----
        // custom_surface.hlsli's translucent main (MS_FetchTracedReflection) reuses
        // the same-frame RTR output named by rtReflectionIdx for a transparent pixel
        // when the opaque surface behind it is near-coplanar and close. Those tests
        // read the deferred GBuffer1 normal and the SSRTraceDepth snapshot, opened
        // bindless from these indices; both are graph physical textures a resize
        // recreates, so like every folded index they are re-derived every frame,
        // never cached. TransparentPass declares ReadSRV on both handles, which
        // guarantees their state when the draws record; this block only names them.
        // Without a TransparentPass both are the Tex2D null and the shader skips
        // the fold.
        {
            // FogApplyCB's depth index stays a raw heap index (0 = none):
            // fog_apply.hlsli reads it raw, not as a typed handle.
            auto graphTexIdx = [&](RG::RGTextureHandle h) -> uint32_t
            {
                const RHI::Texture* t = m_graph.GetPhysicalTexture(h);
                const uint64_t srv = t ? m_gfx.GetTextureSRVGpuHandle(*t) : 0;
                if (!srv) return 0u;
                const uint32_t idx = m_gfx.DescriptorHeapIndexFromGpuHandle(srv);
                return (idx == ~0u) ? 0u : idx;
            };
            // The LightCB fields carry typed handles; a graph texture that is
            // not there is the Tex2D null, which custom_surface.hlsli's gates
            // test with BindlessNone.
            const uint32_t kNoTex = m_gfx.NullOf<RHI::Bindless::Kind::Tex2DFloat>().Bits();
            auto graphTexHandle = [&](RG::RGTextureHandle h, const char* what) -> uint32_t
            {
                const RHI::Texture* t = m_graph.GetPhysicalTexture(h);
                return m_gfx.BindlessOf<RHI::Bindless::Kind::Tex2DFloat>(
                    t ? m_gfx.GetTextureSRVGpuHandle(*t) : 0, what).Bits();
            };
            const bool haveForward = m_transparentPass != nullptr;
            const bool fwdRefl = haveForward &&
                m_transparentReflectionsEnabled.load(std::memory_order_relaxed);
            // Named whenever the forward pass exists, NOT only with the
            // reflection toggle: the depth snapshot also drives the forward
            // BRDF's opaque-proximity gate for the RT sun mask / screen AO
            // folds and the front-layer prepass's occlusion test. The
            // reflection fold itself is still gated by fwdReflStrength.
            lb->fwdReflNormalIdx = haveForward
                ? graphTexHandle(m_normalHandle, "forward reflection GBuffer normal") : kNoTex;
            lb->fwdReflDepthIdx  = haveForward
                ? graphTexHandle(m_ssrTraceDepthHandle, "forward reflection depth snapshot") : kNoTex;
            // The fog and the soft-fade depth for everything drawn after the
            // fog composites: after the frame's view-mode gates (the path
            // tracer's included), before the graph records.
            WriteFogApplyCB(*lb, graphTexIdx(m_ssrTraceDepthHandle));
            const bool addressable = lb->fwdReflNormalIdx != kNoTex && lb->fwdReflDepthIdx != kNoTex;
            // A graph texture whose SRV is not in the shader-visible heap would
            // make the fold silently do nothing forever; say so once instead.
            if (fwdRefl)
                (void)ENGINE_CHECK_ONCE(addressable,
                    "transparent reflections: GBuffer1 normal / SSRTraceDepth "
                    "descriptor is not addressable from the shader-visible heap "
                    "(normal %u, depth %u) — forward traced-reflection fold "
                    "disabled", lb->fwdReflNormalIdx, lb->fwdReflDepthIdx);
            // DX12_FWDREFL_DEBUG=<mode> makes the weight negative: the shader reads
            // the magnitude as the mode and the sign as "paint instead of shading".
            // Mode 1 paints the fold weights (red = folded, green = a traced signal
            // the coplanar / proximity gate rejected, black = no traced signal); mode
            // 2 the folded radiance itself, the only way to eyeball a trace whose
            // Fresnel-weighted contribution is below one 8-bit step in a dark room;
            // mode 3 the front-layer tier's weight, mode 4 its radiance (the pane's
            // own RayQuery trace; custom_surface.hlsli). Read once, like
            // DX12_VERIFY_MATSNAP.
            static const int s_fwdReflDebug = []{
                char* b = nullptr; size_t n = 0; int mode = 0;
                if (_dupenv_s(&b, &n, "DX12_FWDREFL_DEBUG") == 0 && b) { mode = std::atoi(b); std::free(b); }
                return (mode < 0) ? 0 : (mode > 6 ? 6 : mode);
            }();
            lb->fwdReflStrength  = (fwdRefl && addressable)
                                 ? (s_fwdReflDebug ? -float(s_fwdReflDebug) : 1.0f) : 0.0f;

            // The translucent surface's own screen-space march needs last frame's
            // scene-colour pyramid. Gate on "built last frame"
            // (m_sceneColorPyramidBuiltLastFrame), not m_ssrEnabled: the post-graph
            // chain rebuilds the pyramid for SSR, the unified pass and SSGI alike
            // (pyramidWanted), so RTR-only / SSGI-only frames get the march, while a
            // pyramid not rebuilt since SSR went off - SRV still non-zero, content
            // frozen - is never named (it would paint stale reflections onto glass).
            // Orthographic cameras (editor split viewports) are excluded: the shader
            // linearises depth with this camera's perspective planes, as every other
            // screen-tier consumer does; the coplanar reuse needs no gate, RTR is
            // already off for an orthographic view (rtConf 0). The Tex2D null = none:
            // the shader skips the march and the coplanar reuse still applies.
            // The bound view's pyramid (a secondary view's own SSR chain).
            uint32_t prevHdrIdx = kNoTex;
            uint32_t prevHdrMips = 0u;
            SSRSubsystem* fwdSsr = RecordingSSR();
            const bool pyramidBuiltLastFrame =
                primary ? m_sceneColorPyramidBuiltLastFrame : m_secondary[viewSlot].pyramidBuiltLastFrame;
            if (fwdRefl && addressable && fwdSsr && pyramidBuiltLastFrame &&
                !FrameViewRead().camera.orthographic)
            {
                const uint64_t srv = fwdSsr->GetSceneColorPyramidSrv();
                const uint32_t h = m_gfx.BindlessOf<RHI::Bindless::Kind::Tex2DFloat>(srv, "forward reflection scene-colour pyramid").Bits();
                if (h != kNoTex) { prevHdrIdx = h; prevHdrMips = fwdSsr->GetSceneColorPyramidMips(); }
            }
            lb->fwdReflPrevHdrIdx    = prevHdrIdx;
            lb->fwdReflPrevHdrMaxMip = prevHdrMips ? float(prevHdrMips - 1u) : 0.0f;
            lb->fwdReflNearZ         = FrameViewRead().camera.nearZ;
            lb->fwdReflFarZ          = FrameViewRead().camera.farZ;
            lb->fwdReflRoughMax      = m_rtrMaxRoughness;
            lb->fwdReflRayMax        = m_rtrRayTMax;
            // Last frame's jitter, converted from pixels to UV. The pyramid the
            // march samples was rasterised at that offset; the unjittered
            // prevViewProj it reprojects through does not know about it.
            {
                const float rw = static_cast<float>(FrameViewRead().renderWidth);
                const float rh = static_cast<float>(FrameViewRead().renderHeight);
                lb->fwdReflPrevJitterUV[0] = rw > 0.0f ? RecordingViewState().jitter.GetPrevJitterX() / rw : 0.0f;
                lb->fwdReflPrevJitterUV[1] = rh > 0.0f ? RecordingViewState().jitter.GetPrevJitterY() / rh : 0.0f;
            }
            // ---- The front-layer trace (RTReflectionPass's second instance) ----
            // Its denoised output is a pass-owned committed texture (like the opaque
            // instance's, which LightingPass never declares either), so the index comes
            // from the pass's stable SRV; the front-layer depth is a graph texture,
            // named like the two above. Both are the Tex2D null unless the instance ran
            // this frame: a stale texture would be a stale reflection on a pane no
            // longer there. Written unconditionally: LightCB is a ring, a skipped write
            // strands what this slot held kFrameSlots frames ago.
            // The path-trace path traces the pane in PathTracePass instead
            // (FrontReflection, accumulated with the image): same contract, same stamp.
            // The traced front layer is the bound view's (its working set, its stamp); the path tracer's
            // the main view's.
            uint32_t frontRtrIdx = kNoTex, frontDepthIdx = kNoTex;
            const bool rtFront = m_frontLayerActiveThisFrame && m_rtReflectionFrontPass;
            const bool ptFront = primary && m_ptFrontActiveThisFrame && m_pathTracePass;
            if (fwdRefl && addressable && (rtFront || ptFront))
            {
                const uint64_t srv = rtFront ? m_rtReflectionFrontPass->GetUpcomingOutputSrv()
                                             : m_pathTracePass->GetFrontReflectionSrv();
                frontRtrIdx   = m_gfx.BindlessOf<RHI::Bindless::Kind::Tex2DFloat>(srv, "front-layer reflection").Bits();
                frontDepthIdx = graphTexHandle(m_translucentDepthHandle, "front-layer depth");
                if (frontRtrIdx == kNoTex || frontDepthIdx == kNoTex)
                    frontRtrIdx = frontDepthIdx = kNoTex;   // both or neither
            }
            lb->fwdReflFrontRtrIdx   = frontRtrIdx;
            lb->fwdReflFrontDepthIdx = frontDepthIdx;
            // ---- Screen AO for the forward BRDF ----
            // The same texture LightingPass was handed this frame (RTAO's output from
            // Phase 2b.9 or XeGTAO's aoFinal from 2b.9b, both set before this block);
            // the Tex2D null = no AO source. A pass-owned SRV, self-barriered by its
            // producer and parked SRV-readable through LightingPass; the forward draws
            // come later in the same graph. Written every frame.
            {
                const uint64_t aoSrv = (haveForward && m_lightingPass)
                                     ? m_lightingPass->GetSSAOHandle() : 0;
                lb->fwdAoIdx = m_gfx.BindlessOf<RHI::Bindless::Kind::Tex2DFloat>(aoSrv, "forward screen AO").Bits();
            }
        }

        // ---- Transmission ----
        // Whether a render feature refracted and tinted the background under this frame's
        // Transmissive surfaces before their colour pass (ITransmissionComposite, set at the
        // graph stages' prepare). Written every frame: LightCB is a ring.
        {
            bool applied = false;
            m_features.ForEach<ITransmissionComposite>(
                [&](const ITransmissionComposite& c) { applied = applied || c.TransmissionApplied(); });
            lb->transmissionApplied = applied ? 1u : 0u;
        }

        // ---- Water domain: scene depth + refraction source ----
        // water_common.hlsli opens both through ResourceDescriptorHeap[], so it
        // needs the bindless index (RHI::Texture::handleId), not a descriptor
        // handle. 0 means "unbound" and every helper tests for it before sampling -
        // a real index 0 would silently alias whatever occupies heap slot 0.
        // SSRTraceDepth is the depth snapshot taken after terrain + meshes and
        // before GrassPass; created with isSRV, it parks in DEPTH_READ_SRV, which
        // lets water depth-test the main depth buffer while sampling this copy.
        // ReflectionSource_NoTrace is the lit opaque scene used for refraction. Its
        // SRV exists only because WaterSurfacePass::Setup declares ReadSRV on it;
        // without that declaration this resolves to a stale / absent descriptor
        // with no error.
        lb->waterGlobals[0] = 0u;
        lb->waterGlobals[1] = 0u;
        lb->waterGlobals[2] = 0u;   // ripple body count
        lb->waterGlobals[3] = 0u;
        if (m_waterSurfacePass && m_waterSurfacePass->HasWater())
        {
            if (const RHI::Texture* t = m_graph.GetPhysicalTexture(m_reflectionSourceHandle))
                if (t->handleId != ~0u) lb->waterGlobals[0] = t->handleId;
            if (const RHI::Texture* t = m_graph.GetPhysicalTexture(m_ssrTraceDepthHandle))
                if (t->handleId != ~0u) lb->waterGlobals[1] = t->handleId;
        }
        // Ripple bodies. Located by WORLD-XZ CONTAINMENT in the shader rather
        // than by any per-draw identity, because DrawPacket carries no entity
        // and two lakes sharing one material merge into a single packet.
        // Row layout must match light_cb.hlsli gWaterBodies exactly.
        {
            const auto& bodies = m_waterRippleSystem
                ? m_waterRippleSystem->GetActiveBodies()
                : std::vector<WaterRippleSystem::ActiveBody>{};
            const uint32_t n = std::min<uint32_t>(
                static_cast<uint32_t>(bodies.size()),
                RHI::Slots::BindlessIdx::kLightCB_MaxWaterBodies);
            for (uint32_t i = 0; i < n; ++i)
            {
                const auto& b = bodies[i];
                lb->waterBodies[i * 2 + 0][0] = b.bodyOriginX;
                lb->waterBodies[i * 2 + 0][1] = b.bodyOriginZ;
                lb->waterBodies[i * 2 + 0][2] = b.bodySize;
                lb->waterBodies[i * 2 + 0][3] = b.level;
                lb->waterBodies[i * 2 + 1][0] = b.winOriginX;
                lb->waterBodies[i * 2 + 1][1] = b.winOriginZ;
                lb->waterBodies[i * 2 + 1][2] = b.winSize;
                // asfloat() on the shader side: the handle travels through a
                // float slot, not converted. The field's bindless-table slot
                // (handleId) as a typed handle with that slot's generation; no
                // field this frame -> the Tex2D null.
                const uint32_t ripple = b.bindlessIndex
                    ? m_gfx.MakeBindlessHandle(b.bindlessIndex, RHI::Bindless::Kind::Tex2DFloat, 0,
                                               "water ripple field")
                    : BINDLESS_NULL_TEX2D_FLOAT;
                std::memcpy(&lb->waterBodies[i * 2 + 1][3], &ripple, sizeof(uint32_t));
            }
            lb->waterGlobals[2] = n;
        }

        // Render resolution, not display: they differ whenever FSR2 upscales,
        // and the water helpers turn SV_Position into a UV with this.
        lb->waterRTSize[0] = static_cast<float>(FrameViewRead().renderWidth);
        lb->waterRTSize[1] = static_cast<float>(FrameViewRead().renderHeight);
        lb->waterRTSize[2] = 0.0f;
        lb->waterRTSize[3] = 0.0f;

        // ---- Batch 6: the reflection-probe buffer (t24) ----
        // Also outside `if (m_lightingPass)`, for a sharper reason than batch 5's:
        // the resource is a 3-slot UPLOAD ring whose SRV follows
        // m_gfx.GetFrameIndex(), so its value differs on consecutive frames.
        // LightCB is a ring too, so a frame that skipped this write would leave the
        // index of kFrameSlots frames ago - a valid descriptor naming the wrong
        // probe ring slot, data the GPU may be rewriting, silently. Write it every
        // frame. TransparentPass reads the same field (LightCB at b2 on its draws),
        // another reason not to gate on the lighting pass.
        // The source is ReflectionProbeManager itself, never a handle cached in a
        // pass. LightingPass only supplies the absent descriptor: a type-correct
        // null StructuredBuffer of stride 192, because the shader opens this as
        // StructuredBuffer<ReflectionProbe> and a cube-array descriptor there is UB
        // that Tier 3 does not report.
        uint64_t probeBufSrv = m_probeMgr.GetBufferSrv(m_gfx);
        if (!probeBufSrv && m_lightingPass)
            probeBufSrv = m_lightingPass->GetReflProbeBufferNullSrv();
        // A typed handle, the stride checked; neither the ring nor
        // LightingPass's null -> the null of the stride, not heap 0.
        lb->reflProbeBufIdx = m_gfx.BindlessOf<RHI::Bindless::Kind::StructuredBuffer>(probeBufSrv,
                                  "reflection probe buffer", static_cast<uint32_t>(sizeof(GpuShared::ReflectionProbe))).Bits();
    }

    // The second prepare point, right before the graph forks: the surface
    // cache's lazy allocation (it settles SurfaceCacheBake's IsActive, which the
    // graph queries in its serial phase) and NPR's lit-scene snapshot (the
    // NPRLitSnapshot lambda and the post-chain wiring read the ready flag it
    // arms).
    {
        // The passes' deferred resources are created here the first time a
        // frame needs them - e.g. the material surface cache's atlases when the
        // first NPR material appears.
        PROF_SCOPE("RS.PrepareLate");
        m_passes.Prepare(RG::PrepareStage::FrameLate, PrepareContextNow());
    }

    // Whether DDGI's list samples the specular cube's async copy this frame.
    // Without one, SkyIBLPass refreshes the copy in its own list (promoteCL,
    // which does it otherwise, exists only beside ddgiCL).
    if (primary && m_skyIBLPass)
        m_skyIBLPass->SetAsyncReaderActive(ddgiCL.IsValid());

    // Preview targets + the main-thread-only ToneMapPass setters, for the
    // frame-late prepare's reason: this is the device-frame thread in a serial
    // window, and CreateTexture / CreatePipelineState touch the descriptor-heap
    // allocator that the graph's serial Init / OnCompile phases keep off worker
    // threads.
    if (primary && m_preview) m_preview->PrepareFrame();

    _rzMark(7);

    // ---- Phase 2b: Main-thread color passes (concurrent with Worker 0).
    RHI::CommandList colorLastCL{};
#ifdef TRACY_ENABLE
    renderStage.reset(); // lexical zones must end before coroutine suspension
#endif
    if (primary) m_graph.SetSnapshotFrame(FrameViewRead().currentFrame);   // what a snapshot is stamped with
    // The HDR scene colour for this Execute: slot 0 (the main graph runs
    // outside every HDR push scope) and the frame's clear value.
    if (m_hdrHandle.IsValid())
        m_graph.BindImport(m_hdrHandle,
                           m_gfx.GetHdrTexture(primary ? 0u : GraphicsDX12::kFirstViewHdrSlot + viewSlot - 1),
                           FrameViewRead().hdrClearColor);
    // The SSR scene pyramid and result the SSR group left after the last
    // graph - what RTReflection, SSGI and Water read as last frame's (the bound view's chain).
    if (m_ssrPyramidHandle.IsValid())
    {
        SSRSubsystem* ssr = RecordingSSR();
        const RHI::Texture* pyramid = ssr ? ssr->SceneColorPyramidTexture() : nullptr;
        const RHI::Texture* result  = ssr ? ssr->FinalColorTexture() : nullptr;
        m_graph.BindImport(m_ssrPyramidHandle, pyramid ? *pyramid : RHI::Texture{}, nullptr);
        m_graph.BindImport(m_ssrResultHandle,  result  ? *result  : RHI::Texture{}, nullptr);
    }
    // A secondary view's graph after the main view's frame (its per-object and local-light shadow maps,
    // the probes): with no edge, EndFrame's sort may submit the view's independent lists first.
    if (!primary && m_viewAfter.IsValid()) m_graph.SetExecuteAfter(m_viewAfter);
    co_await m_graph.ExecuteAsync(m_gfx, m_clearColor, colorLastCL).Start(owner, token, "RenderGraph::Execute");
#ifdef TRACY_ENABLE
    renderStage.emplace(&kRenderStages[8], true);
#endif
    if (primary) LogMeshletFrame();   // the GBuffer pass finished recording this frame

    // ---- DDGI vs the surface-cache bake ----
    // DDGI's hits sample the material surface-cache atlases. The pages it can
    // reach are never the ones being baked - a page is published
    // kPublishLagFrames after its bake, un-published before a re-bake, and an
    // evicted page belongs to a key not drawn this frame, whose entities lose
    // their binding; DDGISceneAS::Gather runs after AcquirePage. But a bake
    // moves BOTH atlases, whole, to UAV and back on the graphics queue, while
    // DDGI may be sampling other pages on the compute queue: a resource-wide
    // state change under another queue's reads. So on a frame the bake wrote
    // them (startup, new custom-shader materials; none in steady state) DDGI
    // waits for it - the bake is pass #1, right after GBuffer, and nothing it
    // waits on waits on DDGI. -ddgibakeoverlap: no wait, the two overlap.
    if (primary && m_surfaceCacheBakePass)
    {
        const RHI::CommandList bakeCL = m_surfaceCacheBakePass->TakeAtlasWriteList();
        static const bool s_bakeOverlap = Sys::CommandLine::Get().Has("-ddgibakeoverlap");
        if (bakeCL.IsValid() && ddgiCL.IsValid() && !s_bakeOverlap)
        {
            m_gfx.AddCommandListDependency(ddgiCL, bakeCL);
            static uint32_t s_bakeWaits = 0;
            if (++s_bakeWaits <= 3)
                LOG_INFO("[DDGI_BAKEWAIT] frame=%llu DDGI waits for the surface-cache bake (%u page(s)); "
                         "-ddgibakeoverlap does not",
                         static_cast<unsigned long long>(FrameViewRead().currentFrame),
                         m_surfaceCacheBakePass->GetLastBakeCount());
        }
    }

    if (!ddgiCL.IsValid() && ddgiLogTick)
    {
        // Identify which gate tripped (only with the ddgiLogTick debug switch on).
        if (!m_ddgiReady)
            LOG_INFO("DDGI: skipped — m_ddgiReady=false (DXR unsupported or pass init failed)");
        else if (!m_ddgiPass)
            LOG_INFO("DDGI: skipped — m_ddgiPass null");
        else if (m_ddgiMgr.GetActiveVolumeCount() == 0)
            LOG_INFO("DDGI: skipped — 0 active volumes (add a DDGIVolumeComponent to an entity)");
        else if (!m_lastWorld)
            LOG_INFO("DDGI: skipped — m_lastWorld null (BeginFrame not called yet?)");
    }

    _rzMark(8);

    // ---- Phase 3: Wait for Worker 0 ----
    {
        // The Tracy stage zone ends before the co_await (a lexical zone must not
        // span a suspension); RenderCPU.ShadowJoin resumes after it.
#ifdef TRACY_ENABLE
        renderStage.reset();
#endif
        if (primary || viewCsm)
        {
            co_await m_renderWorkers[0].Completion();
            m_renderWorkers[0].Join(); // ready-only, consume the completed slot
        }
#ifdef TRACY_ENABLE
        renderStage.emplace(&kRenderStages[9], true);
#endif

    }

    // The CPU only needs completed skin recording before submission and before
    // probe work can reconfigure render inputs. GPU consumers already follow
    // allocation order or the explicit scene-AS -> m_frameSkinCL dependency.
    // RecordingDrain covers exceptions from every earlier owner stage; this
    // normal join propagates worker exceptions before any command list closes.
    {
#ifdef TRACY_ENABLE
        renderStage.reset();
#endif
        if (primary)
        {
            co_await m_renderWorkers[1].Completion();
            m_renderWorkers[1].Join(); // ready-only, consume the completed slot
        }
#ifdef TRACY_ENABLE
        renderStage.emplace(&kRenderStages[9], true);
#endif
    }

    // ---- Phase 4: Wire shadow -> color GPU dependency ----
    if (colorLastCL.IsValid() && shadowCL.IsValid())
        m_gfx.AddCommandListDependency(colorLastCL, shadowCL);

    // ---- Phase 4.2: Pop one probe off the bake queue; no-op when steady-state.
    {
        // Record half only: the probe arrived in m_probeBakeReq
        // (ExtractProbeBakeRequest, game side); the result goes back through
        // m_probeBakeDone.
        if (primary) ProcessProbeBakeQueue(colorLastCL);
    }

    // ---- Phase 4.3: material preview view ----
    // After the graph's Execute has returned, which puts the device HDR slot
    // hand-over outside every graph Phase B window.
    // colorLastCL is deliberately not reassigned. The preview list depends on
    // colorLastCL (it consumes the graph's output), but the chain must not
    // depend on the preview: colorLastCL feeds Hi-Z, SSR, the DDGI debug list
    // and preComputeCL, and putting those behind a preview panel is a real GPU
    // stall. The list is parked on the Renderer and
    // FramePipeline::RenderHalfAsync gives it its own
    // AddCommandListDependency(primaryCL, previewCL) edge, which keeps it
    // visible to ImGui the same frame. A null preview layer parks an invalid
    // handle.
    if (primary) ProcessPreviewQueue(colorLastCL);

    _rzMark(9);

    // ==== Post-graph GBuffer_Depth chain - declared, not inherited ====
    // Past this point Hi-Z, the SSR trace, the SSR composite, the DDGI
    // probe-debug list and the pre-compute transition list read or write
    // GBuffer_Depth, and each bakes a barrier's StateBefore from the graph
    // tracker at record time: Hi-Z moves the depth and writes the tracker back,
    // and every list recorded after it reads the new value. That is correct only
    // if the GPU executes them in recording order, so a running depthLastCL is
    // threaded through every depth toucher: the edges say what the barriers
    // assume, and they are exactly the order EndFrame's pool-index tie-break
    // would produce. A slip shows as a rare debug-layer mismatch
    //   Before state (0x10: DEPTH_WRITE) of 'GBuffer_Depth' does not match
    //   the state (0xE0: DEPTH_READ|NON_PIXEL_SRV|PIXEL_SRV)
    // (a list recorded after Hi-Z executing before it). GBufferPass declares its
    // chunk lists' chain the same way.
    // Phase 7's restoreCL is the sixth toucher; it is already ordered behind
    // preComputeCL through computeCL, so it needs no edge of its own.
    RHI::CommandList depthLastCL = colorLastCL;
    auto chainDepth = [&](RHI::CommandList cl)
    {
        if (!cl.IsValid()) return;
        // Skip the self-edge and the duplicate: when depthLastCL is still the
        // graph tail, the caller's own colorLastCL edge already expresses this.
        if (depthLastCL.IsValid() && !(depthLastCL == cl))
            m_gfx.AddCommandListDependency(cl, depthLastCL);
        depthLastCL = cl;
    };

    // ---- Phase 4.5: Hi-Z mip chain (for next frame's occlusion culling) ----
    // Consumed by CullingPass at the start of the NEXT frame (m_mainView.hiz.framesValid
    // gates the read so a skipped generation frame disables the test instead
    // of sampling a stale pyramid).
    bool hiZGeneratedThisFrame = false;
    if (primary && m_gpuCullingEnabled && m_hiZEnabled && m_hiZPass)
    {
        const RHI::Texture* depthTex = m_graph.GetPhysicalTexture(m_depthHandle);
        if (depthTex)
        {
            // The pyramid at the depth's size, the device's render size
            // (HiZPass::OnPrepare).
            RG::PrepareContext hiz;
            hiz.gfx = &m_gfx;
            hiz.renderW = m_gfx.GetRenderWidth();
            hiz.renderH = m_gfx.GetRenderHeight();
            m_passes.Prepare(RG::PrepareStage::HiZ, hiz);

            RHI::CommandList hizCL = m_gfx.BeginCommandList(RHI::QueueType::GRAPHICS);
            m_gfx.SetCommandListName(hizCL, "HiZ");
            hizCL.gfx = &m_gfx;
            GfxTrace::Label(hizCL, "HiZ");
            uint32_t hizRegion = m_gfx.BeginGPUTimestamp(hizCL, "HiZ");

            // Transition depth from the graph-tracked state. Hi-Z reads the depth
            // plane from a compute dispatch, so a state holding
            // NON_PIXEL_SHADER_RESOURCE (DEPTH_READ_SRV, the graph's usual exit state,
            // does) serves as it is; any other moves the depth plane alone to
            // DEPTH_READ_SRV. Nothing goes back to DEPTHSTENCIL: the next depth user
            // moves it from the book. -rgplaneoff: a whole-texture round trip through
            // SHADER_RESOURCE.
            const bool planes = RG::RenderGraph::PlanesEnabled();
            RHI::ResourceState depthState = m_graph.GetTextureState(m_depthHandle);
            if (!planes)
                m_gfx.PushBarrier(
                    RHI::GPUBarrier::Image(depthTex,
                        depthState,
                        RHI::ResourceState::SHADER_RESOURCE), hizCL);
            else if (depthState != RHI::ResourceState::DEPTH_READ_SRV &&
                     depthState != RHI::ResourceState::SHADER_RESOURCE &&
                     depthState != RHI::ResourceState::SHADER_RESOURCE_COMPUTE)
                m_graph.MovePlanes(m_gfx, hizCL, m_depthHandle, RHI::ResourceState::DEPTH_READ_SRV,
                                   RHI::ResourceState::UNDEFINED);

            uint64_t depthSrv = m_gfx.GetTextureSRVGpuHandle(*depthTex);
            const bool hizRecorded = m_hiZPass->Execute(hizCL, depthSrv);

            // Transition back and update graph's state tracking (-rgplaneoff).
            if (!planes)
            {
                m_gfx.PushBarrier(
                    RHI::GPUBarrier::Image(depthTex,
                        RHI::ResourceState::SHADER_RESOURCE,
                        RHI::ResourceState::DEPTHSTENCIL), hizCL);
                m_graph.SetTextureState(m_depthHandle, RHI::ResourceState::DEPTHSTENCIL);
            }

            m_gfx.EndGPUTimestamp(hizCL, hizRegion);

            // Head of the post-graph depth chain. depthLastCL is still the
            // graph tail here, so this is the colorLastCL edge; from here on
            // every later depth toucher is ordered behind this list, which makes
            // the tracker write above safe to read.
            chainDepth(hizCL);

            // Pair the pyramid with the camera that produced its depth: the
            // next frame's cull projects with exactly this VP and gates on
            // motion relative to this pose. Only when Execute recorded the chain:
            // an early-out (invalid PSO) must not bump m_mainView.hiz.framesValid,
            // or the cull consumes a never-generated pyramid.
            if (hizRecorded)
            {
                m_mainView.hiz.viewProj = FrameViewRead().view.viewProjMatrixNoJitter;
                m_mainView.hiz.camPos   = FrameViewRead().camera.position;
                m_mainView.hiz.camFwd   = FrameViewRead().view.cameraForward;
            }
            hiZGeneratedThisFrame = hizRecorded;
        }
    }
    m_mainView.hiz.framesValid = hiZGeneratedThisFrame
                     ? std::min<uint32_t>(m_mainView.hiz.framesValid + 1, 1000u)
                     : 0u;

    // ---- Phase 4.6 + 4.7: legacy SSR chain (mode-gated) ----
    // The scene-colour pyramid feeds the screen tier's march and the RT tier's
    // hit-point screen reuse, which are independent, so it builds whenever
    // either is live. Gated on the SSR toggle alone, RTR-only mode would shade
    // every hit through the analytic proxy - the worst case for custom-shader /
    // NPR content, whose real look exists only in shaded pixels.
    // The full march / denoise chain runs only for water frames (WaterPass
    // stamps its GBuffer normal / roughness after the unified pass ran, so water
    // needs the late chain; the composite is modulated by the unified conf) or
    // as the whole-screen fallback without the unified pass (no DXR). The first
    // frame after SSR is re-enabled reuses the last pyramid built before the
    // toggle-off (parked in SHADER_RESOURCE); the reuse's depth validation
    // limits the stale window to a frame or two.
    // SSGI must be in this gate: the pyramid is its only radiance source, and
    // without it every ray resolves to "no history" and the pass silently
    // contributes nothing (the far field still makes the frame look plausible).
    // Under the path tracer the chain runs only for what reads it - water, toon
    // specular, the translucents' screen march (FramePlan::Decisions::ssrChain;
    // always true off the path-tracing path).
    const bool pyramidWanted = (m_ssrEnabled || unifiedReflActive || ssgiActive) && m_plan.ssrChain;
    // The bound view's chain: a secondary view records its own.
    SSRSubsystem* const ssrChain = RecordingSSR();
    // Consumed by next frame's LightCB fold (the translucent screen march):
    // the pyramid is a valid prev-frame source only if this block ran.
    // Set from the block's own condition, so it reads "built" whenever the block
    // runs, also on a frame SSRSubsystem::Render returns early.
    const bool pyramidBuilt = ssrChain && m_viewMode != ViewMode::Wireframe && pyramidWanted;
    if (primary)
        m_sceneColorPyramidBuiltLastFrame = pyramidBuilt;
    else
        m_secondary[viewSlot].pyramidBuiltLastFrame = pyramidBuilt;
    if (pyramidBuilt)
    {
        // EITHER water path counts. Both stamp the GBuffer after the unified
        // reflection pass ran, so their pixels have no traced reflection and
        // still need the legacy screen-space chain. Missing the mesh-driven
        // path here would silently kill reflections on Water-domain materials.
        const bool waterActive =
            (m_waterPass        && m_waterPass->HasWater()) ||
            (m_waterSurfacePass && m_waterSurfacePass->HasWater());
        SSRSubsystem::LegacyMode mode = SSRSubsystem::LegacyMode::PyramidOnly;
        if (m_ssrEnabled && !unifiedReflActive)
            mode = SSRSubsystem::LegacyMode::FullFold;
        else if (m_ssrEnabled && waterActive)
            mode = SSRSubsystem::LegacyMode::FullAdditive;

        SSRSubsystem::FrameContext ctx{};
        ctx.graph                = &m_graph;
        ctx.albedoHandle         = m_albedoHandle;
        ctx.normalHandle         = m_normalHandle;
        ctx.surfaceHandle        = m_surfaceHandle;
        ctx.depthHandle          = m_depthHandle;
        ctx.velocityHandle       = m_velocityHandle;
        ctx.traceDepthHandle     = m_ssrTraceDepthHandle;   // grass-free Hi-Z source
        ctx.reflectionSourceHandle = m_reflectionSourceHandle;
        ctx.viewProj             = FrameViewRead().view.viewProjMatrix;
        ctx.prevViewProjJittered = RecordingViewState().jitter.GetPrevViewProjJittered();
        ctx.cameraPos            = FrameViewRead().camera.position;
        ctx.nearZ                = FrameViewRead().camera.nearZ;
        ctx.farZ                 = FrameViewRead().camera.farZ;
        ctx.renderFrame          = FrameViewRead().currentFrame;
        ctx.brdfLutSrv           = m_brdfLutSrv;
        ctx.skyRadianceSrv       = m_skyRadianceSrvForDDGI
                                 ? m_skyRadianceSrvForDDGI
                                 : m_fallbackSkyCubeSrv;
        ctx.hasSkyRadiance       = (m_skyRadianceSrvForDDGI != 0);
        if (const auto* lb = m_lightCB.Current(m_gfx))
            ctx.skyRadianceMips = (ctx.hasSkyRadiance && lb->iblRadianceMips > 0)
                                ? lb->iblRadianceMips : 1u;
        // Unified overlap guard for the water composite (same-frame t44).
        ctx.unifiedConfSrv =
            (mode == SSRSubsystem::LegacyMode::FullAdditive && unifiedReflActive &&
             m_rtReflectionPass)
                ? m_rtReflectionPass->GetUpcomingOutputSrv() : 0;
        // depthLastCL is threaded through: the trace list and the composite
        // hand-roll GBuffer_Depth round trips off the graph tracker, so they must
        // be ordered behind Hi-Z (which moved it) and ahead of what comes next;
        // Render() advances it only for the lists that touch depth (pyramid-only
        // mode touches none).
        // The HDR target is the other tracker it moves, in every mode (the
        // snapshot copy, the end state), and the probe-debug list and preComputeCL
        // transition HDR off that tracker at record time, so the last list Render()
        // recorded becomes colorLastCL, the chain they wait on. Otherwise
        // pyramid-only frames would order SSR.trace -> PreCompute by EndFrame's
        // pool-index tie-break alone (the frame capture reports such edges:
        // `rg frame`, unorderedEdges).
        const RHI::CommandList ssrLastCL =
            ssrChain->Render(m_gfx, ctx, colorLastCL, depthLastCL, mode);
        if (ssrLastCL.IsValid()) colorLastCL = ssrLastCL;

        // FullFold: LightingPass folds the legacy upsample output at t44
        // next frame - colour and confidence latent together, which keeps the
        // fallback free of a split-frame blend artifact.
        if (mode == SSRSubsystem::LegacyMode::FullFold && m_lightingPass)
            m_lightingPass->SetRTReflection(ssrChain->GetFinalColorSrv());
    }

    // Do not draw the debug wireframe here: it draws post-tonemap in the
    // Overlay stage (Phase 7). Fed through TAA / FSR2 with no motion vectors of
    // their own, the lines would be reprojected by the geometry behind them and
    // smear during camera motion.

    // DDGI probe debug spheres: PS samples irradiance atlas via octahedral mapping.
    // Only runs when at least one DDGIVolumeComponent has debugDraw=true.
    if (primary && m_ddgiProbeDebugPass && m_ddgiProbeDebugPass->enabled
        && colorLastCL.IsValid() && m_ddgiProbeDebugPass->HasDebugVolumes())
    {
        // No World: the slot lane was extracted in BuildScene_UpdateDDGI, inside
        // the walk that already proved each entity alive.
        RHI::CommandList dbgCL = m_gfx.BeginCommandList(RHI::QueueType::GRAPHICS);
        m_gfx.SetCommandListName(dbgCL, "DDGIProbeDebug");
        dbgCL.gfx = &m_gfx;
        GfxTrace::Label(dbgCL, "DDGIProbeDebug");
        m_gfx.AddCommandListDependency(dbgCL, colorLastCL);
        // DDGIProbeDebugPass binds depth as a DSV and emits no barrier for it,
        // so it assumes a state - the one Hi-Z and SSR leave - not just an
        // order. Put it in the chain.
        chainDepth(dbgCL);

        uint32_t dbgRegion = m_gfx.BeginGPUTimestamp(dbgCL, "DDGIProbeDebug");
        const RHI::Texture* depthTex = m_graph.GetPhysicalTexture(m_depthHandle);
        // It reads the book: its pipeline tests depth and writes nothing, so a
        // plane in DEPTH_WRITE or a DEPTH_READ state serves as it is, bound
        // through a read-only DSV where it is not DEPTH_WRITE, and a plane in
        // another state moves to DEPTH_READ_SRV. (D3D12 accepts DEPTH_READ with a
        // read-only DSV, or with a writable one whose pipeline writes no depth.)
        // -rgplaneoff: DEPTHSTENCIL and the writable DSV.
        RHI::DepthView dbgView;
        if (depthTex)
        {
            const RHI::ResourceState curDepth = m_graph.GetTextureState(m_depthHandle);
            if (!RG::RenderGraph::PlanesEnabled())
            {
                if (curDepth != RHI::ResourceState::DEPTHSTENCIL)
                {
                    m_gfx.PushBarrier(RHI::GPUBarrier::Image(
                        depthTex, curDepth, RHI::ResourceState::DEPTHSTENCIL), dbgCL);
                    m_graph.SetTextureState(m_depthHandle, RHI::ResourceState::DEPTHSTENCIL);
                }
            }
            else
            {
                auto testable = [](RHI::ResourceState s) {
                    return s == RHI::ResourceState::DEPTHSTENCIL || s == RHI::ResourceState::DEPTH_READ_SRV ||
                           s == RHI::ResourceState::DEPTHSTENCIL_READONLY;
                };
                const RHI::ResourceState curStencil = m_graph.GetTextureState(m_depthHandle, RG::Plane::Stencil);
                m_graph.MovePlanes(m_gfx, dbgCL, m_depthHandle,
                                   testable(curDepth)   ? RHI::ResourceState::UNDEFINED : RHI::ResourceState::DEPTH_READ_SRV,
                                   testable(curStencil) ? RHI::ResourceState::UNDEFINED : RHI::ResourceState::DEPTH_READ_SRV);
                dbgView.readOnlyDepth   = m_graph.GetTextureState(m_depthHandle) != RHI::ResourceState::DEPTHSTENCIL;
                dbgView.readOnlyStencil = m_graph.GetTextureState(m_depthHandle, RG::Plane::Stencil) !=
                                          RHI::ResourceState::DEPTHSTENCIL;
                dbgView.mayWriteDepth   = false;
                dbgView.mayWriteStencil = false;
            }
        }
        m_ddgiProbeDebugPass->Execute(m_gfx, dbgCL, depthTex,
                                      m_perObjectCB.CurrentBuffer(m_gfx), m_ddgiMgr, dbgView);
        m_gfx.EndGPUTimestamp(dbgCL, dbgRegion);

        colorLastCL = dbgCL;
    }

    _rzMark(10);

    // ---- Phase 5: Post-processing (preComputeCL -> computeCL -> restoreCL) ----
    if (m_taaPass || m_autoExposurePass || m_bloomPass || m_toneMapPass)
    {
        // All resize-time resource creation happens on the main thread: Execute()
        // keeps its lazy checks as a fallback, but the compute worker should only
        // record against stable handles, and descriptor-heap allocation must not
        // overlap other RenderGraph workers during a viewport transition.
        // The plan's stage C. Its facts: the two RR flags, read while the plan's
        // ray reconstruction holds (the RR predicates' own condition, so run.json's
        // unreadFlags does not change), and the path tracer's RR inputs. The raster
        // guides' allocation is a fact too, planned again once RRGuidePass::Prepare
        // has answered, below.
        {
            const auto& cmd = Sys::CommandLine::Get();
            const FramePlan::Path path = FramePlan::EffectivePath(FrameViewRead().planIntent, m_planState);
            const bool traced = path == FramePlan::Path::PathTrace || path == FramePlan::Path::PathTraceRealtime;
            if (primary)
            {
                m_planFacts.rrRaw          = m_plan.rr && cmd.Has("-dlss_rr_raw");
                m_planFacts.rrRasterGuides = m_plan.rr && traced ? cmd.GetInt("-rr_raster_guides", 0) : 0;
                m_planFacts.ptRRReady      = m_pathTracePass && m_pathTracePass->RayReconstructionReady();
                PlanStage(FramePlan::Stage::C);
            }
        }
        const bool fsrActive = m_plan.fsr;
        // RR denoises the path tracer's frame or a ReSTIR frame, and only
        // once this frame's RR inputs exist (RRInputsReady): the frame a
        // switch turns RR on resolves without DLSS.
        const bool dlssActive = m_plan.dlss;
        // "Active" = the user selected it. "Resolved" = it also came up and
        // will really produce a display-res image this frame. The distinction
        // is load-bearing: on the failure path the frame resolves through TAA
        // at RENDER res, and sizing the post chain as if it were display res
        // makes every downstream effect sample a buffer of the wrong size.
        const bool fsrResolved  = m_plan.fsrResolved;
        const bool dlssResolved = m_plan.dlssResolved;
        const bool upscalerActive = fsrActive || dlssActive;
        const uint32_t postW = m_plan.postW;
        const uint32_t postH = m_plan.postH;

        // TAA's history set (~51 MiB at 1080p) exists only while TAA can be the
        // resolve path: FSR2 replaces the resolve (its branch never calls
        // TAA::Execute), and an AA mode without TAA never runs it. Gated on the
        // config, not IsEnabled(): wireframe / debug-view toggles flip IsEnabled()
        // transiently and must not churn 51 MiB of allocations (FSR2 shuts down in
        // Native mode by the same rule).
        const bool taaCfg = (m_aaMode == AAMode::TAA) || (m_aaMode == AAMode::FXAA_TAA) || (m_aaMode==AAMode::DLSS_RR && !IsRayReconstructionActive() && !IsRRFallbackToDLSS());
        // FXAA's output (~15.8 MiB at 1080p) exists only in an AA mode that
        // can run it. The same config-not-IsEnabled() rule as TAA, for the same
        // reason: FXAAPass::Execute also early-outs on fsrActive, and wireframe /
        // lighting-debug views flip IsEnabled() transiently.
        const bool fxaaCfg = (m_aaMode == AAMode::FXAA) || (m_aaMode == AAMode::FXAA_TAA);
        // The post chain's prepare points (the order is
        // Renderer::RegisterPrepareOrder's). First the AA passes, by the rules
        // above.
        RG::PrepareContext pc = PrepareContextNow();
        pc.postW = postW;
        pc.postH = postH;
        // Or the plan's TAA stand-in for an upscaler whose context is down
        // this frame (taaStandIn): its history comes up then.
        pc.taa = (taaCfg && !upscalerActive) || m_plan.taaStandIn;
        pc.fxaa = fxaaCfg && !upscalerActive;
        pc.upscaler = upscalerActive && primary;
        m_passes.Prepare(RG::PrepareStage::PostAA, pc);

        // The four shared-pool producers must learn about the pool before their
        // PrepareResources, or RebuildTextures takes the private-allocation branch
        // and strands a full-res RGBA16F each (~47.5 MiB with DoF / Underwater /
        // Stylize off: PostChain::Execute calls SetSharedOutput only on enabled
        // producers, after PrepareResources ran). The pool is created here, not in
        // the compute worker: DestroyTexture / CreateTexture allocate descriptors,
        // which this block's opening comment forbids on a worker thread.
        // A secondary view's own pool, at its size.
        PostPool& postPool = primary ? m_ppPool : m_secondary[viewSlot].ppPool;
        EnsurePostProcessPool(postPool, postW, postH);
        const bool poolReady = postPool.Ready();
        // Slot assignment stays the PostChain's (it ping-pongs per frame); the
        // pooled producers only learn that a pool exists so they do not allocate a
        // private one. Slot 0 is a placeholder PostChain::Execute overwrites before
        // every dispatch.
        pc.postPool = poolReady ? &postPool.tex[0].Tex() : nullptr;
        m_passes.Prepare(RG::PrepareStage::PostPool, pc);
        // Then the working sets: Bloom publishes the group-D base before
        // NPRStabilize places its output; LensFlare owns a separate committed
        // texture.
        m_passes.Prepare(RG::PrepareStage::PostEffects, pc);
        // The render features prepare PreResolve here (RenderFeature.h): the
        // plan's stage C has run once, and what they learn - the RR raster
        // guides' allocation, made only while the plan wants them (~55 MiB at
        // 1080p) - is the stage's last fact, planned again here.
        {
            FeatureFrameContext featureCtx{ FrameViewRead(), m_plan, m_planFacts, m_gfx, m_graph.View() };
            m_features.Prepare(RG::Stage::PreResolve, featureCtx);
        }
        if (primary)
        {
            PlanStage(FramePlan::Stage::C);
            FinishFramePlan();
        }

        // ---- HDR display output ----
        // The graphics device is the single source of truth for the output mode: it
        // owns the swap-chain format, demotes to SDR when a display refuses the
        // colour space, and latches a mode change to a frame boundary,
        // asynchronously to the setting the user clicked. A copy cached on the
        // Renderer would give a composition buffer whose format disagrees with the
        // back buffer for a frame, so it is read fresh every frame.
        // ToneMap reads it in its prepare (SetOutputMode, then EnsureTexture, whose
        // dirty test includes the mode), here on the main thread before the compute
        // worker is kicked below - never inside a pass Execute.
        m_passes.Prepare(RG::PrepareStage::PostOutput, pc);

        // Every pass that renders into the composition buffer must declare the
        // buffer's real format in its PSO: the RTV silently inherits it from the
        // texture, so a stale RGBA8 is a debug-layer error and undefined output
        // with no compile error. Pushed from one place so a new overlay pass is
        // obviously missing a line. PSODesc::Hash mixes rtvFormats, so the SDR and
        // HDR variants coexist in the cache instead of thrashing it on every toggle.
        if (m_toneMapPass)
        {
            const RHI::Format compFmt = m_toneMapPass->GetFinalOutputFormat();
            if (m_debugWirePass)   m_debugWirePass->SetTargetFormat(compFmt);
            if (m_worldUIPass)     m_worldUIPass->SetTargetFormat(compFmt);
            if (m_debugIconPass)   m_debugIconPass->SetTargetFormat(compFmt);
            if (m_uiPass)          m_uiPass->SetTargetFormat(compFmt);
        }

        // preComputeCL (GRAPHICS): transition HDR + depth for compute reads.
        RHI::CommandList preComputeCL = m_gfx.BeginCommandList(RHI::QueueType::GRAPHICS);
        m_gfx.SetCommandListName(preComputeCL, "PreCompute");
        preComputeCL.gfx = &m_gfx;
        GfxTrace::Label(preComputeCL, "PreCompute");
        if (colorLastCL.IsValid())
            m_gfx.AddCommandListDependency(preComputeCL, colorLastCL);
        // A secondary view's post after the main view's (and the previous view's): the shared
        // grading LUT, the queue order the editor's picture reads expect.
        if (!primary && m_viewAfter.IsValid())
            m_gfx.AddCommandListDependency(preComputeCL, m_viewAfter);
        // Tail of the post-graph depth chain: it transitions depth for the
        // compute reads off the graph tracker, so it must sit behind every
        // list that moved the tracker. colorLastCL (the HDR chain: the SSR
        // lists and the probe-debug list reassign it) does not cover Hi-Z;
        // the depth chain covers every depth mover always. Phase 7's
        // restoreCL inherits the order through computeCL.
        chainDepth(preComputeCL);

        ENGINE_CHECK_ONCE(m_gfx.HdrScopeDepth() == (primary ? 0 : 1),
                          "Renderer pre-compute SetHdrTextureState: HDR access inside an HDR push scope (depth=%d)",
                          m_gfx.HdrScopeDepth());
        m_gfx.SetHdrTextureState(RHI::ResourceState::SHADER_RESOURCE_COMPUTE, preComputeCL);
        if (primary && m_lumaReducePass && !Sys::CommandLine::Get().Has("-energy_after_aa"))
        {
            uint64_t energySrv=m_gfx.GetHdrSceneSrvGpuHandle();
            if (giActive && Sys::CommandLine::Get().Has("-energy_gi"))
            {
                const auto* tex=m_graph.GetPhysicalTexture(m_giResultHandle);
                const auto before=m_graph.GetTextureState(m_giResultHandle);
                m_gfx.PushBarrier(RHI::GPUBarrier::Image(tex,before,RHI::ResourceState::SHADER_RESOURCE_COMPUTE),preComputeCL);
                m_graph.SetTextureState(m_giResultHandle,RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
                energySrv=m_rtGiPass->GetUpcomingOutputSrv();
            }
            m_lumaReducePass->SetSource(m_gfx.DescriptorHeapIndexFromGpuHandle(energySrv),
                FrameViewRead().renderWidth, FrameViewRead().renderHeight, FrameViewRead().currentFrame);
            m_lumaReducePass->Execute(preComputeCL);
        }
        if (m_autoExposurePass) m_autoExposurePass->PrepareForCompute(preComputeCL);
        if (m_toneMapPass) m_toneMapPass->PrepareForCompute(preComputeCL);
        if (taaDepthTex)
        {
            // Each plane from its own state (the compute queue's TAA and
            // reactive mask read depth and stencil), one barrier on every plane
            // when they agree. -rgplaneoff: one whole-texture barrier off the
            // tracker.
            if (RG::RenderGraph::PlanesEnabled())
                m_graph.MovePlanes(m_gfx, preComputeCL, m_depthHandle, RHI::ResourceState::SHADER_RESOURCE_COMPUTE,
                                   RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
            else
            {
                RHI::ResourceState curDepthState = m_graph.GetTextureState(m_depthHandle);
                m_gfx.PushBarrier(RHI::GPUBarrier::Image(
                    taaDepthTex,
                    curDepthState,
                    RHI::ResourceState::SHADER_RESOURCE_COMPUTE), preComputeCL);
                m_graph.SetTextureState(m_depthHandle, RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
            }
        }
        // ---- Render features at PreResolve (RenderFeature.h) ----
        // After every GBuffer writer (the whole graph), before the AA /
        // upscaler, on the pre-compute list: where the RR guides and the
        // velocity fill record. The frame resources they read are filled here,
        // before the upscale branch advances the jitter state - the previous
        // view-projection is still the previous frame's.
        {
            m_frameResources.depthSrv             = depthSrv;
            m_frameResources.viewProjNoJitter     = RecordingViewState().jitter.GetCurrViewProjNoJitter();
            m_frameResources.prevViewProjNoJitter = RecordingViewState().jitter.GetPrevViewProjNoJitter();
            m_frameResources.pathTrace = IsPathTrace(GetRenderPath());
            {
                const auto materialSrv = m_gfx.GetBufferSRVGpuHandle(MaterialSRVBuffer(m_gfx.GetFrameIndex()));
                const auto index = materialSrv ? m_gfx.DescriptorHeapIndexFromGpuHandle(materialSrv) : ~0u;
                m_frameResources.materialsIdx = index == ~0u ? 0u : index;
            }
            {
                const auto* lb = m_lightCB.Current(m_gfx);
                m_frameResources.lightCB = lb != nullptr;
                m_frameResources.sunDirection = lb ? XMFLOAT3{ lb->lightDir[0], lb->lightDir[1], lb->lightDir[2] }
                                                   : XMFLOAT3{};
                m_frameResources.sunColor = lb ? XMFLOAT3{ lb->lightColor[0], lb->lightColor[1], lb->lightColor[2] }
                                               : XMFLOAT3{};
                m_frameResources.iblRadianceMips = lb && lb->iblRadianceMips > 0 ? lb->iblRadianceMips : 1u;
                m_frameResources.iblStrength = lb ? lb->iblStrength : 1.0f;
            }
            {
                const uint64_t sky = m_skyRadianceSrvForDDGI;
                const uint32_t index = sky ? m_gfx.DescriptorHeapIndexFromGpuHandle(sky) : ~0u;
                m_frameResources.skyRadianceIdx = index == ~0u ? 0u : index;
            }
            m_frameResources.diDistance = primary && diActive && m_rtDiPass && m_rtDiPass->RayReconstructionRaw();
            // The post chain's lens flare. m_todMoonActive is extracted on
            // the game side in SyncSkyboxIBL, where the World is already being
            // read for the same component.
            m_frameResources.moonActive = m_todMoonActive;
            m_frameResources.cloudSrv = m_cloudPass ? m_cloudPass->GetCloudSrvHandle() : 0;
            // The post chain's Resolve stage: the jitter TAA and FSR 2 take -
            // read before the chain advances it - and NPRStabilize's scene
            // ceiling.
            m_frameResources.jitter = { RecordingViewState().jitter.GetJitterX(), RecordingViewState().jitter.GetJitterY() };
            {
                const auto* lb = m_lightCB.Current(m_gfx);
                m_frameResources.nprMaxBrightness = lb ? lb->nprMaxBrightness : 0.0f;
            }
            m_frameResources.rtReflectionIdx = 0;
            m_frameResources.rtReflectionDistanceIdx = 0;
            if (unifiedReflActive && m_rtReflectionPass)
            {
                const uint64_t reflection = m_rtReflectionPass->GetUpcomingOutputSrv();
                const uint32_t index = reflection ? m_gfx.DescriptorHeapIndexFromGpuHandle(reflection) : ~0u;
                m_frameResources.rtReflectionIdx = index == ~0u ? 0u : index;
                RHI::ResourceState hitState{};
                if (const RHI::Texture* hit = m_rtReflectionPass->GetRRHitDistance(hitState))
                {
                    const uint32_t hitIndex = m_gfx.DescriptorHeapIndexFromGpuHandle(m_gfx.GetTextureSRVGpuHandle(*hit));
                    m_frameResources.rtReflectionDistanceIdx = hitIndex == ~0u ? 0u : hitIndex;
                }
            }
            FeatureRecordContext featureCtx{ preComputeCL, m_gfx, m_graph, m_frameTextures, FrameViewRead(), m_plan,
                                             m_frameResources };
            m_features.Record(RG::Stage::PreResolve, featureCtx);
        }
        // DLSS reads the raster guides only when this frame wrote them (the RR
        // guide feature's output, FrameResources::rrRasterGuides); the post
        // chain's DLSS gets their textures, not the pass. ReSTIR GI's connection
        // distance is RR's diffuse hit distance. (DLSSPass resolves them to
        // Streamline's native resources.)
        auto valid = [](const RHI::Texture* t) -> const RHI::Texture* { return (t && t->IsValid()) ? t : nullptr; };
        std::array<const RHI::Texture*, 4> dlssRasterGuides{};
        const RHI::Texture* dlssDiffuseDistance = nullptr;
        // The colour before transparency this frame's graph copied
        // (either path).
        const RHI::Texture* rrSnapshot =
            m_rrColorSnapshot && IsRayReconstructionActive() ? m_rrColorSnapshot->Written() : nullptr;
        const RHI::Texture* dlssColorBeforeTransparency = valid(rrSnapshot);
        if (m_frameResources.rrRasterGuidesWritten && DLSSReadsRasterRRGuides())
        {
            for (uint32_t i = 0; i < 4; ++i)
                dlssRasterGuides[i] = valid(m_frameResources.rrRasterGuides[i]);
            if (giActive && m_rtGiPass && m_rtGiPass->RayReconstructionRaw())
                dlssDiffuseDistance = valid(m_rtGiPass->GetRRDiffuseDistance());
        }
        // What the post chain's DLSS reads, decided here on the main thread:
        // RR's guides are RRGuidePass's raster ones when this frame wrote them
        // for DLSS (a raster path, -rr_raster_guides=2), the path tracer's own
        // otherwise; with neither source RR takes none.
        {
            FrameResources& fr = m_frameResources;
            fr.rr = primary && IsRayReconstructionActive();
            const bool rasterGuides = dlssRasterGuides[0] != nullptr;
            fr.rrSetGuides = fr.rr && (m_pathTracePass || rasterGuides);
            fr.rrGuides = {};
            if (fr.rrSetGuides)
                for (uint32_t i = 0; i < 4; ++i)
                {
                    if (rasterGuides) { fr.rrGuides[i] = dlssRasterGuides[i]; continue; }
                    const auto* guide = m_pathTracePass->GetRRGuide(i);
                    if (guide && guide->IsValid()) fr.rrGuides[i] = guide;
                }
            fr.rrDiffuseDistance = rasterGuides ? dlssDiffuseDistance : nullptr;
            fr.rrColorBeforeTransparency = dlssColorBeforeTransparency;
            // Main-thread state, handed to the worker by value.
            fr.slFrameToken = primary ? m_slFrameToken : nullptr;
        }
        // Velocity is made compute-readable here for TAA / FSR2: it carries an
        // SRV (GBuffer3_Velocity isSRV), but the graph leaves it in RENDERTARGET
        // after GBufferPass (and SSR's fromSR restores it there). The normal
        // buffer needs no transition here: XeGTAO declares its read in the graph,
        // which leaves it in SHADER_RESOURCE, and next frame's GBufferPass
        // transitions it back to RENDERTARGET.
        if (taaVelocityTex && m_taaPass)
        {
            // The sky's motion vectors were filled at PreResolve above (the
            // VelocityFill render feature, Renderer/Features/), which tells the
            // graph the state it left the buffer in.
            const RHI::ResourceState curVelState = m_graph.GetTextureState(m_velocityHandle);
            if (curVelState != RHI::ResourceState::SHADER_RESOURCE_COMPUTE)
            {
                m_gfx.PushBarrier(RHI::GPUBarrier::Image(
                    taaVelocityTex,
                    curVelState,
                    RHI::ResourceState::SHADER_RESOURCE_COMPUTE), preComputeCL);
                m_graph.SetTextureState(m_velocityHandle, RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
            }

            // ---- Streamline depth + motion-vector tags ----
            // Tagged here, once, not in DLSSPass: DLSS SR consumes them inside
            // slEvaluateFeature and Frame Generation at Present, ValidUntilPresent
            // satisfies both, and Frame Generation can run without DLSS SR (with
            // FSR2 or no upscaler), so this must not hang off the DLSS path.
            // Both buffers are final here: every geometry pass has run,
            // VelocityFill has filled the sky, and nothing after this writes
            // either. MainSurface() does the tagging.
            if (primary && m_slFrameToken && taaDepthTex && taaVelocityTex)
                m_gfx.MainSurface().TagDepthAndMotion(preComputeCL, *taaDepthTex, *taaVelocityTex,
                                                      m_slFrameToken);
        }
        // ---- The post chain (PostProcess/PostChain.h) ----
        // Its Context is built here, on the main thread, and handed to the
        // worker by value: the Resolve stage's inputs (NPRStabilize,
        // RRColorAlign, the reactive mask, FSR 2 | DLSS | TAA, FXAA -
        // PostProcess/ResolveEffects.h), then the HDR and tonemap effects'.
        // The frame's resolved settings are the hand-off's copy (Runtime::Published): the next resolve may be rewriting.
        ENGINE_CHECK_ONCE(m_gfx.HdrScopeDepth() == (primary ? 0 : 1),
                          "Renderer AA input GetHdrSceneSrvGpuHandle: HDR access inside an HDR push scope (depth=%d)",
                          m_gfx.HdrScopeDepth());
        PostProcess::Context ppCtx{};
        ppCtx.gfx = &m_gfx;
        // The HDR effects' size: the display size once an upscaler resolved,
        // the render size otherwise - the plan's *Resolved, with which the pool
        // was sized above, not whether the upscaler really runs on the worker:
        // a failed DLSS-RR evaluate marks the pass unhealthy there, and sizing
        // the chain from that would put it at render res against the
        // display-res pool (an ENGINE_CHECK). For that one frame the upscaler's
        // output keeps its previous pixels (a stale picture, never an invalid
        // read); the next frame is sized from the new state.
        const bool upResolved = fsrResolved || dlssResolved;
        ppCtx.viewportWidth  = upResolved ? FrameViewRead().vpWidth  : FrameViewRead().renderWidth;
        ppCtx.viewportHeight = upResolved ? FrameViewRead().vpHeight : FrameViewRead().renderHeight;
        ppCtx.deltaTime      = FrameViewRead().deltaTime;
        // The AA input is the HDR scene (NPRStabilize may replace it). The
        // meter AutoExposure reads is the same scene, BEFORE NPRStabilize: that
        // scales exposure-protected NPR pixels by rcp(prev exposure), and
        // metering its output would close a positive feedback loop.
        // NPRStabilize writes a private target, so the scene HDR texture is
        // untouched and stays SHADER_RESOURCE_COMPUTE for the whole frame.
        // Render res - upstream of the upscaler - as the render-res normal /
        // depth / material metadata the weighting reads.
        ppCtx.hdrSrv         = m_gfx.GetHdrSceneSrvGpuHandle();
        ppCtx.meterSrv       = ppCtx.hdrSrv;
        ppCtx.meterWidth     = FrameViewRead().renderWidth;
        ppCtx.meterHeight    = FrameViewRead().renderHeight;
        ppCtx.depthSrv       = depthSrv;
        ppCtx.normalSrv      = normalSrv;
        ppCtx.materialSrv    = materialSrv;
        ppCtx.metaWidth      = FrameViewRead().renderWidth;
        ppCtx.metaHeight     = FrameViewRead().renderHeight;
        ppCtx.materialCount  = GetMaterialSlotCapacity();
        ppCtx.cameraNear     = FrameViewRead().camera.nearZ;
        ppCtx.cameraFar      = FrameViewRead().camera.farZ;
        // The upscalers and TAA: depth and velocity, compute-readable since
        // the pre-compute list above, TAA's SRVs, and the plan that says which
        // resolve the frame takes.
        ppCtx.depthTex       = taaDepthTex;
        ppCtx.velocityTex    = taaVelocityTex;
        ppCtx.surfaceSrv     = surfaceSrv;
        ppCtx.velocitySrv    = velocitySrv;
        ppCtx.stencilSrv     = stencilSrv;
        ppCtx.plan           = &m_plan;
        ppCtx.worldFrozen = IsWorldClockFrozen();
        ppCtx.sceneSettled = PostProcess::Runtime::PublishedSceneSettled(m_sceneRead);
        ppCtx.resources.exposureBuffer = m_autoExposurePass
            ? m_gfx.GetBufferSRVGpuHandle(m_autoExposurePass->GetExposureBuffer()) : 0;
        ppCtx.resolved       = &PostProcess::Runtime::PublishedView(m_sceneRead, viewSlot);
        ppCtx.skipDepthOfField = m_pathTracePass && m_pathTracePass->IsActive()
            && m_pathTracePass->UsesCameraPrimary();
        // The frame: the view, and the frame resources filled above (the
        // sun, the moon, the clouds, the jitter, RR's inputs).
        ppCtx.view  = &FrameViewRead();
        ppCtx.frame = &m_frameResources;
        // A secondary view runs only the view-safe effects (IEffect::ViewSafe).
        ppCtx.secondaryView = !primary;

        // Shared ping-pong targets for the HDR producers, created above
        // (EnsurePostProcessPool) from the same plan values. Never on the
        // worker: DestroyTexture + CreateTexture allocate descriptors, which
        // this Phase's opening comment forbids there.
        PostPool& chainPool = primary ? m_ppPool : m_secondary[viewSlot].ppPool;
        ENGINE_CHECK_ONCE(
            chainPool.w == ppCtx.viewportWidth && chainPool.h == ppCtx.viewportHeight,
            "post-process pool %ux%u does not match this frame's post size %ux%u — "
            "the main-thread Ensure used different dimensions",
            chainPool.w, chainPool.h, ppCtx.viewportWidth, ppCtx.viewportHeight);
        if (chainPool.Ready())
        {
            // Their states are kept in the device's book.
            ppCtx.poolTex[0]   = &chainPool.tex[0].Tex();
            ppCtx.poolTex[1]   = &chainPool.tex[1].Tex();
            ppCtx.poolIndex    = 0;
        }

        // computeCL, cross-queue fenced. Normally the ASYNC COMPUTE queue,
        // which is what lets the post chain overlap the next frame's geometry;
        // an effect can ask for GRAPHICS (DLSS - DLSSEffect::
        // RequiresGraphicsQueue says why). The whole list switches, so the
        // recording order is the same on every path; the cost is the async
        // overlap while DLSS is selected.
        const RHI::QueueType postQueue = m_postChain && m_postChain->RequiresGraphicsQueue(ppCtx)
            ? RHI::QueueType::GRAPHICS : RHI::QueueType::COMPUTE;
        RHI::CommandList computeCL = m_gfx.BeginCommandList(postQueue);
        m_gfx.SetCommandListName(computeCL, "PostChain");
        computeCL.gfx = &m_gfx;
        m_gfx.AddCommandListDependency(computeCL, preComputeCL);
        ppCtx.cl = computeCL;
        // AutoExposure, first in the chain, sets an argument of the shared compute layout before it binds a
        // pipeline: the layout has to be the one in force.
        m_gfx.RestoreDefaultComputeRootSignature(computeCL);

        // Worker 2 only records; every resource was prepared above.
        ViewState* recordingView = &RecordingViewState();
        m_renderWorkers[2].Kick([this, ppCtx, recordingView]() mutable
        {
            // NPRStabilize, RRColorAlign, the reactive mask, FSR 2 | DLSS |
            // TAA, FXAA; the stage ends with its routing (ppCtx.hdrSrv = the
            // frame's resolved image).
            if (m_postChain)
                m_postChain->Execute(ppCtx, RG::Stage::Resolve);

            // One history roll per frame, including AA None/PT/orthographic.
            // Disabled AA emits no GPU region, but temporal scene consumers
            // still need the previous view and jitter to advance.
            recordingView->jitter.AdvanceToNextFrame();

            if (m_postChain)
            {
                // DoF -> CAS -> AutoExposure -> Bloom -> LensFlare -> Underwater ->
                // Stylize -> Tonemap ((stage, Order)), in the region the
                // profiler and its readers know them by.
                uint32_t r = m_gfx.BeginGPUTimestamp(ppCtx.cl, "PostProcessStack");
                m_postChain->Execute(ppCtx, RG::Stage::PostResolveHDR, RG::Stage::Tonemap);
                m_gfx.EndGPUTimestamp(ppCtx.cl, r);
                // GlassShatter: composites into Tonemap's output in place.
                m_postChain->Execute(ppCtx, RG::Stage::PostTonemap);
            }
        });

        // ---- Phase 6: Wait for Worker 2 ----
        {
            // As in Phase 3: no stage zone spans the co_await.
#ifdef TRACY_ENABLE
            renderStage.reset();
#endif
            co_await m_renderWorkers[2].Completion();
            m_renderWorkers[2].Join(); // ready-only, consume the completed slot
#ifdef TRACY_ENABLE
            renderStage.emplace(&kRenderStages[11], true);
#endif

        }

        // ---- Phase 7: restoreCL (GRAPHICS) - depth + display transitions; cross-queue fenced.
        RHI::CommandList restoreCL = m_gfx.BeginCommandList(RHI::QueueType::GRAPHICS);
        m_gfx.SetCommandListName(restoreCL, "Restore");
        restoreCL.gfx = &m_gfx;
        GfxTrace::Label(restoreCL, "Restore");
        m_gfx.AddCommandListDependency(restoreCL, computeCL);

        // Depth is not restored to DEPTHSTENCIL here: DebugWirePass (the
        // Overlay stage, below) runs later in this list and reads it as a
        // pixel-shader SRV for its occlusion test. The restore follows the
        // stage in the same list, so next frame's GBuffer sees the right state.

        // The normal buffer ends the frame in whatever state the graph left it,
        // like albedo / surface / emissive; next frame's GBufferPass declares the
        // write, so the graph emits the transition.
        if (m_toneMapPass) m_toneMapPass->TransitionForDisplay(restoreCL);

        // HDR editor viewport: Phase 5 left it in SHADER_RESOURCE_COMPUTE
        // (NPSR-only, for the compute reads). The next frame's first RG colour pass
        // would transition it back lazily in SetRenderTargetToHdr, but with
        // multi-threaded recording the tracker can desync from the GPU state
        // ("ClearRTV on resource in NPSR" against `GraphicsDX12.HdrEditorViewport`),
        // so restore it explicitly: the renderer's part of the frame ends with HDR
        // in RT. (SetRenderTargetToSwapChain / SetRenderTargetToPresentComposition
        // then take it to ALL_SHADER_RESOURCE for the editor's ImGui sample,
        // through the same tracker.) Idempotent: the helper no-ops when the tracker
        // already says RT.
        ENGINE_CHECK_ONCE(m_gfx.HdrScopeDepth() == (primary ? 0 : 1),
                          "Renderer frame-end restore SetHdrTextureState: HDR access inside an HDR push scope (depth=%d)",
                          m_gfx.HdrScopeDepth());
        m_gfx.SetHdrTextureState(RHI::ResourceState::RENDERTARGET, restoreCL);

        // ---- Debug target view (Renderer/DebugTargets.h) ----
        // One render target painted over the final output: after the whole
        // post chain, before the overlays below, so wires, icons and the HUD
        // stay on top of it; records nothing while no target is asked for.
        // The Texture Viewer, UI designer and pass-capture pictures record just
        // before it, at the same known-legal point.
        // Texture Viewer pictures (TextureViewService.h): only the views whose
        // description or source changed; nothing with no viewer open.
        if (primary && m_textureViews) m_textureViews->Execute(restoreCL, static_cast<std::uint64_t>(FrameViewRead().currentFrame));
        // UI Editor designer pictures (UIViewService.h): at most one view a
        // frame, only when its picture changed; nothing with no designer open.
        if (primary && m_uiViews) m_uiViews->Execute(restoreCL, static_cast<std::uint64_t>(FrameViewRead().currentFrame));
        // The pass capture: what the graph's copy did this frame, and the
        // panel's picture of it - after the graph, so the copy is done;
        // nothing when nothing was armed.
        if (primary)
        {
            ExecutePassCapture(restoreCL);
            ExecuteDebugTargetView(restoreCL);
        }

        // ---- The Overlay stage (PostProcess/OverlayEffects.h) ----
        // Over the finished picture, into Tonemapping's output: the debug
        // wires (post-tonemap: no temporal pass drags them by the velocity
        // buffer, no exposure or bloom touches them), the world-space UI, the
        // editor's icons, Frame Generation's HUD-less colour and UI layer, the
        // HUD - in that order, wires first so icons and the HUD draw over them.
        // Every view's (a secondary view runs the view-safe effects: the editor's overlays where it
        // shows them).
        if (m_postChain)
        {
            PostProcess::Context overlay{};
            overlay.cl        = restoreCL;
            overlay.gfx       = &m_gfx;
            overlay.secondaryView = !primary;
            overlay.show      = m_graph.View().show.bits;
            overlay.deltaTime = FrameViewRead().deltaTime;
            // DebugWire borrows depth as a pixel-shader SRV (its occlusion
            // test) through the graph's tracked state.
            overlay.depthSrv  = depthSrv;
            overlay.depthTex  = taaDepthTex;
            overlay.graph     = &m_graph;
            overlay.textures  = &m_frameTextures;
            overlay.plan      = &m_plan;
            overlay.resolved  = &PostProcess::Runtime::PublishedView(m_sceneRead, viewSlot);
            overlay.view      = &FrameViewRead();
            overlay.frame     = &m_frameResources;
            overlay.resources.tonemapped = m_toneMapPass ? m_toneMapPass->GetFinalOutputTexture() : nullptr;
            m_postChain->Execute(overlay, RG::Stage::Overlay);
        }

        // Depth back to DEPTHSTENCIL for next frame's GBuffer - after the
        // Overlay stage, whose DebugWire read it as an SRV. The same command
        // list, so the next GBuffer still sees DEPTHSTENCIL.
        if (taaDepthTex)
        {
            // Each plane from its own state (DebugWire may have moved the depth
            // plane alone). -rgplaneoff: one whole-texture barrier off the tracker.
            if (RG::RenderGraph::PlanesEnabled())
                m_graph.MovePlanes(m_gfx, restoreCL, m_depthHandle, RHI::ResourceState::DEPTHSTENCIL,
                                   RHI::ResourceState::DEPTHSTENCIL);
            else
            {
                RHI::ResourceState curState = m_graph.GetTextureState(m_depthHandle);
                if (curState != RHI::ResourceState::DEPTHSTENCIL)
                {
                    m_gfx.PushBarrier(RHI::GPUBarrier::Image(
                        taaDepthTex,
                        curState,
                        RHI::ResourceState::DEPTHSTENCIL), restoreCL);
                    m_graph.SetTextureState(m_depthHandle, RHI::ResourceState::DEPTHSTENCIL);
                }
            }
        }
        // Always clear the draw list: widgets re-emit each frame, so a disabled UI cannot accumulate.
        if (primary && m_uiPass) m_uiPass->GetDrawList().Clear();

        result = restoreCL;
        co_return;
    }

    result = colorLastCL.IsValid() ? colorLastCL : shadowCL;
    co_return;
}

// ==== Frame capture (RenderGraph/FrameCapture.h) ====
// Both run on the render half, ArmFrameCapture before the device's BeginFrame
// and PublishFrameCapture after its EndFrame, so the device journal, the main
// graph's snapshot and the join all describe one frame.
bool Renderer::ArmFrameCapture()
{
    if (!RG::FrameCaptures().ConsumeRequest()) return false;
    m_gfx.ArmFrameJournal();
    return true;
}

void Renderer::PublishFrameCapture()
{
    std::shared_ptr<const GfxDiag::FrameSnapshot> device = m_gfx.TakeFrameSnapshot();
    std::shared_ptr<const RG::GraphSnapshot>      graph  = m_graph.TakeFrameCaptureSnapshot();
    if (!device)
    {
        // EndFrame never reached its snapshot (it threw), or the device does
        // not journal. Nothing to publish: the reader's wait reports it.
        LOG_WARNING("[RG_COVERAGE] a frame capture was armed but the device built no frame "
                    "snapshot - nothing published");
        return;
    }
    std::string ignorePass;
#if WITH_HARNESS
    // The rg_frame pack's reverse arm: join as if this pass declared nothing.
    static const std::string s_ignorePass = Sys::CommandLine::Get().Get("-rgtest_undeclare");
    ignorePass = s_ignorePass;
#endif
    std::shared_ptr<RG::FrameCapture> cap =
        RG::BuildFrameCapture(std::move(graph), std::move(device), ignorePass);
    const RG::FrameCapture::Coverage n = cap->coverage;
    // The main depth texture's transitions in the captured frame, which the
    // rg_planes pack bounds: every one, those that moved the depth plane alone,
    // the stencil plane alone, every plane; and stencilInGraph, the ones a graph
    // pass's list made on the stencil plane (or on every plane) - none while
    // the planes are tracked apart.
    uint32_t depthAll = 0, depthPlane = 0, stencilPlane = 0, wholeTexture = 0, stencilInGraph = 0;
    if (cap->device)
    {
        using Snap = GfxDiag::FrameSnapshot;
        const Snap& d = *cap->device;
        for (const Snap::Barrier& b : d.journal)
        {
            if (b.kind != Snap::Kind::Transition || b.resource >= d.resources.size() || (b.flags & 0x1)) continue;
            if (d.resources[b.resource].name != "GBuffer_Depth") continue;   // Renderer_Setup's m_depthHandle
            ++depthAll;
            const bool whole = b.subresource == Snap::kNone;
            if (whole)                  ++wholeTexture;
            else if (b.subresource == 1) ++stencilPlane;
            else                         ++depthPlane;
            if ((whole || b.subresource == 1) && b.cl < cap->passOfList.size() && cap->passOfList[b.cl] >= 0)
                ++stencilInGraph;
        }
    }
    const unsigned long long frame = static_cast<unsigned long long>(cap->frame);
    const unsigned hasGraph = cap->graph ? 1u : 0u;
    RG::FrameCaptures().Publish(std::move(cap));
    // Proof of life, and what the rg_frame pack reads.
    LOG_INFO("[RG_COVERAGE] frame=%llu seq=%llu graph=%u lists=%u batches=%u barriers=%u native=%u "
             "resources=%u observedEdges=%u declaredEdges=%u coveredEdges=%u hiddenEdges=%u "
             "readReadEdges=%u intraPassEdges=%u crossQueueEdges=%u unorderedEdges=%u stateMismatches=%u "
             "undeclaredResourceEdges=%u opaquePasses=%u manualBarriers=%u",
             frame, static_cast<unsigned long long>(RG::FrameCaptures().PublishedSequence()), hasGraph,
             n.commandLists, n.batches, n.barriers, n.nativeBarriers, n.resources,
             n.observedEdges, n.declaredEdges, n.coveredEdges, n.hiddenEdges,
             n.readReadEdges, n.intraPassEdges, n.crossQueueEdges, n.unorderedEdges, n.stateMismatches,
             n.undeclaredResourceEdges, n.opaquePasses, n.manualBarriers);
    LOG_INFO("[RG_PLANES] frame=%llu texture=GBuffer_Depth barriers=%u depthPlane=%u stencilPlane=%u "
             "wholeTexture=%u stencilInGraph=%u planes=%u", frame, depthAll, depthPlane, stencilPlane, wholeTexture,
             stencilInGraph, RG::RenderGraph::PlanesEnabled() ? 1u : 0u);
}
