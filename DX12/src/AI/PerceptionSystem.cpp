#include "AI/PerceptionSystem.h"
#include "ECS/PerceptionComponent.h"
#include "ECS/HierarchyComponents.h"      // GlobalTransform
#include "Physics/PhysicsSystem.h"
#include "System/TaskSystem.h"

#include <DirectXMath.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <vector>

namespace AI
{
namespace
{
    struct PerAgentStats { std::uint32_t candidates = 0, rays = 0; };

    inline DirectX::XMFLOAT3 PosOf(const DirectX::XMFLOAT4X4& m)
    {
        return { m._41, m._42, m._43 };
    }

    // Forward is +Z in this engine's convention (see CharacterMotorSystem's
    // FaceClampRate, which builds its target yaw from atan2(dirX, dirZ)).
    inline DirectX::XMFLOAT3 ForwardOf(const DirectX::XMFLOAT4X4& m)
    {
        const float lx = m._31, ly = m._32, lz = m._33;
        const float len = std::sqrt(lx * lx + ly * ly + lz * lz);
        if (len < 1e-6f) return { 0.f, 0.f, 1.f };
        return { lx / len, ly / len, lz / len };
    }

    // One agent's full evaluation. A free function so the serial and parallel
    // paths cannot drift apart.
    PerAgentStats EvaluateAgent(World& world,
                                const DX12Physics::PhysicsSystem& phys,
                                Entity e, PerceptionComponent& pc,
                                float worldTime,
                                std::vector<Entity>& scratch,
                                std::vector<PerceivedEntity>& prev)
    {
        PerAgentStats st;

        // Hoist the two pools the candidate loop probes: each GetComponent<T> is a GetPool<T> (AccessGuard
        // check, type_index hash, map find), twice per candidate otherwise.
        auto* pGlobalXform  = world.GetPool<GlobalTransform>();
        auto* pPerceptible  = world.GetPool<PerceptibleComponent>();
        if (!pGlobalXform) return st;

        const auto* gt = pGlobalXform->Get(e);
        if (!gt) return st;
        const DirectX::XMFLOAT3 origin = PosOf(gt->matrix);
        const DirectX::XMFLOAT3 fwd    = ForwardOf(gt->matrix);
        const DirectX::XMFLOAT3 eye    = { origin.x, origin.y + pc.eyeHeight, origin.z };

        // sightConeDeg is a half-angle (PerceptionComponent.h): do not halve it again. 180 or more is
        // omnidirectional; cos(180 deg) = -1 passes every dot product, so the branch is only an optimisation.
        const bool  omni    = pc.sightConeDeg >= 180.f;
        const float cosHalf = omni ? -1.f
                                   : std::cos(pc.sightConeDeg * 3.14159265f / 180.f);
        const float radiusSq = pc.sightRange * pc.sightRange;

        // ---- Stage 1: broad phase ----
        scratch.clear();
        const DX12Physics::LayerMask mask =
            pc.layerMask != 0 ? static_cast<DX12Physics::LayerMask>(pc.layerMask)
                              : DX12Physics::MakeLayerMask(DX12Physics::Layer::Character);
        phys.OverlapSphere(origin, pc.sightRange, scratch, mask, pc.maxCandidates);
        st.candidates = static_cast<std::uint32_t>(scratch.size());

        // visibleEntities is rebuilt each tick, but the previous list is kept aside so firstSeenTime
        // survives a continuous sighting: a target held in view for 20 s must not read as newly spotted
        // ("how long have I been looking" gates read it).
        prev.clear();
        prev.swap(pc.visibleEntities);

        Entity bestEntity = NullEntity;
        float  bestDistSq = 3.4e38f;

        for (Entity cand : scratch)
        {
            if (cand == e) continue;

            // ---- Stage 2: faction + exact distance ----
            const auto* pt = pPerceptible ? pPerceptible->Get(cand) : nullptr;
            if (!pt || !pt->enabled || pt->faction == pc.faction) continue;

            const auto* cgt = pGlobalXform->Get(cand);
            if (!cgt) continue;
            const DirectX::XMFLOAT3 cpos = PosOf(cgt->matrix);
            const float dx = cpos.x - origin.x;
            const float dy = cpos.y - origin.y;
            const float dz = cpos.z - origin.z;
            const float distSq = dx * dx + dy * dy + dz * dz;
            if (distSq > radiusSq) continue;          // AABB slop from stage 1

            // ---- Stage 3: field of view ----
            if (!omni)
            {
                const float horizLen = std::sqrt(dx * dx + dz * dz);
                if (horizLen > 1e-4f)
                {
                    const float dot = (fwd.x * dx + fwd.z * dz) / horizLen;
                    if (dot < cosHalf) continue;
                }
            }

            // ---- Stage 4: line of sight ----
            const DirectX::XMFLOAT3 aim = { cpos.x, cpos.y + pt->offsetY, cpos.z };
            if (pc.requireLineOfSight)
            {
                const DirectX::XMFLOAT3 ray = { aim.x - eye.x, aim.y - eye.y, aim.z - eye.z };
                const float rayLen = std::sqrt(ray.x * ray.x + ray.y * ray.y + ray.z * ray.z);
                if (rayLen > 1e-4f)
                {
                    ++st.rays;
                    // Static environment only. Including Character would make
                    // agents block each other's sight, which reads as the AI
                    // randomly going blind in a crowd.
                    const DX12Physics::RayHit h = phys.CastRayClosest(
                        eye, ray, rayLen, DX12Physics::MakeLayerMask(DX12Physics::Layer::StaticEnv));
                    // A hit short of the target means something solid is in
                    // the way. The 0.98 slack absorbs the target's own
                    // collider when it sits on a masked layer.
                    if (h.hit && h.distance < rayLen * 0.98f) continue;
                }
            }

            // ---- Confirmed sighting ----
            PerceivedEntity pe;
            pe.entity       = cand;
            pe.generation   = world.GetGeneration(cand);
            pe.lastKnownPos = aim;
            pe.lastSeenTime = worldTime;
            pe.firstSeenTime = worldTime;
            for (const auto& old : prev)
                if (old.entity == cand) { pe.firstSeenTime = old.firstSeenTime; break; }
            pc.visibleEntities.push_back(pe);

            if (distSq < bestDistSq) { bestDistSq = distSq; bestEntity = cand; }
        }

        // ---- Sticky threat ----
        // Refreshed on sight, otherwise kept until threatMemory expires (or the threat dies). This is what
        // makes "go look where I lost them" possible: visibleEntities alone is stateless.
        if (bestEntity != NullEntity)
        {
            pc.lastKnownThreat = bestEntity;
            for (const auto& v : pc.visibleEntities)
                if (v.entity == bestEntity) { pc.lastKnownThreatPosition = v.lastKnownPos; break; }
            pc.lastSeenTime = worldTime;
        }
        else if (pc.lastKnownThreat != NullEntity)
        {
            const bool expired = (worldTime - pc.lastSeenTime) > pc.threatMemory;
            if (expired || !world.IsAlive(pc.lastKnownThreat))
                pc.lastKnownThreat = NullEntity;
        }

        pc.lastCandidates = static_cast<std::uint16_t>(st.candidates);
        pc.lastRays       = static_cast<std::uint16_t>(st.rays);
        return st;
    }
}

void PerceptionSystem::Update(World& world, const DX12Physics::PhysicsSystem& phys,
                              float dt)
{
    ZoneScopedN("AI.Perception");

    // Reset lastCpuMs with the rest: it is written only on frames where an agent was due, and a stale
    // value would report a parallel speedup with parallelism off.
    lastAgents = lastTicked = lastCandidates = lastRays = 0;
    lastCpuMs = 0.f;
    if (dt <= 0.f) return;
    m_worldTime += dt;
    const float worldTime = m_worldTime;

    auto* pool = world.GetPool<PerceptionComponent>();
    if (!pool) return;

    const auto& ents = pool->Entities();
    auto&       data = pool->Data();
    const size_t n   = data.size();
    lastAgents = static_cast<std::uint32_t>(n);
    if (n == 0) return;

    // Interval gate + per-frame budget, as in AISystem::Update and for the same reason: hashed phase
    // offsets alone collapse into floor(interval / dt) frame buckets, and the longer frame carrying a
    // bucket drags its neighbours in until one convoy carries everybody (0 / 0 / 100 at 30 Hz with 100
    // agents). The budget caps each frame at the expected arrival count, sum(dt / tickInterval), most
    // overdue first; agents left out outrank everyone next frame, so it self-levels with no starvation.
    m_due.clear();
    m_due.reserve(n);
    float expected = 0.f;
    for (size_t i = 0; i < n; ++i)
    {
        PerceptionComponent& pc = data[i];
        if (!pc.phaseSeeded)
        {
            pc.phaseSeeded = true;
            std::uint32_t h = static_cast<std::uint32_t>(ents[i]) * 2654435761u;
            h ^= h >> 15;
            pc.timeSinceLastTick =
                pc.tickInterval * (static_cast<float>(h & 0xFFFFu) / 65535.f);
        }
        expected += (pc.tickInterval > 1e-6f) ? (dt / pc.tickInterval) : 1.f;
        pc.timeSinceLastTick += dt;
        if (pc.timeSinceLastTick < pc.tickInterval) continue;
        m_due.push_back(i);
    }
    if (!m_due.empty())
    {
        // ceil, never zero: one agent on a long interval rounds the expected
        // arrival rate to 0 and would never be picked.
        size_t budget = static_cast<size_t>(expected) + 1u;
        if (budget > m_due.size()) budget = m_due.size();
        if (budget < m_due.size())
        {
            std::partial_sort(
                m_due.begin(), m_due.begin() + budget, m_due.end(),
                [&data](size_t a, size_t b)
                {
                    return (data[a].timeSinceLastTick - data[a].tickInterval)
                         > (data[b].timeSinceLastTick - data[b].tickInterval);
                });
            m_due.resize(budget);
        }
        // Consume the interval only for the agents that run. The debt is clamped to one interval: an agent
        // whose interval just dropped (5 s offscreen -> 0.1 s combat) would otherwise owe 50 back-to-back
        // evaluations on the frame it came into view.
        for (size_t idx : m_due)
        {
            PerceptionComponent& pc = data[idx];
            pc.timeSinceLastTick -= pc.tickInterval;
            if (pc.timeSinceLastTick > pc.tickInterval || pc.timeSinceLastTick < 0.f)
                pc.timeSinceLastTick = 0.f;
        }
    }
    lastTicked = static_cast<std::uint32_t>(m_due.size());
    if (m_due.empty()) return;

    // steady_clock, not high_resolution_clock: the latter aliases system_clock
    // in this toolchain and is not monotonic, so a clock correction mid-run
    // could produce a negative sample and poison a p99.
    using Clock = std::chrono::steady_clock;
    const auto phaseT0 = Clock::now();

    if (!parallel || m_due.size() < 8)
    {
        std::vector<Entity>          scratch;
        std::vector<PerceivedEntity> prev;
        for (size_t idx : m_due)
        {
            const PerAgentStats st =
                EvaluateAgent(world, phys, ents[idx], data[idx], worldTime, scratch, prev);
            lastCandidates += st.candidates;
            lastRays       += st.rays;
        }
        // Serial: CPU time IS wall time. Measuring it per agent here would
        // only add a clock read per agent for the same answer.
        lastCpuMs = std::chrono::duration<float, std::milli>(Clock::now() - phaseT0).count();
    }
    else
    {
        std::atomic<std::uint32_t> cands{ 0 }, rays{ 0 };
        // Nanoseconds, accumulated per agent across whichever thread ran it.
        // Summed in integer ns rather than float ms because hundreds of
        // relaxed float adds lose the small samples to rounding.
        std::atomic<std::uint64_t> cpuNs{ 0 };
        const auto& due = m_due;
        // Interleaved, not contiguous: per-agent cost varies by an order of
        // magnitude with local crowd density, so a contiguous split hands one
        // worker the whole plaza and the rest an empty street.
        //
        // ParallelFor calls back per INDEX, so the candidate buffer cannot be
        // a local. thread_local keeps one buffer per worker and lets it reach
        // its steady-state capacity instead of reallocating per agent.
        TaskSystem::Get().ParallelFor(0u, static_cast<uint32_t>(due.size()),
            [&](uint32_t i)
            {
                thread_local std::vector<Entity>          t_scratch;
                thread_local std::vector<PerceivedEntity> t_prev;
                const size_t idx = due[i];
                const auto t0 = Clock::now();
                const PerAgentStats st =
                    EvaluateAgent(world, phys, ents[idx], data[idx], worldTime, t_scratch, t_prev);
                cpuNs.fetch_add(static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        Clock::now() - t0).count()), std::memory_order_relaxed);
                cands.fetch_add(st.candidates, std::memory_order_relaxed);
                rays.fetch_add(st.rays, std::memory_order_relaxed);
            },
            TaskSystem::TaskPriority::High,
            TaskSystem::ParallelSchedule::Interleaved);
        lastCandidates = cands.load(std::memory_order_relaxed);
        lastRays       = rays.load(std::memory_order_relaxed);
        lastCpuMs      = static_cast<float>(cpuNs.load(std::memory_order_relaxed)) * 1e-6f;
    }

#ifdef TRACY_ENABLE
    TracyPlot("Perc.Agents",     static_cast<int64_t>(lastAgents));
    TracyPlot("Perc.Ticked",     static_cast<int64_t>(lastTicked));
    TracyPlot("Perc.Candidates", static_cast<int64_t>(lastCandidates));
    TracyPlot("Perc.Rays",       static_cast<int64_t>(lastRays));
#endif
}

} // namespace AI
