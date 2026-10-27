#pragma once

// AutoExposurePass - histogram-based auto exposure, two compute dispatches per frame:
//   1. CSHistogramBuild   - 256-bin log-luma histogram of the HDR scene (+ the non-finite count).
//   2. CSHistogramAverage - the adapted exposure from that histogram.
// The exposure lives in the view's 2-float DEFAULT buffer, read by ToneMapPass. The pass runs first in the
// Resolve stage, so NPRStabilize, the upscalers and ToneMap all see the exposure of the frame they belong to.
// While an upscaler runs, the view also keeps the value in a 1x1 R32_FLOAT texture: its Exposure input.

#include "Graphics/FrameConstants.h"
#include "RenderGraph/RenderGraph.h"
#include "RenderGraph/ViewState.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"

#include <array>
#include <atomic>
#include <string>

class AutoExposurePass : public RG::RenderPass
{
public:
    const char* GetName() const override { return "AutoExposurePass"; }
    void Setup  (RG::RenderGraphBuilder&) override {}
    void Init   (IGraphicsDevice& gfx)   override;
    // RG::PrepareStage PostEffects: the exposure texture, only while an upscaler is selected
    // (PrepareContext::upscaler) and it takes the engine's exposure.
    void OnPrepare(const RG::PrepareContext& c) override;
    // Hot reload (standalone: Renderer::ReloadShaders calls it).
    void ReloadShaders(IGraphicsDevice& gfx) override;
    // Moves the exposure buffer from its initial combined pixel + compute SHADER_RESOURCE state to
    // SHADER_RESOURCE_COMPUTE on a graphics command list, before the compute queue consumes it.
    void PrepareForCompute(RHI::CommandList graphicsCl);
    RHI::CommandList Execute(RHI::CommandList cl) override;

    // Set per-frame inputs.
    void SetHdrSrvHandle(uint64_t handle)      { m_hdrSrvHandle = handle; }
    void SetViewportSize(uint32_t w, uint32_t h) { m_vpW = w; m_vpH = h; }

    // Master enable. When false, Execute() skips the histogram dispatches and uploads
    // m_manualExposure into the same exposure buffer; ToneMapPass reads the same SRV either way.
    void  SetEnabled(bool e)         { m_enabled = e; }
    bool  IsEnabled()          const { return m_enabled; }
    // Harness reverse arm: the bias before the bounds, and not on the manual exposure.
    void  SetLegacyBias(bool on)     { m_legacyBias = on; }
    // Fixed exposure used when disabled. 1.0 = no scaling.
    void  SetManualExposure(float e) { m_manualExposure = e; }
    float GetManualExposure()  const { return m_manualExposure; }

    // Tuning (optional; the defaults are reasonable). bright applies when the scene brightens
    // (exposure falls), dark when it darkens (exposure rises); adaptation runs in stops, so equal
    // rates are symmetric.
    void SetAdaptationRate(float bright, float dark)
    {
        m_adaptationRate     = bright;
        m_adaptationRateDark = dark;
    }
    // Callers MUST pass hi > lo: Execute() divides by hi - lo clamped to at least 0.1, so an inverted
    // or degenerate pair meters a 0.1-wide log-luma range.
    void SetLuminanceRange(float lo, float hi) { m_minLogLuma = lo; m_maxLogLuma = hi; }
    void SetPercentiles(float lo, float hi)   { m_lowPercent = lo; m_highPercent = hi; }
    void SetMeteringInputs(uint64_t normalSrv, uint64_t depthSrv, uint64_t materialSrv,
                           uint32_t metaW, uint32_t metaH, uint32_t materialCount)
    {
        m_normalSrv = normalSrv;
        m_depthSrv = depthSrv;
        m_materialSrv = materialSrv;
        m_metaW = metaW;
        m_metaH = metaH;
        m_materialCount = materialCount;
    }
    // Rows skipped per sampled row in the build kernel. A 256-bin histogram is a
    // coarse statistic: 4 gives a statistically identical result for a quarter
    // of the bandwidth. 1 = meter every pixel.
    void SetRowStride(uint32_t s) { m_yStride = (s == 0u) ? 1u : s; }
    void SetMeteringWeights(float center, float sky, float npr, float highlight)
    {
        m_centerWeight = center;
        m_skyWeight = sky;
        m_nprWeight = npr;
        m_highlightWeight = highlight;
    }
    void SetMinExposure(float e)              { m_minExposure = e; }
    void SetMaxExposure(float e)              { m_maxExposure = e; }
    // EV compensation in stops: +1 -> 2x brighter, -1 -> 2x darker. Useful when a very bright IBL
    // sky makes AE crush the subject. It goes on after the min / max exposure bounds, and onto the
    // manual exposure too.
    void  SetEVBias(float ev)        { m_evBias = ev; }
    float GetEVBias() const          { return m_evBias; }
    // Middle-grey key target (default 0.18). Larger = brighter auto-exposure.
    void  SetKeyValue(float k)       { m_keyValue = k; }
    float GetKeyValue() const        { return m_keyValue; }
    float GetLowPercent()  const     { return m_lowPercent; }
    float GetHighPercent() const     { return m_highPercent; }
    float GetMinExposure() const     { return m_minExposure; }
    float GetMaxExposure() const     { return m_maxExposure; }

