#pragma once

// CameraSystem - FPS-style camera controller.
// The camera entity owns a CameraControllerComponent (yaw / pitch / tuning) and a
// LocalTransform (its pose). CameraSystem applies look deltas + WASD to the
// controller and writes the pose onto the LocalTransform; TransformSystem
// propagates it like any other entity.
// Follow modes (mode != Free): WASD is skipped (the player controller owns
// horizontal motion) and ResolveFollowing recomputes the position from the follow
// target each frame; call it AFTER TransformSystem::Propagate so the target's
// GlobalTransform is fresh.

#include "ECS/Components.h"
#include "ECS/HierarchyComponents.h"   // LocalTransform, GlobalTransform
#include <DirectXMath.h>

class World;
namespace DX12Physics { class PhysicsSystem; }

class CameraSystem
{
public:
    // Radians-native look integration, the single writer of ctrl.yaw / pitch.
    // Sign: forward.y = -sin(pitch), so a POSITIVE pitch looks DOWN and a NEGATIVE
    // delta looks UP. Callers pass an already-signed delta and own their invert-Y
    // policy, so a mouse (pixel displacement) and a stick (dt-scaled rate) can be
    // summed without knowing about each other.
    static void ApplyLook(CameraControllerComponent& ctrl, LocalTransform& xform,
                          float yawDeltaRad, float pitchDeltaRad);

    // WASDQE free-fly, separate from ApplyLook so a stick-driven look rotates without
    // arming translation: the editor's W/E/R gizmo shortcuts coexist with fly-WASD
    // only because fly requires RMB held. No-op unless ctrl.mode == Free.
    static void ApplyFly(CameraControllerComponent& ctrl, LocalTransform& xform,
                         float dt);

    // Reposition the camera to track its followTarget. Call AFTER physics +
    // TransformSystem::Propagate so the target's GlobalTransform has this frame's
    // movement. No-op when mode == Free or followTarget is null / dead. Writes
    // LocalTransform only: the caller refreshes the camera's GlobalTransform (a root
    // entity, so xform.ToMatrix() suffices).
    // @p physics is optional: when non-null AND ctrl.cameraCollisionEnabled,
    // ThirdPerson sweeps a sphere from the focus point toward the desired pose and
    // clamps the distance short of any hit, so the camera does not poke through walls.
    // Null gives a collision-free preview cam.
    // @p dt is the gameplay delta-time (seconds) for frame-rate-independent
    // ThirdPerson smoothing (ctrl.followLag / distanceLag); 0 snaps to the target this
    // frame (paused / hit-stop / editor-stopped) so the smoothing never glides while
    // the sim is frozen. ctrl is non-const: the smoothing state (smoothedFocus /
    // smoothedDistance / followSmoothInit) lives there.
    static void ResolveFollowing(World& world,
                                 CameraControllerComponent& ctrl,
                                 LocalTransform& xform,
                                 float dt = 0.0f,
                                 DX12Physics::PhysicsSystem* physics = nullptr);

    // Re-seed the controller's yaw / pitch from a LocalTransform rotation (no roll).
    // Call when the active camera changes, a world loads, or something else (script,
    // turntable, gizmo) moved the camera, so the next look update continues from the
    // current pose without a jump.
    static void SyncControllerFromTransform(CameraControllerComponent& ctrl,
                                            const LocalTransform& xform);

    // Build a left-handed view matrix from a world-space camera transform.
    static DirectX::XMMATRIX BuildViewMatrix(const GlobalTransform& xform);

    // Build a LocalTransform whose translation is @p position and whose
    // rotation matches the controller's yaw/pitch. Convenience for scene setup
    // so a freshly-spawned camera entity starts at a well-defined pose.
    static LocalTransform MakeTransform(const CameraControllerComponent& ctrl,
                                        const DirectX::XMFLOAT3& position);
};
