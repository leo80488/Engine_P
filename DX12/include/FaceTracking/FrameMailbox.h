#pragma once
// A latest-wins hand-off of images between exactly one producer and one consumer: a triple
// buffer. The producer always has a back image to fill and Publish()es it without waiting; the
// consumer TakeNewest()s the most recent published image, if one arrived since it last took one.
// An image published while an older one is still unread replaces it (counted in Dropped): a
// camera's consumer wants the newest frame, never a queue of stale ones. Nothing is allocated
// after the images reach their size, and neither side ever blocks the other.
// Header-only (tests/unit/face_capture_test.cpp).

#include "FaceTracking/FaceImage.h"

#include <atomic>
#include <cstdint>

namespace FaceTracking {

class FrameMailbox
{
public:
    // ---- Producer side ----
    FaceImage& Back() { return m_images[m_back]; }
    void Publish()
    {
        const uint8_t old = m_middle.exchange(static_cast<uint8_t>(m_back | kFresh), std::memory_order_acq_rel);
        m_back = old & kIndex;
        if (old & kFresh) m_dropped.fetch_add(1, std::memory_order_relaxed);
        m_published.fetch_add(1, std::memory_order_relaxed);
    }

    // ---- Consumer side ----
    // The newest image published since the last call, or null; it stays valid (and the
    // consumer's to read) until the next call.
    const FaceImage* TakeNewest()
    {
        if (!(m_middle.load(std::memory_order_acquire) & kFresh)) return nullptr;
        const uint8_t old = m_middle.exchange(static_cast<uint8_t>(m_front), std::memory_order_acq_rel);
        m_front = old & kIndex;
        return &m_images[m_front];
    }
    bool HasNew() const { return (m_middle.load(std::memory_order_acquire) & kFresh) != 0; }

    uint64_t Published() const { return m_published.load(std::memory_order_relaxed); }
    uint64_t Dropped() const { return m_dropped.load(std::memory_order_relaxed); }

private:
    static constexpr uint8_t kIndex = 3, kFresh = 4;
    FaceImage m_images[3];
    std::atomic<uint8_t> m_middle{1}; // image 1, nothing fresh
    int m_back = 0;                   // the producer's
    int m_front = 2;                  // the consumer's
    std::atomic<uint64_t> m_published{0}, m_dropped{0};
};

} // namespace FaceTracking
