#pragma once

// PostProcess::PostChain - the post effects, run in (RG::Stage, Order(), name)
// order. An effect says its post stage (RenderGraph/Stage.h, Resolve ... Overlay)
// and its Order() within it; any number share a stage. Resolve holds
// NPRStabilize ... FSR 2 | DLSS | TAA, FXAA (PostProcess/ResolveEffects.h); the
// HDR effects sit at PostResolveHDR and ToneMap at Tonemap
// (BuiltinPostProcessEffects.h), GlassShatter at PostTonemap, the overlays at
// Overlay (OverlayEffects.h). The Renderer runs it a range of stages at a time,
// with what is not an effect between: the TAA jitter's advance after Resolve, the
// "PostProcessStack" region around PostResolveHDR .. Tonemap.
//
// The chain is a pure CONSUMER of the volume / profile system's output: each frame
// the Renderer points Context::resolved at the frame's
// ResolvedPostProcessSettings (produced by PostProcessResolveSystem) and the
// effects read their fields from it. It knows nothing about volumes, profiles,
// blending or overrides - that happens upstream, in the ECS resolve.

#include "PostProcess/IPostProcessEffect.h"

#include <memory>
#include <vector>

namespace PostProcess
{

class PostChain
{
public:
    PostChain() = default;
    ~PostChain() = default;

    PostChain(const PostChain&)            = delete;
    PostChain& operator=(const PostChain&) = delete;

    // Adds @p effect; Finalize puts the chain in order.
    void Add(std::unique_ptr<IEffect> effect);
    // After the last Add (Renderer::Compile): sorts by (stage, Order, name),
    // checks each effect's stage is a post stage, logs
    // [POST_CHAIN] effects=N <name>(<stage>:<order>) ...
    void Finalize();

    // Runs the enabled effects of the stages first..last in order, each in a
    // GPU region ("PostProcess.<name>", unless it opens its own -
    // IEffect::OwnsRegions). When the range holds the Resolve stage, the
    // stage ends with its routing: ctx.hdrSrv becomes the last claim
    // (IEffect::ClaimsResolve). `ctx.cl` must be a valid command list
    // recorded serially on a single thread, and `ctx.resolved` should point
    // at the frame's resolved settings (engine defaults otherwise).
    void Execute(Context& ctx, RG::Stage first, RG::Stage last);
    void Execute(Context& ctx, RG::Stage stage) { Execute(ctx, stage, stage); }

    // Whether this frame's chain must record on the graphics queue: an effect
    // asks (IEffect::RequiresGraphicsQueue - DLSS). Before the list opens.
    bool RequiresGraphicsQueue(const Context& ctx) const;

    size_t Count() const { return m_effects.size(); }

private:
    std::vector<std::unique_ptr<IEffect>> m_effects;
    bool m_finalized = false;
};

} // namespace PostProcess
