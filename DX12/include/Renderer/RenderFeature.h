#pragma once

// Renderer/RenderFeature.h - a render feature: one effect's passes and the
// Renderer-side work around them, in the effect's own files.
//
// A feature adds its passes, its World sync, its prepare / record work and its
// debug targets from its own files; the Renderer calls every feature at fixed
// INSERTION POINTS, named by RG::Stage (RenderGraph/Stage.h):
//   - Compile, in the main graph: AddPasses(stage) for each graph stage, in stage
//     order, after the Renderer's own passes. ctx.AddPass tags the pass with the
//     stage, and its order constraints say where in the stage it goes
//     (RenderGraph/Order.h; e.g. the height fog's RG::After("CloudPass"): over the
//     clouds, under the video and the volumetric fog) - without one, at the
//     stage's end. The graph's first sort puts it there;
//   - Compile, after the graph: CreatePasses, where the Renderer creates the
//     passes it records outside the graph (among the AA passes) and where a
//     feature adds a post effect to the post chain (ctx.post);
//   - the game half: Extract, in the scene build where the Renderer reads the
//     World's environment (BuildScene_UploadLights, after SyncSkyboxIBL);
//   - the render half, before the main graph records: Prepare(stage) for each
//     graph stage, after the frame's view is settled (FeatureFrameContext's
//     sceneView) and before the forward fog and the graph read what the features
//     set;
//   - the render half: Record(stage) for a stage outside the graph - PreResolve
//     on the pre-compute list, after the graph and before the AA / upscaler;
//   - the render half's post phase: Prepare(PreResolve), after the frame plan's
//     first stage-C pass - a feature's facts (FeatureFrameContext::facts) are the
//     stage's last, planned again right after;
//   - Compile: RegisterDebugTargets, a feature's own rows of the debug target
//     registry (Render.targets(), the viewport's Buffer menu, captures).
// At one insertion point features run by Order(), then by Name().
//
// Capabilities: what only a few features provide is a small interface beside
// IRenderFeature (IForwardFogContributor below), found with
// RenderFeatureRegistry::ForEach - not one more hook every feature carries.
//
// Lifecycle stays on the passes: a feature registers each pass it creates outside
// the graph with ctx.passes (PassSet::Register); shutdown, reload, resize and
// world clear reach the pass from there, and Compile's pass census fails a pass
// nobody registered.
//
// Registration: a feature's .cpp says REGISTER_RENDER_FEATURE(Type), which defines
// its factory. tools/gen_render_features.py collects them into
// src/Renderer/RenderFeatures.generated.cpp, whose RegisterRenderFeatures creates
// every feature in name order; verify's offline gate render_features fails while
// that file is stale. Do not use a static registrar: EngineCore is a static
// library, the linker drops an object nothing references - registrar and all -
// and the feature would silently vanish. The generated list references every
// factory, which links every feature's object in.

#include "Renderer/DebugTargets.h"
#include "Renderer/FrameResources.h"
#include "Renderer/FrameView.h"
#include "RenderGraph/RenderGraph.h"
#include "RenderGraph/Stage.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>
#include <vector>

class IGraphicsDevice;
class PassSet;
class World;
namespace FogApply { struct Constants; }
namespace Media { struct Set; }
namespace PostProcess { class PostChain; }

constexpr uint32_t StageBit(RG::Stage s) { return 1u << static_cast<uint32_t>(s); }

// ==== Contexts: what a feature has at each insertion point ====

// Compile (main thread): the main graph, the Renderer's lifecycle registry, the
// device and the graph textures.
struct FeatureCompileContext
{
    RG::RenderGraph&     graph;
    PassSet&             passes;
    IGraphicsDevice&     gfx;
    const FrameTextures& textures;
    RG::Stage            stage = RG::Stage::Unstaged;   // set for AddPasses: the insertion point's
    // The post chain, for CreatePasses: a feature's post effect goes in with
    // post->Add(std::make_unique<ItsEffect>(...)) - its stage and Order place it
    // among the chain's (PostProcess/PostChain.h); the Renderer finalizes the chain
    // after every Add. Null at the other points.
    PostProcess::PostChain* post = nullptr;

    // A graph pass at this insertion point's stage (AddPasses only; the
    // feature may keep the pointer, the graph owns the pass). @p order places
    // it among the stage's passes (RenderGraph/Order.h):
    // ctx.AddPass(std::move(p), { RG::After("CloudPass", "why") }).
    template <class T>
    T* AddPass(std::unique_ptr<T> pass, std::initializer_list<RG::Order> order = {})
    {
        T* raw = pass.get();
        AddGraphPass(std::move(pass), order);
        return raw;
    }
    void AddGraphPass(std::unique_ptr<RG::RenderPass> pass, std::initializer_list<RG::Order> order = {});
};

