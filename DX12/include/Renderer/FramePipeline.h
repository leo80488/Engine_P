#pragma once
// FramePipeline - the frame executor App drives: the render-owner lane and its
// quiescence protocol, the drains, the render halves (RenderHalf -> RenderHalfAsync),
// the game -> render packet and the render -> game timing mailbox, the barriers that
// close the overlap window, the setup ladder (Configure) and the frame's timing
// publication.
//
// THREADS. Main owns this object and makes every public call. The lane (-renderowner)
// runs RenderOwnerLoop and RenderHalfAsync; the park hook may fire from any thread that
// touches a device resource. The member comments below say who may touch what.
//
// EngineCore is compiled once (CLAUDE.md invariant 1): the editor's part of the frame is
// EditorHooks, which Editor.exe installs (Editor/EditorFrameHooks.h) and Game.exe does
// not; every editor-only path asks whether hooks are installed.

#include "Graphics/GraphicsStruct.h"   // RHI::CommandList (EditorHooks)
#include "System/ExecutionDomain.h"
#include "System/OwnerTask.h"
#include "System/OwnerTaskQueue.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

class IGraphicsDevice;
class Renderer;
class World;
class Scheduler;
struct RenderView;
struct GameModeContext;
struct FrameContext;
namespace Harness { class HarnessCaptureSystem; }
namespace Sys { class FrameTaskContext; }

class FramePipeline
{
public:
    // ---- the editor's part of the frame ----
    // Implemented by EditorCore over EditorLayer (Editor/EditorFrameHooks.h). ImGui
    // is main-thread bound: every call is made on main, except EndUIFrame /
    // EndUIFrameAsync, which only RECORD the frozen snapshot on the render half.
    struct EditorHooks
    {
        // What BuildUI receives for the scene texture before the frame has one
        // (EditorLayer::FrameSceneTexturePlaceholder; asserted equal there).
        static constexpr uint64_t kSceneTexturePlaceholder = ~uint64_t{0} - 1;

        virtual ~EditorHooks() = default;

        // -renderowner: the lane only records the snapshot; main gives ImGui its
        // buffers back when it next drains (CompleteDeferredSnapshot).
        virtual void DeferSnapshotCompletion() = 0;
        // Build the UI on main: the ImGui frame, OnUIRender, the Phase Debug window.
        // @p view = the camera gizmos and picking use; null = the renderer's FrameView.
        virtual void BuildUI(IGraphicsDevice& backend, Renderer& renderer, World* world,
                             uint64_t sceneTexture, const RenderView* view) = 0;
        // Freeze ImGui's draw data for @p frame. false = the frame must not straddle
        // the seam (an ImGui draw callback).
        virtual bool PrepareSnapshot(uint64_t frame) = 0;
        // After the lane recorded the snapshot; @p frameLost = no frame will ever be
        // posted for a snapshot the lane has not taken (the drain is unwinding, or Quit).
        virtual void CompleteDeferredSnapshot(bool frameLost) = 0;
        // Drop a frozen snapshot (the main-thread path's drain, a failed frame).
        virtual void DiscardSnapshot() = 0;
        // The render half's composition: record the UI into @p cl - serially (no graph)
        // or as the chain's EditorImGui.CommitSnapshot segment (graph modes).
        virtual void EndUIFrame(RHI::CommandList cl) = 0;
        virtual Sys::OwnerTask EndUIFrameAsync(RHI::CommandList cl, uint64_t frame,
                                               uint64_t sceneTexture) = 0;
    };

    // @p device is the one App's Window owns; @p world and @p scheduler are App's.
    // Construct it on main (the thread that will run the loop).
    FramePipeline(IGraphicsDevice& device, World& world, Scheduler& scheduler);
    ~FramePipeline();
    FramePipeline(const FramePipeline&) = delete;
    FramePipeline& operator=(const FramePipeline&) = delete;

