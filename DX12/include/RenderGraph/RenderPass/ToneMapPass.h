#pragma once

// ToneMapPass - ACES tone mapping + bloom composite + color grading LUT.
//
// Reads:  HDR scene (SRV), bloom[0] (SRV), exposure buffer (SRV), LUT 3D (SRV),
//         optional lens flare (SRV).
// Writes: the composition buffer (UAV) - RGBA8_UNORM in SDR, R16G16B16A16_FLOAT in
//         an HDR output mode (HDRCompositionFormat() in Graphics/HDROutput.h).
//         Anything that binds it as a render target must ask GetFinalOutputFormat().
//
// The contents are extended-gamma, not a display signal: 1.0 is paper white and
// above 1.0 is HDR headroom. The overlay passes and the editor's ImGui blend into
// it in that space; the present pixel shader does the final PQ / scRGB encode.
//
// Owns a 32^3 3D LUT; when ColorGradingParams change, a compute dispatch bakes it
// before the tonemap dispatch. Neutral parameters skip the LUT altogether.

#include "RenderGraph/RenderGraph.h"
#include "RenderGraph/ViewState.h"   // ViewData (a secondary view's composition buffer)
#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/HDROutput.h"
#include "Graphics/FrameCB.h"
#include "RenderGraph/RenderPass/ColorGradingParams.h"
#include "RenderGraph/RenderPass/UEToneMapParams.h"

class ToneMapPass : public RG::RenderPass
{
public:
    // Harness switches; ToneMapEffect reads their flags. injectColumns: the first n columns of the
    // input read NaN and the next n +Inf. unguarded skips the non-finite guard and noDither the SDR
    // output dither (reverse arms).
    struct DebugSwitches
    {
        uint32_t injectColumns = 0;
        bool     unguarded     = false;
        bool     noDither      = false;
    };

    // The base name every instance's GetName() starts with, kept in one place.
    static constexpr const char* kBaseName = "ToneMapPass";

    // Instance name: kBaseName plus the optional suffix. Not a graph pass (the
    // Renderer drives it through PostProcess::ToneMapEffect; Setup() is never called),
    // so a second instance is cheap. GetName() still names the profiler region, and
    // GetGPUTimestampResult is a first-match-wins name scan: two instances with one
    // name would report the union of both.
    const char* GetName() const override { return m_name.c_str(); }
    // Call before the pass starts recording (see GBufferPass.h).
    void SetNameSuffix(const char* suffix);

    void Setup  (RG::RenderGraphBuilder&) override {}
    void Init   (IGraphicsDevice& gfx)   override;
    // Releases the shaders, PSOs, CB, LUT and output texture. Call only at a quiesced
    // boundary: an earlier ImGui::Image may hold a descriptor of m_finalOutput, and
    // ImGui draw data outlives a panel close by a frame (PreviewRenderer's idle-teardown
    // grace covers that). Main thread only, like EnsureTexture and SetOutputMode.
    void Shutdown(IGraphicsDevice& gfx)  override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    // Hot reload. Not a graph pass: Renderer::ReloadShaders reloads the main
    // instance, PreviewRenderer::ReloadShaders every preview target's. Also
    // re-bakes the grading LUT, which GenerateLUT.cs wrote with the old code.
    void ReloadShaders(IGraphicsDevice& gfx) override;
    // Events Renderer::PassSet delivers; it records any view (its composition buffer is the view's).
    uint32_t HandledEvents() const override { return kShutdown | kReloadShaders | kView | kViewSafe; }
    // The main view keeps the composition buffer below; a secondary view keeps its own in its
    // ViewState. Every call after this one is that view's.
    void BindView(ViewState& view) override;

    // Set per-frame inputs.
    void SetHdrSrvHandle(uint64_t handle)          { m_hdrSrvHandle = handle; }
    void SetBloomSrvHandle(uint64_t handle)        { m_bloomSrvHandle = handle; }
    void SetExposureSrvHandle(uint64_t handle)     { m_exposureSrvHandle = handle; }
    void SetLensFlareSrvHandle(uint64_t handle)    { m_lensFlareSrvHandle = handle; }
    void SetViewportSize(uint32_t w, uint32_t h)   { m_vpW = w; m_vpH = h; }

