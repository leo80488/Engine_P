#include "Harness/Determinism.h"
#include "System/TaskSystem.h"
#include "System/LockStats.h"   // SYS_COUNTED_LOCK contention probe
#include "System/CommandLine.h"
#include "System/Check.h"
#include "System/Log.h"
#include "Diagnostics/Profiler.h"

// Worker-thread identity (see TaskSystem::OnWorkerThread). File-scope so the header stays
// free of the thread_local.
static thread_local bool t_isTaskWorker = false;
static thread_local unsigned t_workerIndex = ~0u;
// See TaskSystem::SetThreadSubmitCostFloor. Per thread; 0 = off.
static thread_local uint32_t t_submitCostFloor = 0;

bool TaskSystem::OnWorkerThread() { return t_isTaskWorker; }
unsigned TaskSystem::WorkerIndex() { return t_workerIndex; }
void TaskSystem::SetThreadSubmitCostFloor(uint32_t microsFloor) { t_submitCostFloor = microsFloor; }
uint32_t TaskSystem::ThreadSubmitCostFloor() { return t_submitCostFloor; }
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>
#ifdef TRACY_ENABLE
#include <cstdio>
#include <tracy/Tracy.hpp>
#endif

namespace
{
    // The physical cores in processor group 0, each as the mask of its logical processors
    // (SMT siblings), in the OS's order.
    std::vector<KAFFINITY> PhysicalCoreMasks()
    {
        DWORD len = 0;
        GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
        if (len == 0) return {};
        std::vector<uint8_t> buf(len);
        auto* first = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data());
        if (!GetLogicalProcessorInformationEx(RelationProcessorCore, first, &len)) return {};
        std::vector<KAFFINITY> cores;
        for (DWORD off = 0; off < len;)
        {
            auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data() + off);
            if (info->Relationship == RelationProcessorCore && info->Processor.GroupCount >= 1 &&
                info->Processor.GroupMask[0].Group == 0 && info->Processor.GroupMask[0].Mask)
                cores.push_back(info->Processor.GroupMask[0].Mask);
            off += info->Size;
        }
        return cores;
    }

    // The logical processors of one core mask, lowest bit first.
    std::vector<KAFFINITY> SplitMask(KAFFINITY m)
    {
        std::vector<KAFFINITY> out;
        for (unsigned b = 0; b < sizeof(KAFFINITY) * 8; ++b)
            if (m & (KAFFINITY(1) << b)) out.push_back(KAFFINITY(1) << b);
        return out;
    }

    // 0 = none (default), 1 = phys, 2 = smt. See the constructor.
    int         g_affinityMode = 0;
    KAFFINITY   g_ownerMask[2] = { 0, 0 };
}

