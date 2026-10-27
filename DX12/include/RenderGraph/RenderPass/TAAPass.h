#pragma once

// TAAPass - temporal anti-aliasing resolve (compute).
//
// Reads the current jittered HDR scene, depth, velocity, the GBuffer and the
// history; writes the resolved HDR colour (UAV, ping-pong).
// Per frame: SetFrameData() and the SRV setters before Execute(); Execute()
// dispatches the resolve; the post chain reads GetResolvedSrvHandle().

#include "RenderGraph/RenderGraph.h"
#include "RenderGraph/PassTexture.h"
#include "RenderGraph/ViewState.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>

class TAAPass : public RG::RenderPass
{
public:
    const char* GetName() const override { return "TAAPass"; }
    void Setup(RG::RenderGraphBuilder&) override {}
    void Init(IGraphicsDevice& gfx)     override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    // Hot reload (standalone: Renderer::ReloadShaders calls it).
    void ReloadShaders(IGraphicsDevice& gfx) override;

    // ---- Per-frame inputs (call before Execute) ----
    void SetHdrSrvHandle(uint64_t h)          { m_hdrSrvHandle      = h; }
    void SetDepthSrvHandle(uint64_t h)        { m_depthSrvHandle    = h; }
    void SetGBufferSrvHandle(uint64_t h)      { m_gbufferSrvHandle  = h; }
    void SetVelocitySrvHandle(uint64_t h)     { m_velocitySrvHandle = h; }
    // Stencil-plane SRV of the depth buffer (X32_TYPELESS_G8X24_UINT: the GBuffer depth
    // is D32_FLOAT_S8X24_UINT). Optional: 0 skips the outline-aware history weakening.
    // Set per frame by the Renderer from GetTextureStencilSRVGpuHandle(depthTex).
    void SetOutlineStencilSrvHandle(uint64_t h){ m_outlineStencilSrvHandle = h; }
    void SetJitter(float jx, float jy)        { m_cbData.jitterX = jx; m_cbData.jitterY = jy; }
    void SetViewportSize(uint32_t w, uint32_t h) { m_vpW = w; m_vpH = h; }

    // Resize-time allocation. Renderer calls this on the main thread before
    // the compute worker starts recording; Execute keeps a fallback check.
    void PrepareResources(uint32_t w, uint32_t h);

    // Releases the history working set (~51 MiB at 1080p: 2x RGBA16F + 2x R32F + 2x R8)
    // while another path owns the temporal resolve: an upscaler replaces this pass, and
    // an AA mode without TAA never runs it. The next PrepareResources rebuilds it;
    // history restarts either way. OnPrepare calls this when the AA config cannot run
    // TAA. No-op when already released.
    void ReleaseResources();

    // Uploads per-frame matrices and parameters (row-vector convention: transposed).
    // @p invVPNoJitter: transpose(inverse(current unjittered viewProj)); feeds TAA.cs's
    //   synthetic sky / cloud motion vectors (jitter-free, matching GBuffer velocity).
    // @p prevVP: transpose(unjittered previous viewProj).
    // @p historyWeight: direct blend weight in [0, 0.99]; alpha_diffuse =
    //   1 - historyWeight (rationale in the TAA_Common.hlsli cbuffer comment).
    // @p hasHistory: false on the first frame / after a resize.
    // @p deltaTime: elapsed seconds this frame (kept in the CB for non-alpha uses).
    void SetFrameData(const DirectX::XMFLOAT4X4& invVPNoJitter,
                      const DirectX::XMFLOAT4X4& prevVP,
                      float historyWeight,
                      bool  hasHistory,
                      float deltaTime);

    // ---- Output ----
    // GPU SRV handle of the buffer the last Execute() wrote - THIS frame's
    // resolve once Execute ran (not the ping-pong's next write target, which
    // holds last frame's). The post chain reads it (the Resolve stage's claim,
    // FXAA in FXAA+TAA) instead of the raw HDR handle.
    uint64_t GetResolvedSrvHandle() const;

