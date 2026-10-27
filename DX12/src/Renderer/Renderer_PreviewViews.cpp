// Renderer_PreviewViews.cpp - the Renderer half of the preview lane: preview
// views and thumbnails (thin forwards to PreviewRenderer), the preview
// environment, scene lights, meshes and cameras, the secondary frusta, the Game
// picture-in-picture, CullWorldForPreview and the preview queue.
// MakeSecondaryViewProj is file-local: nothing else calls it.

#include <optional>
#include "Renderer/Renderer.h"
#include "PostProcess/PostProcessResolve.h"   // ResolveAt: each secondary view's own blend

// engine graphics / backend
#include "Graphics/GraphicsDX12.h"
#include "Graphics/ShadowSystem.h"
#include "Graphics/ParticleSystem.h"
#include "Graphics/AfterimageSystem.h"
#include "Graphics/TrailSystem.h"

// render passes
#include "Renderer/Preview/PreviewRenderer.h"
#include "Renderer/Preview/PreviewStudioRig.h"
#include "RenderGraph/RenderPass/DDGIProbeDebugPass.h"
#include "RenderGraph/RenderPass/ShadowPass.h"
#include "RenderGraph/RenderPass/AutoExposurePass.h"
#include "RenderGraph/RenderPass/NPRStabilizePass.h"
#include "RenderGraph/RenderPass/FSR2Pass.h"
#include "RenderGraph/RenderPass/DLSSPass.h"
#include "Graphics/WaterRippleSystem.h"
#include "RenderGraph/RenderPass/ClusterPass.h"
#include "RenderGraph/RenderPass/BillboardFXPass.h"
#include "RenderGraph/RenderPass/WorldUIBillboardPass.h"

// post-process
#include "PostProcess/PostChain.h"
#include "PostProcess/BuiltinPostProcessEffects.h"
#include "PostProcess/PostProcessRuntime.h"

// ECS components + systems
#include "ECS/ECS.h"
#include "ECS/Components.h"
#include "ECS/CameraStackComponents.h"  // VirtualCameraComponent: the preview camera's lens
#include "ECS/HierarchyComponents.h"
#include "ECS/FaceShadingSystem.h"

// resource system
#include "Resource/MaterialSerializer.h"

// engine systems
#include "System/Log.h"
#include "System/JobDefaults.h"
#include "System/Check.h"     // ENGINE_CHECK_ONCE: the unusable-lens tripwire
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


// ==== Material preview lane ====
// Each function here is a thin forward to PreviewRenderer: with no panel open
// m_preview is null and each of these costs one branch.
bool Renderer::IsPreviewEnabled() const
{
    // Read once into a function-local static (_dupenv_s: getenv is a deprecation
    // error in this warning set). Default is ON: only DX12_PREVIEW=0 turns it off.
    static const bool s_envEnabled = []() -> bool
    {
        char*  buf = nullptr;
        size_t len = 0;
        bool   on  = true;
        if (_dupenv_s(&buf, &len, "DX12_PREVIEW") == 0 && buf)
        {
            on = (std::atoi(buf) != 0);
            std::free(buf);
            if (!on)
                LOG_WARNING("[PREVIEW] disabled by DX12_PREVIEW=0 - no preview "
                            "layer will be constructed (plan sec.4.9.1 kill switch)");
        }
        return on;
    }();
    // Both halves in one answer: reporting only the env half would let a
    // preview.enabled=0 session tell Debug.Stats() the feature is on while the
    // widget refuses to open a panel.
    return s_envEnabled && m_previewPrefEnabled.load(std::memory_order_relaxed);
}

void Renderer::SetPreviewPrefEnabled(bool v)
{
    const bool was = m_previewPrefEnabled.exchange(v, std::memory_order_relaxed);
    if (was != v)
        LOG_INFO("[PREVIEW] editor_prefs preview.enabled=%d (kill switch half 2; "
                 "env half is DX12_PREVIEW)", v ? 1 : 0);
}

uint32_t Renderer::AcquirePreviewView(bool worldCull)
{
    // Gate at the construction site, not in ProcessPreviewQueue: an early-out in
    // the record half would still pay for the targets and the ToneMapPass instance.
    if (!IsPreviewEnabled()) return Preview::kInvalidView;
    if (!m_preview)
    {
        m_preview = std::make_unique<PreviewRenderer>(m_gfx, *this);
        if (m_previewIdleFramesOverride)
            m_preview->SetIdleTeardownFrames(m_previewIdleFramesOverride);
        // Push the latched key-light override the moment the layer exists, like the
        // idle-grace latch above.
        if (m_previewKeyLightSet)
            m_preview->SetKeyLightDirOverride(m_previewKeyLightDir);
        // The latched shadow alpha-test lever. Only OFF is pushed: true is the pass's
        // own default, so a layer built with nothing latched behaves as shipped.
        if (!m_previewShadowAlphaTest)
            m_preview->SetStudioShadowAlphaTest(false);
        // The latched world scene-light off-arm, same one-sided push (on is the
        // default).
        if (!m_previewWorldSceneLights)
            m_preview->SetWorldSceneLightsEnabled(false);
    }
    const uint32_t id = m_preview->AcquireView(worldCull);
    // The one-shot cull diagnostic belongs to one tenancy of the WorldCull lane;
    // Renderer owns this half of it (PreviewRenderer the rest), so re-arm it for
    // every new WorldCull view.
    if (worldCull && id != Preview::kInvalidView) m_previewCullLogged = false;
    return id;
}

bool Renderer::RequestPreviewWorldCullLaneSize(uint32_t w, uint32_t h,
                                               const uint32_t* views,
                                               uint32_t count)
{
    return m_preview &&
           m_preview->RequestWorldCullLaneSize(w, h, views, count);
}

void Renderer::GetPreviewWorldCullSize(uint32_t& outW, uint32_t& outH) const
{
    outW = outH = 0;
    if (m_preview) m_preview->GetWorldCullSize(outW, outH);
}

bool Renderer::IsPreviewViewSizeRefused(uint32_t viewId) const
{
    return m_preview && m_preview->IsViewSizeRefused(viewId);
}

bool Renderer::GetPreviewOutputSize(uint32_t viewId, uint32_t& outW,
                                    uint32_t& outH) const
{
    outW = outH = 0;
    return m_preview && m_preview->GetOutputSize(viewId, outW, outH);
}

void Renderer::SetPreviewPriorityView(uint32_t viewId)
{
    if (m_preview) m_preview->SetPriorityView(viewId);
}

void Renderer::ReleasePreviewView(uint32_t viewId)
{
    if (!m_preview) return;
    if (m_preview->IsWorldCullSlot(viewId)) m_previewCullLogged = false;
    m_preview->ReleaseView(viewId);
}

void Renderer::RequestPreview(uint32_t viewId, const Preview::ViewDesc& v,
                              const MaterialComponent& mat)
{
    if (m_preview) m_preview->SetRequest(viewId, v, mat);
}

uint64_t Renderer::GetPreviewSrv(uint32_t viewId) const
{
    return m_preview ? m_preview->GetSrv(viewId) : 0ull;
}

const RHI::Texture* Renderer::GetPreviewOutputTexture(uint32_t viewId) const
{
    return m_preview ? m_preview->GetOutputTexture(viewId) : nullptr;
}

uint32_t Renderer::GetLastRenderedPreviewView() const
{
    return m_preview ? m_preview->LastRenderedView() : Preview::kInvalidView;
}

uint32_t Renderer::GetPreviewViewLease(uint32_t viewId) const
{
    return m_preview ? m_preview->ViewLease(viewId) : 0u;
}

uint64_t Renderer::GetPreviewViewRenderCount(uint32_t viewId) const
{
    return m_preview ? m_preview->ViewRenderCount(viewId) : 0ull;
}

bool Renderer::GetPreviewViewRequestSize(uint32_t viewId, uint32_t& outW, uint32_t& outH) const
{
    outW = outH = 0;
    return m_preview && m_preview->GetViewRequestSize(viewId, outW, outH);
}

bool Renderer::IsPreviewWorldCullSlot(uint32_t viewId) const
{
    return m_preview && m_preview->IsWorldCullSlot(viewId);
}

uint64_t Renderer::GetPreviewViewsRenderedTotal() const
{
    return m_preview ? m_preview->ViewsRenderedTotal() : 0ull;
}

uint32_t Renderer::GetLastRenderedPreviewStencil() const
{
    return m_preview ? m_preview->LastRenderedStencil() : 0u;
}

uint32_t Renderer::GetLastRenderedPreviewForward() const
{
    return m_preview ? m_preview->LastRenderedForward() : 0u;
}

void Renderer::ServicePreviewReleases()
{
    if (m_preview) m_preview->ServiceReleases();
}

void Renderer::DrainPreviewViewReleases()
{
    if (m_preview) m_preview->DrainPendingViewDestroys();
}

void Renderer::SetPreviewIdleTeardownFrames(uint32_t frames)
{
    // Latched even with no layer yet: the harness parses its flags before any
    // panel opens and the layer is built lazily on the first acquire.
    // AcquirePreviewView / QueuePreviewThumbnail push it into the new layer.
    m_previewIdleFramesOverride = frames;
    if (m_preview) m_preview->SetIdleTeardownFrames(frames);
}