    // Before Configure (which tells the editor to defer snapshot completion).
    void SetEditorHooks(EditorHooks* hooks) { m_editor = hooks; }
    // The registered HarnessCaptureSystem: RenderHalf services a due -ui_screenshot
    // BEFORE it posts the frame to the lane (null without the harness).
    void SetHarnessCapture(Harness::HarnessCaptureSystem* capture) { m_harnessCapture = capture; }
    // Answers the window's messages while main waits for the render-owner lane (the main thread's
    // window would otherwise be "not responding" through a long drain: a world reload, a swap-chain
    // rebuild, a hitching frame). Returns false to stop being called (a WM_QUIT is pending). Main only.
    void SetWaitPump(std::function<bool()> pump) { m_waitPump = std::move(pump); }

    // Boot: -rendermode, the device-thread park hook, -renderowner, -animoverlap,
    // -brslane, -brsplanlane, the destroy deferral and the access tooling, each with
    // its refusals and [SPLITCHECK] / [FRAME_EXECUTOR] lines.
    void Configure(IGraphicsDevice& backend, Renderer& renderer);

    // ---- the drains ----
    // @p cause is what the drain is charged to (Diag::DrainCause in
    // Diagnostics/OwnerAccounting.h; {} = Other). Main only.
    void DrainFrameWork(Diag::DrainCause cause = Diag::DrainCause{});
    void PumpFrameWork();

    // ---- the loop, in the order App::Run calls these ----
    void LogGameBegin(uint64_t frame);               // [FRAME_GAME_BEGIN] (-materialcpuverify)
    void CompatibilityBoundary();                    // the loop-top drain / pump
    void BeginFrameBracket();                        // with cpuFrameMs: reset this bracket's sums
    void AnimationBoundaryBarrier();                 // the unmoved barrier (a no-op under -animoverlap)
    // The game half claims frame ctx.frame's upload slot (async where it can), with the
    // animation visibility prepass beside the GPU's retirement of the slot.
    void AcquireUploadSlot(IGraphicsDevice& backend, Renderer& renderer, const GameModeContext& ctx);

    // ---- the Render phase (App::RenderPhase), in order ----
    void RenderPhaseBarrier();                       // the hard-sync drain (the moved barrier)
    void VerifyAnimOverlap(IGraphicsDevice& backend, Renderer& renderer, const FrameContext& frameCtx);
    void BuildEarlyEditorUI(IGraphicsDevice& backend, Renderer& renderer, World& world,
                            const GameModeContext& ctx);   // before Renderer::BeginFrame; no-op unless m_editorUiEarly
    // The frame's preparation context on main's owner queue (none under -brslane).
    void EmplaceFrameTaskContext(std::optional<Sys::FrameTaskContext>& cpuFrame,
                                 IGraphicsDevice& backend, const GameModeContext& ctx);
    // GAME -> RENDER: the game half's share of drawMs / renderPhaseMs, which travels in
    // the packet (Renderer::BeginFrame, then FinishFramePreparation).
    void SetGameHalfMs(double ms) { m_renderSplitRBeginThisFrame = ms; }
    void AddGameHalfMs(double ms) { m_renderSplitRBeginThisFrame += ms; }
    void LogPrepareOwnerDone(Renderer& renderer, const GameModeContext& ctx, bool mainHasContext);
    // Publish the frame transaction (the packet, with its emit lease) and run RenderHalf.
    void SubmitFrame(IGraphicsDevice& backend, Renderer& renderer, const GameModeContext& ctx);

    // ---- after the Render phase ----
    // The frame-timing snapshot (Diag::FrameTiming) and the per-frame verify lines.
    void PublishFrameTiming(IGraphicsDevice& backend, Renderer& renderer, float realDt,
                            float loopTopMs, uint64_t frameIndex);
    void LogPhaseCpu();                              // -phaselog_every=N
    // The quit path: game frames vs presented frames ([SPLITAUDIT] / [SPLITCHECK]).
    void ReportExitFrameCounts(IGraphicsDevice& backend, const GameModeContext& ctx);
    // [DRAIN] per-cause drain counts as name=calls/blocking/blockingMs: since the last
    // window line (@p window, the timing publication) or since boot. Main only.
    static void LogDrainStats(const char* tag, bool window, uint64_t frames);

