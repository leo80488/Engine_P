#pragma once
#include "RenderGraph/RenderGraph.h"
#include "Graphics/FrameCB.h"

// Opt-in raw, linear HDR measurement. Runs on the post-graph graphics list
// after the HDR transition, before AA/exposure/tonemap. No preview instances.
class LumaReducePass : public RG::RenderPass
{
public:
    const char* GetName() const override { return "LumaReducePass"; }
    void Setup(RG::RenderGraphBuilder&) override {}
    void Init(IGraphicsDevice& gfx) override;
    void Shutdown(IGraphicsDevice& gfx) override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    // Hot reload (standalone: Renderer::ReloadShaders calls it).
    void ReloadShaders(IGraphicsDevice& gfx) override;
    uint32_t HandledEvents() const override { return kReloadShaders | kShutdown; }   // the events Renderer::PassSet delivers to this pass
    void SetSource(uint32_t index, uint32_t width, uint32_t height, uint64_t renderFrame)
    { m_source = index; m_width = width; m_height = height; m_renderFrame = renderFrame; }

private:
    static constexpr uint32_t kTiles = 64;
    struct alignas(16) Constants { uint32_t source, width, height, varianceUav; uint32_t samples, enabled; float alternation; uint32_t measureAlpha; };

    bool CreatePipeline(IGraphicsDevice& gfx);

    IGraphicsDevice* m_gfx = nullptr;
    FrameCB<Constants> m_cb;
    RHI::Shader m_shader;
    RHI::PipelineState m_pso;
    RHI::GPUBuffer m_tiles, m_pixelMoments;
    uint32_t m_varianceStart=500,m_varianceSamples=0,m_varianceSource=~0u,m_varianceWidth=0,m_varianceHeight=0;
    uint32_t m_varianceHistory[RHI::kFramesInFlight]{};
    bool m_pixelVariance=false,m_momentsInitialized=false;
    bool m_measureAlpha=false;
    float m_varianceAlternation=0;
    RHI::Texture m_fixture; // harness-only known-radiance binding control
    RHI::ResourceState m_state = RHI::ResourceState::UNDEFINED;
    RHI::GPUBuffer m_readback[RHI::kFramesInFlight];
    const float* m_mapped[RHI::kFramesInFlight]{};
    uint64_t m_sequence[RHI::kFramesInFlight]{};
    uint64_t m_renderFrameHistory[RHI::kFramesInFlight]{};
    uint64_t m_renderFrame = 0;
    uint32_t m_sourceHistory[RHI::kFramesInFlight]{};
    uint32_t m_widthHistory[RHI::kFramesInFlight]{}, m_heightHistory[RHI::kFramesInFlight]{};
    bool m_primed[RHI::kFramesInFlight]{};
    uint64_t m_frame = 0;
    uint64_t m_stopAfter = 0; // harness same-run ON/OFF pixel control; 0 = unlimited
    bool m_stopLogged = false;
    uint32_t m_source = ~0u, m_width = 0, m_height = 0;
    bool m_ready = false;
};
