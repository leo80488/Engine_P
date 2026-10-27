#pragma once

// The hooks every render pass shares, in the render graph or out of it. RG::RenderPass
// derives from it, and so does every pass the Renderer drives itself (the post chain, the
// upscalers, the guides, the VFX simulations).
//
// Events (Renderer/RenderLifecycle.h): a pass that reacts to one overrides its hook AND names
// it in HandledEvents(). The Renderer's PassSet builds the pass's lifecycle entry from that
// mask (from a base pointer C++ cannot tell whether a virtual was overridden), so the
// [LIFECYCLE] lines list exactly the passes that react. A graph pass reloads through the
// graph's own walk (Renderer::ReloadShaders), so it never names kReloadShaders.
// OnPrepare: the device frame's prepare points (PrepareStage), on the main thread before the
// graph's and the post chain's workers fork: (re)create or release what the pass records into
// this frame, never WaitIdle. A pass is called once per stage the Renderer's order table lists
// it in (Renderer::RegisterPrepareOrder), in that order; the HiZ stage runs at Phase 4.5 on
// the render lane, where the pyramid records.
// OnService: the frame boundary's service point (Renderer::ServiceEnableGatedResources, the
// game half, before BeginFrame). The Renderer fills the pass's request from its settings and
// the frame's plan (a pass may not depend on the Renderer); the pass applies it in its own
// order and may create, release and WaitIdle. The Renderer's own steps sit between the stages
// (ServiceStage); a fact one pass establishes for a later one goes on the context's board.
// BindView: the view whose cross-frame state the pass keeps (RenderGraph/ViewState.h). A pass
// with a history names kView; PassSet hands it the view as it registers (main thread,
// Renderer::Compile) and the pass creates its history object there. Hard cuts, resizes and
// camera cuts are not events: they invalidate the view's history (ViewState::Invalidate, each
// ViewHistory::Invalidate), as the world clear does for the history.

#include <cstdint>

class IGraphicsDevice;
class ViewState;
namespace RHI
{
struct Texture;
enum class ResourceState : uint32_t;
}

