#pragma once

#include "System/TaskGraph.h"
#include "Diagnostics/OwnerAccounting.h"
#include <condition_variable>
#include <deque>

namespace Sys
{
// Explicitly pumped owner executor. Submit is thread-safe; Pump and teardown
// belong to the creating non-worker thread. CpuTask can await its TaskHandle
// without occupying a worker, then resumes on the CPU pool (not the owner).
//
// CloseAndDrain BEFORE TaskSystem::Shutdown. Keep this queue and all borrowed
// frame data alive until producers stop and accepted work drains. A graph lease
// keeps the scheduler alive, not a frame arena or GPU resource. Never Wait on an
// owner completion from the owner itself: pump at a legal application phase.
class OwnerTaskQueue
{
public:
    OwnerTaskQueue() : owner(std::this_thread::get_id()) { CheckOwner(); }
    OwnerTaskQueue(const OwnerTaskQueue&) = delete;
    OwnerTaskQueue& operator=(const OwnerTaskQueue&) = delete;
    ~OwnerTaskQueue() { CloseAndDrain(); }

    TaskHandle Submit(std::function<void()> body, std::span<const TaskHandle> dependencies = {})
    { return SubmitImpl(std::move(body), dependencies, false, true, true); }

    void RunUntil(const TaskHandle& completion)
    {
        CheckOwner();
        while (!completion.Ready())
            if (!Pump() && !WaitForReady() && !completion.Ready())
                throw std::logic_error("Owner completion lost its queued continuation");
        completion.Wait();
    }

    // Nonblocking and bounded. Dependency-waiting nodes never enter this queue
    // and cannot block unrelated ready owner work. Bodies execute without locks.
    size_t Pump(size_t maxTasks = 1)
    {
        CheckOwner();
        size_t executed = 0;
        while (executed < maxTasks)
        {
            std::shared_ptr<Node> node;
            {
                std::lock_guard lock(state->mutex);
                if (state->ready.empty()) break;
                node = std::move(state->ready.front());
                state->ready.pop_front();
            }
            executing = true;
            node->Execute();
            executing = false;
            ++executed;
        }
        return executed;
    }

    // Explicit owner dependency boundary only, after independent owner work.
    // Never called by a CPU worker. Returns false if no accepted work remains.
    bool WaitForReady()
    {
        CheckOwner();
        ZoneScopedN("TaskGraph.OwnerAwaitReady");
        PROF_WAIT(JoinOrParent, "TaskGraph.OwnerAwaitReady");
        std::unique_lock lock(state->mutex);
        state->changed.wait(lock, [&] { return !state->ready.empty() || !state->pending; });
        return !state->ready.empty();
    }

    // Teardown only: atomically reject new submissions, execute every accepted
    // node after ALL dependencies finish, including failures. Errors remain on
    // each returned handle. No spin, worker wait, or implicit frame-time drain.
    void CloseAndDrain()
    {
        CheckOwner();
        { std::lock_guard lock(state->mutex); state->accepting = false; }
        ZoneScopedN("TaskGraph.OwnerDrain");
        for (;;)
        {
            if (Pump()) continue;
            std::unique_lock lock(state->mutex);
            if (!state->pending) break;
            state->changed.wait(lock, [&] { return !state->ready.empty() || !state->pending; });
        }
    }

private:
    struct Node;
    struct State
    {
        std::mutex mutex;
        std::condition_variable changed;
        std::deque<std::shared_ptr<Node>> ready;
        size_t pending = 0;
        bool accepting = true;
    };
    struct Node : TaskDetail::Completion, std::enable_shared_from_this<Node>
    {
        std::shared_ptr<State> queue;
        std::function<void()> body;
        std::atomic<size_t> remaining{1}; // dependency registration gate
        std::mutex errorMutex;
        std::exception_ptr dependencyError;
        uint8_t context = ThreadCtx::Current();
        bool resumeOnError = false;
        bool ownsGraphLease = true;
        // Which render frame's owner budget this node's body belongs to
        // (renderFrame + 1; 0 = not render-half work). Inherited at SUBMIT time
        // from the ambient tag, which is how a coroutine's continuations stay
        // charged to the frame that owns them even though they execute during a
        // later frame's game phases. See Diagnostics/OwnerAccounting.h.
        uint64_t accountTag = 0;

