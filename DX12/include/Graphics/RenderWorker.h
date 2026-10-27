#pragma once
#include "System/TaskGraph.h"
#include <algorithm>

// Owner-side slot for one coarse command-recording task. No dedicated thread
// or semaphore: idle capacity belongs to the shared TaskSystem. Command lists
// are allocated by the render owner before Kick and consumed only after Join.
// One owner submits/joins; the worker updates its estimate before completion.
struct RenderWorker
{
    RenderWorker() = default;
    RenderWorker(const RenderWorker&) = delete;
    RenderWorker& operator=(const RenderWorker&) = delete;
    ~RenderWorker() { Shutdown(); }

    void Kick(std::function<void()> job)
    {
        if (TaskSystem::OnWorkerThread())
            throw std::logic_error("Recording slot requires a render owner");
        Join(); // observe an unclaimed failure before replacing its handle
        Sys::ThreadCtx::Scope context(Sys::ThreadCtx::Render);
        pending = Sys::SubmitCpu([this, job = std::move(job)] {
            ZoneScopedN("RenderWorker.Job");
            struct Measure
            {
                uint32_t& estimate;
                std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
                ~Measure()
                {
                    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - start).count();
                    // Wall-time scheduling hint, not a CPU-utilization metric.
                    const auto sample = std::clamp<int64_t>(us, 1, 1000000);
                    estimate = static_cast<uint32_t>((3ull * estimate + sample) / 4);
                }
            } measure{estimatedMicros};
            job();
        }, {}, estimatedMicros);
    }

    void Join()
    {
        if (TaskSystem::OnWorkerThread())
            throw std::logic_error("Recording join requires a render owner");
        auto task = std::exchange(pending, {});
        task.Wait(); // reports exceptions on the owner, never on a pool worker
    }

    Sys::TaskHandle Completion() const { return pending; }

    void Shutdown() noexcept
    {
        try { Join(); } catch (...) {} // teardown must drain without throwing
    }

private:
    Sys::TaskHandle pending;
    uint32_t estimatedMicros = 100; // bootstrap only; EWMA learns each slot
};
