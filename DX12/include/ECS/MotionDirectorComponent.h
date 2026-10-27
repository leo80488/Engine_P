#pragma once

// MotionDirectorComponent - a "Performance": ONE master clock that drives several motion tracks in
// lockstep, so a camera-motion file and a SEPARATE skinned-motion file (different .ianim assets, on
// different entities) play synchronized under one transport (play / pause / scrub / loop).
//
// The director is a thin CONDUCTOR, and the binding that says "these play together". Each frame
// MotionDirectorTickSystem (CameraStackSystem.cpp) advances masterTime and writes each track's local
// time into the target's EXISTING component (AnimationComponent.primaryTime for a skinned track,
// CameraMotionComponent.time for a camera track), setting that component's `externalTime` flag so
// AnimationSystem / CameraMotionTickSystem sample at the supplied time but do NOT self-advance.

#include "ECS/GuidComponent.h"   // AttachmentRef
#include <vector>
#include <cstdint>

struct MotionDirectorTrack
{
    // Skinned -> AnimationComponent.primaryTime, Camera -> CameraMotionComponent.time (both continuously
    // time-driven). Audio -> AudioSourceComponent: audio cannot scrub, so an audio track is a one-shot
    // CUE that fires the source's Play at `timeOffset` and follows the director's pause/stop.
    enum Type : uint8_t { Skinned = 0, Camera = 1, Audio = 2 };

    AttachmentRef target;            // entity with AnimationComponent / CameraMotionComponent / AudioSourceComponent
    uint8_t       type       = Skinned;
    float         timeOffset = 0.f;  // master time at which this track starts (seconds)
    float         speedMul   = 1.f;  // per-track playback-rate scale (ignored for Audio)

    // ---- Runtime (NOT serialized) ----
    bool          fired      = false; // Audio: cue already triggered this cycle
    // Skinned: entity actually carrying the AnimationComponent. The authored
    // target is often a prefab/scene ROOT whose anim lives on a descendant;
    // MotionDirectorTickSystem resolves via subtree search and caches here.
    mutable Entity animEntity = NullEntity;
};

struct MotionDirectorComponent
{
    // ---- Authoring (serialized) ----
    float masterSpeed = 1.0f;
    float duration    = 0.0f;     // loop/clamp length (sec); 0 = run until tracks end
    bool  looping     = false;
    bool  playOnLoad  = true;
    std::vector<MotionDirectorTrack> tracks;

    // ---- Runtime (NOT serialized) ----
    // When true a Sequencer Director track owns masterTime: MotionDirectorTickSystem still fans the
    // clock out to every track but does NOT advance / loop / clamp it (the contract
    // AnimationComponent::externalTime has with this director, one level up). Without it the
    // sequencer's absolute write and the director's own dt integration would both move the clock and
    // the performance would run at 2x while the playhead says otherwise.
    bool  externalTime  = false;
    float masterTime    = 0.0f;
    bool  playing       = false;
    bool  started       = false;   // playOnLoad one-shot guard
    bool  paused        = false;
    bool  stopRequested = false;   // inspector/Lua Stop -> release all tracks
    bool  tracksDiagnosed = false; // once-per-play unresolvable-track warning latch

    // Auto-resolved total span (seconds) used for countdown / UI when the
    // authored `duration` is 0. MotionDirectorTickSystem walks the non-audio
    // tracks once their clips are loaded and stores max(timeOffset + clipDur /
    // speedMul) here; 0 means "not yet resolved" (still streaming, or the
    // director has only audio tracks). Never serialized.
    float resolvedDuration = 0.0f;
};
