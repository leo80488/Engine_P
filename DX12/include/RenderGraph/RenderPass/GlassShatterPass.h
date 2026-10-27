#pragma once

// GlassShatterPass - GPU-driven glass-shatter post-effect.
//
// Owned by GlassShatterFeature (Renderer/Features/GlassShatterFeature.cpp); its
// effect is PostTonemap 0 of the PostProcess::PostChain, on the chain's compute
// list after Tonemap and before TransitionForDisplay, compositing into the Tonemap
// output UAV in place. Renderer::TriggerGlassShatter reaches it through IGlassShatter.
//
// Init builds one Voronoi cell pattern on the CPU (Sutherland-Hodgman half-plane
// clipping), uploads the centroid + edge tables to immutable buffers and compiles
// the Init / Simulate / Composite compute shaders. Trigger(u, v) sets the impact UV
// and a pending flag; the next Execute copies the Tonemap output to the shatter
// source and runs InitCS to seed per-shard physics. Each active frame then runs
// SimulateCS and CompositeCS (reads the shatter source, shard buffer and edges;
// writes the Tonemap output in place). Deactivates once TimeSinceTrigger >=
// Duration; Trigger() while active restarts with a new impact.

#include "RenderGraph/PassLifecycle.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"
#include <DirectXMath.h>
#include <cstdint>

class IGraphicsDevice;

class GlassShatterPass : public RG::PassLifecycle
{
public:
    void Init(IGraphicsDevice& gfx);
    // Hot reload (Renderer::ReloadShaders calls it).
    void ReloadShaders(IGraphicsDevice& gfx) override;
    uint32_t HandledEvents() const override { return kReloadShaders; }   // lifecycle events (Renderer::PassSet)

    // Set the CB params (knobs from EditorLayer / gameplay code).
    void SetImpactStrength(float v)   { m_impactStrength = v; }
    void SetGravityNDC(float v)       { m_gravityNDC = v; }
    void SetAirDrag(float v)          { m_airDrag = v; }
    void SetAngularDamping(float v)   { m_angularDamping = v; }
    void SetCrackWidth(float v)       { m_crackWidth = v; }
    void SetCrackColor(float r, float g, float b);
    void SetDuration(float v)         { m_duration = v; }
    // Seconds the shatter holds in place after Trigger() before shards start
    // moving. During hold the frozen scene is shown with crack lines visible
    // but every shard pinned at translation=0/angle=0. Useful for an impact
    // freeze ("hit-stop") before the breakup.
    void SetHoldDuration(float v)     { m_holdDuration = v; }

    // CPU trigger. Lazily captures the live Tonemap output as the frozen
    // shatter source on the next Execute(). Re-triggering while active
    // restarts everything cleanly.
    void Trigger(float impactU, float impactV);

    bool IsActive() const { return m_active; }

    // Run on a compute command list, AFTER ToneMap finished and BEFORE
    // TransitionForDisplay. `tonemapOutput` is composited in-place when
    // active. `tonemapEntryExitState` is the texture's state on entry; the
    // pass restores that exact state on exit so external state-tracking
    // (ToneMapPass::m_finalOutputState) stays correct.
    void Execute(RHI::CommandList cl,
                 const RHI::Texture* tonemapOutput,
                 RHI::ResourceState tonemapEntryExitState,
                 uint32_t vpW, uint32_t vpH,
                 float deltaTime);

    // Format of `tonemapOutput`. Push ToneMapPass::GetFinalOutputFormat() before
    // Execute - see m_targetFormat for why a literal will not do.
    void SetTargetFormat(RHI::Format f) { m_targetFormat = f; }

private:
    static constexpr uint32_t kTargetShards = 80;