    // ---- queries ----
    bool RenderOwnerEnabled() const { return m_renderOwnerEnabled; }
    // For a reader that keeps a reference (Editor::PreviewHarness). Written by Configure
    // before the lane starts and by StopRenderOwner after it is joined.
    const bool& RenderOwnerEnabledFlag() const { return m_renderOwnerEnabled; }
    bool RenderOwnerFrameInFlight();
    // Main's owner queue (the async slot acquire, the frame preparation context).
    Sys::OwnerTaskQueue& OwnerQueue();

    // ---- shutdown (ShutdownAllSystems, in its order; ~App: Shutdown) ----
    void StopRenderOwner();
    void UnhookDevice(bool reportDrained);           // the park hook; [FRAME_EXECUTOR] drained=1
    void CloseQueues();                              // GPU completions, then the owner queue
    void Shutdown();                                 // all three, for ~App (idempotent)

private:
    // ---- [RENDERSPLIT] accumulators ----
    struct RenderSplitAccum
    {
        double beginMs  = 0.0;   // device BeginFrame, incl. the GPU slot wait
        double rBeginMs = 0.0;   // Renderer::BeginFrame (+ FinishFramePreparation), from the packet
        double renderMs = 0.0;   // renderer.Render
        double endMs    = 0.0;   // EndFrame / present
        double restMs   = 0.0;   // Render -> EndFrame (composition, UI record) + earlyUiMs
        int    frames   = 0;
        void Reset() { beginMs = rBeginMs = renderMs = endMs = restMs = 0.0; }
    };

    // ---- the game/render seam ----
    struct FramePacket
    {
        uint64_t frame    = 0;   // which frame this describes
        double   rBeginMs = 0.0; // Renderer::BeginFrame, measured on the game half
        double   earlyUiMs = 0.0; // the editor UI build main did before the frame started
        World*   world    = nullptr; // editor-only: the World the editor UI build receives
        bool     uiPrepared = false; // UI built and frozen before the frame: no late build
        bool     allowOverlap = true;
        // This frame's Streamline token, taken on the game thread (SubmitFrame). Do not
        // re-read it from the game latch on the render half: that is the bug
        // [SPLITCHECK][SLTOKEN] detects, and a latch compared with itself cannot fire.
        void*    slToken  = nullptr;
        // ---- the frame's emit lease ----
        // What the emit path must not re-read from live renderer / device state, because
        // main moves it the moment it runs ahead of a lane still preparing frame F:
        //   uploadSlot   - the slot every mapped pointer in BuildScene_SortAndEmit
        //                  derives from.
        //   emitIdx      - which scene-payload buffer frame F owns.
        //   batchPlanGen - the ResolveBatchPlan generation the nest must be reading.
        // A packet is the only thing that crosses the seam by construction. The renderer
        // holds the same values; VerifyEmitLease checks they agree at the consume point.
        uint32_t uploadSlot   = 0;
        uint32_t emitIdx      = 0;
        uint32_t batchPlanGen = 0;
        //   sortGen      - the sort PERMUTATION the nest indexed through (a Renderer
        //                  member). A lane reading a stale or half-rebuilt one is silent:
        //                  no crash, no debug-layer message, just wrong draws.
        uint32_t sortGen      = 0;
        //   laneEmit     - does the LANE owe this frame's emit half? Per frame, not per
        //                  run: under -brslane a frame whose batch plan was refused has
        //                  already emitted on main (BuildScene_PlanEmit's fallback), so the
        //                  lane goes straight to RenderAsync and main owns the lease check.
        //                  Read off Renderer::LaneEmitOwed() at the hand-off, where the lane
        //                  is idle; false on every frame without -brslane.
        bool     laneEmit     = false;
    };

