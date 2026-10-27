#include "Diagnostics/Profiler.h"
#include <optional>
#include "System/PathUtil.h"
#include "Renderer/Renderer.h"
#include "Settings/SettingsStore.h"      // PublishSettingsSnapshot reads the store
#include "PostProcess/PostProcessRuntime.h"   // ... and publishes the frame's post settings
#include "Settings/PassSettings.h"       // pass.pathTrace.mediaMaxDistance (UploadMediumSet)
#include "Scene/MeshAssetRegistry.h"
#include "Resource/MeshLibIdentity.h"

// engine graphics / backend
#include "Graphics/GraphicsDX12.h"
#include "Graphics/DepthTrace.h"
#include "Graphics/ShadowSystem.h"
#include "Graphics/ParticleSystem.h"
#include "Graphics/TracerSystem.h"
#include "Graphics/BeamSystem.h"
#include "Graphics/AfterimageSystem.h"
#include "Graphics/TrailSystem.h"
#include "Graphics/SSR/SSRSubsystem.h"
#include "Graphics/ReflectionProbeTypes.h"

// render passes
#include "Renderer/Preview/PreviewRenderer.h"
#include "Renderer/Preview/PreviewStudioRig.h"
#include "RenderGraph/RenderPass/DDGIPass.h"
#include "RenderGraph/RenderPass/DDGIProbeDebugPass.h"
#include "RenderGraph/RenderPass/GBufferPass.h"
#include "RenderGraph/RenderPass/SurfaceCacheBakePass.h"
#include "RenderGraph/RenderPass/TerrainPass.h"
#include "RenderGraph/RenderPass/GrassPass.h"
#include "RenderGraph/RenderPass/WaterPass.h"
#include "RenderGraph/RenderPass/WaterSurfacePass.h"
#include "RenderGraph/RenderPass/LightingPass.h"
#include "RenderGraph/RenderPass/PickingPass.h"
#include "RenderGraph/RenderPass/SkyboxPass.h"
#include "RenderGraph/RenderPass/TransparentPass.h"
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
#include "RenderGraph/RenderPass/SceneVoxelPass.h"
#include "RenderGraph/RenderPass/OutlinePass.h"
#include "RenderGraph/RenderPass/OutlineGeometryPass.h"
#include "RenderGraph/RenderPass/SkinningPass.h"
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
#include "RenderGraph/RenderPass/ReflectionProbeCapturePass.h"
#include "RenderGraph/RenderPass/SpotShadowPass.h"
#include "RenderGraph/RenderPass/PointShadowPass.h"
#include "RenderGraph/RenderPass/PerObjectShadowPass.h"
#include "RenderGraph/RenderPass/HairShadowMarkPass.h"
#include "RenderGraph/RenderPass/FaceOverlayPass.h"
#include "RenderGraph/RenderPass/ParticlePasses.h"
#include "RenderGraph/RenderPass/TracerPasses.h"
#include "RenderGraph/RenderPass/BeamSimPass.h"
#include "RenderGraph/RenderPass/AfterimageCapturePass.h"
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
#include "Graphics/ShadingStencil.h"
#include "ECS/BeamComponent.h"
#include "ECS/VFXSpawnRequests.h"
#include "ECS/HierarchyComponents.h"
#include "ECS/FaceShadingSystem.h"

// resource system
#include "Resource/MaterialSerializer.h"

// engine systems
#include "System/Log.h"
#include "System/ThreadContext.h"
#include "System/JobDefaults.h"
#include "System/CommandLine.h"   // -mipbias_native, -fwdfog_off, -softfade_range (harness)
#include "System/Check.h"
#include "System/TaskSystem.h"
#include "System/EventBus.h"

// STL / DirectXMath
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <unordered_set>
#include <DirectXMath.h>
#include <algorithm>
#include <bit>
#include <cstring>
#include <cstdlib>
#include <execution>
#include <filesystem>
#include <cmath>

using namespace DirectX;

// CPU-side cbuffer mirrors live in Renderer.h (RendererDetail namespace) so the
// triple-buffered FrameCB<T> members can be instantiated in the class layout.
// Layouts there MUST stay in sync with the matching HLSL.
using PerViewCB       = RendererDetail::PerViewCB;
using LightCB         = RendererDetail::LightCB;
static_assert(sizeof(RendererDetail::TerrainLayerGPU) == 48,
    "TerrainLayerGPU stride drift — sync StructuredBuffer<TerrainLayerGPU> in Terrain.ps.hlsl");

// Hard-cut temporal reset (Play / Stop, a Sequencer seek, the editor's view switch):
// raises the main view's HardCut (RenderGraph/ViewState.h), which resets ReSTIR DI / GI
// and the path tracer, TAA, FSR2, DLSS and SSR. It travels with the frame being built and
// the render half applies it at that frame's start; the render half's own breaks (a
// render-path change, -restir_reset_frame) join in Render_BindFrameResources. XeGTAO, the
// fog, SSGI and the RT shadow / AO denoisers hear only the camera's cut (CameraCut, which
// Play / Stop also trigger through Camera::HardCutTo).
void Renderer::InvalidateTemporalHistory(bool quiet)
{
    // @p quiet skips the log line: the Sequencer calls this on every seek, and dragging a
    // scrubber makes every frame a seek - a line here would flood DX12Log.txt, which appends
    // across runs (the Sequencer logs the seek itself). Otherwise the view logs the
    // [VIEW_STATE] invalidate line with the histories it reached.
    RaiseViewBreak(HistoryReset::HardCut, quiet);
}

