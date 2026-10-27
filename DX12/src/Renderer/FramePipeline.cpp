// FramePipeline - the frame executor App drives (see FramePipeline.h): the
// render-owner lane and its quiescence protocol, the drains, the render halves, the
// game -> render packet and the render -> game timing mailbox, the overlap barriers,
// the setup ladder (Configure) and the frame's timing publication.
//
// EngineCore is compiled once (CLAUDE.md invariant 1): what only the editor does goes
// through FramePipeline::EditorHooks, which Editor.exe installs and Game.exe does not.

#include "Renderer/FramePipeline.h"

#include "Renderer/Renderer.h"
#include "Graphics/AnimProfile.h"
#include "Graphics/IDeviceFrameControl.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/StreamlineBackend.h"
#include "Diagnostics/FrameTiming.h"
#include "Diagnostics/OwnerAccounting.h"
#include "Diagnostics/Profiler.h"
#include "ECS/AccessGuard.h"
#include "ECS/ECS.h"
#include "ECS/FrameContext.h"
#include "ECS/Scheduler.h"
#include "Harness/HarnessCaptureSystem.h"
#include "Harness/RunLedger.h"
#include "Platform/MainWindow.h"         // -renderowner_stall posts the window a message
#include "Scene/GameModeContext.h"
#include "System/Check.h"
#include "System/CommandLine.h"
#include "System/FrameTaskContext.h"
#include "System/HostPolicy.h"
#include "System/JobDefaults.h"
#include "System/LockStats.h"
#include "System/Log.h"
#include "System/TaskSystem.h"
#include "System/ThreadContext.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>

FramePipeline::FramePipeline(IGraphicsDevice& device, World& world, Scheduler& scheduler)
    : m_device(device), m_world(world), m_scheduler(scheduler),
      // Main: App constructs this on the thread that runs App::Run. Recorded here, not in
      // Configure, so DrainFrameWork (which refuses every other thread) serves main from
      // the first boot chunk on, before the boot scripts and -exec's top level.
      m_mainThreadId(std::this_thread::get_id())
{
}

FramePipeline::~FramePipeline()
{
    // ~App already calls Shutdown (after ShutdownAllSystems); this is the fallback,
    // because destroying a joinable lane thread is std::terminate.
    try { Shutdown(); } catch (...) {}
}

namespace
{
// The device's frame threading, through its interface (GraphicsDX12 implements it).
// A device without frame coordination cannot run this executor.
IDeviceFrameControl& FrameControl(IGraphicsDevice& device)
{
    IDeviceFrameControl* control = device.FrameControl();
    if (!control) throw std::logic_error("FramePipeline: the device has no frame control");
    return *control;
}
} // namespace

static const float kClearColor[4] = { 0.05f, 0.05f, 0.1f, 1.0f };

FramePipeline::RenderOwnerLane::RenderOwnerLane()
    : doneEvent(::CreateEventW(nullptr, TRUE, TRUE, nullptr))   // manual-reset, signaled: no frame in flight
{
}

FramePipeline::RenderOwnerLane::~RenderOwnerLane()
{
    if (doneEvent)
        ::CloseHandle(doneEvent);
}

void FramePipeline::Shutdown()
{
    StopRenderOwner();
    FrameControl(m_device).SetFrameDrainHook(nullptr);
    CloseQueues();
}

void FramePipeline::UnhookDevice(bool reportDrained)
{
    IDeviceFrameControl& frameDevice = FrameControl(m_device);
    frameDevice.SetFrameDrainHook(nullptr);
    if (reportDrained)
        LOG_INFO("[FRAME_EXECUTOR] drained=1 uploadGame=%llu uploadRender=%llu",
            static_cast<unsigned long long>(frameDevice.UploadFlushesLifetime(0)),
            static_cast<unsigned long long>(frameDevice.UploadFlushesLifetime(1)));
}

void FramePipeline::CloseQueues()
{
    FrameControl(m_device).CloseGpuCompletions();
    if (m_frameOwnerQueue) m_frameOwnerQueue->CloseAndDrain();
}

Sys::OwnerTaskQueue& FramePipeline::OwnerQueue()
{
    if (!m_frameOwnerQueue) m_frameOwnerQueue = std::make_unique<Sys::OwnerTaskQueue>();
    return *m_frameOwnerQueue;
}

// Starts the lane (RenderOwnerLane, FramePipeline.h) and hands it the device frame
// thread; main keeps no ExecutionDomain::Scope while a frame is in flight.

void FramePipeline::StartRenderOwner(IGraphicsDevice& backend, Renderer& renderer)
{
    if (m_renderOwner) return;
    m_renderOwner = std::make_unique<RenderOwnerLane>();
    RenderOwnerLane& lane = *m_renderOwner;
    lane.thread = std::thread([this, &backend, &renderer] {
#ifdef TRACY_ENABLE
        tracy::SetThreadName("RenderOwner");
#endif
        Diag::Prof::RegisterThread("RenderOwner", Diag::Prof::Role::RenderOwner);
        RenderOwnerLoop(backend, renderer);
    });
    {
        std::unique_lock<std::mutex> lk(lane.mutex);
        lane.done.wait(lk, [&lane] { return lane.ready; });
    }
    // Handed over only once the lane exists: everything before (boot, resource
    // creation, the editor's own device work) issued on main, and IssueDeviceExecution
    // is the only hard gate.
    const std::thread::id laneId = lane.id.load(std::memory_order_acquire);
    m_renderOwnerThreadId.store(laneId, std::memory_order_release);
    FrameControl(backend).SetDeviceFrameThread(laneId);
    // Published LAST, once the lane is fully live: from this store on the park hook and
    // a re-entered DrainFrameWork may find the lane, and always find it complete.
    // StopRenderOwner retires it first.
    m_renderOwnerLane.store(&lane, std::memory_order_release);
    LOG_INFO("[RENDEROWNER] lane started; device frame thread handed over");
}

void FramePipeline::RenderOwnerLoop(IGraphicsDevice& backend, Renderer& renderer)
{
    RenderOwnerLane& lane = *m_renderOwner;
    // -taskaffinity reserves physical core 0's second thread for the lane.
    TaskSystem::Get().PinOwnerThread(1);
    // The lane's jobs go to the global High heap, which every worker checks before its
    // own shard, so a record or plan lane does not wait behind the chain-solve jobs that
    // fill every shard while main runs F+1's Animation. Scheduling only: what runs is
    // unchanged. -lanejobprioff is the escape.
    {
        const bool prio = !Sys::CommandLine::Get().Has("-lanejobprioff");
        TaskSystem::SetThreadSubmitCostFloor(prio ? 1u : 0u);
        LOG_INFO("[LANEJOBPRIO] enabled=%u", prio ? 1u : 0u);
    }
    // THE QUEUE IS CREATED HERE ON PURPOSE: OwnerTaskQueue's constructor binds
    // it to the calling thread and CheckOwner throws for every other one.
    Sys::OwnerTaskQueue queue;
    IDeviceFrameControl& device = FrameControl(backend);
    {
        std::lock_guard<std::mutex> lk(lane.mutex);
        lane.id.store(std::this_thread::get_id(), std::memory_order_release);
        lane.ready = true;
    }
    lane.done.notify_all();

#if defined(WITH_HARNESS) && WITH_HARNESS
    const int verifyEvery = [] {
        const auto& cl = Sys::CommandLine::Get();
        return cl.Has("-renderownerverify") ? (std::max)(1, cl.GetInt("-renderownerverify", 60)) : 0;
    }();
    uint64_t verifyFrames = 0;
    // -renderowner_stall=<ms>@<frame>: the lane holds that render frame for <ms> and posts the window a
    // message half way - a long drain on main, for renderowner_wait_pump.
    struct Stall { int ms = 0; uint64_t frame = 0; } stall;
    {
        const std::string spec = Sys::CommandLine::Get().Get("-renderowner_stall");
        const std::size_t at = spec.find('@');
        if (at != std::string::npos)
        {
            stall.ms    = std::atoi(spec.substr(0, at).c_str());
            stall.frame = static_cast<uint64_t>(std::atoll(spec.substr(at + 1).c_str()));
        }
    }
    // -parkhookstress[=N]: fire the park hook from the lane every N frames, and once more
    // after the loop exits - inside StopRenderOwner's teardown, after the pointer is
    // retired. The hook must recognise the lane by thread id and count a park skip;
    // reaching DrainFrameWork there would throw off-main inside ~OwnerTaskQueue
    // (std::terminate).
    // Not driven from a CPU worker: DrainFrameWork refuses every thread but main by
    // design, so a worker fire is an error path, not a race.
    const int stressEvery = [] {
        const auto& cl = Sys::CommandLine::Get();
        return cl.Has("-parkhookstress") ? (std::max)(1, cl.GetInt("-parkhookstress", 30)) : 0;
    }();
#endif

    for (;;)
    {
        FramePacket pkt{};
        {
            // The lane has nothing to do until main posts the next packet.
            PROF_WAIT(WaitIdle, "RenderOwner.PacketWait");
            std::unique_lock<std::mutex> lk(lane.mutex);
            lane.job.wait(lk, [&lane] { return lane.posted || lane.shutdown; });
            if (!lane.posted) break;   // shutdown with nothing queued
            pkt = lane.packet;
            lane.posted = false;
        }
#if defined(WITH_HARNESS) && WITH_HARNESS
        if (stall.ms > 0 && pkt.frame == stall.frame)
        {
            // The message goes out half way, by when main is waiting for this frame and not at its loop top.
            std::this_thread::sleep_for(std::chrono::milliseconds(stall.ms / 2));
            if (const HWND window = Plat::MainWindowHandle())
                ::PostMessageW(window, WM_USER + 77, 0, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(stall.ms - stall.ms / 2));
        }
#endif
        // Everything the lane records from here on belongs to render frame
        // pkt.frame, whichever game frame main happens to be in.
        Diag::Prof::SetFrameTag(pkt.frame);
        PROF_SCOPE("RenderOwner.Frame");

        std::exception_ptr err;
        std::optional<Sys::ExecutionDomain::Token> token;
        bool scopeConflict = false;
        try
        {
            // Same context the serial path installs in RenderHalf, so the RENDERCTX tripwire
            // and every ThreadCtx::Exempt site behave identically - only the thread underneath
            // differs.
            Sys::ThreadCtx::Scope context(Sys::ThreadCtx::Render);
            token = device.IssueDeviceExecution(pkt.frame);
            Diag::OwnerAccounting::Get().BeginRenderFrame(pkt.frame);
            // ---- -brslane: PREPARE on the lane, before the render chain ----
            // Here and not inside RenderHalfAsync: queue.RunUntil has not started, so the
            // FrameTaskContext this creates pumps its own continuations without recursively
            // pumping a live chain. It runs inside the same ThreadCtx::Render scope and
            // device-execution token as the render half, so the [SPLITCHECK][RENDERCTX] tripwire
            // and every ExecutionDomain assertion see one thread owning the frame end to end.
            // The lease travels in the packet; PrepareFrameOnLane throws rather than guesses if
            // it disagrees with renderer state. Gate on pkt.laneEmit, not m_brsLaneEnabled: the
            // packet says whether THIS frame handed the emit half over (a refused-batch-plan
            // frame did not), and PrepareFrameOnLane throws for a frame it is not owed.
            if (pkt.laneEmit)
            {
                const auto prepBegin = std::chrono::steady_clock::now();
                renderer.PrepareFrameOnLane(queue, pkt.frame, pkt.uploadSlot, pkt.emitIdx);
                // ...and here is the emit lease's consume point on the lane (see RenderHalf): the
                // nest has now latched the generations the packet claims, on this thread, for this
                // frame.
                VerifyEmitLease(renderer, pkt);
                // [BRSLANE_PREPARE]: the lane's prepare span, the work main would otherwise overlap with
                // its UI block; printed only while the -brslaneverify oracle is armed. oracleMs is
                // subtracted, not folded in: the digest hashes ~3 MB of upload-heap bytes on one frame in
                // N (13-21 ms against a 0.8-1.2 ms prepare span), so a prepareMs that included it would
                // spike on the sampled frames. Gated on the ARMED oracle, not the command line: on Retail
                // -brslaneverify parses but nothing arms, and oracleMs would claim the digest was free.
                if (renderer.BrsLaneVerifyArmed())
                {
                    const double totalMs = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - prepBegin).count();
                    const double oracleMs = renderer.TakeBrsLaneOracleMs();
                    LOG_INFO("[BRSLANE_PREPARE] frame=%llu prepareMs=%.4f oracleMs=%.4f",
                        static_cast<unsigned long long>(pkt.frame),
                        (std::max)(0.0, totalMs - oracleMs), oracleMs);
                }
            }
            Sys::TaskHandle completion = [&] {
                Diag::OwnerAccounting::TagScope tag(pkt.frame + 1);
                return RenderHalfAsync(backend, renderer, pkt, queue, token)
                           .Start(queue, token, "App.RenderFrame");
            }();
            // The lane's own blocking drain. It costs main nothing, so these continuations stay
            // counted as viaPump (nothing on main was blocked by them), never as viaDrain.
            queue.RunUntil(completion);
        }
        catch (const std::logic_error& e)
        {
            err = std::current_exception();
            scopeConflict =
                std::string_view(e.what()).find("execution domain") != std::string_view::npos;
        }
        catch (...) { err = std::current_exception(); }

        // Retired HERE: on the thread that issued it, with no scope open, and
        // before main is told the frame is done. Main never touches the domain
        // while the lane could still be inside it.
        if (token)
        {
            try { device.RetireDeviceExecution(*token); }
            catch (...) { if (!err) err = std::current_exception(); }
        }

#if defined(WITH_HARNESS) && WITH_HARNESS
        if (verifyEvery > 0 && ++verifyFrames % verifyEvery == 0)
        {
            const bool deviceThreadOk =
                std::this_thread::get_id() == lane.id.load(std::memory_order_acquire);
            const bool scopeOpen = Sys::ExecutionDomain::HasActiveScope();
            ENGINE_CHECK(deviceThreadOk,
                "[RENDEROWNER] frame=%llu ran on a thread that is not the lane",
                static_cast<unsigned long long>(pkt.frame));
            ENGINE_CHECK(!scopeConflict,
                "[RENDEROWNER] frame=%llu lost the execution domain to another owner",
                static_cast<unsigned long long>(pkt.frame));
            ENGINE_CHECK(!scopeOpen,
                "[RENDEROWNER] frame=%llu left an execution scope open",
                static_cast<unsigned long long>(pkt.frame));
            std::lock_guard<std::mutex> lk(lane.mutex);
            LOG_INFO("[RENDEROWNER] frame=%llu startedOn=renderowner deviceThread=%d "
                     "scopeOpen=%d conflicts=%llu parkSkips=%llu waits=%llu "
                     "blockingWaits=%llu err=%d",
                     static_cast<unsigned long long>(pkt.frame),
                     deviceThreadOk ? 1 : 0,
                     scopeOpen ? 1 : 0,
                     static_cast<unsigned long long>(lane.scopeConflicts + (scopeConflict ? 1u : 0u)),
                     static_cast<unsigned long long>(
                         m_renderOwnerParkSkips.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(lane.waits),
                     static_cast<unsigned long long>(lane.blockingWaits),
                     err ? 1 : 0);
        }
#endif
#if defined(WITH_HARNESS) && WITH_HARNESS
        // The token is retired, no scope is open: exactly the shape a resource
        // path on this thread would have when it parks.
        if (stressEvery > 0 && (lane.frames + 1) % static_cast<uint64_t>(stressEvery) == 0)
            backend.DrainCpuFrame();
#endif
        {
            std::lock_guard<std::mutex> lk(lane.mutex);
            lane.error = err;
            if (scopeConflict) ++lane.scopeConflicts;
            lane.inFlight = false;
            ++lane.frames;
            ::SetEvent(lane.doneEvent);
        }
        lane.done.notify_all();
    }
#if defined(WITH_HARNESS) && WITH_HARNESS
    // The shutdown fire: main is inside StopRenderOwner between retiring the pointer and
    // the join. It must be counted as a park skip, not crash, and not reach
    // DrainFrameWork.
    if (stressEvery > 0) backend.DrainCpuFrame();
#endif
    // ~OwnerTaskQueue CloseAndDrain()s, and this is the only thread allowed to.
}

