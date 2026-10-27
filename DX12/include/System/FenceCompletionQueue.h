#pragma once
#include "System/TaskGraph.h"
#include <array>

namespace Sys {
// Producer owns the scheduler lease until completion or cancellation. Never
// destroy the producer while silently leaving consumers suspended.
class ExternalTaskCompletion {
public:
    ExternalTaskCompletion() : state(std::make_shared<TaskDetail::Completion>()) {
        if (!TaskGraphAccess::Acquire()) throw std::runtime_error("TaskSystem shutting down");
    }
    ExternalTaskCompletion(const ExternalTaskCompletion&) = delete;
    ~ExternalTaskCompletion() {
        if (!claimed.load()) Complete(std::make_exception_ptr(std::runtime_error("External task cancelled")));
    }
    TaskHandle Handle() const { return TaskHandle(state); }
    void Complete(std::exception_ptr error = {}) noexcept {
        if (claimed.exchange(true)) return;
        state->Finish(error);
        TaskGraphAccess::Release();
    }
private:
    std::shared_ptr<TaskDetail::Completion> state;
    std::atomic<bool> claimed{false};
};

class FenceCompletionQueue {
public:
    // All targets must retire. D3D12's UINT64_MAX means device removed, not
    // successful retirement, even if another queue is still below its target.
    static bool Reached(const std::array<uint64_t, 3>& completed,
                        const std::array<uint64_t, 3>& target) {
        bool ready = true;
        for (size_t i = 0; i < 3; ++i) {
            if (!target[i]) continue;
            if (completed[i] == UINT64_MAX) throw std::runtime_error("GPU fence device removed");
            ready &= completed[i] >= target[i];
        }
        return ready;
    }
    ~FenceCompletionQueue() { Close(); }
    TaskHandle Request(std::function<bool()> probe) {
        auto source = std::make_shared<ExternalTaskCompletion>();
        auto handle = source->Handle();
        std::lock_guard lock(mutex);
        if (closed) throw std::runtime_error("Fence completion queue closed");
        pending.push_back({std::move(probe), std::move(source)});
        return handle;
    }
    // Call at owner progress points. Probes must be bounded nonblocking queries,
    // retain their fence objects, and must not call back into this queue.
    void Poll() {
        std::vector<std::pair<std::shared_ptr<ExternalTaskCompletion>, std::exception_ptr>> ready;
        {
            std::lock_guard lock(mutex);
            for (auto it = pending.begin(); it != pending.end();) {
                bool done = false; std::exception_ptr error;
                try { done = it->probe(); } catch (...) { done = true; error = std::current_exception(); }
                if (!done) { ++it; continue; }
                ready.emplace_back(it->source, error);
                it = pending.erase(it); // release fence captures before publication
            }
        }
        for (auto& [source, error] : ready) source->Complete(error);
    }
    // Must run before TaskSystem shutdown, after producers stop. Cancellation
    // does not grant permission to reuse GPU-owned resources.
    void Close() {
        std::vector<Pending> cancelled;
        { std::lock_guard lock(mutex); closed = true; cancelled.swap(pending); }
        const auto error = std::make_exception_ptr(std::runtime_error("GPU fence completion cancelled"));
        for (auto& item : cancelled) { item.probe = {}; item.source->Complete(error); }
    }

private:
    struct Pending {
        std::function<bool()> probe;
        std::shared_ptr<ExternalTaskCompletion> source;
    };

    std::mutex mutex;
    std::vector<Pending> pending;
    bool closed = false;
};
}