#ifdef WITH_HARNESS
void Renderer::SetPreviewMeshLibCap(std::size_t cap)
{
    // No latch needed: the cap is read by TrimPreviewMeshLibs on this object.
    if (cap == 0) return;
    m_previewMeshLibCap = cap;
    LOG_INFO("[PREVIEW] mesh library cache cap = %zu (shipped default %zu) - "
             "harness override", m_previewMeshLibCap, kPreviewMeshLibCap);
}

void Renderer::SetPreviewIblDelayFrames(uint32_t frames)
{
    // Latched here and read by SyncPreviewEnvironment on this object; no preview
    // layer need exist yet.
    m_previewIblDelayFrames = frames;
    LOG_INFO("[PREVIEW] studio IBL publication delayed by %u frame(s) - "
             "harness override (-preview_ibl_delay)", frames);
}

bool Renderer::SetPreviewKeyLightDir(float x, float y, float z)
{
    // Latched like SetPreviewIdleTeardownFrames: the flag is parsed before the
    // lazily built layer exists. Applied immediately when the layer exists.
    const float d[3] = { x, y, z };
    if (!(x * x + y * y + z * z > 1e-8f)) return false;
    m_previewKeyLightSet    = true;
    m_previewKeyLightDir[0] = x;
    m_previewKeyLightDir[1] = y;
    m_previewKeyLightDir[2] = z;
    if (m_preview) return m_preview->SetKeyLightDirOverride(d);
    LOG_INFO("[PREVIEW] studio key light override latched: travel (%.3f, %.3f, "
             "%.3f) (-preview_key_light)", x, y, z);
    return true;
}

void Renderer::SetPreviewStudioShadowAlphaTest(bool on)
{
    // Latched for the pre-layer case like the key-light override, and forwarded
    // immediately when the layer is up - the usual case, since the fixture schedule
    // flips the lever between two windows of one run.
    m_previewShadowAlphaTest = on;
    if (m_preview) m_preview->SetStudioShadowAlphaTest(on);
    else
        LOG_INFO("[PREVIEW] studio shadow alpha test latched: %s "
                 "(-preview_shadow_alpha)", on ? "ON (shipped)" : "OFF");
}
#endif

void Renderer::ServicePreviewCompile()
{
    // Never call this from Renderer::Render. App calls it from the game half's
    // serial window, beside ServicePreviewReleases: RenderGraph::Compile recreates
    // every physical texture and runs each pass's Init()/OnCompile() serially, all
    // of which allocates descriptors - which must not happen on or beside a graph
    // worker.
    if (!m_preview) return;
    // The preview's GBufferPass borrows the main GBufferPass's library (null before
    // Compile()), so every custom material's customPSID resolves to the same
    // bytecode as in the main viewport; a private library would resolve the ids
    // against a registry that never saw the same registration sequence.
    // TransparentPass's library is a second, separate borrow for the forward-domain
    // instance: a translucent material's custom PS is a 1-RTV forward binary and
    // must not resolve into a 6-RTV GBuffer PSO.
    m_preview->ServiceCompile(m_gbufferPass, m_transparentPass);
}

// ==== Asset browser thumbnail lane: thin forwards, game half ====
bool Renderer::QueuePreviewThumbnail(const Preview::ThumbRequest& r)
{
    // Same construction gate as AcquirePreviewView: without it a browser full of
    // material tiles would build the preview layer in a session that turned the
    // feature off.
    if (!IsPreviewEnabled()) return false;
    if (!m_preview)
    {
        m_preview = std::make_unique<PreviewRenderer>(m_gfx, *this);
        if (m_previewIdleFramesOverride)
            m_preview->SetIdleTeardownFrames(m_previewIdleFramesOverride);
        // Push the latched key-light override into the new layer, as
        // AcquirePreviewView does.
        if (m_previewKeyLightSet)
            m_preview->SetKeyLightDirOverride(m_previewKeyLightDir);
        // The latched shadow alpha-test lever: only OFF is pushed (true is the pass
        // default).
        if (!m_previewShadowAlphaTest)
            m_preview->SetStudioShadowAlphaTest(false);
        // The latched world scene-light off-arm: only OFF is pushed (on is the
        // default).
        if (!m_previewWorldSceneLights)
            m_preview->SetWorldSceneLightsEnabled(false);
    }
    return m_preview->QueueThumbnail(r);
}

void Renderer::CancelPreviewThumbnail(uint64_t assetKey)
{
    if (m_preview) m_preview->CancelThumbnail(assetKey);
}

bool Renderer::HasPreviewThumbnailInFlight(uint64_t assetKey) const
{
    return m_preview ? m_preview->HasThumbnailInFlight(assetKey) : false;
}

bool Renderer::ArePreviewThumbnailsDisabled() const
{
    // No layer is not "disabled": a session that never showed a material tile has
    // no PreviewRenderer, and answering true would stop the browser from ever
    // creating the first one.
    return m_preview ? m_preview->ThumbnailsDisabled() : false;
}

std::size_t Renderer::PreviewThumbnailQueueDepth() const
{
    return m_preview ? m_preview->ThumbnailQueueDepth() : 0u;
}

void Renderer::TakePreviewThumbnailResults(std::vector<Preview::ThumbResult>& out)
{
    if (m_preview) m_preview->TakeThumbnailResults(out);
}

bool Renderer::TakePreviewThumbnailPixels(uint64_t& outAssetKey,
                                          uint32_t& outGeneration,
                                          const uint8_t*& outRgba,
                                          uint32_t& outRowPitch,
                                          uint32_t& outWidth, uint32_t& outHeight)
{
    if (!m_preview) return false;
    return m_preview->TakeThumbnailPixels(outAssetKey, outGeneration, outRgba,
                                          outRowPitch, outWidth, outHeight);
}

uint64_t Renderer::GetPreviewThumbnailsRenderedTotal() const
{
    return m_preview ? m_preview->ThumbnailsRenderedTotal() : 0ull;
}

// ==== Preview studio environment ====
// Game half: TextureSystem's Acquire / IsReady / GetTexture are main-thread-only,
// so the record half receives resolved uint64 handles.
// The studio HDRI, not the live scene's sky, is the default: a preview lit by
// the scene changes with time-of-day, which makes golden images and cached
// thumbnails meaningless. The HDRI choice is argued in PreviewStudioRig.h.
void Renderer::SyncPreviewEnvironment()
{
    if (!m_preview || !m_texSys || !m_resMgr) return;

    if (!m_previewIblRequested)
    {
        m_previewIblRequested = true;
        m_previewIrrHandle = m_texSys->Acquire(Preview::kIrradiancePath, *m_resMgr, m_gfx);
        m_previewRadHandle = m_texSys->Acquire(Preview::kRadiancePath,   *m_resMgr, m_gfx);
        LOG_INFO("[PREVIEW] studio IBL requested: %s", Preview::kIrradiancePath);
    }

    uint64_t irr = 0, rad = 0;
    uint32_t radMips = 1;
    if (m_previewIrrHandle != Resource::kInvalidTextureHandle &&
        m_texSys->IsReady(m_previewIrrHandle))
        if (const RHI::Texture* t = m_texSys->GetTexture(m_previewIrrHandle))
            irr = m_gfx.GetTextureSRVGpuHandle(*t);
    if (m_previewRadHandle != Resource::kInvalidTextureHandle &&
        m_texSys->IsReady(m_previewRadHandle))
        if (const RHI::Texture* t = m_texSys->GetTexture(m_previewRadHandle))
        {
            rad     = m_gfx.GetTextureSRVGpuHandle(*t);
            // Prefiltered roughness chain length, read off the resource: Lighting.ps
            // clamps its specular mip by it and an over-count samples past the tail.
            radMips = t->desc.mipLevels ? t->desc.mipLevels : 1u;
        }

#ifdef WITH_HARNESS
    // Harness: force the late-IBL race deterministically. The studio HDRI streams,
    // so whether it resolves before or after the panel's first render depends on
    // the disk, and the late case changes the picture - which is why
    // PreviewRenderer::SetEnvironment has a readiness edge. To exercise that edge,
    // hold the resolved pair back for N calls: the preview draws against 0/0 (its
    // black-cube fallback), then the edge fires with afterFirstRender=1.
    // Only the publication is held back, not the load (the texture is already
    // Ready), so this adds no I/O timing. 0 (the default) is inert.
    if (m_previewIblDelayFrames > 0 && irr && rad &&
        m_previewIblHeldFrames < m_previewIblDelayFrames)
    {
        ++m_previewIblHeldFrames;
        if (m_previewIblHeldFrames == 1)
            LOG_INFO("[PREVIEW] studio IBL held back for %u frame(s) "
                     "(-preview_ibl_delay) - the panel will draw against the "
                     "black-cube fallback first and the readiness edge must "
                     "re-render it",
                     m_previewIblDelayFrames);
        irr = rad = 0;
        radMips = 1;
    }
#endif

    // Logged once when the pair first resolves; the parity pack asserts the text.
    // Lighting.ps clamps the specular mip by LightCB.iblRadianceMips, the main
    // pipeline publishes the authored SkyboxComponent::radianceMipLevels (engine
    // default 7), and the parity fixture authors Preview::kRadianceMipLevels - this
    // line keeps that constant from drifting away from the shipped HDRI.
    if (irr && rad && !m_previewIblLogged)
    {
        m_previewIblLogged = true;
        LOG_INFO("[PREVIEW] studio IBL ready: radiance mips %u", radMips);
    }

    // Both cubes or neither: half an IBL (lit diffuse, black specular) looks like
    // a broken metal preview for the frames it lasts.
    if (!irr || !rad) { irr = rad = 0; radMips = 1; }

    // m_brdfLutSrv is refreshed by SyncSkyboxIBL every frame and is 0 until
    // engine/ibl/BRDF_LUT.itex is Ready. t8 has no fallback and Lighting.ps's
    // indirect block does not gate on the LUT, so the preview keeps the indirect
    // path off while it is 0.
    m_preview->SetEnvironment(irr, rad, radMips, m_brdfLutSrv);

    // The "use scene lighting" alternative, pushed every frame: the toggle
    // (Feature::kSceneLighting) lives in the ViewDesc and is read by the record
    // half, so an unconditional snapshot (one struct copy) avoids a cross-half read.
    // All zeroes is meaningful: the record half reads it as "no sky, fall back to
    // the studio rig", which is what a None / FlatColor WorldEnvironment publishes.
    // The sun comes from the live LightCB upload slot: BuildScene_UploadLights has
    // run this frame (before ExtractPreviewRequests) and its lightDir is in the
    // TRAVEL convention PreviewStudioRig::kKeyLight.dir uses. Resolving the sun
    // again would duplicate SetLightCBSun's time-of-day and atmosphere-transmittance
    // folding.
    PreviewRenderer::SceneLighting sl{};
    sl.irradiance   = m_sceneIblIrradiance;
    sl.radiance     = m_sceneIblRadiance;
    sl.radianceMips = m_sceneIblRadianceMips;
    sl.skyboxCube   = m_sceneSkyboxCube;
    sl.skySH        = m_sceneSkySH;
    sl.skyUseSH     = m_sceneSkyUseSH;
    sl.iblStrength  = m_sceneIblStrength;
    if (const auto* lb = m_lightCB.Upload(m_gfx))
    {
        sl.sunDir[0]   = lb->lightDir[0];
        sl.sunDir[1]   = lb->lightDir[1];
        sl.sunDir[2]   = lb->lightDir[2];
        sl.sunColor[0] = lb->lightColor[0];
        sl.sunColor[1] = lb->lightColor[1];
        sl.sunColor[2] = lb->lightColor[2];
        sl.ambientColor[0] = lb->ambientColor[0];
        sl.ambientColor[1] = lb->ambientColor[1];
        sl.ambientColor[2] = lb->ambientColor[2];
        sl.ambientIntensity = lb->ambientIntensity;
    }
    m_preview->SetSceneLighting(sl);
}

