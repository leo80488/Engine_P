#pragma once

// LensFlarePass - procedural directional-light lens flare.
//
// Reads the hardware depth (sun visibility / occlusion) and writes an additive HDR
// contribution into a half-resolution RGBA16F texture, which ToneMapPass adds to
// the HDR scene before tonemapping, so the flare is ACES-compressed with the rest
// of the frame. The renderer supplies the sun's screen position (SetSun),
// projected on the CPU with the camera's view-proj.

#include "RenderGraph/RenderGraph.h"
#include "RenderGraph/ViewState.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"

#include <DirectXMath.h>

class LensFlarePass : public RG::RenderPass
{
public:
    struct ServiceRequest { uint32_t w = 0, h = 0; };

    const char* GetName() const override { return "LensFlarePass"; }
    void Setup  (RG::RenderGraphBuilder&) override {}
    void Init   (IGraphicsDevice& gfx)   override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    // Hot reload (standalone: Renderer::ReloadShaders calls it).
    void ReloadShaders(IGraphicsDevice& gfx) override;
    // PostEffects prepare stage (RG::PrepareStage): the working set at the post
    // chain's size.
    void OnPrepare(const RG::PrepareContext& c) override { PrepareResources(c.postW, c.postH); }

    // Also the service point: as a group-D resident of the shared alias heap it is
    // prepared before any frame commands record, at the plan's post size.
    ServiceRequest& Request() { return m_request; }
    void OnService(RG::ServiceContext&) override { PrepareResources(m_request.w, m_request.h); }
    // Lifecycle events (Renderer::PassSet); kView binds the view's occlusion history and target, and it
    // records any view (a secondary view's target is prepared in its segment, at its size).
    uint32_t HandledEvents() const override { return kReloadShaders | kView | kViewSafe; }
    // The view's occlusion history: created here, empty - the first
    // PrepareResources makes its texture.
    void BindView(ViewState& view) override;

    // ---- Per-frame inputs (call before Execute) ----
    void SetEnabled       (bool v)       { m_enabled = v; }
    bool IsEnabled        () const       { return m_enabled; }

    void SetDepthSrvHandle(uint64_t h)   { m_depthSrv = h; }
    void SetDepthSourceSize(uint32_t w, uint32_t h) { m_depthW = w; m_depthH = h; }
    void SetViewportSize  (uint32_t w, uint32_t h)  { m_t->vpW = w; m_t->vpH = h; }
    void PrepareResources (uint32_t w, uint32_t h);

    // Quarter-res cloud raymarch SRV (a = view-ray transmittance) - clouds do not
    // write depth, so this is the only way the flare sees overcast. 0 = clouds did not
    // render this frame; occlusion falls back to depth only.
    void SetCloudSrvHandle(uint64_t h)   { m_cloudSrv = h; }

    void SetSun(const DirectX::XMFLOAT2& sunUV, bool sunBehind,
                const DirectX::XMFLOAT3& sunColor)
    {
        m_sunUV = sunUV; m_sunBehind = sunBehind; m_sunColor = sunColor;
    }

    // ---- Tunables (exposed so editor / volume system can drive them) ----
    void  SetIntensity      (float v) { m_intensity = v; }
    float GetIntensity      () const  { return m_intensity; }
    void  SetGhostCount     (uint32_t v) { m_ghostCount = v; }
    uint32_t GetGhostCount  () const  { return m_ghostCount; }
    void  SetGhostDispersal (float v) { m_ghostDispersal = v; }
    float GetGhostDispersal () const  { return m_ghostDispersal; }
    void  SetHaloWidth      (float v) { m_haloWidth = v; }
    float GetHaloWidth      () const  { return m_haloWidth; }
    void  SetStreakLength   (float v) { m_streakLength = v; }
    float GetStreakLength   () const  { return m_streakLength; }
    void  SetChromaticOffset(float v) { m_chromaticOffset = v; }
    float GetChromaticOffset() const  { return m_chromaticOffset; }

    // ---- Photographic-flare knobs ----
    // Defaults target a real-lens look: a hexagonal iris, a restrained streak and a
    // heavy occlusion filter (the sun crossing a branch must not strobe).
    void  SetApertureBlades  (float v) { m_apertureBlades = v; }
    float GetApertureBlades  () const  { return m_apertureBlades; }
    void  SetApertureRotation(float v) { m_apertureRotation = v; }
    float GetApertureRotation() const  { return m_apertureRotation; }
    // Exponential moving average on the sun-occlusion term, 0..0.95. 0 = instant
    // (visibly steppy: the depth test is binary per tap); 0.8 ~ a 5-frame tail, which
    // stops the flare strobing through foliage. Raise it if the sun sweeps fast, lower
    // it if the flare lags.
    void  SetOcclusionSmooth (float v) { m_occlusionSmooth = v; }
    float GetOcclusionSmooth () const  { return m_occlusionSmooth; }
    void  SetStreakStrength  (float v) { m_streakStrength = v; }
    float GetStreakStrength  () const  { return m_streakStrength; }
    void  SetGhostBrightness (float v) { m_ghostBrightness = v; }
    float GetGhostBrightness () const  { return m_ghostBrightness; }
    // Veiling glare - the wide, faint Lorentzian tail around the sun, the term that
    // reads as light scattering inside the lens. Keep it low: past ~1.5 it stops being
    // glare and fogs the whole frame.
    void  SetGlareStrength   (float v) { m_glareStrength = v; }
    float GetGlareStrength   () const  { return m_glareStrength; }

