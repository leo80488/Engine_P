#pragma once

// RTAOPass - ray-traced ambient occlusion denoised by NVIDIA NRD REBLUR_DIFFUSE_OCCLUSION. A
// graphics-queue compute pass between DecalPass and LightingPass (sibling of RTShadowPass)
// that replaces XeGTAO's screen-space AO when active:
//   Trace+Pack - cosine-hemisphere RayQuery over the scene TLAS (mask 0x01, includes skinned
//                characters); writes the REBLUR occlusion inputs (diffHitDist, normalRough,
//                viewZ, mv).
//   REBLUR     - NRD spatial + temporal denoise.
//   Resolve    - OUT_DIFF_HITDIST (x aoPower) -> the AO texture.
//
// Lighting.ps samples the resolved AO (1 = unoccluded) at gSSAO (t18): when active the Renderer
// points LightingPass::SetSSAOHandle at GetUpcomingOutputSrv() and skips the XeGTAO dispatch,
// so Lighting.ps is unchanged. Same-frame, without XeGTAO's one-frame latency. TLAS
// availability is synchronised like RTR: SetExternalWait(GetName(), promoteCL).
//
// Register maps: shaders/RTAO.cs.hlsl + RTAOResolve.cs.hlsl.

#include "RenderGraph/RenderGraph.h"
#include "RenderGraph/ViewState.h"
#include "Graphics/FrameCB.h"
#include "Graphics/GraphicsStruct.h"

#include <DirectXMath.h>
#include <memory>
#include <utility>
#include <vector>

class NrdDenoiser;

class RTAOPass : public RG::RenderPass
{
public:
    struct ServiceRequest { bool wanted = false; };

    struct FrameData
    {
        uint64_t tlasVA = 0;                         // GPU VA; 0 = pass inert this frame
        DirectX::XMFLOAT4X4 viewProj{};              // current, jittered (worldPos reconstruction)
        DirectX::XMFLOAT4X4 viewMatrix{};            // NRD worldToView (non-jittered)
        DirectX::XMFLOAT4X4 projMatrixNoJitter{};    // NRD viewToClip (non-jittered)
        float    jitterX = 0.0f, jitterY = 0.0f;
        float    nearZ = 0.1f;
        float    farZ  = 1000.f;
        uint32_t frameIndex = 0;
        // ---- Alpha-cutout occluders (RTAlphaRecord table) ----
        // 0 = table not live this frame -> accept every candidate (every occluder is solid).
        uint64_t                  alphaRecordVA      = 0;
        // RTInstanceData table (128 B), same InstanceID() index space as the
        // alpha records. Needed ONLY so the cutout test can apply the material
        // UV transform; alphaTestEnabled requires BOTH to be non-zero.
        uint64_t                  instanceVA         = 0;
        uint32_t                  alphaInstanceCount = 0;
        uint64_t                  bindlessBufferTable = 0;  // t0.. space1 (VB/IB)
    };

    RTAOPass(RG::RGTextureHandle normal,
             RG::RGTextureHandle depth,
             RG::RGTextureHandle velocity);

    const char* GetName() const override { return "RTAOPass"; }
    // The resolved AO is this pass's import (RG::PassTex::kRTAO): created by EnsureResources at
    // the frame boundary and transitioned by the graph (ImportTracking::Graph). Lighting and the
    // forward passes declare reading it.
    void DeclareImports(RG::ImportDeclarer& d) override;
    void Setup(RG::RenderGraphBuilder& b) override;
    void Init(IGraphicsDevice& gfx) override;
    void OnCompile(IGraphicsDevice& gfx) override;

    // Enable-gated working set. Called every frame on the main thread before any worker kicks
    // (OnService), with the user's intent (the settings flag), not m_active: m_active needs the
    // output SRV to exist, so gating creation on it would deadlock. wanted=true creates lazily
    // (idempotent), the two compute pipelines included the first time - not at Init, where a run
    // that never enables RTAO would pay ~20 ms of boot; kept once built. wanted=false drains the
    // GPU once (toggles are rare; NrdDenoiser::Resize's convention) and releases ~30 MiB of
    // full-res textures plus the whole NRD pool.
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
    // The view's reset flag (RenderGraph/ViewState.h); the history itself lives in the NRD
    // (REBLUR) instance. It records any view: a secondary view's working set is sized in its
    // segment (EnsureViewResources).
    uint32_t HandledEvents() const override { return kView | kViewSafe; }
    void BindView(ViewState& view) override;

