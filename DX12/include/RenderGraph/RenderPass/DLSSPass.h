#pragma once

// DLSSPass - NVIDIA DLSS Super Resolution / DLAA, and Ray Reconstruction (RR).
//
// A near-mirror of FSR2Pass (standalone non-RG compute pass, same lifecycle split,
// same output contract): the Renderer's upscale branch picks one or the other, so a
// structural divergence becomes a bug in whichever arm is less exercised.
//
// Owns the display-resolution output (R16G16B16A16_FLOAT, R32G32B32A32_FLOAT while
// RR's radiance scale is not 1; SRV+UAV) and the DLSS feature's configuration
// (StreamlineBackend::SetDLSSOptions). The Streamline runtime, the per-frame
// constants and the frame token are process-wide in SL::StreamlineBackend, because
// Frame Generation and Reflex consume the same ones.
//
// Per frame: SetSizes / SetQualityMode / EnsureContext on the MAIN THREAD (OnService's
// Upscalers stage); SetSrcTexturesComputeRead, then Execute(cl, gfx, frameToken) on
// RenderWorker_Compute; ToneMap reads GetOutputSrvGpuHandle().
// Threading (as FSR2Pass, load-bearing): EnsureContext creates textures and can
// allocate GPU memory inside the feature; Execute only records, never allocates.

#include "RenderGraph/PassLifecycle.h"
#include "RenderGraph/ViewState.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/StreamlineBackend.h"
#include <wrl/client.h>
#include <d3d12.h>

struct ID3D12Resource;
class IGraphicsDevice;

class DLSSPass : public RG::PassLifecycle
{
public:
    // Filled by the Renderer for OnService.
    struct ServiceRequest
    {
        bool restirReset = false;   // RR runs on a ReSTIR path this frame
        bool upscaler = false;      // DLSS (SR or RR) is the plan's upscaler - the rest is filled only then
        bool rr = false;
        SL::DLSSRRModel rrModel = SL::DLSSRRModel::D;
        SL::DLSSMode quality = SL::DLSSMode::DLAA;
        uint32_t renderW = 0, renderH = 0, displayW = 0, displayH = 0;
    };

    DLSSPass() = default;
    ~DLSSPass() = default;
    DLSSPass(const DLSSPass&)            = delete;
    DLSSPass& operator=(const DLSSPass&) = delete;

    bool Init(IGraphicsDevice& gfx);
    void Shutdown(IGraphicsDevice& gfx) override;
    // Hot reload (Renderer::ReloadShaders): the one shader this pass compiles
    // itself, the RR radiance-scale CS - the upscaler is Streamline's.
    void ReloadShaders(IGraphicsDevice& gfx) override;
    uint32_t HandledEvents() const override { return kReloadShaders | kShutdown | kView; }   // kView: the reset flag lives in the view's history (BindView)
    // Binds the view's reset flag; the DLSS feature keeps the history itself.
    void BindView(ViewState& view) override;

    // ---- Per-frame configuration ----
    void SetSizes(uint32_t renderW, uint32_t renderH,
                  uint32_t displayW, uint32_t displayH)
    {
        m_renderW = renderW;  m_renderH  = renderH;
        m_displayW = displayW; m_displayH = displayH;
    }
    void SetQualityMode(SL::DLSSMode m) { m_qualityMode = m; }
    void SetColorSrvIndex(uint32_t index) { m_colorSrvIndex=index; }
    void SetRayReconstruction(bool enabled, SL::DLSSRRModel model) { m_rr=enabled; m_rrModel=model; }
    bool HasResources() const { return m_outputCreated; }
    bool IsHealthy() const { return m_healthy; }

    // Re-arms RR after a failed evaluate (Streamline's eWarnOutOfVRAM on a shared GPU,
    // for one): its fallback, progressive PT, never converges in a scene that never holds
    // still. Called once a frame (the Retry service stage): 120 frames after a failure,
    // doubling up to 1920, RR re-arms through a rebuild that frees and re-creates its
    // working set. Under -dlss_rr_fail_evaluate (the injected failure) RR stays down.
    void ServiceRetry();

