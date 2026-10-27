#include "ECS/HeadTrackSystem.h"
#include "ECS/HeadTrackComponent.h"
#include "ECS/AnimationComponents.h"       // SkeletonComponent, kInvalidAnimHandle
#include "Anim/IK/Kinematics.h"                   // Anim::IK::BoneModel (model-space FK)
#include "ECS/HierarchyComponents.h"        // GlobalTransform
#include "ECS/CameraStackComponents.h"      // LiveCameraComponent, Camera::kMainChannel
#include "ECS/CameraStackSystem.h"          // Camera::FindChannelEntity

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace DirectX;

namespace
{
    // Local-space unit axis by enum: 0=+X 1=-X 2=+Y 3=-Y 4=+Z 5=-Z.
    inline XMVECTOR AxisVec(int a)
    {
        switch (a)
        {
            case 0: return XMVectorSet( 1.f,  0.f,  0.f, 0.f);
            case 1: return XMVectorSet(-1.f,  0.f,  0.f, 0.f);
            case 2: return XMVectorSet( 0.f,  1.f,  0.f, 0.f);
            case 3: return XMVectorSet( 0.f, -1.f,  0.f, 0.f);
            case 4: return XMVectorSet( 0.f,  0.f,  1.f, 0.f);
            case 5: return XMVectorSet( 0.f,  0.f, -1.f, 0.f);
            default:return XMVectorSet( 0.f,  0.f,  1.f, 0.f);
        }
    }

    inline float Clampf(float v, float lo, float hi)
    {
        return v < lo ? lo : (v > hi ? hi : v);
    }
}

