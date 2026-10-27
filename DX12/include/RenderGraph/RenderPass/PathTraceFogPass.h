#pragma once

// PathTraceFogPass - the reference path tracer's camera fog over the scene.
//
// While the path tracer owns the camera rays' fog on its GBuffer primary
// (FramePlan::FogOwner::PathTracer), PathTracePass's FogCamera entry leaves per pixel the light
// the fog scatters toward the camera (rgb) and the camera segment's transmittance (a), a running
// mean like the image's. This pass composites it where the height fog would: one fullscreen
// ONE / INV_SRC_ALPHA draw after CloudPass, so the sky, the water and the clouds are fogged too -
// HDR x a + rgb. The raster fog passes and their forward fog are hidden on such a frame.
// Bindings: the constants at b2 (the fog target's heap index); the depth is bound read-only for
// the fullscreen pipeline's formats.

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"

#include <vector>

class PathTraceFogPass : public RG::RenderPass
{
public:
    // HLSL mirror: PathTraceFogApply.ps.hlsl's PTFogCB.
    struct alignas(16) Constants
    {
        uint32_t fogIdx;
        uint32_t _pad[3];
    };
    static_assert(sizeof(Constants) == 16, "PTFogCB layout drift");

    explicit PathTraceFogPass(RG::RGTextureHandle depth) : m_depth(depth) {}

    const char* GetName() const override { return "PathTraceFogPass"; }
    void Setup(RG::RenderGraphBuilder& b) override;
    void Init(IGraphicsDevice& gfx) override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    bool IsActive() const override { return !RG::PassActivityGatesEnabled() || (m_fogIdx != 0 && m_gfx); }
    // kViewSafe: it composites the bound view's camera fog (SetFog, each view's segment).
    uint32_t HandledEvents() const override { return kViewSafe; }

    ShaderLibrary*         GetReloadableShaderLibrary() override { return &m_shaderLib; }
    std::vector<PSOCache*> GetReloadablePSOCaches()     override { return { &m_psoCache }; }

    // This frame's fog target as a heap index; 0 when the path tracer does not composite its fog.
    // Render half, before the graph records: the bound view's.
    void SetFog(uint32_t fogIdx) { m_fogIdx = fogIdx; }

private:
    RG::RGTextureHandle m_depth;
    IGraphicsDevice*    m_gfx = nullptr;
    ShaderLibrary       m_shaderLib;
    PSOCache            m_psoCache;
    uint32_t            m_fogIdx = 0;
};
