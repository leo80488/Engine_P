#pragma once

// IPostProcessEffect - one effect of the PostProcess::PostChain
// (PostProcess/PostChain.h), which the Renderer drives a range of stages at a time
// through PostChain::Execute(). An effect says where it runs: its RG::Stage (a
// post stage, Resolve ... Overlay) and its Order() within that stage, lower
// first; the chain sorts by (stage, Order, name), and any number of effects share
// a stage.
//
// Concurrency: Execute() runs on the post chain's render worker. Do NOT keep the
// Context beyond a single Execute call.

#include <cstdint>
#include <DirectXMath.h>
#include "Graphics/GraphicsStruct.h"  // RHI::CommandList
#include "RenderGraph/Stage.h"

class IGraphicsDevice;
struct FrameView;        // Renderer/FrameView.h
struct FrameResources;   // Renderer/FrameResources.h
struct FrameTextures;    // Renderer/FrameResources.h
namespace FramePlan { struct Decisions; }   // Renderer/FramePlan.h
namespace RG { class RenderGraph; }         // RenderGraph/RenderGraph.h

namespace PostProcess
{

struct ResolvedPostProcessSettings;

// The named post resources: what an effect leaves for one that runs later - a
// typed field each, never a lookup by name. A new kind is one field here, its
// single source, instead of a new Context member.
struct PostResources
{
    uint64_t exposure  = 0;   // AutoExposure's result this frame; Tonemapping reads it
    // The view's 1x1 R32_FLOAT exposure texture (SHADER_RESOURCE_COMPUTE), the upscalers' Exposure input;
    // null unless an upscaler that takes the engine's exposure is selected.
    const RHI::Texture* exposureTex = nullptr;
    uint64_t bloom     = 0;   // Bloom's chain; Tonemapping
    uint64_t lensFlare = 0;   // LensFlare's flare; Tonemapping
    // The view's exposure buffer, filled by the Renderer before the chain runs. AutoExposure writes it first in
    // the Resolve stage, so NPRStabilize (which scales exposure-protected pixels by its reciprocal) reads the
    // value ToneMap will apply.
    uint64_t exposureBuffer = 0;
    // The upscalers' reactive mask this frame (ReactiveMask: R8, render size), FSR
    // 2's reactive mask and DLSS SR's BiasCurrentColor; null = none.
    const RHI::Texture* reactiveMask = nullptr;
    // Tonemapping's output, the composition buffer (compute-readable), which a
    // PostTonemap effect composites into in place (GlassShatter).
    const RHI::Texture* tonemapped = nullptr;
    RHI::Format         tonemappedFormat = RHI::Format::UNKNOWN;   // its live format (SDR / HDR output)
};

// Per-frame bus threaded through every effect, acting as the ping-pong pointer:
// effects that consume the running HDR result read `hdrSrv`; effects that
// produce a new one overwrite it. What an effect leaves for a consumer several
// effects later goes in `resources` (PostResources).
struct Context
{
    RHI::CommandList cl{};
    IGraphicsDevice* gfx = nullptr;

    // The size of the image after the Resolve stage, which the HDR and
    // tonemap effects run at: the display size once an upscaler resolved (the
    // frame plan's), the render size otherwise - what the pool was made at.
    // The Resolve stage's own effects run at the render size (`view`).
    uint32_t viewportWidth  = 0;
    uint32_t viewportHeight = 0;
    float    deltaTime      = 0.0f;
    // A secondary view of the frame (Renderer_Views.cpp): only the view-safe effects
    // (IEffect::ViewSafe) run, or claim the resolve.
    bool     secondaryView  = false;
    // The view's RG::ShowFlag bits (RenderGraph/SceneView.h): the Overlay stage's editor overlays
    // draw only where EditorOverlays is set.
    uint32_t show           = ~0u;

