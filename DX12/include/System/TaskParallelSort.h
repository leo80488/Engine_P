#pragma once
#include "System/TaskSystem.h"
#include <algorithm>
#include <stdexcept>

namespace Sys {
// Scratch belongs to the caller and must not alias values. Each phase joins
// before swapping buffers; workers never retain iterators across a phase.
// Copyable records only. Equivalent records have unspecified relative order,
// matching std::sort. The comparator must be safe for concurrent calls.
template<class T, class Compare>
void TaskParallelSort(std::vector<T>& values, std::vector<T>& scratch, Compare less,
                      bool parallelTail = true)
{
    ZoneScopedN("TaskSystem.Sort");
    if (&values == &scratch) throw std::invalid_argument("sort scratch aliases input");
    const size_t count = values.size();
    const size_t budget = (std::min)(size_t(TaskSystem::Get().GetWorkerCount()), count / 2048);
    const uint32_t lanes = static_cast<uint32_t>((std::max)(size_t(1), budget));
    if (lanes == 1) {
        std::sort(values.begin(), values.end(), less);
        return;
    }
    scratch.resize(count);
    auto boundary = [count, lanes](size_t lane) {
        return (count / lanes) * lane + (std::min)(lane, count % lanes);
    };
    auto& pool = TaskSystem::Get();
    pool.ParallelFor(0, lanes, [&](uint32_t lane) {
        ZoneScopedN("TaskSystem.Sort.Run");
        std::sort(values.begin() + boundary(lane), values.begin() + boundary(lane + 1), less);
    });
    for (size_t width = 1; width < lanes; width *= 2) {
        const uint32_t pairs = static_cast<uint32_t>((lanes + width * 2 - 1) / (width * 2));
        auto merge = [&](uint32_t pair) {
            ZoneScopedN("TaskSystem.Sort.Merge");
            const size_t first = size_t(pair) * width * 2;
            const size_t lo = boundary(first);
            const size_t mid = boundary((std::min)(first + width, size_t(lanes)));
            const size_t hi = boundary((std::min)(first + width * 2, size_t(lanes)));
            // An odd last run still has to reach the output buffer before
            // swapping. Merging with an empty right span copies it exactly once.
            std::merge(values.begin() + lo, values.begin() + mid,
                       values.begin() + mid, values.begin() + hi, scratch.begin() + lo, less);
        };
        // Split only large final merges. Binary searches partition the OUTPUT
        // ranks, so even duplicate-heavy or skewed inputs have balanced writes.
        const uint32_t tailLanes = static_cast<uint32_t>((std::min)(size_t(lanes), count / 16384));
        if (pairs == 1 && parallelTail && tailLanes > 1) {
            const size_t mid = boundary(width), left = mid, right = count - mid;
            auto coRank = [&](size_t rank) {
                size_t low = rank > right ? rank - right : 0;
                size_t high = (std::min)(rank, left);
                while (low <= high) {
                    const size_t a = low + (high - low) / 2, b = rank - a;
                    if (a && b < right && less(values[mid + b], values[a - 1])) high = a - 1;
                    else if (b && a < left && !less(values[mid + b - 1], values[a])) low = a + 1;
                    else return a; // Equal keys take the left run first, like std::merge.
                }
                throw std::logic_error("merge comparator is not a strict weak order");
            };
            auto outputBoundary = [count, tailLanes](size_t lane) {
                return count / tailLanes * lane + (std::min)(lane, count % tailLanes);
            };
            pool.ParallelFor(0, tailLanes, [&](uint32_t lane) {
                ZoneScopedN("TaskSystem.Sort.MergePartition");
                const size_t lo = outputBoundary(lane), hi = outputBoundary(lane + 1);
                const size_t a = coRank(lo), b = coRank(hi);
                std::merge(values.begin() + a, values.begin() + b,
                    values.begin() + mid + lo - a, values.begin() + mid + hi - b,
                    scratch.begin() + lo, less);
            });
        }
        else if (pairs == 1) merge(0);
        else pool.ParallelFor(0, pairs, merge);
        values.swap(scratch);
    }
}
}