    // The exposure buffer (Renderer passes it to gfx.GetBufferSRVGpuHandle()): the bound view's, or
    // before BindView the one Init made.
    const RHI::GPUBuffer& GetExposureBuffer() const { return m_hist ? m_hist->exposure : m_initExposure; }
    // The state this pass last left it in (SHADER_RESOURCE_COMPUTE at the end
    // of a frame: ToneMapPass reads it) - what a synchronous readback of the
    // frame's exposure round-trips through (Renderer::ReadFrameExposure).
    RHI::ResourceState GetExposureBufferState() const { return m_hist ? m_hist->exposureState : m_initExposureState; }
    // The bound view's exposure texture (SHADER_RESOURCE_COMPUTE once Execute has run); null until an
    // upscaler asked for it.
    const RHI::Texture* GetExposureTexture() const
    { return (m_hist && m_hist->exposureTex.IsValid()) ? &m_hist->exposureTex : nullptr; }
    RHI::ResourceState GetExposureTextureState() const { return m_hist ? m_hist->exposureTexState : RHI::ResourceState::UNDEFINED; }
    // The exposure texture's value as the GPU holds it, read back synchronously after the CPU frame is
    // drained (Render.exposure{texture = true}). False, with @p error, when the view has no texture.
    bool ReadExposureTexture(float& value, std::string& error);

    // ---- Debug / tuning readback ----
    // Off by default: it costs one extra state transition and an 8-byte GPU -> CPU copy per frame. The
    // editor turns it on while the Auto Exposure panel is open, the harness on every frame it records
    // (frames.jsonl nanPixels). Values lag kFrameCount - 1 frames: a readout, never an input to
    // rendering. Requests decay: each Execute() that meters consumes one frame of the TTL, so a panel
    // that stops drawing stops paying, with no "disable" call to forget on an editor early-out path.
    void  SetDebugReadbackEnabled(bool e) { m_debugReadbackTtl = e ? kDebugReadbackTtl : 0u; }
    bool  IsDebugReadbackEnabled() const  { return m_debugReadbackTtl > 0u; }
    // Last exposure the GPU actually wrote, or 0 when no reading is available.
    float GetLastExposure() const         { return m_lastExposure; }
    // Same value in stops relative to 1.0 (what a lighting artist reads).
    float GetLastExposureEV() const;
    // The metering histogram of a metered frame: 256 weighted counts over log2 luma in [minLog, maxLog] (the
    // range the meter used; bin b is minLog + b / 255 * (maxLog - minLog)). False while no reading exists:
    // readback off, not primed, or no frame has metered (a held or manual exposure meters nothing).
    static constexpr uint32_t kHistogramBins = 256;
    bool GetLastHistogram(std::array<uint32_t, kHistogramBins>& bins, float& minLog, float& maxLog) const
    {
        if (!m_lastHistogramValid) return false;
        bins = m_lastHistogram; minLog = m_lastHistMinLog; maxLog = m_lastHistMaxLog;
        return true;
    }
    // Metered pixels of that frame whose luma was NaN / Inf (rejected by the histogram, counted).
    // -1 = no reading: readback off, not primed, or auto exposure disabled (the manual path meters
    // nothing).
    int64_t GetLastNonFinitePixels() const { return m_lastNonFinite; }