// The render half, recording a stage outside the graph: that stage's command
// list, the device, the main graph (physical textures and their tracked states:
// a feature that transitions a graph texture tells the graph,
// SetTextureState), the frame's view and plan, and the frame resources.
struct FeatureRecordContext
{
    RHI::CommandList            cl;
    IGraphicsDevice&            gfx;
    RG::RenderGraph&            graph;
    const FrameTextures&        textures;
    const FrameView&            view;
    const FramePlan::Decisions& plan;
    FrameResources&             resources;
};

// The game half (the scene build, main thread): the World this frame renders, the device,
// and this frame's camera without the TAA jitter (Renderer::BuildCameraViewProj: the
// FrameView's view is written only by UploadFrameData, after the scene build).
struct FeatureExtractContext
{
    World&              world;
    IGraphicsDevice&    gfx;
    DirectX::XMFLOAT4X4 viewProjNoJitter{};
};

// The render half, at a prepare point (main thread, before the stage records):
// the frame's view, the plan so far, the facts a feature reports for the plan's
// next stage, the device, and what the main view shows this frame
// (RenderGraph/SceneView.h) - a debug view mode, an ortho camera and the path
// tracer's camera primary hide the environment. A feature's graph pass that
// declares a ShowFlag (RenderPass::RequiredShowFlags) needs nothing from here: the
// graph has already set its Shown().
struct FeatureFrameContext
{
    const FrameView&            view;
    const FramePlan::Decisions& plan;
    FramePlan::Facts&           facts;
    IGraphicsDevice&            gfx;
    const RG::SceneView&        sceneView;
    // The main view's draw packets this frame, final by now: set for the graph stages,
    // null at the points outside the graph.
    const std::vector<DrawPacket>* draws = nullptr;
};

// A debug target a feature owns: its row of the registry - listed right after
// the curated row @c after names (after the curated rows when it names none,
// or one the registry does not have) - and how to find its texels.
struct FeatureDebugTarget
{
    DebugTargets::Info info;             // name, label, group, semantic, meaning, channels, default mapping
    const char*        after = nullptr;  // the curated row it follows
    // The last frame's texture and the state it is in; null when there is none.
    std::function<const RHI::Texture*(RHI::ResourceState& state)> texture;
    const char*        missing = "";     // why there is none
};
class DebugTargetRegistry
{
public:
    void Add(FeatureDebugTarget target) { m_targets.push_back(std::move(target)); }
    const std::vector<FeatureDebugTarget>& Targets() const { return m_targets; }

private:
    std::vector<FeatureDebugTarget> m_targets;
};

class IRenderFeature
{
public:
    virtual ~IRenderFeature() = default;
    // A string literal: the logs and Compile's [POSTSETUP] steps keep the pointer.
    virtual const char* Name() const = 0;
    // The insertion points it takes part in: StageBit(...) | ...
    virtual uint32_t    Stages() const = 0;
    // At one insertion point: lower first, then by Name.
    virtual int         Order() const { return 0; }

    // Compile, in the main graph: this stage's passes (ctx.AddPass).
    virtual void AddPasses(RG::Stage, FeatureCompileContext&) {}
    // Compile, after the graph: the passes it records outside the graph,
    // created and registered with ctx.passes.
    virtual void CreatePasses(FeatureCompileContext&) {}
    // The game half: what this frame needs from the World.
    virtual void Extract(FeatureExtractContext&) {}
    // The render half, before the stage records: this frame's setup (the main view's; it holds for
    // every view - what a feature's view-safe pass needs of a view is PrepareView's).
    virtual void Prepare(RG::Stage, FeatureFrameContext&) {}
    // The render half, a secondary view's segment before its graph records: what the feature's view-safe
    // passes need of that view (ctx.view is its frame: its camera, its sizes). The passes are bound to it.
    virtual void PrepareView(FeatureFrameContext&) {}
    // The render half: record at a stage outside the graph.
    virtual void Record(RG::Stage, FeatureRecordContext&) {}
    // Compile: the debug targets it owns.
    virtual void RegisterDebugTargets(DebugTargetRegistry&) {}
};

class RenderFeatureRegistry
{
public:
    void Add(std::unique_ptr<IRenderFeature> feature);
    // After RegisterRenderFeatures: sorts by (Order, Name), checks names and
    // stages, logs [RENDER_FEATURES] count= <name>(<stages>:<order>) ...
    void Finalize();

