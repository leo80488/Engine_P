#pragma once

// SequenceClock - the sequencer playhead, as pure arithmetic.
// Its own translation unit with ZERO dependencies on the ECS, the renderer or DirectX: these two
// functions carry the whole scrubbing contract, and here they are unit-testable without launching
// the editor.

#include <cstdint>

namespace Seq
{

// Same clamp as App::RunFixedPhysicsLoop's accumulator (kMaxAccum). The frame dt is raw, unclamped
// wall clock; without this clamp one hitch (shader compile, streaming stall, alt-tab, breakpoint)
// would advance the sequence by the full elapsed time but physics by at most 0.25 s, and a
// sequenced actor that must reach a trigger would desync permanently.
inline constexpr float kMaxSequenceStep = 0.25f;

struct ClockStep
{
    int32_t frame    = 0;
    float   subFrame = 0.f;
    bool    wrapped  = false;   // crossed a loop boundary during this step
    bool    ended    = false;   // reached the end of a non-looping range
};

// Advance the playhead by @p dtSec, then wrap or clamp into [0, durationFrames].
// @p stepOneFrame ignores dt and moves exactly one SEQUENCE frame in the play direction: the
// engine's Step button advances 1/60 s (PlayModeController::kFixedStep), a fraction of a frame in
// a 24 or 30 fps cinematic.
ClockStep AdvanceClock(int32_t frame, float subFrame, float dtSec, float fps, float rate,
                       int32_t durationFrames, bool looping, bool stepOneFrame);

// Must the one-shot lane be suppressed across this jump?
// @p dir is the sign of the play rate; it separates reverse PLAYBACK (ordinary motion) from a
// backward SCRUB (a seek). Testing only `frame < prevFrame` would make a negative rate fire
// nothing, forever.
bool IsDiscontinuous(int32_t frame, int32_t prevFrame, int32_t dir,
                     int32_t maxForwardJump, bool seeked, bool wrapped);

} // namespace Seq
