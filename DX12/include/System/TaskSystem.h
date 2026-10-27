#pragma once
#include <tracy/Tracy.hpp> // public inline profiling also used by non-PCH clients

#include "System/ThreadContext.h"  // ParallelFor propagates the caller's frame half
#include "Diagnostics/Profiler.h"  // engine profiler scopes beside the Tracy zones
#include "System/BoundedTaskQueue.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <memory>
#include <queue>
#include <thread>
#include <utility>
#include <vector>

namespace Sys { class TaskGraphAccess; }

// The engine's thread pool, shared by resource loading, physics, animation, ECS systems and
// render recording. Workers = hardware threads - 2 (one each for main and the logger), at
// least 1; -workers=N pins it. Per-worker bounded shards with stealing; the global queues take
// costed tasks and shard overflow. High work runs before Low.
class TaskSystem
{
public:
    enum class TaskPriority
    {
        Low,   // Resource loading, I/O
        High   // Physics, animation updates, etc.
    };

    // How ParallelFor deals indices to chunks. Contiguous (default) suits roughly uniform
    // per-index cost.
    // Interleaved deals index t, t+numChunks, t+2*numChunks, ... to chunk t. Use it when the
    // range is sorted by cost or per-index cost is wildly uneven: chunk bounds are fixed before
    // anything runs and the caller runs chunk 0, so a descending cost sort under Contiguous
    // hands every expensive item to the caller (ChainPhysics' cost-sorted islands: 22 of 25
    // cloth rings on the caller, 410 us against 33 us per worker). It gives up cache locality,
    // so it is opt-in.
    enum class ParallelSchedule : uint8_t { Contiguous, Interleaved };

    static constexpr unsigned kMaxSpinMicros = 200; // see the cap note in the .cpp

    struct WakeStats
    {
        uint64_t parks = 0;        // m_epoch.wait entries (a real futex park)
        uint64_t spinHits = 0;     // warm spin saw a publication; no park
        uint64_t spinGiveUps = 0;  // warm spin expired; parked anyway
        uint64_t batches = 0;      // PublishBatch calls
        uint64_t batchTasks = 0;   // chunks published by those calls
        uint64_t wakeAll = 0;      // notify_all issued
        uint64_t wakeOne = 0;      // notify_one issued
        uint64_t chunkCaller = 0;  // chunks run by the SUBMITTING thread
        uint64_t chunkHelp = 0;    // of those, chunks > 0 (no worker took it)
        uint64_t chunkWorker = 0;  // chunks run on a pool worker
        uint64_t chunkMismatch = 0;// groups whose bodies != chunks (exactly-once)
    };

    static TaskSystem& Get();

    TaskSystem(const TaskSystem&) = delete;
    TaskSystem& operator=(const TaskSystem&) = delete;

    // Submits a task; workers run High before Low. A task that throws is logged and the first
    // escaped exception is kept for RethrowPendingTaskException() on the owning thread. A
    // fire-and-forget task with its own completion counter must make the decrement
    // exception-safe itself (the worker does not know the counter); ParallelFor shows how.
    void Push(std::function<void()> task, TaskPriority priority = TaskPriority::Low);

    // True on a TaskSystem worker thread (thread_local stamp set by WorkerLoop). The cheap
    // building block for main-thread-only checks such as CommandBuffer::Flush's.
    static bool OnWorkerThread();
    static unsigned WorkerIndex(); // stable pool index; ~0u on external threads

    // Per-thread submission cost floor: a non-worker thread that sets it (the render-owner
    // lane) has its cost-0 High submissions enqueued at this cost. A nonzero cost goes to the
    // global High heap, which every worker checks first at each task boundary; a cost-0 task
    // from a non-worker lands in one worker's FIFO shard, and while the chain-solve pipeline
    // keeps every shard full nobody steals it (~2.1 ms/frame of lane wait). 0 = off, the
    // default on every thread.
    static void SetThreadSubmitCostFloor(uint32_t microsFloor);
    static uint32_t ThreadSubmitCostFloor();

    // Pins an owner thread under -taskaffinity=phys|smt, which pins the pool (see the
    // constructor) and reserves physical core 0 for the owners: @p slot 0 = main, 1 = the
    // render-owner lane, each on one of core 0's logical processors. No-op without the flag.
    void PinOwnerThread(unsigned slot);

