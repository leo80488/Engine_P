#pragma once
#include "System/OwnerTaskQueue.h"

namespace Sys
{
// One frame with independently ready preparation/commit stages. CPU bodies capture owned inputs and
// outputs, never renderer/stack/GPU pointers. Only commit may touch GPU staging.
// The stamp identifies the existing fenced upload lease; it does NOT extend a
// GPU resource lifetime or grant device ownership to a worker.
class FrameTaskContext
{
public:
    FrameTaskContext(OwnerTaskQueue& queue, uint64_t generation, uint32_t slot)
        : queue(queue), generation(generation), uploadSlot(slot) { CheckOwner(); }
    FrameTaskContext(const FrameTaskContext&) = delete;
    ~FrameTaskContext()
    {
        state->cancelled = true;
        try { Finish(); } catch (...) {} // no commit while unwinding
    }
    TaskHandle Submit(std::function<void()> body, std::span<const TaskHandle> dependencies = {})
    {
        CheckOwner();
        if (sealed) throw std::logic_error("Frame preparation already sealed");
        tasks.reserve(tasks.size() + 1); // allocate before accepting work
        tasks.push_back(SubmitCpu(std::move(body), dependencies));
        return tasks.back();
    }
    // Commits just the CPU tasks submitted since the previous stage, plus any
    // explicit dependencies. Independent stages do not wait for earlier stages.
    // Pump only at a phase where ALL accepted stage commits are legal.
    TaskHandle CommitStage(std::function<void()> commit,
                           std::span<const TaskHandle> dependencies = {})
    {
        CheckOwner();
        if (sealed) throw std::logic_error("Frame preparation already sealed");
        std::vector<TaskHandle> deps(tasks.begin() + stageBegin, tasks.end());
        deps.insert(deps.end(), dependencies.begin(), dependencies.end());
        stages.reserve(stages.size() + 1);
        // OwnerTaskQueue already joins dependency completion/error publication.
        // Do not schedule an extra fan-in CPU task or coroutine for each batch.
        // Returned handles remain awaitable by arbitrary CpuTask consumers.
        stages.push_back(queue.Submit([state = state, commit = std::move(commit)] {
            if (!state->cancelled) commit();
        }, deps));
        stageBegin = tasks.size();
        return stages.back();
    }
    void Seal(std::function<void()> commit)
    {
        CheckOwner();
        if (sealed) throw std::logic_error("Frame preparation sealed twice");
        // Final publication waits for every stage, including their exceptions.
        const auto priorStages = stages;
        completion = CommitStage(std::move(commit), priorStages);
        sealed = true;
    }
    void Finish()
    {
        CheckOwner();
        if (!sealed) {
            // Unsealed / partially submitted graphs may have CPU consumers of
            // owner stages. Drain by pumping, never block waiting for our own work.
            auto ready = [](const auto& list) {
                for (const auto& h : list) if (!h.Ready()) return false;
                return true;
            };
            while (!ready(tasks) || !ready(stages)) {
                if (queue.Pump()) continue;
                // CPU-only tails need a completion wait, not an owner CV with
                // zero pending owner nodes (which would spin).
                bool pendingStage = false;
                for (const auto& h : stages) pendingStage |= !h.Ready();
                if (pendingStage) queue.WaitForReady();
                else {
                    for (const auto& h : tasks) try { h.Wait(); } catch (...) {}
                }
            }
            std::exception_ptr error;
            for (const auto* list : {&tasks, &stages})
                for (const auto& h : *list)
                    try { h.Wait(); } catch (...) { if (!error) error = std::current_exception(); }
            if (error) std::rethrow_exception(error);
            return;
        }
        // Only the owner sleeps here if it has exhausted independent work.
        // Workers await preparation and return to the shared pool.
        while (!completion.Ready()) {
            if (!queue.Pump() && !queue.WaitForReady() && !completion.Ready())
                throw std::logic_error("Frame completion lost its owner work");
        }
        completion.Wait(); // ready-only: observe the propagated exception
    }
    uint64_t Generation() const { return generation; }
    uint32_t UploadSlot() const { return uploadSlot; }
    size_t TaskCount() const { return tasks.size(); }

private:
    struct State { bool cancelled = false; }; // owner-only

    void CheckOwner() const
    {
        if (TaskSystem::OnWorkerThread() || std::this_thread::get_id() != owner)
            throw std::logic_error("FrameTaskContext requires its creating owner");
    }

    const std::thread::id owner = std::this_thread::get_id();
    OwnerTaskQueue& queue;
    const uint64_t generation;
    const uint32_t uploadSlot;
    std::shared_ptr<State> state = std::make_shared<State>();
    std::vector<TaskHandle> tasks;
    std::vector<TaskHandle> stages;
    size_t stageBegin = 0;
    TaskHandle completion;
    bool sealed = false;
};
}
