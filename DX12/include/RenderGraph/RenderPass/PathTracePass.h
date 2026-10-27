#pragma once
#include "RenderGraph/RenderGraph.h"
#include "RenderGraph/ViewState.h"
#include "Graphics/FrameCB.h"
#include "Graphics/NrdDenoiser.h"
#include "RenderGraph/RenderPass/ReSTIRBackend.h"
#include "RenderGraph/RenderPass/PTRtxdiCore.h"
#include <DirectXMath.h>
#include <atomic>
#include <memory>
#include <utility>
#include <vector>

// The path tracer, from the GBuffer primary (or camera rays). Owns its accumulation; the
// graph owns the result.
class PathTracePass final : public RG::RenderPass
{
public:
    PathTracePass(RG::RenderGraph& graph, RG::RGTextureHandle albedo,
                  RG::RGTextureHandle normal, RG::RGTextureHandle surface,
                  RG::RGTextureHandle depth, RG::RGTextureHandle result);
    const char* GetName() const override { return "PT.Trace"; }
    void Setup(RG::RenderGraphBuilder& b) override;
    void Init(IGraphicsDevice& gfx) override;
    void OnCompile(IGraphicsDevice& gfx) override;
    void Shutdown(IGraphicsDevice& gfx) override;
    void ReloadShaders(IGraphicsDevice& gfx) override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    bool IsActive() const override { return m_ws->active; }
    bool UsesCameraPrimary() const { return (m_flags & 512u) != 0; }
    // The fog as a participating medium (pass.pathTrace.media): integrated when the frame has
    // one (FrameData::mediumIdx), by this pass's own path tracer (not the rtxdi core). Safe on any
    // thread.
    bool MediaEnabled() const;
    // The rtxdi core's segments cross the fog (pass.ptRtxdi.media) - whoever draws the camera's
    // fog - when the frame has one (FrameData::rtxdiMediumIdx). Safe on any thread.
    bool RtxdiMediaEnabled() const;
    // What a continuation segment of the rtxdi core keeps through the fog
    // (pass.ptRtxdi.mediaContinuation, as RtxdiBridgePTConstants.hlsli's kRabMedia*).
    static uint32_t RtxdiMediaRule();
    // The camera segment's fog (FogCamera: rgb the light the fog scatters toward the camera, a its
    // transmittance, running means at the render size) as an SRV heap index, for PathTraceFogPass:
    // 0 unless this frame traces it on the GBuffer primary and it has been written; on a frame the
    // preview denoiser filters it, the filtered target. SRV-readable after this pass. Render half,
    // after SetFrameData: the bound view's.
    uint32_t GetCameraFogIndex() const;
    // The rtxdi core's SHaRC radiance cache for a pass after this one (the froxel fog's indirect
    // rays): resolved by this pass on a frame it traces in the rtxdi mode. Empty otherwise.
    SharcCache::Lookup GetSharcLookup() const;
    void EnsureResources(bool wanted);
    // A secondary view's working set and history textures at its render size while @p wanted (the main
    // view path-traces with this pass's own path tracer), released otherwise - its segment, before the
    // graph records. Its reference denoiser is retired, not drained.
    void EnsureViewResources(uint32_t w, uint32_t h, bool wanted);
    // The *Pinned arguments: the value is a command-line pin (Renderer::DevPin) - an explicit
    // bounce count or reference sends the real-time path's auto to this path tracer and takes
    // precedence over the settings' reference; a pinned area motion holds without an Area mode.
    void SetSettings(uint32_t maxBounces, bool reference, bool bouncesPinned=false, bool referencePinned=false);
    void SetAreaSettings(uint32_t mode, bool motion, uint32_t historyCap,
                         uint32_t spatialK=2, float spatialRadius=6.f, bool reconnect=false, bool motionPinned=false);
    bool UsesAreaV2() const { return m_areaV2; }
    void SetCameraSettings(bool cameraPrimary,bool filmJitter,bool primaryRayCone,float lensRadius,float focusDistance);
    void SetRayReconstruction(bool enabled);
    // The ReSTIR PT core, a mode of this pass: auto | area | rtxdi. Reference / progressive
    // accumulation is the ground truth and never a backend. Call after the frame's Set*Settings
    // (auto reads them); applied at the next EnsureResources.
    void SetBackend(ReSTIR::Backend requested);
    const ReSTIR::BackendStatus& GetBackendStatus() const { return m_status; }
    // The core the request resolves to before availability (an explicit backend as asked,
    // except rtxdi with a thin lens or -pt_gi_shell_model; auto by use - see ResolveRequest), as
    // of the last SetBackend.
    ReSTIR::Backend GetResolvedBackend() const { return m_resolved; }
    // The rtxdi mode (PTRtxdiCore) is in effect: it reads the motion vectors and composites the
    // primary's local lights from RestirDIResult, so the renderer keeps the DI pass running while
    // this mode traces.
    bool UsesRtxdi() const { return m_status.effective == ReSTIR::Backend::Rtxdi; }
    void SetVelocity(RG::RGTextureHandle velocity) { m_velocity = velocity; }
    void SetRestirDIResult(RG::RGTextureHandle result) { m_diResult = result; }
    // The glass's own reflection (PathTrace.cs.hlsl FrontReflection): the translucent surfaces
    // TransparentPass composites over this pass's image trace their specular lobe from
    // TranslucentFrontLayerPass's stamp instead of folding the forward pass's screen tiers -
    // accumulated with the image, or on a frame that does not accumulate (DLSS-RR's one sample a
    // frame; RR has no guides for the translucent layer) denoised on the pane's own surface by
    // REBLUR_SPECULAR (PathTraceFrontNRD.cs.hlsl). SetFrontLayer: the stamp's targets
    // (registration). SetFrontReflection: wanted on this path, before EnsureResources (which
    // builds the entries, the accumulation and the denoiser). SetFrontReflectionActive: the stamp
    // is drawn this frame.
    void SetFrontLayer(RG::RGTextureHandle depth, RG::RGTextureHandle normal, RG::RGTextureHandle surface,
                       RG::RGTextureHandle velocity)
    { m_frontDepth = depth; m_frontNormal = normal; m_frontSurface = surface; m_frontVelocity = velocity; }
    void SetFrontReflection(bool wanted) { m_frontWanted = wanted && !m_frontOff; }
    bool FrontReflectionReady() const
    {
        // The denoiser is part of the tier: every frame that does not accumulate denoises, RR or
        // not.
        return m_frontWanted && m_frontPso.IsValid() && m_frontCb.IsValid() && m_front.IsValid() &&
               m_frontNrd && m_frontNrd->IsReady() && m_frontNoisy.IsValid();
    }
    void SetFrontReflectionActive(bool active) { m_frontActive = active; }
    // rgb = pre-Fresnel reflected radiance, a = 1 where a pane was stamped
    // (custom_surface.hlsli's front tier); SRV-readable after this pass.
    uint64_t GetFrontReflectionSrv() const
    { return m_gfx && m_front.IsValid() ? m_gfx->GetTextureSRVGpuHandle(m_front) : 0; }
    // PT-lit toon receivers (PathTrace.cs.hlsl NprAmbient +
    // PathTraceNprDenoise.cs.hlsl): the traced sphere-mean ambient and sun
    // visibility Lighting.ps's NPR variant reads on this path instead of the
    // DDGI / sky taps and the cascades. SetNprLighting: wanted on this path,
    // before EnsureResources (-pt_npr_raster keeps the raster inputs).
    // SetNprLightingActive: an NPR draw is in this frame.
    void SetNprLighting(bool wanted) { m_nprWanted = wanted && !m_nprOff; }
    bool NprLightingReady() const
    {
        return m_nprWanted && m_velocity.IsValid() && m_nprPso.IsValid() && m_nprTemporalPso.IsValid() && m_nprSpatialPso.IsValid() &&
               m_nprFinal.IsValid();
    }
    void SetNprLightingActive(bool active) { m_nprActive = active; }
    // rgb = sphere-mean radiance, a = sun visibility (a < 0: not a receiver);
    // SRV-readable after this pass.
    uint64_t GetNprLightingSrv() const
    { return m_gfx && m_nprFinal.IsValid() ? m_gfx->GetTextureSRVGpuHandle(m_nprFinal) : 0; }
    // A new world (Renderer::OnWorldClear): the RTXDI core's SHaRC radiance cache starts empty.
    // The image histories have their own resets: the view's WorldClear reaches
    // History::Invalidate right after.
    void OnWorldClear(const RG::WorldClearArgs&) override { if (m_rtxdi) m_rtxdi->ClearCache("world"); }
    // The TLAS holds every static occluder again: during a world's BLAS burst (~16 a frame) the
    // paths went through slabs not there yet, and the cache's multi-bounce feedback would hold
    // that light for ~400 frames.
    void OnStaticGeometryComplete() { if (m_rtxdi) m_rtxdi->ClearCache("geometry"); }
    // The render path: true on Path Trace (Real-time) - auto runs RTXDI's core with its
    // real-time preset (PTRtxdi::SetRealtimePreset), whatever the reference settings say - false
    // on Path Trace (Reference), where auto runs this pass's own path tracer. The RTXDI core is
    // remade when it changes.
    void SetRealtimePreset(bool on);
    // Where the RTXDI core takes x1's direct light (PTRtxdi::DirectLight). The core reads it
    // when it is made: a change remakes it.
    void SetDirectLight(PTRtxdi::DirectLight d);
    // The service point's request: the Renderer fills it once, before the DirectLight stage
    // (x1's direct light, which decides whether ReSTIR DI runs for this pass); the PathTrace
    // stage applies the rest, after ReSTIR DI / GI.
    struct ServiceRequest
    {
        PTRtxdi::DirectLight directLight = PTRtxdi::DirectLight::Auto;
        uint32_t areaMode = 0, areaHistory = 0, areaSpatialK = 2;
        bool     areaMotion = false, areaReconnect = false, areaMotionPinned = false;
        float    areaSpatialRadius = 6.f;
        bool     rrRuns = false;     // SetRayReconstruction
        bool     realtime = false;   // SetRealtimePreset: Path Trace (Real-time)
        uint32_t maxBounces = 8;
        bool     reference = false, bouncesPinned = false, referencePinned = false;
        bool     cameraPrimary = false, filmJitter = false, primaryRayCone = false;
        float    lensRadius = 0.f, focusDistance = 5.f;
        ReSTIR::Backend backend = ReSTIR::Backend::Auto;
        bool     frontReflection = false, nprLighting = false;
        bool     denoise = false;    // SetDenoise: the reference path's preview denoiser
        uint32_t denoiseFadeSpp = 64;
        bool     wanted = false;     // EnsureResources: the path-trace path
    };
    ServiceRequest& Request() { return m_request; }
    void OnService(RG::ServiceContext& c) override;
    // The preview denoiser (PathTraceDenoise.cs.hlsl): wanted on this path, before EnsureResources
    // (which builds or releases it). It filters the image, and the camera fog at the fog's own
    // sample count, while it holds fewer than @p fadeSpp samples, faded to the plain running mean
    // from fadeSpp / 2; 0 filters only the frames that restart. RR, camera rays, Area and
    // -pt_nrd_reference bypass it.
    void SetDenoise(bool wanted, uint32_t fadeSpp) { m_dnWanted = wanted; m_dnFadeSpp = fadeSpp; }
    bool IsRealtimePreset() const { return m_realtime; }
    // Owned full-resolution guides survive the transient DI/GI alias arena.
    const RHI::Texture* GetRRGuide(uint32_t index) const { return m_rrEnabled && index<4 ? &m_rrGuides[index] : nullptr; }
    // RR is on in this pass AND its four guides exist, so this frame's trace writes them: only
    // then may DLSS evaluate RR on the path-trace path. On the frame a mode switch turns RR on the
    // guides do not exist yet (they come with the next resource pass), so the Renderer skips RR.
    // Do not evaluate RR without guides: that marks RR failed, and this pass never turns it on.
    bool RayReconstructionReady() const
    { return m_rrEnabled && m_rrGuides[0].IsValid() && m_rrGuides[1].IsValid() && m_rrGuides[2].IsValid() && m_rrGuides[3].IsValid(); }
    // The guide and the state it is in (the debug-target registry's rr* rows).
    const RHI::Texture* GetRRGuide(uint32_t index, RHI::ResourceState& state) const
    {
        const RHI::Texture* t = GetRRGuide(index);
        if (!t || !t->IsValid()) { state = RHI::ResourceState::UNDEFINED; return nullptr; }
        state = m_rrGuideStates[index];
        return t;
    }
    const char* GetInertReason() const;
    uint64_t GetUpcomingOutputSrv() const;
    // Image-only invalidation: the progressive accumulation restarts, the
    // Area reservoirs survive (their MIS re-evaluates every shifted target).
    void ResetImage()
    {
        if (!m_hist) return;
        m_hist->spp = 0;
        if (!m_hist->resetPending) ++m_hist->resets;
        m_hist->resetPending = true;
    }
    // The ReSTIR passes' rule (RTDIPass): a cut, a resize and a world boundary drop the history
    // (the view's reasons, History::Invalidate); the world clear also empties the RTXDI core's
    // SHaRC (OnWorldClear); shutdown after the GPU drain. It records any view: the image, its
    // accumulation, Area history, preview denoiser and camera fog are the view's; the front reflection,
    // the toon lighting, the motion probe, the RTXDI mode, the RR guides and the stats stay the main
    // view's.
    uint32_t HandledEvents() const override { return kShutdown | kWorldClear | kView | kViewSafe; }
    // The view's history: bound at registration, before Init; OnCompile / EnsureNprLighting
    // make its textures.
    void BindView(ViewState& view) override;
    void ResetHistory()
    {
        if (m_rtxdi && m_ws == &m_main) m_rtxdi->ResetHistory();   // the RTXDI mode is the main view's
        if (m_hist) m_hist->Reset();
    }
    uint32_t GetPublishedSamples() const { return m_publishedSamples.load(std::memory_order_relaxed); }
    uint32_t GetPublishedResets() const { return m_publishedResets.load(std::memory_order_relaxed); }
    struct FrameData
    {
        uint64_t tlasVA = 0, instanceVA = 0, alphaVA = 0, buffers = 0;
        DirectX::XMFLOAT4X4 viewProj{};
        DirectX::XMFLOAT4X4 view{}, projection{};
        DirectX::XMFLOAT3 camera{}, sunDirection{}, sunColor{};
        uint32_t instanceCount = 0, skyIdx = 0, lightsIdx = 0, lightCount = 0, materialsIdx = 0;
        uint32_t activeLocalLightCount = 0, firstActiveLocalLightIndex = 0;
        uint32_t pathMaterialsIdx = 0;
        float environmentScale = 1.f;
        float farZ = 1000.f; // NRD denoisingRange for the ReSTIR passes
        // The front reflection's denoiser (DenoiseFrontReflection): the reverse-Z planes (the toon
        // receivers' denoiser reads them too) and this frame's sub-pixel jitter; cameraCut: the
        // camera's cut this frame.
        float nearZ = 0.1f, jitterX = 0.f, jitterY = 0.f;
        bool cameraCut = false;
        // The view's content cut this frame - the camera's, a hard cut or a world clear
        // (HistoryReset::Cuts): the front reflection's denoiser and the toon receivers' lighting
        // history restart on it (cameraCut alone drives the Renderer's cut block).
        bool contentCut = false;
        uint64_t fingerprint = 0, shadingFingerprint = 0, lightsFingerprint = 0;
        uint64_t textureRevision = 0;
        uint32_t surfaceCacheRevision = 0;
        bool stable = false;
        // -pt_static_scene: nothing that moves restarts the image (the object-motion probe included).
        bool staticScene = false;
        // The world clock advanced this frame (FrameView::worldDeltaTime > 0): a material that reads
        // the time looks different from last frame's.
        bool worldClockRuns = false;
        // Lights v2 (ClusterPass::LightHistory): the previous -> current index map (0 = no ring
        // yet), the previous list, whether this frame's list is the previous one unchanged, and the
        // power alias table over the active local lights.
        uint32_t lightMapIdx = 0, prevLightsIdx = 0, prevLightCount = 0;
        bool lightsIdentity = true;
        uint32_t lightAliasIdx = 0, lightAliasCount = 0;
        // The DI pass ran this frame (RestirDIResult is this frame's).
        bool diActive = false;
        // The TLAS holds a BlendMode::Transmissive instance (DDGISceneAS::TransmissiveInstanceCount).
        bool transmissive = false;
        // The fog as a participating medium (Graphics/MediumSet.h): the set's heap index (0 = no
        // medium) and a hash of its contents, which restarts the accumulation when it changes.
        uint32_t mediumIdx = 0;
        uint64_t mediumFingerprint = 0;
        // The local lights' fog scales (ClusterPass; 0 = every light's is 1) and the sun's.
        uint32_t lightMediumScaleIdx = 0;
        float sunMediumScale = 1.f;
        // The medium the rtxdi core's segments cross (RtxdiMediaEnabled; 0 = none). Its contents and
        // rule are in mediumFingerprint. Under the froxel rule, the fog's last lit volume too.
        uint32_t rtxdiMediumIdx = 0;
        Media::FroxelVolume rtxdiFroxel;
    };
    void SetFrameData(const FrameData& fd, bool active);
private:
    struct alignas(16) Constants
    {
        float invViewProj[16];
        float camera[3]; uint32_t spp;
        float sunDirection[3]; uint32_t width;
        float sunColor[3]; uint32_t height;
        uint32_t albedoIdx, normalIdx, surfaceIdx, depthIdx;
        uint32_t alphaTestEnabled, alphaInstanceCount, seed, skyIdx;
        uint32_t lightsIdx, lightCount, maxBounces; float environmentScale;
        float sunCosRadius; uint32_t flags; float constantEnvironment; uint32_t materialsIdx;
        float cameraRight[3]; float lensRadius;
        float cameraUp[3]; float focusDistance;
        float cameraForward[3]; uint32_t pathMaterialsIdx;
        uint32_t rrAlbedoUav,rrSpecularUav,rrNormalUav,rrDistanceUav;
        uint32_t activeLocalLightCount, firstActiveLocalLightIndex;
        uint32_t areaPreviousIdx, areaCurrentIdx;
        uint32_t areaHistoryCap, areaHistoryValid;
        uint32_t areaPreviousFilmIdx, areaCurrentFilmIdx;
        float areaPreviousViewProj[16];
        uint32_t areaPreviousPrimaryIdx, areaCurrentPrimaryIdx;
        uint32_t areaPreviousGeometryIdx, areaCurrentGeometryIdx;
        float areaReconnectWeight; uint32_t areaRadianceIdx; uint32_t areaFlags; float areaSpatialRadius;
        float areaPrevInvViewProj[16];
        float areaPrevCamera[3]; float areaPrevLensRadius;
        float areaPrevRight[3]; float areaPrevFocus;
        float areaPrevUp[3]; uint32_t areaPreviousLensIdx;
        float areaPrevForward[3]; uint32_t areaCurrentLensIdx;
        uint32_t areaSpatialK; float areaSpatialConfidence; uint32_t ptRRStart, lightAliasIdx;
        uint32_t ptLastNeeOff, ptGiShellModel;   // -pt_last_nee_off, -pt_gi_shell_model (PathTrace.cs.hlsl)
        uint32_t auxOutUav;                      // the FrontReflection / NprAmbient dispatches' output; the
                                                 // main entry's sample moment (RecordDenoise)
        uint32_t ptFrontSamples;                 // FrontReflection: bits 0-7 samples per mirror-like texel,
                                                 // bits 8-15 the continuation's light candidates
        // The medium (PT_MEDIA): the set, the arms (1 single scattering, 2 equi-angular), the lights'
        // and the sun's fog scales.
        uint32_t mediumIdx, mediaFlags, lightMediumScaleIdx; float sunMediumScale;
    };
    static_assert(sizeof(Constants) == 544);
    static_assert(offsetof(Constants, areaPrevInvViewProj) == 368);
    static_assert(offsetof(Constants, areaReconnectWeight) == 352);
    static_assert(offsetof(Constants, areaRadianceIdx) == 356);
    RG::RenderGraph& m_graph;
    RG::RGTextureHandle m_albedo, m_normal, m_surface, m_depth, m_result;
    IGraphicsDevice* m_gfx = nullptr;
    // The pass's pipeline layout (Init): every pipeline below is built on it.
    RHI::PipelineLayout m_layout;
    RHI::PipelineState m_pso;
    // Built with its counters and constants on the first EnsureResources(true), not in Init:
    // ~0.27 s that every boot would pay, raster ones included. A failure is not retried until the
    // next Init (a shader hot reload, an Area mode change).
    bool BuildPipelines();
    bool m_pipelinesFailed = false;
    RHI::PipelineState m_areaSpatialPso;
    FrameCB<Constants> m_cb;
    FrameCB<Constants> m_areaSpatialCb;
    RHI::PipelineState m_clearStats;
    RHI::PipelineState m_probe, m_clearProbe;
    RHI::PipelineState m_dielectricProbe, m_dielectricProbeClear, m_dielectricProbeReduce;
    RHI::PipelineState m_mediaProbeClear, m_mediaProbeHg, m_mediaProbeFreeFlight, m_mediaProbeEquiAngular,
                       m_mediaProbeReduce;
    bool m_probeDispatched = false, m_probeReported = false;
    RHI::PipelineState m_hitProbe;
    bool m_hitProbeReported = false;
    bool m_lensConeReported = false;
    bool m_reconnectProbeReported = false;
    uint32_t m_statsBytes = 7 * sizeof(uint32_t);
    RHI::GPUBuffer m_stats, m_readback[RHI::kFramesInFlight];
    const uint32_t* m_mapped[RHI::kFramesInFlight]{};
    bool m_primed[RHI::kFramesInFlight]{};
    RHI::ResourceState m_statsState = RHI::ResourceState::UNDEFINED;
    // The view's path-tracer history (RenderGraph/ViewState.h): one per view, created empty by
    // BindView (at registration, before Init); OnCompile and EnsureNprLighting make its textures,
    // Shutdown destroys them (a shader reload is Shutdown + Init). The RTXDI core keeps its own
    // history and SHaRC, the NRD reference its own; the front-layer reflection's accumulation
    // stays on the pass, with its denoiser.
    // TODO(RenderPipeline_Architecture_Review_Plan VW4): the front-layer accumulation per view.
    struct History final : ViewHistory
    {
        explicit History(PathTracePass* pass) : m_pass(pass) {}
        const char* Name() const override { return "PathTrace"; }
        // Every reason: the cuts (hard, camera, world clear), a resize and a render-path change.
        // Invalidate ignores the camera's cut alone: the Renderer's cut block (FrameData.cameraCut)
        // resets the pass on a frame its plan runs (stage B, mid render half). A scene /
        // view-projection change, the settings, switching on and a shader reload restart it too -
        // the pass's policy.
        uint32_t ResetsOn() const override
        { return HistoryReset::Cuts | HistoryReset::Resize | HistoryReset::PathChange; }
        // The pass's ResetHistory: the RTXDI core's history and the fields here (the bound view's
        // history is this one). Counted once per invalidation (resetPending).
        // TODO(RenderPipeline_Architecture_Review_Plan VW4): a second view.
        // The pass's ResetHistory for this view (the RTXDI mode's history with the main view's).
        void Invalidate(uint32_t reasons) override
        {
            if (!(reasons & (HistoryReset::HardCut | HistoryReset::Resize | HistoryReset::WorldClear |
                             HistoryReset::PathChange)))
                return;
            Reset();
            if (m_pass->m_mainHist == this && m_pass->m_rtxdi) m_pass->m_rtxdi->ResetHistory();
        }
        void Release(IGraphicsDevice& gfx) override;
        void Reset()
        {
            areaValid = false;
            areaFrame = 0;
            spp = 0;
            // Resize, camera and graph invalidation may describe the same frame.
            // Count one invalidation event until a new sample has been submitted.
            if (!resetPending) ++resets;
            resetPending = true;
        }