    // True when a queued task (Push) escaped an exception nobody has claimed yet. Lock-free;
    // safe to poll once per frame.
    bool HasPendingTaskException() const
    { return m_hasPendingExc.load(std::memory_order_acquire); }

    // Rethrows and clears the first exception that escaped a queued task, on the calling
    // thread; no-op when there is none. Push() has no join point, so without this a failed
    // asynchronous load would only reach the log. Call it from the main loop or any place that
    // can handle the failure.
    void RethrowPendingTaskException();

    // Static-chunk ParallelFor kept only as a benchmark control; engine code uses ParallelFor.
    // Chunks are capped at the worker count and the caller runs chunk 0 then only waits, so a
    // call queues at most workers-1 chunks and leaves one worker for a nested call: safe to
    // depth 2, deadlock at depth 3 (that last worker is itself a blocked caller). Chunks use
    // remainder distribution: ceil-division leaves trailing chunks empty (30 items over 14
    // workers fan out only 10 ways), each still costing a lock and a notify. If fn throws in any
    // chunk, every chunk still runs, the call joins, and the first exception is rethrown on the
    // calling thread.
    template<typename Func>
    void ParallelForLegacy(uint32_t begin, uint32_t end, Func&& fn,
                     TaskPriority pri = TaskPriority::High,
                     ParallelSchedule schedule = ParallelSchedule::Contiguous)
    {
        ZoneScopedN("TaskSystem.ParallelFor");
        PROF_SCOPE_GENERIC("TaskSystem.ParallelFor");
        if (begin >= end) return;
        const uint32_t count = end - begin;
        ZoneValue(count);
        const uint32_t numChunks = (std::min)(count, m_numWorkers);
        const uint32_t base = count / numChunks;   // >= 1: numChunks <= count
        const uint32_t rem  = count % numChunks;   // first `rem` chunks get one extra

        // Chunk t spans [begin + t*base + min(t,rem), + base + (t < rem)).
        auto chunkLo = [&](uint32_t t) { return begin + t * base + (std::min)(t, rem); };

        // Push chunks [1, numChunks) to workers; run chunk 0 on this thread.
        std::atomic<uint32_t> remaining(numChunks - 1);
        std::mutex doneMtx;
        std::condition_variable doneCv;
        // First exception escaping any chunk. Written under doneMtx (the lock the completion
        // decrement already takes), so it cannot race with the join.
        std::exception_ptr firstErr;

        // The caller's frame half, captured here: inside the worker lambda Current() reads the
        // worker's own thread-local (always None), and World accesses a pass makes from a worker
        // (VideoQuadPass::Execute) would go unreported. Chunks inherit it; the worker restores its
        // own on the way out.
        const uint8_t ctxCap = Sys::ThreadCtx::Current();

        for (uint32_t t = 1; t < numChunks; ++t)
        {
            const uint32_t lo = chunkLo(t);
            const uint32_t hi = chunkLo(t + 1);
            Push([&fn, lo, hi, t, begin, end, numChunks, schedule, ctxCap,
                  &remaining, &doneMtx, &doneCv, &firstErr]()
            {
                ZoneScopedN("TaskSystem.Chunk.Worker");
                ZoneValue(t);
                PROF_SCOPE_GENERIC("TaskSystem.Chunk.Worker");
                Sys::ThreadCtx::Scope _ctx(ctxCap);
                // fn is arbitrary engine code (systems, RenderPass recording) and must not skip the
                // decrement below: an escaping exception would leave `remaining` stuck and hang the caller.
                std::exception_ptr err;
                try
                {
                    if (schedule == ParallelSchedule::Contiguous)
                        for (uint32_t i = lo; i < hi; ++i)
                            fn(i);
                    else
                        for (uint32_t i = begin + t; i < end; i += numChunks)
                            fn(i);
                }
                catch (const std::exception& e)
                {
                    LogTaskException("ParallelFor chunk", e.what());
                    err = std::current_exception();
                }
                catch (...)
                {
                    LogTaskException("ParallelFor chunk", nullptr);
                    err = std::current_exception();
                }

                // Decrement UNDER the lock: a relaxed decrement outside it can
                // land between the waiter's predicate check and its wait(),
                // losing the notify and hanging the frame.
                std::lock_guard<std::mutex> lk(doneMtx);
                if (err && !firstErr)
                    firstErr = err;
                remaining.fetch_sub(1, std::memory_order_relaxed);
                doneCv.notify_one();
            }, pri);
        }

        // Caller executes chunk 0 inline instead of blocking idle.
        {
            ZoneScopedN("TaskSystem.Chunk.Caller");
            PROF_SCOPE("TaskSystem.Chunk.Caller");
            const uint32_t hi = chunkLo(1);
            PlotActive(+1);
            std::exception_ptr err;
            try
            {
                // Chunk 0 under either schedule: [begin, hi) contiguously, or
                // begin, begin+numChunks, ... interleaved. Same partition, and
                // under Interleaved the caller takes only ONE of the expensive
                // head items instead of all of them.
                if (schedule == ParallelSchedule::Contiguous)
                    for (uint32_t i = begin; i < hi; ++i)
                        fn(i);
                else
                    for (uint32_t i = begin; i < end; i += numChunks)
                        fn(i);
            }
            catch (const std::exception& e)
            {
                LogTaskException("ParallelFor caller chunk", e.what());
                err = std::current_exception();
            }
            catch (...)
            {
                LogTaskException("ParallelFor caller chunk", nullptr);
                err = std::current_exception();
            }
            PlotActive(-1);   // balanced on the throwing path too
            if (err)
            {
                std::lock_guard<std::mutex> lk(doneMtx);
                if (!firstErr)
                    firstErr = err;
            }
            // Deliberately falls through to the join instead of rethrowing here: remaining, doneMtx
            // and doneCv live on this stack frame and queued chunks still reference them.
        }

        if (numChunks > 1)
        {
            ZoneScopedN("TaskSystem.JoinWait");
            PROF_WAIT(WaitJoin, "TaskSystem.JoinWait");
            std::unique_lock<std::mutex> lk(doneMtx);
            doneCv.wait(lk, [&]{ return remaining.load(std::memory_order_acquire) == 0; });
        }

        // Unlocked read is safe: every writer stored firstErr under doneMtx
        // BEFORE its decrement, and the wait above re-acquired doneMtx after
        // observing remaining == 0, so all those stores happen-before this.
        if (firstErr)
            std::rethrow_exception(firstErr);
    }

