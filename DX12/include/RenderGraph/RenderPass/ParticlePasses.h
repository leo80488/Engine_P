#pragma once

// ParticleSimPass (the emit + update compute shaders) and ParticleRenderPass (the pool drawn
// as billboards): one header, one ParticleSystem pointer and lifecycle.
// Order in Renderer::Render: ParticleSimPass records into its own command list on the
// graphics queue before the render graph (its trailing UAV barrier publishes the pool);
// ParticleRenderPass runs in the graph after TransparentPass, compositing over the scene.

#include "RenderGraph/PassLifecycle.h"
#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/PSOCache.h"
#include "Graphics/FrameCB.h"
#include "Graphics/FogApply.h"

#include <string>

class ParticleSystem;

// Compute only, outside the RenderGraph: Renderer::Render records it into its own command
// list (as SkinningPass). ParticleEmitCS once per active emitter (ceil(spawn / 64) groups),
// the boid passes when a flock is active, then ParticleUpdateCS over the live pool range.
class ParticleSimPass : public RG::PassLifecycle
{
public:
    void Init(IGraphicsDevice& gfx);
    void Shutdown(IGraphicsDevice& gfx) override { (void)gfx; }
    // Hot reload (standalone: Renderer::ReloadShaders calls it).
    void ReloadShaders(IGraphicsDevice& gfx) override;
    uint32_t HandledEvents() const override { return kReloadShaders; }   // lifecycle events (Renderer::PassSet)

    void SetSystem(ParticleSystem* sys) { m_sys = sys; }

    // Sources for mesh-shape emitter sampling, read by the emit shader through compute root
    // slots 10 (MeshDescriptors SRV) and 11 (bindless buffer table). Renderer sets them each
    // frame.
    void SetMeshDescriptorBinding(const RHI::GPUBuffer* meshDescBuf,
                                  uint64_t              bindlessTableHandle)
    {
        m_meshDescBuffer       = meshDescBuf;
        m_bindlessTableHandle  = bindlessTableHandle;
    }

    // Records the compute work into cl.
    void Execute(RHI::CommandList cl);

private:
    bool CreatePipelines(IGraphicsDevice& gfx);

    IGraphicsDevice*   m_gfx = nullptr;
    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_emitPSO;
    RHI::PipelineState m_updatePSO;
    // Boids: snapshot copies a stable {pos,vel,valid} view of the pool; a uniform
    // grid (clear + atomic build) accelerates neighbour queries; steer runs the
    // flocking rules. Dispatched between emit and update only when the
    // ParticleSystem reports an active flock.
    RHI::PipelineState m_boidSnapshotPSO;
    RHI::PipelineState m_boidGridClearPSO;
    RHI::PipelineState m_boidGridBuildPSO;
    RHI::PipelineState m_boidSteerPSO;

    ParticleSystem*    m_sys = nullptr;

    // For mesh-shape emitter sampling (optional; null = the mesh shape spawns nothing, other
    // shapes still work).
    const RHI::GPUBuffer* m_meshDescBuffer      = nullptr;
    uint64_t              m_bindlessTableHandle = 0;
};

// Graphics: draws the pool as billboards inside the RenderGraph, after TransparentPass.
class ParticleRenderPass : public RG::RenderPass
{
public:
    explicit ParticleRenderPass(RG::RGTextureHandle depth);

    const char* GetName() const override { return "ParticleRenderPass"; }
    void Setup  (RG::RenderGraphBuilder& b) override;
    void Init   (IGraphicsDevice& gfx)     override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    // Mirrors Execute's two early-outs: no system / no CB, and the lazily
    // resident pool (which is also how the release frame reads, every bucket's
    // drawCount already zeroed).
    bool IsActive() const override;   // defined in the .cpp (system type is fwd-declared)

    void SetSystem(ParticleSystem* sys) { m_sys = sys; }
    // The opaque depth snapshot (SSRTraceDepth) the soft fade reads through FogApplyCB's heap
    // index - declared so the graph knows. Set before the graph compiles.
    void SetOpaqueDepthSnapshot(RG::RGTextureHandle h) { m_opaqueDepth = h; }
    // It records any view: the Renderer hands it the view's camera (SetFrameData) in that view's segment.
    uint32_t HandledEvents() const override { return kViewSafe; }

    // ParticleSystem registers and reflects material-driven custom shaders against this
    // library, so param reflection and the PSO compile share one cache (as
    // TrailRenderPass::GetShaderLibrary). The ids are DynamicShaderRegistry's, the same in every
    // library.
    ShaderLibrary& GetShaderLibrary() { return m_shaderLib; }

    // Hot reload: every PSO comes from m_psoCache at draw time, so the default
    // ReloadShaders (clear both) is the whole job.
    ShaderLibrary*         GetReloadableShaderLibrary() override { return &m_shaderLib; }
    std::vector<PSOCache*> GetReloadablePSOCaches()     override { return { &m_psoCache }; }