// Reasons raised on the game side wait for the hand-off of the frame being built
// (AdvanceSceneWrite). Any thread, so atomics.
void Renderer::RaiseViewBreak(uint32_t reasons, bool quiet)
{
    m_pendingViewBreaks.fetch_or(reasons, std::memory_order_acq_rel);
    if (!quiet)
        m_pendingViewBreaksLoud.fetch_or(reasons, std::memory_order_acq_rel);
}

// One view's history is never another's: the main view has one set of temporal state, so handing it to
// another camera cuts it. An Edit <-> Play edge raises the same cut on its own; both land in one frame.
void Renderer::SetMainViewOwner(uint32_t owner, const char* name)
{
    if (owner == m_mainViewOwner) return;
    const char* before = m_mainViewOwnerName;
    const bool  adopt  = m_mainViewOwner == 0;
    m_mainViewOwner     = owner;
    m_mainViewOwnerName = name ? name : "";
    if (adopt) return;
    LOG_INFO("[VIEW_STATE] view=main owner %s -> %s: temporal history cut", before, m_mainViewOwnerName);
    RaiseViewBreak(HistoryReset::HardCut);
}

// The five material passes' s0 sampler for these render and display widths
// (Graphics/MaterialSampler.h; BeginFrame passes the render width as the display one when
// no temporal upscaler runs, and calls this only on a change). The [MIPBIAS] line is the
// evidence a ratio reached the samplers. -mipbias_native (harness) keeps the native bias
// whatever the ratio: the reverse arm of the upscaler_inputs pack.
void Renderer::ApplyUpscaleMipBias(uint32_t renderW, uint32_t displayW)
{
    m_mipBiasRenderW  = renderW;
    m_mipBiasDisplayW = displayW;
    bool forcedNative = false;
#if defined(WITH_HARNESS) && WITH_HARNESS
    static const bool s_native = Sys::CommandLine::Get().Has("-mipbias_native");
    forcedNative = s_native;
#endif
    const uint32_t biasDisplayW = forcedNative ? renderW : displayW;
    if (m_gbufferPass)      m_gbufferPass->SetUpscaleMipBias(m_gfx, renderW, biasDisplayW);
    if (m_transparentPass)  m_transparentPass->SetUpscaleMipBias(m_gfx, renderW, biasDisplayW);
    if (m_frontLayerPass)   m_frontLayerPass->SetUpscaleMipBias(m_gfx, renderW, biasDisplayW);
    if (m_waterSurfacePass) m_waterSurfacePass->SetUpscaleMipBias(m_gfx, renderW, biasDisplayW);
    if (m_terrainPass)      m_terrainPass->SetUpscaleMipBias(m_gfx, renderW, biasDisplayW);
    LOG_INFO("[MIPBIAS] render=%u display=%u bias=%.3f terrainBias=%.3f forcedNative=%d",
             renderW, displayW,
             MaterialSampler::Bias(MaterialSampler::kNativeBias, renderW, biasDisplayW),
             MaterialSampler::Bias(MaterialSampler::kTerrainNativeBias, renderW, biasDisplayW),
             forcedNative ? 1 : 0);
}

void Renderer::UploadMediumSet()
{
    Media::Set set{};
    const bool global = m_volFogPass && m_volFogPass->WriteMediumGlobal(set.global);
    // The global medium's reach (pass.pathTrace.mediaMaxDistance; 0: no end) about this frame's
    // camera, written only when there is one.
    const float reach = std::max(0.f, Settings::Pass::ptMediaMaxDistance());
    if (global && reach > 0.f)
    {
        const DirectX::XMFLOAT3& camera = FrameViewRead().camera.position;
        set.global.reach = reach;
        set.camera[0] = camera.x; set.camera[1] = camera.y; set.camera[2] = camera.z;
    }
    bool height = false;
    m_features.ForEach<IMediumContributor>(
        [&set, &height](const IMediumContributor& m) { height |= m.WriteMedium(set); });
    // The local volumes follow the player's volumetric fog allowance, as the froxel fog's do.
    m_mediumLocals.clear();
    if (m_volFogPass && m_volFogPass->IsAllowed())
    {
        static_assert(sizeof(Media::Local) == sizeof(VolumetricFogPass::LocalFogVolumeGPU),
                      "Media::Local mirrors LocalFogVolumeGPU field for field");
        for (const VolumetricFogPass::LocalFogVolumeGPU& v : VolFogSceneRead().mediumLocals)
        {
            Media::Local local;
            std::memcpy(&local, &v, sizeof(local));
            m_mediumLocals.push_back(local);
        }
    }
    m_mediumIdx = m_mediumSet.Upload(m_gfx, m_gfx.GetFrameIndex(), set, m_mediumLocals);

    // Proof of life on every change of what the set holds.
    const uint32_t locals = static_cast<uint32_t>(std::min<size_t>(m_mediumLocals.size(), Media::kMaxLocals));
    const uint32_t key = (global ? 1u : 0u) | (height ? 2u : 0u) | (locals << 2) | (m_mediumIdx ? 0x80000000u : 0u);
    if (key != m_mediumLogKey)
    {
        m_mediumLogKey = key;
        LOG_INFO("[MEDIUM] global=%u height=%u locals=%u uploaded=%u", global ? 1u : 0u, height ? 1u : 0u,
                 locals, m_mediumIdx ? 1u : 0u);
    }
}