bool FramePipeline::RenderOwnerFrameInFlight()
{
    // Through the atomic publication rather than the owning unique_ptr, so there is
    // exactly ONE answer to "is there a lane" in this file.
    RenderOwnerLane* lane = m_renderOwnerLane.load(std::memory_order_acquire);
    if (!lane) return false;
    std::lock_guard<std::mutex> lk(lane->mutex);
    return lane->inFlight;
}

void FramePipeline::PostRenderOwnerFrame(const FramePacket& packet)
{
    RenderOwnerLane& lane = *m_renderOwner;
    {
        std::lock_guard<std::mutex> lk(lane.mutex);
        lane.packet   = packet;
        lane.posted   = true;
        lane.inFlight = true;
        lane.error    = {};
        ::ResetEvent(lane.doneEvent);
    }
    lane.job.notify_one();
    // Opens the overlap window G: taken after the notify, on main, so it spans exactly
    // what main got to do while the lane had the frame. Closed at the next barrier
    // (AnimationBoundaryBarrier, or RenderPhaseBarrier under -animoverlap).
    m_renderOwnerPostAt = std::chrono::steady_clock::now();
}

// Main only: the window baselines are plain statics.
void FramePipeline::LogDrainStats(const char* tag, bool window, uint64_t frames)
{
    constexpr size_t kN = static_cast<size_t>(Diag::DrainCause::Count);
    static uint32_t s_calls[kN]{}, s_block[kN]{};
    static uint64_t s_ns[kN]{};
    auto& acc = Diag::OwnerAccounting::Get();
    char buf[1024];
    int  len = 0;
    uint32_t totalBlock = 0;
    double   totalMs = 0.0;
    for (size_t i = 0; i < kN; ++i)
    {
        const uint32_t c  = acc.drains[i].calls.load(std::memory_order_relaxed);
        const uint32_t b  = acc.drains[i].blocking.load(std::memory_order_relaxed);
        const uint64_t ns = acc.drains[i].blockingNs.load(std::memory_order_relaxed);
        const uint32_t dc = window ? c - s_calls[i] : c;
        const uint32_t db = window ? b - s_block[i] : b;
        const uint64_t dn = window ? ns - s_ns[i] : ns;
        if (window) { s_calls[i] = c; s_block[i] = b; s_ns[i] = ns; }
        totalBlock += db;
        totalMs    += static_cast<double>(dn) * 1.0e-6;
        if (dc == 0 || len >= static_cast<int>(sizeof(buf)) - 64) continue;
        len += std::snprintf(buf + len, sizeof(buf) - len, " %s=%u/%u/%.3f",
                             Diag::DrainCauseName(static_cast<Diag::DrainCause>(i)),
                             dc, db, static_cast<double>(dn) * 1.0e-6);
    }
    buf[len] = 0;
    LOG_INFO("[DRAIN] %s frames=%llu blocking=%u blockingMs=%.3f |%s", tag,
             static_cast<unsigned long long>(frames), totalBlock, totalMs, buf);
}

void FramePipeline::WaitForLaneFrame(RenderOwnerLane& lane, std::unique_lock<std::mutex>& lk)
{
    // A render frame is a few ms: the condition variable, as it always was. Past kPumpAfter main is
    // stuck in a drain that is not a frame (a world reload, a swap-chain rebuild, a hitch), and its
    // window must keep answering: wait on the lane's event AND the thread's messages, and hand the
    // messages to the window. -lane_wait_bare keeps the plain wait (renderowner_wait_pump's reverse arm).
    constexpr auto kPumpAfter = std::chrono::milliseconds(50);
#ifdef WITH_HARNESS
    static const bool s_bare = Sys::CommandLine::Get().Has("-lane_wait_bare");
#else
    constexpr bool s_bare = false;
#endif
    if (lane.done.wait_for(lk, kPumpAfter, [&lane] { return !lane.inFlight; }))
        return;
    if (!m_waitPump || s_bare)
    {
        lane.done.wait(lk, [&lane] { return !lane.inFlight; });
        return;
    }
    bool pumping = true;
    static bool s_logged = false;
    if (!s_logged)
    {
        s_logged = true;
        LOG_INFO("[RENDEROWNER] main waits for the lane past %lld ms - the window's messages are answered meanwhile",
                 static_cast<long long>(kPumpAfter.count()));
    }
    while (lane.inFlight)
    {
        // The event, not the condition variable: a message must wake this wait too. It is set under
        // the lane's mutex as inFlight clears, so a wake with inFlight still true is a message.
        lk.unlock();
        if (pumping)
        {
            const HANDLE events[1] = { static_cast<HANDLE>(lane.doneEvent) };
            const DWORD woke = ::MsgWaitForMultipleObjectsEx(1, events, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (woke == WAIT_OBJECT_0 + 1)
                pumping = m_waitPump();
        }
        else
        {
            ::WaitForSingleObject(static_cast<HANDLE>(lane.doneEvent), INFINITE);
        }
        lk.lock();
    }
}

void FramePipeline::DrainRenderOwner(RenderOwnerLane& lane, Diag::DrainCause cause)
{
    struct DrainDepthGuard {
        int& depth; const bool outermost;
        explicit DrainDepthGuard(int& d) : depth(d), outermost(d == 0) { ++d; }
        ~DrainDepthGuard() { --depth; }
    } drainDepth(m_drainDepth);
    const auto drainStart = std::chrono::steady_clock::now();
    std::exception_ptr error;
    {
        std::unique_lock<std::mutex> lk(lane.mutex);
        ++lane.waits;
        if (lane.inFlight)
        {
            ++lane.blockingWaits;
            Diag::OwnerAccounting::BlockingScope blocking;
            PROF_WAIT(WaitJoin, "App.DrainRenderOwner");
            const auto waitStart = std::chrono::steady_clock::now();
            WaitForLaneFrame(lane, lk);
            Diag::OwnerAccounting::Get().NoteDrainBlocked(cause, static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - waitStart).count()));
        }
        error = lane.error;
        lane.error = {};
    }
    const double drainMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - drainStart).count();
    if (drainDepth.outermost) m_blockingDrainMsThisFrame += drainMs;
    else { m_nestedDrainMsThisFrame += drainMs; ++m_nestedDrainsThisFrame; }
    if (Sys::CommandLine::Get().Has("-materialcpuverify"))
        LOG_INFO("[FRAME_EXECUTOR] frame=%llu dedicated=1 complete=1",
            static_cast<unsigned long long>(m_fromRender.frame));
    if (error) std::rethrow_exception(error);
}

void FramePipeline::StopRenderOwner()
{
    if (!m_renderOwner) return;
    // ---- the quiescence protocol ----
    // 1. Retire the publication: from this store on, no new park hook and no re-entered
    //    DrainFrameWork can find the lane. m_renderOwnerThreadId stays set: the lane's
    //    ~OwnerTaskQueue still runs after this point and its device work must still be
    //    recognised.
    m_renderOwnerLane.store(nullptr, std::memory_order_release);
    // (m_renderOwnerEnabled is cleared after the join, step 4: the lane reads it.)
    // 2. Wait for the drains already inside: a body that loaded the pointer before step 1
    //    may still be in DrainRenderOwner. No deadlock: the lane is still running, so its
    //    cv is serviced, and it has not been asked to shut down. Yield, do not sleep.
    //    Today this exits on its first test (DrainFrameWork refuses every thread but
    //    main, and this is main); the wait keeps the protocol correct if that refusal is
    //    ever relaxed.
    while (m_renderOwnerHookDepth.load(std::memory_order_acquire) != 0)
        std::this_thread::yield();
    // 3. Now the lane is ours alone.
    std::unique_ptr<RenderOwnerLane> lane = std::move(m_renderOwner);
    std::exception_ptr pending;
    {
        std::unique_lock<std::mutex> lk(lane->mutex);
        lane->done.wait(lk, [&lane] { return !lane->inFlight; });
        pending = lane->error;
        lane->error = {};
        lane->shutdown = true;
    }
    lane->job.notify_all();
    if (lane->thread.joinable()) lane->thread.join();
    // 4. Only now is the lane thread gone, so only now may the id publication be
    //    cleared: ~OwnerTaskQueue's CloseAndDrain ran between step 1 and here and could
    //    fire the park hook the whole time.
    m_renderOwnerThreadId.store(std::thread::id{}, std::memory_order_release);
    // ...and only now may the flag the lane reads change (RenderHalfAsync's uiOnMain).
    // Its other writer, Configure, runs before the lane starts, so the lane never races
    // a write.
    m_renderOwnerEnabled = false;
    FrameControl(m_device).SetDeviceFrameThread(m_mainThreadId);
    LOG_INFO("[RENDEROWNER] stopped frames=%llu waits=%llu blockingWaits=%llu "
             "parkSkips=%llu conflicts=%llu torn=%llu dropped=%llu",
             static_cast<unsigned long long>(lane->frames),
             static_cast<unsigned long long>(lane->waits),
             static_cast<unsigned long long>(lane->blockingWaits),
             static_cast<unsigned long long>(
                 m_renderOwnerParkSkips.load(std::memory_order_relaxed)),
             static_cast<unsigned long long>(lane->scopeConflicts),
             static_cast<unsigned long long>(Diag::OwnerAccounting::Get().TornReads()),
             static_cast<unsigned long long>(Diag::OwnerAccounting::Get().DroppedCharges()));
    // Reported, not swallowed: a frame that failed after the last drain would
    // otherwise disappear with the lane.
    if (pending)
    {
        try { std::rethrow_exception(pending); }
        catch (const std::exception& e)
        { ENGINE_CHECK(false, "[RENDEROWNER] last frame ended in an error: %s", e.what()); }
        catch (...) { ENGINE_CHECK(false, "[RENDEROWNER] last frame ended in an unknown error"); }
    }
}