    ServiceRequest& Request() { return m_request; }
    // The Retry stage re-arms a failed RR evaluate; the RRReset stage resets RR's history
    // in THIS frame's Streamline constants when a ReSTIR pass that will run resets its own
    // (read from the board). The Upscalers stage, before the plan's resolve, configures the
    // feature for this frame's sizes and modes while DLSS is the plan's upscaler, and
    // releases its resources at any other mode.
    void OnService(RG::ServiceContext& c) override;
    uint32_t RestirHistoryResets() const { return m_restirResets; }
    bool UpdateRRView(const DirectX::XMFLOAT4X4& view, const DirectX::XMFLOAT4X4& inverseView);
    // The RR guides (albedo, specular albedo, normal + roughness, specular hit distance)
    // as RHI textures; Execute resolves them to the native resources Streamline takes.
    void SetRRGuides(const RHI::Texture* albedo, const RHI::Texture* specular, const RHI::Texture* normal,
                     const RHI::Texture* distance)
    { m_rrGuideTex[0]=albedo; m_rrGuideTex[1]=specular; m_rrGuideTex[2]=normal; m_rrGuideTex[3]=distance; }
    // The optional diffuse hit distance (ReSTIR GI's connection distance,
    // NON_PIXEL_SHADER_RESOURCE); null = not tagged.
    void SetRRDiffuseHitDistance(const RHI::Texture* distance) { m_rrDiffuseDistanceTex=distance; }
    // The colour before transparency (RRColorSnapshot, NON_PIXEL_SHADER_RESOURCE);
    // null = not tagged.
    void SetRRColorBeforeTransparency(const RHI::Texture* color) { m_rrColorBeforeTransparencyTex=color; }
    // DLSS SR's BiasCurrentColor hint (UpscalerReactiveMaskPass, R8 at the render size,
    // NON_PIXEL_SHADER_RESOURCE): tagged on every SR evaluate, null = "no hint this
    // frame"; never on an RR evaluate.
    void SetBiasCurrentColor(const RHI::Texture* mask) { m_biasCurrentColorTex=mask; }
    // The engine's exposure (AutoExposurePass's 1x1 texture, compute-readable) for this evaluate; null
    // without one. Tagged as the Exposure input when pass.upscaler.engineExposure is on.
    void SetExposureTexture(const RHI::Texture* tex) { m_exposureTex=tex; }
    void SetJitter(float jitterPxX, float jitterPxY)
    {
        m_jitterX = jitterPxX; m_jitterY = jitterPxY;
    }
    void SetReset(bool reset) { if (m_hist) m_hist->reset = reset; }

    // Whether the next evaluate must discard DLSS's temporal history.
    // FSR2 passes its reset in the dispatch; Streamline carries it in the per-frame
    // constants, which the Renderer uploads. So the flag must be readable here, or
    // SetReset is a write nobody observes and every hard cut (Play/Stop, Sequencer seek,
    // VCam cut, teleport) smears the pre-cut frame across the new view. Cleared in
    // Execute once an evaluate has consumed it.
    bool IsResetPending() const { return !m_hist || m_hist->reset; }
    // The view's reasons that reset DLSS's history (History::Invalidate). The Renderer
    // also reads them from a frame's pending breaks when it uploads that frame's
    // constants: the view applies them only at the render half's start, after the upload.
    static uint32_t ViewResetReasons() { return HistoryReset::Cuts | HistoryReset::PathChange; }
    // Whether this frame's constants carried a reset (FrameView::dlssResetSent): the
    // evaluate clears only a reset they carried, so one asked for after the upload - the
    // render half's own hard cuts, an AA-mode switch - reaches the next frame's constants
    // instead of being cleared unseen. Set before each Execute (the resolve's DLSSEffect).
    void SetConstantsCarryReset(bool carried) { m_constantsCarryReset = carried; }

    // Input textures, all already transitioned to a compute-readable state by the
    // caller's pre-compute command list (same contract as FSR2Pass).
    // @p color null = the HDR scene target.
    void SetSrcTexturesComputeRead(const RHI::Texture* color,
                                   const RHI::Texture* depth,
                                   const RHI::Texture* motion);

    // ---- Lifecycle ----
    // MAIN THREAD. Creates/resizes the output texture and (re)configures the DLSS
    // feature. Returns true when the pass is ready to Execute this frame.
    bool EnsureContext(IGraphicsDevice& gfx);

