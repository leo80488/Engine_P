#pragma once

// HeadTrackComponent: turns a skinned character's head bone toward a target (the active camera by
// default, or any entity targetRef names), clamped to yaw/pitch limits (a look-at bone constraint).
//
// Attach to the skeleton root entity (the SkeletonComponent carrier). HeadTrackSystem runs after
// the animation/IK pose is sampled and before the world matrices are baked, so the look-at
// composes on top of the playing clip instead of replacing it.
//
// When the target leaves the yaw/pitch cone (or disappears) the head does not stick at the limit:
// it eases back to the rest pose (returnSpeed), like a person who gives up craning.
//
// Authoring notes
//   * headBoneName selects the bone to rotate. PMX/MMD rigs usually use the Japanese kanji for
//     "head"; FBX/Assimp rigs commonly "Head".
//   * forwardAxis / upAxis name the head bone's local axes that point out of the face and up. PMX
//     head bones often run +Y along the bone, so the default +Z forward is often wrong for them:
//     check the read-only "Look Forward" vector in the inspector.
//   * targetRef is a save-safe Guid entity reference. Empty = look at the active camera; set it
//     (inspector picker / drag-drop) to look at a prop, another character or a look-at helper.

#include "ECS/ECS.h"
#include "ECS/GuidComponent.h"   // AttachmentRef (save-safe entity reference)

#include <DirectXMath.h>
#include <cstdint>
#include <string>

struct HeadTrackComponent
{
    // Master enable. When false (as with weight <= 0) the head eases back to rest through the
    // rest-return fade instead of freezing: to the animated pose under a clip, to the bind rest pose
    // without one. Re-enabling eases back into tracking.
    bool enabled = true;

    // Head bone name in the skeleton (resolved to a bone index by name scan).
    std::string headBoneName = "Head";

    // Who to look at. Empty (no Guid) = the active camera; otherwise the resolved entity's world
    // position. Save-safe: stores the target's Guid, not a volatile entity id (ECS/GuidComponent.h).
    AttachmentRef targetRef;

    // Which head-bone LOCAL axis points OUT of the face (the look/forward axis)
    // and which points UP. 0=+X 1=-X 2=+Y 3=-Y 4=+Z 5=-Z.
    int forwardAxis = 4;   // +Z
    int upAxis      = 2;   // +Y

    // Rotation limits in degrees, measured from the animated look direction.
    //   yaw   = left/right turn (about the head up axis)
    //   pitch = up/down turn   (about the head right axis)
    // Beyond the cone the swing is clamped to the limit and fades back to rest (returnSpeed).
    float yawLimitDeg   = 70.0f;
    float pitchLimitDeg = 45.0f;

    // Tracking weight: 0 = passthrough (animation only), 1 = fully face the target.
    // For gameplay fades (e.g. release tracking during a scripted turn).
    float weight = 1.0f;

    // Exponential smoothing rate (1/s) for easing the head toward the target. Higher = snappier;
    // 0 = no ease (only the turn-speed cap). Applies only while dt > 0; at dt == 0 (Edit, paused) the
    // head tracks instantly so orbiting the editor camera previews it.
    float smoothingSpeed = 8.0f;

    // Hard cap on how fast the head may swing (deg/s), the main "don't whip the head around" knob:
    // exponential smoothing alone lunges when the target is far, since its per-frame step is
    // proportional to the remaining error. The cap slews that down to a constant, plausible turn rate.
    // 0 = uncapped. Lower = more deliberate; ~90-180 reads as natural.
    float maxTurnSpeedDeg = 160.0f;

    // Optional angular-acceleration limit (deg/s^2). > 0 gives the head an ease-in: it takes a beat
    // to reach full turn speed, which reads as a short reaction delay. 0 = no limit (reach the speed
    // cap at once). Try ~600-900.
    float turnAccelDeg = 0.0f;

    // How fast the head returns to its rest pose when the target leaves the yaw/pitch cone or
    // disappears (exponential rate, 1/s); lower drifts home more lazily. Re-acquiring runs at 4x this
    // rate so tracking stays responsive. 0 = snap.
    float returnSpeed = 3.0f;

    // ---- Runtime (not authored, not serialized) ----
    uint32_t    cachedBoneIdx = ~0u;   // resolved head bone index
    std::string resolvedName;          // headBoneName that cachedBoneIdx came from

    // Smoothing state: the last applied parent-space swing quaternion (identity = no turn). Only the
    // additive swing is smoothed, never the animated base.
    DirectX::XMFLOAT4 smoothSwing = { 0.f, 0.f, 0.f, 1.f };
    bool              smoothValid = false;

    // Current swing angular speed (rad/s), carried between frames so the accel
    // limiter can ramp toward the speed cap. Reset whenever smoothing state does.
    float             turnSpeed   = 0.0f;

    // Rest-return fade: 1 = fully tracking, 0 = fully at rest pose. Eased toward
    // 0 while out of range / target-less, back to 1 on re-acquire. Starts at 0 so
    // a fresh head eases INTO tracking rather than snapping.
    float             rangeFade   = 0.0f;

    // Read-only diagnostic: the head's resulting look direction (parent space), written every frame
    // so authors can verify the forward axis.
    DirectX::XMFLOAT3 lookForward = { 0.f, 0.f, 1.f };
};
