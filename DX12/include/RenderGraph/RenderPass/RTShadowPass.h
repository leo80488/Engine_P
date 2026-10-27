#pragma once

// RTShadowPass: ray-traced SUN shadow mask (full screen, R8, 1 = lit), denoised by
// NVIDIA NRD SIGMA.
//
// A GRAPHICS-queue compute pass between RTReflectionPass and LightingPass:
//   Trace+Pack - one inline-RayQuery shadow ray per pixel toward the sun, jittered
//                inside the sun's angular cone; writes the SIGMA inputs directly
//                (penumbra, normalRough, viewZ, mv, translucency). Rays trace
//                instance-mask bit 1 (0x02) = "casts RT sun shadow" (skinned
//                characters included).
//   SIGMA      - NRD SIGMA_SHADOW_TRANSLUCENCY spatial + temporal-stabilization
//                denoise (Kind::SigmaShadowTranslucency).
//   Resolve    - SIGMA_BackEnd_UnpackShadow (x*x) -> .y -> the final mask.
//
// Translucent occluders: alpha-BLEND materials (glass, water planes, smoke cards)
// are flagged RT_ALPHA_FLAG_BLEND by DDGISceneAS and never committed by the
// candidate loop - it multiplies their opacity into a transmittance product and
// keeps traversing. That scalar rides IN_TRANSLUCENCY, so it gets the same
// spatial + temporal filter as the shadow. The resolve reads .y (translucent
// shadow), NOT .x (binary shadow, still 0 for a pixel occluded only by glass).
// Additive / Multiply never absorb light and are excluded upstream
// (DDGISceneAS's instanceMask lambda).
//
// Without NRD there are no RT shadows: no fallback denoiser.
//
// Lighting.ps opens the final mask from the heap (LightCB.rtShadowMaskIdx) and
// min()s it into the CSM result (shadow.hlsli, RT_SHADOW_MASK), gated on
// LightCB.rtShadowEnabled, which Renderer sets ONLY on frames this pass produced
// a fresh mask. TLAS availability is synchronised like RTR:
// SetExternalWait(GetName(), promoteCL) each active frame.
//
// Register maps: shaders/RTShadow.cs.hlsl + RTShadowResolve.cs.hlsl.

#include "RenderGraph/RenderGraph.h"
#include "RenderGraph/ViewState.h"
#include "Graphics/FrameCB.h"
#include "Graphics/GraphicsStruct.h"

#include <DirectXMath.h>
#include <memory>
#include <utility>
#include <vector>

class NrdDenoiser;

class RTShadowPass : public RG::RenderPass
{
public:
    struct ServiceRequest { bool wanted = false; };

    struct FrameData
    {
        uint64_t tlasVA = 0;                         // GPU VA; 0 = pass inert this frame
        DirectX::XMFLOAT4X4 viewProj{};              // current, jittered (worldPos reconstruction)
        DirectX::XMFLOAT4X4 viewMatrix{};            // NRD worldToView (non-jittered)
        DirectX::XMFLOAT4X4 projMatrixNoJitter{};    // NRD viewToClip (non-jittered)
        float    jitterX = 0.0f, jitterY = 0.0f;     // NRD sub-pixel jitter
        DirectX::XMFLOAT3   sunDirection {0.f, -1.f, 0.f}; // FROM-light (LightCB.lightDir)
        float    nearZ = 0.1f;
        float    farZ  = 1000.f;
        uint32_t frameIndex = 0;
        // ---- Alpha-cutout occluders (RTAlphaRecord table) ----
        // 0 = table not live this frame -> the shader accepts every candidate (every
        // occluder solid).
        uint64_t                  alphaRecordVA     = 0;
        // RTInstanceData table (128 B), same InstanceID() index space as the
        // alpha records. Needed ONLY so the cutout test can apply the material
        // UV transform; alphaTestEnabled requires both (and the two bindless tables).
        uint64_t                  instanceVA        = 0;
        uint32_t                  alphaInstanceCount = 0;
        uint64_t                  bindlessBufferTable = 0;  // t0.. space1 (VB/IB)
    };

    RTShadowPass(RG::RGTextureHandle normal,
                 RG::RGTextureHandle surface,
                 RG::RGTextureHandle depth,
                 RG::RGTextureHandle velocity);

    const char* GetName() const override { return "RTShadowPass"; }
    // The resolved mask is this pass's import (RG::PassTex::kRTShadowMask), created
    // at the frame boundary (EnsureResources -> OnCompile); the graph transitions it
    // (Graph tracking). Lighting and the forward passes declare reading it.
    void DeclareImports(RG::ImportDeclarer& d) override;
    void Setup(RG::RenderGraphBuilder& b) override;
    void Init(IGraphicsDevice& gfx) override;
    void OnCompile(IGraphicsDevice& gfx) override;