    // COMPUTE WORKER. Tags the inputs and evaluates DLSS into @p cl. @p frameToken must
    // be the token taken at this frame's single serialisation point (see
    // StreamlineBackend's call-order note).
    void Execute(RHI::CommandList cl, IGraphicsDevice& gfx, void* frameToken);

    // ---- Output ----
    uint64_t            GetOutputSrvGpuHandle(const IGraphicsDevice& gfx) const;
    const RHI::Texture* GetOutputTexture() const { return &m_outputTex; }
    RHI::ResourceState  GetOutputState()   const { return m_outputState; }
    void                SetOutputState(RHI::ResourceState s) { m_outputState = s; }

    // True once the output exists, the feature is configured for the current sizes, AND
    // evaluation is actually working. The Renderer gates the whole display-res
    // post-process domain on this, so it must never be optimistic: a pass that reports
    // valid but never writes its output presents as an unexplained black screen.
    bool IsContextValid() const { return m_configured && m_outputCreated && m_healthy; }

    // Sub-pixel jitter for @p frameIndex, in pixels at render resolution.
    // Streamline publishes no jitter sequence (unlike FFX); the host owns it. DLSS expects
    // a Halton(2,3) sequence whose phase count scales with the square of the upscale
    // ratio (8 at 1:1, 32 at 2x); a fixed 16-phase sequence under-samples at Performance
    // and reads as residual aliasing that no preset fixes.
    bool QueryJitter(uint32_t frameIndex, float& outX, float& outY) const;

private:
    struct History final : ViewHistory
    {
        bool reset = true;   // the next evaluate discards the history

        const char* Name() const override { return "DLSS"; }
        // The content's cuts (hard cut, camera cut - SR too, not only RR - and world clear)
        // and a render-path change: the view tells it, and UploadFrameData reads the frame's
        // own into the constants. Resize is named but not acted on here: a rebuilt feature
        // (RebuildForSizes: new size, quality mode, preset or RR model) resets, as do the
        // AA-mode switch (Renderer::SetAAMode) and RR's ReSTIR reset (OnService).
        uint32_t ResetsOn() const override
        { return HistoryReset::Cuts | HistoryReset::Resize | HistoryReset::PathChange; }
        void Invalidate(uint32_t reasons) override { if (reasons & ViewResetReasons()) reset = true; }
        void Release(IGraphicsDevice&) override {}
    };

    // Evaluate failures in a row before the pass disables itself (m_consecutiveFailures).
    static constexpr uint32_t kMaxConsecutiveFailures = 4;

    bool RebuildForSizes(IGraphicsDevice& gfx);

    bool PrepareRadianceEncoding(IGraphicsDevice& gfx);
    bool CreateScalePSO(IGraphicsDevice& gfx);
    void ScaleRadiance(RHI::CommandList cl, IGraphicsDevice& gfx, uint32_t inputIndex,
        RHI::Texture& output, uint32_t width, uint32_t height, float scale);

public:
    // ---- Per-frame configuration ----
    // Test hook: EnsureContext always fails, as a context lost at runtime
    // (harness -upscaler_fail=dlss, read by the Renderer).
    bool failContextForTest = false;

    // Streamline exposes sharpness through the optimal-settings query rather
    // than as a free parameter; this is the value it recommended, applied on the
    // next SetDLSSOptions.
    float sharpness = 0.0f;

    // Which DL model DLSS runs, applied on the next RebuildForSizes.
    // Pinned to M, not the SDK default: Default gives K for DLAA/Quality/Balanced and M/L
    // for Performance/UltraPerformance, so the quality slider would change the MODEL as
    // well as the resolution (K is softer and more ghost-prone: character outlines read
    // visibly thicker in exactly those three modes). M is the most stable of the four
    // here, and pinning makes a quality change a resolution change and nothing else.
    // Pinning also removes an OTA hazard: sl_dlss.h says eDefault "may or may not change
    // after an OTA", and this engine enables OTA.
    SL::DLSSPreset preset = SL::DLSSPreset::M;

private:
    ServiceRequest m_request;      // the Renderer's, read by OnService
    uint32_t m_restirResets = 0;   // RR history resets a ReSTIR reset asked for (the [DLSS_RR] count)