// The fog the opaque scene gets this frame, for the surfaces drawn after the fog
// composites (Graphics/FogApply.h, fog_apply.hlsli). Each composite says whether it will
// run and with what (a render feature's IForwardFogContributor - the height fog's - and
// VolumetricFogPass::ForwardScattering), so forward surfaces are fogged exactly when, and
// as, the opaque scene is. Called from RenderAsync after the frame's view-mode gates (the
// path tracer's camera primary hides the fog there too) and before the graph records.
// -fwdfog_off (harness): no forward fog - the reverse arm of fog_forward.
// -softfade_range=<m> (harness) replaces m_softFadeRange; 0 = no soft fade.
void Renderer::WriteFogApplyCB(const RendererDetail::LightCB& lb, uint32_t opaqueDepthIdx)
{
    FogApply::Constants* slot = m_fogApplyCB.Current(m_gfx);
    if (!slot) return;

    bool  fogOff = false;
    float range  = m_softFadeRange;
#if defined(WITH_HARNESS) && WITH_HARNESS
    static const bool  s_fogOff = Sys::CommandLine::Get().Has("-fwdfog_off");
    static const float s_range  = Sys::CommandLine::Get().GetFloat("-softfade_range", -1.0f);
    fogOff = s_fogOff;
    if (s_range >= 0.0f) range = s_range;
#endif

    FogApply::Constants c{};
    std::memcpy(c.invViewProj, lb.invViewProj, sizeof(c.invViewProj));
    for (int k = 0; k < 3; ++k)
    {
        c.cameraPos[k] = lb.cameraPos[k];
        c.dirToSun[k]  = -lb.lightDir[k];   // lightDir is FROM the light
        c.sunColor[k]  = lb.lightColor[k];
    }
    // VolumetricApply's NearZ / FarZ (SetFrameState) - the camera's.
    c.nearZ = FrameViewRead().view.nearZ;
    c.farZ  = FrameViewRead().view.farZ;
    const uint32_t rw = FrameViewRead().renderWidth, rh = FrameViewRead().renderHeight;
    c.invRenderSize[0] = rw ? 1.0f / static_cast<float>(rw) : 0.0f;
    c.invRenderSize[1] = rh ? 1.0f / static_cast<float>(rh) : 0.0f;

    if (!fogOff)
    {
        m_features.ForEach<IForwardFogContributor>(
            [&c](const IForwardFogContributor& fog) { fog.WriteForwardFog(c); });
        uint64_t scatterSrv = 0;
        float    froxelNear = 0.0f, froxelFar = 0.0f;
        uint32_t froxelDepth = 0;
        // The froxel volume of the view being recorded: the volumetric fog records in the main view
        // only, and its grid is the main camera's.
        const bool froxelHere = m_volFogPass && (RHI::CurrentRecordingView().slot == 0 ||
                                                 (m_volFogPass->HandledEvents() & RG::PassLifecycle::kViewSafe));
        if (froxelHere && m_volFogPass->ForwardScattering(scatterSrv, froxelNear, froxelFar, froxelDepth))
        {
            const uint32_t idx = m_gfx.DescriptorHeapIndexFromGpuHandle(scatterSrv);
            if (idx != ~0u)
            {
                c.flags      |= FogApply::kFlagFroxel;
                c.froxelIdx   = idx;
                c.froxelNear  = froxelNear;
                c.froxelFar   = froxelFar;
                c.froxelDepth = froxelDepth;
            }
        }
    }
    c.opaqueDepthIdx = opaqueDepthIdx;
    c.softFadeRange  = std::max(range, 0.0f);
    *slot = c;

    // Proof of life, on every change of what is applied (fog on/off, a fog
    // type appearing, the soft fade losing its depth): a forward fog that never
    // armed and one that works look alike until the fog is thick.
    const uint32_t key = c.flags | (c.opaqueDepthIdx ? 4u : 0u) | (fogOff ? 8u : 0u);
    if (key != m_fogApplyLogKey)
    {
        m_fogApplyLogKey = key;
        LOG_INFO("[FWDFOG] height=%u froxel=%u softDepth=%u softRange=%.3f forcedOff=%u",
                 (c.flags & FogApply::kFlagHeight) ? 1u : 0u, (c.flags & FogApply::kFlagFroxel) ? 1u : 0u,
                 c.opaqueDepthIdx ? 1u : 0u, c.softFadeRange, fogOff ? 1u : 0u);
    }
}