    // The running HDR scene SRV. HDR producers (NPRStabilize, DoF, CAS, Underwater,
    // Stylize) replace it; after the Resolve stage it is the resolved image
    // (IEffect::ClaimsResolve). Readers include Bloom and Tonemapping.
    uint64_t hdrSrv = 0;
    // Its texture while that is not the HDR scene (NPRStabilize's output) - the
    // upscalers take textures; null = the HDR scene.
    const RHI::Texture* hdrTex = nullptr;

    // METERING source for AutoExposure - the raw lit HDR scene BEFORE NPRStabilize.
    // It must not be `hdrSrv`: NPRStabilize divides exposure-protected NPR pixels by
    // the previous frame's exposure, so metering its output closes a positive
    // feedback loop and the meter ends up measuring itself. Always render resolution
    // (the upscaler runs after NPRStabilize), which also makes metering cheaper under
    // DLSS / FSR. 0 = not supplied; AutoExposure falls back to `hdrSrv`.
    uint64_t meterSrv    = 0;
    uint32_t meterWidth  = 0;
    uint32_t meterHeight = 0;

    // Hardware scene depth SRV (reverse-Z, render resolution) + camera planes, set
    // by the Renderer. Read by depth-aware effects (Depth of Field, LensFlare).
    uint64_t depthSrv   = 0;
    // GBuffer metadata for weighted exposure metering. Both stay at render
    // resolution under an upscaler; consumers sample them by normalized UV.
    uint64_t normalSrv  = 0;
    uint64_t materialSrv = 0;
    // Dimensions of normalSrv / depthSrv, and the element count of materialSrv,
    // supplied so consumers do not pay a per-pixel GetDimensions for
    // dispatch-uniform values.
    uint32_t metaWidth     = 0;
    uint32_t metaHeight    = 0;
    uint32_t materialCount = 0;
    float    cameraNear = 0.1f;
    float    cameraFar  = 1000.0f;

    // ---- The Resolve stage's inputs ----
    // The scene depth and velocity textures (compute-readable by now), which the
    // upscalers take, and the SRVs TAA reads beside depthSrv.
    const RHI::Texture* depthTex    = nullptr;
    const RHI::Texture* velocityTex = nullptr;
    uint64_t surfaceSrv  = 0;
    uint64_t velocitySrv = 0;
    uint64_t stencilSrv  = 0;   // the depth's stencil plane; 0 = the format has none
    // The frame plan: which upscaler is selected (fsr / dlss) and which really
    // resolves to the display size (fsrResolved / dlssResolved). Non-owning,
    // read-only.
    const FramePlan::Decisions* plan = nullptr;
    // The main graph and its textures' handles, for an effect that borrows one of
    // them outside the graph - DebugWire reads depth as a pixel-shader SRV and
    // records the state change in the graph's tracker (FeatureRecordContext carries
    // the same pair). Non-owning.
    RG::RenderGraph*     graph    = nullptr;
    const FrameTextures* textures = nullptr;
    // NPRStabilize replaced the running image this frame (RRColorAlign aligns
    // the colour before transparency only then).
    bool stabilized   = false;
    // FSR 2, DLSS and TAA are one choice: the first enabled one records the
    // frame's resolve and sets this, the others then decline - TAA stays the
    // fallback of an upscaler that could not run.
    bool resolveTaken = false;

    // What effects leave for later ones.
    PostResources resources;

    // The frame the chain belongs to, read-only: its view (the camera, the
    // render and viewport sizes) and the frame resources the Renderer filled
    // before PreResolve (the sun, the moon, the clouds' transmittance ...).
    // Non-owning; valid for the duration of PostChain::Execute().
    const FrameView*      view  = nullptr;
    const FrameResources* frame = nullptr;