    // CPU-side mirrors of HLSL structs. Layout MUST match shader-side.
    struct ShardMeta
    {
        DirectX::XMFLOAT2 centroidUV;
        uint32_t          edgeOffset;
        uint32_t          edgeCount;
    };
    struct ShardEdge
    {
        DirectX::XMFLOAT2 normal;
        float             d;
        float             _pad;
    };
    struct ShardGPU
    {
        DirectX::XMFLOAT2 centroidUV;
        DirectX::XMFLOAT2 translation;
        float             angle2D;
        float             angularVel2D;
        DirectX::XMFLOAT2 linearVelocity;
        float             spawnDelay;
        float             alpha;
        float             seed;
        uint32_t          edgeOffset;
        uint32_t          edgeCount;
        float             _pad0;
        float             _pad1;
        float             _pad2;
    };
    struct alignas(16) ShatterCB
    {
        float    TimeSinceTrigger;
        float    DeltaTime;
        float    Duration;
        uint32_t ShardCount;

        DirectX::XMFLOAT2 ImpactPointUV;
        float    ImpactRadialStrength;
        float    GravityNDC;

        float    AirDrag;
        float    AngularDamping;
        float    CrackWidth;
        float    HoldDuration;

        DirectX::XMFLOAT3 CrackColor;
        float    _pad0;

        uint32_t Width;
        uint32_t Height;
        uint32_t _pad1;
        uint32_t _pad2;
    };

    void GenerateVoronoiMesh();
    void EnsureSourceTexture(uint32_t w, uint32_t h);
    void UploadCB(uint32_t w, uint32_t h);
    bool CreatePipelines(IGraphicsDevice& gfx);

    IGraphicsDevice* m_gfx = nullptr;
    ShaderLibrary    m_shaderLib;

    RHI::PipelineState m_initPSO;
    RHI::PipelineState m_simulatePSO;
    RHI::PipelineState m_compositePSO;

    RHI::GPUBuffer m_metaBuffer;     // SR<ShardMeta>, immutable post-Init
    RHI::GPUBuffer m_edgeBuffer;     // SR<ShardEdge>, immutable post-Init
    RHI::GPUBuffer m_shardBuffer;    // RW<ShardGPU>, GPU-managed lifecycle

    RHI::Texture       m_shatterSource;
    RHI::ResourceState m_shatterSourceState = RHI::ResourceState::COPY_DST;
    uint32_t           m_sourceW = 0, m_sourceH = 0;

    // Format of the composition buffer this pass freezes a copy of. It follows the HDR
    // output mode - RGBA8_UNORM in SDR, FP16 in HDR10 / scRGB (HDRCompositionFormat,
    // Graphics/HDROutput.h) - and no PSO catches a mismatch: the capture is a bare
    // CopyTextureSubresource, which answers a src/dst format mismatch with a
    // debug-layer error or device removal. The Renderer pushes the live format via
    // SetTargetFormat before Execute.
    RHI::Format        m_targetFormat = RHI::Format::R8G8B8A8_UNORM;
    // What EnsureSourceTexture last CREATED, so a mode change is detected like a
    // resize: a dimensions-only dirty test would keep a stale RGBA8 copy when HDR
    // toggles at a fixed window size (the hazard ToneMapPass::EnsureTexture guards).
    RHI::Format        m_sourceFormat = RHI::Format::UNKNOWN;

    FrameCB<ShatterCB> m_paramsCB;

    uint32_t m_shardCount = 0;

    // Runtime state.
    bool              m_active            = false;
    bool              m_pendingInit       = false;
    float             m_timeSinceTrigger  = 0.0f;
    DirectX::XMFLOAT2 m_impactUV          { 0.5f, 0.5f };

    // CB knobs.
    float             m_impactStrength    = 0.6f;
    float             m_gravityNDC        = -1.6f;
    float             m_airDrag           = 0.3f;
    float             m_angularDamping    = 0.4f;
    float             m_crackWidth        = 0.0025f;
    DirectX::XMFLOAT3 m_crackColor        { 0.85f, 0.95f, 1.0f };
    float             m_duration          = 3.0f;
    float             m_holdDuration      = 0.6f;  // hit-stop before breakup
};