// The scene's punctual lights per live WorldCull view - the game half of the
// seam. m_frameLights is World-derived and the render half must never read it,
// while only the render half knows which view gets the lane's upload slot. So
// the cull runs here per view, and PreviewRenderer::WriteWorldClusterLights
// writes the one view ProcessQueue picked.
// The step order is the contract:
//   1.  drop directionals - AccumClusteredLight ignores type 0 and the sun
//       arrives through the LightCB copy, so this frees cap budget and cannot
//       change a pixel.
//   1b. drop non-finite inputs - a NaN position or radius passes every plane
//       test, and a NaN score breaks step 3's strict weak ordering (UB, not a
//       wrong order). Counted as `nonfinite=`, never clamped.
//   2.  frustum-test against the exact planes ExtractPreviewCameras built for
//       this view: a light whose sphere misses it reaches no rasterised pixel.
//   3.  cap by importance, only when over Preview::kMaxPreviewLights.
//   4.  restore source order (load-bearing): the kept set must be a subsequence
//       of m_frameLights. AccumClusteredLight accumulates in list order and the
//       main ClusterCull writes ascending indices, so any other order changes
//       the float accumulation and moves the parity floor off maxDelta 1. The
//       score chooses which K, never the order.
void Renderer::ExtractPreviewSceneLights()
{
    if (!m_preview) return;
    // Zero idle cost: before m_frameLights is touched, a game with no PiP or an
    // editor with only a material inspector open pays exactly this compare.
    if (m_preview->LiveWorldCullViewCount() == 0) return;

    // The eye for the distance term is each view's own camera, not the main one:
    // the two can face opposite ends of the level.
    for (uint32_t v = 0; v < Preview::kMaxPreviews; ++v)
    {
        if (!m_preview->IsWorldCullView(v))      continue;
        // A view whose camera ExtractPreviewCameras refused (no size, no lens, a
        // degenerate lens) has no planes and no picture this frame. Pushing 0 lights
        // keeps its stamp fresh, so the render half's refusal counter does not read it
        // as the off-by-one alarm.
        if (!m_previewLightPlanesValid[v])
        {
            m_preview->SetViewSceneLights(v, nullptr, 0, 0, 0, 0, 0, 0);
            continue;
        }

        const FrustumPlanes&     planes = m_previewLightPlanes[v];
        const DirectX::XMFLOAT3& eye    = m_previewLightEye[v];

        m_previewLightScore.clear();
        uint32_t sceneCount = 0;   // punctual lights the scene had
        uint32_t culled     = 0;   // rejected by the frustum test
        uint32_t nonFinite  = 0;   // refused for a NaN / infinite input

        for (uint32_t i = 0; i < static_cast<uint32_t>(m_frameLights.size()); ++i)
        {
            const ResolvedLight& L = m_frameLights[i];
            if (L.type == LightType::Directional) continue;   // step 1
            ++sceneCount;
            // Step 1b: refuse non-finite inputs. std::nth_element below needs a strict
            // weak ordering, which a NaN score breaks (UB, not a bounded wrong order), and
            // a NaN position or radius makes every FrustumTestSphere plane test false, so
            // the light would pass step 2 and reach the GPU. A corrupt transform is
            // reachable; skip rather than clamp (there is no defensible clamp target) and
            // count it, so the content bug shows as `nonfinite=` on the log line and the
            // shutdown summary.
            if (!std::isfinite(L.position.x) || !std::isfinite(L.position.y) ||
                !std::isfinite(L.position.z) || !std::isfinite(L.radius))
            {
                ++nonFinite;
                continue;
            }
            if (!FrustumTestSphere(planes, L.position, L.radius))   // step 2
            {
                ++culled;
                continue;
            }
            // Importance score, used only to choose K: radiant power over the squared
            // distance from the eye to the light's sphere (not its centre), so a big dim
            // lamp the camera stands inside outranks a bright one across the level. The
            // 1e-3 floor keeps an eye-inside light from becoming an unstable infinity.
            const float dx = eye.x - L.position.x;
            const float dy = eye.y - L.position.y;
            const float dz = eye.z - L.position.z;
            const float d  = std::sqrt(dx * dx + dy * dy + dz * dz);
            const float gap = (std::max)(d - L.radius, 1e-3f);
            const float luma = 0.2126f * L.color.x + 0.7152f * L.color.y
                             + 0.0722f * L.color.z;
            const float score = (L.intensity * (std::max)(luma, 1e-6f))
                              / (gap * gap);
            // Position and radius are finite, but colour and intensity are untested: an
            // infinite intensity makes the score inf (or NaN with a zero numerator). Refuse
            // it for the same reason - the comparator must not see one.
            if (!std::isfinite(score))
            {
                ++nonFinite;
                continue;
            }
            m_previewLightScore.emplace_back(score, i);
        }

        const uint32_t kept = static_cast<uint32_t>(m_previewLightScore.size());
        uint32_t dropped = 0;
        if (kept > Preview::kMaxPreviewLights)
        {
            dropped = kept - Preview::kMaxPreviewLights;
            // Partial sort (O(n)): only the top K must be right. Descending by score with
            // the source index as the tie-break, so equal scores resolve deterministically
            // instead of flickering between runs.
            std::nth_element(
                m_previewLightScore.begin(),
                m_previewLightScore.begin() + Preview::kMaxPreviewLights,
                m_previewLightScore.end(),
                [](const std::pair<float, uint32_t>& a,
                   const std::pair<float, uint32_t>& b)
                { return a.first != b.first ? a.first > b.first
                                            : a.second < b.second; });
            m_previewLightScore.resize(Preview::kMaxPreviewLights);
        }
        // Step 4: sort the survivors back into m_frameLights order (see the function
        // comment).
        std::sort(m_previewLightScore.begin(), m_previewLightScore.end(),
                  [](const std::pair<float, uint32_t>& a,
                     const std::pair<float, uint32_t>& b)
                  { return a.second < b.second; });

        // Pack into a fixed-size stack array: the hand-over must not reallocate under
        // the render half, and SetViewSceneLights copies out before it returns.
        Preview::GPULight packed[Preview::kMaxPreviewLights];
        uint32_t n = 0;
        for (const auto& e : m_previewLightScore)
        {
            if (n >= Preview::kMaxPreviewLights) break;
            const ResolvedLight& L = m_frameLights[e.second];
            Preview::GPULight& g = packed[n++];
            g.position    = { L.position.x, L.position.y, L.position.z };
            g.radius      = L.radius;
            g.color       = { L.color.x, L.color.y, L.color.z };
            g.intensity   = L.intensity;
            g.direction   = { L.direction.x, L.direction.y, L.direction.z };
            g.spotAngle   = L.spotAngle;
            g.type         = static_cast<uint32_t>(L.type);
            // No punctual shadows in secondary views: L.shadowSliceIdx may be real
            // (BuildScene_UploadLights assigns slices to shadow-casting spots and points),
            // but the preview graph binds SetSpotShadowAtlas(0, 0) / SetPointShadowAtlas(0),
            // so a real index would sample a 1x1 fallback per light per pixel; 0xFFFFFFFF
            // skips the branch. A shadow-casting lamp is therefore unshadowed here while
            // the main viewport shadows it (the log's `shadowed=0`).
            // TODO(MultiViewport_MaterialPreview_Plan NF1-D): borrow the spot / point
            // shadow atlases as the CSM is borrowed, then pass L.shadowSliceIdx through.
            g.shadowSliceIdx = 0xFFFFFFFFu;
            g.powerBits = g.sourceRadiusBits = 0u;
        }

        // The `shadowed=` tally reads the packed records back rather than counting
        // beside the assignment, so it reports what crosses the seam; the packs that
        // assert `shadowed=0` go red once shadow slices are passed through.
        uint32_t shadowed = 0;
        for (uint32_t i = 0; i < n; ++i)
            if (packed[i].shadowSliceIdx != 0xFFFFFFFFu) ++shadowed;

        m_preview->SetViewSceneLights(v, packed, n, sceneCount, culled, dropped,
                                      shadowed, nonFinite);
    }
}

