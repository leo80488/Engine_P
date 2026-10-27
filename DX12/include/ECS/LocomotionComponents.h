#pragma once

// Locomotion + layered-animation components.
//
// LocomotionComponent - input-driven blend space (1D by speed, or 2D by direction + speed). Gameplay
//   writes `speed` (and `dirDeg`) each frame; AnimationSystem smooths them, picks the bracketing
//   samples and samples their clips at a SHARED normalized gait phase, so walk and run cycles stay
//   foot-aligned and the blend never scissors. The result is the entity's BASE pose, cross-faded
//   against the Animator by locoWeight.
// LocomotionTransitionComponent - start / stop / pivot / turn / jump one-shots OVER the blend space.
// AnimLayerComponent - masked layers ON TOP of the base pose: each plays its own clip through a BONE
//   MASK derived from the skeleton's HumanoidProfile (UpperBody = the Spine role bone's subtree), e.g.
//   locomotion drives the legs while an UpperBody layer plays aim / attack / reload.
//
// Clip references are .ianim PATHS (stable, serialized); AnimationClipSystem::ResolvePendingBinds
// resolves the runtime indices asynchronously (same flow as PendingAnimBind). AnimationSystem::Update
// consumes all three; there is no separate system to schedule.

#include "ECS/ECS.h"

#include <cstdint>
#include <string>
#include <vector>

// ==== LocomotionComponent ====
enum class LocomotionMode : uint8_t
{
    Blend1D = 0,  // samples on a speed axis (idle/walk/run)
    Blend2D = 1,  // polar blend space: (direction angle, speed), 8-way strafe.
                  // Author idle as ONE sample at speed 0 (angle ignored), then
                  // direction spokes at each gait ring, e.g. 8 x walk @1.5 +
                  // 8 x run @4. Runtime picks the two speed rings bracketing the
                  // input speed and the two spokes bracketing the input angle in
                  // each ring -> bilinear blend of up to 4 phase-synced clips.
};

struct LocomotionSample
{
    std::string animPath;              // .ianim (first bone clip is used)
    float       speed    = 0.f;        // speed threshold / ring (units/s)
    float       angleDeg = 0.f;        // 2D only: direction spoke. 0 = forward,
                                       // +90 = right, +-180 = back, -90 = left.

    // Runtime (not serialized):
    uint32_t clip          = ~0u;      // resolved ClipLibrary index
    uint32_t pendingPacked = 0;        // AnimHandle::packed while loading (0 = none)
};

struct LocomotionComponent
{
    static constexpr uint32_t MAX_SAMPLES = 24;  // 8 dirs x 2 gaits + idle fits
    static constexpr uint32_t MAX_BLEND   = 4;   // polar bilinear worst case

    bool    enabled  = true;
    uint8_t mode     = static_cast<uint8_t>(LocomotionMode::Blend1D);
    float   speed    = 0.f;  // INPUT: desired locomotion speed (gameplay writes)
    float   dirDeg   = 0.f;  // INPUT (2D): movement direction, character-local
                             // (0 = forward, +90 = right)
    float   damping  = 6.f;  // parameter smoothing rate (1/s); 0 = snap
    float   playRate = 1.f;  // cycle playback rate multiplier

    LocomotionSample samples[MAX_SAMPLES];
    int              sampleCount = 0;

    // How much of the BASE pose this blend space owns, cross-faded against the Animator
    // (AnimationComponent primary/secondary): 1 = locomotion owns the body, 0 = the Animator's clip does,
    // in between they are lerped. An Animator one-shot (Animator.Play / Character.RequestAction) is
    // visible only while this is below 1: CharacterAnimDriver ramps it to 0 while an action runs and back
    // to 1 when it finishes; Lua (Locomotion.SetWeight, Character.AdoptCurrentPose) can also set it.
    // NOT serialized: a character with no AnimDriver keeps the default 1.
    float locoWeight = 1.f;

    // ---- Runtime state (written by AnimationSystem) ----
    float smoothedSpeed = 0.f;
    float smoothedDir   = 0.f;   // degrees, wrap-aware smoothing
    float phase         = 0.f;   // normalized gait phase [0,1)

    // Resolved blend for this frame (consumed by sampleEntity):
    uint32_t outClip[MAX_BLEND]   = { ~0u, ~0u, ~0u, ~0u };
    float    outTime[MAX_BLEND]   = {};
    uint8_t  outSample[MAX_BLEND] = {}; // authored sample identity, including repeated clip aliases
    float    outWeight[MAX_BLEND] = {};
    int      outCount             = 0;

    bool Active() const { return enabled && outCount > 0; }
};

// ==== LocomotionTransitionComponent ====
// One-shot clips layered OVER the blend space. The blend space is a pure parametric sampler that can
// hold a gait but not enter or leave one; this is the state machine's clip table for start / stop /
// pivot / turn-in-place.
//
// CharacterAnimDriver watches the motor (speed, movement direction, facing) and fires one;
// AnimationSystem samples it, cross-fades it OVER the blend-space pose by `weight`, then releases
// back. Direction-keyed like LocomotionSample::angleDeg (an 8-way set has 8 Starts, 8 Stops, ...);
// the runtime picks the spoke nearest the movement direction. Optional: with no component or an
// empty list, locomotion runs the blend space alone.
enum class LocoTransitionKind : uint8_t
{
    Start       = 0,  // idle -> moving, in direction angleDeg
    Stop        = 1,  // moving in direction angleDeg -> idle
    Pivot       = 2,  // sharp direction change, angleDeg = the NEW direction
    TurnInPlace = 3,  // stationary yaw change; angleDeg sign = turn direction

