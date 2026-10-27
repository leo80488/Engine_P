#pragma once

// Video helpers outside the per-tick VideoSystem: author code (Lua bindings, editor tools, test
// scenes) may call them at any time.

#include "ECS/VideoComponent.h"

class IGraphicsDevice;

namespace Video
{
    // Releases every GPU resource the engine allocated for @p vc (each dpb[] NV12 texture) through the
    // deferred-release queue, so in-flight GPU work is unaffected, and resets the runtime state.
    // Destroying the component does not do this: call it first.
    void ReleaseGPUResources(IGraphicsDevice& gfx, VideoComponent& vc);

    // Replaces @p vc's frames with one NV12 texture of SMPTE 75% colour bars (left to right: white,
    // yellow, cyan, green, magenta, red, blue, black), drops the frame source and sets state = Paused,
    // so VideoSystem leaves it alone and VideoPass shows it next frame: a check of the sampling and
    // YUV -> RGB chain without a bitstream. @p width and @p height must be even and > 0 (NV12 chroma is
    // half-res); returns false on bad dimensions or a failed texture creation.
    bool LoadTestPattern(IGraphicsDevice& gfx, VideoComponent& vc,
                         uint32_t width, uint32_t height);

    // ==== Playback controls ====
    // Thin mutators of state / playRate, which VideoSystem reads each tick (the AnimationComponent
    // speed / paused / looping convention); call them from gameplay code, inspectors or Lua.

    // Starts from PTS 0 when Stopped, resumes the current clock when Paused. It only sets the state:
    // VideoSystem ticks the frame source from its next update.
    inline void Play(VideoComponent& vc)
    {
        if (vc.state == VideoPlaybackState::Stopped)
        {
            vc.clockSeconds   = 0.0;
            vc.lastDecodedPts = -1.0;
        }
        vc.state = VideoPlaybackState::Playing;
    }

    // Freezes playback; the clock and dpb[] stay, so the renderer keeps showing the last frame.
    inline void Pause(VideoComponent& vc)
    {
        if (vc.state == VideoPlaybackState::Playing)
            vc.state = VideoPlaybackState::Paused;
    }

    // Playing <-> Paused; Stopped -> Play (from the start).
    inline void TogglePause(VideoComponent& vc)
    {
        switch (vc.state)
        {
        case VideoPlaybackState::Playing: vc.state = VideoPlaybackState::Paused; break;
        case VideoPlaybackState::Paused:  vc.state = VideoPlaybackState::Playing; break;
        case VideoPlaybackState::Stopped: Play(vc); break;
        }
    }

    // Halts playback and rewinds the clock to 0, so the next Play decodes from the start. dpb[] stays
    // allocated, but the renderer skips Stopped components, so nothing is drawn until Play.
    inline void Stop(VideoComponent& vc)
    {
        vc.state          = VideoPlaybackState::Stopped;
        vc.clockSeconds   = 0.0;
        vc.lastDecodedPts = -1.0;
    }

    // Stop + Play in one call.
    inline void Restart(VideoComponent& vc)
    {
        Stop(vc);
        Play(vc);
    }

    // Sets the playback rate multiplier (0 freezes; VideoSystem clamps negative values to 0), like
    // AnimationComponent::speed.
    inline void SetSpeed(VideoComponent& vc, float speedMul)
    {
        vc.playRate = speedMul;
    }

    // Seeks to @p timeSeconds: sets the clock and forwards to the frame source's Seek, which may have to
    // start from the nearest keyframe. Without a source (test pattern) only the clock moves.
    void Seek(VideoComponent& vc, double timeSeconds);
} // namespace Video