TaskSystem::TaskSystem()
{
    unsigned hw = std::thread::hardware_concurrency();
    // Reserve one core for the main thread and one for the logger. `hw - 2u` is unsigned and
    // wraps at hw <= 2, so guard the subtraction itself.
    m_numWorkers = (hw > 3u) ? (hw - 2u) : 1u;

    // -workers=N pins the pool for a reproducible run. Opt-in: pinning to 1 removes the
    // cross-system enqueue race and the physics job-graph nondeterminism but stops exercising
    // the parallel paths, so it is for golden-image packs only (see Determinism::Workers()).
    const int pinned = Harness::Determinism::Get().Workers();
    if (pinned > 0)
        m_numWorkers = static_cast<unsigned>(pinned);

    // Wake policy, both opt-in and off by default.
    // -taskspin=<us>: a worker that finds no work re-reads the publication epoch with
    // YieldProcessor for up to <us> before a futex park; without it every chunk that does not
    // land on a running worker costs a wake syscall. Capped at kMaxSpinMicros: a spinning
    // worker is a core the owner thread cannot use, and past ~200 us it costs more than the
    // stagger it removes.
    // -taskbatchpublish: see PublishBatch.
    {
        const auto& cl = Sys::CommandLine::Get();
        const int spin = cl.GetInt("-taskspin", 0);
        m_spinMicros = spin <= 0 ? 0u
                     : static_cast<unsigned>((std::min)(spin, int(kMaxSpinMicros)));
        m_batchPublish = cl.Has("-taskbatchpublish");
        s_wakeStats    = cl.Has("-taskwakeverify");
    }
    m_queues.reserve(m_numWorkers);
    for (unsigned i=0; i<m_numWorkers; ++i) m_queues.push_back(std::make_unique<WorkerQueues>());
    m_workers.reserve(m_numWorkers);
    try {
        for (unsigned i = 0; i < m_numWorkers; ++i)
            m_workers.emplace_back(&TaskSystem::WorkerLoop, this, i);
    } catch (...) {
        m_admission.fetch_or(kClosed, std::memory_order_acq_rel);
        Wake(true);
        for (auto& worker : m_workers) worker.join();
        throw;
    }
    LOG_INFO("[TASK_RUNTIME] backend=sharded workers=%u queueCapacity=256 spinUs=%u batchPublish=%d",
             m_numWorkers, m_spinMicros, m_batchPublish ? 1 : 0);

    // -taskaffinity=<none|phys|smt> (opt-in):
    //   phys  worker i -> both logical processors of physical core 1+(i % (C-1)): one worker
    //         per core while workers <= C-1;
    //   smt   worker i -> one logical processor, cores 1..C-1 taken sibling by sibling: two
    //         workers per core.
    // Both reserve core 0 for main (slot 0) and the render-owner lane (slot 1) via
    // PinOwnerThread, so job threads never share a core with them.
    {
        const std::string mode = Sys::CommandLine::Get().Get("-taskaffinity", "none");
        g_affinityMode = mode == "phys" ? 1 : mode == "smt" ? 2 : 0;
        const auto cores = g_affinityMode ? PhysicalCoreMasks() : std::vector<KAFFINITY>{};
        if (g_affinityMode && cores.size() < 2)
        {
            LOG_WARNING("[TASK_AFFINITY] -taskaffinity=%s needs >= 2 physical cores (found %zu) - ignored",
                        mode.c_str(), cores.size());
            g_affinityMode = 0;
        }
        if (g_affinityMode)
        {
            const auto own = SplitMask(cores[0]);
            g_ownerMask[0] = own[0];
            g_ownerMask[1] = own.size() > 1 ? own[1] : own[0];
            std::vector<KAFFINITY> logical;   // smt: cores 1.., sibling by sibling
            for (size_t c = 1; c < cores.size(); ++c)
                for (KAFFINITY m : SplitMask(cores[c])) logical.push_back(m);
            std::string masks;
            for (unsigned i = 0; i < m_numWorkers; ++i)
            {
                const KAFFINITY m = g_affinityMode == 1
                    ? cores[1 + (i % (cores.size() - 1))]
                    : logical[i % logical.size()];
                SetThreadAffinityMask(static_cast<HANDLE>(m_workers[i].native_handle()), m);
                char b[24];
                std::snprintf(b, sizeof(b), "%s%llx", i ? "," : "", static_cast<unsigned long long>(m));
                masks += b;
            }
            LOG_INFO("[TASK_AFFINITY] mode=%s cores=%zu workers=%u owner=%llx,%llx workerMasks=%s",
                     mode.c_str(), cores.size(), m_numWorkers,
                     static_cast<unsigned long long>(g_ownerMask[0]),
                     static_cast<unsigned long long>(g_ownerMask[1]), masks.c_str());
        }
    }
}

void TaskSystem::PinOwnerThread(unsigned slot)
{
    if (!g_affinityMode || slot > 1) return;
    SetThreadAffinityMask(GetCurrentThread(), g_ownerMask[slot]);
    LOG_INFO("[TASK_AFFINITY] owner slot %u pinned to %llx", slot,
             static_cast<unsigned long long>(g_ownerMask[slot]));
}

TaskSystem::~TaskSystem()
{
    Shutdown();
}

TaskSystem& TaskSystem::Get()
{
    static TaskSystem s_instance;
    return s_instance;
}

void TaskSystem::Push(std::function<void()> task, TaskPriority priority)
{
    if (!task) return;
    if (!ENGINE_CHECK(AcquireGraphLease(), "TaskSystem::Push after Shutdown — task dropped")) return;
    try { EnqueueAccepted(std::move(task), priority); }
    catch (...) { ReleaseGraphLease(); throw; }
    ReleaseGraphLease();
}

// Do not add a WaitAll(): an empty queue is no barrier (just-dequeued tasks still run).
// Join with a per-call completion counter instead (see ParallelFor).

void TaskSystem::Shutdown()
{
    if (OnWorkerThread())
        throw std::logic_error("TaskSystem worker cannot shut down its own pool");
    std::lock_guard shutdownLock(m_shutdownMutex);
    m_admission.fetch_or(kClosed, std::memory_order_acq_rel);
    Wake(true);
    for (auto& w : m_workers)
    {
        if (w.joinable())
            w.join();
    }
    m_workers.clear();
}