void HeadTrackSystem::Update(World& world, float dt,
                             const std::unordered_set<Entity>* activeSet)
{
    if (!m_enabled) return;

    auto* pHead = world.GetPool<HeadTrackComponent>();
    if (!pHead) return;
    auto* pSkel = world.GetPool<SkeletonComponent>();
    if (!pSkel) return;

    // Resolve the active camera's world position once: the default look target of every head without
    // an explicit targetRef. LiveCameraComponent is one frame stale here (written later, in PreRender),
    // imperceptible for head tracking. Heads with an explicit target do not need it, so do not return
    // when it is absent: no camera just means no fallback target.
    XMVECTOR camWorld = XMVectorZero();
    bool haveCam = false;
    if (Entity ch = Camera::FindChannelEntity(world, Camera::kMainChannel); ch != NullEntity)
    {
        if (const auto* live = world.GetComponent<LiveCameraComponent>(ch))
        {
            camWorld = XMLoadFloat3(&live->position);
            haveCam = true;
        }
    }

    const auto& ents = pHead->Entities();
    auto&       data = pHead->Data();
    const size_t n   = data.size();

    for (size_t i = 0; i < n; ++i)
    {
        const Entity e = ents[i];
        if (activeSet && activeSet->find(e) == activeSet->end()) continue;

        HeadTrackComponent& ht = data[i];

        // A disabled / zero-weight head must ease back to its rest pose, not freeze at its last turn, so
        // do not skip it: forceRest drives the same rest-return fade below (to the animated pose while a
        // clip plays, to the bind rest pose without one), and re-enabling eases back into tracking.
        // Without a clip AnimationSystem does not restore the head's rest rotation, so skipping would
        // leave it stuck.
        const bool forceRest = (!ht.enabled || ht.weight <= 0.0f);

        const SkeletonComponent* skc = pSkel->Get(e);
        if (!skc || skc->assetIndex == kInvalidAnimHandle) continue;

        AnimationSystem::LocalPose* poses = m_animSys.GetMutableLocalPose(e);
        if (!poses) continue;

        const SkeletonAsset& skel = m_skeletons.Get(skc->assetIndex);

        // Resolve head bone index by name. Re-resolve only when the name changes
        // or the cache is stale (heads are few, so this is not a hot path).
        if (ht.cachedBoneIdx == ~0u ||
            ht.cachedBoneIdx >= skel.boneCount ||
            ht.resolvedName != ht.headBoneName)
        {
            ht.cachedBoneIdx = ~0u;
            for (uint32_t b = 0; b < skel.boneCount; ++b)
            {
                if (std::strcmp(skel.boneNames[b], ht.headBoneName.c_str()) == 0)
                {
                    ht.cachedBoneIdx = b;
                    break;
                }
            }
            ht.resolvedName = ht.headBoneName;
            if (ht.cachedBoneIdx == ~0u) { ht.smoothValid = false; ht.turnSpeed = 0.0f; ht.rangeFade = 0.0f; continue; }
        }
        const uint32_t b = ht.cachedBoneIdx;

        // Base local rotation the swing composes onto, and the frame the yaw/pitch limits are measured
        // from; read up front for both the tracking and the rest-return path.
        //
        // With a live pose source AnimationSystem re-samples poses[b] before this system every frame, so
        // poses[b].rot is the clean animated base. Without one the pose cache is seeded with the rest pose
        // once (AnimationSystem's rest fallback skips re-init), so poses[b].rot holds this system's own
        // output from last frame: reading it back would compound the swing, drift the limit cone with
        // the head and break the clamps. So un-animated skeletons take the base from the skeleton's rest
        // pose. The test is "is there a live pose source" (HasLivePoseSource), not "is primaryClip set":
        // a LocomotionComponent blend space or an AnimLayer also refreshes poses[b] every frame.
        // AnimationSystem owns this rule; ChainPhysicsSystem and IKSystem use the same predicate.
        const bool clipActive = m_animSys.HasLivePoseSource(world, e);
        XMVECTOR animRot;
        if (clipActive)
        {
            animRot = XMQuaternionNormalize(XMLoadFloat4(&poses[b].rot));
        }
        else
        {
            XMVECTOR restScl, restRot, restTr;
            animRot = XMMatrixDecompose(&restScl, &restRot, &restTr,
                                        XMLoadFloat4x4(&skel.restPoseLocal[b]))
                ? XMQuaternionNormalize(restRot)
                : XMQuaternionIdentity();
        }

        // ---- Resolve this head's look target (world space) ----
        // An explicit targetRef entity wins, else the active camera. If neither resolves (no camera,
        // target destroyed / not placed, or it points at this entity) there is nothing to look at: ease
        // the head back to rest instead of freezing.
        XMVECTOR targetWorld = camWorld;
        bool     haveTarget  = haveCam;
        if (forceRest)
        {
            haveTarget = false;                         // disabled / zero weight -> ease to rest
        }
        else if (const Entity tgt = ht.targetRef.Resolve(world); tgt != NullEntity && tgt != e)
        {
            if (const auto* tgtGT = world.GetComponent<GlobalTransform>(tgt))
            {
                const XMMATRIX tgtWorld = XMLoadFloat4x4(&tgtGT->matrix);
                targetWorld = tgtWorld.r[3];            // world-space translation
                haveTarget  = true;
            }
            else
            {
                haveTarget = false;                     // referenced but not placed yet
            }
        }

        // wantRest = no valid in-cone look this frame; drives the fade below.
        bool     wantRest  = !haveTarget;
        XMVECTOR baseSwing = XMQuaternionIdentity();

        if (haveTarget)
        {
            // ---- Gather transforms (model / skeleton-root space) ----
            const int32_t  p  = skel.parentIndex[b];
            const XMMATRIX Mb = Anim::IK::BoneModel(b, poses, skel);
            const XMMATRIX Mp = (p >= 0)
                ? Anim::IK::BoneModel(static_cast<uint32_t>(p), poses, skel)
                : XMMatrixIdentity();

            const XMVECTOR headPosModel = Mb.r[3];

            // Bring the world-space target into MODEL space (undo the character
            // root's world transform) so it's comparable with the skeleton matrices.
            const GlobalTransform* gt = world.GetComponent<GlobalTransform>(e);
            const XMMATRIX rootWorld = gt ? XMLoadFloat4x4(&gt->matrix) : XMMatrixIdentity();
            const XMMATRIX rootWorldInv = XMMatrixInverse(nullptr, rootWorld);
            const XMVECTOR targetModel = XMVector3TransformCoord(targetWorld, rootWorldInv);

            // Desired look direction into the head's PARENT-local frame.
            const XMVECTOR toTargetModel = XMVectorSubtract(targetModel, headPosModel);
            const XMMATRIX MpInv = XMMatrixInverse(nullptr, Mp);
            XMVECTOR desiredDirParent = (XMVectorGetX(XMVector3LengthSq(toTargetModel)) >= 1e-8f)
                ? XMVector3TransformNormal(XMVector3Normalize(toTargetModel), MpInv)
                : XMVectorZero();

            // Animated look basis in parent space, from the head's local rotation (scale-free, so bone
            // scaling does not affect it).
            XMVECTOR F0 = XMVector3Normalize(XMVector3Rotate(AxisVec(ht.forwardAxis), animRot));
            XMVECTOR U0 = XMVector3Normalize(XMVector3Rotate(AxisVec(ht.upAxis),      animRot));
            XMVECTOR R0 = XMVector3Cross(U0, F0);          // LH right = up x forward

            if (XMVectorGetX(XMVector3LengthSq(desiredDirParent)) < 1e-8f ||
                XMVectorGetX(XMVector3LengthSq(R0)) < 1e-8f)
            {
                // Target sits on the head, or forward parallel to up (bad axis pick): no look direction, so ease
                // back to rest.
                wantRest = true;
            }
            else
            {
                desiredDirParent = XMVector3Normalize(desiredDirParent);
                R0 = XMVector3Normalize(R0);
                U0 = XMVector3Cross(F0, R0);               // re-orthogonalize up

                // Decompose desired dir into yaw (about U0) + pitch (about R0).
                const float x = XMVectorGetX(XMVector3Dot(desiredDirParent, R0));
                const float y = XMVectorGetX(XMVector3Dot(desiredDirParent, U0));
                const float z = XMVectorGetX(XMVector3Dot(desiredDirParent, F0));

                const float rawYaw   = std::atan2(x, z);
                const float rawPitch = std::atan2(y, std::sqrt(std::max(0.0f, x * x + z * z)));

                const float yawLim   = XMConvertToRadians(ht.yawLimitDeg);
                const float pitchLim = XMConvertToRadians(ht.pitchLimitDeg);

                // Outside the cone: do not pin the head at the limit; fade it home.
                if (std::fabs(rawYaw) > yawLim || std::fabs(rawPitch) > pitchLim)
                    wantRest = true;

                const float yaw   = Clampf(rawYaw,   -yawLim,   yawLim);
                const float pitch = Clampf(rawPitch, -pitchLim, pitchLim);

                const float cy = std::cos(yaw),   sy = std::sin(yaw);
                const float cp = std::cos(pitch), sp = std::sin(pitch);
                XMVECTOR clampedDir = XMVectorAdd(
                    XMVectorAdd(XMVectorScale(F0, cp * cy), XMVectorScale(R0, cp * sy)),
                    XMVectorScale(U0, sp));
                clampedDir = XMVector3Normalize(clampedDir);

                // Shortest-arc swing (parent space): animated look -> clamped target.
                const float d = Clampf(XMVectorGetX(XMVector3Dot(F0, clampedDir)), -1.0f, 1.0f);
                if (d <= 0.999999f)
                {
                    XMVECTOR axis = XMVector3Cross(F0, clampedDir);
                    if (XMVectorGetX(XMVector3LengthSq(axis)) < 1e-12f)
                        axis = U0;                          // ~180 deg: any axis perpendicular to F0 works
                    axis = XMVector3Normalize(axis);
                    baseSwing = XMQuaternionRotationAxis(axis, std::acos(d));
                }
            }
        }

        // ---- Rest-return fade ----
        // rangeFade scales the whole swing: 1 = tracking, 0 = rest pose. Re-acquire quickly (stay
        // responsive), ease back to rest slowly when the target leaves the cone or disappears, so the
        // head glides home instead of sticking at the limit or snapping. At dt <= 0 (Edit) it resolves
        // instantly.
        const float fadeTarget = wantRest ? 0.0f : 1.0f;
        if (dt > 0.0f && ht.returnSpeed > 0.0f)
        {
            const float rate = (fadeTarget > ht.rangeFade) ? ht.returnSpeed * 4.0f  // acquire faster
                                                           : ht.returnSpeed;         // release slowly
            ht.rangeFade += (fadeTarget - ht.rangeFade) * (1.0f - std::exp(-rate * dt));
        }
        else
        {
            ht.rangeFade = fadeTarget;
        }
        ht.rangeFade = Clampf(ht.rangeFade, 0.0f, 1.0f);

        // Blend by tracking weight x rest-return fade (0 = animation passthrough), and give way to
        // whatever holds the head this frame (a tracked face: SetHeadAuthority). Only this product
        // changes - forceRest stays as it was (rangeFade would decay for as long as the face is tracked)
        // and ht.weight, an authored field, is never written.
        float handoff = 1.0f;
        if (m_headAuthority)
            handoff = 1.0f - Clampf(m_headAuthority(world, e), 0.0f, 1.0f);
        const float w = Clampf(ht.weight, 0.0f, 1.0f) * ht.rangeFade * handoff;
        XMVECTOR targetSwing = XMQuaternionSlerp(XMQuaternionIdentity(), baseSwing, w);

        // Ease the swing toward the target (never the animated base pose), then slew-rate limit it so the
        // head cannot whip around. dt <= 0 (Edit / paused) or a first frame with no prior state snaps
        // straight to the target.
        XMVECTOR appliedSwing = targetSwing;
        if (dt > 0.0f && ht.smoothValid)
        {
            const XMVECTOR prev = XMLoadFloat4(&ht.smoothSwing);

            // 1) Exponential ease toward the target (the soft "smooth" look). With
            //    smoothingSpeed == 0 this is a no-op and we ease purely via the cap.
            XMVECTOR eased = targetSwing;
            if (ht.smoothingSpeed > 0.0f)
            {
                const float alpha = 1.0f - std::exp(-ht.smoothingSpeed * dt);
                eased = XMQuaternionSlerp(prev, targetSwing, alpha);
            }

            // 2) Angular distance (rad) the head would move this frame, prev -> eased. Quaternion geodesic
            //    angle = 2*acos(|dot|); the swing stays well inside the yaw/pitch cone, far from the 180 deg
            //    axis singularity.
            float cosHalf = std::fabs(XMVectorGetX(XMQuaternionDot(prev, eased)));
            cosHalf = Clampf(cosHalf, 0.0f, 1.0f);
            const float stepAngle = 2.0f * std::acos(cosHalf);   // >= 0

            const float maxSpeed = XMConvertToRadians(std::max(0.0f, ht.maxTurnSpeedDeg)); // rad/s (0 = uncapped)
            if ((maxSpeed > 0.0f || ht.turnAccelDeg > 0.0f) && stepAngle > 1e-6f)
            {
                // Speed the ease wants this frame, capped to the max turn rate.
                float targetSpeed = stepAngle / dt;
                if (maxSpeed > 0.0f) targetSpeed = std::min(targetSpeed, maxSpeed);

                // Ramp the current speed toward that target (accel-limited ease-in).
                float speed = targetSpeed;
                if (ht.turnAccelDeg > 0.0f)
                {
                    const float dv = XMConvertToRadians(ht.turnAccelDeg) * dt;
                    speed = Clampf(targetSpeed, ht.turnSpeed - dv, ht.turnSpeed + dv);
                }

                const float allowed = speed * dt;                // rad we may move
                if (allowed < stepAngle)
                {
                    appliedSwing = XMQuaternionSlerp(prev, eased, allowed / stepAngle);
                    ht.turnSpeed = speed;                        // moved the full allowance
                }
                else
                {
                    appliedSwing = eased;                        // ease didn't need the cap
                    ht.turnSpeed = stepAngle / dt;               // record the actual speed used
                }
            }
            else
            {
                appliedSwing = eased;                            // no limits (or no motion)
                ht.turnSpeed = stepAngle / dt;
            }
        }
        else
        {
            ht.turnSpeed = 0.0f;                                 // snapped this frame
        }
        appliedSwing = XMQuaternionNormalize(appliedSwing);
        XMStoreFloat4(&ht.smoothSwing, appliedSwing);
        ht.smoothValid = true;

        // Compose the swing on top of the animated local rotation (anim first, then the parent-space
        // swing), the same convention as ComposeIKAnim.
        const XMVECTOR finalLocal =
            XMQuaternionNormalize(XMQuaternionMultiply(animRot, appliedSwing));
        XMStoreFloat4(&poses[b].rot, finalLocal);

        // Diagnostic: resulting look direction in parent space.
        XMStoreFloat3(&ht.lookForward,
            XMVector3Normalize(XMVector3Rotate(AxisVec(ht.forwardAxis), finalLocal)));
    }
}
