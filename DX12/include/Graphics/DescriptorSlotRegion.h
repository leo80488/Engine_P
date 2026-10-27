#pragma once

// DescriptorSlotRegion - the slot bookkeeping of one descriptor heap region (the CPU staging heap, or
// the GPU heap's static region): a bump cursor, a first-fit free list and one bit per slot that says
// the slot is handed out. The bit is what refuses a second Free of the same slots: without it they go
// onto the free list twice and the next two allocations share them. D3D12-free and header-only, so
// tests/unit/descriptor_slot_region_test.cpp drives it without a device.
//
// Not thread-safe: DescriptorHeapAllocator holds its mutex around every call.

#include <cstddef>
#include <cstdint>
#include <vector>

class DescriptorSlotRegion
{
public:
    struct Block
    {
        uint32_t start = 0;
        uint32_t count = 0;
    };

    static constexpr uint32_t kNone = ~0u;

    // Empties the region and gives it @p capacity slots (one bit each: 128 KB per million).
    void Reset(uint32_t capacity)
    {
        m_capacity = capacity;
        m_cursor   = 0;
        m_live     = 0;
        m_freeList.clear();
        m_handedOut.assign((static_cast<size_t>(capacity) + 63) / 64, 0ull);
    }

    // @p count contiguous slots: the first free-list block that holds them, else the cursor. kNone, with
    // nothing changed, when neither has room. @p outFresh (optional) says the cursor served it: the only
    // case that raises the high-water Cursor() reports.
    uint32_t Allocate(uint32_t count, bool* outFresh = nullptr)
    {
        if (outFresh) *outFresh = false;
        if (count == 0) return kNone;
        uint32_t start = kNone;
        for (size_t i = 0; i < m_freeList.size(); ++i)
        {
            Block& b = m_freeList[i];
            if (b.count < count) continue;
            start = b.start;
            if (b.count == count) { b = m_freeList.back(); m_freeList.pop_back(); }
            else                  { b.start += count; b.count -= count; }
            break;
        }
        if (start == kNone)
        {
            if (count > m_capacity - m_cursor) return kNone;
            start     = m_cursor;
            m_cursor += count;
            if (outFresh) *outFresh = true;
        }
        SetRange(start, count, true);
        m_live += count;
        return start;
    }

    // True when every slot of [start, start + count) is handed out, the condition Free needs.
    bool IsHandedOut(uint32_t start, uint32_t count) const
    {
        if (count == 0 || start >= m_capacity || count > m_capacity - start) return false;
        for (uint32_t s = start; s < start + count; ++s)
            if (!Test(s)) return false;
        return true;
    }

    // Hands [start, start + count) back. False, with nothing changed, unless every one of those slots
    // is handed out: a second Free of the same allocation, or a range this region never gave.
    bool Free(uint32_t start, uint32_t count)
    {
        if (!IsHandedOut(start, count)) return false;
        SetRange(start, count, false);
        m_freeList.push_back({ start, count });
        m_live -= count;
        return true;
    }

    uint32_t Capacity()   const { return m_capacity; }
    // The monotonic high-water: Free never lowers it.
    uint32_t Cursor()     const { return m_cursor; }
    // Slots handed out now.
    uint32_t Live()       const { return m_live; }
    size_t   FreeBlocks() const { return m_freeList.size(); }

    // Free-list blocks that hold @p slot: at most one while every Free was checked.
    size_t BlocksCovering(uint32_t slot) const
    {
        size_t n = 0;
        for (const Block& b : m_freeList)
            if (slot >= b.start && slot - b.start < b.count) ++n;
        return n;
    }

    // The maximal runs of handed-out slots, in slot order: the first @p maxRuns into @p out. Returns
    // the number of runs.
    size_t LiveRuns(std::vector<Block>& out, size_t maxRuns) const
    {
        out.clear();
        size_t   runs = 0;
        uint32_t s    = 0;
        while (s < m_cursor)
        {
            if (!Test(s)) { ++s; continue; }
            const uint32_t first = s;
            while (s < m_cursor && Test(s)) ++s;
            if (out.size() < maxRuns) out.push_back({ first, s - first });
            ++runs;
        }
        return runs;
    }

private:
    bool Test(uint32_t slot) const
    {
        return (m_handedOut[slot >> 6] >> (slot & 63u)) & 1ull;
    }

    void SetRange(uint32_t start, uint32_t count, bool on)
    {
        for (uint32_t s = start; s < start + count; ++s)
        {
            const uint64_t bit = 1ull << (s & 63u);
            if (on) m_handedOut[s >> 6] |= bit;
            else    m_handedOut[s >> 6] &= ~bit;
        }
    }

    uint32_t              m_capacity = 0;
    uint32_t              m_cursor   = 0;
    uint32_t              m_live     = 0;
    std::vector<Block>    m_freeList;
    std::vector<uint64_t> m_handedOut;   // one bit per slot
};