    // Splits [begin, end) across the workers, calls fn(index) for each index and blocks until
    // all ran. The calling thread runs chunk 0 and then every chunk no worker claimed yet, so
    // nesting never starves the pool; it helps only its own chunks, never arbitrary jobs (which
    // may take a lock the caller holds). Contiguous chunks use remainder distribution. If fn
    // throws in any chunk, every chunk still runs, the call joins, and the first exception is
    // rethrown here. Throws during shutdown.
    template<typename Func>
    void ParallelFor(uint32_t begin, uint32_t end, Func&& fn,
                     TaskPriority priority = TaskPriority::High,
                     ParallelSchedule schedule = ParallelSchedule::Contiguous)
    {
        ZoneScopedN("TaskSystem.ParallelFor");
        PROF_SCOPE_GENERIC("TaskSystem.ParallelFor");
        if (begin >= end) return;
        const uint32_t count = end-begin;
        const uint32_t chunks = (std::min)(count, m_numWorkers);
        if (!AcquireGraphLease()) throw std::runtime_error("ParallelFor during shutdown");
        if (chunks == 1) {
            try { for (uint32_t i=begin;i<end;++i) fn(i); }
            catch (...) { ReleaseGraphLease(); throw; }
            ReleaseGraphLease();
            return;
        }
        using Body = std::remove_reference_t<Func>;
        struct Group {
            Body* body;
            uint32_t begin, end, chunks;
            ParallelSchedule schedule;
            uint8_t context;
            std::unique_ptr<std::atomic<bool>[]> claimed;
            std::atomic<uint32_t> remaining;
            // Bodies that actually RAN, for the exactly-once oracle in ~Group.
            // Counted only under -taskwakeverify: it is one relaxed RMW per
            // chunk and the wake experiments must not move the default path.
            std::atomic<uint32_t> executed{0};
#ifdef TRACY_ENABLE
            // Publication instant, so the last chunk's start latency (what the wake policy tries to
            // move) is plottable.
            std::chrono::steady_clock::time_point publishedAt{std::chrono::steady_clock::now()};
#endif
            std::mutex errorMutex;
            std::exception_ptr error;
            // The submitting site's name (Diag::Prof::CurrentLabel), reopened
            // inside every chunk so the chunk is attributed to it, not to
            // TaskSystem.Chunk.Worker. Constructed on the calling thread.
            ::Diag::Prof::NameId label = ::Diag::Prof::CurrentLabel();
            Group(Body* f,uint32_t b,uint32_t e,uint32_t n,ParallelSchedule s)
                : body(f),begin(b),end(e),chunks(n),schedule(s),context(Sys::ThreadCtx::Current()),
                  claimed(new std::atomic<bool>[n]{}),remaining(n) {}
            ~Group() {
                if (TaskSystem::WakeStatsEnabled())
                    TaskSystem::NoteGroupRetired(chunks, executed.load(std::memory_order_relaxed));
                TaskSystem::Get().ReleaseGraphLease();
            }
            // @p byCaller: the submitting thread runs the chunk inline (chunk 0, or one no worker
            // claimed) - not the same as !OnWorkerThread(), since a nested ParallelFor's caller is a
            // worker. The zones still split on OnWorkerThread(), so captures keep their meaning.
            void Run(uint32_t chunk, bool byCaller = false) {
                if (claimed[chunk].exchange(true,std::memory_order_acq_rel)) return;
                const bool caller = !TaskSystem::OnWorkerThread();
                ZoneNamedN(workerZone,"TaskSystem.Chunk.Worker",!caller);
                ZoneNamedN(callerZone,"TaskSystem.Chunk.Caller",caller);
                static ::Diag::Prof::NameSlot s_profWorker{"TaskSystem.Chunk.Worker"};
                static ::Diag::Prof::NameSlot s_profCaller{"TaskSystem.Chunk.Caller"};
                ::Diag::Prof::Scope profChunk(caller ? s_profCaller : s_profWorker,
                                              ::Diag::Prof::Cat::Work, ::Diag::Prof::Level::Stats,
                                              ::Diag::Prof::Generic{});
                PROF_LABEL(label);
#ifdef TRACY_ENABLE
                // Chunk ORDINAL on the zone: the wake-policy A/B is read per
                // ordinal, and a zone without it cannot be filtered that way.
                ZoneValueV(workerZone,chunk);
                ZoneValueV(callerZone,chunk);
                if (!caller && chunk + 1 == chunks) {
                    // Through a local: TracyPlot is a macro, and the comma in
                    // duration<double,std::micro> would split its arguments.
                    const double startUs = std::chrono::duration<double,std::micro>(
                        std::chrono::steady_clock::now() - publishedAt).count();
                    TracyPlot("TaskSystem.LastChunkStartUs", startUs);
                }
#endif
                if (TaskSystem::WakeStatsEnabled()) {
                    executed.fetch_add(1,std::memory_order_relaxed);
                    TaskSystem::NoteChunkRun(byCaller,chunk);
                }
                Sys::ThreadCtx::Scope ctx(context);
                if (caller) TaskSystem::PlotActive(+1);
                try {
                    if (schedule == ParallelSchedule::Contiguous) {
                        const auto count=end-begin, base=count/chunks, extra=count%chunks;
                        const auto lo=begin+chunk*base+(std::min)(chunk,extra);
                        const auto hi=lo+base+(chunk<extra);
                        for (auto i=lo;i<hi;++i) (*body)(i);
                    } else {
                        for (auto i=begin+chunk;i<end;) {
                            (*body)(i);
                            if (end-i<=chunks) break; // avoid uint32 wrap near UINT_MAX
                            i+=chunks;
                        }
                    }
                } catch (...) {
                    std::lock_guard lock(errorMutex);
                    if (!error) error=std::current_exception();
                }
                if (caller) TaskSystem::PlotActive(-1);
                if (remaining.fetch_sub(1,std::memory_order_acq_rel)==1) remaining.notify_all();
            }
        };
        std::shared_ptr<Group> group;
        try { group=std::make_shared<Group>(&fn,begin,end,chunks,schedule); }
        catch (...) { ReleaseGraphLease(); throw; }
        std::exception_ptr submissionError;
        // -taskbatchpublish: push every chunk before any wake, then bump the epoch once and
        // notify as many waiters as there are chunks. The default bumps and notifies once per
        // chunk, which staggers the fan-out (~0.27 ms/frame). Publish before Run(0) below, or no
        // worker could start until the caller's chunk finished.
        const bool batch = m_batchPublish;
        try {
            for (uint32_t chunk=1;chunk<chunks;++chunk)
                EnqueueAccepted([group,chunk] { group->Run(chunk); },priority,0,batch);
        } catch (...) { submissionError=std::current_exception(); }
        // Unconditional, including on the submission-failure path: a chunk that
        // was pushed before the throw is already outstanding, and leaving it
        // unwoken strands the Group (and its graph lease) until shutdown.
        if (batch) PublishBatch(chunks-1);
        group->Run(0,true);
        for (uint32_t chunk=1;chunk<chunks;++chunk) group->Run(chunk,true);
        {
            ZoneScopedN("TaskSystem.JoinWait");
            PROF_WAIT(WaitJoin, "TaskSystem.JoinWait");
            auto remaining=group->remaining.load(std::memory_order_acquire);
            while (remaining) {
                group->remaining.wait(remaining,std::memory_order_acquire);
                remaining=group->remaining.load(std::memory_order_acquire);
            }
        }
        // All bodies have returned. Late queue wrappers own Group and only
        // inspect claimed=true; they never dereference the caller's fn again.
        if (submissionError) std::rethrow_exception(submissionError);
        if (group->error) std::rethrow_exception(group->error);
    }

