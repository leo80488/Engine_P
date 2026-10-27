#pragma once

// UpscalerReactiveMaskPass - the reactive mask a temporal upscaler reads
// (UpscalerReactiveMask.cs.hlsl has the why).
//
// One R8_UNORM texture at the render resolution, written from the stencil plane by
// one dispatch right before the upscaler and handed to it: FSR 2's dispatch
// `reactive`, DLSS SR's BiasCurrentColor tag. Do not give it to DLSS-RR: the RR
// guide does not document the hint, and an undocumented colour input
// (ColorBeforeParticles) turned RR's output black.
//
// Standalone (not an RG pass), like VelocityFillPass: it runs on the post list,
// after the whole graph, where the stencil is already compute-readable for TAA's
// own read of the same bits.
//
// Lifetime: Prepare, on the main thread at the frame boundary, allocates the mask
// while an upscaler that reads it runs and releases it otherwise, so a TAA / native
// frame owns nothing.

#include "RenderGraph/PassLifecycle.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"

#include <atomic>
#include <cstdint>

class IGraphicsDevice;

class UpscalerReactiveMaskPass : public RG::PassLifecycle
{
public:
    struct ServiceRequest { bool wanted = false; };

    void Init(IGraphicsDevice& gfx);
    // Hot reload (standalone: the lifecycle list calls it).
    void ReloadShaders(IGraphicsDevice& gfx) override;
    void Release(IGraphicsDevice& gfx);
    uint32_t HandledEvents() const override { return kReloadShaders | kShutdown; }   // events Renderer::PassSet delivers
    void Shutdown(IGraphicsDevice& gfx) override { Release(gfx); }

    // Main thread, at the frame boundary: own the mask at the HDR target's size
    // (gfx.GetRenderWidth/Height) while @p wanted, release it otherwise.
    void Prepare(IGraphicsDevice& gfx, bool wanted);

    // The service point: OnService calls Prepare with Request().wanted, set when the
    // resolved upscaler reads the mask.
    ServiceRequest& Request() { return m_request; }
    void OnService(RG::ServiceContext& c) override { Prepare(*c.gfx, m_request.wanted); }
    bool Ready() const { return m_wanted && m_tex.IsValid() && m_pso.IsValid(); }

    // Runs on the post list before the upscaler: fills the mask from @p stencilSrv
    // (the stencil-plane SRV TAA reads) with TAA's weights, leaves it compute-readable
    // (SHADER_RESOURCE_COMPUTE) and returns it. Null when it did not run (not prepared,
    // no stencil view, or not this frame's render size @p renderW x @p renderH): give
    // the upscaler nothing then.
    const RHI::Texture* Execute(RHI::CommandList cl, IGraphicsDevice& gfx, uint64_t stencilSrv,
                                uint32_t renderW, uint32_t renderH,
                                float particleWeight, float outlineWeight);

    // The debug target upscalerReactive (Renderer/DebugTargets.h): the mask
    // when the last frame wrote it.
    const RHI::Texture* GetDebugOutput(RHI::ResourceState& state) const
    {
        if (!m_tex.IsValid() || !m_written.load(std::memory_order_acquire)) return nullptr;
        state = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;   // its state between frames
        return &m_tex;
    }

private:
    struct alignas(16) ReactiveMaskCB
    {
        uint32_t width;
        uint32_t height;
        uint32_t particleBit;
        uint32_t outlineBit;
        float    particleWeight;
        float    outlineWeight;
        float    _pad[2];
    };

    bool CreatePipelines(IGraphicsDevice& gfx);

    ServiceRequest m_request;   // read by OnService

    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_pso;
    RHI::Texture       m_tex;
    RHI::ResourceState m_state{};
    std::atomic<bool>  m_written{ false };
    uint32_t           m_w = 0, m_h = 0;
    bool               m_wanted = false;
    bool               m_loggedFirstDispatch = false;
};