void TaskSystem::WorkerLoop(unsigned index)
{
    t_isTaskWorker = true;   // see OnWorkerThread()
    t_workerIndex = index;
    // Reserve stack headroom for the unhandled-exception filter (main.cpp). Deep recursion
    // (scene graph walks, BVH builds) runs on workers, and a stack overflow runs the filter on
    // the overflowed stack - without headroom the crash reporter faults again and leaves no
    // dump. Per-thread: must be set from the thread itself.
    {
        ULONG stackGuarantee = 64 * 1024;
        ::SetThreadStackGuarantee(&stackGuarantee);
    }

#ifdef TRACY_ENABLE
    {
        char tname[24];
        std::snprintf(tname, sizeof(tname), "TaskWorker %u", index);
        tracy::SetThreadName(tname);
    }
#endif
    {
        // Engine profiler identity (RegisterThread also sets the thread description; Tracy's
        // name above exists only in Profile).
        char pname[24];
        std::snprintf(pname, sizeof(pname), "TaskWorker %u", index);
        Diag::Prof::RegisterThread(pname, Diag::Prof::Role::Worker);
    }
    static Diag::Prof::NameSlot s_profHigh{ "TaskWorker.High" };
    static Diag::Prof::NameSlot s_profLow { "TaskWorker.Low" };
    while (true)
    {
        QueuedTask task;
        bool highPriority = false;
        {
            ZoneScopedN("TaskWorker.IdleWait");
            // Idle by definition: the queue scan plus the futex park is "nothing to do" for
            // utilisation.
            PROF_WAIT(WaitIdle, "TaskWorker.IdleWait");
            // Observe BEFORE searching: a publication during/after the search
            // changes the epoch, so atomic::wait cannot miss its wakeup.
            for (;;) {
                const auto epoch = m_epoch.load(std::memory_order_acquire);
                if (TryTake(index, task, highPriority)) break;
                if (Drained()) return;
                // Warm spin before parking (-taskspin). Never while the pool is
                // closing: the closed path wants every worker to reach the
                // Drained() return as cheaply as possible, and a spinning
                // worker would burn a core while the last accepted jobs run.
                if (m_spinMicros &&
                    !(m_admission.load(std::memory_order_acquire) & kClosed) &&
                    SpinForWork(epoch)) continue;
                if (s_wakeStats) s_parks.fetch_add(1, std::memory_order_relaxed);
                if (m_batchPublish) {
                    // Dekker handshake with PublishBatch: publish the intent to
                    // park BEFORE re-reading the epoch, so a publisher that
                    // misses this waiter must have bumped the epoch first and
                    // this thread therefore does not park at all. Both sides
                    // are seq_cst for exactly that store/load pair.
                    m_idleWaiters.fetch_add(1, std::memory_order_seq_cst);
                    if (m_epoch.load(std::memory_order_seq_cst) == epoch)
                        m_epoch.wait(epoch, std::memory_order_acquire);
                    m_idleWaiters.fetch_sub(1, std::memory_order_seq_cst);
                } else {
                    m_epoch.wait(epoch, std::memory_order_acquire);
                }
            }
        }

        if (task.run)
        {
            ZoneNamedN(highZone, "TaskWorker.High", highPriority);
            ZoneNamedN(lowZone, "TaskWorker.Low", !highPriority);
#ifdef TRACY_ENABLE
            // Zone value is enqueue-to-start WALL latency, not execution time.
            const auto latency = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - task.queuedAt).count();
            if (highPriority) { ZoneValueV(highZone, latency); }
            else { ZoneValueV(lowZone, latency); }
#else
            (void)highPriority;
#endif
            // Plot on every transition, not once per frame: the bursts this is
            // meant to show are ~139 us wide inside a 16.7 ms frame, so a
            // once-per-frame sample would miss them entirely.
            TaskSystem::PlotActive(+1);
            // The frame the task was queued for, so its events are attributable
            // in overlap mode; the scope is the worker's busy time.
            Diag::Prof::SetFrameTagByte(task.frameTag);
            Diag::Prof::Scope profTask(highPriority ? s_profHigh : s_profLow,
                                       Diag::Prof::Cat::Work, Diag::Prof::Level::Stats,
                                       Diag::Prof::Generic{});
            // Catch here: the Scheduler's ECS systems and every RenderPass::Execute run on workers, and
            // a throw out of the thread function is std::terminate - wWinMain's try/catch is on another
            // stack, and the async logger's queue would die unflushed.
            try
            {
                task.run();
            }
            catch (const std::exception& e)
            {
                LogTaskException("queued task", e.what());
                NotePendingException(std::current_exception());
            }
            catch (...)
            {
                LogTaskException("queued task", nullptr);
                NotePendingException(std::current_exception());
            }
            // Balanced on the throwing path too: one unbalanced +1 biases the Tracy "Active" plot for
            // good.
            TaskSystem::PlotActive(-1);
        }
        task.run = {}; // captures die before the outstanding count reaches zero
        if (m_outstanding.fetch_sub(1, std::memory_order_acq_rel) == 1 &&
            (m_admission.load(std::memory_order_acquire) & kClosed)) Wake(true);
    }
}