    // ---- Resize handling ----
    // Execute also detects a missed resize as a safety fallback.
    void InvalidateHistory() { if (m_hist) m_hist->hasHistory = false; }
    // Events from Renderer::PassSet; kView: a hard cut and a resize are the view's; kViewSafe: its history
    // is the view's, so it resolves any view.
    uint32_t HandledEvents() const override { return kReloadShaders | kView | kViewSafe; }
    // The view's history: created here, empty; RebuildBuffers (PrepareResources)
    // makes its buffers.
    void BindView(ViewState& view) override;
    // RG::PrepareStage::PostAA: the history set (~51 MiB at 1080p) while the AA config
    // can run TAA - not IsEnabled(), which wireframe / debug views flip; an upscaler
    // replaces the resolve.
    void OnPrepare(const RG::PrepareContext& c) override
    {
        if (c.taa) PrepareResources(c.renderW, c.renderH);
        else       ReleaseResources();
        // The history's flip: after a frame that resolved, before the post chain records
        // (Execute, GetResolvedSrvHandle).
        if (m_hist) m_hist->tex.BeginFrame();
    }

    // ---- Enable / disable ----
    // When disabled, Execute() returns immediately and downstream passes read the raw
    // HDR scene (BuildRenderScene skips the camera jitter too, or there would be wobble
    // without reconstruction). Turning it back on invalidates history.
    bool IsEnabled() const { return m_enabled; }
    void SetEnabled(bool v)
    {
        if (v == m_enabled) return;
        m_enabled = v;
        if (v) InvalidateHistory();
    }
    // This frame TAA resolves for an upscaler whose context is down
    // (FramePlan::Decisions::taaStandIn), so Execute runs although the AA mode leaves
    // the pass disabled. Set every frame the post chain's TAA effect runs, on the render
    // half.
    void SetStandIn(bool v)
    {
        if (v && !m_standIn && !m_enabled) InvalidateHistory();
        m_standIn = v;
    }

private:
    // CPU-side CB mirror (must match TAACB in TAA_Common.hlsli exactly).
    struct alignas(16) TAACB
    {
        float    invViewProjNoJitter[16];   // inverse(current unjittered VP)
        float    prevViewProj[16];
        uint32_t width;
        uint32_t height;
        float    historyWeight;             // direct diffuse-history blend weight [0, 0.99]
        float    hasHistory;
        float    deltaTime;                 // elapsed seconds this frame
        float    jitterX;                   // subpixel jitter X in pixels [-0.5, +0.5)
        float    jitterY;                   // subpixel jitter Y in pixels [-0.5, +0.5)
        float    colorBoxSigma;             // base AABB gamma
        float    colorBoxSigmaSpecular;     // specular AABB gamma
        float    specularRoughnessMax;      // specular roughness threshold
        uint32_t antiFlicker;               // 0/1: Falcor distance-to-clamp formula
        float    velocityWiden;             // motion-proportional AABB widening factor
        float    sharpenStrength;           // Karis unsharp blend factor
        float    outlineMinAlpha;           // alpha floor for outline-stencil-tagged pixels
        uint32_t outlineStencilBit;         // 0 disables the lookup; otherwise the bit mask to test
        float    particleVfxMinAlpha;       // alpha floor for particle-VFX stencil-tagged pixels
        uint32_t particleVfxBit;            // 0 disables; otherwise the particle stencil bit (0x40)
        float    motionVelocityStartPx;
        float    motionVelocityEndPx;
        float    motionAlphaFloor;
        float    motionRawBlend;
        uint32_t outlineStencilDebug;      // see TAA_Common.hlsli
        float    outlineVacatedAlpha;      // see TAA_Common.hlsli
        uint32_t hasOutlineHist;           // 0 when the outline ping-pong is not ready
    };
    // Content is 224 bytes; alignas(16) keeps sizeof at 224.
    static_assert(sizeof(TAACB) == 224, "TAACB must match TAA_Common.hlsli");