void FramePipeline::Configure(IGraphicsDevice& backend, Renderer& renderer)
{
    // -rendermode=serial|graph (env DX12_RENDERMODE): the game/render split's
    // mode. Graph is the default; serial is the diagnostic escape. Both use owner
    // coroutines and shared CPU recording jobs, and neither creates a dedicated render
    // thread or packet semaphore (the second owner is -renderowner's lane).
    // (m_mainThreadId is recorded by the constructor, before any boot chunk.)
    {
        const char* reason = "default";
        std::string want   = "graph";
        char*  envBuf = nullptr;
        size_t envLen = 0;
        if (_dupenv_s(&envBuf, &envLen, "DX12_RENDERMODE") == 0 && envBuf)
        {
            want   = envBuf;
            reason = "env";
            std::free(envBuf);
        }
        if (Sys::CommandLine::Get().Has("-rendermode"))
        {
            // Fallback "" rather than "serial": a bare -rendermode has no value to honour, and
            // serial differs from the default, so a typo would become a silent downgrade. "" is
            // not a mode, so the ENGINE_CHECK below reports it.
            want   = Sys::CommandLine::Get().Get("-rendermode", "");
            reason = "cmdline";
        }

        // An unrecognised value is an ERROR, not a silent fall to serial: a
        // typo in a pack's args would otherwise run the wrong mode and pass.
        // "threaded" was an alias of graph (no dedicated thread); a stale command line that
        // still says it gets the error and the graph mode it always got.
        if (want == "threaded")
        {
            ENGINE_CHECK(false, "-rendermode=threaded was removed (it was an alias of graph); running graph.");
            want   = "graph";
            reason = "threaded-removed";
        }
        else if (want != "serial" && want != "graph")
        {
            ENGINE_CHECK(false,
                "-rendermode='%s' is not a mode (expected serial|graph); running "
                "serial. NOTE serial is no longer the default, so this is a change "
                "of behaviour, not a no-op.",
                want.c_str());
            want   = "serial";
            reason = "bad-value";
        }

        // Graph execution requires frame-keyed upload slots and stamped resource release:
        // -slotkey=dxgi drops to serial with an error.
        if (want == "graph")
        {
            const char* bad = nullptr;
            if (Sys::CommandLine::Get().Get("-slotkey", "frame") != "frame") bad = "-slotkey=dxgi";
            if (bad)
            {
                ENGINE_CHECK(false,
                    "-rendermode=graph is not compatible with %s; running serial. "
                    "Graph execution requires frame-keyed slots and stamped resource release.", bad);
                want   = "serial";
                reason = "incompatible-switch";
            }
        }

        m_renderGraph = (want == "graph");
        FrameControl(backend).EnableLogicalFrameCoordinator();
        LOG_INFO("[SPLITCHECK] mode=%s reason=%s", want.c_str(), reason);
    }

    // -sltokenlag=N: fault injection for the Streamline token oracle (see m_slTokenLag and
    // SubmitFrame). Clamped to the ring so a typo cannot read a slot that was never
    // written.
    m_slTokenLag = static_cast<uint64_t>(
        std::max(0, Sys::CommandLine::Get().GetInt("-sltokenlag", 0)));
    if (m_slTokenLag >= kSlTokenRing)
    {
        LOG_WARNING("-sltokenlag=%llu exceeds the %d-slot token ring; clamping",
                    static_cast<unsigned long long>(m_slTokenLag), kSlTokenRing - 1);
        m_slTokenLag = kSlTokenRing - 1;
    }
    if (m_slTokenLag > 0)
        LOG_INFO("[SPLITCHECK] sltokenlag=%llu",
                 static_cast<unsigned long long>(m_slTokenLag));

    IDeviceFrameControl& frameDevice = FrameControl(backend);
    frameDevice.SetDeviceFrameThread(m_mainThreadId);
    // THE PARK HOOK. On main it is "finish the in-flight frame, then proceed"
    // (DrainFrameWork, which routes to DrainRenderOwner under -renderowner). On the lane
    // it cannot be a drain - the lane IS the in-flight frame and OwnerTaskQueue refuses a
    // recursive pump - so it is counted as a park skip. The chain holds its
    // ExecutionDomain::Scope across every resume, so DrainFrameIfNeeded's !Owns() gate
    // keeps that branch unreachable during a frame; the counter proves it, and
    // -parkhookstress reaches it on purpose.
    // It touches no lane memory: the thread id and the counter are atomics that outlive
    // the lane object, so the last fires (~OwnerTaskQueue's CloseAndDrain on the lane,
    // after the pointer is retired) are still answered instead of reaching a
    // DrainFrameWork that throws off-main inside a destructor.
    frameDevice.SetFrameDrainHook([this] {
        if (std::this_thread::get_id() ==
            m_renderOwnerThreadId.load(std::memory_order_acquire))
        {
            m_renderOwnerParkSkips.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        DrainFrameWork(static_cast<Diag::DrainCause>(
            Diag::OwnerAccounting::Get().pendingHookCause.load(std::memory_order_relaxed)));
    });
    m_frameOverlap = m_renderGraph && !Sys::CommandLine::Get().Has("-frameoverlapoff") &&
        (Sys::CommandLine::Get().Has("-frameoverlap") || Sys::JobSplittingDefaultsEnabled());
    LOG_INFO("[JOB_DEFAULTS] enabled=%u frameOverlap=%u", unsigned(Sys::JobSplittingDefaultsEnabled()), unsigned(m_frameOverlap));
    LOG_INFO("[FRAME_EXECUTOR] coordinator=owner dedicated=0 uploadGame=0 uploadRender=1");

    // ---- -renderowner: the second owner thread (RenderOwnerLane, FramePipeline.h) ----
    // Default on in graph mode on every host (measured: AIBench frame p50 about -1.8 ms,
    // total CPU work not up); -renderowneroff is the escape and beats an explicit
    // -renderowner, as -frameoverlapoff beats -frameoverlap. It refuses rather than falls
    // back where an invariant cannot hold, because a silent fallback makes an A/B measure
    // nothing: -rendermode=serial has no owner coroutine chain to hand over. The default
    // excludes serial, so a serial run boots silent.
    // On the editor host main builds the UI while the lane is idle (BuildEarlyEditorUI after
    // the Render-phase drain, or RenderHalf after its entry drain), freezes the ImGui
    // snapshot and only then posts; the lane issues and retires its own token, and main
    // completes the snapshot when it next drains. ImGui never runs on the lane.
#if defined(WITH_HARNESS) && WITH_HARNESS
    m_renderOwnerVerifyEvery = Sys::CommandLine::Get().Has("-renderownerverify")
        ? (std::max)(1, Sys::CommandLine::Get().GetInt("-renderownerverify", 60)) : 0;
#endif
    const bool renderOwnerDefault = m_renderGraph;   // every host
    const bool renderOwnerWanted  = !Sys::CommandLine::Get().Has("-renderowneroff") &&
        (Sys::CommandLine::Get().Has("-renderowner") || renderOwnerDefault);
    if (renderOwnerWanted)
    {
        const char* refused = nullptr;
        if (!m_renderGraph)
            refused = "-rendermode=serial: no owner coroutine chain to hand over";
        if (refused)
            LOG_WARNING("[RENDEROWNER] refused: %s; the render half stays on main", refused);
        else
        {
            m_renderOwnerEnabled = true;
            LOG_INFO("[RENDEROWNER] enabled=1 overlap=%u editorUi=%s", unsigned(m_frameOverlap),
                     Sys::IsEditorHost() ? "main" : "none");
            // The lane's EndUIFrameAsync only records; main completes the snapshot (gives ImGui
            // its buffers back) in DrainFrameWork. Editor.exe only: Game.exe installs no hooks.
            if (m_editor) m_editor->DeferSnapshotCompletion();
        }
    }
    // ONE line that always prints, on every host and in every mode: "did the render half
    // run on main in this log?" has two answers, and the [RENDEROWNER] lines exist for
    // only one of them.
    LOG_INFO("[FRAME_EXECUTOR] renderowner=%u default=%u",
             unsigned(m_renderOwnerEnabled), unsigned(renderOwnerDefault));

    // ---- -animoverlap (kAnimOverlapDefault, FramePipeline.h) ----
    // Let Animation(F) overlap render(F-1) by moving the Animation-boundary barrier down
    // to the DrainFrameWork that opens TickPhase::Render. It refuses rather than falls
    // back, like -renderowner; every rung is a real gap:
    //   - no lane (-renderowneroff / -rendermode=serial): a render half on main has
    //     nothing to overlap.
    //   - -slotkey=dxgi: the acquire goes through WaitForNextFrameSlot, which refuses an
    //     open frame.
    //   - -brssplit: PrepareStaticSceneEarly rewrites m_candCache inside the Animation
    //     phase, i.e. inside the window this flag opens. Mutually exclusive by explicit
    //     refusal, never by whichever happened to be checked last.
    // The editor host needs no rung: its editor-only writers (AddAnimPinOwner, the
    // chain-preview lane, the stage clock's step count) are written in
    // EditorLayer::ProcessPendingActions after the moved barrier's drain and read only by
    // the game half's Animation phases; SetCamera / BuildGameView write FrameViewWrite()
    // as in Game.exe, and the UI build runs with the lane idle.
    // THE DEFAULT IS HOST-AWARE: asked only where the lane is itself the default and
    // running, so a serial or -renderowneroff boot does not warn about a refusal it never
    // asked for, and [FRAME_EXECUTOR] default= keeps its meaning. An explicit -animoverlap
    // there still gets its refusal, which animoverlap_refused_* pin. -brslane's default
    // also excludes the editor unless its UI is built early (editorUiEarlyAvail below).
    const bool laneIsDefaultHere  = renderOwnerDefault && m_renderOwnerEnabled && !Sys::IsEditorHost();
    const bool animLaneDefaultHere = renderOwnerDefault && m_renderOwnerEnabled;
    const bool animOverlapDefault = kAnimOverlapDefault && animLaneDefaultHere;
    const bool animOverlapWanted  = !Sys::CommandLine::Get().Has("-animoverlapoff") &&
        (Sys::CommandLine::Get().Has("-animoverlap") || animOverlapDefault);
    if (animOverlapWanted)
    {
        const char* refused = nullptr;
        if (!m_renderOwnerEnabled)
            refused = "no render-owner lane: the render half runs on main, so moving the "
                      "Animation barrier would overlap nothing";
        else if (!m_device.IsSlotKeyedByFrame())
            refused = "-slotkey=dxgi: the game half's acquire goes through "
                      "WaitForNextFrameSlot, which refuses to run while a frame is open "
                      "(it would reassign the back-buffer index under the lane's command "
                      "lists), and the back-buffer slot is not a function of the frame "
                      "counter so the halves cannot be one frame apart";
        else if (Sys::CommandLine::Get().Has("-brssplit"))
            refused = "-brssplit: PrepareStaticSceneEarly rewrites the retained candidate "
                      "cache from inside the Animation phase, which is the window this flag "
                      "opens beside a live render half";
        if (refused)
            LOG_WARNING("[ANIMOVERLAP] refused: %s; the Animation barrier stays put", refused);
        else
        {
            m_animOverlapEnabled = true;
            // The device-side precondition the moved barrier lifts: AcquireUploadSlotAsync
            // otherwise refuses an open frame (its comment says why that refusal is a proxy and
            // what replaces it).
            FrameControl(m_device).SetConcurrentUploadAcquire(true);
        }
    }
#if defined(WITH_HARNESS) && WITH_HARNESS
    m_animOverlapVerifyEvery = Sys::CommandLine::Get().Has("-animoverlapverify")
        ? (std::max)(1, Sys::CommandLine::Get().GetInt("-animoverlapverify", 60)) : 0;
    // The oracle's only new instrument: GetUploadSlot() asked from a thread inside
    // Sys::ThreadCtx::Render. Armed by the verify flag, not by -animoverlap, so a
    // baseline arm (barrier unmoved) reports the same number and conflicts=0 is a
    // comparison, not an assertion about an untested path.
    if (m_animOverlapVerifyEvery > 0)
        FrameControl(m_device).SetGameSlotAudit(true);
#endif
    // ONE line that always prints, like [FRAME_EXECUTOR]: "was the barrier moved in this
    // log?" must be answerable without knowing whether a refusal line was emitted.
    LOG_INFO("[ANIMOVERLAP] enabled=%u verify=%d",
             unsigned(m_animOverlapEnabled), m_animOverlapVerifyEvery);

    // ---- -brslane: the emit half runs on the render-owner lane ----
    // Sys::CommandLine has no registry of known flags (an unrecognised -x is stored and
    // never read), so this ladder refuses OUT LOUD and always prints [BRSLANE] enabled=:
    // "the flag did nothing" and "the flag is not a flag" must be distinguishable in a
    // log.
    // MOVES: BuildScene_EmitBatches (the emit nest + the post-nest tail),
    // QueueFrameMaterialWrites through a FrameTaskContext created ON the lane, and
    // FinishFramePreparation (seal + PublishSceneRead + the skin read half). STAYS ON
    // MAIN: BuildScene_PlanEmit including ResolveBatchPlan (both allocators and the
    // surface-cache epoch stay main's; -brsplanlane below moves the plan too), stages
    // 9-13 of BuildRenderScene, DDGISceneAS::Gather and UploadFrameData.
    // The default is host-aware with one term -animoverlap's lacks, the moved barrier:
    // the lane refuses without it, so an -animoverlapoff run boots silent, while an
    // explicit -brslane there gets the refusal (brslane_refused pins it). The editor
    // joins the default only when it builds its UI early (not -editoruilate): every
    // UI-time renderer write then lands before this frame's scene build, on both sides
    // of the seam.
    // TOLD, NOT ASKED: Renderer::BrsLaneWanted and the -brslane -> -surfacepagesearly
    // implication latch on first call, so the host default is published here before
    // either is read; the terms live only here, so the renderer cannot disagree with the
    // ladder that refuses.
    const bool editorUiEarlyAvail = Sys::IsEditorHost() &&
                                    !Sys::CommandLine::Get().Has("-editoruilate");
    Renderer::SetBrsLaneHostDefault(
        (laneIsDefaultHere || (animLaneDefaultHere && editorUiEarlyAvail)) && m_animOverlapEnabled);
    const bool brsLaneDefault = Renderer::BrsLaneDefaultActive();
    bool       brsLaneEnabled = false;
    const bool brsLaneWanted  = Renderer::BrsLaneWanted();
    // EVERY RUNG IS A REAL CONSTRAINT, not defensiveness:
    if (brsLaneWanted)
    {
        const char* refused = nullptr;
        if (Sys::IsEditorHost() && !editorUiEarlyAvail)
            // -editoruilate: the UI is built after BeginFrame(F), so its renderer writes (Project
            // Settings toggles, material shader registration, preview requests - about seventy
            // setters) would land between main's scene capture and the lane's plan + nest of
            // the same frame. The early UI build is what removes that.
            refused = "editor host with -editoruilate: the UI is built after "
                      "BeginFrame, between main's scene capture and the lane's emit";
        else if (!m_renderOwnerEnabled)
            refused = "no render-owner lane: the render half already runs on main, so "
                      "moving the emit half there moves it nowhere";
        else if (Sys::CommandLine::Get().Has("-brssplit"))
            // Asked before the -animoverlap rung on purpose: -animoverlap has a -brssplit rung of
            // its own, so with the general question first this rung is unreachable and a
            // -brssplit run is told its barrier is missing.
            refused = "-brssplit: PrepareStaticSceneEarly rewrites the retained candidate "
                      "cache from inside the Animation phase, i.e. inside the window the "
                      "lane is reading m_candScratch in";
        else if (!m_animOverlapEnabled)
            // NOT a preference. Without the moved barrier the only drain that
            // joins the lane sits AFTER renderer.BeginFrame(F+1), so F+1 would
            // enter BeginFrame while the lane still owes F its publish - and
            // BeginFrame's own guard would throw on the second frame.
            refused = "-animoverlap is off: the unmoved Animation barrier drains the "
                      "lane AFTER BeginFrame(F+1), so F+1 would start building while "
                      "the lane still owes F its publish";
        else if (Sys::CommandLine::Get().Has("-surfacepagesearlyoff"))
            // -brslane implies -surfacepagesearly (Renderer::SurfacePagesEarlyActive). With the
            // hoist off, the nest calls AcquirePersistentMatSlot, AcquirePage and MapEntity: two
            // allocators and the publish epoch DDGISceneAS::Gather reads on main after the
            // hand-off.
            refused = "-surfacepagesearlyoff: without the ResolveBatchPlan hoist the "
                      "emit nest owns two allocators and the surface-cache publish "
                      "epoch, and main reads that epoch after the hand-off";
        else if (Sys::CommandLine::Get().Has("-framepreparelegacy"))
            refused = "-framepreparelegacy: the lane-side material transaction needs a "
                      "FrameTaskContext, and this flag is the arm that runs without one";
        else if (Renderer::MatSnapOracleArmed())
            // The oracle at the material-batch head dereferences first.mc, a
            // raw MaterialComponent pool pointer whose next writer is
            // TickPhase::PreRender(F+1) on main. Reading it from the lane is a
            // use-after-move the [SPLITCHECK][RENDERCTX] tripwire cannot see,
            // because the pointer was captured while still on main.
            refused = "DX12_VERIFY_MATSNAP=1: that oracle dereferences a MaterialComponent "
                      "pool pointer from inside the emit nest, and the nest is about to "
                      "stop being on the thread that owns the pool";
        else if (Renderer::MatBufChecksumArmed())
            // The checksum is read on main inside BeginFrame, BEFORE the
            // hand-off - so under the flag it would hash the PREVIOUS frame's
            // material table and report a difference that is entirely its own.
            refused = "DX12_MATBUF_CHECKSUM: the checksum is taken on main inside "
                      "BeginFrame, which under this flag is before the emit half has "
                      "written the material table";
        if (refused)
            LOG_WARNING("[BRSLANE] refused: %s; the emit half stays on main", refused);
        else
        {
            brsLaneEnabled = true;
            renderer.SetBrsLaneEnabled(true);
        }
    }
    m_brsLaneEnabled = brsLaneEnabled;
    // The editor builds its UI early exactly when its emit half is on the lane (a
    // lane-off or -brslaneoff editor builds it late, in RenderHalf).
    m_editorUiEarly = Sys::IsEditorHost() && brsLaneEnabled && editorUiEarlyAvail;
    if (Sys::IsEditorHost())
        LOG_INFO("[EDITORUI] early=%u (lane emit %u, -editoruilate %u)",
                 unsigned(m_editorUiEarly), unsigned(brsLaneEnabled),
                 unsigned(Sys::CommandLine::Get().Has("-editoruilate")));

    // ---- -brsplanlane: the emit half's PLAN on the lane ----
    // The sort, the preamble and ResolveBatchPlan move to the lane with the nest (in
    // PrepareFrameOnLane, before it). It needs the lane emit and nothing else: the DDGI
    // proxy binding, the one main-side reader of the plan's allocators after the
    // hand-off, runs in BuildOrRefit on the lane. The default wherever the lane emit is
    // on (measured on AIBench200: frame p50 -0.3 ms, main -0.7 ms); -brsplanlaneoff is
    // the escape. An explicit -brsplanlane whose emit half stays on main is refused out
    // loud; the default is silent there, so the editor and every lane-off run boot
    // without a warning. The line always prints.
    const bool brsPlanLaneDefault = brsLaneEnabled;
    bool       brsPlanLane        = false;
    if (!Sys::CommandLine::Get().Has("-brsplanlaneoff") &&
        (Sys::CommandLine::Get().Has("-brsplanlane") || brsPlanLaneDefault))
    {
        if (!brsLaneEnabled)
            LOG_WARNING("[BRSPLANLANE] refused: the emit half is not on the lane "
                        "(-brslane refused or off); the plan stays on main");
        else
        {
            brsPlanLane = true;
            renderer.SetBrsPlanLane(true);
        }
    }
    LOG_INFO("[BRSPLANLANE] enabled=%u default=%u", unsigned(brsPlanLane),
             unsigned(brsPlanLane == brsPlanLaneDefault));
#if defined(WITH_HARNESS) && WITH_HARNESS
    // Armed independently of the ladder, for the reason -animoverlapverify is:
    // the digest has to print in the arm the comparison is AGAINST.
    renderer.SetBrsLaneVerify(
        Sys::CommandLine::Get().Has("-brslaneverify")
            ? (std::max)(1, Sys::CommandLine::Get().GetInt("-brslaneverify", 60)) : 0,
        // -brslaneselftest=N: the POSITIVE CONTROL. Without it "[BRS_LANE_ORACLE] match=1"
        // and "match= cannot be anything but 1" read identically.
        Sys::CommandLine::Get().Has("-brslaneselftest")
            ? Sys::CommandLine::Get().GetInt("-brslaneselftest", 0) : -1);
#endif
    LOG_INFO("[BRSLANE] enabled=%u wanted=%u", unsigned(brsLaneEnabled),
             unsigned(brsLaneWanted));

    // ---- ONE line that names the whole executor shape ----
    // [FRAME_EXECUTOR] renderowner=/default= answers one question and packs pin it
    // verbatim, so new fields go here. This is what a measurement reads: an A/B arm is
    // only comparable to an arm of the same shape, and the lane and the moved barrier
    // are invisible in renderMode. tools/cpu_profile.py records all five fields.
    // default= is the CONJUNCTION ("is this the shipped shape for this host?"), not one
    // flag's default, so it keeps its meaning through every flip and is the field an
    // analysis groups on.
    const bool executorIsDefault =
        (m_renderOwnerEnabled  == renderOwnerDefault) &&
        (m_animOverlapEnabled  == animOverlapDefault) &&
        (brsLaneEnabled        == brsLaneDefault)     &&
        (brsPlanLane           == brsPlanLaneDefault) &&
        // -editoruilate changes the editor's lane-emit default itself, so the comparisons
        // above cannot see it.
        (!Sys::IsEditorHost() || editorUiEarlyAvail) &&
        // Against the hoist's EFFECTIVE default (its own constant OR the lane's
        // implication), not the bare constant: kSurfacePagesEarlyDefault is false (alone the
        // hoist is a measured cost), so comparing with it would print default=0 on every
        // shipped-shape Game run.
        (Renderer::SurfacePagesEarlyActive() == Renderer::SurfacePagesEarlyDefaultActive());
    LOG_INFO("[FRAME_EXECUTOR] executor renderowner=%u animoverlap=%u brslane=%u "
             "surfacepagesearly=%u default=%u",
             unsigned(m_renderOwnerEnabled), unsigned(m_animOverlapEnabled),
             unsigned(brsLaneEnabled), unsigned(Renderer::SurfacePagesEarlyActive()),
             unsigned(executorIsDefault));
    if (Sys::CommandLine::Get().Has("-renderstub"))
        LOG_INFO("[FRAME_EXECUTOR] renderstub retired; explicit upload lanes are active");

    // ---- destroy deferral: destroys stop draining the lane ----
    // Only with the lane: without it the render half runs on main and no frame is in
    // flight. -destroyimmediate is the escape (the drain path); -destroyquarantineonly
    // keeps the id quarantine but applies destroys immediately - the exactness oracle's
    // reference arm (same entity ids).
    {
        const auto& cl = Sys::CommandLine::Get();
        const bool quarantine = m_renderOwnerEnabled && !cl.Has("-destroyimmediate");
        const bool defer = quarantine && !cl.Has("-destroyquarantineonly");
        // The structural mailbox (Lua VFX / DDGI watch / ripple requests, the PiP rect).
        // -mailboximmediate is its escape (drain + run now).
        const bool mailbox = m_renderOwnerEnabled && !cl.Has("-mailboximmediate");
        renderer.SetDeferEntityDestroys(defer);
        renderer.SetDeferSyncPointCalls(mailbox);
        renderer.SetRenderFrameInFlightQuery([this] { return RenderOwnerFrameInFlight(); });
        m_world.SetIdQuarantine(quarantine);
        LOG_INFO("[DESTROY_DEFER] enabled=%u quarantine=%u mailbox=%u",
                 unsigned(defer), unsigned(quarantine), unsigned(mailbox));
    }

    // ---- access-declaration tooling ----
    //   -accessguard_strict  a solo DECLARED system is held to its declaration too
    //                        (AccessGuard.h), not only concurrent members.
    //   -schedplan_dump      [SCHEDACCESS] per system per plan rebuild.
    //   -worldhash_every=N   [WORLDHASH] World::StructuralHash every N frames
    //                        (PublishFrameTiming).
    {
        const auto& cl = Sys::CommandLine::Get();
        ECS::AccessGuard::SetStrict(cl.Has("-accessguard_strict"));
        m_scheduler.SetPlanDump(cl.Has("-schedplan_dump"));
        if (ECS::AccessGuard::IsStrict())
            LOG_INFO("[ACCESSGUARD] strict: solo declared systems are guarded too");
    }
}

void FramePipeline::PublishFrameTiming(IGraphicsDevice& backend, Renderer& renderer, float realDt,
                                       float loopTopMs, uint64_t frameIndex)
{
    // The park-hook count BeginFrameBracket latched when the CPU bracket opened.
    const uint32_t parkHookAtBracketOpen = m_parkHookAtBracketOpen;
    // ---- publish the frame-level timing snapshot ----
    // ONE object (Diag::FrameTiming), read by both the in-viewport HUD and the editor's
    // Profiler, so the two readouts cannot disagree.
    // frameMs is realDt, NOT the CPU bracket: the pacing sleep and the message pump sit
    // outside the bracket, so 1000/cpuFrameMs would report several hundred fps on a
    // VSync'd frame.
    {
        auto& ft = Diag::FrameTiming::Get();
        ft.frameMs    = realDt * 1000.0f;
        ft.cpuFrameMs = Harness::FrameProbe::Get().cpuFrameMs;
        // Everything the frame did that was not render submission. Allocation-free:
        // GetDebugInfo() would heap-allocate inside the bracket being measured.
        ft.gameMs     = m_scheduler.PhaseMsExcluding(TickPhase::Render);
        // ft.gpuWaitMs was written earlier this frame (AcquireUploadSlot).
        // The rest comes from m_completedRender, the RENDER -> GAME snapshot, never from
        // m_fromRender (the lane may be writing it) or GetGPUFrameTiming / GetGPUProfiler()
        // (unsynchronised render-half fields). Under frame overlap it describes an earlier render
        // frame than this bracket (renderTimingFrame says which); GPU numbers are ~2 frames old
        // (gpuAgeFrames - never hard-code it), and gpuValid=false (profiler off) renders as "--",
        // not as a free GPU.
        // Applied in ONE place, right before Update() advances the EMAs: FrameTiming is an
        // unsynchronised singleton whose history ring belongs to this thread.
        ft.loopTopMs = loopTopMs;
        ft.renderTimingFrame = m_completedRender.frame;
        ft.renderTimingValid = m_completedRender.valid;
        ft.drawMs        = m_completedRender.drawMs;
        ft.beginWaitMs   = m_completedRender.beginWaitMs;
        ft.presentMs     = m_completedRender.presentMs;
        ft.restMs        = m_completedRender.restMs;
        ft.renderPhaseMs  = m_completedRender.renderPhaseMs;
        ft.renderThreadMs = m_completedRender.renderThreadMs;
        // No executor joins a render thread: valid=false, so every consumer prints "--"
        // instead of a confident zero (entity_churn_render asserts on that in both modes).
        ft.joinWaitMs    = 0.0f;
        ft.joinWaitValid = false;
        ft.renderMode    = m_frameOverlap ? 3u : (m_renderGraph ? 2u : 0u);

        // ---- owner-active rows ----
        // renderOwnerMs describes renderTimingFrame, the same frame as the span rows above,
        // so the two are comparable. It is read out of the accounting ring rather than
        // accumulated here, because frame F's continuations are still running when frame F's
        // bracket closes.
        Diag::OwnerAccounting::Frame ownerFrame{};
        ft.renderOwnerValid = m_completedRender.valid &&
            Diag::OwnerAccounting::Get().Read(m_completedRender.frame, ownerFrame);
        ft.renderOwnerMs = ft.renderOwnerValid
                         ? static_cast<float>(ownerFrame.ownerMs) : 0.0f;
        ft.ownerContinuationsPump  = ft.renderOwnerValid ? ownerFrame.viaPump  : 0u;
        ft.ownerContinuationsDrain = ft.renderOwnerValid ? ownerFrame.viaDrain : 0u;
#if defined(WITH_HARNESS) && WITH_HARNESS
        // ---- the main side of the accounting publication ----
        // The lane writes the accounting ring and main reads it here, so the reader side has
        // its own oracle. torn= is the direct symptom: Read() refuses a slot whose seqlock
        // moved under it, which happens only if a writer was inside the slot. pair= is the
        // shape check a tear would break first: ownerMs and the two continuation counts are
        // written by the same Charge() and must agree.
        if (m_renderOwnerVerifyEvery > 0)
        {
            static uint64_t s_reads = 0, s_ok = 0, s_pairFail = 0;
            ++s_reads;
            if (ft.renderOwnerValid)
            {
                ++s_ok;
                const uint32_t conts = ownerFrame.viaPump + ownerFrame.viaDrain;
                const bool pairOk = (conts > 0) == (ownerFrame.ownerMs > 0.0);
                if (!pairOk) ++s_pairFail;
                ENGINE_CHECK_ONCE(pairOk,
                    "[RENDEROWNERPUB] frame=%llu ownerMs=%.6f but %u continuation(s) "
                    "- the slot's fields disagree, which is what a torn read looks like",
                    static_cast<unsigned long long>(m_completedRender.frame),
                    ownerFrame.ownerMs, conts);
            }
            const uint64_t tornTotal = Diag::OwnerAccounting::Get().TornReads();
            ENGINE_CHECK_ONCE(tornTotal == 0,
                "[RENDEROWNERPUB] %llu accounting read(s) were refused mid-write "
                "- main observed the lane inside a slot",
                static_cast<unsigned long long>(tornTotal));
            if (s_reads % static_cast<uint64_t>(m_renderOwnerVerifyEvery) == 0)
                LOG_INFO("[RENDEROWNERPUB] frame=%llu reads=%llu ok=%llu torn=%llu "
                         "pairFail=%llu dropped=%llu",
                         static_cast<unsigned long long>(m_completedRender.frame),
                         static_cast<unsigned long long>(s_reads),
                         static_cast<unsigned long long>(s_ok),
                         static_cast<unsigned long long>(tornTotal),
                         static_cast<unsigned long long>(s_pairFail),
                         static_cast<unsigned long long>(
                             Diag::OwnerAccounting::Get().DroppedCharges()));
        }
#endif
        // These two are THIS bracket's, not renderTimingFrame's.
        ft.tailDrainMs     = static_cast<float>(m_tailDrainMsThisFrame);
        ft.blockingDrainMs = static_cast<float>(m_blockingDrainMsThisFrame);
        ft.parkHookCount   =
            Diag::OwnerAccounting::Get().parkHookFires.load(std::memory_order_relaxed)
            - parkHookAtBracketOpen;
        // [DRAIN] window line, on the phase log's cadence.
        {
            static const int s_drainEvery = Sys::CommandLine::Get().GetInt("-phaselog_every", 0);
            static uint64_t s_drainFrames = 0;
            ++s_drainFrames;
            if (s_drainEvery > 0 && s_drainFrames % static_cast<uint64_t>(s_drainEvery) == 0)
            {
                LogDrainStats("window", true, static_cast<uint64_t>(s_drainEvery));
                const auto& ds = renderer.GetDestroyDeferStats();
                if (renderer.DeferEntityDestroys() || renderer.DeferSyncPointCalls() ||
                    ds.immediate || ds.callsImmediate)
                    LOG_INFO("[DESTROY_DEFER] deferred=%llu applied=%llu immediate=%llu "
                             "skippedAlive=%llu peak=%zu quarantined=%zu calls=%llu "
                             "callsApplied=%llu callsImmediate=%llu callsPeak=%zu",
                             static_cast<unsigned long long>(ds.deferred),
                             static_cast<unsigned long long>(ds.applied),
                             static_cast<unsigned long long>(ds.immediate),
                             static_cast<unsigned long long>(ds.skippedAlive),
                             ds.peak, m_world.QuarantinedIdCount(),
                             static_cast<unsigned long long>(ds.callsDeferred),
                             static_cast<unsigned long long>(ds.callsApplied),
                             static_cast<unsigned long long>(ds.callsImmediate),
                             ds.callsPeak);
                // How many lane submissions the cost floor lifted into the global heap (0 per
                // window = -lanejobprioff, or no lane).
                static uint64_t s_boostedPrev = 0;
                const uint64_t boosted = TaskSystem::Get().BoostedSubmitCount();
                LOG_INFO("[LANEJOBPRIO] window boostedPerFrame=%.1f",
                         static_cast<double>(boosted - s_boostedPrev) / s_drainEvery);
                s_boostedPrev = boosted;
            }
        }
        // [WORLDHASH]: the World's structure at the end of the frame, for comparing
        // schedules (-workers=1 vs the default).
        {
            static const int s_hashEvery = Sys::CommandLine::Get().GetInt("-worldhash_every", 0);
            if (s_hashEvery > 0 && frameIndex % static_cast<uint64_t>(s_hashEvery) == 0)
            {
                size_t pools = 0;
                const uint64_t h = m_world.StructuralHash(&pools);
                LOG_INFO("[WORLDHASH] frame=%llu entities=%zu pools=%zu hash=%016llx",
                         static_cast<unsigned long long>(frameIndex),
                         m_world.GetEntities().size(), pools,
                         static_cast<unsigned long long>(h));
            }
        }

        ft.gpuValid = m_completedRender.gpuValid;
        if (ft.gpuValid)
        {
            ft.gpuBusyMs     = m_completedRender.gpuBusyMs;
            ft.gpuOverlapMs  = m_completedRender.gpuOverlapMs;
            ft.gpuAgeFrames  = m_completedRender.gpuAgeFrames;
            ft.gpuCalibrated = m_completedRender.gpuCalibrated;
        }
        // CPU critical path: the bracket minus the GPU fence wait, i.e. work rather than
        // back-pressure. Computed from the RAW pair (both describe THIS frame) and smoothed
        // by Update(); subtracting a raw wait from a smoothed bracket would mix two frames.
        ft.cpuPeriodMs = (ft.cpuFrameMs > ft.gpuWaitMs)
                       ? (ft.cpuFrameMs - ft.gpuWaitMs)
                       : ft.cpuFrameMs;
        ft.Update();

#if defined(WITH_HARNESS) && WITH_HARNESS
        // -ownertimeverify[=N]: the order/accounting oracle for the owner-active rows.
        //  1. ORDER: is every render frame's chain COMPLETE before its numbers are published?
        //     dropped= is the accounting ring overflowing (a charge for a frame whose slot a
        //     later frame took), missing= a frame published after its owner budget was
        //     recycled. Both must stay 0; dropped is also an ENGINE_CHECK, so a pack with
        //     -maxerrors=0 fails on it.
        //  2. COMPOSITION: resid = (cpu + loopTop) - (owner + game) is what neither the
        //     owner-active rows nor the non-Render phases explain (Renderer::BeginFrame /
        //     BuildRenderScene, the editor UI build, the present tail). A WINDOW MEAN, never
        //     per frame: renderOwnerMs describes renderTimingFrame and straddles two
        //     brackets (FrameTiming.h).
        // dropped= / missing= / nested= are deltas over the same window; droppedTotal= /
        // missingTotal= are the process totals. nested= / nestedMs= are the re-entrant
        // park-hook drains DrainFrameWork does not charge twice; blockMs <= cpuMs is
        // asserted, because containment is the invariant double-charging breaks.
        {
            static const int s_every = []{
                const auto& cl = Sys::CommandLine::Get();
                return cl.Has("-ownertimeverify")
                     ? (std::max)(1, cl.GetInt("-ownertimeverify", 60)) : 0;
            }();
            if (s_every > 0)
            {
                static double   s_owner = 0.0, s_cpu = 0.0, s_game = 0.0;
                static double   s_loop  = 0.0, s_tail = 0.0, s_block = 0.0;
                static double   s_nestedMs = 0.0;
                static uint64_t s_pump = 0, s_drain = 0, s_park = 0;
                static uint64_t s_nested = 0;
                static uint64_t s_frames = 0;
                // Window deltas: the totals are process-cumulative; these are the bases taken at
                // the last emit.
                static uint64_t s_missingTotal = 0, s_missingBase = 0;
                static uint64_t s_droppedBase  = 0;
                // The measured overlap window G (m_overlapWindowMsThisFrame).
                static double   s_win = 0.0;

                s_win   += m_overlapWindowMsThisFrame;
                s_owner += ft.renderOwnerMs;
                s_cpu   += ft.cpuFrameMs;
                s_game  += ft.gameMs;
                s_loop  += ft.loopTopMs;
                s_tail  += ft.tailDrainMs;
                s_block += ft.blockingDrainMs;
                s_pump  += ft.ownerContinuationsPump;
                s_drain += ft.ownerContinuationsDrain;
                s_park  += ft.parkHookCount;
                s_nested   += m_nestedDrainsThisFrame;
                s_nestedMs += m_nestedDrainMsThisFrame;
                if (m_completedRender.valid && !ft.renderOwnerValid) ++s_missingTotal;

                // CONTAINMENT. Every outermost blocking drain of this frame runs inside the
                // cpuFrameMs bracket (BeginFrameBracket resets the sums where the bracket opens), so
                // their sum can never exceed it; charging a nested park-hook drain on top of its
                // parent would. The assertion lives here because the nested count alone is usually
                // zero.
                ENGINE_CHECK_ONCE(ft.blockingDrainMs <= ft.cpuFrameMs * 1.05f + 0.5f,
                    "[OWNERTIME] blocking drain %.3f ms exceeds the %.3f ms frame "
                    "bracket that contains it (%u nested drain(s) this frame) - "
                    "a nested DrainFrameWork is being charged twice",
                    ft.blockingDrainMs, ft.cpuFrameMs, m_nestedDrainsThisFrame);

                const uint64_t droppedTotal =
                    Diag::OwnerAccounting::Get().DroppedCharges();
                ENGINE_CHECK_ONCE(droppedTotal == 0,
                    "[OWNERTIME] accounting ring overflowed: %llu charge(s) "
                    "arrived for a render frame whose slot had been reused",
                    static_cast<unsigned long long>(droppedTotal));

                if (++s_frames % s_every == 0)
                {
                    const double n = static_cast<double>(s_every);
                    LOG_INFO("[OWNERTIME] frames=%llu ownerMs=%.3f cpuMs=%.3f "
                             "gameMs=%.3f loopTopMs=%.3f resid=%.3f tailMs=%.3f "
                             "blockMs=%.3f pump=%.2f drain=%.2f park=%.2f "
                             "nested=%llu nestedMs=%.3f "
                             "dropped=%llu missing=%llu "
                             "droppedTotal=%llu missingTotal=%llu "
                             "winMs=%.3f",
                             static_cast<unsigned long long>(s_frames),
                             s_owner / n, s_cpu / n, s_game / n, s_loop / n,
                             (s_cpu + s_loop - s_owner - s_game) / n,
                             s_tail / n, s_block / n,
                             static_cast<double>(s_pump)  / n,
                             static_cast<double>(s_drain) / n,
                             static_cast<double>(s_park)  / n,
                             static_cast<unsigned long long>(s_nested),
                             s_nestedMs / n,
                             static_cast<unsigned long long>(droppedTotal - s_droppedBase),
                             static_cast<unsigned long long>(s_missingTotal - s_missingBase),
                             static_cast<unsigned long long>(droppedTotal),
                             static_cast<unsigned long long>(s_missingTotal),
                             s_win / n);
                    s_win = 0.0;
                    s_owner = s_cpu = s_game = s_loop = s_tail = s_block = 0.0;
                    s_nestedMs = 0.0;
                    s_pump = s_drain = s_park = s_nested = 0;
                    s_droppedBase = droppedTotal;
                    s_missingBase = s_missingTotal;
                }
            }
        }

        // -taskwakeverify[=N]: the order oracle for the TaskSystem wake policies (-taskspin
        // / -taskbatchpublish).
        //  1. ORDER / EXACTLY-ONCE: mismatch= counts ParallelFor groups that retired with a
        //     body count other than their chunk count; batch publish changes WHEN chunks
        //     become visible to workers, so "every chunk ran, once" is what it could break.
        //     Also an ENGINE_CHECK. overflow= must not rise either: a bulk push landing on
        //     one shard spills to the global mutex and trades one stall for another.
        //  2. COMPOSITION: chunkHelp= is the chunks a ParallelFor's own CALLER ran because no
        //     worker claimed them (a worker that never woke). Per frame over the window,
        //     like [OWNERTIME], because the counters are process-cumulative.
        {
            static const int s_every = []{
                const auto& cl = Sys::CommandLine::Get();
                return cl.Has("-taskwakeverify")
                     ? (std::max)(1, cl.GetInt("-taskwakeverify", 60)) : 0;
            }();
            if (s_every > 0)
            {
                static TaskSystem::WakeStats s_prev{};
                static uint64_t s_wakeFrames = 0;
                if (++s_wakeFrames % s_every == 0)
                {
                    auto& pool = TaskSystem::Get();
                    const auto now = TaskSystem::SampleWakeStats();
                    const double n = static_cast<double>(s_every);
                    const auto per = [&](uint64_t a, uint64_t b)
                    { return static_cast<double>(a - b) / n; };
                    const double chunks = per(now.chunkCaller, s_prev.chunkCaller)
                                        + per(now.chunkWorker, s_prev.chunkWorker);
                    ENGINE_CHECK_ONCE(now.chunkMismatch == 0,
                        "[TASKWAKE] %llu ParallelFor group(s) retired with a body "
                        "count that is not their chunk count",
                        static_cast<unsigned long long>(now.chunkMismatch));
                    LOG_INFO("[TASKWAKE] frames=%llu spinUs=%u batch=%d parks=%.2f "
                             "spinHits=%.2f spinGiveUps=%.2f batches=%.2f "
                             "batchTasks=%.2f wakeAll=%.2f wakeOne=%.2f "
                             "chunkCaller=%.2f chunkHelp=%.2f chunkWorker=%.2f "
                             "helpFrac=%.4f mismatch=%llu overflow=%llu steals=%llu",
                             static_cast<unsigned long long>(s_wakeFrames),
                             pool.SpinMicros(), pool.BatchPublishEnabled() ? 1 : 0,
                             per(now.parks,       s_prev.parks),
                             per(now.spinHits,    s_prev.spinHits),
                             per(now.spinGiveUps, s_prev.spinGiveUps),
                             per(now.batches,     s_prev.batches),
                             per(now.batchTasks,  s_prev.batchTasks),
                             per(now.wakeAll,     s_prev.wakeAll),
                             per(now.wakeOne,     s_prev.wakeOne),
                             per(now.chunkCaller, s_prev.chunkCaller),
                             per(now.chunkHelp,   s_prev.chunkHelp),
                             per(now.chunkWorker, s_prev.chunkWorker),
                             chunks > 0.0 ? per(now.chunkHelp, s_prev.chunkHelp) / chunks : 0.0,
                             static_cast<unsigned long long>(now.chunkMismatch),
                             static_cast<unsigned long long>(pool.OverflowCount()),
                             static_cast<unsigned long long>(pool.StealCount()));
                    s_prev = now;
                }
            }
        }
#endif
        // ---- -lockstats[=N] ----
        // Per-frame acquisitions, contended acquisitions and blocked time per shared lock,
        // over the last N frames. Blocking in std::mutex parks the thread, so the profiler
        // books it as idle: this is the only place that time is visible.
        if (Sys::LockStats::Armed())
        {
            static const int s_every =
                (std::max)(1, Sys::CommandLine::Get().GetInt("-lockstats", 300));
            static uint64_t s_lockFrames = 0;
            struct Snap { uint64_t acq = 0, cont = 0, ns = 0; };
            static Snap s_prev[static_cast<size_t>(Sys::LockStats::Id::Count)];
            if (++s_lockFrames % static_cast<uint64_t>(s_every) == 0)
            {
                const double n = static_cast<double>(s_every);
                for (size_t i = 0; i < static_cast<size_t>(Sys::LockStats::Id::Count); ++i)
                {
                    auto& c = Sys::LockStats::g_counters[i];
                    const Snap now{ c.acquired.load(std::memory_order_relaxed),
                                    c.contended.load(std::memory_order_relaxed),
                                    c.waitNs.load(std::memory_order_relaxed) };
                    LOG_INFO("[LOCKSTATS] frames=%llu lock=%s acqPerFrame=%.2f "
                             "contendedPerFrame=%.3f waitUsPerFrame=%.3f maxWaitUs=%.1f "
                             "contendedTotal=%llu",
                             static_cast<unsigned long long>(s_lockFrames),
                             Sys::LockStats::Name(static_cast<Sys::LockStats::Id>(i)),
                             static_cast<double>(now.acq - s_prev[i].acq) / n,
                             static_cast<double>(now.cont - s_prev[i].cont) / n,
                             static_cast<double>(now.ns - s_prev[i].ns) / n / 1000.0,
                             static_cast<double>(c.maxWaitNs.load(std::memory_order_relaxed)) / 1000.0,
                             static_cast<unsigned long long>(now.cont));
                    s_prev[i] = now;
                }
            }
        }

    }
}

void FramePipeline::DrainFrameWork(Diag::DrainCause cause)
{
    if (std::this_thread::get_id() != m_mainThreadId)
        throw std::logic_error("Frame drain requires the application owner");
    Diag::OwnerAccounting::Get().NoteDrainCall(cause);
    // -renderowner: main owns no render continuations, so "drain" means "wait for the
    // lane to publish the frame" at every call site - the Animation-boundary barrier, the
    // park hook on main, and every hard-sync path: world reload / scene load
    // (Renderer::OnWorldClear, the deferred-release drain), swap-chain resize, and
    // device-lost teardown and orderly exit through ShutdownAllSystems. The park hook
    // firing ON the lane never gets here (see the hook in Configure).
    // The lane is reached through the atomic publication with the hook-depth guard held
    // across the branch: StopRenderOwner retires the pointer, then waits for the depth to
    // reach zero before it moves, joins or destroys anything. The depth can only be
    // non-zero on main today (the first line refuses other threads), so this is a
    // structural guard: the day another thread may drain, the retirement protocol
    // already holds.
    {
        struct HookDepthGuard {
            std::atomic<int>& depth;
            explicit HookDepthGuard(std::atomic<int>& d) : depth(d)
            { depth.fetch_add(1, std::memory_order_acq_rel); }
            ~HookDepthGuard() { depth.fetch_sub(1, std::memory_order_acq_rel); }
        } hookDepth(m_renderOwnerHookDepth);
        if (RenderOwnerLane* lane = m_renderOwnerLane.load(std::memory_order_acquire))
        {
            // The lane recorded the ImGui snapshot; its buffers go back to ImGui here, on main,
            // after the lane is done with them - also when the lane's frame failed (the
            // destructor runs on the throw). A snapshot the lane has not taken yet (frozen by
            // BuildEarlyEditorUI before RenderHalf's entry drain) is kept, unless no frame will
            // ever be posted for it: this drain is unwinding, or it is the Quit drain.
            // Editor.exe only: without hooks there is no snapshot.
            struct CompleteSnapshot {
                EditorHooks* editor;
                const bool   quit;
                const int    unwindingAtEntry = std::uncaught_exceptions();
                ~CompleteSnapshot()
                {
                    if (editor)
                        editor->CompleteDeferredSnapshot(quit || std::uncaught_exceptions() > unwindingAtEntry);
                }
            } completeSnapshot{ m_editor, cause == Diag::DrainCause::Quit };
            DrainRenderOwner(*lane, cause);
            return;
        }
    }
    if (!m_activeFrameOwner) return;
    std::exception_ptr error;
    // Every continuation run under this scope is attributed to a BLOCKING drain, not an
    // opportunistic Pump, and the wall time is summed into blockingDrainMs. The scope
    // wraps RunUntil itself because this function is also the drain for the park hook
    // and the RenderHalf entry.
    // RE-ENTRANCY: a continuation RunUntil is running can touch a device resource outside
    // an ExecutionDomain::Scope; GraphicsDX12::DrainFrameIfNeeded then fires the park
    // hook, which re-enters this function while m_activeFrameOwner is still set. The
    // inner interval lies inside the outer one, so only depth 0 charges
    // blockingDrainMs; nested time is accumulated separately and reported by
    // [OWNERTIME].
    struct DrainDepthGuard {
        int& depth; const bool outermost;
        explicit DrainDepthGuard(int& d) : depth(d), outermost(d == 0) { ++d; }
        ~DrainDepthGuard() { --depth; }
    } drainDepth(m_drainDepth);
    const auto drainStart = std::chrono::steady_clock::now();
    {
        Diag::OwnerAccounting::BlockingScope blocking;
        PROF_SCOPE("App.DrainFrameWork");
        try { m_activeFrameOwner->RunUntil(m_renderFrameCompletion); }
        catch (...) { error = std::current_exception(); }
    }
    const double drainMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - drainStart).count();
    if (drainDepth.outermost)
    {
        m_blockingDrainMsThisFrame += drainMs;
        Diag::OwnerAccounting::Get().NoteDrainBlocked(cause,
            static_cast<uint64_t>(drainMs * 1.0e6));
    }
    else { m_nestedDrainMsThisFrame += drainMs; ++m_nestedDrainsThisFrame; }
    m_activeFrameOwner = nullptr;
    if (m_renderFrameToken) {
        if (Sys::CommandLine::Get().Has("-materialcpuverify"))
            LOG_INFO("[FRAME_EXECUTOR] frame=%llu dedicated=0 complete=1",
                static_cast<unsigned long long>(m_fromRender.frame));
        FrameControl(m_device).RetireDeviceExecution(*m_renderFrameToken);
        m_renderFrameToken.reset();
    }
    if (m_editor) m_editor->DiscardSnapshot();
    if (error) std::rethrow_exception(error);
}

