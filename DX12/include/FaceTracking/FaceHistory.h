#pragma once
// A source's recent frames. Service::Pump appends what the source's ring delivered, once per
// engine frame. Every reader keeps its own FaceCursor and reads from here, so two characters
// bound to one source both see every frame - two readers on the ring itself would split the
// frames between them.
// Sample() is the one interpolation every source shares: channels linearly, the head by
// slerp, eyes and position linearly; at most one frame interval past the newest frame,
// then the newest frame is held unchanged.
// Header-only: tests/unit/face_core_test.cpp drives it without the engine library.

#include "FaceTracking/FaceFrame.h"
#include "FaceTracking/FaceMath.h"

#include <algorithm>
#include <cstdint>
#include <deque>

namespace FaceTracking {

struct FaceCursor
{
    uint64_t lastSeq = 0; // the newest frame this reader consumed
    uint64_t missed = 0;  // frames it never saw: the window dropped them, or the source skipped seqs
};

enum class SampleState : uint8_t
{
    Empty,        // no frame yet
    Before,       // t precedes the oldest frame: that frame
    Interpolated, // between two frames, or exactly on one
    Extrapolated, // past the newest frame by at most one frame interval
    Holding,      // further past: the newest frame, unchanged
};

inline const char* SampleStateName(SampleState s)
{
    switch (s)
    {
    case SampleState::Empty: return "empty";
    case SampleState::Before: return "before";
    case SampleState::Interpolated: return "interp";
    case SampleState::Extrapolated: return "extrap";
    case SampleState::Holding: return "hold";
    }
    return "?";
}

// a and b blended at alpha (0 = a, 1 = b, above 1 extrapolates). A channel only one side has
// keeps that side's value; ARKit channels stay inside 0..1. The result carries b's seq and
// capture time; the caller sets its sourceTimeUs.
inline void BlendFrames(const FaceFrame& a, const FaceFrame& b, float alpha, FaceFrame& out)
{
    out = b;
    out.present[0] = a.present[0] | b.present[0];
    out.present[1] = a.present[1] | b.present[1];
    for (int ch = 0; ch < kMaxFaceChannels; ++ch)
    {
        const bool ha = a.Has(ch), hb = b.Has(ch);
        if (!ha && !hb) continue;
        float v = (ha && hb) ? FaceMath::Lerp(a.values[ch], b.values[ch], alpha) : (ha ? a.values[ch] : b.values[ch]);
        if (ch < kArkitChannelCount) v = std::clamp(v, 0.0f, 1.0f);
        out.values[ch] = v;
    }
    const uint32_t fa = a.flags, fb = b.flags;
    if ((fa & kFaceHasHead) && (fb & kFaceHasHead)) out.headRot = FaceMath::SlerpUnclamped(a.headRot, b.headRot, alpha);
    else if (fa & kFaceHasHead) out.headRot = a.headRot;
    if ((fa & kFaceHasHeadPos) && (fb & kFaceHasHeadPos))
    {
        out.headPos.x = FaceMath::Lerp(a.headPos.x, b.headPos.x, alpha);
        out.headPos.y = FaceMath::Lerp(a.headPos.y, b.headPos.y, alpha);
        out.headPos.z = FaceMath::Lerp(a.headPos.z, b.headPos.z, alpha);
    }
    else if (fa & kFaceHasHeadPos) out.headPos = a.headPos;
    if ((fa & kFaceHasEyes) && (fb & kFaceHasEyes))
    {
        for (int side = 0; side < 2; ++side)
            for (int k = 0; k < 2; ++k)
                out.eyeYawPitch[side][k] = FaceMath::Lerp(a.eyeYawPitch[side][k], b.eyeYawPitch[side][k], alpha);
    }
    else if (fa & kFaceHasEyes)
    {
        for (int side = 0; side < 2; ++side)
            for (int k = 0; k < 2; ++k) out.eyeYawPitch[side][k] = a.eyeYawPitch[side][k];
    }
    out.confidence = std::clamp(FaceMath::Lerp(a.confidence, b.confidence, alpha), 0.0f, 1.0f);
    out.faceFound = alpha < 0.5f ? a.faceFound : b.faceFound;
    out.flags = fb | (fa & (kFaceHasBlend | kFaceHasHead | kFaceHasHeadPos | kFaceHasEyes));
}

class FaceHistory
{
public:
    static constexpr int64_t kWindowUs = 2'000'000; // about the last two seconds
    static constexpr size_t kMaxFrames = 2048;      // a bound for sources faster than 1 kHz