    void  SetLensFlareStrength(float v) { m_lensFlareStrength = v; }
    float GetLensFlareStrength() const  { return m_lensFlareStrength; }

    // MAIN THREAD ONLY. The dirty test covers the output mode as well as the size:
    // otherwise toggling HDR at a fixed window size keeps the RGBA8 texture.
    void EnsureTexture(uint32_t w, uint32_t h)
    {
        m_vpW = w; m_vpH = h;
        if (m_vpW != m_out->lastW || m_vpH != m_out->lastH || m_outputMode != m_out->lastMode)
            RebuildTexture();
    }

    // ---- HDR output ----
    // Set from the Renderer on the main thread, next to EnsureTexture and before the
    // compute worker is kicked - never from ToneMapEffect::Execute (RenderWorker_Compute),
    // where a texture rebuild would race descriptor allocation.
    void SetOutputMode(HDROutputMode mode, float paperWhiteNits, float peakNits)
    {
        m_outputMode     = mode;
        m_paperWhiteNits = paperWhiteNits;
        m_peakNits       = peakNits;
    }
    HDROutputMode GetOutputMode() const { return m_outputMode; }
    // PostOutput prepare stage: applies the main surface's output mode, then sizes the
    // composition buffer to the viewport (EnsureTexture's dirty test includes the mode).
    void OnPrepare(const RG::PrepareContext& c) override;

    // The format every PSO that renders INTO this texture must declare.
    RHI::Format GetFinalOutputFormat() const { return HDRCompositionFormat(m_outputMode); }

    float GetBloomStrength() const      { return m_bloomStrength; }
    void  SetBloomStrength(float v)     { m_bloomStrength = v; }
    void     SetDebugSwitches(const DebugSwitches& d) { m_debug = d; }

    // ---- Color grading ----
    // The grading the next Execute applies. The LUT is baked per parameter set and kept (kMaxLuts of them), so
    // views grading differently - the main view and a secondary view in an underwater volume - bake once, not
    // every frame.
    void SetColorGradingParams(const ColorGradingParams& p) { m_gradingParams = p; }
    // Makes room for @p count parameter sets: main thread, when a secondary view is made (the main view's LUT
    // is made at Init and a game without views never makes more).
    void ReserveLutSlots(uint32_t count);
    const ColorGradingParams& GetColorGradingParams() const { return m_gradingParams; }
    void SetUEColorGradingParams(const UEToneMapParams& p) { m_ueGrading = p; }
    bool  IsColorGradingEnabled() const { return m_colorGradingEnabled; }
    // Only gates the LUT fetch; do not dirty the LUT here. The LUT depends only on the
    // params and the GenerateLUT shader, each of which dirties it, and ToneMapEffect
    // calls this every frame: a per-frame re-bake is not bit-stable on every GPU
    // (1-ulp fp16 differences between pipelined and drained frames).
    void  SetColorGradingEnabled(bool v){ m_colorGradingEnabled = v; }

    // SRV handle of the composition buffer; 0 before it exists.
    uint64_t GetFinalOutputSrvHandle() const;

    const RHI::Texture* GetFinalOutputTexture() const { return &m_out->tex; }
    // The composition buffer @p view renders into (null: it has none yet), whichever view is bound.
    const RHI::Texture* FinalOutputOf(const ViewState& view) const;

    void TransitionForDisplay(RHI::CommandList& cl);
    void PrepareForCompute(RHI::CommandList& cl);