        void Arrive(std::exception_ptr error = {}) noexcept
        {
            if (error) {
                std::lock_guard lock(errorMutex);
                if (!dependencyError) dependencyError = error;
            }
            if (remaining.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
            // Allocation failure is fatal, as for TaskGraph ready publication.
            {
                std::lock_guard lock(queue->mutex);
                queue->ready.push_back(shared_from_this());
            }
            queue->changed.notify_one();
        }
        void Execute() noexcept
        {
            ZoneScopedN("TaskGraph.OwnerContinuation");
            // A render-half node carries its render frame (accountTag - 1):
            // republish it as the profiler's frame tag for the body, so events
            // it records are attributed to the frame that owns them even when
            // they run inside a later frame's game half.
            struct ProfTag
            {
                uint8_t prev = ::Diag::Prof::FrameTag();
                explicit ProfTag(uint64_t tag) { if (tag) ::Diag::Prof::SetFrameTag(tag - 1); }
                ~ProfTag() { ::Diag::Prof::SetFrameTagByte(prev); }
            } profTag{ accountTag };
            PROF_SCOPE("TaskGraph.OwnerContinuation");
            ThreadCtx::Scope scope(context);
            // Owner-active accounting. Republishes accountTag as ambient for
            // the whole body, so a co_await inside it submits its continuation
            // with the same tag. Costs nothing when accountTag is 0.
            Diag::OwnerAccounting::NodeScope charge(accountTag);
            auto error = resumeOnError ? std::exception_ptr{} : dependencyError;
            if (!error)
                try { if (body) body(); }
                catch (...) { error = std::current_exception(); }
            body = {}; // owned frame captures die before completion publication
            Finish(error);
            if (ownsGraphLease) TaskGraphAccess::Release();
            {
                std::lock_guard lock(queue->mutex);
                --queue->pending;
            }
            queue->changed.notify_one();
        }
    };

    friend class OwnerTask;
    // Accepted owner coroutines retain one graph lease for their whole lifetime.
    // Their resumes must survive CloseAndDrain and dependency failures; the
    // coroutine observes the latter at await_resume, so it can drain siblings.
    TaskHandle SubmitImpl(std::function<void()> body, std::span<const TaskHandle> dependencies,
                          bool resumeOnError, bool ownsGraphLease, bool requireAccepting)
    {
        auto node = std::make_shared<Node>();
        node->queue = state;
        node->body = std::move(body);
        node->resumeOnError = resumeOnError;
        node->ownsGraphLease = ownsGraphLease;
        node->accountTag = Diag::OwnerAccounting::AmbientTag();
        {
            std::lock_guard lock(state->mutex);
            if (requireAccepting && !state->accepting) throw std::runtime_error("OwnerTaskQueue closed");
            if (ownsGraphLease && !TaskGraphAccess::Acquire()) throw std::runtime_error("TaskSystem shutting down");
            ++state->pending;
        }
        for (const auto& dependency : dependencies)
        {
            if (!dependency.state) continue;
            node->remaining.fetch_add(1, std::memory_order_relaxed);
            try { dependency.state->Subscribe([node](std::exception_ptr e) { node->Arrive(e); }); }
            catch (...) { std::terminate(); } // cannot abandon partially registered work
        }
        node->Arrive();
        return TaskHandle(std::move(node));
    }

    void CheckOwner() const
    {
        if (TaskSystem::OnWorkerThread() || std::this_thread::get_id() != owner)
            throw std::logic_error("OwnerTaskQueue requires creating owner thread");
        if (executing) throw std::logic_error("OwnerTaskQueue cannot recursively pump or drain");
    }

    const std::thread::id owner;
    std::shared_ptr<State> state = std::make_shared<State>();
    bool executing = false; // owner-only
};
}