    // RENDER -> GAME (RenderToGame). Produced by the render half; main copies it to
    // m_completedRender once the frame is complete (at the next RenderHalf entry under frame
    // overlap, else in SubmitFrame), and PublishFrameTiming feeds that copy to the HUD,
    // FrameTiming's EMAs, Lua Debug.Stats and frames.jsonl. Packs assert on several fields.
    // Not in here, because they are game-to-game: gameMs, cpuFrameMs, frameMs and
    // everything GetFrameStats() returns (Renderer::BeginFrame runs on the game half).
    struct RenderToGame
    {
        // Pure render-half spans, ms.
        float  beginWaitMs = 0.f;   // device BeginFrame, incl. the GPU slot wait
        float  renderMs    = 0.f;   // renderer.Render
        float  restMs      = 0.f;
        float  presentMs   = 0.f;
        // Already composed with the frame's OWN game term from the FramePacket, so the
        // consumer never has to know a sum spans the seam.
        float  drawMs        = 0.f;
        float  renderPhaseMs = 0.f;
        // The render thread's own total: beginWait + render + rest + present, without the
        // game half's Renderer::BeginFrame term (and, under -renderowner, the UI build main
        // did) that renderPhaseMs carries. renderPhaseMs is the phase's cost: once the
        // halves run on different threads it is neither a per-thread total nor a
        // wall-clock span.
        float  renderThreadMs = 0.f;
        // GPU profiler, snapshotted where it is written rather than read
        // through GetGPUProfiler() from the wrong thread.
        bool     gpuValid      = false;
        bool     gpuCalibrated = false;
        float    gpuBusyMs     = 0.f;
        float    gpuOverlapMs  = 0.f;
        uint32_t gpuAgeFrames  = 0;
        // The join wait (no executor joins a render thread: always 0 and invalid) and the
        // resolved mode: 0 serial, 2 graph, 3 graph with frame overlap.
        float    joinWaitMs    = 0.f;
        bool     joinWaitValid = false;
        uint8_t  renderMode    = 0;
        uint64_t frame         = 0;
        bool valid             = false;
    };

    // ---- -renderowner: the second owner thread ----
    // Default on in graph mode on every host; -renderowneroff is the escape and
    // -rendermode=serial refuses it. Graph-overlap INTERLEAVES on one thread (span T >=
    // serial inventory S), so only a second owner moves render-owner ms out of main's
    // serial span. RenderHalf hands the FramePacket to this thread, which owns its OWN
    // OwnerTaskQueue (created in RenderOwnerLoop: the queue binds to its creator) and
    // runs the whole RenderHalfAsync chain; main returns to the game loop and blocks
    // again at the next drain.
    // The device frame thread moves with it: IssueDeviceExecution refuses any thread but
    // the one SetDeviceFrameThread names, so the lane issues and retires its own token
    // per frame with no ExecutionDomain::Scope open. The domain has ONE shared Gate:
    // main must never hold a scope while a frame is in flight, which is why the editor
    // UI is built on main, scope-free, while the lane is idle.
    struct RenderOwnerLane
    {
        RenderOwnerLane();
        ~RenderOwnerLane();
        RenderOwnerLane(const RenderOwnerLane&) = delete;
        RenderOwnerLane& operator=(const RenderOwnerLane&) = delete;