    // Out-of-pass barrier coordination: UIPass and the overlays read the tracked
    // state to emit matching transitions, then write back the state they leave the
    // texture in.
    RHI::ResourceState GetFinalOutputState() const { return m_out->state; }
    void               SetFinalOutputState(RHI::ResourceState s) { m_out->state = s; }

private:
    struct alignas(16) ToneMapCB
    {
        uint32_t width;
        uint32_t height;
        float    bloomStrength;
        uint32_t enableLUT;
        float    lensFlareStrength;
        uint32_t hdrMode;       // HDROutputMode
        float    hdrPeakRatio;  // peakNits / paperWhiteNits
        uint32_t reserved;      // unused; keeps the CB layout
        UEToneMapParams ueGrading;
        uint32_t debugInjectColumns;   // DebugSwitches::injectColumns
        uint32_t debugFlags;           // bit 0: DebugSwitches::unguarded, bit 1: noDither
    };

    // One view's composition buffer, and what it was last built for (EnsureTexture's dirty test).
    struct Output
    {
        RHI::Texture       tex;
        RHI::ResourceState state{};
        uint32_t           lastW    = 0;
        uint32_t           lastH    = 0;
        HDROutputMode      lastMode = HDROutputMode::SDR;
    };
    struct ViewOutput : ViewData
    {
        Output out;
        void Release(IGraphicsDevice& gfx) override
        {
            if (out.tex.IsValid()) gfx.DestroyTexture(out.tex);
            out = Output{};
        }
    };

    void RebuildTexture();
    static constexpr uint32_t kMaxLuts = 4;
    // One baked grading LUT (32^3, R16G16B16A16_FLOAT, SRV + UAV) and the parameters it holds.
    struct LutSlot
    {
        RHI::Texture       tex;
        RHI::ResourceState state{};
        ColorGradingParams params;
        bool               baked   = false;
        uint64_t           lastUse = 0;
    };
    bool CreateLutSlot(LutSlot& slot);
    void BakeLUT(RHI::CommandList cl, LutSlot& slot);
    // The slot holding m_gradingParams, baked now if no slot has them (least recently used replaced); null
    // when no slot can be baked.
    LutSlot* LutFor(RHI::CommandList cl);
    bool CreatePipelines(IGraphicsDevice& gfx);

    // See GetName() / SetNameSuffix.
    std::string m_name = kBaseName;

    // Tonemap PSO
    RHI::PipelineState m_pso;

    // LUT bake PSO
    RHI::PipelineState m_lutPso;

    ShaderLibrary m_shaderLib;

    // Composition buffer (HDRCompositionFormat of the output mode; SRV + UAV + RT): the main view's
    // here, a secondary view's in its ViewState; m_out is the bound view's (BindView).
    Output              m_mainOutput;
    Output*             m_out = &m_mainOutput;

    // The grading LUTs, by parameter set; slot 0 is made at Init.
    LutSlot             m_luts[kMaxLuts];
    uint64_t            m_lutClock = 0;

    // Per-dispatch CB (tonemap); mirrors `cbuffer PerDispatch` in ToneMap.cs.hlsl.
    // The optional UE grading block follows the 32-byte prefix; FrameCB allocates the
    // expanded payload.
    // Execute() builds it fresh (`ToneMapCB cb{}`) and assigns each field: a field
    // added here but not assigned there reads 0, which for hdrMode silently means SDR.
    FrameCB<ToneMapCB> m_cb;

    // ---- HDR output state ----
    HDROutputMode m_outputMode     = HDROutputMode::SDR;
    float         m_paperWhiteNits = kHDRPaperWhiteDefault;
    float         m_peakNits       = kHDRPeakNitsDefault;

    // Color grading state
    ColorGradingParams m_gradingParams;
    UEToneMapParams m_ueGrading;
    bool               m_colorGradingEnabled = true;

    uint64_t m_hdrSrvHandle       = 0;
    uint64_t m_bloomSrvHandle     = 0;
    uint64_t m_exposureSrvHandle  = 0;
    uint64_t m_lensFlareSrvHandle = 0;
    uint32_t m_vpW = 0;
    uint32_t m_vpH = 0;

    float m_bloomStrength     = 0.04f;
    float m_lensFlareStrength = 1.0f;
    DebugSwitches m_debug;

    IGraphicsDevice* m_gfx = nullptr;
};
