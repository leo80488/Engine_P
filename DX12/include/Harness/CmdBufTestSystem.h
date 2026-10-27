#pragma once
// Harness/CmdBufTestSystem.h - the CommandBuffer determinism fixture, registered
// only under -cmdbuftest.
//
// Two DECLARED systems that the linear-greedy planner puts in ONE parallel batch
// (disjoint writes, no exclusives), so they really run concurrently on workers.
// Every frame each destroys its half of a tagged population (by id parity) and
// queues deferred creates to replace it, all through the phase's CommandBuffer.
// Destroy order decides which ids the World hands out next (LIFO free list; the
// quarantine also releases in destroy order), so the population's ids - and
// [WORLDHASH] - repeat run to run only if the buffer replays in (writer key,
// submission) order rather than lock order. This fixture exercises exactly that.
// Header-only: no vcxproj registration.

#include "ECS/ECS.h"
#include "ECS/ISystem.h"
#include "ECS/FrameContext.h"
#include "ECS/CommandBuffer.h"

#include <chrono>
#include <cstdint>

struct CmdBufTestTag
{
    uint32_t generation = 0;   // how many times this slot of the population was recycled
};

class CmdBufTestSystem : public SystemInPhase<TickPhase::GameplayPreLogic>
{
public:
    static constexpr uint32_t kPopulation = 256;

    explicit CmdBufTestSystem(uint32_t parity) : m_parity(parity & 1u) {}

    const char* GetName() const override
    {
        return m_parity ? "CmdBufTestSystem1" : "CmdBufTestSystem0";
    }

    void DeclareAccess(SystemAccessBuilder& b) const override
    {
        // Reads the tag pool; everything structural goes through the buffer,
        // so nothing else needs declaring (and nothing else is touched).
        b.Read<CmdBufTestTag>();
    }

    void Update(World& world, const FrameContext& ctx) override
    {
        CommandBuffer* cmd = ctx.commandBuffer;
        if (!cmd) return;
        const auto* pool = world.GetPool<CmdBufTestTag>();
        const uint32_t live = pool ? static_cast<uint32_t>(pool->Entities().size()) : 0u;
        if (live == 0)
        {
            // Seed: each system creates its half of the population once.
            for (uint32_t i = m_parity; i < kPopulation; i += 2)
            {
                const auto p = cmd->CreateEntity();
                cmd->AddComponent<CmdBufTestTag>(p, CmdBufTestTag{ 0u });
            }
            return;
        }
        const auto& ents = pool->Entities();
        const auto& data = pool->Data();
        for (size_t k = 0; k < ents.size(); ++k)
        {
            if ((static_cast<uint32_t>(ents[k]) & 1u) != m_parity) continue;
            cmd->DestroyEntity(ents[k]);
            const auto p = cmd->CreateEntity();
            cmd->AddComponent<CmdBufTestTag>(p, CmdBufTestTag{ data[k].generation + 1u });
            // ~2 us of "work" per entity, so the two batch-mates' enqueues really
            // interleave (without it the calling thread's system finishes before the
            // worker's has woken, and a lock-order replay would look deterministic by
            // accident).
            const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(2);
            while (std::chrono::steady_clock::now() < until) {}
        }
    }

private:
    uint32_t m_parity;
};