    // SRV GPU handle of the half-res RGBA16F flare texture; 0 unless Execute wrote it
    // this frame (see m_wroteThisFrame), and ToneMapPass adds no flare for 0. Disabled,
    // or with the sun behind the camera, Execute still dispatches and writes black.
    uint64_t GetSrvHandle() const;
    const RHI::Texture* GetTexture() const { return &m_t->texture; }

private:
    struct alignas(16) LensFlareCB
    {
        uint32_t dstWidth;
        uint32_t dstHeight;
        uint32_t srcDepthWidth;
        uint32_t srcDepthHeight;

        uint32_t enabled;
        float    intensity;
        float    sunBehind;
        float    chromaticOffset;

        float    sunUV[2];
        float    haloWidth;
        float    streakLength;

        float    sunColor[3];
        float    ghostDispersal;

        uint32_t ghostCount;
        float    streakWidth;
        float    occlusionRadius;
        uint32_t cloudValid;

        float    apertureBlades;   // iris blades -> ghost polygon sides (<3 = round)
        float    apertureRotation; // radians, keeps the polygon off-axis
        float    occlusionSmooth;  // per-frame EMA factor for the occlusion term
        uint32_t historyValid;     // 0 on the first frame / after a resize

        float    streakStrength;
        float    ghostBrightness;
        float    glareStrength;
        float    pad0;
    };

    struct History final : ViewHistory
    {
        // 1x1 R32_FLOAT, persistent across frames - the EMA state of the sun occlusion
        // term. Viewport-independent, so it is created once.
        RHI::Texture occlusion;
        bool         valid = false;   // reset on resize / first dispatch

        const char* Name() const override { return "LensFlare"; }
        // The content's cuts (a hard cut, the camera's, a world clear - the view
        // reports them) and a new post size (PrepareResources notices it).
        uint32_t ResetsOn() const override { return HistoryReset::Cuts | HistoryReset::Resize; }
        void Invalidate(uint32_t reasons) override { if (reasons & HistoryReset::Cuts) valid = false; }
        void Release(IGraphicsDevice& gfx) override;
    };

    bool CreatePipelines(IGraphicsDevice& gfx);

    void RebuildTexture();
    void DestroyTexture();
    void EnsureHistory();

    ServiceRequest m_request;   // OnService's size
    // True only for a frame in which Execute actually dispatched. GetSrvHandle
    // is assigned into ctx.resources.lensFlare unconditionally by the adapter, so
    // without this a declined frame publishes last frame's (or, once placed,
    // another group-D member's) pixels to ToneMap.
    bool m_wroteThisFrame = false;

    RHI::PipelineState m_pso;
    ShaderLibrary      m_shaderLib;

    // One view's half-res target and its size stamps (the viewport size the post chain set, the size it
    // was built at). m_t is the bound view's (BindView).
    struct Target
    {
        RHI::Texture       texture;          // half-res RGBA16F
        RHI::ResourceState textureState{};
        uint32_t           lastVpW = 0;
        uint32_t           lastVpH = 0;
        uint32_t           texW    = 0;
        uint32_t           texH    = 0;
        uint32_t           vpW     = 0;
        uint32_t           vpH     = 0;
    };
    struct ViewTarget : ViewData
    {
        Target t;
        void Release(IGraphicsDevice& gfx) override;
    };
    Target  m_mainTarget;
    Target* m_t = &m_mainTarget;

    FrameCB<LensFlareCB> m_cb;

    // Per-frame inputs
    bool                m_enabled    = true;
    uint64_t            m_depthSrv   = 0;
    uint64_t            m_cloudSrv   = 0;
    uint32_t            m_depthW     = 0;
    uint32_t            m_depthH     = 0;
    DirectX::XMFLOAT2   m_sunUV      { 0.5f, 0.5f };
    bool                m_sunBehind  = true;
    DirectX::XMFLOAT3   m_sunColor   { 1.0f, 0.95f, 0.85f };

    // Tunables
    float    m_intensity       = 0.2f;
    uint32_t m_ghostCount      = 6;
    float    m_ghostDispersal  = 0.18f;
    float    m_haloWidth       = 0.32f;
    float    m_streakLength    = 0.55f;
    // Dispersion is a 5-tap spectral integral, not a 3-tap RGB offset, so this value
    // reads as a wide, soft rainbow.
    float    m_chromaticOffset = 0.020f;
    float    m_apertureBlades   = 6.0f;    // hexagonal iris - the common case
    float    m_apertureRotation = 0.35f;   // off-axis, so it never looks stamped
    float    m_occlusionSmooth  = 0.80f;
    float    m_streakStrength   = 0.35f;   // photographic restraint
    float    m_ghostBrightness  = 0.45f;
    float    m_glareStrength    = 1.0f;

    // The view's occlusion history (RenderGraph/ViewState.h): one per view, created
    // empty by BindView; EnsureHistory makes its texture (the first PrepareResources,
    // main thread).
    History* m_hist = nullptr;   // the bound view's (BindView)

    IGraphicsDevice* m_gfx = nullptr;
};
