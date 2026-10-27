#pragma once

// FSR2Pass - AMD FidelityFX FSR upscaler.
//
// Standalone (non-RG) compute pass that runs in place of TAA when the frame's plan
// selects FSR (at UpscaleMode::Native it is a 1:1 AA). Reads the render-resolution
// HDR scene + depth + motion vectors and produces the display-resolution HDR texture
// ToneMap consumes.
//
// Owns FFX::FidelityFXBackend (the ffxContext, recreated on a size change) and
// m_outputTex (R16G16B16A16_FLOAT, SRV+UAV, display res).
//
// Per frame: SetSizes + EnsureContext on the main thread (OnService's Upscalers
// stage); SetCameraParams, SetJitter (pixels, render res), SetMotionVectorScale
// (NDC -> pixels), SetSrcTexturesComputeRead and SetReactiveMask; then
// Execute(cl, gfx). ToneMap reads GetOutputSrvGpuHandle().

#include "RenderGraph/PassLifecycle.h"
#include "RenderGraph/ViewState.h"
#include "Graphics/FidelityFXBackend.h"
#include "Graphics/GraphicsStruct.h"

struct ID3D12Resource;
class IGraphicsDevice;

class FSR2Pass : public RG::PassLifecycle
{
public:
    struct ServiceRequest { bool wanted = false; uint32_t renderW = 0, renderH = 0, displayW = 0, displayH = 0; };

    FSR2Pass() = default;
    ~FSR2Pass();
    FSR2Pass(const FSR2Pass&)            = delete;
    FSR2Pass& operator=(const FSR2Pass&) = delete;

    // No GPU work: the output texture + FFX context are created by EnsureContext once
    // the sizes are known.
    bool Init(IGraphicsDevice& gfx);
    void Shutdown(IGraphicsDevice& gfx) override;
    uint32_t HandledEvents() const override { return kShutdown | kView; }   // kView: the reset flag lives in the view's history (BindView)
    // Binds the view's reset flag; FFX keeps the history itself.
    void BindView(ViewState& view) override;

    // Per-frame configuration.
    void SetSizes(uint32_t renderW, uint32_t renderH,
                  uint32_t displayW, uint32_t displayH)
    {
        m_renderW = renderW; m_renderH = renderH;
        m_displayW = displayW; m_displayH = displayH;
    }
    void SetCameraParams(float nearZ, float farZ, float fovYRadians, float frameTimeMs)
    {
        m_cameraNear  = nearZ;
        m_cameraFar   = farZ;
        m_cameraFovY  = fovYRadians;
        m_frameTimeMs = frameTimeMs;
    }
    void SetJitter(float jitterPxX, float jitterPxY)
    {
        m_jitterX = jitterPxX;
        m_jitterY = jitterPxY;
    }
    // Motion vector scale: FSR multiplies the input motion vector by this
    // to get pixel-space motion at render res. For engine's NDC velocity
    // (currentNDC - previousNDC): scale = (-renderW * 0.5,
    // +renderH * 0.5), producing previousUV - currentUV for FSR.
    void SetMotionVectorScale(float sx, float sy)
    {
        m_motionScaleX = sx;
        m_motionScaleY = sy;
    }
    void SetSrcResources(ID3D12Resource* color,  uint32_t colorStateFfx,
                         ID3D12Resource* depth,  uint32_t depthStateFfx,
                         ID3D12Resource* motion, uint32_t motionStateFfx)
    {
        m_colorRes  = color;  m_colorStateFfx  = colorStateFfx;
        m_depthRes  = depth;  m_depthStateFfx  = depthStateFfx;
        m_motionRes = motion; m_motionStateFfx = motionStateFfx;
    }
    // The inputs, all in compute-read state (the only case the Renderer supplies: its
    // pre-compute CL transitions them first), as RHI textures - @p color null = the HDR
    // scene target. Execute resolves them for FFX and hands them to SetSrcResources.
    void SetSrcTexturesComputeRead(const RHI::Texture* color,
                                   const RHI::Texture* depth,
                                   const RHI::Texture* motion);
    // The dispatch's reactive mask (R8, render resolution, compute-readable;
    // UpscalerReactiveMaskPass) - null = none this frame. Set every frame before Execute.
    // The engine's exposure (AutoExposurePass's 1x1 texture, compute-readable) for this dispatch; null
    // without one.
    void SetExposureTexture(const RHI::Texture* tex) { m_exposureTex = tex; }
    void SetReactiveMask(const RHI::Texture* mask) { m_reactiveTex = mask; }
    void SetReset(bool reset) { if (m_hist) m_hist->reset = reset; }

    // Ensure the FFX context + output texture exist for the current sizes. Call on the
    // main thread BEFORE kicking the worker threads that call Execute: CreateTexture and
    // context creation are not worker-thread-safe. Returns true on success (or if
    // already valid).
    bool EnsureContext(IGraphicsDevice& gfx);

    // Records FSR dispatch into @p cl. Requires EnsureContext to have
    // succeeded for the current sizes. No-op on failure.
    void Execute(RHI::CommandList cl, IGraphicsDevice& gfx);