bool TaskSystem::AcquireGraphLease()
{
    auto state = m_admission.load(std::memory_order_acquire);
    while (!(state & kClosed)) {
        if (m_admission.compare_exchange_weak(state, state+1, std::memory_order_acq_rel)) return true;
    }
    return false;
}

void TaskSystem::ReleaseGraphLease()
{
    if (m_admission.fetch_sub(1, std::memory_order_acq_rel) == kClosed+1) Wake(true);
}

void TaskSystem::PushHigh(QueuedTask task)
{
    task.sequence = m_nextSequence++;
    m_highQueue.push_back(std::move(task));
    std::push_heap(m_highQueue.begin(), m_highQueue.end(), ShorterFirst{});
    m_globalHigh.store(m_highQueue.size(), std::memory_order_release);
}

void TaskSystem::PushGraphContinuation(std::function<void()> task, uint32_t estimatedMicros)
{
    EnqueueAccepted(std::move(task), TaskPriority::High, estimatedMicros);
}

bool TaskSystem::Drained() const {
    return m_admission.load(std::memory_order_acquire) == kClosed &&
        m_outstanding.load(std::memory_order_acquire) == 0;
}
void TaskSystem::Wake(bool all) {
    m_epoch.fetch_add(1, std::memory_order_release);
    if (s_wakeStats) (all ? s_wakeAll : s_wakeOne).fetch_add(1, std::memory_order_relaxed);
    if (all) m_epoch.notify_all(); else m_epoch.notify_one();
}

// One publication event for a whole ParallelFor group (-taskbatchpublish). The default
// path bumps the epoch and notifies once per chunk, so the n-th notify waits for the n-th
// push and the last chunk of a 13-wide fan-out starts ~110 us late (p50). Here all pushes
// land first (same shards, same overflow fallback) and the wake is one event.
// notify_all is not unconditional: waking 14 workers for 3 chunks costs more than it buys,
// so this wakes min(count, parked) and escalates to notify_all only when the group is at
// least as wide as the parked set. m_idleWaiters is maintained only on the batch park path,
// the only park this can race with while the flag is on.
void TaskSystem::PublishBatch(uint32_t count) {
    if (!count) return;
    if (s_wakeStats) {
        s_batches.fetch_add(1, std::memory_order_relaxed);
        s_batchTasks.fetch_add(count, std::memory_order_relaxed);
    }
    m_epoch.fetch_add(1, std::memory_order_seq_cst);
    const uint32_t idle = m_idleWaiters.load(std::memory_order_seq_cst);
    // idle == 0 needs no syscall at all: a worker that has not yet published
    // its intent to park re-reads the epoch after it does, sees the bump above,
    // and skips the wait. That is the other half of the handshake in
    // WorkerLoop, and getting it wrong is a lost wakeup, not a slow one.
    if (idle == 0) return;
    if (count >= idle) {
        if (s_wakeStats) s_wakeAll.fetch_add(1, std::memory_order_relaxed);
        m_epoch.notify_all();
    } else {
        if (s_wakeStats) s_wakeOne.fetch_add(count, std::memory_order_relaxed);
        for (uint32_t i = 0; i < count; ++i) m_epoch.notify_one();
    }
}