void FramePipeline::PumpFrameWork()
{
    if (!m_activeFrameOwner) return;
    PROF_SCOPE("App.PumpFrameWork");
    m_activeFrameOwner->Pump();
    if (m_renderFrameCompletion.Ready()) DrainFrameWork(Diag::DrainCause::Pump);
}

// Pairs each value the packet carried (snapshotted on main at the hand-off) with the
// one the emit path latched while consuming the plan; see the declaration.
void FramePipeline::VerifyEmitLease(Renderer& renderer, const FramePacket& pkt)
{
    uint32_t batchPlanGen = pkt.batchPlanGen;
#if WITH_HARNESS
    // The POSITIVE CONTROL: without it "the check never fired" and "the check cannot
    // fire" read identically in every log.
    static const int s_selfTestFrame = []{
        const auto& cl = Sys::CommandLine::Get();
        return cl.Has("-emitleaseselftest") ? cl.GetInt("-emitleaseselftest", 0) : -1;
    }();
    if (s_selfTestFrame >= 0 && pkt.frame == static_cast<uint64_t>(s_selfTestFrame))
    {
        const uint32_t detected = renderer.EmitLeaseMismatch(
            pkt.uploadSlot, pkt.emitIdx, pkt.batchPlanGen + 1u, pkt.sortGen);
        const uint32_t clean = renderer.EmitLeaseMismatch(
            pkt.uploadSlot, pkt.emitIdx, pkt.batchPlanGen, pkt.sortGen);
        // detected= must name the batchPlan bit and NOTHING else, and clean=
        // must be 0: a predicate that answered "mismatch" for every input
        // would pass the first half of this on its own.
        LOG_INFO("[EMITLEASE_SELFTEST] frame=%llu corrupted=1 detected=%u "
                 "expected=%u clean=%u match=%u",
                 static_cast<unsigned long long>(pkt.frame), detected,
                 unsigned(Renderer::kEmitLeaseBatchPlan), clean,
                 unsigned(detected == Renderer::kEmitLeaseBatchPlan && clean == 0u));
        // Then let the REAL check see the corrupted value, so the failure path
        // below is executed rather than reasoned about.
        batchPlanGen = pkt.batchPlanGen + 1u;
    }
#endif
    const uint32_t mask = renderer.EmitLeaseMismatch(
        pkt.uploadSlot, pkt.emitIdx, batchPlanGen, pkt.sortGen);
    ENGINE_CHECK_ONCE(mask == 0,
        "[EMITLEASE] frame %llu: the packet's emit lease does not describe the "
        "frame the emit path built (mask=0x%X; uploadSlot=%u emitIdx=%u "
        "batchPlanGen=%u sortGen=%u) - the render half is about to consume "
        "another frame's instance, material and permutation buffers",
        static_cast<unsigned long long>(pkt.frame), mask,
        pkt.uploadSlot, pkt.emitIdx, batchPlanGen, pkt.sortGen);
}