    RHI::PipelineLayout m_scaleLayout;   // RRRadianceScale's private layout (five root constants)
    RHI::PipelineState  m_scalePSO;
    RHI::Texture m_rrEncodedInput;
    RHI::ResourceState m_rrEncodedState{};
    float m_rrRadianceScale=1.f;
    uint32_t m_colorSrvIndex=~0u;
    RHI::Texture       m_outputTex;
    RHI::Texture       m_rrExposureTex;
    float              m_rrExposure = 0.f; // zero selects SDK auto exposure
    RHI::ResourceState m_outputState{};
    bool               m_outputCreated = false;
    bool               m_configured    = false;

    uint32_t m_renderW  = 0, m_renderH  = 0;
    uint32_t m_displayW = 0, m_displayH = 0;
    uint32_t m_lastRenderW = 0, m_lastRenderH = 0;
    uint32_t m_lastDisplayW = 0, m_lastDisplayH = 0;
    SL::DLSSMode m_qualityMode     = SL::DLSSMode::DLAA;
    SL::DLSSMode m_lastQualityMode = SL::DLSSMode::Off;
    // Mirrors m_lastQualityMode: the value the SDK was last configured with, so
    // an editor change to `preset` is detected. (The first configure is already
    // guaranteed by m_configured == false, not by this seed.)
    SL::DLSSPreset m_lastPreset    = SL::DLSSPreset::Default;

    // The inputs as the Renderer hands them...
    const RHI::Texture* m_colorTex = nullptr;
    bool                m_colorIsHdrScene = false;   // set by SetSrcTexturesComputeRead(nullptr, ...)
    const RHI::Texture* m_depthTex  = nullptr;
    const RHI::Texture* m_motionTex = nullptr;
    const RHI::Texture* m_rrGuideTex[4]{};
    const RHI::Texture* m_rrDiffuseDistanceTex = nullptr;
    const RHI::Texture* m_rrColorBeforeTransparencyTex = nullptr;
    const RHI::Texture* m_biasCurrentColorTex = nullptr;
    const RHI::Texture* m_exposureTex = nullptr;
    // The feature was configured to take the engine's exposure (SetDLSSOptions useAutoExposure off, the
    // texture tagged); the setting changing reconfigures it.
    bool m_engineExposure=false;
    // ...and the resources Execute resolves them to for this evaluate.
    ID3D12Resource* m_colorRes  = nullptr;
    ID3D12Resource* m_depthRes  = nullptr;
    ID3D12Resource* m_motionRes = nullptr;
    ID3D12Resource* m_rrGuides[4]{};
    ID3D12Resource* m_rrDiffuseDistance=nullptr;
    ID3D12Resource* m_rrColorBeforeTransparency=nullptr;   // RR only
    ID3D12Resource* m_biasCurrentColor=nullptr;            // SR only
    bool m_rr=false,m_lastRR=false;
    SL::DLSSRRModel m_rrModel=SL::DLSSRRModel::D,m_lastRRModel=SL::DLSSRRModel::D;
    uint32_t m_rrEvaluations=0;

    float m_jitterX = 0.0f;
    float m_jitterY = 0.0f;

    // The view's DLSS history state (a ViewHistory, RenderGraph/ViewState.h): the history
    // itself lives in the DLSS feature; the engine's side is whether the next evaluate
    // must discard it (IsResetPending: the frame constants carry it). Created by BindView.
    History* m_hist = nullptr;   // the bound view's (BindView); none = every evaluate resets
    bool     m_constantsCarryReset = true;   // SetConstantsCarryReset

    // Self-disable after repeated evaluate failures, so a broken DLSS
    // configuration degrades to TAA instead of to a black screen. Reset by
    // RebuildForSizes, so changing quality mode or resolution retries.
    //
    // THREADING: written by Execute on RenderWorker_Compute (the post chain's recording
    // worker), read on the main thread (UploadFrameData, the frame plan). Safe without
    // atomics: the render half joins that worker, and the frame boundary drains the render
    // half before the next frame's UploadFrameData runs - the happens-before edge.
    uint32_t m_consecutiveFailures = 0;
    // RR's failure state; ServiceRetry re-arms it.
    bool     m_rrFailed = false;
    uint32_t m_rrFailFrames = 0, m_rrRetries = 0;
    bool     m_healthy             = true;
    bool     m_contextFailLogged   = false;   // OnService: one WARNING per failing streak
};
