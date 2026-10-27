// Renderer/Features/HeightFogFeature.cpp - UE-style exponential height fog as
// a render feature.
//
// An analytic fullscreen composite in the Atmosphere stage, right after the
// clouds (its RG::After constraint, RenderGraph/Order.h): cloud, water and sky
// already in HDR get fogged, the video overlay stays unfogged and the froxel
// fog's near volumetric detail composes over this analytic far fog.
//   Extract (the game half, after SyncSkyboxIBL): the first HeightFogComponent
//     - a singleton by convention - is the fog. The sun's direction and colour
//     need no plumbing: the apply shader reads LightCB, time-of-day
//     authoritative by then.
//   The view: the pass declares RG::ShowFlag::HeightFog, so the main graph's
//     SetView hides it - and its forward fog - while the environment is
//     hidden: a debug view, an ortho camera, the path tracer's camera primary.
//     The feature has no Prepare.
//   IForwardFogContributor: the same fog for the surfaces drawn after the fog
//     composites (Renderer::WriteFogApplyCB).
//   IMediumContributor: the fog as a participating medium for the path tracers
//     (Renderer::UploadMediumSet), whether or not the view shows the composite.
//   IFroxelHeightFog: the ray-traced froxels light the fog within their range; the
//     composite and its forward fog start where the range ends.
//
// The component's other homes (ECS reflection, serialization, the scene
// defaults, the Sequencer's targets, the editor) live with the component, not
// here. The preview graphs draw no fog.

#include "Renderer/RenderFeature.h"

#include "ECS/ECS.h"
#include "ECS/HeightFogComponent.h"
#include "Graphics/FogApply.h"
#include "Graphics/MediumSet.h"
#include "RenderGraph/RenderPass/HeightFogPass.h"

#include <algorithm>

namespace
{
class HeightFogFeature final : public IRenderFeature, public IForwardFogContributor, public IMediumContributor,
                               public IFroxelHeightFog
{
public:
    const char* Name() const override { return "HeightFog"; }
    uint32_t    Stages() const override { return StageBit(RG::Stage::Atmosphere); }

    void AddPasses(RG::Stage, FeatureCompileContext& ctx) override
    {
        m_pass = ctx.AddPass(std::make_unique<HeightFogPass>(ctx.textures.depth),
                             { RG::After("CloudPass", "composites over the clouds, under the video overlay and the "
                                                      "volumetric fog") });
    }

    void Extract(FeatureExtractContext& ctx) override
    {
        if (!m_pass)
            return;
        HeightFogComponent* hf = nullptr;
        if (auto* p = ctx.world.GetPool<HeightFogComponent>(); p && !p->Data().empty())
            hf = &p->Data()[0];   // singleton-by-convention, first wins
        if (hf) m_pass->SetParams(*hf);
        m_pass->SetEnabled(hf && hf->enabled);
    }

    bool WriteForwardFog(FogApply::Constants& c) const override
    {
        HeightFogPass::HeightFogConstants hf{};
        if (!m_pass || !m_pass->ForwardConstants(hf))
            return false;
        c.flags         |= FogApply::kFlagHeight;
        c.hfDensity      = hf.fogDensity;
        c.hfFalloff      = hf.fogHeightFalloff;
        c.hfHeight       = hf.fogHeight;
        c.hfStart        = hf.startDistance;
        c.hfColor[0]     = hf.fogColor[0];
        c.hfColor[1]     = hf.fogColor[1];
        c.hfColor[2]     = hf.fogColor[2];
        c.hfMaxOpacity   = hf.maxOpacity;
        c.hfSunInscatter = hf.sunInscatterIntensity;
        c.hfAnisotropy   = hf.anisotropy;
        return true;
    }

    void SetFroxelHeightFog(float range) override
    {
        if (m_pass) m_pass->SetStartFloor(range);
    }

    bool WriteMedium(Media::Set& set) const override
    {
        if (!m_pass || !m_pass->IsEnabled())
            return false;
        const HeightFogComponent& p = m_pass->GetParams();
        if (!(p.fogDensity > 0.0f))
            return false;
        set.height.density    = p.fogDensity;
        set.height.falloff    = std::max(p.fogHeightFalloff, 0.0f);
        set.height.height     = p.fogHeight;
        set.height.anisotropy = std::clamp(p.anisotropy, 0.0f, 0.95f);
        set.height.albedo[0]  = std::clamp(p.albedo.x, 0.0f, 1.0f);
        set.height.albedo[1]  = std::clamp(p.albedo.y, 0.0f, 1.0f);
        set.height.albedo[2]  = std::clamp(p.albedo.z, 0.0f, 1.0f);
        return true;
    }

private:
    HeightFogPass* m_pass = nullptr;   // owned by the main graph
};
}

REGISTER_RENDER_FEATURE(HeightFogFeature)
