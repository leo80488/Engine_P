#pragma once

// PerceptionSystem - fills every PerceptionComponent's sightings each tick.
// Per agent, cheapest test first, since each stage filters the next stage's cost:
//   1. broad-phase sphere overlap (one query, conservative candidate set)
//   2. faction + exact distance   (drops the AABB slop and the own team)
//   3. FOV dot product            (drops everything behind the agent)
//   4. line-of-sight ray          (the only per-pair geometry cost, reached by the fewest pairs)
// Keep this order: it is the difference between a linear and a quadratic AI.
// Runs in TickPhase::AI before the behaviour trees, so a tree reads a sighting the frame it happens.

#include "ECS/ECS.h"
#include <cstdint>
#include <vector>

namespace DX12Physics { class PhysicsSystem; }

namespace AI
{
    class PerceptionSystem
    {
    public:
        // firstSeenTime, lastSeenTime and the threatMemory test use a play clock accumulated here from dt
        // (FrameContext has no world time), counted from the first Update of the session. Only differences
        // of it are compared, so its origin does not matter.
        void Update(World& world, const DX12Physics::PhysicsSystem& phys, float dt);

        // Fans the per-agent loop across the task system (when 8 or more agents are due). Off by default:
        // the work is safe (read-only on the world, each agent writes only its own component, and Jolt's
        // broad phase allows concurrent readers since the physics step runs in a later phase), but the AI
        // benchmark measures exactly what workers gain, and a mode that switched itself by population would
        // hide that. The caller turns it on.
        bool parallel = false;

        // Last-frame totals, for plots and the benchmark report.
        std::uint32_t lastAgents     = 0;   // agents carrying the component
        std::uint32_t lastTicked     = 0;   // agents that passed the interval gate
        std::uint32_t lastCandidates = 0;   // broad-phase results, summed
        std::uint32_t lastRays       = 0;   // line-of-sight rays, summed

        // Per-agent evaluation time summed over every thread that ran one: equal to the wall time when
        // serial, larger when fanned out (the ratio is the achieved speedup).
        float         lastCpuMs      = 0.f;

    private:
        // Indices of the agents whose interval elapsed this frame; a member so the per-frame allocation is
        // reused.
        std::vector<std::size_t> m_due;
        float                    m_worldTime = 0.f;
    };
}