    void AddPasses(RG::Stage stage, FeatureCompileContext& ctx);
    // The features with a stage outside the graph. @p created, if set, is told
    // each one's name once its passes exist (Compile's [POSTSETUP] steps).
    void CreatePasses(FeatureCompileContext& ctx, void (*created)(const char* name) = nullptr);
    void Extract(FeatureExtractContext& ctx);
    // Every graph stage's Prepare, in stage order.
    void PrepareGraphStages(FeatureFrameContext& ctx);
    // A stage outside the graph (PreResolve).
    void Prepare(RG::Stage stage, FeatureFrameContext& ctx);
    // A secondary view's segment (IRenderFeature::PrepareView), every feature.
    void PrepareView(FeatureFrameContext& ctx);
    void Record(RG::Stage stage, FeatureRecordContext& ctx);
    void RegisterDebugTargets(DebugTargetRegistry& registry);
    // Whether a feature of this name is registered (the frame plan's state asks).
    bool Has(const char* name) const;

    // Each feature that provides capability @p I, in registry order.
    template <class I, class Fn>
    void ForEach(Fn&& fn) const
    {
        for (const std::unique_ptr<IRenderFeature>& f : m_features)
            if (I* c = dynamic_cast<I*>(f.get()))
                fn(*c);
    }

    size_t Count() const { return m_features.size(); }

private:
    std::vector<std::unique_ptr<IRenderFeature>> m_features;
    bool m_finalized = false;
};

// ==== Capabilities ====

// The fog the opaque scene gets this frame, for the surfaces drawn after the fog
// composites (Renderer::WriteFogApplyCB, Graphics/FogApply.h). Writes its part of
// @p c and returns true when it fogs this frame - exactly when, and as, its
// composite runs.
class IForwardFogContributor
{
public:
    virtual ~IForwardFogContributor() = default;
    virtual bool WriteForwardFog(FogApply::Constants& c) const = 0;
};

// The fog a feature owns as a participating medium, for the path tracers and the ray-traced
// froxel lighting (Graphics/MediumSet.h, Renderer::UploadMediumSet). Writes its part of @p set
// and returns true when it holds a medium this frame. Render half; unlike the forward fog it
// does not depend on the view showing the feature's composite.
class IMediumContributor
{
public:
    virtual ~IMediumContributor() = default;
    virtual bool WriteMedium(Media::Set& set) const = 0;
};

// The reference path tracer's camera fog, composited over the scene by a render feature
// (Renderer/Features/PathTraceFogFeature.cpp): the Renderer hands it the fog target's heap index on
// the render half before the graph records (PathTracePass::GetCameraFogIndex), 0 when the path
// tracer does not trace the camera's fog this frame.
class IPathTraceFogComposite
{
public:
    virtual ~IPathTraceFogComposite() = default;
    virtual void SetPathTraceFog(uint32_t fogIdx) = 0;
};

// The ray-traced froxel fog holds the height fog's medium up to @p range metres along each camera
// ray this frame (VolumetricFogPass's FROXEL_RT density): the height fog's own composite and its
// forward fog start no nearer. 0: the composite takes its whole ray. The Renderer sets it on the
// render half before the graph records.
class IFroxelHeightFog
{
public:
    virtual ~IFroxelHeightFog() = default;
    virtual void SetFroxelHeightFog(float range) = 0;
};

// The background behind the Transmissive surfaces is refracted and tinted this frame, before
// their colour pass: the Renderer tells that pass through LightCB.gTransmissionApplied, and
// without it the surfaces fall back to a grey coverage blend.
class ITransmissionComposite
{
public:
    virtual ~ITransmissionComposite() = default;
    virtual bool TransmissionApplied() const = 0;
};

// The glass shatter's one-shot trigger, for the Renderer's public
// TriggerGlassShatter (the editor's Glass Shatter button) - the feature owns the
// pass (Renderer/Features/GlassShatterFeature.cpp). Captures the picture on the
// next frame and breaks it from @p impactU/V in [0, 1]; a trigger while one plays
// restarts it.
class IGlassShatter
{
public:
    virtual ~IGlassShatter() = default;
    virtual void Trigger(float impactU, float impactV) = 0;
};

// src/Renderer/RenderFeatures.generated.cpp (tools/gen_render_features.py).
void RegisterRenderFeatures(RenderFeatureRegistry& registry);

// In a feature's .cpp, at namespace scope: the factory the generated list
// calls. @p Type is the class's unqualified name (it names the factory).
#define REGISTER_RENDER_FEATURE(Type)                                        \
    static_assert(std::is_base_of_v<IRenderFeature, Type>,                   \
                  #Type " must derive from IRenderFeature");                 \
    std::unique_ptr<IRenderFeature> CreateRenderFeature_##Type()              \
    {                                                                        \
        return std::make_unique<Type>();                                     \
    }
