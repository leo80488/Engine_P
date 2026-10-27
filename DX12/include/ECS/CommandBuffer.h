#pragma once

// CommandBuffer: defers structural ECS mutations to the phase boundary.
// Add/RemoveComponent or DestroyEntity on a pool that another system of the same
// phase is iterating reshapes its dense vectors mid-iteration. The Scheduler
// hands every phase a CommandBuffer; systems queue mutations here, and the
// Scheduler flushes once every system's Update has returned.
//
// THREADS: enqueue ops are mutex-protected, so systems batched onto TaskSystem
// workers may enqueue concurrently. Flush runs serially at the phase boundary
// (main thread), and the queued lambdas run single-threaded against the World.
//
// DETERMINISTIC REPLAY ORDER. Every op carries the WRITER KEY of the thread that
// queued it (the Scheduler sets system index + 1 around each Update; 0 = no
// system, App code between phases), and Flush replays in (key, submission order):
// registration order, then each system's own order, whatever the worker count or
// timing. Lock order would change the destroy order from run to run and, through
// the World's LIFO free list, the ids the next spawns receive. A system that fans
// its enqueues out across its own ParallelFor still races within its key, so an
// enqueue from a worker with no key set is counted (unkeyedWorkerOps) and
// reported once.
//
// DEFERRED CREATE. CreateEntity returns a PendingEntity that this buffer's ops
// accept and that resolves at Flush in the same order, so a declared (parallel)
// system can spawn without touching the World, which the AccessGuard structural
// tripwire forbids. After the flush, Resolve(p) answers the real id until the
// next flush begins.
//
// -cmdbufv1 replays in lock order instead (escape hatch and A/B arm).

#include "ECS.h"  // Entity, World
#include "System/Check.h"        // ENGINE_CHECK_ONCE - Flush thread contract
#include "System/CommandLine.h"  // -cmdbufv1
#include "System/TaskSystem.h"   // OnWorkerThread
#include "System/ThreadContext.h" // render-context tripwire (game/render split)
#include "System/Log.h"          // -cmdbufverify

#include <algorithm>
#include <cstdint>
#include <functional>
#include <mutex>
#include "System/LockStats.h"   // contention probe (SYS_COUNTED_LOCK)
#include <utility>
#include <vector>

class CommandBuffer
{
public:
    // An entity this buffer will create at the next Flush.
    struct PendingEntity
    {
        uint32_t index = 0xFFFFFFFFu;
        bool Valid() const { return index != 0xFFFFFFFFu; }
    };

    struct WriterScope
    {
        explicit WriterScope(uint32_t key) : m_prev(WriterKey()) { WriterKey() = key; }
        ~WriterScope() { WriterKey() = m_prev; }
        WriterScope(const WriterScope&)            = delete;
        WriterScope& operator=(const WriterScope&) = delete;
    private:
        uint32_t m_prev;
    };

    // The writer key of the calling thread (see the replay-order note above).
    static uint32_t& WriterKey()
    {
        static thread_local uint32_t t_writerKey = 0;
        return t_writerKey;
    }

    // Queue an entity for destruction. An entity already dead at flush time is
    // skipped silently, so two systems may both queue Destroy(e) in one phase.
    void DestroyEntity(Entity e)
    {
        Push([e](World& w) {
            if (w.IsAlive(e)) w.DestroyEntity(e);
        });
    }

    // Queue an AddComponent. Value is moved into the deferred closure so
    // the system's local doesn't have to outlive the phase.
    template <typename T>
    void AddComponent(Entity e, T value)
    {
        Push([e, v = std::move(value)](World& w) mutable {
            if (w.IsAlive(e)) w.AddComponent<T>(e, std::move(v));
        });
    }

    template <typename T>
    void RemoveComponent(Entity e)
    {
        Push([e](World& w) {
            if (w.IsAlive(e)) w.RemoveComponent<T>(e);
        });
    }

    // Generic escape hatch for ops the typed wrappers don't cover (e.g.
    // rename, re-parent, attach socket, fire an event). Caller is
    // responsible for the lambda being safe to invoke against the world
    // at flush time.
    void Defer(std::function<void(World&)> fn)
    {
        Push(std::move(fn));
    }

    // ---- Deferred create ----
    PendingEntity CreateEntity()
    {
        SYS_COUNTED_LOCK(lk, m_mutex, CommandBuffer);
        const PendingEntity p{ m_pendingCreates++ };
        PushLocked([this, p](World& w) { m_resolved[p.index] = w.CreateEntity(); });
        return p;
    }

    template <typename T>
    void AddComponent(PendingEntity p, T value)
    {
        Push([this, p, v = std::move(value)](World& w) mutable {
            const Entity e = ResolveDuringFlush(p);
            if (e != NullEntity && w.IsAlive(e)) w.AddComponent<T>(e, std::move(v));
        });
    }

    // fn(world, the created id) at flush time, in order with the other ops.
    void Defer(PendingEntity p, std::function<void(World&, Entity)> fn)
    {
        Push([this, p, f = std::move(fn)](World& w) {
            const Entity e = ResolveDuringFlush(p);
            if (e != NullEntity) f(w, e);
        });
    }

    // The id a PendingEntity became at the LAST flush (NullEntity before it,
    // and again once the next flush starts).
    Entity Resolve(PendingEntity p) const
    {
        SYS_COUNTED_LOCK(lk, m_mutex, CommandBuffer);
        return p.index < m_lastResolved.size() ? m_lastResolved[p.index] : NullEntity;
    }