// The preview's mesh override. See the header for why .imsh is refused and
// why the library handle is cached.
bool Renderer::ResolvePreviewMesh(const std::string& meshlibPath, uint32_t meshId,
                                  uint32_t& outSlot, uint32_t& outIndexCount,
                                  float& outFitScale, float outFitOffset[3])
{
    // ResolvePreviewMeshEntry (PreviewAssetSource.cpp) owns the library load, the
    // slot registration and the cache rules: a failed load is not cached (it is
    // usually transient), the successful handle is, retries are rate-limited per
    // asset, and the cache key is one spelling for every caller. It returns the raw
    // AABB so the multi-part path can merge entries before fitting; this
    // single-mesh entry point does its own fit.
    float aabbMin[3] = { 0.f, 0.f, 0.f }, aabbMax[3] = { 0.f, 0.f, 0.f };
    if (!ResolvePreviewMeshEntry(meshlibPath, meshId, outSlot, outIndexCount,
                                 aabbMin, aabbMax))
        return false;

    // Fit the mesh into the primitives' unit box: the studio camera is authored
    // for a unit-diameter subject at the origin (kStudioCamera, PreviewStudioRig.h),
    // so a mesh in metres would fill one pixel or overflow the frame. The fit is
    // uniform and centred on the AABB, preserving proportions.
    // Both the scale and the offset are guarded: a corrupt (non-finite) library
    // AABB would otherwise give a NaN translation and an empty panel. A degenerate
    // box marks the mesh unfitted, so the panel shows its "drawn at its AUTHORED
    // size" note, with the same once-per-session line as the multi-part path
    // (ResolvePreviewAsset).
    const bool finiteBox =
        std::isfinite(aabbMin[0]) && std::isfinite(aabbMin[1]) && std::isfinite(aabbMin[2]) &&
        std::isfinite(aabbMax[0]) && std::isfinite(aabbMax[1]) && std::isfinite(aabbMax[2]);
    const float ex = finiteBox ? (aabbMax[0] - aabbMin[0]) : 0.f;
    const float ey = finiteBox ? (aabbMax[1] - aabbMin[1]) : 0.f;
    const float ez = finiteBox ? (aabbMax[2] - aabbMin[2]) : 0.f;
    const float maxExtent = std::max(ex, std::max(ey, ez));

    outFitScale     = (maxExtent > 1e-5f) ? (1.0f / maxExtent) : 1.0f;
    // A finite zero-extent box still centres (its centre is a point); a non-finite
    // one has no centre to use.
    outFitOffset[0] = finiteBox ? -0.5f * (aabbMin[0] + aabbMax[0]) : 0.f;
    outFitOffset[1] = finiteBox ? -0.5f * (aabbMin[1] + aabbMax[1]) : 0.f;
    outFitOffset[2] = finiteBox ? -0.5f * (aabbMin[2] + aabbMax[2]) : 0.f;

    if (!finiteBox || maxExtent <= 1e-5f)
    {
        // Insert the canonical key: IsPreviewAssetUnfitted is called with the panel's
        // spelling of the path, which may differ by a separator or by relativisation.
        m_previewUnfittedAssets.insert(PreviewAssetKey(meshlibPath));
        if (!m_previewMeshFitDegenerateLogged)
        {
            m_previewMeshFitDegenerateLogged = true;
            LOG_WARNING("[PREVIEW] mesh override '%s'#%u: its library AABB is %s - "
                        "the studio auto-fit could not derive a scale, so it is "
                        "drawn at its AUTHORED size (logged once per session)",
                        meshlibPath.c_str(), meshId,
                        finiteBox ? "zero-extent" : "not finite");
        }
    }
    return true;
}

// ==== Camera preview / in-game PiP ====
// Each function belongs to one half:
//   GAME   - ExtractPreviewCameras (needs World), SetGamePiP / ClearGamePiP.
//   RENDER - CullWorldForPreview, PreviewFrameState, PreviewMainShadowSnapshot,
//            GetGamePiPComposite. These read state the game half has already
//            published for this frame and is parked against.

void Renderer::SetPreviewCameraEntity(uint32_t viewId, Entity cameraEntity)
{
    if (viewId >= Preview::kMaxPreviews) return;
    m_previewCamEntity[viewId] = cameraEntity;
}

// The one frustum construction for both halves of the secondary-view cull:
// ExtractPreviewCameras (game half) uses it to decide which candidates to
// duplicate, CullWorldForPreview (render half) to query the BVH. Sharing it
// keeps the two consistent, orthographic cameras included.
// Reversed-Z (far, near): the result feeds ExtractFrustumPlanes, which reads the
// clip planes out of the matrix; a non-reversed projection swaps near and far
// and culls everything or nothing.
// @p inflate applies the main view's classify margin (UploadFrameData: Near
// x0.9, Far x1.1, slopes x1.05) to the projection - in tangent space for
// perspective, to the half-height for ortho - so an object crossing the edge
// is not admitted to the main view a frame before the secondary one (popping).
static DirectX::XMMATRIX MakeSecondaryViewProj(const RenderCamera& cam,
                                               uint32_t viewW, uint32_t viewH,
                                               bool inflate)
{
    using namespace DirectX;
    XMVECTOR fwd = XMVector3Normalize(XMLoadFloat3(&cam.forward));
    XMVECTOR up  = XMVector3Normalize(XMLoadFloat3(&cam.up));
    if (XMVector3IsNaN(up) || std::abs(XMVectorGetX(XMVector3Dot(up, fwd))) > 0.999f)
        up = XMVectorSet(0.f, 1.f, 0.f, 0.f);
    const float aspect = (viewH != 0)
        ? static_cast<float>(viewW) / static_cast<float>(viewH) : 1.0f;

    const float k     = inflate ? 1.05f : 1.0f;
    // Clamp near: the projection divides by the near-far distance, and an
    // authored nearZ of 0 would produce infinities and six NaN planes.
    const float nearZ = (std::max)(cam.nearZ * (inflate ? 0.9f : 1.0f), 1e-4f);
    const float farZ  = (std::max)(cam.farZ  * (inflate ? 1.1f : 1.0f), nearZ + 1e-3f);

    const XMMATRIX view = XMMatrixLookToLH(XMLoadFloat3(&cam.position), fwd, up);
    XMMATRIX proj;
    if (cam.orthographic)
    {
        const float halfH = (std::max)(cam.orthoHalfHeight, 0.01f) * k;
        proj = XMMatrixOrthographicLH(halfH * aspect * 2.f, halfH * 2.f, farZ, nearZ);
    }
    else
    {
        float fov = cam.fov;
        if (inflate)
        {
            // 2*atan(tan(fov/2) * 1.05), clamped below XM_PI so a near-180 deg lens keeps
            // the matrix finite.
            const float halfTan = std::tan((std::min)(fov, XM_PI * 0.98f) * 0.5f) * k;
            fov = (std::min)(2.f * std::atan(halfTan), XM_PI * 0.99f);
        }
        proj = XMMatrixPerspectiveFovLH(fov, aspect, farZ, nearZ);
    }
    return XMMatrixMultiply(view, proj);
}