    struct History final : ViewHistory
    {
        // The ping-pongs (RG::HistoryTexture): three planes, one flip at the frame's start
        // (OnPrepare), after a frame that resolved. Cur is what this frame's Execute writes,
        // Prev what the last one wrote, read as the history. Their states are the device's
        // book's (RG::TransitionTo).
        //   plane 0: the resolved colour (R16G16B16A16_FLOAT); Cur is this frame's resolve
        //            once Execute ran (GetResolvedSrvHandle).
        //   plane 1: the previous frame's depth (R32_FLOAT), written at the end of the
        //            dispatch and read next frame by the depth disocclusion detector.
        //   plane 2: the outline-coverage history (R8_UNORM): whether the texel was part of
        //            the outline rim, read back at prevUV. The signal the depth buffer
        //            cannot give: the inverted hull writes no depth, so a texel the outline
        //            just vacated shows no depth change and the dark rim in history would
        //            blend into the background (the outline-shaped trail). R8_UNORM so the
        //            lookup can be filtered (slight dilation is desirable).
        RG::HistoryTexture  tex{
            RG::PassTextureDesc{ .format = RHI::Format::R16G16B16A16_FLOAT, .debugName = "TAA.Color" },
            RG::PassTextureDesc{ .format = RHI::Format::R32_FLOAT,          .debugName = "TAA.PrevDepth" },
            RG::PassTextureDesc{ .format = RHI::Format::R8_UNORM,           .debugName = "TAA.OutlineHistory" } };
        bool                hasHistory = false;

        // The size the buffers were built at (made or not); 0 after a release,
        // so the next PrepareResources rebuilds.
        uint32_t            width  = 0;
        uint32_t            height = 0;

        const char* Name() const override { return "TAA"; }
        // Every reason: the content's cuts (a camera cut, a world clear), a resize (a new
        // size also rebuilds it) and a render-path change (the view tells it). The AA
        // config's own switches restart it too (SetEnabled, ReleaseResources): the pass's
        // policy, not a reason the view's history breaks.
        uint32_t ResetsOn() const override
        { return HistoryReset::Cuts | HistoryReset::Resize | HistoryReset::PathChange; }
        void Invalidate(uint32_t) override { hasHistory = false; }
        void Release(IGraphicsDevice& gfx) override;
    };

    void RebuildBuffers();
    bool CreatePipelines(IGraphicsDevice& gfx);

public:
    // Runtime tunables, read by the Renderer when it calls SetFrameData; public so the
    // editor panel can adjust them without setters.
    float historyWeight         = 0.8f;   // diffuse-history blend weight [0, 0.99]: alpha_diffuse = 1 - w, alpha_spec = alpha_diffuse x 0.125. 0.8 (~5-frame integration, mild LDR ghost without per-scene tuning), 0.5 fast response (foliage / curtains), 0.95 cinematic; see TAA_Common.hlsli
    float colorBoxSigma         = 1.5f;   // base AABB gamma, HDR-linear. 1.5 trades slightly more ghost for less distant shimmer (Falcor uses 1.0; 1.2-1.3 suits most scenes). TAA_USE_TONEMAP_BLEND rescales it x1.5 internally so the value stays linear (kTonemapSigmaScale in TAA.cs.hlsl)
    float colorBoxSigmaSpecular = 2.0f;   // AABB gamma for specular pixels: a wider box keeps specular history alive
    float specularRoughnessMax  = 0.5f;   // roughness threshold for "specular" classifier
    bool  antiFlicker           = false;  // Falcor-style distance-to-clamp anti-flicker (Karis 2014)
    float velocityWiden         = 0.3f;   // motion-proportional AABB widening. 1.0 doubles the AABB in motion, so the variance clip misses HDR sub-pixel fireflies (camera-motion shimmer); 0.3 leaves room for jitter-displaced highlights without flooding history with aliases
    float sharpenStrength       = 0.1f;   // Karis 5-tap unsharp blend factor (0 disables, 0.1 default; 0.2+ ringy)
    // Outline-aware history weakening: forces alpha to at least this on every pixel the
    // rim covers this frame. Default 0 (off): the rim is a thin line jittering +-0.5 px
    // per frame, so denying it history keeps the jitter from averaging and the outline
    // shimmers. The outline trail is handled at its source by outlineVacatedAlpha,
    // which rejects history only on the texels the rim left. Raise only for a
    // deliberately hard, aliased outline.
    float outlineMinAlpha       = 0.0f;

