#pragma once

// Trail passes, parallel to ParticlePasses.
//
// TrailUpdatePass: compute, run standalone (manual Execute in the Renderer's render
// phases).
// TrailRenderPass: graphics, in the render graph after ParticleRenderPass.

#include "RenderGraph/PassLifecycle.h"
#include "RenderGraph/RenderGraph.h"
#include "RenderGraph/ViewState.h"   // ViewData (a secondary view's scene snapshot)
#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/PSOCache.h"
#include "Graphics/FrameCB.h"
#include "Graphics/FogApply.h"
#include <DirectXMath.h>

class TrailSystem;

class TrailUpdatePass : public RG::PassLifecycle
{
public:
    void Init(IGraphicsDevice& gfx);
    void Shutdown(IGraphicsDevice& gfx) override { (void)gfx; }
    // Hot reload (standalone: Renderer::ReloadShaders calls it).
    void ReloadShaders(IGraphicsDevice& gfx) override;
    uint32_t HandledEvents() const override { return kReloadShaders; }   // events Renderer::PassSet delivers

    void SetSystem(TrailSystem* sys) { m_sys = sys; }

    void Execute(RHI::CommandList cl);

private:
    bool CreatePipelines(IGraphicsDevice& gfx);

    IGraphicsDevice*   m_gfx = nullptr;
    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_pso;
    TrailSystem*       m_sys = nullptr;
};

class TrailRenderPass : public RG::RenderPass
{
public:
    explicit TrailRenderPass(RG::RGTextureHandle depth);

    const char* GetName() const override { return "TrailRenderPass"; }
    void Setup  (RG::RenderGraphBuilder& b) override;
    void Init   (IGraphicsDevice& gfx)     override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    // Mirrors Execute's three early-outs (no system / no CB, no active slot,
    // no draw buckets).
    bool IsActive() const override;   // defined in the .cpp (system type is fwd-declared)
    // Main thread (called by SetFrameData): allocates the HDR scene snapshot custom trail
    // PSs sample at t6, only when a custom-shader bucket draws this frame.
    void PrepareSceneSnapshot();

    void SetSystem(TrailSystem* sys) { m_sys = sys; }

    // It records any view: the Renderer hands it the view's camera (SetFrameData) in that view's segment, and
    // the scene snapshot is the bound view's.
    uint32_t HandledEvents() const override { return kView | kViewSafe; }
    void BindView(ViewState& view) override;
    // The opaque depth snapshot (SSRTraceDepth) the soft fade reads through
    // FogApplyCB's heap index, declared so the graph knows. Set before the graph
    // compiles.
    void SetOpaqueDepthSnapshot(RG::RGTextureHandle h) { m_opaqueDepth = h; }

    // TrailSystem registers and reflects custom trail shaders against this pass's
    // library, so the PSO compile and the param reflection share one cache (the ids are
    // DynamicShaderRegistry's, the same in every library).
    ShaderLibrary& GetShaderLibrary() { return m_shaderLib; }

    // Hot-reload opt-in: without these, ShaderLibrary::m_failedShaders and
    // PSOCache::m_failedPSOs are never cleared, so a custom trail shader that failed
    // once stays dead until relaunch. ClearCaches() keeps DynamicShaderRegistry, so
    // bucket ids survive; the next frame recompiles (packCustomParams returns
    // ready=false for that frame, as on the first frame).
    ShaderLibrary*         GetReloadableShaderLibrary() override { return &m_shaderLib; }
    std::vector<PSOCache*> GetReloadablePSOCaches()     override { return { &m_psoCache }; }

    // Per-frame camera state, set by the Renderer before the graph executes. timeSec
    // is engine global time, passed to the shaders as a b0 root constant (FX shaders
    // scroll UVs with it).
    void SetFrameData(const DirectX::XMFLOAT4X4& viewProj,
                      const DirectX::XMFLOAT3&   camForward,
                      float                      timeSec)
    {
        m_viewProj   = viewProj;
        m_camForward = camForward;
        m_timeSec    = timeSec;
        PrepareSceneSnapshot();
    }

private:
    // Must mirror the TrailRenderCB cbuffer in Trail.vs.hlsl. maxPoints / maxSubdiv
    // are the frame's maxima (>= 1) feeding the VS instance decode; Execute() derives
    // the DrawInstanced count from the same values - a desync scrambles which instance
    // maps to which quad.
    struct alignas(16) RenderCB
    {
        float    viewProj[16];  // 64
        float    camForward[3]; // 12
        uint32_t maxSegments;   // 4
        uint32_t maxPoints;     // 4
        uint32_t maxSubdiv;     // 4
        uint32_t _pad0;         // 4
        uint32_t _pad1;         // 4
    };
    static_assert(sizeof(RenderCB) == 96, "RenderCB drift vs Trail.vs.hlsl TrailRenderCB");

    // Ribbon PSO keyed by (custom psID, TAA stencil variant). Blend is premultiplied
    // (ONE, INV_SRC_ALPHA) in every permutation: the PS outputs float4(rgb*a, a) for
    // alpha and float4(rgb*a, 0) for additive, so the blend axis never splits the PSO.
    // Custom trail shaders must honour the same contract (TrailFX.hlsli TrailOutput()).
    PSODesc BuildTrailPSODesc(uint32_t customPSID, bool taaExclude) const;

    RG::RGTextureHandle m_depth;
    RG::RGTextureHandle m_opaqueDepth;   // soft fade (SetOpaqueDepthSnapshot)

    IGraphicsDevice*    m_gfx = nullptr;
    ShaderLibrary       m_shaderLib;
    PSOCache            m_psoCache;

    FrameCB<RenderCB>   m_renderCB;
    float               m_timeSec = 0.f;

    DirectX::XMFLOAT4X4 m_viewProj{};
    DirectX::XMFLOAT3   m_camForward{ 0, 0, 1 };

    TrailSystem*        m_sys = nullptr;

    // One view's HDR scene snapshot (custom trail PSs, t6).
    struct SceneSnapshot
    {
        RHI::Texture       tex;
        uint32_t           width = 0, height = 0;
        RHI::ResourceState state = RHI::ResourceState::COPY_DST;
        bool               ready = false;
    };
    struct ViewSnapshot : ViewData
    {
        SceneSnapshot snapshot;
        void Release(IGraphicsDevice& gfx) override;
    };
    // The main view's; m_scene is the bound view's (BindView).
    SceneSnapshot  m_mainScene;
    SceneSnapshot* m_scene = &m_mainScene;
};