// The frame's intent (Renderer/FramePlan.h), on the game half right after
// RenderPhaseBarrier: the render half is not running, and what the game half changes later
// this frame (the editor UI, the scene build, Lua in the next Sim) reaches the next frame's
// plan, on both halves.
void Renderer::CaptureFrameIntent()
{
    // The render half is not running here (see the declaration): the plan it
    // last finished is complete and stays put until it starts again.
    m_planRendered    = m_plan;
    m_planRenderedFog = m_planState.fog;
    NoteAAFallback();   // a demoted DLSS request, once per change
    FramePlan::Intent& i = FrameViewWrite().planIntent;
    i = {};
    i.captured = true;
    i.path = PlanPath(m_renderPath.load(std::memory_order_relaxed));
    i.aa   = PlanAA(m_aaMode);
    const ViewMode vm = m_viewMode.load(std::memory_order_relaxed);
    i.lit           = vm == ViewMode::Lit;
    i.lightingDebug = IsLightingDebugView(vm);
    i.wireframe     = vm == ViewMode::Wireframe;
    i.rtReflections          = m_rtReflectionsEnabled.load(std::memory_order_relaxed);
    i.ssr                    = m_ssrEnabled;
    i.transparentReflections = m_transparentReflectionsEnabled.load(std::memory_order_relaxed);
    i.rtShadows = m_rtShadowsEnabled.load(std::memory_order_relaxed);
    i.rtao      = m_rtaoEnabled;
    i.ssao      = m_ssaoEnabled;
    // The game half's stage A, from this intent (GamePlan).
    m_gamePlan = FramePlan::PlanFrame(i, ReadPlanState(FrameViewWrite(), /*renderHalf=*/false), FramePlan::Facts{});
}

// The plan when it changes, and the frames a path-tracing path fell back to
// the raster features or had its path tracer vetoed (FramePlan.h, the rules).
// Called at the end of the decision block, on the render half.
void Renderer::FinishFramePlan()
{
    ++m_planFrames;
    const FramePlan::Decisions& plan = m_plan;
    const FramePlan::State& s = m_planState;
    const FramePlan::Intent& intent = FrameViewRead().planIntent;
    const FramePlan::Path path = FramePlan::EffectivePath(intent, s);
    const bool traced = path == FramePlan::Path::PathTrace || path == FramePlan::Path::PathTraceRealtime;
    const auto frame = static_cast<unsigned long long>(FrameViewRead().currentFrame);
    // A path-tracing path whose path tracer does not own the frame renders the
    // raster features as the switches have them; why, the first few times.
    if (traced && !plan.ptOwns)
    {
        ++m_planFallbacks;
        if (m_planEventLogs < 16)
        {
            ++m_planEventLogs;
            const char* why = !intent.lit ? "view" : !s.ptPass ? "pass" : s.ptInert ? "inert"
                            : !s.ptOutput ? "output" : (s.ptCameraPrimary && s.ortho) ? "ortho" : "?";
            LOG_INFO("[FRAMEPLAN] fallback frame=%llu path=%s reason=%s", frame, FramePlan::PathName(path), why);
        }
    }
    // The path tracer owned the frame and this frame's facts vetoed it: what
    // it replaces stays off for the frame.
    if (plan.ptOwns && !plan.pt)
    {
        ++m_planVetoes;
        if (m_planEventLogs < 16)
        {
            ++m_planEventLogs;
            LOG_INFO("[FRAMEPLAN] veto frame=%llu pt tlas=%u tables=%u promote=%u", frame,
                     m_planFacts.tlas ? 1u : 0u, m_planFacts.tlasTables ? 1u : 0u, m_planFacts.promote ? 1u : 0u);
        }
    }
    // The plan when it changes: one line per configuration the session ran.
    if (m_planFrames == 1 || FramePlan::DiffMask(plan, m_planPrinted) != 0)
    {
        char line[512];
        FramePlan::Format(plan, line, sizeof(line));
        LOG_INFO("[FRAMEPLAN] frame=%llu path=%s lighting=%s %s", frame, FramePlan::PathName(path),
                 FramePlan::TierName(plan), line);
        m_planPrinted = plan;
        ++m_planChanges;
    }
}

// Depth-prepass forwarding (the pass owns the state; the Renderer owns the API).
void Renderer::SetDepthPrepassEnabled(bool v)
{
    if (m_gbufferPass) m_gbufferPass->SetDepthPrepassEnabled(v);
}

// AO and volumetric fog run when the player ALLOWS them (features.ssao /
// features.volumetricFog) AND the scene wants them - Settings::Policy::Capped, the rule the
// settings store records. The player's setters below only write the allowance; the scene's
// intent arrives through the Set*SceneIntent setters (its .ippc at load,
// PostProcessConfig::ApplyTo; that file's defaults for a scene without one) and, for fog,
// the components the game half gathers.
void Renderer::SetSSAOAuthored(bool v)
{
    m_ssaoAuthored = v;
    m_ssaoEnabled  = v && m_ssaoSceneIntent;
}

