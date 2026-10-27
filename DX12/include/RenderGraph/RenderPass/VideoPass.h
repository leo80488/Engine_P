#pragma once

// VideoPass - composites a decoded video frame onto the HDR scene colour.
//
// Pipeline:
//   1. The Renderer picks the first VideoComponent in `Playing` / `Paused` state
//      with a valid currentDpbSlot and calls SetActiveFrame(component). Its frame
//      source already put the frame in the NV12 texture on the graphics queue (a
//      copy that waits on the decoder's fence, or an upload).
//   2. The pass binds the Y plane SRV -> t6 space0 and the UV plane SRV -> t7
//      space0 and writes RGB into HDR with the VideoComposite VS+PS,
//      alpha-blended over what earlier passes drew.
//
// Surface mode (SetRect):
//   * Fullscreen     - rectUV = (0,0,1,1), the default; replaces the scene image.
//   * Letterbox      - a sub-rect from source vs. screen aspect (the Renderer's
//                      helper preserves the aspect ratio by default).
//   * Custom overlay - any rectUV (in-game TV, debug preview, picture-in-picture).

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"

#include <vector>

struct VideoComponent;

class VideoPass : public RG::RenderPass
{
public:
    // HLSL cbuffer layout - keep aligned with VideoComposite.ps.hlsl b2 space0.
    struct alignas(16) VideoCompositeCB
    {
        float    rectUV[4];      // xy = min, zw = max
        float    alpha;
        uint32_t colorSpace;     // 0 = BT.709, 1 = BT.601
        float    _pad[2];
    };

    VideoPass();
    ~VideoPass();

    const char* GetName() const override { return "VideoPass"; }
    void Setup(RG::RenderGraphBuilder& b) override;
    void Init (IGraphicsDevice& gfx)      override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    // Mirrors Execute's early-outs.
    bool IsActive() const override;
    // The game screen's video: a view that shows the HUD shows it too, over its own picture (the rect
    // is normalised; the composite CB is a FrameCB).
    uint32_t HandledEvents() const override { return kViewSafe; }
    uint32_t RequiredShowFlags() const override { return static_cast<uint32_t>(RG::ShowFlag::Hud); }

    ShaderLibrary*         GetReloadableShaderLibrary() override { return &m_shaderLib; }
    std::vector<PSOCache*> GetReloadablePSOCaches()     override { return { &m_psoCache }; }

    // ---- Renderer-pushed per-frame state ----
    // Per-frame: the VideoComponent whose latest decoded frame should be
    // composited this tick. Pass nullptr to skip the pass (Execute becomes a
    // no-op).
    void SetActiveFrame(const VideoComponent* vc);

    // Sub-rectangle of HDR that receives the video, in normalised screen
    // space [0,1]. Default = fullscreen. Aspect-preserving letterbox is up to
    // the caller (Renderer typically computes it from source vs. screen ratio).
    void SetRect(float uMin, float vMin, float uMax, float vMax);

    // Alpha applied uniformly to the composited RGB (1.0 = opaque, default).
    void SetAlpha(float alpha) { m_alpha = alpha; }

    // 0 = BT.709 (HD content, default), 1 = BT.601 (SD content).
    void SetColorSpace(uint32_t cs) { m_colorSpace = cs; }

private:
    IGraphicsDevice* m_gfx = nullptr;
    ShaderLibrary    m_shaderLib;
    PSOCache         m_psoCache;

    // Whether Init created the composite PSO; Execute fetches it from PSOCache.
    bool m_psoCreated = false;

    FrameCB<VideoCompositeCB> m_cb;

    // Per-frame state (set by Renderer; not owned).
    const VideoComponent* m_activeFrame = nullptr;
    float    m_rectUV[4]  = { 0.f, 0.f, 1.f, 1.f };
    float    m_alpha      = 1.f;
    uint32_t m_colorSpace = 0;
};