    // The frame's fully-resolved post-process settings - produced by the volume /
    // profile resolve (PostProcessResolveSystem), consumed by the adapters.
    // Non-owning; valid for the duration of PostChain::Execute().
    const ResolvedPostProcessSettings* resolved = nullptr;
    // Camera-primary PT already integrates the lens; never blur it a second time.
    bool skipDepthOfField = false;
    // The path tracer's time freeze holds the world clock (Renderer::BeginWorldClock); AutoExposure holds its
    // exposure with it.
    bool worldFrozen = false;
    // Every mesh and material texture the scene asked for has arrived: AutoExposure holds a pending snap
    // until then, so it does not snap to a half-streamed picture.
    bool sceneSettled = true;

    // ---- Shared HDR ping-pong pool (owned by the Renderer) ----
    // The chain is linear and every producer FULLY overwrites its target, so however
    // many effects run, only TWO full-res buffers are ever live: the one being read
    // and the one being written. Two RGBA16F targets instead of one per producer
    // (32 MB vs 79 MB at 1080p for five producers).
    // Null = every pass uses its own private target. Their states are in the
    // device's state book: a producer moves its slot with RG::TransitionTo, so no
    // state travels with the pointer.
    const RHI::Texture* poolTex[2]   = { nullptr, nullptr };
    // Advanced by PostChain::Execute; producers take poolIndex, then it flips.
    int                 poolIndex    = 0;
};

class IEffect
{
public:
    virtual ~IEffect() = default;

    // Where it runs: a post stage (RG::Stage::Resolve ... Overlay) and its
    // place within it, lower first (ties by name).
    virtual RG::Stage   GetStage() const = 0;
    virtual int         Order()    const = 0;
    virtual const char* GetName()  const = 0;

    // Zero-cost skip gate: false when the effect should not run this frame
    // (disabled, weight == 0, inputs unavailable). The chain then records no commands
    // and leaves the Context untouched.
    virtual bool IsEnabled(const Context& ctx) const = 0;

    // Record the effect's commands onto ctx.cl (a compute list, except on the
    // Overlay stage or when an effect RequiresGraphicsQueue) and, when the effect
    // produces output, update the Context slot downstream readers use.
    virtual void Execute(Context& ctx) = 0;

    // Producers that write a full-res HDR target override this to take a pooled
    // buffer instead of allocating their own. Default no-op: effects that produce
    // nothing (AutoExposure) or own a differently-shaped target (Bloom's mip chain,
    // ToneMap's LDR output) are unaffected.
    virtual void SetSharedOutput(const RHI::Texture* /*tex*/) {}

    // True only for effects that consume a pool slot, so the chain advances the
    // ping-pong exactly once per real producer.
    virtual bool UsesSharedOutput() const { return false; }

    // GPU profiler regions. false: the chain opens "PostProcess.<name>" around
    // Execute; a pass's own region nests inside (AutoExposure's does). true: Execute
    // opens its own, under the pass's established name ("NPRStabilize", "TAA",
    // "DLSS" / "DLSS.RR", "GlassShatter" ...).
    virtual bool OwnsRegions() const { return false; }

    // The Resolve stage's output. After the stage the chain's image
    // (Context::hdrSrv) is the SRV of the LAST effect of the stage that claims the
    // frame, or the stage's input when none does. Asked of every effect of the stage,
    // run or not: an upscaler the frame plan resolves through hands its output over
    // even in a frame it could not run (the pool was made at its size).
    // ResolveEffects.h has the rules.
    virtual bool ClaimsResolve(const Context& /*ctx*/, uint64_t& /*srv*/) const { return false; }

    // True when this frame's Execute must record on the graphics queue (DLSS:
    // Streamline's own barriers target pixel-shader states, illegal on a compute
    // list). Asked on the Renderer's thread before the chain's list opens; the list
    // is graphics if any effect asks.
    virtual bool RequiresGraphicsQueue(const Context& /*ctx*/) const { return false; }

    // True when it runs in a secondary view: everything per view it reads or writes is the view's
    // (its pass's history and output follow the bound view). One that is not is skipped there.
    virtual bool ViewSafe() const { return false; }
};

} // namespace PostProcess