void Renderer::SetSSAOSceneIntent(bool on, std::string_view writer)
{
    m_ssaoSceneIntent = on;
    m_ssaoEnabled     = m_ssaoAuthored && on;
    // Settings.where("features.ssao") shows it beside the allowance. Written
    // once per scene load / editor edit, so no change filter.
    Settings::SettingsStore& st = Settings::SettingsStore::Get();
    if (const Settings::Row* r = st.Find("features.ssao"))
        st.Write(*r, Settings::Layer::Scene, Settings::Value{ .b = on }, writer);
}

void Renderer::SetVolumetricFogAuthored(bool v)
{
    m_volFogAuthored = v;
    if (m_volFogPass) m_volFogPass->SetAllowed(v);
}

void Renderer::SetFogSceneConfigIntent(bool on, std::string_view writer)
{
    m_fogSceneConfig  = on;
    m_fogConfigWriter = std::string(writer);
    // The pass's enable flag is the scene's switch for a scene that has no Volumetric Fog
    // component (VolumetricFogPass::WantsFog). The settings store hears it from
    // GatherVolumetricFog, with the component and the local volumes that may overrule it.
    if (m_volFogPass) m_volFogPass->SetEnabled(on);
}

// At the hand-off, on main (AdvanceSceneWrite): the frame in flight's settings
// snapshot, refilled only when the settings store has moved on since that
// buffer was filled - two refills per change, one per buffer. The post-process
// settings PostProcessResolveSystem resolved this frame go into the same buffer
// index: the render half reads them as PostProcess::Runtime::Published(m_sceneRead).
void Renderer::PublishSettingsSnapshot(uint32_t emitIdx)
{
    Settings::FillSnapshot(Settings::SettingsStore::Get(), m_settingsSnap[emitIdx]);
    m_settingsSnapLast = emitIdx;
    bool settled = m_matTexUnsettled == 0 && m_candUnresolved == 0;
#if defined(WITH_HARNESS) && WITH_HARNESS
    // -scene_unsettled=<publish>:<count>: the scene reports itself unsettled for @c count hand-offs from the
    // @c publish-th, so a pack can hold a snap on a scene that has nothing left to stream.
    static const std::string s_unsettled = Sys::CommandLine::Get().Get("-scene_unsettled", "");
    static uint32_t s_publishes = 0;
    unsigned from = 0, count = 0;
    if (!s_unsettled.empty() && sscanf_s(s_unsettled.c_str(), "%u:%u", &from, &count) == 2
        && s_publishes >= from && s_publishes < from + count)
    {
        settled = false;
        if (s_publishes == from) LOG_INFO("[SETTLE] the scene reports itself unsettled from hand-off %u for %u", from, count);
    }
    ++s_publishes;
#endif
    PostProcess::Runtime::Get().SetSceneSettled(settled);
    PostProcess::Runtime::Get().Publish(emitIdx);
}
bool Renderer::IsDepthPrepassEnabled() const
{
    return m_gbufferPass && m_gbufferPass->IsDepthPrepassEnabled();
}
uint32_t Renderer::GetDepthPrepassDrawCount() const
{
    return m_gbufferPass ? m_gbufferPass->GetDepthPrepassDrawCount() : 0u;
}

// Clears every peak the pool panel reads, plus the warn-throttle latches that ride with
// them. OnWorldClear touches none of these, so without a reset the first heavy scene pins
// the readings for the session and the panel stops answering "is THIS scene close to a
// cap?". The current / last-frame counters stay: the next BuildRenderScene overwrites them,
// and zeroing them would only flash a wrong value for one frame.
void Renderer::ResetPoolWatermarks()
{
    m_instanceEmitHighWater   = 0;
    m_instanceDropHighWater   = 0;
    m_instanceOverflowFrames  = 0;
    m_instSlotHighWater       = 0;
    m_instUploadRecordsPeak   = 0;

    m_materialCopyRecordsPeak      = 0;
    m_materialBindingHighWater     = 0;
    m_customMaterialTableHighWater = 0;
    m_customMaterialCbvHighWater   = 0;
    m_runtimeMaterialEntityHighWater   = 0;
    m_runtimeMaterialResourceHighWater = 0;

    m_spotShadowCasterHighWater     = 0;
    m_spotShadowReadyHighWater      = 0;
    m_spotShadowCastersDeniedWorst  = 0;
    m_pointShadowCasterHighWater    = 0;
    m_pointShadowCastersDeniedWorst = 0;

    // The bindless mesh/buffer slot pools live one level down and share the panel's table:
    // reset them too, or half the rows stay pinned at an old peak while the rest read zero.
    m_meshMgr.GetDescriptorHeap().ResetPoolWatermarks();

    LOG_INFO("Renderer: pool watermarks reset");
}

