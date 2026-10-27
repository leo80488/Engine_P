#pragma once

// TracerSimPass - runs the tracer emit + age compute shaders.
// TracerRenderPass - draws the tracer pool as instanced cylindrical billboards.
// Mirrors the ParticleSystem / ParticlePasses / TrailSystem split.
//
// Order in the Renderer's render phases:
//   1. TracerSimPass (compute, on its own BeginCommandList, like ParticleSimPass /
//      SkinningPass).
//   2. TracerRenderPass (graphics, in the render graph after TransparentPass /
//      ParticleRenderPass / TrailRenderPass).

#include "RenderGraph/PassLifecycle.h"
#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"
#include "Graphics/FogApply.h"

#include <DirectXMath.h>

class TracerSystem;

// ==== TracerSimPass: compute only, not in the render graph ====
class TracerSimPass : public RG::PassLifecycle
{
public:
    void Init(IGraphicsDevice& gfx);
    void Shutdown(IGraphicsDevice& gfx) override { (void)gfx; }
    // Hot reload (standalone: Renderer::ReloadShaders calls it).
    void ReloadShaders(IGraphicsDevice& gfx) override;
    uint32_t HandledEvents() const override { return kReloadShaders; }   // events Renderer::PassSet delivers

    void SetSystem(TracerSystem* sys) { m_sys = sys; }

    void Execute(RHI::CommandList cl);

private:
    bool CreatePipelines(IGraphicsDevice& gfx);

    IGraphicsDevice*   m_gfx = nullptr;
    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_emitPSO;
    RHI::PipelineState m_updatePSO;

    TracerSystem*      m_sys = nullptr;
};

// ==== TracerRenderPass ====
// Graphics, in the render graph after TransparentPass. Reads scene depth as an SRV
// (no DSV) so the PS can do the soft-particle fade.
class TracerRenderPass : public RG::RenderPass
{
public:
    explicit TracerRenderPass(RG::RGTextureHandle depth);

    const char* GetName() const override { return "TracerRenderPass"; }
    void Setup  (RG::RenderGraphBuilder& b) override;
    void Init   (IGraphicsDevice& gfx)      override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    // Mirrors Execute's two early-outs; valid because nothing before Execute's
    // HasLiveTracers() test has a side effect the frame depends on.
    bool IsActive() const override;   // defined in the .cpp (system type is fwd-declared)
    void ReloadShaders(IGraphicsDevice& gfx) override;

    void SetSystem(TracerSystem* sys) { m_sys = sys; }

    // Per-frame camera data, pushed by the Renderer: the camera position for the
    // cylindrical billboards, nearZ / farZ to linearise depth for the soft-particle fade.
    // It records any view: the Renderer hands it the view's camera (SetFrameData) in that view's segment.
    uint32_t HandledEvents() const override { return kViewSafe; }

    void SetFrameData(const DirectX::XMFLOAT4X4& viewProj,
                      const DirectX::XMFLOAT3&   cameraPos,
                      float                      time,
                      float                      nearZ,
                      float                      farZ)
    {
        m_viewProj  = viewProj;
        m_cameraPos = cameraPos;
        m_time      = time;
        m_nearZ     = nearZ;
        m_farZ      = farZ;
    }

    // Visual tuning, driven by the editor.
    void SetTuning(float fadeRange, float coreSharpness,
                   float noiseTiling, float scrollSpeed, float noiseFloor)
    {
        m_fadeRange     = fadeRange;
        m_coreSharpness = coreSharpness;
        m_noiseTiling   = noiseTiling;
        m_scrollSpeed   = scrollSpeed;
        m_noiseFloor    = noiseFloor;
    }

private:
    // Render CB (b2 space0); FrameCB places each copy 256-byte aligned for the root CBV.
    struct alignas(16) RenderCB
    {
        float    viewProj[16];   // 64 - column-major (CPU transposes from row-vector)
        float    cameraPos[3];   // 12
        float    time;           // 4
        float    nearZ;          // 4
        float    farZ;           // 4
        float    fadeRange;      // 4
        float    coreSharpness;  // 4
        float    noiseTiling;    // 4
        float    scrollSpeed;    // 4
        float    noiseFloor;     // 4
        float    _pad;           // 4
    };

    bool CreatePipelines(IGraphicsDevice& gfx);

    DirectX::XMFLOAT4X4 m_viewProj{};
    DirectX::XMFLOAT3   m_cameraPos{};
    float               m_time           = 0.0f;
    float               m_nearZ          = 0.1f;
    float               m_farZ           = 1000.0f;
    float               m_fadeRange      = 0.3f;
    float               m_coreSharpness  = 2.5f;
    float               m_noiseTiling    = 6.0f;
    float               m_scrollSpeed    = 4.0f;
    float               m_noiseFloor     = 0.65f;

    RG::RGTextureHandle m_depth;

    IGraphicsDevice*   m_gfx = nullptr;
    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_pso;

    FrameCB<RenderCB> m_renderCB;

    TracerSystem*  m_sys = nullptr;
};
