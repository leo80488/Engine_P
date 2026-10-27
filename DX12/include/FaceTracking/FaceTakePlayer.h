#pragma once
// Plays a take's frames against a simulation clock. Advance(clock) emits, in order, every
// frame whose time is at or before the clock plus the one frame after it, so a reader
// sampling at the clock interpolates between two real frames instead of extrapolating past
// the newest. Looping shifts each pass by the take's period (its span plus its last frame
// interval), so emitted times and seqs keep rising.
// Only the clock decides what is emitted: under -fixeddt two runs emit the same frames.
// Header-only: tests/unit/face_core_test.cpp drives it without the engine library.

#include "FaceTracking/FaceFrame.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace FaceTracking {

class FaceTakePlayer
{
public:
    // `frames` is borrowed: it must outlive the player. Times are re-based so the first frame is 0.
    void Reset(const std::vector<FaceFrame>* frames, bool loop)
    {
        m_frames = (frames && !frames->empty()) ? frames : nullptr;
        m_loop = loop;
        m_next = 0;
        m_pass = 0;
        m_seq = 0;
        m_lastEmittedUs = std::numeric_limits<int64_t>::min();
        m_finished = m_frames == nullptr;
        m_t0 = m_frames ? m_frames->front().sourceTimeUs : 0;
        m_spanUs = m_frames ? m_frames->back().sourceTimeUs - m_t0 : 0;
        m_periodUs = 0;
        if (m_frames && m_frames->size() >= 2)
        {
            const int64_t lastGap = m_frames->back().sourceTimeUs - (*m_frames)[m_frames->size() - 2].sourceTimeUs;
            m_periodUs = m_spanUs + std::max<int64_t>(lastGap, 1);
        }
        if (m_periodUs <= 0) m_loop = false; // a one-frame take holds instead of looping
    }

    // emit(const FaceFrame&) -> bool; false (the ring is full) stops here, and the next
    // Advance resumes at the same frame.
    template <class Emit>
    size_t Advance(int64_t clockUs, Emit&& emit)
    {
        size_t n = 0;
        while (!m_finished && m_lastEmittedUs <= clockUs)
        {
            const FaceFrame& src = (*m_frames)[m_next];
            FaceFrame f = src;
            f.sourceTimeUs = (src.sourceTimeUs - m_t0) + static_cast<int64_t>(m_pass) * m_periodUs;
            f.seq = m_seq + 1;
            if (!emit(f)) break;
            m_seq = f.seq;
            m_lastEmittedUs = f.sourceTimeUs;
            ++n;
            if (++m_next == m_frames->size())
            {
                if (m_loop)
                {
                    m_next = 0;
                    ++m_pass;
                }
                else
                {
                    m_finished = true;
                }
            }
        }
        return n;
    }

    bool Finished() const { return m_finished; }
    bool Looping() const { return m_loop; }
    uint64_t Pass() const { return m_pass; }
    uint64_t Emitted() const { return m_seq; }
    int64_t SpanUs() const { return m_spanUs; }
    int64_t PeriodUs() const { return m_periodUs; }
    size_t FrameCount() const { return m_frames ? m_frames->size() : 0; }

private:
    const std::vector<FaceFrame>* m_frames = nullptr;
    bool m_loop = false;
    bool m_finished = true;
    size_t m_next = 0;
    uint64_t m_pass = 0;
    uint64_t m_seq = 0;
    int64_t m_lastEmittedUs = 0;
    int64_t m_t0 = 0;
    int64_t m_spanUs = 0;
    int64_t m_periodUs = 0;
};

} // namespace FaceTracking