void FramePipeline::RenderHalf(IGraphicsDevice& backend, Renderer& renderer, const FramePacket& pkt)
{
    // Check the lease at its consume point, which -brslane moves: batchPlanGen and
    // sortGen are compared with what the emit nest latched (m_batchPlanGenConsumed /
    // m_sortIdxGenConsumed). On a lane-emit frame the nest has not run yet - it runs on
    // the lane after the post - so checking here would compare this packet with the
    // previous frame's latch and flag a good packet; the lane calls VerifyEmitLease
    // itself after PrepareFrameOnLane. Per frame (pkt.laneEmit): a refused-batch-plan
    // frame emitted on main, so here is its consume point.
    if (!pkt.laneEmit) VerifyEmitLease(renderer, pkt);
    DrainFrameWork(Diag::DrainCause::RenderHalf);
    if (m_frameOverlap) m_completedRender = m_fromRender;
    // ---- -renderowner: hand the packet over and go straight back to the game loop ----
    // Nothing renderer-side is touched on main after the post until the next blocking
    // drain, and the drain above guarantees no frame is in flight, so the hand-off needs
    // no lock beyond the lane's own mailbox.
    if (m_renderOwnerEnabled)
    {
        StartRenderOwner(backend, renderer);
        FramePacket lanePacket = pkt;
        // (Editor.exe only - the hooks are the editor's.)
        if (m_editor)
        {
        // The editor UI is built on main, before the post. The drain above left the lane
        // idle and main holds no device-execution token (the lane issues its own), so the
        // UI build's device work - ImGui texture updates, preview requests, uploads on the
        // game context - runs with no frame in flight. The draw data is frozen into the
        // snapshot; the lane records it (EditorImGui.CommitSnapshot) and main completes it
        // on its next drain. Always before the post, whatever m_frameOverlap says:
        // otherwise BuildEditorUI - and ImGui - would run on the lane.
        if (m_earlyUiBuilt)
        {
            // Built and frozen at the top of the Render phase (BuildEarlyEditorUI).
            m_earlyUiBuilt = false;
            lanePacket.allowOverlap = m_earlyUiAllowOverlap;
            lanePacket.uiPrepared = true;
            lanePacket.earlyUiMs = m_earlyUiMs;
        }
        else
        {
            ENGINE_CHECK_ONCE(!m_editorUiEarly,
                "[EDITORUI] early UI mode but no UI was built before BeginFrame - "
                "building it late, after the scene capture");
            ENGINE_CHECK_ONCE(!Sys::ExecutionDomain::HasActiveScope(),
                "[RENDEROWNER] editor UI build with an execution scope open on main");
            Sys::ThreadCtx::Scope uiContext(Sys::ThreadCtx::Render);
            const auto uiStart = std::chrono::steady_clock::now();
            m_editor->BuildUI(backend, renderer, pkt.world, EditorHooks::kSceneTexturePlaceholder, nullptr);
            lanePacket.allowOverlap = m_editor->PrepareSnapshot(pkt.frame);
            lanePacket.uiPrepared = true;
            lanePacket.earlyUiMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - uiStart).count();
        }
        }
#if defined(WITH_HARNESS)
        // -ui_screenshot, before the post. The request is recorded by the NEXT EndFrame: on
        // the serial path that is this frame's (its tail runs after PostRender, where
        // HarnessCaptureSystem asks), but the lane may reach EndFrame(F) before main reaches
        // PostRender(F), so the shot would land on F or F+1 by timing. Asking here, while the
        // lane is idle, makes it frame F on both paths. Both hosts.
        if (m_harnessCapture) m_harnessCapture->ServiceUiShotsBeforePost(pkt.frame);
#endif
        m_renderRecordingReady = false;
        m_renderOwnerRecordingReady.store(false, std::memory_order_relaxed);
        // The lane writes the latch and nobody reads it (main pumps no render work under
        // -renderowner); it is set so the callback contract is the same on both paths.
        renderer.SetRecordingReadyCallback([this] {
            m_renderOwnerRecordingReady.store(true, std::memory_order_release);
        });
        PostRenderOwnerFrame(lanePacket);
        // allowOverlap=false means the frame may not straddle the seam (an
        // ImGui draw callback in the editor): wait for the lane right away.
        if (!m_frameOverlap || !lanePacket.allowOverlap) DrainFrameWork(Diag::DrainCause::RenderHalf);
        return;
    }
    if (!m_frameOwnerQueue) m_frameOwnerQueue = std::make_unique<Sys::OwnerTaskQueue>();
    auto& owner = *m_frameOwnerQueue;
    IDeviceFrameControl& device = FrameControl(backend);
    std::optional<Sys::ExecutionDomain::Token> token;
    token = device.IssueDeviceExecution(pkt.frame);
    m_renderFrameToken = token;
    Sys::ThreadCtx::Scope context(Sys::ThreadCtx::Render);
    FramePacket renderPacket = pkt;
    try {
    if (m_editor && m_frameOverlap) {
        Sys::ExecutionDomain::Scope authority(*token);
        const auto uiStart = std::chrono::steady_clock::now();
        m_editor->BuildUI(backend, renderer, pkt.world, EditorHooks::kSceneTexturePlaceholder, nullptr);
        renderPacket.allowOverlap = m_editor->PrepareSnapshot(pkt.frame);
        renderPacket.uiPrepared = true;
        renderPacket.earlyUiMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - uiStart).count();
    }
    m_renderRecordingReady = false;
    renderer.SetRecordingReadyCallback([this] { m_renderRecordingReady = true; });
    // Arm this frame's owner budget, then stamp the ROOT node with it. Every
    // continuation the chain schedules inherits the tag from the node it was
    // submitted inside, so the tail segments that run during frame F+1's game
    // phases stay charged to F. Diagnostics/OwnerAccounting.h explains why the
    // inheritance is sound (await_suspend runs on this thread).
    Diag::OwnerAccounting::Get().BeginRenderFrame(pkt.frame);
    m_renderFrameCompletion = [&] {
        Diag::OwnerAccounting::TagScope tag(pkt.frame + 1);
        return RenderHalfAsync(backend, renderer, renderPacket, owner, token).Start(owner, token, "App.RenderFrame");
    }();
    m_activeFrameOwner = &owner;
    if (m_frameOverlap && renderPacket.allowOverlap) {
        while (!m_renderRecordingReady && !m_renderFrameCompletion.Ready())
            if (!owner.Pump() && !owner.WaitForReady() && !m_renderFrameCompletion.Ready())
                throw std::logic_error("Frame recording publication lost its continuation");
        if (Sys::CommandLine::Get().Has("-materialcpuverify"))
            LOG_INFO("[FRAME_RECORD_READY] frame=%llu pending=%d", static_cast<unsigned long long>(pkt.frame), !m_renderFrameCompletion.Ready());
        if (m_renderFrameCompletion.Ready()) DrainFrameWork(Diag::DrainCause::RenderHalf);
    } else DrainFrameWork(Diag::DrainCause::RenderHalf);
    } catch (...) {
        auto error = std::current_exception();
        if (m_activeFrameOwner) { try { DrainFrameWork(Diag::DrainCause::RenderHalf); } catch (...) {} }
        else if (m_renderFrameToken) {
            device.RetireDeviceExecution(*m_renderFrameToken);
            m_renderFrameToken.reset();
            if (m_editor) m_editor->DiscardSnapshot();
        }
        std::rethrow_exception(error);
    }
}