    // Output at display resolution; 0 until the output texture exists.
    uint64_t            GetOutputSrvGpuHandle(const IGraphicsDevice& gfx) const;
    const RHI::Texture* GetOutputTexture() const { return &m_outputTex; }
    RHI::ResourceState  GetOutputState()   const { return m_outputState; }
    void                SetOutputState(RHI::ResourceState s) { m_outputState = s; }

    bool IsContextValid() const { return m_backend.IsValid(); }

    ServiceRequest& Request() { return m_request; }
    // Filled by the Renderer for OnService's Upscalers stage, before the plan's resolve:
    // the context for this frame's sizes while FSR 2 is the plan's upscaler; at any other
    // mode a live context is released (its VRAM back).
    void OnService(RG::ServiceContext& c) override;

    // Bridge to the FFX jitter sequence: the Renderer jitters the projection with it so
    // projection and FSR dispatch agree. Returns false on backend error (the caller
    // falls back to Halton).
    bool QueryJitter(uint32_t frameIndex, float& outX, float& outY);

private:
    struct History final : ViewHistory
    {
        bool reset = true;   // the next dispatch discards the accumulation

        const char* Name() const override { return "FSR2"; }
        // The content's cuts (hard cut, camera cut, world clear) and a render-path change,
        // which the view tells it. Resize is named but not acted on here: a new render or
        // display size rebuilds the context, and RebuildForSizes resets.
        uint32_t ResetsOn() const override
        { return HistoryReset::Cuts | HistoryReset::Resize | HistoryReset::PathChange; }
        void Invalidate(uint32_t reasons) override
        { if (reasons & (HistoryReset::Cuts | HistoryReset::PathChange)) reset = true; }
        void Release(IGraphicsDevice&) override {}
    };

    // Recreates the context + output texture for the current sizes (EnsureContext calls
    // it on a size change). Returns true on success.
    bool RebuildForSizes(IGraphicsDevice& gfx);

public:
    // FSR temporal reconstruction is intentionally soft without a sharpen
    // stage, including Native 1:1 AA mode.
    bool  sharpeningEnabled = true;
    float sharpness          = 0.25f;

    // Execute puts the shared compute root signature back after FFX's
    // dispatch. False only for pack fsr2_bare_post's reverse arm
    // (-fsr2_keep_ffx_layout, read by PostProcess::FSR2Effect).
    bool  restoreEngineLayout = true;
    // Test hook: EnsureContext always fails, as a context lost at runtime
    // (harness -upscaler_fail=fsr, read by the Renderer).
    bool  failContextForTest = false;

private:
    ServiceRequest m_request;   // the Renderer's, read by OnService
    bool m_contextFailLogged = false;   // OnService: one WARNING per failing streak

    FFX::FidelityFXBackend m_backend;

    // Display-res output (R16G16B16A16_FLOAT, SRV+UAV).
    RHI::Texture       m_outputTex;
    RHI::ResourceState m_outputState{};
    bool               m_outputCreated = false;

    uint32_t m_renderW       = 0;
    uint32_t m_renderH       = 0;
    uint32_t m_displayW      = 0;
    uint32_t m_displayH      = 0;
    uint32_t m_lastRenderW   = 0;
    uint32_t m_lastRenderH   = 0;
    uint32_t m_lastDisplayW  = 0;
    uint32_t m_lastDisplayH  = 0;

    // The inputs as the Renderer hands them, and the resources Execute resolves them to.
    const RHI::Texture* m_colorTex    = nullptr;
    bool                m_colorIsHdrScene = false;   // set by SetSrcTexturesComputeRead(nullptr, ...)
    const RHI::Texture* m_depthTex    = nullptr;
    const RHI::Texture* m_motionTex   = nullptr;
    const RHI::Texture* m_reactiveTex = nullptr;
    const RHI::Texture* m_exposureTex = nullptr;
    // The context was made without FSR's own metering (pass.upscaler.engineExposure), so each dispatch is
    // given the engine's exposure. The setting changing re-makes the context.
    bool                m_engineExposure = false;
    uint32_t            m_dispatches = 0;
    uint32_t            m_dispatchesWithExposure = 0;
    ID3D12Resource* m_colorRes      = nullptr;
    ID3D12Resource* m_depthRes      = nullptr;
    ID3D12Resource* m_motionRes     = nullptr;
    ID3D12Resource* m_reactiveRes   = nullptr;   // the reactive mask; always compute-read
    uint32_t        m_colorStateFfx  = 0;  // FfxApiResourceState bitfield
    uint32_t        m_depthStateFfx  = 0;
    uint32_t        m_motionStateFfx = 0;

    float m_motionScaleX  = 0.0f;
    float m_motionScaleY  = 0.0f;
    float m_cameraNear    = 0.1f;
    float m_cameraFar     = 1000.0f;
    float m_cameraFovY    = 1.0f;
    float m_frameTimeMs   = 16.66f;
    float m_jitterX       = 0.0f;
    float m_jitterY       = 0.0f;

    // The view's FSR 2 history state (a ViewHistory, RenderGraph/ViewState.h): the
    // accumulation itself lives in the FFX context; the engine's side is whether the
    // next dispatch must discard it. Created by BindView.
    History* m_hist = nullptr;   // the bound view's (BindView); none = every dispatch resets
};
