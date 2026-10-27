#include "ECS/CameraSystem.h"
#include "ECS/ECS.h"               // World: ResolveFollowing reads the target's GlobalTransform
#include "ECS/HierarchyComponents.h"
#include "ECS/CharacterControllerComponent.h" // follow target's own query body
#include "ECS/PhysicsComponents.h"            // RigidBodyComponent::bodyId
#include "Input/InputSystem.h"
#include "Physics/PhysicsSystem.h" // CastSphereClosest for camera-collision probe

#define WIN32_LEAN_AND_MEAN
#include <windows.h>     // VK_* constants only (Input handles state)

#include <algorithm>
#include <cmath>
#include <DirectXMath.h>

using namespace DirectX;

static constexpr float kPitchMax = XM_PIDIV2 - 0.01f;

// The follow target's OWN physics body, so the spring-arm probe cannot hit the
// character it orbits: a character's CharacterControllerComponent::queryBody, else
// a RigidBodyComponent body (vehicle, platform). kInvalidPhysicsBodyId (neither)
// makes CastSphereClosest filter nothing.
static std::uint32_t FollowTargetOwnBody(World& world, Entity e)
{
    if (const auto* cc = world.GetComponent<CharacterControllerComponent>(e))
        if (cc->bodyId != kInvalidPhysicsBodyId) return cc->bodyId;
    if (const auto* rb = world.GetComponent<RigidBodyComponent>(e))
        if (rb->bodyId != kInvalidPhysicsBodyId) return rb->bodyId;
    return kInvalidPhysicsBodyId;
}

// FPS forward convention (left-handed):
//   forward = (sin(yaw)*cos(pitch), -sin(pitch), cos(yaw)*cos(pitch))
// This is exactly +Z rotated by pitch around X then yaw around Y, i.e.
// XMQuaternionRotationRollPitchYaw(pitch, yaw, 0).
static XMVECTOR ForwardFromYawPitch(float yaw, float pitch)
{
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float cy = std::cos(yaw),   sy = std::sin(yaw);
    return XMVectorSet(sy * cp, -sp, cy * cp, 0.f);
}

// Single writer of ctrl.yaw / pitch; sign convention at the declaration.
void CameraSystem::ApplyLook(CameraControllerComponent& ctrl, LocalTransform& xform,
                             float yawDeltaRad, float pitchDeltaRad)
{
    ctrl.yaw   += yawDeltaRad;
    ctrl.pitch += pitchDeltaRad;
    ctrl.pitch  = std::clamp(ctrl.pitch, -kPitchMax, kPitchMax);
    ctrl.yaw    = std::fmod(ctrl.yaw, XM_2PI);

    // Mirror the controller's yaw/pitch onto the transform's rotation so
    // LocalTransform stays the single source of truth for the camera pose.
    XMStoreFloat4(&xform.rotation,
                  XMQuaternionRotationRollPitchYaw(ctrl.pitch, ctrl.yaw, 0.f));
}

// WASDQE free-fly. Reads the raw keyboard directly and deliberately NOT the
// action layer: routing editor navigation through rebindable actions gains
// nothing and would re-open the W/E/R gizmo-shortcut collision.
void CameraSystem::ApplyFly(CameraControllerComponent& ctrl, LocalTransform& xform,
                            float dt)
{
    // WASD strafe only in Free mode: in player camera modes PlayerControllerSystem owns
    // horizontal motion and ResolveFollowing overwrites the position anyway, so a
    // keyboard nudge would show for one frame and snap back.
    if (dt > 0.0f && ctrl.mode == CameraControllerComponent::Mode::Free)
    {
        const Input& in = Input::Get();
        float speed = ctrl.moveSpeed * dt;
        if (in.IsKeyDown(VK_LSHIFT)) speed *= 3.0f;

        const XMVECTOR forward = ForwardFromYawPitch(ctrl.yaw, ctrl.pitch);
        const XMVECTOR up      = XMVectorSet(0.f, 1.f, 0.f, 0.f);
        const XMVECTOR right   = XMVector3Normalize(XMVector3Cross(up, forward));

        XMVECTOR pos = XMLoadFloat3(&xform.translation);

        if (in.IsKeyDown('W')) pos = XMVectorAdd(pos, XMVectorScale(forward, speed));
        if (in.IsKeyDown('S')) pos = XMVectorSubtract(pos, XMVectorScale(forward, speed));
        if (in.IsKeyDown('D')) pos = XMVectorAdd(pos, XMVectorScale(right, speed));
        if (in.IsKeyDown('A')) pos = XMVectorSubtract(pos, XMVectorScale(right, speed));
        if (in.IsKeyDown('E')) pos = XMVectorAdd(pos, XMVectorScale(up, speed));
        if (in.IsKeyDown('Q')) pos = XMVectorSubtract(pos, XMVectorScale(up, speed));

        XMStoreFloat3(&xform.translation, pos);
    }
}