// Bounded warm spin on the publication epoch (-taskspin=<us>). Re-reads only
// the epoch, never the rings: a publication always bumps it, so the epoch is
// the cheapest possible "is a scan worth it" predicate. Returning true means
// "go around the idle loop" (TryTake and the Drained() check run again there);
// false means nothing arrived and the caller should park.
bool TaskSystem::SpinForWork(uint64_t epoch) const {
    ZoneScopedN("TaskWorker.WarmSpin");
    PROF_SPIN("TaskWorker.WarmSpin");
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::microseconds(m_spinMicros);
    for (;;) {
        // steady_clock::now() is ~20 ns here, so only sample it once per batch
        // of pauses - otherwise the clock IS the spin.
        for (unsigned i = 0; i < 64; ++i) {
            if (m_epoch.load(std::memory_order_acquire) != epoch) {
                if (s_wakeStats) s_spinHits.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            YieldProcessor();
        }
        // A shutdown bumps the epoch and would already have been caught above;
        // this covers a close that raced in before the spin started.
        if (m_admission.load(std::memory_order_acquire) & kClosed) return true;
        if (std::chrono::steady_clock::now() >= deadline) break;
    }
    if (s_wakeStats) s_spinGiveUps.fetch_add(1, std::memory_order_relaxed);
    return false;
}

TaskSystem::WakeStats TaskSystem::SampleWakeStats() noexcept {
    WakeStats s;
    s.parks         = s_parks.load(std::memory_order_relaxed);
    s.spinHits      = s_spinHits.load(std::memory_order_relaxed);
    s.spinGiveUps   = s_spinGiveUps.load(std::memory_order_relaxed);
    s.batches       = s_batches.load(std::memory_order_relaxed);
    s.batchTasks    = s_batchTasks.load(std::memory_order_relaxed);
    s.wakeAll       = s_wakeAll.load(std::memory_order_relaxed);
    s.wakeOne       = s_wakeOne.load(std::memory_order_relaxed);
    s.chunkCaller   = s_chunkCaller.load(std::memory_order_relaxed);
    s.chunkHelp     = s_chunkHelp.load(std::memory_order_relaxed);
    s.chunkWorker   = s_chunkWorker.load(std::memory_order_relaxed);
    s.chunkMismatch = s_chunkMismatch.load(std::memory_order_relaxed);
    return s;
}

void TaskSystem::EnqueueAccepted(std::function<void()> body, TaskPriority priority, uint32_t cost,
                                 bool deferWake) {
    // A thread with a submission floor (the render-owner lane) sends its cost-0 High work to
    // the global heap instead of a worker's FIFO shard. Never on a worker: a render job's own
    // sub-tasks keep the shard path.
    if (cost == 0 && t_submitCostFloor != 0 && priority == TaskPriority::High && !t_isTaskWorker)
    {
        cost = t_submitCostFloor;
        m_boostedSubmits.fetch_add(1, std::memory_order_relaxed);
    }
    QueuedTask task{std::move(body)};
    task.estimatedMicros = cost;
    task.frameTag = Diag::Prof::FrameTag();
#ifdef TRACY_ENABLE
    task.queuedAt = std::chrono::steady_clock::now();
#endif
    m_outstanding.fetch_add(1, std::memory_order_relaxed);
    m_queued.fetch_add(1, std::memory_order_relaxed);
    bool published = false;
    try {
        if (cost == 0) {
            const unsigned target = OnWorkerThread() ? t_workerIndex :
                static_cast<unsigned>(m_injection.fetch_add(1, std::memory_order_relaxed) % m_numWorkers);
            auto& queues = *m_queues[target];
            published = priority == TaskPriority::High ? queues.high.Push(std::move(task)) : queues.low.Push(std::move(task));
            if (!published) m_overflows.fetch_add(1, std::memory_order_relaxed);
        }
        if (!published) {
            ZoneScopedN("TaskSystem.EnqueueLock");
            SYS_COUNTED_LOCK(lock, m_mutex, TaskSystemQueue);   // contention probe
            if (priority == TaskPriority::High) PushHigh(std::move(task));
            else { m_lowQueue.push(std::move(task)); m_globalLow.store(m_lowQueue.size(), std::memory_order_release); }
        }
    } catch (...) {
        m_queued.fetch_sub(1, std::memory_order_relaxed);
        m_outstanding.fetch_sub(1, std::memory_order_acq_rel);
        Wake(true);
        throw;
    }
#ifdef TRACY_ENABLE
    // Counts reserved slots not yet published: neither the ready-heap size nor running work.
    TracyPlot("TaskSystem.QueuedOrPublishing", m_queued.load(std::memory_order_relaxed));
#endif
    // The batch path wakes once for the whole group in PublishBatch. The throw path above keeps
    // its own Wake(true): a failed push is not a deferral.
    if (!deferWake) Wake();
}
bool TaskSystem::TryTake(unsigned index, QueuedTask& task, bool& high) {
    auto global = [&](bool highQueue) {
        if (!(highQueue ? m_globalHigh : m_globalLow).load(std::memory_order_acquire)) return false;
        SYS_COUNTED_LOCK(lock, m_mutex, TaskSystemQueue);   // contention probe
        if (highQueue) {
            if (m_highQueue.empty()) return false;
            std::pop_heap(m_highQueue.begin(), m_highQueue.end(), ShorterFirst{});
            task = std::move(m_highQueue.back()); m_highQueue.pop_back();
            m_globalHigh.store(m_highQueue.size(), std::memory_order_release);
        } else {
            if (m_lowQueue.empty()) return false;
            task = std::move(m_lowQueue.front()); m_lowQueue.pop();
            m_globalLow.store(m_lowQueue.size(), std::memory_order_release);
        }
        return true;
    };
    auto shard = [&](bool highQueue) {
        auto pop = [&](unsigned id) {
            return highQueue ? m_queues[id]->high.Pop(task) : m_queues[id]->low.Pop(task);
        };
        if (pop(index)) return true;
        thread_local unsigned t_victim = index;
        const unsigned start = (++t_victim) % m_numWorkers;
        for (unsigned offset=0; offset<m_numWorkers; ++offset) {
            const unsigned id = (start+offset) % m_numWorkers;
            if (id != index && pop(id)) { m_steals.fetch_add(1, std::memory_order_relaxed); return true; }
        }
        return false;
    };
    high = true;
    bool found = global(true) || shard(true);
    if (!found) { high = false; found = global(false) || shard(false); }
    if (found) {
        m_queued.fetch_sub(1, std::memory_order_relaxed);
#ifdef TRACY_ENABLE
        TracyPlot("TaskSystem.QueuedOrPublishing", m_queued.load(std::memory_order_relaxed));
#endif
    }
    return found;
}

void TaskSystem::LogTaskException(const char* context, const char* what)
{
    const char* ctx = context ? context : "task";
    // Thread id is part of the message on purpose: it is the handle that ties
    // this line to the right stack in a minidump (see main.cpp's CrashHandler),
    // and "which thread" is the first question about any worker-side failure.
    if (what)
        LOG_ERROR("TaskSystem: exception escaped %s on thread %lu: %s",
                  ctx, ::GetCurrentThreadId(), what);
    else
        LOG_ERROR("TaskSystem: UNKNOWN exception (not a std::exception) escaped "
                  "%s on thread %lu", ctx, ::GetCurrentThreadId());

    // Flush synchronously. The usual sequel to a half-executed system or render
    // pass is a hard crash a few milliseconds later, and the async logger would
    // still be holding this very line in its queue when the process dies.
    // FlushBlocking is documented safe from any thread.
    Logger::Get().FlushBlocking();
}

void TaskSystem::NotePendingException(std::exception_ptr e)
{
    if (!e)
        return;
    std::lock_guard<std::mutex> lk(m_excMutex);
    if (m_pendingExc)
        return;   // keep the first; the rest are normally its fallout
    m_pendingExc = std::move(e);
    m_hasPendingExc.store(true, std::memory_order_release);
}

void TaskSystem::RethrowPendingTaskException()
{
    std::exception_ptr e;
    {
        std::lock_guard<std::mutex> lk(m_excMutex);
        if (!m_pendingExc)
            return;
        e = std::move(m_pendingExc);
        m_pendingExc = nullptr;
        m_hasPendingExc.store(false, std::memory_order_release);
    }
    // Rethrow OUTSIDE the lock: the handler may itself queue work or tear the
    // pool down, and doing that under m_excMutex is an invitation to deadlock.
    std::rethrow_exception(e);
}

// Active-executor counter. ParallelFor's caller counts too (it runs chunk 0 inline), or
// concurrency would read one low during every parallel region.
void TaskSystem::PlotActive(int delta)
{
#ifdef TRACY_ENABLE
    static std::atomic<int> s_active{ 0 };
    const int v = s_active.fetch_add(delta, std::memory_order_relaxed) + delta;
    TracyPlot("TaskSystem.Active", static_cast<int64_t>(v));
#else
    (void)delta;
#endif
}