        // The progressive accumulation and its sample / reset counters.
        RHI::Texture       accum;
        RHI::ResourceState accumState = RHI::ResourceState::UNORDERED_ACCESS;
        uint32_t           spp = 0, resets = 0;
        bool               resetPending = false;
        // Selected replay seed, effective M, normalization W and target density.
        // Both temporal banks are committed, independent of the alias arena.
        RHI::Texture areaHistory[2];
        RHI::Texture areaFilm[2]; // Packed modulo-65536 film rotations, two axes.
        RHI::Texture areaPrimary[2], areaGeometry[2]; // Selected temporal primary context.
        RHI::Texture areaLens[2]; // v2: explicit lens uv (half2 disk) of a reconnected sample.
        RHI::ResourceState areaStates[2]{};
        uint32_t areaWrite = 0, areaFrame = 0;
        bool areaValid = false;
        DirectX::XMFLOAT4X4 areaViewProj{};
        // v2: the camera the Area history was traced from.
        DirectX::XMFLOAT4X4 prevInvViewProj{};
        float prevCamera[3]{}, prevRight[3]{}, prevUp[3]{}, prevForward[3]{};
        float prevLens = 0.f, prevFocus = 5.f;
        bool prevCamValid = false;
        // Free-running RNG frame: never reset, so a moving (reset every frame)
        // camera does not replay the same random stream and freeze the noise.
        uint32_t rngFrame = 0;
        // Consecutive frames whose scene content changed (SetFrameData), and whether last frame
        // traced with the bounce cap (Execute's moving-scene rule).
        uint32_t contentStreak = 0;
        bool lastCapped = false;
        // The toon receivers' lighting history (RecordNprLighting): the ping-pong
        // accumulation and its view depth / length, their states (the pass keeps
        // the noisy / spatial / final scratch's), the parity and the seed frame.
        RHI::Texture       nprHistory[2], nprMeta[2];
        RHI::ResourceState nprStates[4]{};   // history 0/1, meta 0/1
        uint32_t           nprWrite = 0, nprFrame = 0;
        bool               nprValid = false;
        // The camera segment's fog (RecordCameraFog): its RGBA32F running mean at the image's size,
        // its state, the samples in it and the camera it was traced from. fogStale: an image restart
        // Execute found after SetFrameData decided the frame restarts the fog on the next one.
        RHI::Texture        fog;
        RHI::ResourceState  fogState = RHI::ResourceState::UNDEFINED;
        uint32_t            fogW = 0, fogH = 0, fogSpp = 0, fogFrame = 0;
        bool                fogValid = false, fogStale = false;
        DirectX::XMFLOAT4X4 fogViewProj{};
        // The preview denoiser's (RecordDenoise) at the image's size: the history ping-pongs, the
        // a-trous ping-pong and the path tracer's sample moment; which history is written next, whether
        // it holds the last frame, whether the sample moment holds this image's samples (Execute).
        RHI::Texture dnHistory[2], dnMoments[2], dnMeta[2], dnFilter[2], dnSampleMoment;
        uint32_t     dnW = 0, dnH = 0, dnWrite = 0;
        bool         dnHistoryValid = false, dnMomentValid = false;
        // The camera fog's preview denoiser (RecordFogDenoise) at the image's size: the history
        // ping-pongs (light + transmittance; length + view depth), the target the composite reads on
        // the frames it filters and its state; which history is written next, whether it holds the
        // last frame and the view-projection it was written with.
        RHI::Texture        fogDnHistory[2], fogDnMeta[2], fogDnOut;
        RHI::ResourceState  fogDnOutState = RHI::ResourceState::UNDEFINED;
        uint32_t            fogDnW = 0, fogDnH = 0, fogDnWrite = 0;
        bool                fogDnHistoryValid = false;
        DirectX::XMFLOAT4X4 fogDnViewProj{};
    private:
        PathTracePass* m_pass;
    };
    History* m_hist     = nullptr;   // the bound view's (BindView)
    History* m_mainHist = nullptr;   // the main view's (the RTXDI mode's history goes with it)
    // One view's working set: this frame's inputs and run state, the size, the same-frame Area radiance
    // and the reference denoiser - and, a secondary view's, the main view's settings its history was traced
    // with (a change starts it over, as the main view's setters do).
    struct WorkingSet
    {
        FrameData fd{};
        uint32_t w = 0, h = 0;
        bool active = false;
        bool cameraMoved = false;      // SetFrameData: this frame's view-projection is not last frame's
        bool contentChanged = false;   // SetFrameData: this frame's scene content changed
        bool fogWanted = false;        // SetFrameData: the camera segment's fog is traced this frame
        // SetFrameData, for the camera fog: its running mean restarts this frame, the preview denoiser
        // filters it, and its samples after the frame (RecordCameraFog may still restart it).
        bool fogRestart = false, fogDenoise = false;
        uint32_t fogSamples = 0;
        RHI::Texture areaRadiance;   // Same-frame selected radiance, never temporal history.
        RHI::ResourceState areaRadianceState = RHI::ResourceState::UNDEFINED;
        std::unique_ptr<NrdDenoiser> reference;
        uint32_t traced[5]{};        // m_flags, m_maxBounces, m_areaHistoryCap, m_areaSpatialK, the Area / backend bits
        float    tracedF[5]{};       // m_lensRadius, m_focusDistance, m_areaSpatialRadius, m_areaReconnectWeight,
                                     // m_constantEnvironment
    };
    // A secondary view's working set, given back when the view goes (its reference denoiser retired).
    struct ViewWork : ViewData
    {
        WorkingSet     ws;
        PathTracePass* owner = nullptr;
        void Release(IGraphicsDevice& gfx) override;
    };
    WorkingSet  m_main;
    WorkingSet* m_ws = &m_main;   // the bound view's (BindView)
    // NRD tears its pool down at once: an instance a view drops waits out the frames in flight that may
    // still read it (OnService counts the frame boundaries down).
    std::vector<std::pair<std::unique_ptr<NrdDenoiser>, uint32_t>> m_retiredNrd;
    // The bound view's working set at @p w x @p h (its history textures with it).
    void BuildWorkingSet(IGraphicsDevice& gfx, uint32_t w, uint32_t h);
    bool m_areaV2 = false, m_areaV2Reconnect = false, m_areaNoIdentity = false;
    uint32_t m_areaSpatialK = 2; float m_areaSpatialRadius = 6.f, m_areaSpatialConfidence = 1.f;
    uint32_t m_areaHistoryCap = 20;
    float m_areaReconnectWeight = 1.f;
    bool m_areaMotion = false;
    uint32_t m_areaModeRequested = 0, m_areaCapRequested = 20;
    bool m_areaMotionRequested = false, m_areaMotionPinned = false;
    RHI::Texture m_rrGuides[4];
    RHI::ResourceState m_rrGuideStates[4]{};
    bool m_rrEnabled=false;
    uint32_t m_rrFrame=0;
    std::atomic<uint32_t> m_publishedSamples{0}, m_publishedResets{0};
    bool m_wanted = false;
    uint32_t m_maxBounces = 1, m_flags = 0;
    uint32_t m_seed = 1;
    float m_constantEnvironment = 0.f;
    float m_lensRadius = 0.f, m_focusDistance = 5.f;
    bool m_cameraSettingsLogged = false;
    float m_sunAngularRadius = 0.0047f;
    // The settings' unlimited reference arm, bounded while the frame does not accumulate (Area,
    // an unstable scene); explicit CLI choices are exact.
    bool m_referenceFromSettings = false, m_bounceCapCli = false, m_spatialGate = true;
    bool m_realtime = false;   // SetRealtimePreset
    ServiceRequest m_request;  // OnService's
    uint32_t m_effectiveBounces = 1, m_rrStart = 3;
    bool m_bounceCapLogged = false;
    bool m_uniformLights = false; // -pt_uniform_lights: uniform next-event light choice, not the power alias table
    bool m_lastNeeOff = false;    // -pt_last_nee_off: RTXDI's path-length count (the RtxdiPT A/B)
    // -pt_gi_shell_model: the bounce vertices take the ReSTIR GI shell's hit model, a matched
    // reference for tools/restir_gi_energy.py.
    bool m_giShellModel = false;
    // The rtxdi mode's core: built when the mode is in effect, released with the pass's working
    // set.
    std::unique_ptr<PTRtxdi::Core> m_rtxdi;
    RG::RGTextureHandle m_velocity, m_diResult;
    bool m_rtxdiLogged = false;
    void RecordRtxdi(RHI::CommandList cl);
    // The front-layer reflection (SetFrontLayer): its entry, constants and
    // accumulation, built on the first frame it is wanted. m_frontSpp counts
    // its own samples: it restarts with the image and after a frame the stamp
    // was not traced.
    RG::RGTextureHandle m_frontDepth, m_frontNormal, m_frontSurface, m_frontVelocity;
    // The fog as a participating medium (PT_MEDIA): the media variants of the image's entries and
    // the camera segment's (FogCamera), built the first frame the medium is traced (a failure is
    // not retried until the next Init); the camera fog's accumulation (the view's History: an
    // RGBA32F running mean at the image's size that restarts with the image and whenever the camera
    // moves); what [PT_MEDIA] last printed. SetFrameData decides the fog's restart and its sample
    // count for the frame (WorkingSet::fogRestart, fogSamples) before GetCameraFogIndex hands the
    // composite its target; an image restart Execute finds later restarts the fog on the next frame
    // (History::fogStale), filtered, unless this frame filters it or nothing would.
    bool BuildMediaPipelines();
    bool EnsureCameraFog(IGraphicsDevice& gfx);
    void ReleaseCameraFog(IGraphicsDevice& gfx);
    void RecordCameraFog(RHI::CommandList cl, uint32_t imageSpp);
    void WriteMediaConstants(Constants& cb) const;
    const char* MainEntry() const;
    const char* SpatialEntry() const;
    RHI::PipelineState m_mediaPso, m_mediaSpatialPso, m_fogPso;
    FrameCB<Constants> m_fogCb;
    bool m_mediaFailed = false;
    uint64_t m_mediaLogKey = ~0ull;   // the main view's
    RHI::PipelineState m_frontPso;
    FrameCB<Constants> m_frontCb;
    RHI::Texture m_front;
    RHI::ResourceState m_frontState = RHI::ResourceState::UNDEFINED;
    uint32_t m_frontW = 0, m_frontH = 0, m_frontSpp = 0, m_frontFrame = 0;
    bool m_frontWanted = false, m_frontActive = false, m_frontFailed = false, m_frontValid = false;
    bool m_frontOff = false;   // -pt_front_off
    // The front denoiser (every frame that does not accumulate): the per-frame sample, NRD's
    // inputs / output (pass-owned, their states tracked here) and the pack / resolve entries with
    // their constants (PathTraceFrontNRD.cs.hlsl's cbuffer).
    struct alignas(16) FrontNrdConstants
    {
        uint32_t width, height, depthIdx, normalIdx;
        uint32_t surfaceIdx, velocityIdx, noisyIdx, normalRoughUav;
        uint32_t viewZUav, mvUav, specInUav, specOutIdx;
        uint32_t frontUav; float nearZ, farZ, hitDistA;
    };
    static_assert(sizeof(FrontNrdConstants) == 64);
    std::unique_ptr<NrdDenoiser> m_frontNrd;
    RHI::PipelineState m_frontPackPso, m_frontResolvePso;
    FrameCB<FrontNrdConstants> m_frontNrdCb;
    RHI::Texture m_frontNoisy, m_frontNrdNormalRough, m_frontNrdViewZ, m_frontNrdMV, m_frontNrdSpecIn, m_frontNrdSpecOut;
    RHI::ResourceState m_frontNrdStates[6]{};
    uint32_t m_frontNrdFrame = 0;
    uint32_t m_frontMirrorSamples = 2;   // -pt_front_spp
    uint32_t m_frontNeeCandidates = 8;   // -pt_front_nee
    bool m_frontNrdValid = false;   // last frame's history is this pane's
    bool EnsureFrontDenoiser(IGraphicsDevice& gfx, uint32_t w, uint32_t h);
    void ReleaseFrontDenoiser(IGraphicsDevice& gfx);
    void DenoiseFrontReflection(RHI::CommandList cl);
    // PT-lit toon receivers (SetNprLighting): the sample entry and its own
    // constants, the denoiser's entries with one constants ring per dispatch
    // (temporal, horizontal, vertical - a FrameCB holds one write per frame),
    // the per-frame sample, the ping-pong accumulation (+ view depth / length),
    // the spatial intermediate and the texture Lighting reads.
    struct alignas(16) NprDenoiseConstants
    {
        uint32_t width, height, depthIdx, velocityIdx;
        uint32_t noisyIdx, historyIdx, historyMetaIdx, historyOutUav;
        uint32_t historyMetaOutUav, spatialInIdx, spatialOutUav, direction;
        float nearZ, farZ; uint32_t reset, pad;
    };
    static_assert(sizeof(NprDenoiseConstants) == 64);
    RHI::PipelineState m_nprPso, m_nprTemporalPso, m_nprSpatialPso;
    FrameCB<Constants> m_nprCb;
    FrameCB<NprDenoiseConstants> m_nprDenoiseCb[3];
    RHI::Texture m_nprNoisy, m_nprSpatial, m_nprFinal;   // + the view's history / meta ping-pongs
    RHI::ResourceState m_nprStates[3]{};   // noisy, spatial, final (history / meta: the view's)
    // The state of NPR target @p i in the order the tables list them - noisy,
    // history 0/1, meta 0/1, spatial, final: the pass's or the view's.
    RHI::ResourceState& NprState(uint32_t i)
    { return (i >= 1 && i <= 4) ? m_hist->nprStates[i - 1] : m_nprStates[i == 0 ? 0 : i - 4]; }
    uint32_t m_nprW = 0, m_nprH = 0;
    bool m_nprWanted = false, m_nprActive = false, m_nprFailed = false;
    bool m_nprOff = false;   // -pt_npr_raster
    void EnsureNprLighting(IGraphicsDevice& gfx);
    void ReleaseNprLighting(IGraphicsDevice& gfx);
    void RecordNprLighting(RHI::CommandList cl);
    // The auxiliary entries' (FrontReflection, NprAmbient) constants shared
    // with the image, and their root binding (through @p pso, the entry about
    // to run: it sets the layout the arguments go to).
    Constants AuxConstants(uint32_t width, uint32_t height) const;
    void BindAuxRoot(IGraphicsDevice& gfx, RHI::CommandList cl, const RHI::GPUBuffer& constants,
                     const RHI::PipelineState& pso);
    void EnsureFrontReflection(IGraphicsDevice& gfx);
    void ReleaseFrontReflection(IGraphicsDevice& gfx);
    void RecordFrontReflection(RHI::CommandList cl, uint32_t imageSpp);
    // The preview denoiser (SetDenoise): Temporal + four a-trous passes, each with its own
    // constants (a FrameCB holds one write a frame). The view's history ping-pongs (illumination +
    // length, luminance moments, normal + view depth), the a-trous ping-pong and the path
    // tracer's sample moment (RTStoreSample through the main entry's auxOutUav) stay in
    // UNORDERED_ACCESS; UAV barriers order the passes. A cut, a resize, a frame the path tracer
    // did not run and a frame that did not denoise (other than the fade) invalidate the history.
    struct alignas(16) DenoiseConstants
    {
        uint32_t width, height, depthIdx, normalIdx;
        uint32_t albedoIdx, velocityIdx, resultUav, sampleMomentUav;
        uint32_t historyUav, historyMomentsUav, historyMetaUav, historyOutUav;
        uint32_t historyMomentsOutUav, historyMetaOutUav, filterInUav, filterOutUav;
        uint32_t step, samples, flags, motionUav;
        float nearZ, farZ, fadeStart, fadeEnd;
        uint32_t materialsIdx, fogIdx, pad[2];
        // The camera fog's reprojection (FogTemporal): this frame's inverse view-projection and the
        // history's view-projection, transposed.
        float fogInvViewProj[16], fogPrevViewProj[16];
    };
    static_assert(sizeof(DenoiseConstants) == 240);
    static constexpr uint32_t kDenoisePasses = 5;
    // The bound view's denoiser history at @p w x @p h (the pipelines once, for every view).
    void EnsureDenoiser(IGraphicsDevice& gfx, uint32_t w, uint32_t h);
    void ReleaseDenoiser(IGraphicsDevice& gfx);
    // This frame filters: wanted, ready, no bypass, and @p samples (the image's after this frame)
    // under the fade.
    bool DenoiseRuns(uint32_t samples, bool restarted) const;
    void RecordDenoise(RHI::CommandList cl, uint32_t samples, bool restarted);
    RHI::PipelineState m_dnTemporalPso, m_dnAtrousPso;
    FrameCB<DenoiseConstants> m_dnCb[kDenoisePasses];
    uint32_t m_dnFadeSpp = 64, m_dnFrames = 0;   // m_dnFrames: the main view's [PT_DENOISE] lines
    bool m_dnWanted = false, m_dnFailed = false;
    // The camera fog's preview denoiser (RecordFogDenoise): the image's filter run over the fog
    // (FogTemporal + four FogAtrous) on the frames DenoiseRuns would filter at the fog's sample
    // count, with its own constants. Its history ping-pong (light + transmittance; length + view
    // depth; both UNORDERED_ACCESS), the view-projection the history was written with and the
    // target the composite reads on those frames (RGBA16F, SHADER_RESOURCE after the filter) are the
    // view's History. The a-trous ping-pong is the image's denoiser's, which runs first: DenoiseRuns
    // holds only while it exists. WorkingSet::fogDenoise: this frame filters the fog (SetFrameData).
    // EnsureFogDenoiser makes the bound view's targets at its image's size (the pipelines once, for
    // every view).
    void EnsureFogDenoiser(IGraphicsDevice& gfx);
    void ReleaseFogDenoiser(IGraphicsDevice& gfx);
    void RecordFogDenoise(RHI::CommandList cl);
    RHI::PipelineState m_fogDnTemporalPso, m_fogDnAtrousPso;
    FrameCB<DenoiseConstants> m_fogDnCb[kDenoisePasses];
    uint32_t m_fogDnFrames = 0;   // the main view's [PT_FOG_DENOISE] lines
    bool m_fogDnFailed = false;
    // Object motion on screen (PathTraceDenoise.cs.hlsl MotionProbe): the GBuffer pixels whose
    // motion vector is not 0 while the camera holds still - WPO and anything else the
    // fingerprints do not see - and the pixels whose material reads the time
    // (kMatGpuFlagReadsTime). Counted on the GPU each frame, read back kFramesInFlight frames
    // later; moved pixels from a still-camera frame, or time-reading ones from a frame the world
    // clock ran, restart the image like a scene change.
    void EnsureMotionProbe(IGraphicsDevice& gfx);
    void RecordMotionProbe(RHI::CommandList cl);
    // Execute, before the frame's sample index is taken: true when a frame the probe measured had
    // pixels moving on their own.
    bool ReadObjectMotion(IGraphicsDevice& gfx);
    RHI::PipelineState m_motionClearPso, m_motionProbePso;
    FrameCB<DenoiseConstants> m_motionCb;
    RHI::GPUBuffer m_motion, m_motionReadback[RHI::kFramesInFlight];
    const uint32_t* m_motionMapped[RHI::kFramesInFlight]{};
    bool m_motionPrimed[RHI::kFramesInFlight]{};
    bool m_motionCameraStill[RHI::kFramesInFlight]{};   // the camera held still the frame that slot measured
    bool m_motionClockRan[RHI::kFramesInFlight]{};      // the world clock advanced the frame that slot measured
    RHI::ResourceState m_motionState = RHI::ResourceState::UNDEFINED;
    bool m_motionFailed = false;
    bool m_objectMotion = false;         // the main view's probe saw pixels moving on their own (Execute)
    uint32_t m_motionPixels = 0, m_timePixels = 0;   // the last counts read back
    ReSTIR::Backend m_requested = ReSTIR::Backend::Area;
    // The request resolved by use: the concrete core and why ("auto: ...", or the thin-lens
    // fallback of an explicit rtxdi); nullptr = as requested.
    ReSTIR::Backend m_resolved = ReSTIR::Backend::Area;
    const char* m_resolveWhy = nullptr;
    ReSTIR::Backend ResolveRequest(ReSTIR::Backend requested, const char*& why) const;
    bool m_bouncesCli = false;   // -pt_bounces: the engine path tracer's own path length
    ReSTIR::BackendStatus m_status{ReSTIR::Backend::Area, ReSTIR::Backend::Area, "area", "area"};
    bool m_backendDirty = true, m_logBackend = false;
    double m_pipelineMs = 0.0;
    void ApplyBackend();
    uint64_t AreaResidentBytes() const;
};
