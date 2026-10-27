#pragma once

// CharacterControllerComponent: the kinematic character controller (KCC), the
// execution-layer Motor of the movement stack:
//   brain (player input / AI / NavAgent) -> CharacterIntentComponent
//   -> CharacterMotorSystem writes the drive inputs below
//   -> PhysicsSystem steps JPH::CharacterVirtual (sweep / slide / ground snap)
//   -> LocalTransform.
// Human and AI drivers take the same path. Independent of RigidBodyComponent /
// ColliderComponent: with both present PhysicsSystem skips the rigid body and the
// CharacterVirtual is the only collider.
// Gravity is gameplay gravity, not Jolt's world gravity: PhysicsSystem integrates
// the vertical velocity itself and calls ExtendedUpdate with zero gravity. After
// each fixed step it writes back velocity, isGrounded, groundNormal, mode,
// wasBlockedLastStep, etc. for the next frame.

#include "ECS/ECS.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>
#include <cstdint>

inline constexpr std::uint32_t kInvalidCharacterId = 0xffffffffu;

// Movement mode. PhysicsSystem sets Walking / Falling from the grounded state;
// LaunchedTraversal is the off-mesh-link handshake (see launchFinished).
// Swimming and Climbing are reserved: nothing sets them.
enum class MovementMode : std::uint8_t
{
    Walking,            // standing on a walkable slope
    Falling,            // airborne (jump apex / coyote / off ledge)
    Swimming,           // inside a water volume (reserved)
    Climbing,           // on a ladder / climb surface (reserved)
    LaunchedTraversal,  // following an off-mesh-link arc; Motor ignores desiredHorizontalVelocity
};

struct CharacterControllerComponent : ComponentBase
{
    // ---- Capsule shape ----
    // Default: cylinder 2 x 0.70 = 1.4 m + two 0.30 m hemispheres = 2.0 m tall.
    // Keep it a single capsule: several movement shapes break collide-and-slide;
    // per-bone collision belongs to a separate hurt-box layer.
    float capsuleRadius     = 0.30f;
    float capsuleHalfHeight = 0.70f;  // half the cylinder segment, EXCLUDES the hemispheres

    // ---- Movement tuning ----
    // Maximum step-up (m): a lower ledge is walkable, a higher one is a wall. Passed to Jolt's stair walk
    // per character. The tallest ledge a capsule climbs is a little more than this: the capsule is lifted by
    // stepHeight and meets the edge on its padded bottom hemisphere, so the limit is
    // stepHeight + radius - (radius + skinWidth) * cos(maxSlope) (0.3737 m for the defaults).
    float stepHeight        = 0.30f;
    // Slopes shallower than this are walkable (gravity cancels, motion follows the
    // slope); on steeper ones gravity accumulates and the character slides down.
    // Default 45 deg.
    float maxSlopeRad       = 0.7854f;
    // Small gap between the capsule and the world that keeps the narrow phase
    // numerically stable. Do not set 0: sliding contacts jitter visibly.
    float skinWidth         = 0.02f;
    // Mass (kg): the weight the character puts on a dynamic body it stands on (mass x the physics gravity
    // as a downward impulse each step, so a raft sinks under it).
    float mass              = 80.f;
    // Strength multiplier on the force the character pushes dynamic objects with (0 = no push, 1 = Jolt's
    // 100 N limit). It scales CharacterVirtual's maximum push force, applied each step.
    float pushStrength      = 1.0f;

    // ---- Gameplay gravity (NOT Jolt PhysicsSystem gravity) ----
    // m/s^2, negative pulls down. Larger magnitude = snappier; -20 to -30 is the
    // usual 3D-platformer range, real-world -9.8 feels floaty.
    float gravity           = -25.0f;
    // Terminal fall speed (m/s): PhysicsSystem clamps the downward vertical velocity
    // to it. Prevents tunnelling through thin floors after long falls; also bounds the
    // worst-case ground-probe overshoot.
    float maxFallSpeed      = 55.f;

    // ---- Horizontal acceleration (momentum) ----
    // Rate (m/s^2) at which CharacterMotorSystem moves the ACTUAL velocity toward the
    // requested one instead of assigning it.
    // 0 = INSTANT (the default): desiredHorizontalVelocity is assigned from the input,
    // so there is no inertia - releasing a key stops within one step, and leaving the
    // ground replaces the speed with input x airControl.
    // Nonzero: on the ground the character ramps up and coasts down; in the air
    // airAccel is STEERING authority over the speed it jumped with, and when
    // airAccel > 0 it REPLACES PlayerComponent::airControl (that only scales the
    // target, so the two would fight).
    // Opt-in so existing characters, AI included, keep their feel.
    float groundAccel = 0.f;   // input -> full speed
    float groundDecel = 0.f;   // input released -> stop
    float airAccel    = 0.f;   // steering authority while airborne