namespace
{
    // @p e's camera: its GlobalTransform's basis and its lens. False when it has no transform or no
    // lens - not a camera.
    bool RenderCameraFromEntity(World& world, Entity e, RenderCamera& rc)
    {
        const GlobalTransform* gt = world.GetComponent<GlobalTransform>(e);
        if (!gt) return false;

        const DirectX::XMMATRIX world4 = DirectX::XMLoadFloat4x4(&gt->matrix);
        // Same basis App resolves the main camera from: position = translation row,
        // forward = third basis vector, up = second. Written out because App's copy
        // sits inside the VCam / LiveCameraComponent fallback chain, and this must work
        // for an entity that is not the main camera.
        DirectX::XMStoreFloat3(&rc.position, world4.r[3]);
        DirectX::XMStoreFloat3(&rc.forward,
                               DirectX::XMVector3Normalize(world4.r[2]));
        DirectX::XMStoreFloat3(&rc.up,
                               DirectX::XMVector3Normalize(world4.r[1]));

        // Lens: VirtualCameraComponent first (the authored lens on a VCam rig;
        // CameraComponent may be a stale bridge copy). Its quaternion also carries
        // roll, which the basis above reproduces only when the transform itself is
        // rolled: a VCam driven by the blend stack writes its rotation into the
        // quaternion, not the transform.
        bool haveLens = false;
        if (const auto* vc = world.GetComponent<VirtualCameraComponent>(e))
        {
            rc.fov   = vc->fov;
            rc.nearZ = vc->nearZ;
            rc.farZ  = vc->farZ;
            haveLens = true;
        }
        if (const auto* cam = world.GetComponent<CameraComponent>(e))
        {
            if (!haveLens)
            {
                rc.fov   = cam->fov;
                rc.nearZ = cam->nearZ;
                rc.farZ  = cam->farZ;
                haveLens = true;
            }
        }
        // No lens, no picture: an entity without a camera component is not a camera,
        // and the RenderCamera defaults would show a plausible view from an arbitrary
        // lens.
        return haveLens;
    }
}

void Renderer::ExtractPreviewCameras(World& world)
{
    // Rebuilt from scratch every frame; cleared before the null test so releasing
    // the last view leaves the lane inactive rather than frozen on its last frustum.
    m_secondaryFrusta.clear();
    m_secondaryCullActive = false;

    // The secondary views that follow a camera (SetViewCamera): this frame's, first.
    for (uint32_t slot = 1; slot < RHI::kMaxViews; ++slot)
    {
        const SecondaryView& v = m_secondary[slot];
        if (!v.live || !v.ready || !v.followCamera) continue;
        RenderCamera rc;
        if (v.followMain)
            rc = FrameViewWrite().camera;
        else if (!world.IsAlive(v.cameraEntity) || !RenderCameraFromEntity(world, v.cameraEntity, rc))
        {
            ClearSecondaryView(slot);   // its camera is gone: nothing to render through
            PostProcess::Runtime::Get().ClearViewResolved(slot);
            continue;
        }
        SetSecondaryView(slot, rc, v.reqWidth, v.reqHeight, v.reqShow);
        // Its own post-process blend, at its camera (the volumes it is in, not the main camera's).
        PostProcess::ResolvedPostProcessSettings post;
        PostProcess::ResolveAt(world, rc.position, 0xFFFFFFFFu, post);
        PostProcess::Runtime::Get().SetViewResolved(slot, post);
    }
    // The frame's secondary views (FrameView::secondary): the classify keeps their candidates the
    // same way. Their own cull (CullView, in their segment) uses the exact planes.
    for (const SecondaryFrameView& s : FrameViewWrite().secondary)
    {
        if (s.slot == 0 || s.vpWidth == 0 || s.vpHeight == 0) continue;
        DirectX::XMFLOAT4X4 vp;
        DirectX::XMStoreFloat4x4(&vp, MakeSecondaryViewProj(s.camera, s.vpWidth, s.vpHeight, /*inflate=*/true));
        m_secondaryFrusta.push_back(ExtractFrustumPlanes(vp));
    }
    m_secondaryCullActive = !m_secondaryFrusta.empty();

    // Null test first: a game with no PiP pays one compare per frame here.
    // m_preview exists only once something acquires a view.
    if (!m_preview) return;

    // Invalidate every view's exact light-cull frustum before the loop, so a view
    // that stopped being live (or lost its camera) leaves no stale planes for
    // ExtractPreviewSceneLights.
    for (bool& pv : m_previewLightPlanesValid) pv = false;

    for (uint32_t v = 0; v < Preview::kMaxPreviews; ++v)
    {
        if (!m_preview->IsWorldCullView(v)) continue;
        const Entity e = m_previewCamEntity[v];
        if (e == NullEntity || !world.IsAlive(e))
        {
            m_preview->SetViewCamera(v, RenderCamera{}, /*valid=*/false);
            continue;
        }

        RenderCamera rc;
        if (!RenderCameraFromEntity(world, e, rc)) { m_preview->SetViewCamera(v, RenderCamera{}, false); continue; }

        // This view's inflated frustum for the classify lane. Built here, on the game
        // half, because BuildScene_ClassifyCandidates runs later in this
        // BuildRenderScene and deletes the candidates a secondary view needs.
        // Everything that can refuse a frustum runs before SetViewCamera: a camera
        // published as valid with no entry in m_secondaryFrusta would get no
        // duplicates and silently show only what the main frustum contains. A view
        // that cannot contribute a frustum is held invalid (PreviewRenderer degrades
        // on that) and the reason is logged once.
        uint32_t vw = 0, vh = 0;
        if (!m_preview->GetViewRequestSize(v, vw, vh) || vw == 0 || vh == 0)
        {
            // Not an error: a view is legitimately unsized between AcquireView and its
            // first SetRequest. Logged once per view, so a persistent case shows without
            // flooding the log.
            if (!m_previewNoFrustumLogged[v])
            {
                m_previewNoFrustumLogged[v] = true;
                LOG_INFO("[PREVIEW] view %u has no request size yet - its camera "
                         "is held INVALID until it does, so the classify lane "
                         "keeps no secondary duplicates for it this frame", v);
            }
            m_preview->SetViewCamera(v, RenderCamera{}, /*valid=*/false);
            continue;
        }

        // A degenerate lens must not be silent: its planes would be NaN and
        // SecondaryFrustaSee would answer false for the whole world, which looks like
        // a camera pointed at nothing.
        const bool lensOk = rc.orthographic
            ? (rc.orthoHalfHeight > 0.f && std::isfinite(rc.orthoHalfHeight))
            : (rc.fov > 0.f && rc.fov < DirectX::XM_PI && std::isfinite(rc.fov));
        if (!ENGINE_CHECK_ONCE(lensOk && std::isfinite(rc.nearZ) &&
                               std::isfinite(rc.farZ) && rc.farZ > rc.nearZ,
                "Renderer::ExtractPreviewCameras: view %u camera entity %u has an "
                "unusable lens (ortho=%d fov=%.4f halfH=%.4f near=%.4f far=%.4f); "
                "no secondary frustum can be built, so the view is held INVALID.",
                v, static_cast<unsigned>(e), rc.orthographic ? 1 : 0,
                rc.fov, rc.orthoHalfHeight, rc.nearZ, rc.farZ))
        {
            m_preview->SetViewCamera(v, RenderCamera{}, /*valid=*/false);
            continue;
        }

        // historyValid stays at its default: a secondary view has no temporal history,
        // and copying it would suggest something consumes it.
        m_preview->SetViewCamera(v, rc, /*valid=*/true);
        m_previewNoFrustumLogged[v] = false;   // it contributed one; re-arm

        // Orthographic works too: the frusta are stored as six planes, not as a
        // slope-based BoundingFrustum (see the member in Renderer.h).
        DirectX::XMFLOAT4X4 vp;
        DirectX::XMStoreFloat4x4(&vp,
            MakeSecondaryViewProj(rc, vw, vh, /*inflate=*/true));
        m_secondaryFrusta.push_back(ExtractFrustumPlanes(vp));

        // The exact frustum, for the light cull. The classify lane uses the inflated
        // form because keeping one candidate too many is free; a light outside the
        // exact frustum lights no rasterised pixel and would waste one of
        // Preview::kMaxPreviewLights slots.
        // Built here so it passes the same refusals (no size, no lens, degenerate
        // lens) as the classify frustum. Indexed by v, not appended:
        // m_secondaryFrusta is compacted, so its index is not a view id.
        DirectX::XMFLOAT4X4 exactVp;
        DirectX::XMStoreFloat4x4(&exactVp,
            MakeSecondaryViewProj(rc, vw, vh, /*inflate=*/false));
        m_previewLightPlanes[v]      = ExtractFrustumPlanes(exactVp);
        m_previewLightEye[v]         = rc.position;
        m_previewLightPlanesValid[v] = true;
    }
    m_secondaryCullActive = !m_secondaryFrusta.empty();

    // Log how many frusta the classify lane keeps duplicates for, once per change.
    // A suspended view (excluded by IsWorldCullView) shows in no per-view counter
    // or capture, so this line is the witness: it drops by one on the frame a view
    // is suspended and comes back when the lane moves to a size that view fits.
    const uint32_t frusta = static_cast<uint32_t>(m_secondaryFrusta.size());
    if (frusta != m_previewSecondaryFrustaLogged)
    {
        m_previewSecondaryFrustaLogged = frusta;
        LOG_INFO("[PREVIEW] secondary frusta=%u", frusta);
    }

    // Arm the auto-exposure CPU readback while a PiP is live: a secondary view's
    // tone map and its forward Unlit / emissive materials scale by the scene
    // exposure, whose only CPU-visible copy is AutoExposurePass's TTL-gated 4-byte
    // readback (normally armed by the editor's Auto Exposure panel). The TTL
    // decays by itself, so a game with no PiP pays nothing; a live PiP pays one
    // barrier pair and a 4-byte copy per frame.
    if (m_secondaryCullActive && m_autoExposurePass)
        m_autoExposurePass->SetDebugReadbackEnabled(true);
}