        // A manual-reset Win32 event, set while no frame is in flight (inFlight false) and reset while
        // one is: main waits on it together with its window's messages (WaitForLaneFrame).
        void*                       doneEvent = nullptr;
        std::thread                 thread;
        std::mutex                  mutex;
        std::condition_variable     job;   // main -> lane: a packet is waiting
        std::condition_variable     done;  // lane -> main: the frame is published
        bool                        shutdown = false;
        bool                        posted   = false; // packet queued, not yet taken
        bool                        inFlight = false; // queued or running
        bool                        ready    = false; // the lane reached its loop
        FramePacket                 packet{};
        std::exception_ptr          error;
        // Set by the lane before `ready`; StartRenderOwner republishes it as
        // m_renderOwnerThreadId, which is what the park hook reads.
        std::atomic<std::thread::id> id{};
        uint64_t frames         = 0;  // frames the lane completed
        uint64_t waits          = 0;  // DrainRenderOwner calls on main
        uint64_t blockingWaits  = 0;  // ...that actually had to sleep
        uint64_t scopeConflicts = 0;  // "Another frame owns this execution domain"
        // parkSkips does not live here: the last park-hook fires arrive from
        // ~OwnerTaskQueue's CloseAndDrain after StopRenderOwner has retired the pointer, so
        // the hook must count without touching lane memory (m_renderOwnerParkSkips).
    };

    // ---- -animoverlap ----
    // Moves the Animation-boundary barrier (AnimationBoundaryBarrier, right after
    // TickPhase::PhysicsInterpolation) DOWN to the drain that already opens
    // TickPhase::Render (RenderPhaseBarrier), so Animation / BoneAttachment /
    // SecondaryPhysics / PreRender / SetCamera of frame F run beside the lane's render
    // of F-1. No barrier is removed: the Render-phase drain is the hard-sync point
    // resize, world teardown and the editor's pending actions rely on, and
    // Renderer::BeginFrame and everything under it still run with the lane quiescent.
    // The default is this constant plus the host-aware terms Configure applies;
    // -animoverlapoff is the escape. Alone it is about neutral; it is on because
    // -brslane needs it (the unmoved barrier drains the lane after BeginFrame(F+1)),
    // and the pair is the measured CPU win. -brslane's default asks for the moved
    // barrier, so turning this off turns the lane emit off too.
    static constexpr bool kAnimOverlapDefault = true;

    // ---- -sltokenlag ----
    // The -sltokenlag ring only has to outlive the largest lag worth testing.
    static constexpr int kSlTokenRing = 8;

    // ---- the game/render seam ----
    // One retained render transaction may overlap the next game's simulation.
    // Slot reuse, animation preparation and resource mutations drain its lease.
    void RenderHalf(IGraphicsDevice& backend, Renderer& renderer, const FramePacket& pkt);
    // Does @p pkt describe the frame the emit path actually built? Called where the
    // packet is CONSUMED, so it compares a value snapshotted at the hand-off with one
    // the emit path latched - never a value with a copy of itself.
    // -emitleaseselftest=<frame> feeds it a corrupted packet on that frame and asserts
    // it says no.
    void VerifyEmitLease(Renderer& renderer, const FramePacket& pkt);
    Sys::OwnerTask RenderHalfAsync(IGraphicsDevice& backend, Renderer& renderer, FramePacket packet,
        Sys::OwnerTaskQueue& owner, std::optional<Sys::ExecutionDomain::Token> token);

    // ---- -renderowner: the second owner thread ----
    void StartRenderOwner(IGraphicsDevice& backend, Renderer& renderer);
    void RenderOwnerLoop(IGraphicsDevice& backend, Renderer& renderer);
    void PostRenderOwnerFrame(const FramePacket& packet);
    void DrainRenderOwner(RenderOwnerLane& lane, Diag::DrainCause cause);

    // ---- Core state ----
    IGraphicsDevice& m_device;
    World&           m_world;
    Scheduler&       m_scheduler;
    EditorHooks*     m_editor = nullptr;
    // The park-hook count when this frame's CPU bracket opened (BeginFrameBracket).
    uint32_t         m_parkHookAtBracketOpen = 0;
    std::unique_ptr<Sys::OwnerTaskQueue> m_frameOwnerQueue;

    // ---- [RENDERSPLIT] accumulators ----
    // BUSY vs BLOCKED for the Render phase, under -phaselog_every. backend.BeginFrame
    // does a pipelined wait on the GPU fence, so without the split a GPU-bound frame
    // lands its whole wait on "RenderSystem" in the phase table, indistinguishable from
    // CPU work.
    // RENDER HALF ONLY, every field: the render half (main or the lane) accumulates,
    // prints and resets them; rBeginMs, measured on the game half, arrives in the
    // FramePacket.
    RenderSplitAccum m_renderSplit;