namespace RG
{
// Where in the device frame a prepare runs (RenderAsync).
enum class PrepareStage : uint8_t
{
    FrameEarly,    // before the frame's SRV capture: XeGTAO, the RT reflection passes
    FrameLate,     // just before the graph forks: SurfaceCacheBake, NPR's lit snapshot
    HiZ,           // Phase 4.5, the frames the Hi-Z pyramid generates: its texture (the device's render size)
    PostAA,        // Phase 5: the AA passes
    PostPool,      // Phase 5, after the post pool: the pooled producers learn of it
    PostEffects,   // Phase 5: the post chain's working sets
    PostOutput,    // Phase 5: ToneMap's output mode and composition buffer
    Count
};
inline const char* PrepareStageName(PrepareStage s)
{
    switch (s)
    {
    case PrepareStage::FrameEarly:  return "frameEarly";
    case PrepareStage::FrameLate:   return "frameLate";
    case PrepareStage::HiZ:         return "hiZ";
    case PrepareStage::PostAA:      return "postAA";
    case PrepareStage::PostPool:    return "postPool";
    case PrepareStage::PostEffects: return "postEffects";
    case PrepareStage::PostOutput:  return "postOutput";
    case PrepareStage::Count:       break;
    }
    return "?";
}

// Where in the service point a service runs (each stage has its order table).
enum class ServiceStage : uint8_t
{
    Retry,         // first: DLSS re-arms a failed RR evaluate (the plan's resolve reads its health)
    Upscalers,     // FSR 2 / DLSS's context while the game plan selects it, the other released - before
                   // the plan's resolve, whose post size and RR read whether the context exists
    DirectLight,   // the path tracer's x1 direct light (the Renderer decides ptWantsDI from it)
    Restir,        // ReSTIR DI / GI and their compare shadows
    PathTrace,     // the path tracer (after the Renderer's GI emitter step)
    StaticGeometry,  // the static BLAS set came complete (dispatched only then): the SHaRC caches forget
    Lights,        // Lights v2's history rings (after the passes above: descriptor indices), light sampling
    RRReset,       // RR's history reset when a ReSTIR pass that will run resets its own
    RTReflectionTier,  // the RT reflection passes' hardware tier (its PSO, first wanted frame)
    WorkingSets,   // the switches' working sets: RT reflections' RR mode, RR's colour snapshot, the reactive
                   // mask, RTAO, SSGI, RT shadows, the debug target view (then the Renderer's texture / UI views)
    Residents,     // fog's froxels, the star cube, the group-D post residents, the shadow atlases
    Count
};
inline const char* ServiceStageName(ServiceStage s)
{
    switch (s)
    {
    case ServiceStage::Retry:       return "retry";
    case ServiceStage::Upscalers:   return "upscalers";
    case ServiceStage::DirectLight: return "directLight";
    case ServiceStage::Restir:      return "restir";
    case ServiceStage::PathTrace:   return "pathTrace";
    case ServiceStage::StaticGeometry: return "staticGeometry";
    case ServiceStage::Lights:      return "lights";
    case ServiceStage::RRReset:     return "rrReset";
    case ServiceStage::RTReflectionTier: return "rtReflectionTier";
    case ServiceStage::WorkingSets: return "workingSets";
    case ServiceStage::Residents:   return "residents";
    case ServiceStage::Count:       break;
    }
    return "?";
}

// What earlier passes of one service point tell later ones.
struct ServiceBoard
{
    bool diEngineRuns   = false;   // the main DI pass runs the engine core (its compare shadow runs the other)
    bool giEngineRuns   = false;   // likewise GI
    bool diLightTables  = false;   // a DI pass that will run draws RTXDI candidates (LightSampling's tables)
    bool diHistoryReset = false;   // the DI pass that will run resets its history (RR resets with it)
    bool giHistoryReset = false;
};

// The service point's context: one per frame, through every stage.
struct ServiceContext
{
    ServiceStage     stage = ServiceStage::Retry;
    IGraphicsDevice* gfx = nullptr;
    ServiceBoard     board;
};

// What a prepare reads. The Renderer fills what the stage has: the sizes
// always; the post sizes, the AA config and the pool from Phase 5 on.
struct PrepareContext
{
    PrepareStage     stage = PrepareStage::FrameEarly;
    IGraphicsDevice* gfx = nullptr;
    uint32_t renderW = 0, renderH = 0;       // the render (HDR) size
    uint32_t viewportW = 0, viewportH = 0;   // the display viewport
    uint32_t postW = 0, postH = 0;           // the post chain's: render, or display once an upscaler resolved
    bool ssao = false;                       // SSAO is on at all (XeGTAO is RTAO's same-frame fallback)
    bool taa = false, fxaa = false;          // the AA config can run them this frame (no upscaler resolve)
    bool upscaler = false;                   // FSR 2 or DLSS is the plan's upscaler on this (the main) view
    // The shared post target (slot 0), when the pool exists. Its state is the device's book's
    // (RG::TransitionTo).
    const RHI::Texture* postPool = nullptr;
};
// Renderer::OnWorldClear's arguments (RenderLifecycle::WorldClearArgs).
struct WorldClearArgs
{
    bool idsPreserved  = false;   // entity ids survive (the editor's Play / Stop)
    bool worldSurvives = false;   // nothing was destroyed (the collision-baker restore)
};

class PassLifecycle
{
public:
    enum Event : uint32_t
    {
        kShutdown      = 1u << 0,   // ~Renderer, after the GPU drain: release what the pass created
        kReloadShaders = 1u << 1,   // Renderer::ReloadShaders, before the main graph's walk
        kWorldClear    = 1u << 2,   // Renderer::OnWorldClear (runs mid-frame: set flags, free what is owned) -
                                    // the world's state; the view's history hears it through ViewHistory::Invalidate
        kView          = 1u << 3,   // BindView: the pass keeps a history in the view (not an event)
        kViewSafe      = 1u << 4,   // the pass records any view: everything per view it keeps is in the
                                    // view (ViewState::History / Data) or a FrameCB; a graph pass without
                                    // it is skipped in a secondary view (not an event)
    };

    virtual ~PassLifecycle() = default;

    // The events this pass reacts to (the hooks below it overrides).
    virtual uint32_t HandledEvents() const { return 0; }

    virtual void Shutdown(IGraphicsDevice& /*gfx*/) {}
    virtual void ReloadShaders(IGraphicsDevice& /*gfx*/) {}
    virtual void OnWorldClear(const WorldClearArgs& /*args*/) {}

    // The view this pass renders and keeps its history in (kView): the pass creates its history
    // object here - empty; its GPU resources are made where the pass makes them - and keeps a
    // pointer to it. Called for the main view as the pass registers (Renderer::Compile), and
    // again before every other view records and after it (PassSet::BindView): each call points
    // the pass at that view's objects (ViewState::History / Data), so it must be idempotent.
    virtual void BindView(ViewState& /*view*/) {}

    // The device frame's prepare: @p ctx.stage says which point.
    virtual void OnPrepare(const PrepareContext& /*ctx*/) {}

    // The frame boundary's service: @p ctx.stage says which stage.
    virtual void OnService(ServiceContext& /*ctx*/) {}
};
} // namespace RG