void Renderer::EnsurePostProcessPool(PostPool& pool, uint32_t w, uint32_t h)
{
    if (w == 0 || h == 0) return;
    if (w == pool.w && h == pool.h && pool.Ready())
        return;

    // The shared HDR post-process ping-pong pool: two full-res RGBA16F buffers reused by every
    // producer in the linear post chain, rebuilt only when the resolution changes. Each slot is
    // destroyed and made again (SRV|UAV, created UNORDERED_ACCESS); its state from here is the
    // device's book's.
    for (int i = 0; i < 2; ++i)
        if (!pool.tex[i].Rebuild(m_gfx, w, h))
            LOG_ERROR("Renderer: post-process pool texture %d creation failed (%ux%u)", i, w, h);
    pool.w = w; pool.h = h;
    LOG_INFO("Renderer: post-process ping-pong pool %ux%u (2 x RGBA16F, %.1f MB total)",
             w, h, double(w) * double(h) * 8.0 * 2.0 / (1024.0 * 1024.0));
}

void Renderer::BuildGameView(uint32_t vpW, uint32_t vpH)
{
    // The viewport size is an argument: FrameViewWrite's vpWidth / vpHeight are assigned by
    // UploadFrameData, which has not run yet this frame, and a one-frame-stale aspect ratio
    // slides UI anchors during a resize. They are swapped in for the build and restored.
    const uint32_t saveW = FrameViewWrite().vpWidth, saveH = FrameViewWrite().vpHeight;
    FrameViewWrite().vpWidth  = vpW ? vpW : FrameViewWrite().vpWidth;
    FrameViewWrite().vpHeight = vpH ? vpH : FrameViewWrite().vpHeight;

    DirectX::XMMATRIX view, projNoJitter;
    BuildCameraViewProj(view, projNoJitter);

    FrameViewWrite().vpWidth = saveW; FrameViewWrite().vpHeight = saveH;

    DirectX::XMStoreFloat4x4(&m_gameView.viewMatrix, view);
    DirectX::XMStoreFloat4x4(&m_gameView.viewProjMatrixNoJitter,
                             DirectX::XMMatrixMultiply(view, projNoJitter));
    // The early editor UI build reads its camera from here (gizmos / picking need the
    // un-jittered projection).
    DirectX::XMStoreFloat4x4(&m_gameView.projMatrixNoJitter, projNoJitter);
    m_gameView.cameraPosition = FrameViewWrite().camera.position;
    m_gameView.cameraForward  = FrameViewWrite().camera.forward;
}

void Renderer::ExtractWorldUI(World& world)
{
    if (!m_worldUIPass || !m_worldUIPass->enabled) return;
    m_worldUIPass->BuildLane(world, 0,
                             FrameViewWrite().view.viewProjMatrixNoJitter,
                             FrameViewWrite().view.viewMatrix,
                             FrameViewWrite().vpWidth, FrameViewWrite().vpHeight,
                             FrameViewWrite().globalTimeSec);
    m_frameStats.worldUIVerts = m_worldUIPass->LastVertexCount();
    // Each secondary view this frame: its own lane, through its camera (WorldSpaceUISystem::Tick
    // wrote its fade, scale and cull).
    for (uint32_t slot = 1; slot < RHI::kMaxViews; ++slot)
    {
        ViewFrameCamera c;
        if (GetViewFrameCamera(slot, c))
            m_worldUIPass->BuildLane(world, slot, c.viewProjNoJitter, c.view, c.width, c.height,
                                     FrameViewWrite().globalTimeSec);
    }
}

DrawList Renderer::GetDrawList(DrawFilter f) const
{
    const auto* begin = DrawPacketsRead().data();
    const auto* end   = begin + DrawPacketsRead().size();
    const auto* first = std::find_if(begin, end,
        [f](const DrawPacket& dp) { return dp.filter == f; });
    if (first == end) return {};
    const auto* last = std::find_if(first, end,
        [f](const DrawPacket& dp) { return dp.filter != f; });
    return { first, static_cast<size_t>(last - first) };
}

void Renderer::EnsureWorkers()
{
    if (m_workersStarted) return;




    m_workersStarted = true;
    LOG_INFO("Renderer: Shadow and Compute recording use shared task graph");
}

void Renderer::ReloadShaders()
{
    // Standalone first: every pass and system outside m_graph that owns
    // shaders registered OnReloadShaders where Compile created it
    // (RenderLifecycle.h). A holder missing from that list keeps its old
    // bytecode until a restart, and the reload's receipt says so
    // (restart_required, ShaderRecipes::CheckCoverage). Each rebuilds its PSOs
    // onto the live members, so a failed rebuild keeps the pipeline it had.
    m_lifecycle.Run(RenderLifecycle::Event::ReloadShaders);

    // The graph passes (RTShadowPass and RTAOPass among them).
    m_graph.ReloadShaders(m_gfx);

    // Then the preview graph, after the main one: GBufferPass#2 borrows the main instance's
    // ShaderLibrary, so the owner must have cleared and re-warmed it before the borrower
    // rebuilds PSOs against it. The borrower clears only its own PSOCache (GBufferPass gates the
    // library ClearCaches on OwnsShaderLibrary()), so the reload epoch MaterialReflectionSync
    // stamps against is bumped exactly once. Without this call the preview keeps stale PSOs
    // after a shader edit while the viewport updates.
    if (m_preview) m_preview->ReloadShaders(m_gfx);

    // A hot-reloaded custom PS/VS can change its b8 reflection layout while its dynamic shader
    // ID (part of MaterialBindingKey) stays the same, so the persistent store would pack against
    // a stale layout. Arm the full slot reset: every key re-acquires a fresh slot (old slots
    // park in the deferred-reuse queue), so the repack never rewrites a region in-flight frames
    // still read - the same no-in-place-rewrite contract as the base MaterialGPUData slots.
    // Cost: one full-rebuild frame.
    m_persistentMatSlotsReset = true;
    std::fill(m_matSlotCustomPacked.begin(), m_matSlotCustomPacked.end(),
              static_cast<uint8_t>(0));
    std::fill(m_matSlotCustomOversize.begin(), m_matSlotCustomOversize.end(),
              static_cast<uint8_t>(0));
}