uint32_t Renderer::SecondaryFrustaSee(const DirectX::XMFLOAT3& mn,
                                      const DirectX::XMFLOAT3& mx) const
{
    if (!m_secondaryCullActive) return 0u;
    uint32_t seen = 0u;
    // Six inward-normal planes, one dot product each (the FrustumPlanes overload
    // of FrustumTestAABB).
    for (const FrustumPlanes& fp : m_secondaryFrusta)
        if (FrustumTestAABB(fp, mn, mx)) { seen |= kSeenByPiP; break; }
    // The baking probe's faces as the cube they cover together: an overlap test.
    // The near plane's 0.1 hole at the centre is ignored; it only keeps a copy too
    // many.
    if (m_probeCullActive &&
        mn.x <= m_probeCullMax.x && mx.x >= m_probeCullMin.x &&
        mn.y <= m_probeCullMax.y && mx.y >= m_probeCullMin.y &&
        mn.z <= m_probeCullMax.z && mx.z >= m_probeCullMin.z)
        seen |= kSeenByProbe;
    return seen;
}

bool Renderer::RemapToSecondaryFilter(DrawCandidate& c)
{
    switch (c.filter)
    {
    case DrawFilter::Opaque:
        c.filter = DrawFilter::SecondaryView;
        return true;
    case DrawFilter::Transparent:
        c.filter = DrawFilter::SecondaryViewBlend;
        return true;
    // An outline shell: only the main pipeline's secondary views ask for it (the preview graph does not).
    case DrawFilter::Custom:
        c.filter = DrawFilter::SecondaryViewCustom;
        return true;
    // Water is excluded on purpose: a water draw needs WaterSurfacePass (its own
    // PSO, refraction / depth snapshot inputs and the SSR result index), which the
    // preview graph lacks, and the preview TransparentPass would shade it with a
    // forward path it was never compiled for. (The main pipeline's secondary views
    // need no copy: the classify never drops a water draw off the main screen.)
    default:
        return false;
    }
}

bool Renderer::WantsProbeCopy(const DrawCandidate& c)
{
    return c.filter == DrawFilter::Opaque && (c.viewMask & ViewBit::ReflectionProbe) != 0;
}

void Renderer::NoteSecondaryViewCandidate(const DrawCandidate& c,
                                          const DirectX::XMFLOAT3& mn,
                                          const DirectX::XMFLOAT3& mx)
{
    // One bool test when nothing is live: the whole cost of this lane in a game
    // that never opens a PiP.
    if (!m_secondaryCullActive) return;
    // Cheap rejections before the ~200-byte DrawCandidate copy: one compare for
    // the filter, six dot products for the frustum.
    if (c.filter != DrawFilter::Opaque && c.filter != DrawFilter::Transparent && c.filter != DrawFilter::Custom)
        return;
    const uint32_t seen = SecondaryFrustaSee(mn, mx);
    if (seen & kSeenByPiP)
    {
        m_secondaryCands.push_back(c);
        RemapToSecondaryFilter(m_secondaryCands.back());
    }
    // A probe baking this frame keeps its own copy of what the main view is about
    // to drop or convert, so its content does not depend on the main camera. A
    // separate copy even when a PiP sees it too: each view reads only its own
    // filter.
    if ((seen & kSeenByProbe) && WantsProbeCopy(c))
    {
        m_secondaryCands.push_back(c);
        m_secondaryCands.back().filter = DrawFilter::ProbeView;
    }
}

// The PiP latch is pending-then-applied. SetGamePiP / ClearGamePiP can be
// called from unsafe places - e.g. the harness block at the tail of App::Run's
// frame loop, while the previous frame's render half may still be reading
// Target::req / result / needsRender in ProcessQueue - so they only write the
// game-owned m_pipPending. ApplyPendingGamePiP (from BuildRenderScene, inside
// the extract window, after the park) is the single point that turns it into
// AcquireView / ReleaseView / SetRequest.
// Returns false when the request cannot be honoured (no entity, zero size,
// preview layer off); an acquire failure is logged by the apply.
bool Renderer::SetGamePiP(Entity cameraEntity, uint32_t w, uint32_t h,
                          uint32_t tickInterval)
{
    if (cameraEntity == NullEntity || w == 0 || h == 0) return false;
    if (!IsPreviewEnabled()) return false;

    m_pipPending.action = PipPending::Action::Set;
    m_pipPending.entity = cameraEntity;
    m_pipPending.w      = w;
    m_pipPending.h      = h;
    m_pipPending.tick   = (tickInterval == 0) ? 1u : tickInterval;
    return true;
}

void Renderer::ClearGamePiP()
{
    // Last write wins, including over a Set queued earlier in the same frame.
    m_pipPending.action = PipPending::Action::Clear;
    m_pipPending.entity = NullEntity;
}

void Renderer::ApplyPendingGamePiP()
{
    // The scene-light off-arm rides the same parked window. It runs first,
    // outside the early-out below: it is not part of the PiP latch, just another
    // game-half write that must not land while the render half reads. The dirty
    // flag keeps a session that never touches the arm from writing it.
    if (m_previewWorldSceneLightsDirty && m_preview)
    {
        m_preview->SetWorldSceneLightsEnabled(m_previewWorldSceneLights);
        m_previewWorldSceneLightsDirty = false;
    }

    const PipPending req = m_pipPending;
    m_pipPending.action = PipPending::Action::None;
    if (req.action == PipPending::Action::None) return;

    if (req.action == PipPending::Action::Clear)
    {
        if (m_pipView == Preview::kInvalidView) { m_pipEntity = NullEntity; return; }
        SetPreviewCameraEntity(m_pipView, NullEntity);
        ReleasePreviewView(m_pipView);
        LOG_INFO("[PREVIEW] game PiP OFF (view %u released)", m_pipView);
        m_pipView   = Preview::kInvalidView;
        m_pipEntity = NullEntity;
        m_pipW = m_pipH = 0;
        return;
    }

    // Re-point, don't re-acquire: switching cameras (e.g. from Lua) is normal, and
    // a second acquire would leak the first view's HDR slot for the session.
    // A re-point may also change the size. The PiP is normally the only live
    // camera view, so it owns the whole lane and RequestPreviewWorldCullLaneSize
    // below moves it (count == 1, the rule the editor panels use). With an editor
    // camera panel also live (not reachable from Game.exe) the move is refused:
    // one WARNING plus IsPreviewViewSizeRefused, not a silent freeze.
    if (m_pipView == Preview::kInvalidView)
    {
        m_pipView = AcquirePreviewView(/*worldCull=*/true);
        if (m_pipView == Preview::kInvalidView)
        {
            LOG_WARNING("[PREVIEW] game PiP requested for entity %u but no view "
                        "was available (another WorldCull view owns the lane, or "
                        "the pool is full); the PiP is NOT enabled.",
                        static_cast<unsigned>(req.entity));
            return;
        }
    }
    m_pipEntity = req.entity;
    m_pipW      = req.w;
    m_pipH      = req.h;
    m_pipTick   = req.tick;
    SetPreviewCameraEntity(m_pipView, req.entity);

    // Latch the ViewDesc once, not per frame: a WorldCull view's picture does not
    // depend on it (the world moves under a fixed description).
    // ExtractPreviewCameras and the tick regulator drive the redraw.
    Preview::ViewDesc vd{};
    vd.w      = req.w;
    vd.h      = req.h;
    vd.source = Preview::Source::WorldCull;
    vd.tickIntervalFrames = m_pipTick;
    // The scene's sky behind the geometry, so the PiP looks like the game rather
    // than a cut-out on black.
    vd.features   = Preview::Feature::kSkybox;
    vd.background = Preview::Background::Skybox;
    // Move the lane first (the mechanism the editor panels use): the request below
    // is judged against the lane's current size. count == 1 because the PiP is the
    // whole lane whenever it can be moved at all.
    RequestPreviewWorldCullLaneSize(req.w, req.h, &m_pipView, 1u);
    RequestPreview(m_pipView, vd, MaterialComponent{});

    // Report what the latch took, not what was asked: a refused size keeps the
    // view rendering at the old one.
    const bool sizeRefused = IsPreviewViewSizeRefused(m_pipView);
    if (sizeRefused)
    {
        uint32_t lw = 0, lh = 0;
        GetPreviewWorldCullSize(lw, lh);
        LOG_WARNING("[PREVIEW] game PiP ON: entity %u, tick %u, view %u - but the "
                    "requested %ux%u was REFUSED and it is still drawing at the "
                    "lane's %ux%u (another camera view is live).",
                    static_cast<unsigned>(req.entity), m_pipTick, m_pipView,
                    req.w, req.h, lw, lh);
        // Keep m_pipW/H honest so the composite does not stretch the old texture into
        // the new rect: GetGamePiPComposite falls back to them before the target
        // exists.
        m_pipW = lw ? lw : m_pipW;
        m_pipH = lh ? lh : m_pipH;
    }
    else
        LOG_INFO("[PREVIEW] game PiP ON: entity %u, %ux%u, tick %u, view %u",
                 static_cast<unsigned>(req.entity), req.w, req.h, m_pipTick,
                 m_pipView);
}

