#include "Sequencer/SequenceClock.h"

#include <algorithm>
#include <cmath>

namespace Seq
{

ClockStep AdvanceClock(int32_t frame, float subFrame, float dtSec, float fps, float rate,
                       int32_t durationFrames, bool looping, bool stepOneFrame)
{
    ClockStep out;
    out.frame    = frame;
    out.subFrame = subFrame;

    const int32_t dir = (rate < 0.f) ? -1 : 1;

    if (stepOneFrame)
    {
        out.frame   += dir;
        out.subFrame = 0.f;
    }
    else
    {
        if (fps <= 0.f) fps = 30.f;
        out.subFrame += std::min(dtSec, kMaxSequenceStep) * fps * rate;
        // floor, NOT trunc. trunc rounds toward zero, so at a negative rate the
        // playhead would stall forever between -1 and 0 sub-frames instead of
        // stepping back a frame.
        const float whole = std::floor(out.subFrame);
        out.frame    += static_cast<int32_t>(whole);
        out.subFrame -= whole;
    }

    const int32_t last = (durationFrames > 0) ? durationFrames : 0;
    if (last <= 0) { out.frame = 0; out.subFrame = 0.f; return out; }

    if (looping)
    {
        while (out.frame >= last) { out.frame -= last; out.wrapped = true; }
        while (out.frame <  0)    { out.frame += last; out.wrapped = true; }
    }
    else if (out.frame >= last)
    {
        out.frame    = last;
        out.subFrame = 0.f;
        out.ended    = (dir > 0);
    }
    else if (out.frame < 0)
    {
        out.frame    = 0;
        out.subFrame = 0.f;
        out.ended    = (dir < 0);
    }
    return out;
}

bool IsDiscontinuous(int32_t frame, int32_t prevFrame, int32_t dir,
                     int32_t maxForwardJump, bool seeked, bool wrapped)
{
    // A wrap re-arms the one-shot lane: the next pass over the range must fire
    // its events again, and the cross-time cursors downstream (which read
    // prev > curr as a loop) need the same re-arm a seek gets.
    if (seeked || wrapped) return true;
    if (prevFrame < 0)     return true;   // no previous observation

    const int32_t delta = frame - prevFrame;
    if (delta * ((dir < 0) ? -1 : 1) < 0) return true;   // moved against the play direction

    return std::abs(delta) > std::max(1, maxForwardJump);
}

} // namespace Seq