    // Diagnostic: paints outline-stencil-tagged pixels magenta so the tagging can be
    // seen. Every outline mitigation in TAA.cs.hlsl is gated on that stencil bit; if the
    // ring does not appear, they are all inert and the ghosted outline comes from a path
    // TAA cannot identify.
    bool     outlineStencilDebug = false;

    // Alpha floor for a texel the outline occupied last frame but not now (the band the
    // rim just vacated), the cause of outline-shaped trails behind a moving character:
    //   * the rim writes no depth, so depth disocclusion never fires there;
    //   * the rim writes no velocity, so the texel reprojects onto itself and pulls the
    //     dark rim straight out of history;
    //   * the 3x3 variance clamp cannot reject it, because the rim's new position, a
    //     texel or two away, drags the box minimum down to the outline colour.
    // This floor is the disocclusion signal the depth buffer lacks. 1.0 = discard
    // history on the vacated band; 0 disables. Requires OutlinePass's stencil tag
    // (OutlinePass::tagStencilForTAA).
    float    outlineVacatedAlpha = 1.0f;

    // Particle-VFX history weakening: the outline mechanism with a higher floor.
    // Additive / unlit particles (sparks, electric, fire) have no velocity and should
    // stay sharp, so alpha is pushed near 1 on their stencil-tagged pixels: 0.92 is
    // mostly current frame with a sliver of stabilization; 1.0 excludes them fully.
    // Per-emitter opt-in via excludeFromTAA.
    float particleVfxMinAlpha   = 0.92f;

    // Responsive motion path. Geometry velocity is jitter-free, so a low
    // threshold safely catches distant sub-pixel skeletal motion.
    float motionVelocityStartPx = 0.05f;
    float motionVelocityEndPx   = 1.0f;
    float motionAlphaFloor      = 0.35f;
    float motionRawBlend        = 0.70f;

private:
    IGraphicsDevice*    m_gfx       = nullptr;
    RHI::PipelineState  m_pso;
    ShaderLibrary       m_shaderLib;

    // The view's TAA history (RenderGraph/ViewState.h): one per view, created empty by
    // BindView; RebuildBuffers makes its buffers (PrepareResources on the main thread,
    // or Execute's fallback check).
    History* m_hist = nullptr;   // the bound view's (BindView)

    // 1x1 zero-velocity fallback bound to the velocity slot (t4 space2, root 8) when no
    // GBuffer velocity SRV is available. TAA.cs reads gVelocity unconditionally, so the
    // slot must never be unbound (GBV "uninitialized root argument"). Zero velocity =
    // no reprojection.
    RHI::Texture        m_zeroVelocityTex;
    uint64_t            m_zeroVelocitySrv = 0;

    // Persistently mapped constant buffer.
    FrameCB<TAACB> m_cb;

    // Per-frame data set by Renderer.
    uint64_t m_hdrSrvHandle      = 0;
    uint64_t m_depthSrvHandle    = 0;
    uint64_t m_gbufferSrvHandle  = 0;
    uint64_t m_velocitySrvHandle = 0;
    uint64_t m_outlineStencilSrvHandle = 0;
    uint32_t m_vpW = 0;
    uint32_t m_vpH = 0;

    TAACB    m_cbData{};
    bool     m_enabled = true;
    bool     m_standIn = false;   // SetStandIn
};
