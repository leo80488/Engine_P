#pragma once

// PlayerComponent - tag + tuning data for the player avatar. Pair with
// CharacterControllerComponent on the same entity. PlayerControllerSystem reads the
// device-independent ActionState (an active data-defined InputProfile can bridge Move / Jump /
// Sprint into it) and the basis camera's facing, and writes CharacterIntentComponent;
// CharacterMotorSystem turns that into the CCC drive fields, using this component's jump-assist,
// air-control and facing tuning. ControlRouter uses Character.Possess to choose which entity the
// player brain drives.

#include "ECS/ECS.h"
#include "ECS/GuidComponent.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>
#include <cstdint>

// How the avatar's yaw relates to its movement. FaceMovement is the classic third-person feel:
// the body turns into whatever direction you push, so the character always runs forward and a
// directional (8-way) blend space only ever sees angle 0. The other two decouple facing from
// movement, so the strafe / back-pedal spokes of a Blend2D LocomotionComponent get used:
//   FaceCamera - body yaw follows the basis camera (shooter / lock-on feel); WASD becomes
//                forward / back / strafe relative to the body.
//   Locked     - the body never auto-rotates (script / animation own the yaw); handy for a
//                keyboard-only 8-way locomotion test with no camera control.
enum class PlayerFacingMode : std::uint8_t
{
    FaceMovement = 0,
    FaceCamera   = 1,
    Locked       = 2,
};

struct PlayerComponent : ComponentBase
{
    // ---- Movement feel ----
    float walkSpeed = 4.5f;     // m/s
    float runSpeed  = 7.5f;     // m/s, while the Sprint action is held

    // Gamepad: stick deflection does not change speed. Any input past the deadzone moves at the
    // current gait's full speed, like a key; running is an explicit input (the Sprint action: L3 on a
    // pad, LShift on the keyboard). The stick still supplies a full 360-degree direction.
    // Multiplier on horizontal input while airborne: 1.0 = full control, 0.0 = no mid-air steering.
    // 0.4 is a common 3D-platformer default.
    float airControl = 0.4f;

    // ---- Facing ----
    // Angular speed (rad/s) at which the avatar turns to face its movement direction; 0 = never
    // (animation / script own the rotation). The default ~12 rad/s (~688 deg/s) settles a direction
    // flip within a frame or two without looking like a teleport. Below `facingMinSpeed` the rotation
    // is skipped so a stationary player keeps the last-faced direction.
    float turnRate       = 12.f;
    float facingMinSpeed = 0.1f;   // m/s
    // What the yaw tracks (see PlayerFacingMode). The modes other than FaceMovement free the
    // movement direction from the body direction, which a directional locomotion blend space needs.
    PlayerFacingMode facingMode = PlayerFacingMode::FaceMovement;

    // ---- Jump assist ----
    // Coyote time: a jump is still allowed within N seconds of leaving the ground. CharacterMotorSystem
    // honours it while the CCC's `timeInAir <= coyoteTime` (and the body is not rising), though
    // `isGrounded` is already false.
    float coyoteTime     = 0.10f;
    // Jump buffer: a jump pressed up to N seconds BEFORE landing is queued and fires on the first
    // grounded frame, so a press just before landing is not lost. The countdown is
    // CharacterIntentComponent::jumpBufferTimer.
    float jumpBufferTime = 0.10f;

    // ---- Camera basis ----
    // Entity whose GlobalTransform forward / right, projected to XZ, are the movement basis. Empty
    // ref = the main camera the App passes to PlayerControllerSystem::Update. Save-stable via GUID:
    // survives a scene reload.
    AttachmentRef cameraEntity;

    // ---- Runtime ----
    // Unused: the live jump-buffer countdown is CharacterIntentComponent::jumpBufferTimer, owned by
    // CharacterMotorSystem.
    float jumpBufferTimer = 0.f;
};