    // Per-frame camera state, set by Renderer::Render before the graph executes. viewProj: the
    // jittered VP (SV_POSITION); camRight / camUp: world-space billboard axes; particleSize: a
    // global size multiplier; time: scene time in seconds for VS animation hooks.
    void SetFrameData(const DirectX::XMFLOAT4X4& viewProj,
                      const DirectX::XMFLOAT3&   camRight,
                      const DirectX::XMFLOAT3&   camUp,
                      float                      particleSize = 1.0f,
                      float                      time         = 0.0f)
    {
        m_viewProj     = viewProj;
        m_camRight     = camRight;
        m_camUp        = camUp;
        m_particleSize = particleSize;
        m_time         = time;
    }

    // Forward-lighting handles for LIT buckets (as TransparentPass::SetIBL / SetSkySH);
    // Renderer pushes them each frame from the same IBL sources.
    void SetIBL(uint64_t irradiance)  { m_iblIrradianceHandle = irradiance; }
    void SetSkySH(uint64_t skySH)     { m_skySHHandle = skySH; }
    // CSM cascade array (t9 space0). Bound only for buckets that set both lit and
    // receiveShadow: any other bucket's PS never declares t9.
    void SetShadowMap(uint64_t srv)   { m_shadowSrvHandle = srv; }
    // The DEBUG_SHADE permutation on this frame's bucket PSOs (see GBufferPass::SetDebugShade).
    void SetDebugShade(bool on)       { m_debugShade = on; }

private:
    // Per-frame constants, root CBV b3 space0. Layout must match ParticleRenderCB in
    // particle_vertex.hlsli.
    struct alignas(16) RenderCB
    {
        float    viewProj[16];
        float    camRight[3];
        float    particleSize;
        float    camUp[3];
        float    gTime;
    };

    struct BucketDynIds
    {
        uint32_t producerVs = ~0u;   // ParticleBucket::dynVsId when last resolved
        uint32_t producerPs = ~0u;
        uint32_t vsID = 0, psID = 0; // ids in THIS pass's library (0 = default)
    };

    // Builds the particle PSO descriptor: vsID / psID from the resolved custom ids (0 -> the
    // engine's Particle_VS / PS), the blend state from blendMode, a TAA-exclude stencil write
    // when taaExclude is set, the PARTICLE_LIT permutation when lit, and PARTICLE_SHADOW when a
    // lit bucket also receives the sun shadow (ParticleBucket::receiveShadow).
    PSODesc BuildParticlePSODesc(uint32_t customVSID, uint32_t customPSID,
                                 uint32_t blendMode, bool taaExclude, bool lit,
                                 bool receiveShadow) const;

    // Per-frame data set by Renderer.
    bool                m_debugShade = false;   // SetDebugShade
    DirectX::XMFLOAT4X4 m_viewProj{};
    DirectX::XMFLOAT3   m_camRight{ 1, 0, 0 };
    DirectX::XMFLOAT3   m_camUp   { 0, 1, 0 };
    float               m_particleSize = 1.0f;
    float               m_time         = 0.0f;

    RG::RGTextureHandle m_depth;
    RG::RGTextureHandle m_opaqueDepth;   // soft fade (SetOpaqueDepthSnapshot)

    IGraphicsDevice*    m_gfx = nullptr;
    ShaderLibrary       m_shaderLib;
    PSOCache            m_psoCache;     // keyed by the desc: custom shaders, blend, permutations

    // Per-bucket dynamic-id cache. This pass registers custom shaders into its own library
    // (RegisterDynamic: a string key under the library's global mutex, on a graph worker). The
    // producer-side ids ParticleSystem resolved at bucket claim are integer change keys - same
    // (path, stage) -> same id - so a bucket re-registers only when its shaders change: every
    // other pass recording on the graph workers would pay for that mutex too.
    BucketDynIds m_bucketDynIds[8]; // = ParticleSystem::kMaxParticleBuckets (static_assert in .cpp)

    FrameCB<RenderCB>   m_renderCB;

    // Forward lighting (LIT buckets): IBL/SH SRV handles.
    uint64_t            m_iblIrradianceHandle = 0;
    uint64_t            m_skySHHandle         = 0;
    uint64_t            m_shadowSrvHandle     = 0;   // CSM array, t9 (PARTICLE_SHADOW only)
    // 1x1 fully-lit stand-in, as in the other shadow-receiving passes: a PARTICLE_SHADOW
    // bucket's PS declares t9 unconditionally, so on a frame without ShadowPass's cascade array
    // (the first frames after a world load, or directional shadows off) skipping the bind leaves
    // a sampled table unbound - UB that Tier 3 does not report. R32_FLOAT, arraySize > 1, to
    // match Texture2DArray<float>.
    RHI::Texture        m_shadowFallbackTex;
    uint64_t            m_shadowFallbackHandle = 0;

    ParticleSystem*     m_sys = nullptr;
};