    void SetFrameData(const FrameData& fd) { m_ws->fd = fd; }
    void SetActive(bool active)
    {
        if (active && !m_ws->active) RequestHistoryReset();
        m_ws->active = active;
    }
    // RenderGraph queries this in its serial Phase A and skips the pass when false - keep it a
    // side-effect-free read of pre-graph state (the Renderer sets the bound view's flag before
    // m_graph.Execute).
    bool IsActive() const override { return m_ws->active; }
    // The pass and the bound view's working set (its NRD instance up).
    bool IsReady()  const { return m_ready && m_ws->ready; }

    // SRV of the resolved AO (one texture, not ping-ponged) - a stable handle for
    // LightingPass::SetSSAOHandle.
    uint64_t GetUpcomingOutputSrv() const;
    // The resolved AO as this frame left it, and its state - for the debug target view and
    // capture {"attachment": "rtao"} (Renderer/DebugTargets.h). Null when the pass is not ready or
    // did not run this frame. The graph transitions an imported AO, so the state then comes from
    // the device's tracker.
    const RHI::Texture* GetDebugOutput(RHI::ResourceState& state) const;

private:
    struct alignas(16) RTAOConstants
    {
        float    invViewProj[16];
        uint32_t screenW;      uint32_t screenH;
        float    invScreenW;   float    invScreenH;
        float    aoRadius;     float    normalBias;
        uint32_t rayCount;     uint32_t frameIndex;
        float    nearZ;        float    farZ;
        // aoPower and the three alpha-cutout words end the struct; alignas(16) pads it to 128 B.
        float    aoPower;           uint32_t alphaTestEnabled;
        uint32_t alphaInstanceCount; float   alphaMipLevel;
    };
    static_assert(sizeof(RTAOConstants) == 128,
                  "must mirror the cbuffer in RTAO.cs.hlsl / RTAOResolve.cs.hlsl");

    struct History final : ViewHistory
    {
        bool reset = true;   // the next denoise discards NRD's history

        const char* Name() const override { return "RTAO"; }
        // The content's cuts (a hard cut, the camera's, a world clear; the view reports them at the
        // render frame's start) and a new size (OnCompile notices it). Switching the pass on and a
        // shader reload reset it too - the pass's policy, not a break in the view's history.
        uint32_t ResetsOn() const override { return HistoryReset::Cuts | HistoryReset::Resize; }
        void Invalidate(uint32_t reasons) override { if (reasons & HistoryReset::Cuts) reset = true; }
        void Release(IGraphicsDevice&) override {}
    };

    // One view's working set: the REBLUR inputs and output, the resolved AO, its NRD instance, the size
    // latch, this frame's camera data and its run state.
    struct WorkingSet
    {
        RHI::Texture       diffHitIn;            // R8 - IN_DIFF_HITDIST
        RHI::ResourceState diffHitInState = RHI::ResourceState::SHADER_RESOURCE;
        RHI::Texture       normalRough;          // R10G10B10A2 - IN_NORMAL_ROUGHNESS
        RHI::ResourceState normalRoughState = RHI::ResourceState::SHADER_RESOURCE;
        RHI::Texture       viewZ;                // R32F - IN_VIEWZ
        RHI::ResourceState viewZState = RHI::ResourceState::SHADER_RESOURCE;
        RHI::Texture       mv;                   // RG16F - IN_MV
        RHI::ResourceState mvState = RHI::ResourceState::SHADER_RESOURCE;
        RHI::Texture       diffHitOut;           // R8 - OUT_DIFF_HITDIST
        RHI::ResourceState diffHitOutState = RHI::ResourceState::SHADER_RESOURCE;
        // Resolved AO (the gSSAO texture) - one texture, not ping-ponged, stable SRV.
        RHI::Texture       aoFinal;
        RHI::ResourceState aoFinalState = RHI::ResourceState::SHADER_RESOURCE;
        std::unique_ptr<NrdDenoiser> nrd;   // REBLUR_DIFFUSE_OCCLUSION
        uint32_t  w = 0, h = 0;
        FrameData fd{};
        bool      ready  = false;   // the textures and a live NRD instance at w x h
        bool      active = false;
    };
    // A secondary view's working set, given back when the view goes (its NRD instance retired).
    struct ViewWork : ViewData
    {
        WorkingSet ws;
        RTAOPass*  owner = nullptr;
        void Release(IGraphicsDevice& gfx) override;
    };