    // ---- the game/render seam ----
    // Everything that crosses the seam travels in a FramePacket (GAME -> RENDER) or a
    // RenderToGame (RENDER -> GAME). A value read across the seam any other way
    // silently reads whatever the other half last left.

    // GAME -> RENDER: the game half's share of drawMs / renderPhaseMs (Renderer::BeginFrame
    // + FinishFramePreparation). Those rows are sums that span both halves, so the term
    // must travel WITH its frame: it is parked here until SubmitFrame builds the packet.
    // Main only; it never outlives the iteration that writes it.
    double m_renderSplitRBeginThisFrame = 0.0;

    RenderToGame m_fromRender; // Owner writer; read only after frame completion.
    RenderToGame m_completedRender; // Main-only stable timing snapshot.

    // Bracket-local blocking-drain accounting, reset when cpuFrameMs opens.
    // Plain doubles: main-thread only, and they never outlive the iteration.
    double m_tailDrainMsThisFrame     = 0.0; // the Animation-boundary drain, moved or not
    double m_blockingDrainMsThisFrame = 0.0; // OUTERMOST blocking DrainFrameWork brackets only
    // DrainFrameWork re-entrancy: GraphicsDX12::DrainFrameIfNeeded fires the park hook
    // (which IS DrainFrameWork) from inside a continuation an outer RunUntil is running,
    // while m_activeFrameOwner is still set. The nested wall time lies inside its
    // parent's, so only depth 0 charges m_blockingDrainMsThisFrame; the nested time is
    // kept apart so [OWNERTIME] shows that the path fired.
    // The overlap window G - post(F-1) to the barrier of F - is measured on main (two
    // steady_clock reads per frame) and reported as winMs= on [OWNERTIME] and
    // [ANIM_OVERLAP]; 0 without the lane, where there is no window.
    std::chrono::steady_clock::time_point m_renderOwnerPostAt{};
    double   m_overlapWindowMsThisFrame = 0.0;
    int      m_drainDepth              = 0;   // 0 outside DrainFrameWork
    double   m_nestedDrainMsThisFrame  = 0.0; // depth >= 1 brackets, diagnostic only
    uint32_t m_nestedDrainsThisFrame   = 0;   // how many nested brackets this frame
    Sys::OwnerTaskQueue* m_activeFrameOwner = nullptr; // owner-only; valid until DrainFrameWork
    Sys::TaskHandle m_renderFrameCompletion;
    std::optional<Sys::ExecutionDomain::Token> m_renderFrameToken;
    bool m_renderRecordingReady = false;
    bool m_frameOverlap = false;
    std::thread::id m_mainThreadId;
    bool m_renderGraph = true;

