#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <numeric>
#include <span>
#include <stdexcept>
#include <vector>

namespace Sys
{
// Exclusive builder; readers may use batches until the next Build. Costs are
// relative work units, not microseconds. Retains capacity, never input pointers.
class WeightedTaskPlan
{
public:
    void Build(std::span<const uint64_t> costs, uint32_t workers, uint32_t minItems = 4)
    {
        if (costs.size() > UINT32_MAX) throw std::length_error("Weighted task count");
        const auto count = static_cast<uint32_t>(costs.size());
        active = count ? (std::min)((std::max)(1u, workers),
            (std::max)(1u, count / (std::max)(1u, minItems))) : 0;
        order.resize(count);
        std::iota(order.begin(), order.end(), 0u);
        std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
            return costs[a] != costs[b] ? costs[a] > costs[b] : a < b;
        });
        if (batches.size() < active) batches.resize(active);
        for (auto& batch : batches) { batch.items.clear(); batch.cost = 0; }
        for (const auto item : order)
        {
            uint32_t lane = 0;
            for (uint32_t i = 1; i < active; ++i)
                if (batches[i].cost < batches[lane].cost ||
                    (batches[i].cost == batches[lane].cost &&
                     batches[i].items.size() < batches[lane].items.size())) lane = i;
            auto& batch = batches[lane];
            batch.items.push_back(item);
            const auto cost = (std::max)(uint64_t{1}, costs[item]);
            batch.cost += (std::min)(cost, UINT64_MAX - batch.cost);
        }
    }
    uint32_t Count() const { return active; }
    std::span<const uint32_t> Items(uint32_t batch) const { return batches.at(batch).items; }
    uint64_t Cost(uint32_t batch) const { return batches.at(batch).cost; }
private:
    struct Batch { std::vector<uint32_t> items; uint64_t cost = 0; };
    std::vector<Batch> batches;
    std::vector<uint32_t> order;
    uint32_t active = 0;
};
}