    // Frames arrive with rising seq and non-falling time; one that breaks either is refused and
    // counted - a source bug, not something to interpolate across.
    bool Append(const FaceFrame& f)
    {
        if (f.seq == 0 || (!m_frames.empty() && (f.seq <= m_frames.back().seq ||
                                                 f.sourceTimeUs < m_frames.back().sourceTimeUs)))
        {
            ++m_refused;
            return false;
        }
        m_frames.push_back(f);
        return true;
    }

    // Drops what fell out of the window behind the newest frame; keeps at least two frames.
    void Trim()
    {
        if (m_frames.empty()) return;
        const int64_t keepFrom = m_frames.back().sourceTimeUs - kWindowUs;
        while (m_frames.size() > 2 && (m_frames.front().sourceTimeUs < keepFrom || m_frames.size() > kMaxFrames))
            m_frames.pop_front();
    }

    void Clear() { m_frames.clear(); }
    bool Empty() const { return m_frames.empty(); }
    size_t Size() const { return m_frames.size(); }
    const FaceFrame* Newest() const { return m_frames.empty() ? nullptr : &m_frames.back(); }
    const FaceFrame* Oldest() const { return m_frames.empty() ? nullptr : &m_frames.front(); }
    uint64_t Refused() const { return m_refused; }

    // A reader that starts with the frames still to come rather than the ones already here.
    FaceCursor CursorAtNewest() const
    {
        FaceCursor c;
        if (!m_frames.empty()) c.lastSeq = m_frames.back().seq;
        return c;
    }

    // fn(const FaceFrame&) for every frame after cursor.lastSeq, oldest first; advances the cursor.
    template <class Fn>
    size_t ReadNew(FaceCursor& c, Fn&& fn) const
    {
        auto it = std::upper_bound(m_frames.begin(), m_frames.end(), c.lastSeq,
                                   [](uint64_t seq, const FaceFrame& f) { return seq < f.seq; });
        size_t n = 0;
        for (; it != m_frames.end(); ++it, ++n)
        {
            if (it->seq > c.lastSeq + 1) c.missed += it->seq - c.lastSeq - 1;
            fn(*it);
            c.lastSeq = it->seq;
        }
        return n;
    }

    // The face at time t (the source's clock). Frames sharing a time: the last one wins.
    SampleState Sample(int64_t tUs, FaceFrame& out) const
    {
        if (m_frames.empty()) return SampleState::Empty;
        if (tUs < m_frames.front().sourceTimeUs)
        {
            out = m_frames.front();
            return SampleState::Before;
        }
        // `after` = the first frame later than t; the one before it is the last at or before t.
        const auto after = std::upper_bound(m_frames.begin(), m_frames.end(), tUs,
                                            [](int64_t t, const FaceFrame& f) { return t < f.sourceTimeUs; });
        const FaceFrame& a = *(after - 1);
        if (a.sourceTimeUs == tUs)
        {
            out = a;
            return SampleState::Interpolated;
        }
        if (after != m_frames.end())
        {
            const FaceFrame& b = *after;
            const float alpha = static_cast<float>(static_cast<double>(tUs - a.sourceTimeUs) /
                                                   static_cast<double>(b.sourceTimeUs - a.sourceTimeUs));
            BlendFrames(a, b, alpha, out);
            out.sourceTimeUs = tUs;
            return SampleState::Interpolated;
        }
        // Past the newest frame: extrapolate along the last interval, for one interval.
        const FaceFrame* prev = nullptr;
        for (auto it = after - 1; it != m_frames.begin();)
        {
            --it;
            if (it->sourceTimeUs < a.sourceTimeUs)
            {
                prev = &*it;
                break;
            }
        }
        if (prev)
        {
            const int64_t interval = a.sourceTimeUs - prev->sourceTimeUs;
            if (tUs - a.sourceTimeUs <= interval)
            {
                const float alpha = static_cast<float>(static_cast<double>(tUs - prev->sourceTimeUs) /
                                                       static_cast<double>(interval));
                BlendFrames(*prev, a, alpha, out);
                out.sourceTimeUs = tUs;
                return SampleState::Extrapolated;
            }
        }
        out = a;
        return SampleState::Holding;
    }

private:
    std::deque<FaceFrame> m_frames;
    uint64_t m_refused = 0;
};

} // namespace FaceTracking