    // ---- Drive inputs (written each frame) ----
    // World-space horizontal velocity (m/s); Y is ignored, the KCC owns vertical
    // motion (gravity + jump). CharacterMotorSystem writes it from the intent, root
    // motion or a motor lock.
    DirectX::XMFLOAT3 desiredHorizontalVelocity = { 0.f, 0.f, 0.f };
    // Desired world-space facing (XZ; Y ignored), enabled by useDesiredFacing.
    // Nothing reads either: CharacterMotorSystem applies facing to
    // LocalTransform.rotation directly from CharacterIntentComponent.
    DirectX::XMFLOAT3 desiredFacing     = { 0.f, 0.f, 1.f };
    bool              useDesiredFacing  = false;
    // Set to request a jump. PhysicsSystem consumes it (resets it to false) on the
    // next fixed step: applied as jumpSpeed when the CharacterVirtual starts the step
    // on ground, or within jumpGraceTime of leaving it (not rising); dropped otherwise.
    // Drive jumps through CharacterIntentComponent::jump (jump buffer + coyote time)
    // rather than setting this.
    bool              jumpRequested = false;
    // Seconds after leaving the ground in which a jump request still jumps (coyote time).
    // CharacterMotorSystem writes PlayerComponent::coyoteTime here each frame; 0 (the
    // default) refuses a jump in the air, as Character.Jump documents.
    float             jumpGraceTime = 0.f;
    // Crouch held, copied from CharacterIntentComponent::crouch by
    // CharacterMotorSystem.
    // TODO(character_movement_architecture 2.1): nothing reads it - shrink the capsule
    // and lower the walk speed while it is held.
    bool              wantsCrouch   = false;
    // Vertical velocity (m/s) applied on jump, used directly: the KCC does not solve
    // for jump height, so pick the speed for the apex the level needs.
    float             jumpSpeed     = 7.5f;

    // ---- Broad-phase proxy (spatial queries) ----
    // Give this character a body the physics queries can find. A CharacterVirtual
    // without an inner body has no broad-phase presence, so it is invisible to
    // OverlapSphere, CastRayClosest and every other query, which silently blinds AI
    // perception (its targets ARE characters). When set, Jolt creates a kinematic
    // capsule (CharacterVirtualSettings::mInnerBodyShape) that tracks the character,
    // carries the entity id as user data and is excluded from the character's own
    // collide-and-slide.
    // Opt-in: that body on the CHARACTER layer collides with dynamic props and raises
    // ContactBegan/Ended events existing content does not expect. Changing it at
    // runtime rebuilds the CharacterVirtual (it is part of the shape snapshot).
    bool              queryBody = false;

    // ---- Escape hatch (cutscene / teleport) ----
    // When set, the next KCC step zeroes velocity and desiredHorizontalVelocity, then
    // clears the flag. Unlike writing desiredHorizontalVelocity = 0, this also flushes
    // the carried velocity, so nothing glides on.
    bool              instantStopRequested = false;

    // ---- Off-mesh link handshake ----
    // In mode LaunchedTraversal PhysicsSystem flies the character as a projectile on
    // its CharacterVirtual velocity (desiredHorizontalVelocity is ignored); on landing
    // it sets mode = Walking and launchFinished = true. NavAgentSystem pauses path
    // following while the mode is LaunchedTraversal and clears this flag to advance
    // the path.
    // TODO(character_movement_architecture 7 trap 6): nothing enters LaunchedTraversal
    // (the navmesh has no off-mesh links). NavAgentSystem must set it and pre-load the
    // ballistic launch vector, and read launchFinished outside its LaunchedTraversal
    // branch: landing has already switched the mode to Walking.
    bool              launchFinished = false;

    // ---- State (PhysicsSystem writes each fixed step) ----
    // The velocity the character really had over the last step (XYZ, m/s, world space): its
    // displacement over the step, so a character pressed against a wall reads 0 whatever it was
    // commanded. A free move reads the commanded value exactly. Vertically, while grounded it is the
    // ground's: a stair step or a slope is not a vertical speed. It includes the ground's own motion
    // (groundVelocity). Do not write: PhysicsSystem overwrites it every step.
    DirectX::XMFLOAT3 velocity      = { 0.f, 0.f, 0.f };
    // World velocity of the ground the character stands on (a moving platform), zero in the air. The
    // character inherits it into its own step; subtract it from velocity for the speed relative to the ground.
    DirectX::XMFLOAT3 groundVelocity = { 0.f, 0.f, 0.f };
    // True while the KCC stands on a walkable slope; false when airborne or on a
    // too-steep slope.
    bool              isGrounded    = false;
    bool              wasGrounded   = false;  // previous step's isGrounded: landing / takeoff edges
    // World-space normal of the surface the character stands on; +Y when airborne
    // so readers need no null check.
    DirectX::XMFLOAT3 groundNormal  = { 0.f, 1.f, 0.f };
    // Entity ID of the body the character is standing on, or NullEntity
    // when airborne / not attached. Useful for moving-platform parenting,
    // dynamic-prop interactions, surface-material lookup.
    Entity            groundEntity  = NullEntity;
    // Seconds off the ground. Read by CharacterMotorSystem's coyote-time check and by
    // the animation driver for air-duration cues.
    float             timeInAir     = 0.f;

    // Current movement mode (see MovementMode). NavAgentSystem reads it for the
    // off-mesh-link handshake.
    MovementMode      mode          = MovementMode::Falling;

    // ---- Motor reports for NavAgentSystem stuck detection / replan ----
    // True when the last sweep moved less than 5% of the desired horizontal
    // displacement (wall hit, geometry pin).
    bool              wasBlockedLastStep = false;
    // Coarse normal of the blocking surface: the negated desired direction, since
    // Jolt does not expose the wall-contact normal. Valid only when
    // wasBlockedLastStep is true.
    DirectX::XMFLOAT3 blockedNormal      = { 0.f, 0.f, 0.f };

    // ---- Runtime (PhysicsSystem owns the lifecycle) ----
    // Packed JPH::BodyID of the queryBody inner body, for queries that want to find
    // or ignore the character; kInvalidPhysicsBodyId when there is none. Jolt owns
    // that body.
    std::uint32_t bodyId       = 0xffffffffu;
    // Bump via MarkDirty() after changing the capsule to rebuild the
    // CharacterVirtual on the next step (PhysicsSystem also rebuilds when the radius,
    // half height or queryBody differ from the built shape).
    std::uint32_t generation   = 0;
    // Generation the active CharacterVirtual was built from. Internal: do not write.
    std::uint32_t lastBuiltGeneration = 0;

    void MarkDirty() { ++generation; }
};