void CameraSystem::SyncControllerFromTransform(CameraControllerComponent& ctrl,
                                               const LocalTransform& xform)
{
    // Recover yaw/pitch from the transform's forward vector. Inverse of the
    // ForwardFromYawPitch / RollPitchYaw mapping above (assumes no roll).
    const XMVECTOR q   = XMLoadFloat4(&xform.rotation);
    const XMVECTOR fwd = XMVector3Rotate(XMVectorSet(0.f, 0.f, 1.f, 0.f), q);
    XMFLOAT3 f;
    XMStoreFloat3(&f, fwd);

    ctrl.pitch = -std::asin(std::clamp(f.y, -1.f, 1.f));
    ctrl.yaw   = std::atan2(f.x, f.z);
}

DirectX::XMMATRIX CameraSystem::BuildViewMatrix(const GlobalTransform& xform)
{
    const XMMATRIX world = XMLoadFloat4x4(&xform.matrix);

    // World-space camera basis: translation = row 3, forward = +Z row.
    const XMVECTOR pos     = world.r[3];
    const XMVECTOR forward = XMVector3Normalize(world.r[2]);
    const XMVECTOR up      = XMVectorSet(0.f, 1.f, 0.f, 0.f);

    return XMMatrixLookToLH(pos, forward, up);
}

LocalTransform CameraSystem::MakeTransform(const CameraControllerComponent& ctrl,
                                           const DirectX::XMFLOAT3& position)
{
    LocalTransform lt;
    lt.translation = position;
    XMStoreFloat4(&lt.rotation,
                  XMQuaternionRotationRollPitchYaw(ctrl.pitch, ctrl.yaw, 0.f));
    return lt;
}

