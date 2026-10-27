#pragma once
// An append-only array whose elements never move and whose existing entries can be read
// without a lock while one writer (holding the owner's mutex) appends.
// Not std::deque: its push_back keeps the elements in place but may reallocate the block
// map operator[] indexes through, so a racing reader reads a freed map (GraphicsDX12's
// m_psoPool: recording workers read PSOs while other workers create them lazily).
// Here the chunk-pointer table is a fixed array inside the pool, a chunk is allocated when
// an index first reaches it and freed only by clear(), and the size is published with
// release ordering after the element is in place: for any i < size() (acquire), element i
// and its chunk are safe to read from any thread.

#include <array>
#include <atomic>
#include <cstddef>
#include <utility>

namespace Sys
{
template <class T, std::size_t ChunkBits = 8, std::size_t MaxChunks = 1024>
class StableAppendPool
{
public:
    static constexpr std::size_t kChunkSize = std::size_t(1) << ChunkBits;
    static constexpr std::size_t kCapacity  = kChunkSize * MaxChunks;

    StableAppendPool()
    {
        for (auto& c : m_chunks) c.store(nullptr, std::memory_order_relaxed);
    }
    ~StableAppendPool() { clear(); }
    StableAppendPool(const StableAppendPool&)            = delete;
    StableAppendPool& operator=(const StableAppendPool&) = delete;

    // Any thread.
    std::size_t size() const noexcept { return m_size.load(std::memory_order_acquire); }
    bool        empty() const noexcept { return size() == 0; }

    // Any thread, for i < size().
    T& operator[](std::size_t i) noexcept
    {
        return m_chunks[i >> ChunkBits].load(std::memory_order_acquire)[i & (kChunkSize - 1)];
    }
    const T& operator[](std::size_t i) const noexcept
    {
        return m_chunks[i >> ChunkBits].load(std::memory_order_acquire)[i & (kChunkSize - 1)];
    }

    // ONE writer at a time (the owner's mutex). False when full: nothing moved.
    bool push_back(T&& value)
    {
        const std::size_t i = m_size.load(std::memory_order_relaxed);
        if (i >= kCapacity) return false;
        std::atomic<T*>& slot = m_chunks[i >> ChunkBits];
        T* chunk = slot.load(std::memory_order_relaxed);
        if (!chunk)
        {
            chunk = new T[kChunkSize]();
            slot.store(chunk, std::memory_order_release);
        }
        chunk[i & (kChunkSize - 1)] = std::move(value);
        m_size.store(i + 1, std::memory_order_release);
        return true;
    }

    // With no reader and no writer left (shutdown).
    void clear()
    {
        m_size.store(0, std::memory_order_relaxed);
        for (auto& c : m_chunks)
            delete[] c.exchange(nullptr, std::memory_order_relaxed);
    }

private:
    std::array<std::atomic<T*>, MaxChunks> m_chunks;
    std::atomic<std::size_t>               m_size{ 0 };
};
} // namespace Sys