    // ---- -renderowner: the second owner thread ----
    // OWNERSHIP vs REACHABILITY. m_renderOwner OWNS the lane and is touched on main
    // only. Anything that can run on another thread - the park hook, and DrainFrameWork
    // re-entered through it - reaches the lane through m_renderOwnerLane, an atomic
    // publication of the same object:
    //   StartRenderOwner  construct -> thread -> ready -> publish pointer LAST
    //   StopRenderOwner   null the pointer FIRST, wait for m_renderOwnerHookDepth == 0,
    //                     then move / join / destroy
    // m_renderOwnerThreadId is a separate publication that lives until after the join:
    // ~OwnerTaskQueue's CloseAndDrain runs on the lane after the pointer is retired and
    // can still touch a device resource. Do not clear it earlier: that fire would reach
    // DrainFrameWork, which throws off-main inside a destructor (std::terminate).
    // Waits, on main, for the lane to publish the frame in flight; @p lk is the lane's mutex, held on
    // entry and on return. A frame is normally a few ms and the wait is the condition variable's; past
    // kPumpAfter it becomes a wait on the lane's event AND the window's messages, pumped by m_waitPump.
    void WaitForLaneFrame(RenderOwnerLane& lane, std::unique_lock<std::mutex>& lk);
    std::function<bool()>             m_waitPump;                 // main only
    std::unique_ptr<RenderOwnerLane>  m_renderOwner;              // main only
    std::atomic<RenderOwnerLane*>     m_renderOwnerLane{ nullptr };
    std::atomic<std::thread::id>      m_renderOwnerThreadId{};
    std::atomic<int>                  m_renderOwnerHookDepth{ 0 };
    std::atomic<uint64_t>             m_renderOwnerParkSkips{ 0 };
    bool m_renderOwnerEnabled = false;
    // Set by SetHarnessCapture: RenderHalf services a due -ui_screenshot before the
    // post, because the lane's EndFrame is what records the copy.
    Harness::HarnessCaptureSystem* m_harnessCapture = nullptr;
    // -renderownerverify=N cadence, read once in Configure. This is the MAIN half of
    // that oracle (PublishFrameTiming reads Diag::OwnerAccounting); the lane half lives
    // in RenderOwnerLoop. Armed by the flag alone, not by the lane starting, so the
    // serial path (main is writer and reader) runs the same assertions and reports
    // torn=0.
    int m_renderOwnerVerifyEvery = 0;

    // ---- -animoverlap ----
    // Configure's answer: the barrier is moved this run (false until Configure runs).
    bool m_animOverlapEnabled = false;
    // -animoverlapverify[=N]: emit [ANIM_OVERLAP] every N frames at the moved
    // barrier, where the lane is provably quiescent and both slots are stable.
    int  m_animOverlapVerifyEvery = 0;
    // -brslane: the emit half of BuildRenderScene, its material transaction and the
    // frame's publish run on the render-owner lane. The pipeline needs its own copy of
    // the ladder's answer for two decisions: not to create main's FrameTaskContext (the
    // lane makes its own, on its own queue), and to run Renderer::PrepareFrameOnLane
    // before RenderHalfAsync. Configure sets this and Renderer::SetBrsLaneEnabled from
    // the same answer so they cannot drift. The overlap main loses is measured on the
    // lane: [BRSLANE_PREPARE] prepareMs= (RenderOwnerLoop).
    bool m_brsLaneEnabled = false;
    // With the emit half on the lane, the editor builds its UI at the top of
    // TickPhase::Render (BuildEarlyEditorUI) - after the drain, ResolvePick and
    // ProcessPendingActions, before Renderer::BeginFrame - so every renderer write the
    // UI makes lands before this frame's scene build on main and before the lane's plan
    // + nest. RenderHalf then posts that snapshot. Off for a lane-off or -brslaneoff
    // editor (RenderHalf builds the UI late), and -editoruilate turns it off (which
    // refuses the editor's lane emit).
    bool   m_editorUiEarly       = false;
    bool   m_earlyUiBuilt        = false;   // this frame's UI is built and frozen
    bool   m_earlyUiAllowOverlap = true;
    double m_earlyUiMs           = 0.0;
    // The wait at the MOVED barrier, in ms. Charged into tailDrainMs exactly once (the
    // unmoved site is a no-op then), so [OWNERTIME] tailMs means "how long main waited
    // for the lane" under both flag states.
    double m_animBarrierWaitMsThisFrame = 0.0;
    // Written by the lane, never read by main: the recording-ready latch only lets main
    // stop pumping, and main pumps no render work when the lane owns it.
    std::atomic<bool> m_renderOwnerRecordingReady{ false };

    // -sltokenlag=N: publish an N-frames-stale Streamline token at the seam, so
    // the [SPLITCHECK][SLTOKEN] oracle has a way to be proven red. 0 in every
    // normal run.
    uint64_t         m_slTokenLag = 0;
    void*            m_slTokenRing[kSlTokenRing] = {};
};