    unsigned GetWorkerCount() const { return m_numWorkers; }
    uint64_t StealCount() const { return m_steals.load(std::memory_order_relaxed); }
    uint64_t OverflowCount() const { return m_overflows.load(std::memory_order_relaxed); }
    // Submissions that SetThreadSubmitCostFloor lifted into the global heap.
    uint64_t BoostedSubmitCount() const { return m_boostedSubmits.load(std::memory_order_relaxed); }

    // ---- Wake policy (opt-in, both off by default) ----
    // -taskspin=<us>: warm spin before a worker parks on m_epoch.wait.
    // -taskbatchpublish: publish a ParallelFor group, then wake once.
    // Read once in the constructor; a running pool is never reconfigured.
    unsigned SpinMicros() const noexcept { return m_spinMicros; }
    bool BatchPublishEnabled() const noexcept { return m_batchPublish; }

    // Counters behind -taskwakeverify. Static, not per-instance: ParallelFor's Group::Run is a
    // header-inline local class with no TaskSystem `this`, and there is exactly one pool. Every
    // increment is gated on WakeStatsEnabled(), so the default path's code is unchanged.
    static bool WakeStatsEnabled() noexcept { return s_wakeStats; }
    static WakeStats SampleWakeStats() noexcept;

    // Called from ParallelFor's Group; public only because that local class is
    // instantiated from a template body. Not part of the engine-facing API.
    static void NoteChunkRun(bool byCaller, uint32_t chunk) noexcept
    {
        if (byCaller)
        {
            s_chunkCaller.fetch_add(1, std::memory_order_relaxed);
            if (chunk) s_chunkHelp.fetch_add(1, std::memory_order_relaxed);
        }
        else s_chunkWorker.fetch_add(1, std::memory_order_relaxed);
    }
    static void NoteGroupRetired(uint32_t chunks, uint32_t executed) noexcept
    {
        if (chunks != executed) s_chunkMismatch.fetch_add(1, std::memory_order_relaxed);
    }

