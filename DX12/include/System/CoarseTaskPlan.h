#pragma once
#include <algorithm>
#include <cstdint>

namespace Sys
{
// Workload/worker-count adaptation, not an instantaneous idle-worker estimate.
// Reserve roughly half the pool for other systems; the owner is one participant.
// At most two chunks per participant permit redistribution without tiny jobs.
struct CoarseTaskPlan
{
    uint32_t lanes = 1;
    uint32_t chunks = 1;
    static CoarseTaskPlan Make(uint32_t count, uint32_t workers, uint32_t minItems = 1024)
    {
        const uint32_t budget = (std::max)(1u, workers / 2 + workers % 2);
        const uint32_t useful = (std::max)(1u, count / (std::max)(1u, minItems));
        const uint32_t lanes = (std::min)(budget, useful);
        // A single participant needs no atomic chunk distribution.
        return {lanes, lanes == 1 ? 1u : static_cast<uint32_t>((std::min)(uint64_t(useful), uint64_t(lanes) * 2))};
    }
    uint32_t Begin(uint32_t count, uint32_t chunk) const { return uint64_t(count) * chunk / chunks; }
    uint32_t End(uint32_t count, uint32_t chunk) const { return uint64_t(count) * (uint64_t(chunk) + 1) / chunks; }
};
}
