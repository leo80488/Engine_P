#pragma once
#include "System/ExecutionDomain.h"

#include "System/Check.h"        // CpuTaskScope's owner-hand-off report
#include "System/TaskSystem.h"
#include "Diagnostics/Profiler.h"  // resume/join scopes + the suspension hook
#include <coroutine>
#include <memory>
#include <span>
#include <stdexcept>
#include <thread>

namespace Sys
{
// Internal bridge. A lease keeps workers alive while a graph is suspended.
class TaskGraphAccess
{
public:
    static bool Acquire() { return TaskSystem::Get().AcquireGraphLease(); }
    static void Release() { TaskSystem::Get().ReleaseGraphLease(); }
    static void Enqueue(std::function<void()> f, uint32_t estimatedMicros = 0)
    { TaskSystem::Get().PushGraphContinuation(std::move(f), estimatedMicros); }
};

namespace TaskDetail
{
struct Completion
{
    std::mutex mutex;
    std::atomic<bool> done{false};
    std::exception_ptr error;
    std::vector<std::function<void(std::exception_ptr)>> continuations;

    bool Ready()
    {
        return done.load(std::memory_order_acquire);
    }
    void Subscribe(std::function<void(std::exception_ptr)> f)
    {
        std::exception_ptr e;
        {
            std::lock_guard lock(mutex);
            if (!done) { continuations.push_back(std::move(f)); return; }
            e = error;
        }
        f(e);
    }
    void Finish(std::exception_ptr e = {}) noexcept
    {
        std::vector<std::function<void(std::exception_ptr)>> ready;
        {
            std::lock_guard lock(mutex);
            error = e;
            ready.swap(continuations);
            done.store(true, std::memory_order_release);
        }
        done.notify_all();
        // Callbacks enqueue, never resume inline. Allocation failure in the
        // runtime is fatal rather than silently losing a completion.
        for (auto& f : ready) f(e);
    }
    void Wait()
    {
        if (!Ready())
        {
            if (TaskSystem::OnWorkerThread())
                throw std::logic_error("Task worker must co_await instead of blocking Wait");
            // Only a wait that actually blocks is a scope: a Ready() handle is
            // the common case (await_resume) and must stay free.
            PROF_WAIT(JoinOrParent, "TaskGraph.Wait");
            done.wait(false, std::memory_order_acquire);
        }
        if (error) std::rethrow_exception(error);
    }
};
struct CoroutineControl;
}

class CpuTask;
class TaskHandle
{
public:
    struct Awaiter;

    TaskHandle() = default; // empty means already successfully completed
    bool Ready() const { return !state || state->Ready(); }
    void Wait() const { if (state) state->Wait(); }

    Awaiter operator co_await() const;

private:
    explicit TaskHandle(std::shared_ptr<TaskDetail::Completion> s) : state(std::move(s)) {}

    friend class CpuTask;
    friend class OwnerTaskQueue;
    friend class ExternalTaskCompletion;
    friend class OwnerTask;
    friend TaskHandle SubmitCpu(std::function<void()>, std::span<const TaskHandle>, uint32_t);

