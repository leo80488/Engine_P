#pragma once
#include "System/OwnerTaskQueue.h"
#include <optional>

namespace Sys {
// A lazy coroutine whose execution segments always resume on an explicit owner
// queue. CPU work is submitted separately; awaiting it occupies no CPU worker.
// The creating queue must outlive completion (CloseAndDrain drains accepted
// continuations, including those that suspend again during shutdown).
class OwnerTask {
    struct Control;

public:
    struct promise_type {
        struct Awaiter {
            TaskHandle dependency;
            bool await_ready() const { return dependency.Ready(); }
            void await_suspend(std::coroutine_handle<promise_type> frame) noexcept;
            void await_resume() const { dependency.Wait(); }
        };

        std::shared_ptr<TaskDetail::Completion> completion = std::make_shared<TaskDetail::Completion>();
        std::weak_ptr<Control> control;
        std::exception_ptr error;

        OwnerTask get_return_object() { return OwnerTask(std::coroutine_handle<promise_type>::from_promise(*this)); }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { error = std::current_exception(); }

        Awaiter await_transform(TaskHandle dependency) { return {std::move(dependency)}; }
    };

    OwnerTask(OwnerTask&& other) noexcept : frame(std::exchange(other.frame, {})) {}
    OwnerTask(const OwnerTask&) = delete;
    ~OwnerTask() { if (frame) frame.destroy(); }
    TaskHandle Start(OwnerTaskQueue& queue, std::optional<ExecutionDomain::Token> token = {},
                     const char* zone = "TaskGraph.OwnerResume") {
        if (TaskSystem::OnWorkerThread() || std::this_thread::get_id() != queue.owner)
            throw std::logic_error("OwnerTask requires its queue's owner thread");
        if (!frame) throw std::logic_error("OwnerTask already started");
        auto control = std::make_shared<Control>();
        control->queue = &queue;
        control->token = std::move(token);
        control->zone = zone; // static lifetime name
        if (!TaskGraphAccess::Acquire()) throw std::runtime_error("TaskSystem shutting down");
        control->frame = std::exchange(frame, {});
        control->frame.promise().control = control;
        auto completion = control->frame.promise().completion;
        try { control->Schedule({}, true); }
        catch (...) { TaskGraphAccess::Release(); throw; }
        return TaskHandle(std::move(completion));
    }

private:
    struct Control : std::enable_shared_from_this<Control> {
        OwnerTaskQueue* queue = nullptr;
        std::coroutine_handle<promise_type> frame;
        std::optional<ExecutionDomain::Token> token;
        const char* zone = nullptr;
        uint8_t context = ThreadCtx::Current();
        ~Control() { if (frame) frame.destroy(); }
        void Schedule(TaskHandle dependency, bool root = false) {
            auto self = shared_from_this();
            queue->SubmitImpl([self] { self->Resume(); },
                std::span<const TaskHandle>(&dependency, 1), true, false, root);
        }
        void Resume() noexcept {
            // No lexical profiling or TLS/device RAII scope in the coroutine
            // may span suspension. This runtime restores them per segment.
#ifdef TRACY_ENABLE
            ZoneTransientN(resumeZone, zone, true);
#endif
            // The engine profiler's view of this segment (Profiler.h: the
            // coroutine rule - segments are recorded here, never by scopes
            // written inside the coroutine body across a co_await).
            PROF_SCOPE_DYN(zone);
            ThreadCtx::Scope ctx(context);
            std::exception_ptr setupError;
            try {
                std::optional<ExecutionDomain::Scope> authority;
                if (token) authority.emplace(*token);
#ifdef TRACY_ENABLE
                if (ExecutionDomain::HasActiveScope())
                    ZoneValueV(resumeZone, ExecutionDomain::CurrentGeneration());
#endif
                frame.resume();
            } catch (...) { setupError = std::current_exception(); }
            if (setupError || frame.done()) {
                auto completion = frame.promise().completion;
                auto error = setupError ? setupError : frame.promise().error;
                auto finished = std::exchange(frame, {});
                finished.destroy();
                completion->Finish(error);
                TaskGraphAccess::Release();
            }
        }
    };

    explicit OwnerTask(std::coroutine_handle<promise_type> h) : frame(h) {}

    std::coroutine_handle<promise_type> frame;
};
inline void OwnerTask::promise_type::Awaiter::await_suspend(std::coroutine_handle<promise_type> frame) noexcept {
    Diag::Prof::NoteSuspend();   // the coroutine rule, see Profiler.h
    // Another owner resume cannot run before this segment returns: queue Pump
    // is non-reentrant and confined to one native owner thread.
    frame.promise().control.lock()->Schedule(dependency);
}
}