    // Enable-gated working set - same contract and rationale as
    // RTAOPass::EnsureResources: main-thread, driven by the settings flag, releases
    // ~45 MiB of textures + the SIGMA pool when off. Builds the two compute
    // pipelines the first time RT shadows are wanted.
    void EnsureResources(bool wanted);

    // A secondary view's working set at its render size, after the main view's request (its segment,
    // before the graph records). Its NRD pools are retired, not drained.
    void EnsureViewResources(uint32_t w, uint32_t h);

    // The service point: the working set follows the intent's switch; the retired NRD pools whose
    // frames have drained go.
    ServiceRequest& Request() { return m_request; }
    void OnService(RG::ServiceContext&) override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    void ReloadShaders(IGraphicsDevice& gfx) override;
    // The view's reset flag (RenderGraph/ViewState.h): the history itself lives in
    // the NRD (SIGMA) instance. It records any view: a secondary view's working set (its
    // textures and SIGMA instance) is sized in its segment (EnsureViewResources).
    uint32_t HandledEvents() const override { return kView | kViewSafe; }
    void BindView(ViewState& view) override;

    // ---- Per-frame inputs, set by Renderer BEFORE m_graph.Execute (the bound view's) ----
    void SetFrameData(const FrameData& fd) { m_ws->fd = fd; }

    // ---- Activation and readiness ----
    void SetActive(bool active)
    {
        if (active && !m_ws->active) RequestHistoryReset();
        m_ws->active = active;
    }
    // RenderGraph queries this in its serial Phase A and SKIPS the pass entirely
    // when false - it must stay a side-effect-free read of pre-graph state
    // (the bound view's flag, pushed by Renderer before m_graph.Execute).
    bool IsActive() const override { return m_ws->active; }
    // Ready requires DXR AND the bound view's live NRD SIGMA instance (no fallback denoiser).
    bool IsReady()  const { return m_ready && m_ws->ready; }

    // ---- Output ----
    // SRV of the bound view's final mask (single non-ping-pong texture) - stable handle.
    uint64_t GetUpcomingOutputSrv() const;
    // The final mask as this frame left it, and its state - the debug target
    // view / capture {"attachment": "rtshadow"}. Null when the pass is not
    // ready or did not run this frame. An imported mask is graph-tracked: its
    // state then comes from the device's book (QueryTextureState).
    const RHI::Texture* GetDebugOutput(RHI::ResourceState& state) const;

private:
    struct alignas(16) RTShadowConstants
    {
        float    invViewProj[16];
        float    sunDirection[3];   float sunTanHalfAngle;
        uint32_t screenW;           uint32_t screenH;
        float    invScreenW;        float    invScreenH;
        float    normalBias;        float    rayTMax;
        uint32_t frameIndex;        float    nearZ;
        // alignas(16) rounds sizeof up to 128 either way: the alpha fields fill what
        // would otherwise be padding.
        float    farZ;              uint32_t alphaTestEnabled;
        uint32_t alphaInstanceCount; float   alphaMipLevel;
    };
    static_assert(sizeof(RTShadowConstants) == 128,
                  "must mirror the cbuffer in rt_shadow_common.hlsli");

    struct History final : ViewHistory
    {
        bool reset = true;   // the next denoise discards NRD's history

        const char* Name() const override { return "RTShadow"; }
        // The content's cuts - a hard cut, the camera's, a world clear (the view tells
        // it at the render frame's start) - and a new size (OnCompile notices it).
        // Switching the pass on and a shader reload reset it too - the pass's policy,
        // not a reason the view's history breaks.
        uint32_t ResetsOn() const override { return HistoryReset::Cuts | HistoryReset::Resize; }
        void Invalidate(uint32_t reasons) override { if (reasons & HistoryReset::Cuts) reset = true; }
        void Release(IGraphicsDevice&) override {}
    };