    std::shared_ptr<TaskDetail::Completion> state;
};

// Lazy, move-only orchestration. Start transfers ownership to the runtime;
// dropping the returned completion handle never destroys suspended work.
// Only TaskHandle await is supported; arbitrary awaiters could violate lease
// and executor lifetime contracts. No thread-affine engine work in this task.
class CpuTask
{
public:
    struct promise_type
    {
        std::shared_ptr<TaskDetail::Completion> completion = std::make_shared<TaskDetail::Completion>();
        std::weak_ptr<TaskDetail::CoroutineControl> control;
        std::exception_ptr error;
        CpuTask get_return_object() { return CpuTask(std::coroutine_handle<promise_type>::from_promise(*this)); }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { error = std::current_exception(); }
        TaskHandle await_transform(TaskHandle handle) noexcept { return handle; }
    };
    CpuTask(CpuTask&& other) noexcept : handle(std::exchange(other.handle, {})) {}
    CpuTask(const CpuTask&) = delete;
    ~CpuTask() { if (handle) handle.destroy(); }
    TaskHandle Start();
private:
    explicit CpuTask(std::coroutine_handle<promise_type> h) : handle(h) {}
    std::coroutine_handle<promise_type> handle;
};

namespace TaskDetail
{
struct CoroutineControl : std::enable_shared_from_this<CoroutineControl>
{
    std::coroutine_handle<CpuTask::promise_type> handle;
    uint8_t context = ThreadCtx::Current();
    // The starting site's name, reopened around every resume segment.
    ::Diag::Prof::NameId label = ::Diag::Prof::CurrentLabel();
    // Subscribe may enqueue a resume before await_suspend has returned. This
    // gate prevents another worker from resuming/destroying that frame early.
    std::mutex resumeMutex;
    ~CoroutineControl() { if (handle) handle.destroy(); }
    void Schedule()
    {
        auto self = shared_from_this();
        TaskGraphAccess::Enqueue([self] {
            std::lock_guard lock(self->resumeMutex);
            ZoneScopedN("TaskGraph.CoroutineResume");
            // One resume SEGMENT - the only honest unit of coroutine time.
            PROF_SCOPE_GENERIC("TaskGraph.CoroutineResume");
            PROF_LABEL(self->label);
            ThreadCtx::Scope context(self->context);
            self->handle.resume();
            if (self->handle.done())
            {
                auto completion = self->handle.promise().completion;
                auto error = self->handle.promise().error;
                auto finished = std::exchange(self->handle, {});
                finished.destroy(); // frame captures die before owner observes completion
                completion->Finish(error);
                TaskGraphAccess::Release();
            }
        });
    }
};
}

struct TaskHandle::Awaiter
{
    std::shared_ptr<TaskDetail::Completion> state;
    bool await_ready() const {
        if (!state || state->Ready()) return true;
        if (ExecutionDomain::HasActiveScope())
            throw std::logic_error("Release device execution scope before coroutine suspension");
        return false;
    }
    // Allocation failure must not let a batch skip a still-running child.
    void await_suspend(std::coroutine_handle<CpuTask::promise_type> h) noexcept
    {
        Diag::Prof::NoteSuspend();   // the coroutine rule, see Profiler.h
        auto control = h.promise().control.lock();
        state->Subscribe([control](std::exception_ptr) { control->Schedule(); });
    }
    void await_resume() const { if (state) state->Wait(); }
};
inline TaskHandle::Awaiter TaskHandle::operator co_await() const { return {state}; }

inline TaskHandle CpuTask::Start()
{
    if (!handle) throw std::logic_error("CpuTask already started");
    auto control = std::make_shared<TaskDetail::CoroutineControl>();
    if (!TaskGraphAccess::Acquire()) throw std::runtime_error("TaskSystem shutting down");
    control->handle = std::exchange(handle, {});
    auto completion = control->handle.promise().completion;
    control->handle.promise().control = control;
    try { control->Schedule(); }
    catch (...) { TaskGraphAccess::Release(); throw; }
    return TaskHandle(std::move(completion));
}

inline TaskHandle SubmitCpu(std::function<void()> body, std::span<const TaskHandle> dependencies = {},
                            uint32_t estimatedMicros = 0)
{
    struct Node : TaskDetail::Completion, std::enable_shared_from_this<Node>
    {
        std::function<void()> body;
        std::atomic<size_t> remaining{1}; // registration gate
        std::mutex errorMutex;
        std::exception_ptr dependencyError;
        uint8_t context = ThreadCtx::Current();
        uint32_t estimatedMicros = 0;
        // The submitting site's name (captured by make_shared on the
        // submitting thread), reopened inside the job.
        ::Diag::Prof::NameId label = ::Diag::Prof::CurrentLabel();
        void Arrive(std::exception_ptr e = {}) noexcept
        {
            if (e) { std::lock_guard lock(errorMutex); if (!dependencyError) dependencyError = e; }
            if (remaining.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
            auto self = shared_from_this();
            TaskGraphAccess::Enqueue([self] {
                ZoneScopedN("TaskGraph.Job");
                PROF_SCOPE_GENERIC("TaskGraph.Job");
                PROF_LABEL(self->label);
                ThreadCtx::Scope context(self->context);
                auto error = self->dependencyError;
                if (!error)
                    try { if (self->body) self->body(); }
                    catch (...) { error = std::current_exception(); }
                self->body = {}; // release borrowed captures before publication
                self->Finish(error);
                TaskGraphAccess::Release();
            }, estimatedMicros);
        }
    };
    auto node = std::make_shared<Node>();
    node->body = std::move(body);
    node->estimatedMicros = estimatedMicros;
    if (!TaskGraphAccess::Acquire()) throw std::runtime_error("TaskSystem shutting down");
    // Each successful subscription owns the node. Registration allocation
    // failure is fatal, like ready-queue allocation failure: publishing an
    // error after only partial registration could release borrowed inputs
    // while an unregistered dependency still uses them.
    for (const auto& dependency : dependencies)
    {
        if (!dependency.state) continue;
        node->remaining.fetch_add(1, std::memory_order_relaxed);
        try { dependency.state->Subscribe([node](std::exception_ptr e) { node->Arrive(e); }); }
        catch (...) { std::terminate(); }
    }
    node->Arrive();
    return TaskHandle(std::move(node));
}

inline TaskHandle WhenAll(std::span<const TaskHandle> tasks) { return SubmitCpu({}, tasks); }

// Value parameters belong to the coroutine frame. Launch siblings first, then
// suspend on their fan-in. Even a submission failure must drain prior siblings.
inline CpuTask RunCpuBatch(std::vector<std::function<void()>> jobs)
{
    std::vector<TaskHandle> tasks;
    tasks.reserve(jobs.size());
    std::exception_ptr submitError;
    try { for (auto& job : jobs) tasks.push_back(SubmitCpu(std::move(job))); }
    catch (...) { submitError = std::current_exception(); }
    // Await every sibling on the error path too. This also remains usable if
    // shutdown rejects creating a new WhenAll node after the children started.
    std::exception_ptr first = submitError;
    for (auto& task : tasks)
        try { co_await task; } catch (...) { if (!first) first = std::current_exception(); }
    if (first) std::rethrow_exception(first);
}

// Stack ownership boundary for borrowed input/output. Must live on an owner thread, never
// a worker. Join drains every sibling before reporting failure.
// Owner-to-owner hand-off: the constructor refuses only worker threads, so "constructed on
// main, joined on the render-owner lane" passes it (a lane is an owner) - -brslane does
// this with BuildScene_PlanEmit's planScope and earlyMaterialScope, joined in
// BuildScene_EmitBatches. The scope therefore records its creating thread and reports any
// other thread that touches it, unless AllowOwnerHandoff() was called at the hand-off.
// It reports rather than throws (Join runs from the destructor, also during unwind).
// OwnerHandoffViolations() makes the check testable (task_graph_test's
// TestScopeOwnerHandoff).
class CpuTaskScope
{
public:
    explicit CpuTaskScope(size_t expectedTasks = 0)
    {
        if (TaskSystem::OnWorkerThread())
            throw std::logic_error("CpuTaskScope requires owner thread");
        tasks.reserve(expectedTasks);
    }
    CpuTaskScope(const CpuTaskScope&) = delete;
    ~CpuTaskScope() { try { Join(); } catch (...) {} }
    // Declares that this scope is handed from one owner thread to another. The caller asserts
    // the two are ordered (a barrier separates the hand-off and the join), a claim only the
    // caller can make; Renderer::BuildScene_PlanEmit is the only site.
    void AllowOwnerHandoff() { transferable = true; }
    // Violations seen by this process, for the positive control. Not reset.
    static std::atomic<uint32_t>& OwnerHandoffViolations()
    { static std::atomic<uint32_t> s_violations{0}; return s_violations; }
    TaskHandle Submit(std::function<void()> body)
    {
        CheckOwner("Submit");
        tasks.reserve(tasks.size() + 1); // allocate BEFORE publishing borrowed work
        auto h = SubmitCpu(std::move(body));
        tasks.push_back(h);
        return h;
    }
    TaskHandle Start(CpuTask task)
    {
        CheckOwner("Start");
        tasks.reserve(tasks.size() + 1);
        auto h = task.Start();
        tasks.push_back(h);
        return h;
    }
    void Join()
    {
        // Before the empty early-out on purpose: an unmarked empty scope joined on another thread
        // is the same missing barrier, one frame before it carries work.
        CheckOwner("Join");
        if (tasks.empty()) return;
        ZoneScopedN("TaskGraph.OwnerJoin");
        PROF_WAIT(WaitJoin, "TaskGraph.OwnerJoin");
        std::exception_ptr first;
        for (const auto& h : tasks)
            try { h.Wait(); } catch (...) { if (!first) first = std::current_exception(); }
        tasks.clear();
        if (first) std::rethrow_exception(first);
    }
private:
    void CheckOwner(const char* what)
    {
        if (transferable || owner == std::this_thread::get_id()) return;
        OwnerHandoffViolations().fetch_add(1, std::memory_order_relaxed);
        ENGINE_CHECK_ONCE(false,
            "CpuTaskScope::%s from a thread that did not create it and that was "
            "not told to expect the hand-off (call AllowOwnerHandoff at the "
            "point the scope crosses) - the borrowed inputs it drains are "
            "ordered by a barrier nobody named", what);
    }
    std::vector<TaskHandle> tasks;
    std::thread::id owner = std::this_thread::get_id();
    bool transferable = false;
};

// UE::Tasks FPipe semantics on this task graph: tasks launched into one pipe run one at a
// time, in launch order, on whichever worker takes each - never pinned to a thread, so a
// non-thread-safe service (the Lua VM, the EventBus) gets mutual exclusion without being
// tied to main. Each launch depends on the previous launch's completion; no lock is held
// while a task runs.
// Errors do not stall the pipe: a failed dependency would skip its dependent's body, so
// each body runs under try/catch, the first exception is kept for TakeError, and the
// returned handle completes without error even when the body threw.
// Launch is thread-safe (workers included). WaitIdle blocks, so it is owner-thread only
// (TaskHandle::Wait refuses workers); the destructor waits the same way - a pipe must
// outlive everything launched into it.
class Pipe
{
public:
    Pipe() = default;
    Pipe(const Pipe&)            = delete;
    Pipe& operator=(const Pipe&) = delete;
    ~Pipe() { try { WaitIdle(); } catch (...) {} }

    TaskHandle Launch(std::function<void()> body, uint32_t estimatedMicros = 0)
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto run = [this, b = std::move(body)]() mutable {
            try { b(); }
            catch (...)
            {
                std::lock_guard<std::mutex> e(m_errorMutex);
                if (!m_error) m_error = std::current_exception();
            }
        };
        const TaskHandle prev[1] = { m_last };
        m_last = SubmitCpu(std::move(run), std::span<const TaskHandle>(prev, 1), estimatedMicros);
        ++m_launched;
        return m_last;
    }

    // Owner thread: wait until everything launched so far has run.
    void WaitIdle()
    {
        TaskHandle last;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            last = m_last;
        }
        last.Wait();
    }

    // The first exception a body threw since the last call (null if none).
    std::exception_ptr TakeError()
    {
        std::lock_guard<std::mutex> e(m_errorMutex);
        return std::exchange(m_error, nullptr);
    }

    uint64_t Launched() const
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_launched;
    }

private:
    mutable std::mutex m_mutex;
    TaskHandle         m_last;
    uint64_t           m_launched = 0;
    std::mutex         m_errorMutex;
    std::exception_ptr m_error;
};
}