void CameraSystem::ResolveFollowing(World& world,
                                    CameraControllerComponent& ctrl,
                                    LocalTransform& xform,
                                    float dt,
                                    DX12Physics::PhysicsSystem* physics)
{
    using Mode = CameraControllerComponent::Mode;
    if (ctrl.mode == Mode::Free) return;
    const Entity followE = ctrl.followTarget.Resolve(world);
    if (followE == NullEntity) return;

    const GlobalTransform* targetGT = world.GetComponent<GlobalTransform>(followE);
    if (!targetGT) return;

    // Only the target's translation is used: the camera's rotation comes from
    // ctrl.yaw / pitch, not the target (the player looks where the camera looks).
    XMVECTOR vScale, vQuat, vTrans;
    const XMMATRIX targetM = XMLoadFloat4x4(&targetGT->matrix);
    if (!XMMatrixDecompose(&vScale, &vQuat, &vTrans, targetM)) return;
    XMFLOAT3 targetPos; XMStoreFloat3(&targetPos, vTrans);

    const XMVECTOR yawAxis   = XMVectorSet(0.f, 1.f, 0.f, 0.f);
    const XMVECTOR camFwd    = XMVector3Normalize(
        XMVectorSet(std::sin(ctrl.yaw) * std::cos(ctrl.pitch),
                    -std::sin(ctrl.pitch),
                    std::cos(ctrl.yaw) * std::cos(ctrl.pitch),
                    0.f));
    const XMVECTOR camRight  = XMVector3Normalize(XMVector3Cross(yawAxis, camFwd));

    XMFLOAT3 worldPos;
    if (ctrl.mode == Mode::FirstPerson)
    {
        // Glue the camera to target + headOffset (world axes: the target's rotation is
        // deliberately ignored), no distance term. First person is rigid (a smoothed eye
        // induces lag / nausea); reset the smoothing state so a later switch to
        // ThirdPerson snaps instead of gliding from a stale focus.
        worldPos.x = targetPos.x + ctrl.headOffset.x;
        worldPos.y = targetPos.y + ctrl.headOffset.y;
        worldPos.z = targetPos.z + ctrl.headOffset.z;
        ctrl.followSmoothInit = false;
        ctrl.smoothedDistance = 0.f;
    }
    else // ThirdPerson
    {
        // Focus = target + shoulderOffset.y up + shoulderOffset.x along the camera right
        // (so the character is not dead-centred); the camera sits behind it along -forward
        // at the clamped, smoothed thirdPersonDistance.
        const XMVECTOR focusV =
            XMVectorAdd(vTrans, XMVectorSet(0.f, ctrl.shoulderOffset.y, 0.f, 0.f));
        const XMVECTOR sideV  = XMVectorScale(camRight, ctrl.shoulderOffset.x);
        const XMVECTOR desiredFocus = XMVectorAdd(focusV, sideV);

        // ---- Frame-rate-independent focus smoothing ----
        // Low-pass the orbit pivot so target jitter / 60 Hz interpolation aliasing does
        // not reach the camera as shake. Smoothing the FOCUS (not the camera position)
        // keeps yaw / pitch changes instant without rubber-banding the radius. dt <= 0
        // (paused / hit-stop / editor-stopped) and a large one-frame jump (respawn, warp)
        // snap.
        const bool firstResolve = !ctrl.followSmoothInit;
        XMVECTOR focusWithShoulder = desiredFocus;
        if (!firstResolve)
        {
            const XMVECTOR prevFocus = XMLoadFloat3(&ctrl.smoothedFocus);
            const float jump = XMVectorGetX(
                XMVector3Length(XMVectorSubtract(desiredFocus, prevFocus)));
            constexpr float kTeleportSnap = 25.0f;  // metres in one frame
            if (dt > 0.f && ctrl.followLag > 1e-4f && jump < kTeleportSnap)
            {
                const float a = 1.f - std::exp(-dt / ctrl.followLag);
                focusWithShoulder = XMVectorLerp(prevFocus, desiredFocus, a);
            }
        }
        XMStoreFloat3(&ctrl.smoothedFocus, focusWithShoulder);

        // Spring-arm collision probe: sweep a sphere from the focus point (chest /
        // shoulder) toward the desired pose; on a hit, clamp the orbit distance so the
        // camera stops short of the wall.
        // The sweep MUST exclude the followed character and MUST NOT run against the
        // Character / Ragdoll / Projectile layers: the focus point sits INSIDE the target's
        // own capsule whenever |shoulderOffset.x| - cameraProbeRadius < capsuleRadius
        // (normally: 0.35 - 0.20 < 0.30 for the stock player), and with
        // CharacterControllerComponent::queryBody an unfiltered sweep hits it at fraction
        // 0 every frame, slamming the camera onto the 0.05 m floor. The layer mask alone
        // is not enough (a ragdoll or body-carrying target can sit on a world layer), so
        // the target's own body id is excluded too.
        float targetDistance = ctrl.thirdPersonDistance;
        if (physics && ctrl.cameraCollisionEnabled && ctrl.thirdPersonDistance > 0.f)
        {
            XMFLOAT3 fromPt; XMStoreFloat3(&fromPt, focusWithShoulder);
            XMFLOAT3 dirF3;  XMStoreFloat3(&dirF3,  XMVectorNegate(camFwd));
            const auto hit = physics->CastSphereClosest(
                fromPt, dirF3,
                std::max(0.01f, ctrl.cameraProbeRadius),
                ctrl.thirdPersonDistance,
                FollowTargetOwnBody(world, followE),
                DX12Physics::LAYER_MASK_CAMERA_PROBE);
            if (hit.hit)
            {
                // Pull in to just-before-the-wall. The shrink keeps the camera
                // outside the wall by the probe radius (Jolt already inset by
                // the sphere radius via mFraction, so we don't double-inset).
                targetDistance = std::max(0.05f, hit.distance);
            }
        }

        // ---- Asymmetric distance damping (anti-flicker spring arm) ----
        // Pull IN instantly (never sit inside a wall), ease OUT slowly: otherwise an
        // intermittent probe hit (grazing a corner / ground / railing) toggles the clamp
        // frame to frame and punches the camera in and out along the view axis.
        float clampedDistance = targetDistance;
        if (firstResolve || ctrl.smoothedDistance <= 0.f)
        {
            clampedDistance = targetDistance;                 // snap on first frame
        }
        else if (targetDistance < ctrl.smoothedDistance)
        {
            clampedDistance = targetDistance;                 // closer wall: pull in now
        }
        else if (dt > 0.f && ctrl.distanceLag > 1e-4f)
        {
            const float a = 1.f - std::exp(-dt / ctrl.distanceLag);
            clampedDistance = ctrl.smoothedDistance +
                              (targetDistance - ctrl.smoothedDistance) * a;  // ease out
        }
        ctrl.smoothedDistance = clampedDistance;
        ctrl.followSmoothInit = true;

        const XMVECTOR backV  = XMVectorScale(camFwd, -clampedDistance);
        const XMVECTOR posV   = XMVectorAdd(focusWithShoulder, backV);
        XMStoreFloat3(&worldPos, posV);
    }

    xform.translation = worldPos;
    XMStoreFloat4(&xform.rotation,
                  XMQuaternionRotationRollPitchYaw(ctrl.pitch, ctrl.yaw, 0.f));
}
