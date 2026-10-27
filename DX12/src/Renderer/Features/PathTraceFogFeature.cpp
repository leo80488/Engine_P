// Renderer/Features/PathTraceFogFeature.cpp - the reference path tracer's camera fog as a render
// feature.
//
// One graph pass in the Atmosphere stage, right after the clouds - where the height fog
// composites (its RG::After constraint): PathTraceFogPass draws the fog the path tracer integrated
// along the camera rays over the scene, HDR x T + the light the fog scatters toward the camera, so
// the sky, the water and the clouds are fogged as the path tracer's own surfaces are.
//   IPathTraceFogComposite (the Renderer, render half, before the graph records): the fog
//     target's heap index while the path tracer owns the camera rays' fog on its GBuffer primary
//     (FramePlan::FogOwner::PathTracer; its camera primary integrates the fog in the path), else
//     0 and nothing draws. The raster fog passes are hidden on such a frame, so the fog is applied
//     once.
// The preview graphs draw no fog.

#include "Renderer/RenderFeature.h"

#include "RenderGraph/RenderPass/PathTraceFogPass.h"

#include <memory>

namespace
{
class PathTraceFogFeature final : public IRenderFeature, public IPathTraceFogComposite
{
public:
    const char* Name() const override { return "PathTraceFog"; }
    uint32_t    Stages() const override { return StageBit(RG::Stage::Atmosphere); }

    void AddPasses(RG::Stage, FeatureCompileContext& ctx) override
    {
        m_pass = ctx.AddPass(std::make_unique<PathTraceFogPass>(ctx.textures.depth),
                             { RG::After("CloudPass", "fogs the clouds, the water and the sky, as the height fog "
                                                      "does") });
    }

    void SetPathTraceFog(uint32_t fogIdx) override
    {
        if (m_pass) m_pass->SetFog(fogIdx);
    }

private:
    PathTraceFogPass* m_pass = nullptr;   // owned by the main graph
};
}

REGISTER_RENDER_FEATURE(PathTraceFogFeature)
