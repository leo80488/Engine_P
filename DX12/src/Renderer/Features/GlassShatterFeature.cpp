// Renderer/Features/GlassShatterFeature.cpp - the glass shatter: a post
// effect that a render feature adds to the post chain.
//
// A Voronoi shatter of the finished picture: while one plays, its shards are
// composited in place into Tonemapping's output (the composition buffer) on the
// post chain's list - PostTonemap 0, after Tonemapping and before the Overlay
// stage (the HUD and the overlays draw over the shards).
//   CreatePasses: the pass (registered with ctx.passes for its lifecycle) and
//     its effect, added to the post chain (ctx.post) - the Renderer neither
//     owns the pass nor lists the effect.
//   Prepare(PreResolve) (the post phase, main thread): the harness's test
//     trigger, -glass_shatter_frame=N, on frame N.
//   IGlassShatter: Renderer::TriggerGlassShatter (the editor's Glass Shatter
//     button) finds the feature by the capability.
// No prepare-table entry: the pass makes its textures on its first Execute
// after a trigger, sized from the target it composites into.

#include "Renderer/RenderFeature.h"

#include "Graphics/IGraphicsDevice.h"
#include "PostProcess/IPostProcessEffect.h"
#include "PostProcess/PostChain.h"
#include "RenderGraph/RenderPass/GlassShatterPass.h"
#include "Renderer/PassSet.h"
#include "System/Check.h"
#include "System/CommandLine.h"

namespace
{
// The chain's effect: in place into the composition buffer at the display
// size; the pass restores the buffer's entry state (compute-readable) on exit.
class GlassShatterEffect final : public PostProcess::IEffect
{
public:
    explicit GlassShatterEffect(GlassShatterPass* pass) : m_pass(pass) {}
    RG::Stage   GetStage() const override { return RG::Stage::PostTonemap; }
    int         Order()    const override { return 0; }
    const char* GetName()  const override { return "GlassShatter"; }
    bool        OwnsRegions() const override { return true; }

    bool IsEnabled(const PostProcess::Context& ctx) const override
    {
        return m_pass && m_pass->IsActive() && ctx.resources.tonemapped && ctx.view;
    }

    void Execute(PostProcess::Context& ctx) override
    {
        // The composition buffer's live format (SDR or HDR output) - the PSO
        // must declare it; Tonemapping left it beside its output.
        m_pass->SetTargetFormat(ctx.resources.tonemappedFormat);
        const uint32_t r = ctx.gfx->BeginGPUTimestamp(ctx.cl, "GlassShatter");
        m_pass->Execute(ctx.cl, ctx.resources.tonemapped, RHI::ResourceState::SHADER_RESOURCE_COMPUTE,
                        ctx.view->vpWidth, ctx.view->vpHeight, ctx.view->deltaTime);
        ctx.gfx->EndGPUTimestamp(ctx.cl, r);
    }

private:
    GlassShatterPass* m_pass = nullptr;
};

class GlassShatterFeature final : public IRenderFeature, public IGlassShatter
{
public:
    const char* Name() const override { return "GlassShatter"; }
    // PreResolve: the test trigger's prepare point (and what makes the registry
    // call CreatePasses); PostTonemap: where its effect runs, in the chain.
    uint32_t    Stages() const override
    {
        return StageBit(RG::Stage::PreResolve) | StageBit(RG::Stage::PostTonemap);
    }

    void CreatePasses(FeatureCompileContext& ctx) override
    {
        m_pass = std::make_unique<GlassShatterPass>();
        m_pass->Init(ctx.gfx);
        ctx.passes.Register("GlassShatter", m_pass);
        if (ENGINE_CHECK(ctx.post != nullptr, "GlassShatterFeature: no post chain to add its effect to"))
            ctx.post->Add(std::make_unique<GlassShatterEffect>(m_pass.get()));
    }

    void Prepare(RG::Stage stage, FeatureFrameContext& ctx) override
    {
#ifdef WITH_HARNESS
        // Test hook: -glass_shatter_frame=N triggers the shatter at the screen
        // centre on frame N, as the editor's button does - nothing else a run can
        // reach triggers it.
        static const int s_frame = Sys::CommandLine::Get().GetInt("-glass_shatter_frame", -1);
        if (stage == RG::Stage::PreResolve && s_frame >= 0 &&
            ctx.view.currentFrame == static_cast<FrameIndex>(s_frame))
            Trigger(0.5f, 0.5f);
#else
        (void)stage; (void)ctx;
#endif
    }

    void Trigger(float impactU, float impactV) override
    {
        if (m_pass) m_pass->Trigger(impactU, impactV);
    }

private:
    std::unique_ptr<GlassShatterPass> m_pass;
};
}

REGISTER_RENDER_FEATURE(GlassShatterFeature)