void Renderer::SetGamePiPRect(int32_t x, int32_t y, int32_t w, int32_t h)
{
    // The composite (render half) reads m_pipRect, so the write goes through the
    // structural mailbox: it lands at this frame's sync point, before the render
    // half reads it.
    RunOrDeferToSyncPoint(
        [this, x, y, w, h]
        {
            // A non-positive extent means AUTO: bottom-right with a margin, resolved at
            // composite time against the back buffer. That is how to drop an explicit
            // rect.
            m_pipRect[0] = x;
            m_pipRect[1] = y;
            m_pipRect[2] = (w > 0) ? w : 0;
            m_pipRect[3] = (h > 0) ? h : 0;
        },
        [this] { m_gfx.DrainCpuFrame(static_cast<uint8_t>(Diag::DrainCause::PiPRect)); });
}

bool Renderer::GetGamePiPComposite(uint64_t& outSrv, int32_t& outX, int32_t& outY,
                                   int32_t& outW, int32_t& outH) const
{
    if (!m_preview || m_pipView == Preview::kInvalidView) return false;
    // Re-resolve every frame, never cache: the descriptor behind this handle goes
    // back through the allocator's free list on a resize.
    const uint64_t srv = m_preview->GetSrv(m_pipView);
    if (!srv) return false;   // nothing recorded yet

    outSrv = srv;
    // Use the target's actual size, not the requested one: until ServiceCompile
    // rebuilds the graph the texture behind `srv` is still the old size, and
    // compositing it into the new rect would stretch it. m_pipW/H (the request) is
    // the fallback before the target exists, when nothing is composited anyway.
    uint32_t aw = m_pipW, ah = m_pipH;
    m_preview->GetOutputSize(m_pipView, aw, ah);
    outW = static_cast<int32_t>(aw);
    outH = static_cast<int32_t>(ah);
    if (m_pipRect[2] > 0 && m_pipRect[3] > 0)
    {
        outX = m_pipRect[0]; outY = m_pipRect[1];
        outW = m_pipRect[2]; outH = m_pipRect[3];
        return true;
    }
    // AUTO: bottom-right with a 16 px margin, in back-buffer pixels (the
    // composite viewport's space).
    const int32_t bw = static_cast<int32_t>(m_gfx.GetWidth());
    const int32_t bh = static_cast<int32_t>(m_gfx.GetHeight());
    const int32_t margin = 16;
    outX = bw - outW - margin;
    outY = bh - outH - margin;
    if (outX < 0) outX = 0;
    if (outY < 0) outY = 0;
    return true;
}

// @p viewId defaults to Preview::kInvalidView, meaning "whichever view recorded
// last" - what a single-view caller (preview_pip_cull's driver) relies on.
uint32_t Renderer::GetPreviewWorldCullDraws(uint32_t viewId) const
{
    return m_preview ? m_preview->LastWorldCullDraws(viewId) : 0u;
}

uint32_t Renderer::GetPreviewWorldCullCandidates(uint32_t viewId) const
{
    return m_preview ? m_preview->LastWorldCullCandidates(viewId) : 0u;
}

uint32_t Renderer::GetPreviewWorldCullSecondary(uint32_t viewId) const
{
    return m_preview ? m_preview->LastWorldCullSecondary(viewId) : 0u;
}

// What the record half last bound, not what the cull produced; 0 with no
// preview layer.
uint32_t Renderer::GetPreviewWorldClusterLights(uint32_t viewId) const
{
    return m_preview ? m_preview->LastWorldClusterLights(viewId) : 0u;
}

// Scene punctual lights in WorldCull views on / off (default on).
void Renderer::SetPreviewWorldSceneLights(bool on)
{
    // Latched here and pushed by ApplyPendingGamePiP inside the parked extract
    // window (and into a new layer by AcquirePreviewView / QueuePreviewThumbnail).
    // The latch survives a harness flag or Lua call that lands before the first
    // SetGamePiP builds the layer. Do not touch the layer here:
    // PreviewRenderer::m_worldSceneLightsEnabled is a plain bool read on the render
    // half by WriteWorldClusterLights, and one caller is the harness block at the
    // tail of App::Run's frame loop, where the previous frame's render half can
    // still be recording - the same hazard as the PiP latch. The dirty flag means
    // a session that never touches the arm never writes it.
    m_previewWorldSceneLights      = on;
    m_previewWorldSceneLightsDirty = true;
}

bool Renderer::PreviewWorldSceneLightsEnabled() const
{
    // Returns the game half's intent, not the render half's copy: reading
    // m_preview->WorldSceneLightsEnabled() would be the cross-half read the pending
    // apply exists to avoid, and would lag a fresh set until the next
    // BuildRenderScene. The intent is not evidence the render half acted on it;
    // Preview.WorldClusterLights() reports what was bound.
    return m_previewWorldSceneLights;
}

// The preview frame counter, not the engine's; 0 when the layer was never
// built.
uint64_t Renderer::GetPreviewFrame() const
{
    return m_preview ? m_preview->PreviewFrame() : 0ull;
}

void Renderer::GetPreviewAssetStats(uint32_t viewId, uint32_t& outParts,
                                    uint32_t& outMaterials,
                                    uint32_t& outDropped,
                                    Resource::PrefabSkipCounts* outSkipped) const
{
    outParts = outMaterials = outDropped = 0;
    if (outSkipped) *outSkipped = Resource::PrefabSkipCounts{};
    if (m_preview)
        m_preview->GetAssetStats(viewId, outParts, outMaterials, outDropped,
                                 outSkipped);
}

Renderer::PreviewFrameSnapshot Renderer::PreviewFrameState() const
{
    PreviewFrameSnapshot s{};
    const uint32_t frameSlot = m_gfx.GetFrameIndex();
    if (frameSlot >= kFrameSlots) return s;

    // The same two expressions Renderer::Render binds the main graph from, so a
    // secondary view agrees with the main one on which buffer this frame's packets
    // index. Retained mode draws from the single DEFAULT buffer with a per-frame
    // remap; otherwise from this frame's UPLOAD slot.
    s.instanceBuffer = RetainedInstancesActive() ? &m_instanceBufferGPU
                                                 : &m_instanceBuffer[frameSlot];
    s.materialBuffer = &MaterialSRVBuffer(frameSlot);
    // Every field below is a render-slot read: the snapshot is taken on the
    // render half for a list recorded this frame, so every ring is indexed the way
    // the render half indexes it (GetFrameIndex(), m_lightCB.Current()). Do not mix
    // in game-half accessors (GetUploadSlot(), m_lightCB.Upload()): they agree only
    // while the render half never lags, and a one-frame lag would hand the preview
    // transforms, remap and lights from different frames. ProcessProbeBakeQueue
    // follows the same rule.
    s.pristineRemapIdx = RetainedInstancesActive()
        ? m_instanceRemapHeapIdx[frameSlot] : 0u;
    s.mainLightCB     = const_cast<Renderer*>(this)->m_lightCB.Current(m_gfx);
    // FrameViewRead(), not FrameViewWrite(): the render half reads the published
    // view; the write side is the game's next frame.
    s.frameIndex      = static_cast<uint32_t>(FrameViewRead().currentFrame);
    s.dissolveNoiseIdx = m_dissolveNoiseTex.IsValid()
                       ? m_dissolveNoiseTex.handleId : ~0u;
    s.globalTimeSec   = FrameViewRead().globalTimeSec;
    // The scene's exposure, for the preview's own exposure buffer and ToneMapPass:
    // the PiP tracks the viewport instead of a neutral 1.0, under which forward
    // Unlit / emissive materials divide by an exposure nothing multiplies back. 0
    // means "no reading" and the preview falls back to neutral. Two sources, the
    // exact one preferred:
    //   * auto exposure off: resolvedPost.autoExposure.manualExposure, the CPU value
    //     the main ToneMap uses (Renderer hands it to
    //     AutoExposurePass::SetManualExposure in the post phase) - exact, zero
    //     frames of lag. Preview packs pin a profile with auto exposure off.
    //   * auto exposure on: AutoExposurePass's readback ring, kFramesInFlight-1
    //     frames old (the newest slot the GPU has provably retired) - the floor for
    //     any CPU-side answer.
    // Do not bind the scene's GPU exposure buffer directly: it is written on the
    // async compute queue this frame (computeCL and previewCL are siblings off
    // colorLastCL, unordered against each other); it is left in
    // SHADER_RESOURCE_COMPUTE while the preview reads it from a pixel shader; and
    // caching another graph's per-frame pass state across the seam is forbidden
    // (as for ClusterPass's SRVs, see WriteClusterLights). The preview keeps its own
    // per-frame-slot UPLOAD scalar (GENERIC_READ, legal for pixel and compute
    // reads), and this function fills it.
    s.sceneExposure = 0.0f;
    s.sceneExposureExact = false;
    {
        const auto& resolvedPost = PostProcess::Runtime::Published(m_sceneRead);
        if (!resolvedPost.autoExposure.enabled)
        {
            const float e = resolvedPost.autoExposure.manualExposure;
            if (std::isfinite(e) && e > 0.0f)
            {
                s.sceneExposure      = e;
                s.sceneExposureExact = true;
            }
        }
        else if (m_autoExposurePass)
        {
            const float e = m_autoExposurePass->GetLastExposure();
            if (std::isfinite(e) && e > 0.0f) s.sceneExposure = e;
        }
    }
    return s;
}

// Reads the main SkyboxPass's CPU-side sun / moon / star members, as
// PreviewMainShadowSnapshot reads ShadowPass's - not the SkyCB ring, which is
// another graph's per-frame GPU state.
SkyboxPass::SkyState Renderer::PreviewSkyState() const
{
    if (!m_skyboxPass) return {};
    return m_skyboxPass->GetSkyState(m_gfx);
}