Sys::OwnerTask FramePipeline::RenderHalfAsync(IGraphicsDevice& backend, Renderer& renderer,
    FramePacket pkt, Sys::OwnerTaskQueue& owner, std::optional<Sys::ExecutionDomain::Token> token)
{
    // Main, or the -renderowner lane: both are non-worker owner threads. The check
    // guards "not a CPU worker and not some unrelated thread", which is why the lane is
    // named explicitly rather than relaxing it to !TaskSystem::OnWorkerThread(). Read
    // m_renderOwnerThreadId, not m_renderOwner: this body runs ON the lane, and the
    // unique_ptr is main's to move.
    ENGINE_CHECK_ONCE(std::this_thread::get_id() == m_mainThreadId ||
                      std::this_thread::get_id() ==
                          m_renderOwnerThreadId.load(std::memory_order_acquire),
        "[FRAME_EXECUTOR] owner continuation executed on the wrong thread");

        // The packet owns this frame's latency token across coroutine awaits.
        {
            auto& sl = SL::StreamlineBackend::Get();
            sl.PublishRenderToken(pkt.slToken);
            // Opens the render-submit span on the same thread as the RenderSubmitEnd that
            // closes it: the pair must not straddle the seam.
            sl.SetLatencyMarker(SL::LatencyMarker::RenderSubmitStart,
                                sl.RenderFrameToken());
        }

        // Name the frame BEFORE opening the device frame: BeginFrame derives the
        // render upload slot from it, and reading the game thread's counter
        // there would read whatever the game half has already moved on to.
        FrameControl(backend).SetRenderFrame(pkt.frame);

        // A reader asked for a frame capture (-rgdump, the `rg` verb): journal this whole
        // device frame. Armed before BeginFrame so the primary list's first barrier is in
        // it; published after EndFrame.
        const bool frameCapture = renderer.ArmFrameCapture();

        // BeginFrame does a pipelined wait + drains this backbuffer's
        // deferred-release slot. Any Destroy/Release after this point queues
        // for a future BeginFrame and is safe regardless of in-flight GPU.
        const auto _t0b = std::chrono::steady_clock::now();
        RHI::CommandList primaryCL = backend.BeginFrame();
        m_renderSplit.beginMs += std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - _t0b).count();


        const auto _t2 = std::chrono::steady_clock::now();
        RHI::CommandList lastPassCL{};
        co_await renderer.RenderAsync(owner, token, lastPassCL).Start(owner, token, "Renderer.Render");
        const auto _t3 = std::chrono::steady_clock::now();
        // The "Draw" row: Renderer::BeginFrame (ECS -> DrawPackets, the packet's rBeginMs) +
        // Render (graph record + submit). Consumed next frame: this frame's HUD was already
        // emitted. The game term comes from the packet, never from a game-half local.
        // Into the MAILBOX, not straight into Diag::FrameTiming: FrameTiming is an
        // unsynchronised singleton whose EMAs and history ring the game thread advances in
        // Update(), and the game half applies the mailbox just before Update().
        m_fromRender.drawMs =
            static_cast<float>(
                pkt.rBeginMs +
                std::chrono::duration<double, std::milli>(_t3  - _t2 ).count());
        if (lastPassCL.IsValid())
            backend.AddCommandListDependency(primaryCL, lastPassCL);

        // ---- The secondary views (Renderer_Views.cpp): the same passes again, after the main view ----
        // Each view's post is ordered behind the main view's last list (and the previous view's);
        // the primary list, whose ImGui samples their pictures, behind the last view's.
        {
            RHI::CommandList viewsLastCL{};
            co_await renderer.RenderViewsAsync(owner, token, lastPassCL, viewsLastCL)
                .Start(owner, token, "Renderer.RenderViews");
            if (viewsLastCL.IsValid())
                backend.AddCommandListDependency(primaryCL, viewsLastCL);
        }

        // ---- material preview: its own edge to primaryCL ----
        // Not by riding the graph tail: Renderer::Render can return an INVALID lastPassCL (it
        // falls back to shadowCL, which may also be invalid), and a preview spliced into
        // that chain would then have no ordering against the ImGui pass that samples it.
        // The reverse edge (colorLastCL depending on the preview) would put Hi-Z, SSR and
        // preComputeCL behind a preview panel. ImGui records into primaryCL and EndFrame
        // topologically sorts on wait_for, so this is a real GPU guarantee, not submit-order
        // luck.
        const RHI::CommandList previewCL = renderer.TakePreviewCommandListThisFrame();
        if (previewCL.IsValid())
            backend.AddCommandListDependency(primaryCL, previewCL);

        // Post-graph: composite to the display. GAME renders only the scene, so it binds the
        // back buffer directly and the composite triangle IS the display encode. EDITOR also
        // needs ImGui's chrome, and ImGui's RTV format cannot change at runtime (see
        // ImGuiManager), so it composes into a fixed-format FP16 buffer and resolves that
        // through the same encode. Both paths end in the same shader.
        // (EDITOR = the editor's hooks are installed: Editor.exe, and only it.)
        if (m_editor)
            backend.MainSurface().SetRenderTargetToPresentComposition(kClearColor, primaryCL);
        else
            backend.MainSurface().SetRenderTargetToSwapChain(kClearColor, primaryCL);
        // NOTE the fallback: without a ToneMapPass output sceneTexId is the raw HDR scene,
        // scene-referred LINEAR rather than the extended-gamma the encode expects, so it
        // looks wrong under HDR. That only happens for a renderer without a tonemap stage;
        // it is left as-is rather than silently tone-mapped.
        ENGINE_CHECK_ONCE(backend.HdrScopeDepth() == 0,
                          "App editor-viewport GetHdrSceneSrvGpuHandle: HDR access inside an HDR push scope (depth=%d)",
                          backend.HdrScopeDepth());
        const uint64_t sceneTexId = renderer.GetFinalOutputSrvHandle()
                                    ? renderer.GetFinalOutputSrvHandle()
                                    : backend.GetHdrSceneSrvGpuHandle();
        if (m_editor)
        {
        // The late UI build, on main: only when nothing prepared the UI before the frame
        // started (no lane and no frame overlap).
        if (!pkt.uiPrepared)
        {
            // Never on the render-owner lane: RenderHalf prepares the UI on
            // main before every post (ImGui is main-thread bound).
            ENGINE_CHECK_ONCE(std::this_thread::get_id() == m_mainThreadId,
                "[RENDEROWNER] editor UI build reached the lane (uiPrepared=0)");
            m_editor->BuildUI(backend, renderer, pkt.world, sceneTexId, nullptr);
        }
        if (!m_renderGraph) m_editor->EndUIFrame(primaryCL);
        if (m_renderGraph)
            co_await m_editor->EndUIFrameAsync(primaryCL, pkt.frame, sceneTexId).Start(owner, token, "EditorImGui.CommitSnapshot");

        // Everything ImGui just drew lives in the composition buffer. Resolve
        // it to the back buffer through the display encode (a passthrough in
        // SDR, PQ or scRGB otherwise).
        backend.MainSurface().ResolvePresentCompositionToSwapChain(primaryCL);
        }
        else
        {
        // Game build: no ImGui - the composite triangle reads the tonemapped composition
        // buffer and performs the display encode into the back buffer.
        backend.MainSurface().CompositeTextureToSwapChain(sceneTexId, primaryCL);

        // ---- the in-game picture-in-picture ----
        // After the full-screen composite (it overwrites its corner rect) and after the
        // previewCL dependency edge above, which makes the pixels it samples finished ones
        // rather than a half-recorded target. Same PSO and display encode: the inset is the
        // same triangle with a different viewport.
        // Opt-in: with no Renderer::SetGamePiP call this is one accessor that returns false,
        // which preview_view_game asserts on every frame.
        {
            uint64_t pipSrv = 0;
            int32_t  px = 0, py = 0, pw = 0, ph = 0;
            if (renderer.GetGamePiPComposite(pipSrv, px, py, pw, ph))
                backend.MainSurface().CompositeTextureToSwapChainRect(pipSrv, px, py, pw, ph,
                                                        primaryCL);
        }
        }

        // Closes the render-submit span. EndFrame below owns the Present
        // markers, because Present is where Frame Generation takes over and the
        // pacer needs that pair to be exactly around the real Present call.
        SL::StreamlineBackend::Get().SetLatencyMarker(
            SL::LatencyMarker::RenderSubmitEnd,
            SL::StreamlineBackend::Get().RenderFrameToken());

        const auto _t4 = std::chrono::steady_clock::now();
        backend.EndFrame();
        if (frameCapture) renderer.PublishFrameCapture();
        {
            using ms = std::chrono::duration<double, std::milli>;
            const auto _t5 = std::chrono::steady_clock::now();
            // The game half's term arrives in the packet and is summed here, on the render half,
            // which also reads and resets these sums below; summing it on main would race the
            // lane's read and Reset() under -renderowner.
            m_renderSplit.rBeginMs += pkt.rBeginMs;
            m_renderSplit.renderMs += ms(_t3 - _t2).count();
            m_renderSplit.endMs    += ms(_t5 - _t4).count();
            // rest = the span between renderer.Render and EndFrame plus pkt.earlyUiMs, the editor UI
            // build main did before the frame started. Renderer::BeginFrame is above the seam and
            // reported on its own, so nothing is subtracted.
            m_renderSplit.restMs   += pkt.earlyUiMs + ms(_t4 - _t3).count();

            // ---- split baseline rows ----
            // The same spans [RENDERSPLIT] averages, published per frame so the HUD / Profiler /
            // frames.jsonl read one definition. `rest` is included deliberately: it carries the
            // editor UI build and record, the present composition and the PiP, so a row that omitted
            // it would not be comparable across executor shapes.
            {
                // beginWait runs from the device BeginFrame to the start of Render, the render
                // half's own top; Renderer::BeginFrame is above the seam and not inside it.
                const float beginWait = static_cast<float>(ms(_t2 - _t0b).count());
                // Like drawMs: the game term arrives through the packet, so this sum stays one
                // frame's sum.
                const float rBegin    = static_cast<float>(pkt.rBeginMs);
                const float render    = static_cast<float>(ms(_t3 - _t2).count());
                const float present   = static_cast<float>(ms(_t5 - _t4).count());
                const float rest      = static_cast<float>(pkt.earlyUiMs + ms(_t4 - _t3).count());
                // RENDER -> GAME, filled where the values are PRODUCED: the game half reads only the
                // m_completedRender copy made once the frame is complete, never render-half state
                // (GetGPUFrameTiming / GetGPUProfiler() hand out unsynchronised render-half fields).
                m_fromRender.beginWaitMs   = beginWait;
                m_fromRender.renderMs      = render;
                m_fromRender.restMs        = rest;
                m_fromRender.presentMs     = present;
                // drawMs was filled right after Render() returned. renderPhaseMs carries rBegin, a
                // GAME-half span, so it is the phase's cost and NOT this thread's; renderThreadMs
                // beside it is the per-thread number.
                m_fromRender.renderPhaseMs  = beginWait + rBegin + render + rest + present;
                // Under -renderowner the editor UI (pkt.earlyUiMs) was built on
                // MAIN before the post: this phase's cost, not this thread's.
                const float uiOnMain = m_renderOwnerEnabled ? static_cast<float>(pkt.earlyUiMs) : 0.f;
                m_fromRender.renderThreadMs = beginWait + render + (rest - uiOnMain) + present;
                {
                    float gMs = 0.f, gOverlap = 0.f; uint32_t gAge = 0;
                    m_fromRender.gpuValid      = backend.GetGPUFrameTiming(gMs, gOverlap, gAge);
                    m_fromRender.gpuBusyMs     = gMs;
                    m_fromRender.gpuOverlapMs  = gOverlap;
                    m_fromRender.gpuAgeFrames  = gAge;
                    RHI::GpuTimingView timings;
                    m_fromRender.gpuCalibrated =
                        backend.GetGPUTimings(timings) && timings.calibrationValid;
                }
                m_fromRender.frame = pkt.frame;
                m_fromRender.valid = true;
                // No executor joins a render thread, so the join wait has no producer: valid=false
                // makes every consumer print "--" instead of a confident zero (entity_churn_render
                // asserts on that). PublishFrameTiming applies the same on the game side.
                m_fromRender.joinWaitMs    = 0.f;
                m_fromRender.joinWaitValid = false;
                // The RESOLVED mode (0 serial, 2 graph, 3 graph with frame overlap), as frames.jsonl
                // and Debug.Stats().renderMode report it.
                m_fromRender.renderMode    = m_frameOverlap ? 3u : (m_renderGraph ? 2u : 0u);
            }

            // Proof of life for the render-context tripwire, once per run: without it "zero
            // violations" and "the check never armed" are indistinguishable in a log (the same
            // reason RenderGraph prints its RG_VALIDATE_ACCESS ACTIVE line).
            static bool s_ctxAnnounced = false;
            if (!s_ctxAnnounced)
            {
                s_ctxAnnounced = true;
                LOG_INFO("[SPLITCHECK] RenderCtx tripwire ACTIVE level=%d "
                         "exemptEntries=%u violations(frame 0)=%u",
                         Sys::ThreadCtx::Level(),
                         Sys::ThreadCtx::ExemptEntries().load(),
                         Sys::ThreadCtx::Violations().load());
            }
            static const bool s_split = Sys::CommandLine::Get().Has("-phaselog_every");
            if (s_split && ++m_renderSplit.frames % 120 == 0)
            {
                LOG_INFO("[RENDERSPLIT] over 120 frames: BeginFrame(GPU wait) %.2f ms | "
                         "Renderer::BeginFrame %.2f ms | renderer.Render %.2f ms | EndFrame(present) %.2f ms | rest %.2f ms",
                         m_renderSplit.beginMs / 120.0, m_renderSplit.rBeginMs / 120.0, m_renderSplit.renderMs / 120.0,
                         m_renderSplit.endMs / 120.0, m_renderSplit.restMs / 120.0);
                m_renderSplit.Reset();
            }
        }
}