    // Bumps the Tracy "TaskSystem.Active" plot. No-op without TRACY_ENABLE.
    static void PlotActive(int delta);

    // Call at program shutdown; stops all workers.
    void Shutdown();

private:
    static constexpr uint64_t kClosed = uint64_t{1} << 63;

    struct QueuedTask
    {
        std::function<void()> run;
        uint32_t estimatedMicros = 0;
        uint64_t sequence = 0;
        // Diag::Prof frame tag of the SUBMITTING thread, re-published on the
        // worker for the duration of the task so its events carry the frame
        // they were queued for (overlap mode runs two frames at once).
        uint8_t  frameTag = 0;
#ifdef TRACY_ENABLE
        std::chrono::steady_clock::time_point queuedAt;
#endif
    };
    // Ready high-priority work: largest estimated cost first, FIFO among equal costs (so
    // zero-cost tasks stay FIFO). Not preemption.
    struct ShorterFirst
    {
        bool operator()(const QueuedTask& a, const QueuedTask& b) const
        {
            if (a.estimatedMicros != b.estimatedMicros)
                return a.estimatedMicros < b.estimatedMicros;
            return a.sequence > b.sequence;
        }
    };
    struct WorkerQueues {
        Sys::BoundedTaskQueue<QueuedTask, 256> high, low;
    };