RHI::CommandList Renderer::Render()
{
    Sys::OwnerTaskQueue owner;
    RHI::CommandList result{};
    owner.RunUntil(RenderAsync(owner, {}, result).Start(owner, {}, "Renderer.Render"));
    return result;
}

// Hi-Z is last frame's pyramid: usable only once a frame has generated it and the view's
// content did not cut (the camera's cut, a hard cut, a world clear - HistoryReset::Cuts);
// a pyramid of a different view would wrongly cull for one frame. Also gated on per-frame
// camera motion: editor focus teleports and animated camera cuts move the view without a
// cut, and one frame of occlusion-off is invisible while a frame of mass false culls is not.
// Render half; InstanceCull (RenderAsync) and the meshlet amplification shader
// (WriteMeshletCullCB) ask the same frame the same question, so the instance and the
// cluster level never disagree.
bool Renderer::HiZUsableThisFrame() const
{
    if (!m_hiZEnabled || !m_hiZPass || m_mainView.hiz.framesValid == 0 ||
        (m_mainView.FrameBreaks() & HistoryReset::Cuts))
        return false;
    const float dxp = FrameViewRead().camera.position.x - m_mainView.hiz.camPos.x;
    const float dyp = FrameViewRead().camera.position.y - m_mainView.hiz.camPos.y;
    const float dzp = FrameViewRead().camera.position.z - m_mainView.hiz.camPos.z;
    const float fwdDot =
        FrameViewRead().view.cameraForward.x * m_mainView.hiz.camFwd.x +
        FrameViewRead().view.cameraForward.y * m_mainView.hiz.camFwd.y +
        FrameViewRead().view.cameraForward.z * m_mainView.hiz.camFwd.z;
    return (dxp*dxp + dyp*dyp + dzp*dzp) < 1.0f &&
           fwdDot > 0.9995f; // ~1.8 deg per frame
}

void Renderer::InitMeshManager()
{
    m_meshMgr.Init(m_gfx);
    m_meshMgr.InitPrimitives();
    // The billboard quad is created in InitSkinningSystems, with the pass that uses it.
}

const ShaderReflect::Reflection*
Renderer::GetCustomShaderReflection(int customShaderID, const MaterialComponent* mc) const
{
    if (customShaderID <= 0 || !m_gbufferPass) return nullptr;
    return LookupCustomShaderReflection(static_cast<uint32_t>(customShaderID), mc);
}

ShaderLibrary::DynamicReads Renderer::GetCustomShaderReads(int customShaderID) const
{
    ShaderLibrary::DynamicReads reads;
    if (customShaderID <= 0) return reads;
    ShaderLibrary* libs[] = { m_gbufferPass ? &m_gbufferPass->GetShaderLibrary() : nullptr,
                              m_transparentPass ? &m_transparentPass->GetShaderLibrary() : nullptr };
    for (ShaderLibrary* lib : libs)
    {
        if (!lib) continue;
        ShaderLibrary::DynamicReads r = lib->GetDynamicReads(static_cast<uint32_t>(customShaderID));
        reads.known = reads.known || r.known;
        reads.vars.insert(r.vars.begin(), r.vars.end());
        reads.bindings.insert(r.bindings.begin(), r.bindings.end());
    }
    return reads;
}

ShaderLibrary* Renderer::GetMaterialShaderLibrary() const
{
    return m_gbufferPass ? &m_gbufferPass->GetShaderLibrary() : nullptr;
}

uint64_t Renderer::GetTextureContentRevision() const
{
    return m_texSys ? m_texSys->ContentRevision() : 0;
}

uint32_t Renderer::GetMaterialShaderReloadEpoch() const
{
    return m_gbufferPass ? m_gbufferPass->GetShaderLibrary().GetReloadEpoch() : 0;
}

std::size_t Renderer::ShaderPoolUsed() const
{
    return static_cast<const GraphicsDX12&>(m_gfx).ShaderPoolUsed();
}

std::size_t Renderer::ShaderPoolCapacity() const
{
    return static_cast<const GraphicsDX12&>(m_gfx).ShaderPoolCapacity();
}

std::string
Renderer::GetCustomShaderError(int customShaderID) const
{
    if (customShaderID <= 0 || !m_gbufferPass) return {};
    return m_gbufferPass->GetShaderLibrary()
        .GetDynamicCompileError(static_cast<uint32_t>(customShaderID));
}

