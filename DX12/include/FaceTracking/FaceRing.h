#pragma once
// A single-producer / single-consumer ring: a source's producer - its own thread, the Lua
// thread for a synthetic source, or Service::Pump itself for a take - pushes; Service::Pump,
// on the animation chain's thread, is the only reader. A full ring refuses the push and
// counts it; the producer decides what that means (a live source drops the frame, a take
// retries at the next Pump).
// Header-only: tests/unit/face_core_test.cpp drives it from two threads.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace FaceTracking {

template <class T, size_t kCapacity>
class SpscRing
{
public:
    SpscRing() : m_slots(std::make_unique<T[]>(kCapacity)) {}
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    static constexpr size_t Capacity() { return kCapacity; }

    // Producer side.
    bool TryPush(const T& v)
    {
        const size_t tail = m_tail.load(std::memory_order_relaxed);
        if (tail - m_head.load(std::memory_order_acquire) >= kCapacity)
        {
            m_refused.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        m_slots[tail & (kCapacity - 1)] = v;
        m_tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Consumer side.
    bool TryPop(T& out)
    {
        const size_t head = m_head.load(std::memory_order_relaxed);
        if (head == m_tail.load(std::memory_order_acquire)) return false;
        out = m_slots[head & (kCapacity - 1)];
        m_head.store(head + 1, std::memory_order_release);
        return true;
    }

    // Either side; exact only when the other side is idle.
    size_t SizeApprox() const
    {
        return m_tail.load(std::memory_order_acquire) - m_head.load(std::memory_order_acquire);
    }
    uint64_t Refused() const { return m_refused.load(std::memory_order_relaxed); }

private:
    static_assert(kCapacity >= 2 && (kCapacity & (kCapacity - 1)) == 0, "the capacity is a power of two");

    // No cache-line padding between the two indices: a face source pushes at tens of Hz,
    // not the rate where false sharing shows.
    std::unique_ptr<T[]> m_slots;
    std::atomic<size_t> m_head{0}; // next slot to read  (consumer-owned)
    std::atomic<size_t> m_tail{0}; // next slot to write (producer-owned)
    std::atomic<uint64_t> m_refused{0};
};

} // namespace FaceTracking