    // ---- Airborne state machine (direction-agnostic; author angle 0) ----
    // Driven by the KCC's grounded flag, NOT by the jump input: walking off a ledge plays the same fall,
    // and the driver picks JumpStart vs JumpLoop from the vertical velocity when the ground is lost.
    JumpStart   = 4,  // ground -> air with upward velocity (an actual jump)
    JumpLoop    = 5,  // held while airborne; set `looping` on this one
    JumpEnd     = 6,  // air -> ground (landing), however the fall started
};

struct LocoTransition
{
    std::string animPath;             // .ianim (first bone clip)
    uint8_t     kind     = static_cast<uint8_t>(LocoTransitionKind::Start);
    float       angleDeg = 0.f;       // same convention as LocomotionSample
    float       speed    = 1.f;       // playback rate for this one-shot
    // Play at most this many seconds of the clip (0 = to the end). Authored start/stop clips are often
    // 2-4 s long because they are distance-matched ("step off, then walk several steps"); played to the
    // end they hold the body long after the start or stop is over. Cap this to the useful HEAD of the clip.
    float       maxDuration = 0.f;
    // Loop instead of auto-releasing at the end. A looping transition is a STATE, not a one-shot: only
    // the driver takes it off (JumpLoop is held for as long as the character is airborne).
    bool        looping     = false;

    // Runtime (not serialized):
    uint32_t clip          = ~0u;
    uint32_t pendingPacked = 0;
};

struct LocomotionTransitionComponent
{
    static constexpr int MAX_TRANSITIONS = 32;   // 8 dirs x (start/stop/pivot) + turns

    bool enabled = true;

    // ---- Trigger tuning ----
    // Speed crossings that fire Start / Stop, with hysteresis: start must exceed startSpeed, stop must
    // fall below stopSpeed; stopSpeed < startSpeed keeps a character hovering at the threshold from
    // firing them repeatedly.
    float startSpeed = 0.25f;   // m/s
    float stopSpeed  = 0.10f;   // m/s
    // Movement-direction change (deg) while moving that fires a Pivot.
    float pivotAngle = 100.f;
    // Facing change rate (deg/s) while stationary that fires TurnInPlace.
    float turnRate   = 120.f;
    // Cross-fade in/out against the blend-space pose (seconds).
    float blendIn    = 0.15f;
    float blendOut   = 0.20f;
    // Minimum seconds between transitions; stops start/stop flapping.
    float cooldown   = 0.25f;
    // A transition is released this many seconds before its clip ends, so the
    // fade back to the blend space finishes exactly as the clip does.
    float releaseLead = 0.20f;

    LocoTransition transitions[MAX_TRANSITIONS];
    int            count = 0;

    // ---- Runtime state (driver writes active*, AnimationSystem advances) ----
    int      active     = -1;    // index into transitions, -1 = none
    float    time       = 0.f;   // playback position in the active clip
    float    weight     = 0.f;   // 0..1 blend OVER the locomotion pose
    bool     releasing  = false; // fading back out
    // Heartbeat for `looping` transitions: the driver must re-assert this EVERY frame it still wants the
    // loop held; AnimationSystem releases the loop on any frame it was not re-asserted, then clears it.
    // A latch would strand the loop whenever the driver stops running (a held death action returns
    // before the transition tick, the component is disabled mid-air, a MotionDirector takes the clock),
    // and nothing else releases a looping clip; a heartbeat makes "not driven" mean "release".
    bool     loopHeld   = false;
    float    cooldownTimer = 0.f;

    bool Active() const { return enabled && active >= 0 && weight > 0.001f; }
};

// ==== AnimLayerComponent ====
enum class BoneMaskMode : uint8_t
{
    FullBody   = 0,  // no mask (weight 1 everywhere)
    UpperBody  = 1,  // Spine role bone + all descendants (torso/arms/head)
    LowerBody  = 2,  // complement of UpperBody (hips/legs/root/etc.)
    CustomRoot = 3,  // customRootBone + all descendants
};

struct AnimLayer
{
    std::string animPath;              // .ianim (first bone clip)
    float       weight  = 1.f;         // master layer weight
    float       speed   = 1.f;
    bool        looping = true;
    bool        playing = true;
    uint8_t     maskMode = static_cast<uint8_t>(BoneMaskMode::UpperBody);
    std::string customRootBone;        // for BoneMaskMode::CustomRoot

    // Runtime (not serialized):
    uint32_t clip          = ~0u;
    uint32_t pendingPacked = 0;
    float    time          = 0.f;

    // Per-skeleton bone-weight cache (rebuilt when skeleton/mode/root changes).
    std::vector<float> maskW;
    uint32_t           maskSkel       = ~0u;
    uint8_t            maskModeCached = 0xFF;
    std::string        maskRootCached;
    // An unresolved mask is NOT cached (so fixing the humanoid profile or the bone name re-resolves it),
    // which would make the diagnostic fire every frame; this latches the log line instead. Cleared on a
    // successful build.
    bool               maskWarned     = false;
};

struct AnimLayerComponent
{
    static constexpr int MAX_LAYERS = 4;

    AnimLayer layers[MAX_LAYERS];
    int       layerCount = 0;
};