Renderer::PreviewMainShadow Renderer::PreviewMainShadowSnapshot() const
{
    PreviewMainShadow s{};
    if (!m_shadowPass || !m_shadowSystem || !m_shadowSystem->HasValidLight())
        return s;
    const uint64_t atlas = m_shadowPass->GetShadowArrayGpuHandle();
    if (!atlas) return s;

    s.valid    = true;
    s.atlasSrv = atlas;
    const DirectX::XMFLOAT4X4* casc = m_shadowSystem->CascadeMatricesTransposed();
    const float*               spl  = m_shadowSystem->CascadeSplits();
    const float*               tex  = m_shadowSystem->CascadeTexelWorldSize();
    for (int c = 0; c < 4 && c < ShadowSystem::kCascadeCount; ++c)
    {
        std::memcpy(s.matrix[c], &casc[c], sizeof(DirectX::XMFLOAT4X4));
        s.splits[c]     = spl[c];
        s.texelWorld[c] = tex[c];
    }
    s.bias         = ShadowSystem::ShadowBias();
    s.strength     = ShadowSystem::ShadowStrength();
    s.texelSize    = 1.0f / static_cast<float>(m_shadowPass->GetShadowResolution());
    s.blendRange   = m_shadowSystem->ShadowBlendRange();
    s.normalOffset = ShadowSystem::ShadowNormalOffset();
    s.frameIndex   = m_shadowSystem->ShadowFrameIndex();
    return s;
}

void Renderer::CullWorldForPreview(const RenderCamera& cam, uint32_t viewW,
                                   uint32_t viewH, uint32_t viewBit,
                                   std::vector<DrawPacket>& out) const
{
    using namespace DirectX;
    out.clear();

    // 1. This camera's view, built by MakeSecondaryViewProj (also used by
    // ExtractPreviewCameras) with the reversed-Z near/far swap BuildCameraViewProj
    // uses for the main view. The exact form (inflate=false): this half chooses
    // what to draw, the classify lane what to keep, and a keep-set tighter than
    // the draw set would pop.
    // Blended geometry sorts by planar view depth, dot(centre - camPos, camForward),
    // the same metric as the main path (refreshDepth in Renderer_Scene.cpp), not
    // Euclidean distance.
    RG::SceneView view;
    view.name = "world";
    XMFLOAT4X4 vp;
    XMStoreFloat4x4(&vp, MakeSecondaryViewProj(cam, viewW, viewH,
                                               /*inflate=*/false));
    CullPlanesFrom(ExtractFrustumPlanes(vp), view.cull.volume);
    XMFLOAT3 camFwd;
    XMStoreFloat3(&camFwd, XMVector3Normalize(XMLoadFloat3(&cam.forward)));
    view.cull.eye[0] = cam.position.x; view.cull.eye[1] = cam.position.y; view.cull.eye[2] = cam.position.z;
    view.cull.forward[0] = camFwd.x;   view.cull.forward[1] = camFwd.y;   view.cull.forward[2] = camFwd.z;
    view.cull.viewBit = viewBit;

    // 2. What it draws, via Renderer::CullView: four lists, and the shadow list is
    // deliberately not one of them. Opaque / Transparent are what the main camera
    // sees; SecondaryView / SecondaryViewBlend are the duplicates the game half
    // made for candidates the main frustum rejected. DrawFilter::Shadow packets are
    // lossy (the conversion resets permutation, stencil ref, cull mode and depth,
    // so a skinned NPR character would draw as unskinned PBR), and every caster a
    // secondary view sees already has an intact duplicate - reading both would
    // draw it twice.
    // Order is the contract: RenderContext::GetDrawList finds a filter's span as a
    // contiguous run, so Opaque (both sources) precedes blended (both sources).
    // In this preview-local copy survivors get their natural filters back (the
    // preview GBufferPass asks for Opaque, its TransparentPass for Transparent; the
    // engine's packet array keeps the secondary filters that keep them out of the
    // main view) and viewMask ~0u: GBufferPass always tests
    // `dp.viewMask & ViewBit::MainCamera`, so an entity opted out of the main camera
    // but into the secondary one would otherwise be dropped. The transparent tail is
    // re-sorted against this camera.
    ViewCullRequest req;
    req.lists[0] = DrawFilter::Opaque;
    req.lists[1] = DrawFilter::SecondaryView;      req.secondary[1] = true;
    req.lists[2] = DrawFilter::Transparent;
    req.lists[3] = DrawFilter::SecondaryViewBlend; req.secondary[3] = true;
    req.listCount       = 4;
    req.naturalFilters  = true;
    req.rewriteViewMask = true;
    req.sortTransparent = true;
    ViewCullStats st;
    CullView(view, req, out, &st);

    // Logged once: "0 packets kept" has two causes - the BVH returned nothing (a
    // wrong frustum) or entities the packets do not resolve to (a remap read from
    // the wrong buffer) - and only the BVH's own count tells them apart.
    if (!m_previewCullLogged)
    {
        m_previewCullLogged = true;
        LOG_INFO("[PREVIEW] world cull BVH: leaves=%u visible=%u (opaque=%zu "
                 "transparent=%zu secondary=%zu/%zu packets)",
                 st.bvhLeaves, st.bvhVisible,
                 GetDrawList(DrawFilter::Opaque).size(),
                 GetDrawList(DrawFilter::Transparent).size(),
                 GetDrawList(DrawFilter::SecondaryView).size(),
                 GetDrawList(DrawFilter::SecondaryViewBlend).size());
    }

    // What this scene had to exclude, logged on change: the panels' tooltips say
    // "no water surface", and a nonzero count shows the exclusion applied to
    // something. Documentation, not a gate: the exclusion is true by construction.
    {
        const uint32_t waterPackets =
            static_cast<uint32_t>(GetDrawList(DrawFilter::Water).size());
        if (waterPackets != m_previewWaterPacketsLogged)
        {
            m_previewWaterPacketsLogged = waterPackets;
            LOG_INFO("[PREVIEW] secondary views exclude water: waterPackets=%u "
                     "(RemapToSecondaryFilter answers false for "
                     "DrawFilter::Water - no WaterSurfacePass in the preview "
                     "graph)", waterPackets);
        }
    }

    if (m_preview)
        m_preview->NoteWorldCullCounts(st.candidates, st.keptSecondary, st.leaflessPrim,
                                       st.leaflessOther, st.leaflessPrimKept,
                                       st.leaflessOtherKept);
}

void Renderer::ApplyPreviewFeedback()
{
    // Null test first: with no preview layer the mailbox loads and FrameStats
    // stores would be waste on every frame. Both counters keep their FrameStats
    // default of 0 when m_preview is null (nothing else writes them), so a check on
    // previewViews reads a real 0.
    if (!m_preview) return;
    m_frameStats.previewViewsRendered =
        m_previewViewsRenderedMbx.load(std::memory_order_relaxed);
    m_frameStats.previewViewDraws =
        m_previewViewDrawsMbx.load(std::memory_order_relaxed);
    m_preview->ApplyFeedback();
}

void Renderer::ExtractPreviewRequests()
{
    if (!m_preview) return;
    // Top of the game half's preview window. The asset frame counter advances
    // exactly once per game frame, so kPreviewAssetRetryFrames means the same
    // however the loop is scheduled. Do not drain the mesh-library hand-backs here:
    // this runs last in BuildRenderScene, after the scene's own slot registrations;
    // DrainPreviewMeshLibHandbacks runs at the top of BuildRenderScene instead.
    ++m_previewAssetFrame;
    // Refresh the environment handles before the request is handed over: the
    // record half binds them this frame.
    SyncPreviewEnvironment();
    m_preview->ExtractRequests();
    // WorldCull views borrow the main CSM (PreviewMainShadowSnapshot), so a
    // path-traced frame's plan keeps the CSM for them. Counted here, while the
    // render half is parked, and read at its top (ReadPlanState).
    m_planPreviewWorldViews = m_preview->LiveWorldCullViewCount();
    // The scene's punctual lights, after ExtractRequests: it arms req.inFlight, so
    // IsWorldCullView answers for the views the tick regulator let through this
    // frame (typically one of four on a split panel). Park-first makes this
    // function exclusive: nothing it writes is read before the render half resumes.
    ExtractPreviewSceneLights();
}

void Renderer::ProcessPreviewQueue(RHI::CommandList colorLastCL)
{
    m_previewCLThisFrame = RHI::CommandList{};
    if (!m_preview) return;
    // The returned list is parked, not chained: FramePipeline::RenderHalfAsync
    // gives it its own edge to primaryCL. See TakePreviewCommandListThisFrame for
    // why it must not become the caller's colorLastCL.
    m_previewCLThisFrame = m_preview->ProcessQueue(colorLastCL);
    m_previewViewsRenderedMbx.store(m_preview->ViewsRenderedThisFrame(),
                                    std::memory_order_relaxed);
    m_previewViewDrawsMbx.store(m_preview->ViewDrawsThisFrame(),
                                std::memory_order_relaxed);
}

RHI::CommandList Renderer::TakePreviewCommandListThisFrame()
{
    const RHI::CommandList cl = m_previewCLThisFrame;
    m_previewCLThisFrame = RHI::CommandList{};   // consumed exactly once
    return cl;
}
