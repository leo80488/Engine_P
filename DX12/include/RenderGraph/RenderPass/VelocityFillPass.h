#pragma once

// VelocityFillPass - writes camera-only motion vectors into sky pixels.
//
// The velocity buffer (GBuffer3) is written only by geometry passes; SkyboxPass and
// the volumetric-cloud composite bind colour only, so sky pixels reach the upscaler
// with zero motion. TAA synthesises its own (guarded); FSR2 and DLSS do not, and
// ghost the sky and clouds under camera rotation. This pass fills the shared buffer
// once, so no consumer needs a special case (VelocityFill.cs.hlsl has the
// derivation and why it cannot double-apply with TAA's workaround).
//
// Not an RG pass: it runs on the pre-compute command list after the whole render
// graph, right before the upscaler reads velocity - every geometry writer has run,
// in-graph consumers (SSR, XeGTAO) see the unfilled buffer, and it sits inside the
// block that already transitions velocity for compute reads (only the UAV round
// trip is added).
// Run it before TAAJitterState::AdvanceToNextFrame (end of the upscale branch):
// GetPrevViewProjNoJitter must still be the previous frame's matrix, or the
// reprojection is identity and the fill writes zeros.

#include "RenderGraph/PassLifecycle.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"

#include <DirectXMath.h>

class IGraphicsDevice;

class VelocityFillPass : public RG::PassLifecycle
{
public:
    void Init(IGraphicsDevice& gfx);
    // Hot reload (standalone: Renderer::ReloadShaders calls it).
    void ReloadShaders(IGraphicsDevice& gfx) override;
    uint32_t HandledEvents() const override { return kReloadShaders; }   // events Renderer::PassSet delivers

    // @p invViewProjNoJitter  inverse of the current unjittered view-projection
    // @p prevViewProj         previous frame's unjittered view-projection
    // Both already transposed for the row-vector HLSL convention, matching how
    // Renderer feeds the equivalent matrices to TAAPass.
    void SetFrameData(const DirectX::XMFLOAT4X4& invViewProjNoJitter,
                      const DirectX::XMFLOAT4X4& prevViewProj)
    {
        m_invViewProjNoJitter = invViewProjNoJitter;
        m_prevViewProj        = prevViewProj;
    }
    void SetViewportSize(uint32_t w, uint32_t h) { m_vpW = w; m_vpH = h; }

    // Records the dispatch into @p cl. @p velocityUav / @p depthSrv are the
    // graph textures' descriptor handles; the CALLER owns transitioning
    // velocity to UNORDERED_ACCESS and depth to a compute-readable state, and
    // owns transitioning velocity back afterwards.
    void Execute(RHI::CommandList cl,
                 uint64_t depthSrv, uint64_t velocityUav);

    bool IsEnabled() const  { return m_enabled; }
    void SetEnabled(bool v) { m_enabled = v; }

private:
    struct alignas(16) VelocityFillCB
    {
        DirectX::XMFLOAT4X4 invViewProjNoJitter;
        DirectX::XMFLOAT4X4 prevViewProj;
        uint32_t            width;
        uint32_t            height;
        float               _pad[2];
    };

    bool CreatePipelines(IGraphicsDevice& gfx);

    IGraphicsDevice*      m_gfx = nullptr;
    ShaderLibrary         m_shaderLib;
    RHI::PipelineState    m_pso;

    DirectX::XMFLOAT4X4 m_invViewProjNoJitter{};
    DirectX::XMFLOAT4X4 m_prevViewProj{};
    uint32_t m_vpW = 0, m_vpH = 0;
    bool     m_enabled = true;
    bool     m_loggedFirstDispatch = false;
};
