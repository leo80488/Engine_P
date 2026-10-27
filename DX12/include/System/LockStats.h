#pragma once

// Sys::LockStats - contention probe for the engine's shared mutexes.
//
// A lock rework (spin with random backoff, thread-local accumulation) pays off
// only where a lock is actually contended, and the profiler cannot say where:
// a thread blocked in std::mutex::lock() parks in the kernel and shows as IDLE,
// not as Spin or Wait. This counts it directly: every armed acquisition first
// tries try_lock(); only a failed try is contention, and only that path pays
// for a clock read around the blocking lock().
//
// -lockstats[=N] arms it (FramePipeline logs [LOCKSTATS] every N frames,
// default 300). Unarmed, Lock() is lock() behind one well-predicted branch on
// a latched static, like the other harness probes.
//
// Header-only on purpose: the counters are inline variables, so this adds no
// translation unit to either project.

#include "System/CommandLine.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <type_traits>

namespace Sys::LockStats
{
    enum class Id : uint32_t
    {
        TaskSystemQueue,     // TaskSystem::m_mutex - the global heaps (cost != 0 / shard overflow)
        CommandBuffer,       // ECS CommandBuffer::m_mutex - enqueue from co-batched systems
        ResourceManager,     // Resource::ResourceManager::m_mutex - handle table / states
        ResourceUploads,     // Resource::ResourceManager::m_uploadMutex - the GPU upload list
        Count
    };

    inline const char* Name(Id id)
    {
        switch (id)
        {
        case Id::TaskSystemQueue: return "TaskSystem.queue";
        case Id::CommandBuffer:   return "CommandBuffer";
        case Id::ResourceManager: return "ResourceManager";
        case Id::ResourceUploads: return "ResourceManager.uploads";
        default:                  return "?";
        }
    }

    struct Counter
    {
        std::atomic<uint64_t> acquired{0};
        std::atomic<uint64_t> contended{0};
        std::atomic<uint64_t> waitNs{0};
        std::atomic<uint64_t> maxWaitNs{0};
    };

    inline Counter g_counters[static_cast<size_t>(Id::Count)];

    // Latched once. The TaskSystem can take its first lock before the command
    // line is parsed only if it starts before Sys::CommandLine::Initialize,
    // which main.cpp orders the other way round.
    inline bool Armed()
    {
        static const bool s_on = Sys::CommandLine::Get().Has("-lockstats");
        return s_on;
    }

    template <class Mutex>
    inline void Lock(Mutex& m, Id id)
    {
        if (!Armed()) { m.lock(); return; }
        Counter& c = g_counters[static_cast<size_t>(id)];
        c.acquired.fetch_add(1, std::memory_order_relaxed);
        if (m.try_lock()) return;
        const auto t0 = std::chrono::steady_clock::now();
        m.lock();
        const uint64_t ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - t0).count());
        c.contended.fetch_add(1, std::memory_order_relaxed);
        c.waitNs.fetch_add(ns, std::memory_order_relaxed);
        uint64_t prev = c.maxWaitNs.load(std::memory_order_relaxed);
        while (ns > prev && !c.maxWaitNs.compare_exchange_weak(prev, ns, std::memory_order_relaxed)) {}
    }
}

// `std::lock_guard name(mtx);` with the acquisition counted. The guard adopts
// the lock Lock() took, so scope and unlock are exactly a lock_guard's.
#define SYS_COUNTED_LOCK(name, mtx, id)                                        \
    ::Sys::LockStats::Lock((mtx), ::Sys::LockStats::Id::id);                   \
    std::lock_guard<std::remove_reference_t<decltype(mtx)>> name((mtx), std::adopt_lock)