// ==== the loop's calls, in the order App::Run makes them ====
void FramePipeline::LogGameBegin(uint64_t frame)
{
    if (m_frameOverlap && Sys::CommandLine::Get().Has("-materialcpuverify"))
        LOG_INFO("[FRAME_GAME_BEGIN] frame=%llu renderPending=%d",
            static_cast<unsigned long long>(frame),
            m_renderOwnerLane.load(std::memory_order_acquire)
                ? RenderOwnerFrameInFlight()
                : (m_activeFrameOwner != nullptr && !m_renderFrameCompletion.Ready()));
}

void FramePipeline::CompatibilityBoundary()
{
    ZoneScopedN("App.RenderPark.CompatibilityBoundary");
    PROF_SCOPE("App.RenderPark.CompatibilityBoundary");
    if (!m_frameOverlap) DrainFrameWork(Diag::DrainCause::Barrier);
    else PumpFrameWork();
}

void FramePipeline::BeginFrameBracket()
{
    m_tailDrainMsThisFrame     = 0.0;
    m_blockingDrainMsThisFrame = 0.0;
    m_nestedDrainMsThisFrame   = 0.0;
    m_nestedDrainsThisFrame    = 0;
    m_parkHookAtBracketOpen =
        Diag::OwnerAccounting::Get().parkHookFires.load(std::memory_order_relaxed);
}

void FramePipeline::AnimationBoundaryBarrier()
{
    if (!m_animOverlapEnabled)
    {
        const auto tailStart = std::chrono::steady_clock::now();
        // Close the overlap window G here, BEFORE the drain, so it is the window main actually
        // had and not the window plus the wait. tailDrainMs is the other half: with G measured,
        // the wait omega = max(0, L - G) (L = the lane's frame) is checked, not used to infer G.
        m_overlapWindowMsThisFrame =
            (m_renderOwnerPostAt.time_since_epoch().count() != 0)
                ? std::chrono::duration<double, std::milli>(
                      tailStart - m_renderOwnerPostAt).count()
                : 0.0;
        DrainFrameWork(Diag::DrainCause::Barrier);
        m_tailDrainMsThisFrame += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - tailStart).count();
    }
}

void FramePipeline::AcquireUploadSlot(IGraphicsDevice& backend, Renderer& renderer,
                                      const GameModeContext& ctx)
{
    // Read-only visibility jobs can run while the previous GPU tenant retires.
    // Keep acquisition before the scheduler: other Animation systems may write rings.
    const bool asyncSlot = !Sys::CommandLine::Get().Has("-framepreparelegacy") &&
        backend.IsSlotKeyedByFrame();
    Sys::TaskHandle slotAcquired;
    struct SlotAcquireDrain {
        Sys::OwnerTaskQueue* queue = nullptr;
        Sys::TaskHandle& completion;
        ~SlotAcquireDrain() {
            if (queue && !completion.Ready())
                try { queue->RunUntil(completion); } catch (...) {}
        }
    } slotDrain{nullptr, slotAcquired};
    if (asyncSlot) {
        ZoneScopedN("FrameSlot.RequestRetirement");
        if (!m_frameOwnerQueue) m_frameOwnerQueue = std::make_unique<Sys::OwnerTaskQueue>();
        slotAcquired = FrameControl(backend).AcquireUploadSlotAsync(ctx.frame)
            .Start(*m_frameOwnerQueue, {}, "FrameSlot.OwnerResume");
        slotDrain.queue = m_frameOwnerQueue.get();
        // Register the GPU dependency before independent CPU preparation.
        m_frameOwnerQueue->Pump();
    }
    Anim::AnimProfile::Get().visibilityPrepassMs = 0.f;
    if (!Sys::CommandLine::Get().Has("-framepreparelegacy") && backend.IsSlotKeyedByFrame())
    {
        ZoneScopedN("Anim.VisibilityPrepass");
        Anim::StageTimer prepass;
        renderer.PrepareAnimationVisibility(m_world, ctx.frame);
        Anim::AnimProfile::Get().visibilityPrepassMs = prepass.Ms();
    }
    // Drain the upcoming slot's GPU fence BEFORE Animation: Animation writes per-frame
    // upload-buffer slots the GPU may still be reading from kFrameCount frames ago, and
    // backend.BeginFrame then skips its own wait. Zoned because it is a GPU fence wait
    // inside the frame's otherwise uninstrumented time.
    {
        ZoneScopedN("WaitFrameSlot");
        PROF_WAIT(WaitGpu, "WaitFrameSlot");
        // Timed as well as zoned: this wait is INSIDE the CPU bracket, so a GPU-bound frame
        // inflates what everyone reads as "CPU time"; the stat-unit HUD subtracts gpuWaitMs
        // and shows it as its own row.
        const auto _tw0 = std::chrono::steady_clock::now();
        // An explicit frame, not "whatever the swap chain says next": this is the game side
        // claiming the upload slot it is about to write, and ctx.frame is the only number
        // that means the same thing once the render half runs a frame behind. Under
        // -slotkey=dxgi AcquireUploadSlot falls through to WaitForNextFrameSlot.
        if (asyncSlot) m_frameOwnerQueue->RunUntil(slotAcquired);
        else backend.AcquireUploadSlot(ctx.frame);
        slotDrain.queue = nullptr;
        const auto _tw1 = std::chrono::steady_clock::now();
        Diag::FrameTiming::Get().gpuWaitMs =
            std::chrono::duration<float, std::milli>(_tw1 - _tw0).count();
    }
}

