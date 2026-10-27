#pragma once

// Scheduler: runs one TickPhase's systems against the World.
//
// App calls RunPhase() per phase in execution order; the Scheduler dispatches that phase's
// systems (in parallel batches built from DeclareAccess when the phase allows it), then flushes
// the FrameContext's CommandBuffer so deferred structural changes land before the next phase.
// Per-system and per-phase timing is captured every frame for the editor's Phase Debug panel.

#include "TickPhase.h"
#include "SystemSet.h"

#include <array>
#include <cstdint>
#include <functional>
#include <typeindex>
#include <unordered_set>
#include <vector>

class World;
struct FrameContext;
class SystemRegistry;
namespace Gameplay { class GameFlowStateMachine; }

struct SystemDebugInfo
{
    const char* name        = "";
    float       lastFrameMs = 0.f;
};

struct PhaseDebugInfo
{
    TickPhase   phase       = TickPhase::Input;
    const char* name        = "";
    float       lastFrameMs = 0.f;
    std::vector<SystemDebugInfo> systems;
};

class Scheduler
{
public:
    // Declared component access for one system. Public only because the .cpp's Conflict() helper
    // needs it. Cached inside the batch plan because AccessGuard scopes alias these sets while a
    // batched system runs on a worker: the plan's lifetime is load-bearing (ECS/AccessGuard.h).
    struct AccessSet
    {
        std::unordered_set<std::type_index> reads;
        std::unordered_set<std::type_index> writes;
        std::unordered_set<uint64_t>        exclusives;
        bool                                opaque = false;
    };

    void SetRegistry(SystemRegistry* reg) { m_registry = reg; }
    void SetGameFlow(Gameplay::GameFlowStateMachine* flow) { m_gameFlow = flow; }
    // -schedplan_dump: on every plan rebuild, log each system's declared access as [SCHEDACCESS]
    // lines (the input of tools/frame_dag_sim.py).
    void SetPlanDump(bool on) { m_planDump = on; }

    // A callback at the edge of a phase. It belongs to no SystemSet and has no run mask: it runs in
    // every mode, whatever the game flow gates, before the phase's first system (Begin) or after
    // its last system and the command-buffer flush (End), also for a phase with no system. Hooks of
    // one edge run in the order they were added. Main thread only.
    enum class PhaseEdge : std::uint8_t { Begin, End };
    using PhaseEdgeHook = std::function<void(World&, FrameContext&)>;
    void AddPhaseEdgeHook(TickPhase phase, PhaseEdge edge, PhaseEdgeHook hook);

    // Runs once every system of @p phase that the run mask and the SystemSet gates allow, between
    // the phase's edge hooks. No-op if no registry is bound. Records last-frame ms per system and
    // per phase for GetDebugInfo().
    void RunPhase(TickPhase phase, World& world, FrameContext& ctx);

    // Snapshot of every phase's last-frame timing. Phases with no registered systems still appear
    // (lastFrameMs = 0, empty systems); the caller filters on .systems.empty().
    // Allocates N+1 times (a vector of structs holding vectors): fine for a polling editor window, not
    // on the frame path. Per-frame consumers use PhaseMsExcluding().
    std::vector<PhaseDebugInfo> GetDebugInfo() const;

    // Sum of every phase's last-frame ms except @p exclude: the "Game" half of an Unreal-style
    // stat-unit readout (everything but render submission). Reads the array RunPhase maintains - no
    // allocation, no locking.
    float PhaseMsExcluding(TickPhase exclude) const
    {
        float sum = 0.f;
        for (size_t i = 0; i < m_phaseLastMs.size(); ++i)
            if (i != static_cast<size_t>(exclude)) sum += m_phaseLastMs[i];
        return sum;
    }

private:
    struct BatchPlan
    {
        uint64_t                          generation = 0;
        std::vector<std::vector<size_t>>  batches;
        std::vector<AccessSet>            access;   // per system - see AccessSet
        // Per-system (indexed like the phase's system span): true when the
        // system declared ExclusiveRes::kEventBus (or is opaque), i.e. it may
        // legitimately touch EventBus from a worker and the dispatch must
        // wrap it in an EventBus::ExclusiveGrant for the thread check.
        std::vector<uint8_t>              busGrant;
        // Per system: true when it declared ExclusiveRes::kScriptVM (and is not opaque) - the worker
        // dispatch then holds a ScriptVM::Grant.
        std::vector<uint8_t>              vmGrant;
    };

    // The systems of one phase, without its edge hooks.
    void RunPhaseSystems(TickPhase phase, World& world, FrameContext& ctx);

    // One-shot diff the first time SystemSet gating stops being pass-through:
    // lists every system that was running and is now gated off. Called from
    // RunPhase; self-disarming.
    void LogGatingCliffOnce(const std::unordered_set<SystemSetId>& activeSets,
                            bool unsetRuns);

    SystemRegistry* m_registry = nullptr;
    Gameplay::GameFlowStateMachine* m_gameFlow = nullptr;
    bool            m_gatingCliffLogged = false;
    bool            m_planDump = false;

    // Last-frame totals, indexed by static_cast<size_t>(TickPhase).
    std::array<float, static_cast<size_t>(TickPhase::COUNT)> m_phaseLastMs{};
    // Last-frame per-system ms, parallel to the registry's system span
    // for each phase. Sized lazily by RunPhase.
    std::array<std::vector<float>, static_cast<size_t>(TickPhase::COUNT)> m_systemLastMs;

    // Per-phase parallel batch plan: batches[k] = system indices that can run concurrently.
    // Linear-greedy build: a system joins the current last batch when it conflicts with no one in it,
    // else starts a new batch, so batch order matches registration order. Cached by
    // SystemRegistry::GetGeneration().
    mutable std::array<BatchPlan, static_cast<size_t>(TickPhase::COUNT)> m_batchPlans;

    // Edge hooks per phase: [Begin, End].
    std::array<std::array<std::vector<PhaseEdgeHook>, 2>, static_cast<size_t>(TickPhase::COUNT)>
        m_edgeHooks;
};
