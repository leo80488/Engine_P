#pragma once

// Per-thread running counts of the asset work a caller can attribute to itself. A
// runtime spawn's [SPAWN] census reads the difference across the call on its own
// thread, so a file a worker finishes reading at the same moment never lands in a
// main-thread spawn's count.
//
// Written and read by the thread doing the work: plain thread_local counters, no
// atomics, no locks, nothing to reset. Another thread's numbers are unreadable by
// design.

#include <cstdint>

namespace Resource::LoadCounters
{
    struct Counts
    {
        uint32_t fileReads      = 0;   // AssetFS::ReadFile calls (whether they found the file or not)
        uint32_t materialParses = 0;   // .imat / .imatinst files read and parsed (a cache hit is not one)
        uint32_t meshlibLoads   = 0;   // MeshLibrary::Load calls that read the file (a dedup hit is not one)
    };

    inline thread_local Counts t_counts;
    inline Counts& ThisThread() noexcept { return t_counts; }
}