    // Apply every queued command. Commands queued from inside a flush callback land
    // in the NEXT flush: the list is swapped out under the lock and executed outside
    // it, so a callback that enqueues cannot deadlock.
    void Flush(World& world)
    {
        // Structural mutations are exactly what parallel batches must never do.
        // Flushing from a worker would replay them concurrently with the rest of the
        // running batch, the corruption this buffer exists to prevent. Phase
        // boundaries run on the main thread; anything else is a wiring bug.
        ENGINE_CHECK_ONCE(!TaskSystem::OnWorkerThread(),
                          "CommandBuffer::Flush on a TaskSystem worker — "
                          "structural replay is racing the running batch; "
                          "flush from the phase boundary instead");
        // Same rule on the other axis (game / render split): the render context
        // replays nothing structural, and a flush from there would mutate pools the
        // game thread owns.
        ENGINE_CHECK_ONCE(!Sys::ThreadCtx::StructuralForbidden(),
                          "[SPLITCHECK][RENDERCTX] CommandBuffer::Flush from "
                          "render context — structural replay belongs to the "
                          "game thread's phase boundaries");
        std::vector<Command> local;
        uint32_t creates = 0;
        {
            SYS_COUNTED_LOCK(lk, m_mutex, CommandBuffer);
            if (m_commands.empty()) return;
            local.swap(m_commands);
            creates = m_pendingCreates;
            m_pendingCreates = 0;
        }
        ENGINE_CHECK_ONCE(m_unkeyedWorkerOps == 0,
                          "CommandBuffer: %llu op(s) were queued from a TaskSystem worker "
                          "with no writer key (a system's own ParallelFor?) - their replay "
                          "order is the lock order, not deterministic",
                          static_cast<unsigned long long>(m_unkeyedWorkerOps));
        static const bool s_v1 = Sys::CommandLine::Get().Has("-cmdbufv1");
        if (!s_v1)
            std::stable_sort(local.begin(), local.end(),
                             [](const Command& a, const Command& b) { return a.key < b.key; });
        // -cmdbufverify: measures the replay ORDER - adjacent key inversions (always 0
        // in keyed order; >0 under -cmdbufv1 whenever batch-mates interleaved) and an
        // FNV of the executed key sequence (identical run to run in keyed order).
        // Logs [CMDBUF] every 256 flushes (order=v1 under -cmdbufv1, v2 for keyed order).
        static const bool s_verify = Sys::CommandLine::Get().Has("-cmdbufverify");
        if (s_verify)
        {
            for (size_t i = 0; i < local.size(); ++i)
            {
                if (i && local[i].key < local[i - 1].key) ++m_verifyInversions;
                m_verifyKeySeq ^= local[i].key;
                m_verifyKeySeq *= 1099511628211ull;
            }
            m_verifyOps += local.size();
            if (++m_verifyFlushes % 256 == 0)
                LOG_INFO("[CMDBUF] order=%s flushes=%llu ops=%llu inversions=%llu keyseq=%016llx",
                         s_v1 ? "v1" : "v2",
                         static_cast<unsigned long long>(m_verifyFlushes),
                         static_cast<unsigned long long>(m_verifyOps),
                         static_cast<unsigned long long>(m_verifyInversions),
                         static_cast<unsigned long long>(m_verifyKeySeq));
        }
        m_resolved.assign(creates, NullEntity);
        for (auto& cmd : local) cmd.fn(world);
        {
            SYS_COUNTED_LOCK(lk, m_mutex, CommandBuffer);
            m_lastResolved = m_resolved;
        }
    }

    bool Empty() const
    {
        SYS_COUNTED_LOCK(lk, m_mutex, CommandBuffer);
        return m_commands.empty();
    }
    size_t Size() const
    {
        SYS_COUNTED_LOCK(lk, m_mutex, CommandBuffer);
        return m_commands.size();
    }

private:
    struct Command
    {
        uint32_t                          key = 0;   // writer key; stable_sort keeps submission order within it
        std::function<void(World&)>       fn;
    };

    void Push(std::function<void(World&)> fn)
    {
        SYS_COUNTED_LOCK(lk, m_mutex, CommandBuffer);
        PushLocked(std::move(fn));
    }
    void PushLocked(std::function<void(World&)> fn)
    {
        const uint32_t key = WriterKey();
        if (key == 0 && TaskSystem::OnWorkerThread()) ++m_unkeyedWorkerOps;
        m_commands.push_back({ key, std::move(fn) });
    }
    Entity ResolveDuringFlush(PendingEntity p) const
    {
        return p.index < m_resolved.size() ? m_resolved[p.index] : NullEntity;
    }

    mutable std::mutex   m_mutex;
    std::vector<Command> m_commands;
    uint32_t             m_pendingCreates = 0;
    uint64_t             m_unkeyedWorkerOps = 0;
    std::vector<Entity>  m_resolved;       // filled DURING a flush (main only)
    std::vector<Entity>  m_lastResolved;   // the last flush's, for Resolve()
    // -cmdbufverify counters (Flush, main only).
    uint64_t             m_verifyFlushes = 0, m_verifyOps = 0, m_verifyInversions = 0;
    uint64_t             m_verifyKeySeq = 1469598103934665603ull;
};