// ==== the Render phase, in the order App::RenderPhase calls these ====
void FramePipeline::RenderPhaseBarrier()
{
    m_animBarrierWaitMsThisFrame = 0.0;
    if (m_animOverlapEnabled)
    {
        const auto barrierStart = std::chrono::steady_clock::now();
        m_overlapWindowMsThisFrame =
            (m_renderOwnerPostAt.time_since_epoch().count() != 0)
                ? std::chrono::duration<double, std::milli>(
                      barrierStart - m_renderOwnerPostAt).count()
                : 0.0;
        DrainFrameWork(Diag::DrainCause::Barrier);
        m_animBarrierWaitMsThisFrame = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - barrierStart).count();
        m_tailDrainMsThisFrame += m_animBarrierWaitMsThisFrame;
    }
    else
        DrainFrameWork(Diag::DrainCause::Barrier);
}

void FramePipeline::VerifyAnimOverlap(IGraphicsDevice& backend, Renderer& renderer,
                                      const FrameContext& frameCtx)
{
    (void)backend; (void)renderer; (void)frameCtx;   // Retail: the oracle is compiled out
#if defined(WITH_HARNESS) && WITH_HARNESS
    // ---- -animoverlapverify: the moved barrier's oracle ----
    // Emitted only here: the lane is quiescent (the drain above just returned) and the
    // game half has claimed frame F's upload slot, so both halves' slots are stable.
    //   slotsDistinct  the slot the game claimed for F is not the one the last render
    //                  used. True in both arms (m_uploadSlotRender is republished in
    //                  backend.BeginFrame, below this point), which gives the field a
    //                  control; with the barrier moved, EQUAL would mean the claim
    //                  landed on the ring the lane is reading. kFramesInFlight=3 keeps
    //                  them apart.
    //   conflicts      render-half GetUploadSlot() calls (GraphicsDX12's mirror audit).
    //                  Every ring the render half reads must resolve "current" through
    //                  GetFrameIndex(); with the barrier moved GetUploadSlot() reads one
    //                  slot ahead.
    //   skinHalves     SkinJobsWrite() (BuildSkinJobs, inside the window) and
    //                  SkinJobsRead() (SkinningPass, on the lane) must never be the same
    //                  vector.
    //   ctxViolations  the RENDERCTX tripwire: any World pool touched from a
    //                  Render-context thread.
    //   parkHooks      a World::DestroyEntity inside the window fires
    //                  DrainFrameIfNeeded on main and collapses that frame's overlap.
    //                  Not an error, but a gain measurement must be able to see it.
    // Armed by the flag alone, not by m_animOverlapEnabled, so a baseline arm prints the
    // same line and conflicts=0 is a comparison. Frame 0 is excluded: nothing has
    // rendered, so m_uploadSlotRender is still its initial 0.
    if (m_animOverlapVerifyEvery > 0 && frameCtx.frameIndex > 0 &&
        (frameCtx.frameIndex % static_cast<uint64_t>(m_animOverlapVerifyEvery)) == 0)
    {
        IGraphicsDevice& dev = backend;
        const uint32_t gameSlot   = dev.GetUploadSlot();
        const uint32_t renderSlot = dev.GetFrameIndex();
        const uint32_t conflicts  = FrameControl(backend).GameSlotReadsFromRender();
        const unsigned skinHalves = renderer.SkinJobHalvesDistinct() ? 1u : 0u;
        const uint32_t ctxViol    = Sys::ThreadCtx::Violations().load(
                                        std::memory_order_relaxed);
        const unsigned slotsDistinct =
            (!dev.IsSlotKeyedByFrame() || gameSlot != renderSlot) ? 1u : 0u;
        const unsigned match = (conflicts == 0 && slotsDistinct == 1 &&
                                skinHalves == 1 && ctxViol == 0) ? 1u : 0u;
        LOG_INFO("[ANIM_OVERLAP] frame=%llu moved=%u barrierWaitMs=%.3f winMs=%.3f "
                 "gameSlot=%u renderSlot=%u slotsDistinct=%u skinHalves=%u "
                 "conflicts=%u ctxViolations=%u parkHooks=%llu match=%u",
                 static_cast<unsigned long long>(frameCtx.frameIndex),
                 unsigned(m_animOverlapEnabled),
                 m_animBarrierWaitMsThisFrame, m_overlapWindowMsThisFrame,
                 gameSlot, renderSlot, slotsDistinct, skinHalves,
                 conflicts, ctxViol,
                 static_cast<unsigned long long>(
                     Diag::FrameTiming::Get().parkHookCount),
                 match);
        ENGINE_CHECK(conflicts == 0,
            "[ANIM_OVERLAP] %u render-half GetUploadSlot() call(s): the render half is "
            "reading the GAME slot, which -animoverlap moves out from under it. See the "
            "[SPLITAUDIT][GAMESLOT] lines for the call sites.", conflicts);
        ENGINE_CHECK(slotsDistinct == 1,
            "[ANIM_OVERLAP] frame %llu has gameSlot == renderSlot == %u: the game half's "
            "claim is the slot the last render used. Under -animoverlap that acquire "
            "happened while the lane was still reading it.",
            static_cast<unsigned long long>(frameCtx.frameIndex), gameSlot);
        ENGINE_CHECK(skinHalves == 1,
            "[ANIM_OVERLAP] skin job write and read halves are the same buffer");
    }
#endif
}

// With the emit half on the lane (m_editorUiEarly) the editor UI is built here: the
// lane is idle (the Render-phase drain), the pick has resolved, ProcessPendingActions
// has run and Renderer::BeginFrame has NOT - so every renderer write the UI makes
// (Project Settings toggles, material-shader registration, preview requests) is seen
// by this frame's scene build on main and by the lane's plan + nest. Its camera is
// THIS frame's (GetGameView: BuildGameView ran in the Animation window; FrameView
// still holds the previous one), and it runs before the selection is pushed, so a
// click outlines this frame. RenderHalf posts the snapshot frozen here.
void FramePipeline::BuildEarlyEditorUI(IGraphicsDevice& backend, Renderer& renderer, World& world,
                                       const GameModeContext& ctx)
{
    // m_editorUiEarly is only ever set on the editor host, which installs the hooks.
    if (m_editor && m_editorUiEarly)
    {
        ENGINE_CHECK_ONCE(!Sys::ExecutionDomain::HasActiveScope(),
            "[EDITORUI] early UI build with an execution scope open on main");
        Sys::ThreadCtx::Scope uiContext(Sys::ThreadCtx::Render);
        const auto uiStart = std::chrono::steady_clock::now();
        // The editor reads cameraForward / cameraPosition / viewMatrix / projMatrixNoJitter,
        // and Ray::FromViewport viewProjMatrix: the game view's un-jittered product is the
        // right one for a picking ray (the jittered one is off by up to half a pixel).
        RenderView uiView = renderer.GetGameView();
        uiView.viewProjMatrix = uiView.viewProjMatrixNoJitter;
        m_editor->BuildUI(backend, renderer, &world, EditorHooks::kSceneTexturePlaceholder,
                          &uiView);
        m_earlyUiAllowOverlap = m_editor->PrepareSnapshot(ctx.frame);
        m_earlyUiBuilt = true;
        m_earlyUiMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - uiStart).count();
    }
}

void FramePipeline::EmplaceFrameTaskContext(std::optional<Sys::FrameTaskContext>& cpuFrame,
                                            IGraphicsDevice& backend, const GameModeContext& ctx)
{
    static const bool s_frameContinuations = !Sys::CommandLine::Get().Has("-framepreparelegacy");
    // -brslane: main makes NO context. FrameTaskContext binds to its
    // creating thread (CheckOwner throws for every other one), so the one
    // this frame's material writes go through has to be the lane's, made in
    // Renderer::PrepareFrameOnLane. Renderer::BuildScene_PlanEmit keeps
    // `framePreparation` true off m_emitDeferred, so nothing downstream
    // falls back to the serial material path just because main is empty-handed.
    if (s_frameContinuations && !m_brsLaneEnabled) {
        if (!m_frameOwnerQueue) m_frameOwnerQueue = std::make_unique<Sys::OwnerTaskQueue>();
        cpuFrame.emplace(*m_frameOwnerQueue, ctx.frame, backend.GetUploadSlot());
    }
}

void FramePipeline::LogPrepareOwnerDone(Renderer& renderer, const GameModeContext& ctx,
                                        bool mainHasContext)
{
    // -materialcpuverify: [FRAME_PREPARE_OWNER_DONE] opens the span from "main finished
    // the owner-side work it can overlap with UI extraction" to [FRAME_PREPARE_PUBLISH].
    // Printed here, outside the caller's `if (cpuFrame)`, because under -brslane main
    // holds no context and the marker would vanish; owner= names which thread will
    // publish, so the two arms' spans are not one name for different things.
    if (Sys::CommandLine::Get().Has("-materialcpuverify"))
        // Read off the hand-off latch, not m_brsLaneEnabled: a -brslane frame whose batch
        // plan was refused emitted and published on main inside BeginFrame, and calling it
        // "lane" would put a frame with no [FRAME_PREPARE_PUBLISH] of its own into the lane's
        // span.
        LOG_INFO("[FRAME_PREPARE_OWNER_DONE] frame=%llu owner=%s",
            static_cast<unsigned long long>(ctx.frame),
            (!mainHasContext && renderer.LaneEmitOwed()) ? "lane" : "main");
}

void FramePipeline::SubmitFrame(IGraphicsDevice& backend, Renderer& renderer,
                                const GameModeContext& ctx)
{
    // Publish a bounded frame transaction. RenderHalf pumps owner
    // continuations through completion before the next World mutation.
    // Each execution segment restores the render context tripwire.
    FramePacket pkt;
    pkt.frame    = ctx.frame;
    pkt.rBeginMs = m_renderSplitRBeginThisFrame;
    pkt.world    = ctx.world;
    // The emit lease travels WITH the frame: read back from the renderer here, at the
    // hand-off, and the lane prepares against these values instead of live state.
    pkt.uploadSlot   = renderer.GetFrameUploadSlot();
    pkt.emitIdx      = renderer.GetEmitIdx();
    pkt.batchPlanGen = renderer.GetBatchPlanGen();
    pkt.sortGen      = renderer.GetSortIdxGen();
    // ...and whether the lane owes this frame's emit half at all. Read here, at the
    // hand-off, where the lane is idle (the moved Animation barrier drained it before
    // BeginFrame). A frame whose batch plan was refused cleared the deferral inside
    // BeginFrame and emitted on main; the lane must not prepare a frame nobody owes it.
    pkt.laneEmit     = renderer.LaneEmitOwed();
    // Taken HERE, on the game thread, and carried across. -sltokenlag
    // rewrites which one travels; the ring is game-thread state and
    // stays on this side with it.
    pkt.slToken  = SL::StreamlineBackend::Get().GameFrameToken();
    if (m_slTokenLag > 0)
    {
        m_slTokenRing[ctx.frame % kSlTokenRing] = pkt.slToken;
        const uint64_t back = (ctx.frame >= m_slTokenLag)
                            ? (ctx.frame - m_slTokenLag) : ctx.frame;
        pkt.slToken = m_slTokenRing[back % kSlTokenRing];
    }
    RenderHalf(backend, renderer, pkt);
    if (!m_frameOverlap) m_completedRender = m_fromRender;
}

// Called after the PostRender phase (App::Run).
void FramePipeline::LogPhaseCpu()
{
    // -phaselog_every=N: dump the scheduler's per-phase / per-system CPU breakdown every
    // N frames, so a headless run can ask WHICH phase owned a slow frame. Systems under
    // 0.05 ms are omitted: across ~14 phases the noise would bury the lines that matter.
    static const int s_every = []{
        const auto& cl = Sys::CommandLine::Get();
        return cl.Has("-phaselog_every") ? cl.GetInt("-phaselog_every", 60) : 0;
    }();
    if (s_every > 0)
    {
        static int s_n = 0;
        if ((s_n++ % s_every) == 0)
        {
            float total = 0.f;
            for (const auto& ph : m_scheduler.GetDebugInfo())
                total += ph.lastFrameMs;
            LOG_INFO("[PHASECPU] frame total %.2f ms", total);
            for (const auto& ph : m_scheduler.GetDebugInfo())
            {
                if (ph.lastFrameMs < 0.05f) continue;
                std::string line;
                for (const auto& sys : ph.systems)
                {
                    if (sys.lastFrameMs < 0.05f) continue;
                    line += "  ";
                    line += sys.name ? sys.name : "?";
                    line += " ";
                    char b[32];
                    std::snprintf(b, sizeof(b), "%.2f", sys.lastFrameMs);
                    line += b;
                }
                LOG_INFO("[PHASECPU]   %-22s %6.2f ms |%s",
                         ph.name ? ph.name : "?", ph.lastFrameMs, line.c_str());
            }
        }
    }
}

void FramePipeline::ReportExitFrameCounts(IGraphicsDevice& backend, const GameModeContext& ctx)
{
    // GAME frames and PRESENTED frames are different numbers, and only one is what a
    // pack means by "it ran 150 frames": the render half may run a frame behind, and the
    // quit path can retire the loop with a packet still unrendered. Reported
    // unconditionally so "presented == game" is a number someone read, not an
    // assumption.
    const uint64_t presented = backend.MainSurface().PresentedFrames();
    LOG_INFO("[SPLITAUDIT] max observed game-render frame lag = %llu "
             "(0 means the SLOTPARITY/SLTOKEN parity checks had "
             "nothing to catch this run)",
             static_cast<unsigned long long>(
                 backend.MaxObservedFrameLag()));
    LOG_INFO("[SPLITCHECK] frames game=%llu presented=%llu",
             static_cast<unsigned long long>(ctx.frame),
             static_cast<unsigned long long>(presented));
    const uint64_t lag = (ctx.frame > presented)
                       ? (ctx.frame - presented) : 0;
    // The message names no harness artifact on purpose: this check ships in Retail, where
    // there is no run.json, and tools/strip_gate.py fails a Retail binary that carries that
    // file name. In a harness run the casualty is run.json's framesRun (NoteFrames records
    // ctx.frame) and every pack asserting on it.
    ENGINE_CHECK(lag <= 1,
        "[SPLITCHECK][FRAMECOUNT] the loop ran %llu frames but only %llu "
        "were presented - a frame count read from the game loop is "
        "measuring the loop rather than the screen",
        static_cast<unsigned long long>(ctx.frame),
        static_cast<unsigned long long>(presented));
}