bool Renderer::CaptureViewportToPNG(const char* path)
{
    if (!m_toneMapPass) return false;
    const RHI::Texture* tex = m_toneMapPass->GetFinalOutputTexture();
    if (!tex || !tex->IsValid()) return false;
    // Tonemap sits in SHADER_RESOURCE post-composite; capture roundtrips through COPY_SOURCE.
    return m_gfx.CaptureTextureToPNG(*tex, RHI::ResourceState::SHADER_RESOURCE, path);
}

// GetAttachmentSource: Renderer_DebugTargets.cpp (the debug target registry).

Renderer::AttachmentCamera Renderer::GetAttachmentCamera() const
{
    // FrameViewEmit: the view the frame in flight was emitted with - the one
    // the GBuffer holds - not FrameViewWrite, which after the hand-off is the
    // buffer the next frame's SetCamera writes.
    const FrameView& fv = FrameViewEmit();
    AttachmentCamera c;
    c.nearZ        = fv.camera.nearZ;
    c.farZ         = fv.camera.farZ;
    c.fovY         = fv.camera.fov;
    c.orthographic = fv.camera.orthographic;
    c.position[0]  = fv.camera.position.x; c.position[1] = fv.camera.position.y; c.position[2] = fv.camera.position.z;
    c.forward[0]   = fv.camera.forward.x;  c.forward[1]  = fv.camera.forward.y;  c.forward[2]  = fv.camera.forward.z;
    c.renderWidth  = fv.renderWidth;
    c.renderHeight = fv.renderHeight;
    return c;
}

bool Renderer::ReadFrameExposure(float& exposure, bool& autoExposure, std::string& error)
{
    exposure = 0.f;
    autoExposure = false;
    if (!m_autoExposurePass || !m_autoExposurePass->GetExposureBuffer().IsValid())
    {
        error = "no auto-exposure pass / exposure buffer in this renderer";
        return false;
    }
    autoExposure = m_autoExposurePass->IsEnabled();
    m_gfx.DrainCpuFrame();   // the pass's tracked state is the render lane's until then
    std::vector<std::uint8_t> bytes;
    if (!m_gfx.ReadBufferRaw(m_autoExposurePass->GetExposureBuffer(), m_autoExposurePass->GetExposureBufferState(),
                             0, sizeof(float), bytes, error))
        return false;
    std::memcpy(&exposure, bytes.data(), sizeof(float));
    return true;
}

// Current-frame camera matrices from the FrameView's camera (FrameViewWrite for the
// member forms). UploadFrameData builds its view / proj through these helpers, and the
// scene-build consumers derive identical matrices earlier in the frame: keep them in
// lockstep.
void Renderer::BuildCameraBasis(XMVECTOR& outPos, XMVECTOR& outForward,
                                XMVECTOR& outUp) const
{
    BuildCameraBasisFor(FrameViewWrite(), outPos, outForward, outUp);
}

void Renderer::BuildCameraBasisFor(const FrameView& fv, XMVECTOR& outPos,
                                   XMVECTOR& outForward, XMVECTOR& outUp)
{
    outForward = XMVector3Normalize(XMLoadFloat3(&fv.camera.forward));
    outPos     = XMLoadFloat3(&fv.camera.position);
    // Camera up: usually world-up, but cinematic / camera-motion cameras carry a rolled up
    // vector (banked / Dutch-angle shots). Fall back to world-up if the supplied up is
    // degenerate (~parallel to forward).
    XMVECTOR up = XMVector3Normalize(XMLoadFloat3(&fv.camera.up));
    if (XMVector3IsNaN(up) ||
        std::abs(XMVectorGetX(XMVector3Dot(up, outForward))) > 0.999f)
        up = XMVectorSet(0.f, 1.f, 0.f, 0.f);
    outUp = up;
}

void Renderer::BuildCameraViewProj(XMMATRIX& outView, XMMATRIX& outProjNoJitter) const
{
    BuildCameraViewProjFor(FrameViewWrite(), outView, outProjNoJitter);
}

void Renderer::BuildCameraViewProjFor(const FrameView& fv, XMMATRIX& outView,
                                      XMMATRIX& outProjNoJitter)
{
    XMVECTOR pos, forward, up;
    BuildCameraBasisFor(fv, pos, forward, up);

    const float aspect = (fv.vpHeight != 0)
        ? static_cast<float>(fv.vpWidth) / static_cast<float>(fv.vpHeight)
        : 1.0f;
    outView = XMMatrixLookToLH(pos, forward, up);
    // Reversed-Z: near/far swapped (NDC z=1 near, 0 far) + GREATER_EQUAL test.
    if (fv.camera.orthographic)
    {
        const float halfH = std::max(fv.camera.orthoHalfHeight, 0.01f);
        outProjNoJitter = XMMatrixOrthographicLH(halfH * aspect * 2.f, halfH * 2.f,
                                                 fv.camera.farZ, fv.camera.nearZ);
    }
    else
    {
        outProjNoJitter = XMMatrixPerspectiveFovLH(fv.camera.fov, aspect,
                                                   fv.camera.farZ, fv.camera.nearZ);
    }
}