    inline static bool s_wakeStats = false;   // -taskwakeverify, set in the ctor
    inline static std::atomic<uint64_t> s_parks{0}, s_spinHits{0}, s_spinGiveUps{0};
    inline static std::atomic<uint64_t> s_batches{0}, s_batchTasks{0};
    inline static std::atomic<uint64_t> s_wakeAll{0}, s_wakeOne{0};
    inline static std::atomic<uint64_t> s_chunkCaller{0}, s_chunkHelp{0};
    inline static std::atomic<uint64_t> s_chunkWorker{0}, s_chunkMismatch{0};

    friend class Sys::TaskGraphAccess;
    // A lease includes suspended continuations, not just queued/running jobs.
    bool AcquireGraphLease();
    void ReleaseGraphLease();
    void PushGraphContinuation(std::function<void()> task, uint32_t estimatedMicros = 0);

    TaskSystem();
    ~TaskSystem();
    void WorkerLoop(unsigned index);

    // Logs one escaped task exception and flushes the logger synchronously. Out-of-line so
    // this header does not pull in Log.h (windows.h). @p what is null when the thrown object
    // was not a std::exception.
    static void LogTaskException(const char* context, const char* what);

    // Records the first exception escaping a queued task; later ones are almost always its
    // fallout and are dropped (already logged).
    void NotePendingException(std::exception_ptr e);

    void EnqueueAccepted(std::function<void()> task, TaskPriority priority, uint32_t cost = 0,
                         bool deferWake = false);
    bool TryTake(unsigned index, QueuedTask& task, bool& high);
    bool Drained() const;
    void Wake(bool all = false);
    // One epoch bump for a whole ParallelFor group. @p count is how many chunks
    // were pushed, which is also how many waiters are worth waking.
    void PublishBatch(uint32_t count);
    // Bounded warm spin on the publication epoch. True = "something changed,
    // go around the idle loop again"; false = "nothing arrived, park".
    bool SpinForWork(uint64_t epoch) const;

    void PushHigh(QueuedTask task); // caller holds m_mutex

    std::atomic<uint64_t> m_admission{0}; // closed bit + accepted graph/publisher leases
    alignas(64) std::atomic<uint64_t> m_outstanding{0}; // queued AND executing wrappers
    alignas(64) std::atomic<uint64_t> m_epoch{0}; // publication/close event, not a spin counter

    unsigned m_numWorkers;
    std::vector<std::thread> m_workers;
    std::mutex m_mutex;
    std::mutex m_shutdownMutex;

    std::vector<std::unique_ptr<WorkerQueues>> m_queues;
    alignas(64) std::atomic<uint64_t> m_injection{0};
    std::atomic<uint64_t> m_globalHigh{0}, m_globalLow{0};
    std::atomic<int64_t> m_queued{0}; // includes producer reservations until publication
    std::atomic<uint64_t> m_steals{0}, m_overflows{0};
    std::atomic<uint64_t> m_boostedSubmits{0};

    unsigned m_spinMicros = 0;   // -taskspin=<us>; 0 = no spin (default)
    bool     m_batchPublish = false; // -taskbatchpublish
    // Workers parked in m_epoch.wait, maintained only under -taskbatchpublish (the default
    // path never reads it, and the two extra RMWs would land on every park). PublishBatch and
    // the park form a Dekker handshake through it; see both sides.
    alignas(64) std::atomic<uint32_t> m_idleWaiters{0};

    uint64_t m_nextSequence = 0;
    std::vector<QueuedTask> m_highQueue;
    std::queue<QueuedTask> m_lowQueue;

    // Escaped-exception slot for the plain Push() path (ParallelFor carries its
    // own per-call slot and never touches these).
    mutable std::mutex m_excMutex;
    std::exception_ptr m_pendingExc;
    std::atomic<bool>  m_hasPendingExc{ false };
};