    // ---- Holding the exposure ----
    // A held exposure is neither metered nor adapted: the buffer keeps its value, so a frozen path-traced
    // frame, a 1 spp frame after a Render.ptStep restart and a sequence's frames agree. It holds while
    // locked (Lua Render.lockExposure) or while the world clock is frozen (SetFrozen, from the chain's
    // context), unless a snap is pending (a world clear, a new view, RequestMeter): that meters once.
    void SetLocked(bool on)        { m_locked.store(on, std::memory_order_relaxed); }
    bool IsLocked() const          { return m_locked.load(std::memory_order_relaxed); }
    void SetFrozen(bool frozen)    { m_frozen = frozen; }
    bool IsHolding() const         { return !m_holdOff && (IsLocked() || m_frozen); }
    // The next frame meters once and snaps to the result, held or not.
    void RequestMeter()            { m_meterRequested.store(true, std::memory_order_relaxed); }
    bool ConsumeMeterRequest()     { return m_meterRequested.exchange(false, std::memory_order_relaxed); }
    // Harness reverse arm: nothing holds.
    void SetHoldOff(bool off)      { m_holdOff = off; }
    // A pending snap (a world clear, a new view, RequestMeter) is held, snapping every frame, until the scene
    // has settled (its meshes and material textures arrived) or kSnapWaitFrames have passed: a snap to a
    // half-streamed picture would fade up from it over seconds. @p wait false (a harness reverse arm)
    // takes the snap at once.
    void SetSceneSettled(bool settled, bool wait) { m_sceneSettled = settled || !wait; }

    // Discards temporal history so the next frame adapts in one step. Call on world load and hard
    // camera cuts: without it a new level starts from the previous level's adapted exposure and drags
    // to the new target at the normal multi-second rate.
    void Snap() { if (m_hist) m_hist->snap = true; }
    // kView: BindView per view; a world clear resets the view's history; kViewSafe: it meters any view.
    uint32_t HandledEvents() const override { return kReloadShaders | kView | kViewSafe; }
    // Binds the view's exposure history (created on first bind). The first view to bind adopts the
    // exposure buffer Init made; a later view's history makes its own, which snaps to its first
    // measurement.
    void BindView(ViewState& view) override;

private:
    static constexpr uint32_t kFrameCount = RHI::kFramesInFlight;

    struct alignas(16) AutoExposureCB
    {
        uint32_t width;
        uint32_t height;
        float    minLogLuma;
        float    invLogLumaRange;
        float    adaptationRate;
        float    lowPercent;
        float    highPercent;
        float    minExposure;
        float    maxExposure;
        float    evBias;
        float    keyValue;
        float    centerWeight;
        float    skyWeight;
        float    nprWeight;
        float    highlightWeight;
        float    adaptationRateDark;
        // Dispatch-uniform values hoisted off the per-pixel path.
        float    hiStart;
        float    hiEnd;
        uint32_t metaWidth;
        uint32_t metaHeight;
        uint32_t materialCount;
        uint32_t yStride;
        uint32_t debugNonFinite;   // -ae_inject_nan (harness): the first n columns meter as NaN
        uint32_t debugFlags;       // kFlagLegacyBias | kFlagExportTexture | kFlagExportOnly
    };
    static constexpr uint32_t kFlagLegacyBias    = 1u;   // harness: the bias before the bounds
    static constexpr uint32_t kFlagExportTexture = 2u;   // the average kernel also writes the exposure texture
    static constexpr uint32_t kFlagExportOnly    = 4u;   // no metering: only the texture takes the buffer's value

    // Frames one SetDebugReadbackEnabled(true) keeps the readback on; each Execute() that meters
    // consumes one (the disabled path and the early returns do not).
    static constexpr uint32_t kDebugReadbackTtl = 4;
    // Frames a snap waits for the scene to settle at most (2 s at 60 fps).
    static constexpr uint32_t kSnapWaitFrames = 120;

    // The view's adapted exposure (RenderGraph/ViewState.h).
    struct History final : ViewHistory
    {
        // 2-float exposure: [0] the exposure (read by ToneMap, NPRStabilize and
        // LightCB.exposureBufIdx), [1] the frame's non-finite count. UAV + SRV.
        RHI::GPUBuffer     exposure;
        RHI::ResourceState exposureState = RHI::ResourceState::UNDEFINED;
        // The same value as a 1x1 R32_FLOAT texture, made on the main thread when an upscaler first needs it.
        RHI::Texture       exposureTex;
        RHI::ResourceState exposureTexState = RHI::ResourceState::UNDEFINED;
        // Set by Snap(), consumed by the next Execute(): forces the adaptation rate
        // to 1.0 for one frame so the meter jumps straight to its target.
        bool               snap = false;
        uint32_t           snapWait = 0;   // frames the snap has been held for the scene to settle

        const char* Name() const override { return "AutoExposure"; }
        // A world clear or a hard cut (new world, a cut of the shown content, Play / Stop, a Sequencer
        // jump) snaps the exposure to its first measurement instead of fading from the previous shot's
        // value. A camera cut (an in-game shot change) and a render-path change keep the adaptation. A
        // manual-exposure frame also snaps it (Execute).
        uint32_t ResetsOn() const override { return HistoryReset::HardCut | HistoryReset::WorldClear; }
        void Invalidate(uint32_t) override { snap = true; }
        void Release(IGraphicsDevice& gfx) override;
    };