    bool BuildRootSignature(IGraphicsDevice& gfx);
    bool CreatePipelines(IGraphicsDevice& gfx);
    // CreatePipelines on first use (or a shader reload in use: @p why), timed
    // and logged; a failure disables the pass until the next ReloadShaders
    // instead of retrying every frame.
    bool BuildPipelines(const char* why);
    // The bound view's working set at @p w x @p h. @p mayStall: the frame boundary's (the main view's),
    // which resizes its NRD in place; a view's segment retires its old instance instead.
    void BuildWorkingSet(IGraphicsDevice& gfx, uint32_t w, uint32_t h, bool mayStall);
    void DestroyResolutionResources(IGraphicsDevice& gfx);
    void RetireNrd();
    // The import's source: the bound view's resolved AO (the graph reads it at each Execute).
    void SyncImports() { m_aoFinalImport = m_ws->aoFinal; }

    void RequestHistoryReset() { if (m_hist) m_hist->reset = true; }

public:
    // Tunables (editor UI). aoRadius in world meters.
    float    aoRadius   = 1.0f;
    float    normalBias = 0.02f;
    float    aoPower    = 1.5f;
    uint32_t rayCount   = 2;
    // Explicit mip for the alpha-cutout sample: an inline RayQuery has no ray differentials. AO
    // rays are short (aoRadius, 1 m), so mip 0 is affordable.
    float    alphaMipLevel = 0.0f;

private:
    ServiceRequest m_request;   // set by the Renderer, read by OnService

    RG::RGTextureHandle m_normalH, m_depthH, m_velocityH;
    IGraphicsDevice*    m_gfx = nullptr;
    // Latched by EnsureResources; OnCompile refuses to allocate while false so
    // a boot/resize graph Compile cannot resurrect the working set of a
    // feature that is switched off.
    bool                m_resourcesWanted = false;

    RHI::PipelineLayout m_layout;                  // its own compute root layout (BuildRootSignature)
    uint64_t m_bindlessTexTable = 0;               // t0 space2, cached at Init
    RHI::PipelineState m_tracePSO;
    RHI::PipelineState m_resolvePSO;
    bool m_pipelinesFailed = false;   // BuildPipelines failed; cleared by ReloadShaders

    FrameCB<RTAOConstants> m_cb;

    // ---- The working sets: the main view's here, a secondary view's in its ViewWork ----
    WorkingSet  m_main;
    WorkingSet* m_ws = &m_main;   // the bound view's (BindView)
    RHI::Texture        m_aoFinalImport;   // the import's source: the bound view's AO (SyncImports)
    RG::RGTextureHandle m_aoFinalH;        // its graph import
    // NRD tears its pool down at once: an instance a view drops waits out the frames in flight that may
    // still read it (OnService counts the frame boundaries down).
    std::vector<std::pair<std::unique_ptr<NrdDenoiser>, uint32_t>> m_retiredNrd;
    bool m_ready = false;   // DXR, the root signature and the CB (Init); the pipelines did not fail

    // The view's RTAO history state (RenderGraph/ViewState.h): NRD keeps the temporal history;
    // the engine's side is whether the next denoise must discard it. Created by BindView.
    History* m_hist = nullptr;   // the bound view's (BindView); none = every denoise resets
};