    // One view's working set: the SIGMA inputs and output, the final mask, its NRD instance, the size
    // latch, this frame's camera data and its run state.
    struct WorkingSet
    {
        RHI::Texture       penumbra;             // R16F - IN_PENUMBRA
        RHI::ResourceState penumbraState = RHI::ResourceState::SHADER_RESOURCE;
        RHI::Texture       normalRough;          // R10G10B10A2 - IN_NORMAL_ROUGHNESS
        RHI::ResourceState normalRoughState = RHI::ResourceState::SHADER_RESOURCE;
        RHI::Texture       viewZ;                // R32F - IN_VIEWZ
        RHI::ResourceState viewZState = RHI::ResourceState::SHADER_RESOURCE;
        RHI::Texture       mv;                   // RG16F - IN_MV
        RHI::ResourceState mvState = RHI::ResourceState::SHADER_RESOURCE;
        // RGBA8 - IN_TRANSLUCENCY. .x = lit bit, .yzw = grey transmittance
        // accumulated across alpha-BLEND occluders by the trace's candidate loop.
        RHI::Texture       translucency;
        RHI::ResourceState translucencyState = RHI::ResourceState::SHADER_RESOURCE;
        // RGBA8 - OUT_SHADOW_TRANSLUCENCY (also history). MUST be 4-channel:
        // SIGMA_Config.hlsli makes SIGMA_TYPE float4 under TRANSLUCENCY==1 and
        // Sigma_ShadowTranslucency.hpp sizes its transient pool RGBA8_UNORM, so an
        // R8 UAV here would silently discard .yzw on every store.
        RHI::Texture       shadowOut;
        RHI::ResourceState shadowOutState = RHI::ResourceState::SHADER_RESOURCE;
        // Resolved mask (LightCB.rtShadowMaskIdx) - single non-ping-pong, stable SRV.
        RHI::Texture       final;
        RHI::ResourceState finalState = RHI::ResourceState::SHADER_RESOURCE;
        std::unique_ptr<NrdDenoiser> nrd;   // SIGMA_SHADOW_TRANSLUCENCY
        uint32_t  w = 0, h = 0;
        FrameData fd{};
        bool      ready  = false;   // the textures and a live NRD instance at w x h
        bool      active = false;
    };
    // A secondary view's working set, given back when the view goes (its NRD instance retired).
    struct ViewWork : ViewData
    {
        WorkingSet    ws;
        RTShadowPass* owner = nullptr;
        void Release(IGraphicsDevice& gfx) override;
    };

    bool BuildRootSignature(IGraphicsDevice& gfx);
    bool CreatePipelines(IGraphicsDevice& gfx);
    bool BuildPipelines(const char* why);   // first use - see RTAOPass::BuildPipelines
    // The bound view's working set at @p w x @p h. @p mayStall: the frame boundary's (the main view's),
    // which resizes its NRD in place; a view's segment retires its old instance instead.
    void BuildWorkingSet(IGraphicsDevice& gfx, uint32_t w, uint32_t h, bool mayStall);
    void DestroyResolutionResources(IGraphicsDevice& gfx);
    void RetireNrd();
    // The import's source: the bound view's final mask (the graph reads it at each Execute).
    void SyncImports() { m_finalImport = m_ws->final; }

    void RequestHistoryReset() { if (m_hist) m_hist->reset = true; }

public:
    // Tunables. Sun angular radius default ~0.27 deg (real sun); stored as
    // tan(radius in radians). Larger = softer/wider penumbra.
    float    sunTanHalfAngle  = 0.0047f;
    // 150 m: a non-opaque shadow ray pays a bindless fetch + texture sample per
    // reported candidate (the alpha-cutout loop), and rayTMax is the only real leash
    // on that cost - the editor profiler's auto-disable guard needs ~3 s of
    // sustained overrun while Windows TDR fires at 2 s on a single dispatch. Raise
    // only after measuring RTShadowPass GPU ms on a foliage-heavy scene.
    float    rayTMax          = 150.0f;
    float    normalBias       = 0.03f;
    // Explicit mip for the cutout alpha sample - an inline RayQuery has no ray
    // differentials, so the mip MUST be constant. 0 = the authored silhouette;
    // raise to 1-2 to trade cutout crispness for texture-cache misses.
    float    alphaMipLevel    = 0.0f;

private:
    ServiceRequest m_request;   // OnService's

    RG::RGTextureHandle m_normalH, m_surfaceH, m_depthH, m_velocityH;
    IGraphicsDevice*    m_gfx = nullptr;
    // See RTAOPass::m_resourcesWanted - same boot/resize-Compile gate.
    bool                m_resourcesWanted = false;

    RHI::PipelineLayout m_layout;                  // BuildRootSignature
    uint64_t m_bindlessTexTable = 0;               // t0 space2, cached at Init
    RHI::PipelineState m_tracePSO;
    RHI::PipelineState m_resolvePSO;
    bool m_pipelinesFailed = false;   // BuildPipelines failed; cleared by ReloadShaders

    FrameCB<RTShadowConstants> m_cb;

    // ---- The working sets: the main view's here, a secondary view's in its ViewWork ----
    WorkingSet  m_main;
    WorkingSet* m_ws = &m_main;   // the bound view's (BindView)
    RHI::Texture        m_finalImport;   // the import's source: the bound view's final mask (SyncImports)
    RG::RGTextureHandle m_finalH;        // its graph import (DeclareImports)
    // NRD tears its pool down at once: an instance a view drops waits out the frames in flight that may
    // still read it (OnService counts the frame boundaries down).
    std::vector<std::pair<std::unique_ptr<NrdDenoiser>, uint32_t>> m_retiredNrd;
    bool m_ready = false;   // DXR, the root signature and the CB (Init); the pipelines did not fail

    // The view's RTShadow history state (RenderGraph/ViewState.h): NRD keeps the
    // temporal history; the engine's side is whether the next denoise must discard
    // it. Created by BindView.
    History* m_hist = nullptr;   // the bound view's (BindView); none = every denoise resets
};
