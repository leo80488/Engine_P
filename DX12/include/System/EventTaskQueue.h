#pragma once
#include "System/FenceCompletionQueue.h"
#include <Windows.h>

namespace Sys {
// Bridges one-shot external events to task completion. The producer must be
// idle/stopped before destruction: Close cancels consumers but cannot cancel
// the external producer's use of an event handle (e.g. a GPU fence registration).
class EventTaskQueue {
public:
    ~EventTaskQueue() { Close(); }
    // arm must register one eventual event signal, or throw. verify runs only
    // in the OS callback, must own its inputs and must not re-enter this queue.
    TaskHandle Request(std::function<void(HANDLE)> arm, std::function<void()> verify) {
        Collect();
        auto item = std::make_unique<Pending>(std::move(verify));
        auto handle = item->source.Handle();
        std::lock_guard lock(mutex);
        if (closed) throw std::runtime_error("Event completion queue closed");
        auto* node = item.get();
        pending.push_back(std::move(item)); // retain event even if producer setup throws
        try {
            arm(node->event);
            SetThreadpoolWait(node->wait, node->event, nullptr);
        } catch (...) { node->source.Complete(std::current_exception()); throw; }
        return handle;
    }
    // Housekeeping only: completion publication does not need this call.
    void Collect() {
        std::vector<std::unique_ptr<Pending>> finished;
        {
            std::lock_guard lock(mutex);
            for (auto it = pending.begin(); it != pending.end(); )
                if ((*it)->signalled.load(std::memory_order_acquire)) {
                    finished.push_back(std::move(*it)); it = pending.erase(it);
                } else ++it;
        }
        // Destructor joins the tiny callback tail outside the container lock.
    }
    // Called after request producers stop, before task-system/owner-queue drain.
    // Keep unsignalled events alive until the external producer is idle/stopped.
    void Close() {
        std::lock_guard lock(mutex);
        if (closed) return;
        closed = true;
        const auto error = std::make_exception_ptr(std::runtime_error("Event completion cancelled"));
        for (auto& item : pending) {
            item->StopCallbacks();
            item->source.Complete(error);
        }
    }

private:
    struct Pending {
        ExternalTaskCompletion source;
        std::function<void()> verify;
        HANDLE event = nullptr;
        PTP_WAIT wait = nullptr;
        std::atomic<bool> signalled{false};
        explicit Pending(std::function<void()> check) : verify(std::move(check)) {
            event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!event) throw std::runtime_error("Completion event allocation failed");
            wait = CreateThreadpoolWait(&Callback, this, nullptr);
            if (!wait) { CloseHandle(event); event = nullptr; throw std::runtime_error("Completion wait allocation failed"); }
        }
        static void CALLBACK Callback(PTP_CALLBACK_INSTANCE, void* context, PTP_WAIT, TP_WAIT_RESULT result) noexcept {
            auto& self = *static_cast<Pending*>(context);
            std::exception_ptr error;
            try {
                if (result != WAIT_OBJECT_0) throw std::runtime_error("Unexpected completion event result");
                self.verify(); // bounded query only; never pump owner work here
            } catch (...) { error = std::current_exception(); }
            self.signalled.store(true, std::memory_order_release);
            self.source.Complete(error);
        }
        void StopCallbacks() noexcept {
            if (!wait) return;
            SetThreadpoolWait(wait, nullptr, nullptr);
            WaitForThreadpoolWaitCallbacks(wait, TRUE);
            CloseThreadpoolWait(wait);
            wait = nullptr;
        }
        ~Pending() {
            StopCallbacks();
            verify = {}; // release retained external objects before event handle
            if (event) CloseHandle(event);
        }
    };

    std::mutex mutex;
    std::vector<std::unique_ptr<Pending>> pending;
    bool closed = false;
};
}