    bool CreatePipelines(IGraphicsDevice& gfx);
    // The view's exposure texture, initial value 1; false (logged) on failure.
    bool CreateExposureTexture(History& h);
    // The exposure texture takes the buffer's value in a dispatch of its own (a held or manual exposure).
    void RecordExport(RHI::CommandList cl, History& h);
    // The debug readback ring's copy of the view's exposure and its read of the oldest slot.
    void RecordReadback(RHI::CommandList cl, History& h, bool primary);
    // The same ring for the histogram, between the build and the average (which clears the bins).
    void RecordHistogramReadback(RHI::CommandList cl, float minLog, float maxLog);
    // A view's exposure buffer ({1, 0}) and its state after the upload; false (logged) on failure.
    bool CreateExposureBuffer(RHI::GPUBuffer& out, RHI::ResourceState& state);

    IGraphicsDevice* m_gfx = nullptr;
    RHI::PipelineState m_buildPSO;
    RHI::PipelineState m_averagePSO;
    ShaderLibrary       m_shaderLib;

    // 256-bin histogram + the non-finite pixel count (uint), UAV only
    RHI::GPUBuffer m_histogramBuffer;
    RHI::ResourceState m_histogramState = RHI::ResourceState::UNDEFINED;
    // The exposure buffer Init made, until BindView hands it to the view's history. Allocated between
    // the histogram and the CB on purpose: allocation order changes pictures.
    RHI::GPUBuffer m_initExposure;
    RHI::ResourceState m_initExposureState = RHI::ResourceState::UNDEFINED;

    // UPLOAD staging that pushes m_manualExposure into the exposure buffer when the pass is disabled;
    // persistently mapped. One per frame in flight: Execute() writes a slot and CopyBuffer queues a GPU
    // read of it, so a single buffer would race the pending copy when the CPU runs ahead.
    RHI::GPUBuffer m_manualStaging[kFrameCount];
    void*          m_manualStagingMapped[kFrameCount] = {};

    // Per-dispatch CB
    FrameCB<AutoExposureCB> m_cb;

    uint64_t m_hdrSrvHandle = 0;
    uint64_t m_normalSrv = 0;
    uint64_t m_depthSrv = 0;
    uint64_t m_materialSrv = 0;
    uint32_t m_vpW = 0;
    uint32_t m_vpH = 0;
    uint32_t m_metaW = 0;
    uint32_t m_metaH = 0;
    uint32_t m_materialCount = 0;
    uint32_t m_yStride = 4;

    float m_adaptationRate     = 0.02f; // set per frame by AutoExposureEffect via SetAdaptationRate()
    float m_adaptationRateDark = 0.02f;
    float m_minLogLuma     = -5.0f;
    float m_maxLogLuma     =  3.5f;
    float m_lowPercent     = 0.50f;
    float m_highPercent    = 0.85f;
    float m_minExposure    = 0.10f;
    float m_maxExposure    = 8.0f;
    float m_evBias         = 0.0f;   // exposure compensation in EV stops
    float m_keyValue       = 0.18f;  // middle-grey target
    float m_centerWeight   = 2.0f;
    float m_skyWeight      = 0.25f;
    float m_nprWeight      = 0.50f;
    float m_highlightWeight = 0.50f;

    bool  m_enabled        = true;
    bool  m_legacyBias     = false;
    std::atomic<bool> m_locked{ false };
    std::atomic<bool> m_meterRequested{ false };
    bool  m_frozen         = false;
    bool  m_sceneSettled   = true;
    bool  m_holdOff        = false;
    float m_manualExposure = 1.0f;   // used when !m_enabled

    // ---- Debug readback ring ----
    // Debug readback ring: one READBACK slot per frame in flight; Execute() copies into the current
    // slot and reads the oldest, which the GPU has finished with. Persistently mapped, like
    // m_manualStaging.
    RHI::GPUBuffer m_readback[kFrameCount];
    void*          m_readbackMapped[kFrameCount] = {};
    uint32_t       m_debugReadbackTtl = 0;
    bool           m_readbackPrimed[kFrameCount] = {};
    RHI::GPUBuffer m_histReadback[kFrameCount];
    void*          m_histReadbackMapped[kFrameCount] = {};
    bool           m_histPrimed[kFrameCount] = {};
    std::array<uint32_t, kHistogramBins> m_lastHistogram{};
    bool           m_lastHistogramValid = false;
    float          m_lastHistMinLog = 0.0f, m_lastHistMaxLog = 0.0f;
    float          m_lastExposure  = 0.0f;
    int64_t        m_lastNonFinite = -1;

    // ---- The bound view's history ----
    History* m_hist = nullptr;   // the bound view's (BindView)
};
