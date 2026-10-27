#pragma once

// SSRTemporalPass - pass 4 of the Hi-Z SSR pipeline.
//
// Dual-reprojection history accumulation: blends the resolve output with the previous
// frame's history using both velocity reprojection (surface motion) and reflection-hit
// reprojection (the hit point's previous-frame UV). Keeps ping-pong color / variance
// buffers plus a depth history for disocclusion detection, all at full render res.

#include "Graphics/GraphicsStruct.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/FrameCB.h"
#include "RenderGraph/PassLifecycle.h"
#include "RenderGraph/ViewState.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>

class IGraphicsDevice;

// A PassLifecycle only for the view: its history lives in the view (kView);
// SSRSubsystem drives everything else.
class SSRTemporalPass : public RG::PassLifecycle
{
public:
    struct Camera
    {
        DirectX::XMFLOAT4X4 invViewProj;   // current inverse, jittered
        DirectX::XMFLOAT4X4 prevViewProj;  // previous-frame VP, jittered
        float               nearZ;
        float               farZ;
    };

    struct alignas(16) SSRTemporalCB
    {
        float    invViewProj[16];   // jittered (matches depth rasterization)
        float    prevViewProj[16];  // prev-frame jittered (matches history grid)
        uint32_t traceW;      uint32_t traceH;
        float    invTraceW;   float    invTraceH;
        uint32_t resetHistory;
        float    nearZ;       float    farZ;
        uint32_t renderW;     uint32_t renderH;
        uint32_t frameIndex;  uint32_t _pad[1];
    };

    void Init(IGraphicsDevice& gfx);
    void ReloadShaders(IGraphicsDevice& gfx) override;
    uint32_t HandledEvents() const override { return kView; }
    // Creates the view's history, empty; EnsureTextures makes its textures (BeginFrame's
    // OnResize).
    void BindView(ViewState& view) override;
    void EnsureTextures(uint32_t renderW, uint32_t renderH);

    void SetCamera(const Camera& cam) { m_cam = cam; }
    void SetFrameIndex(uint32_t f)    { m_frameIndex = f; }
    // The render frame this Execute belongs to (FrameView::currentFrame): the chain runs
    // only on some frames (FullFold / FullAdditive), and a history that skipped frames
    // restarts.
    void SetRenderFrame(uint64_t f)   { m_renderFrame = f; }
    void MarkReset()                  { if (m_hist) m_hist->reset = true; }

    // After Execute these refer to the buffer written THIS frame (the ping-pong advances
    // at the top of Execute): the upsample consumes them in the same frame, so they must
    // never hand back the history buffer.
    uint64_t GetColorSrv()    const;
    uint64_t GetVarianceSrv() const;
    const RHI::Texture* GetColorTexture()    const;
    const RHI::Texture* GetVarianceTexture() const;

    void Execute(RHI::CommandList cl,
                 uint32_t traceW, uint32_t traceH,
                 uint64_t colorCurrentSrv,
                 uint64_t varianceCurrentSrv,
                 uint64_t reprojDepthSrv,
                 uint64_t velocitySrv,
                 uint64_t depthSrv);

    uint32_t GetRenderWidth()  const { return m_renderW; }
    uint32_t GetRenderHeight() const { return m_renderH; }

private:
    struct History final : ViewHistory
    {
        RHI::Texture color[2];        // RGBA16F ping-pong
        RHI::Texture variance[2];     // RG16F ping-pong (.x variance, .y accum count)
        RHI::Texture depthHistory[2]; // R32F ping-pong (reverse-Z NDC)
        uint32_t     writeIdx = 0;
        bool         reset    = true;
        // The render frame the history last accumulated (SetRenderFrame);
        // none yet = ~0.
        uint64_t     lastFrame = ~0ull;

        const char* Name() const override { return "SSR"; }
        // Resets on the content's cuts (camera cut, world clear), a render-path change and a
        // new size (EnsureTextures notices it). A shader reload resets it too, and so does a
        // frame the chain skipped (Execute: lastFrame).
        uint32_t ResetsOn() const override
        { return HistoryReset::Cuts | HistoryReset::Resize | HistoryReset::PathChange; }
        void Invalidate(uint32_t reasons) override
        { if (reasons & (HistoryReset::Cuts | HistoryReset::PathChange)) reset = true; }
        void Release(IGraphicsDevice& gfx) override;
    };

    IGraphicsDevice*   m_gfx = nullptr;
    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_pso;
    FrameCB<SSRTemporalCB> m_cb;

    // The view's SSR history (RenderGraph/ViewState.h): one per view, created empty by
    // BindView; EnsureTextures makes its textures. The size latch stays in the pass.
    History* m_hist = nullptr;   // the bound view's (BindView)

    uint32_t m_w = 0, m_h = 0;
    uint32_t m_renderW = 0, m_renderH = 0;
    uint32_t m_frameIndex = 0;
    uint64_t m_renderFrame = 0;   // SetRenderFrame
    Camera   m_cam{};
};
