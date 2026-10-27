#pragma once

// System/BoundedParallel.h - a one-off burst of blocking work (the driver compiling pipelines, DXC
// compiling shaders) on this thread plus a few short-lived OS threads, not on the task system: the
// work blocks in a driver or a compiler, and the frame's workers may be what this frame is about to
// need. Jobs are pulled from one counter in the order given, so a caller that sorts its jobs
// longest-first gets the shortest wall time. Nothing on the helper threads may enter a profiler
// scope: the profiler's thread registry never recycles a slot (Diag::Prof).

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace Sys
{
    // The threads a burst may add besides its caller: the logical cores less two (the caller and the
    // frame's other thread), at least one; @p cap, when non-zero, lowers that.
    inline uint32_t BurstHelpers(uint32_t cap = 0)
    {
        const uint32_t cores = std::max(1u, std::thread::hardware_concurrency());
        uint32_t helpers = cores > 2 ? cores - 2 : 1;
        if (cap != 0) helpers = std::min(helpers, cap);
        return helpers;
    }

    // fn(i) for every i in [0, count), on this thread and at most @p helpers others; returns when every
    // call has. fn must not throw.
    template <class Fn>
    void RunBounded(uint32_t count, uint32_t helpers, Fn&& fn)
    {
        std::atomic<uint32_t> next{ 0 };
        auto drain = [&]
        {
            for (uint32_t i; (i = next.fetch_add(1, std::memory_order_relaxed)) < count;)
                fn(i);
        };
        const uint32_t extra = std::min(helpers, count > 1 ? count - 1 : 0u);
        std::vector<std::thread> pool;
        pool.reserve(extra);
        for (uint32_t t = 0; t < extra; ++t) pool.emplace_back(drain);
        drain();
        for (std::thread& t : pool) t.join();
    }
}
