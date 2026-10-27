#include "Resource/RetargetBaker.h"
#include "Resource/RetargetMath.h"   // ReconstructOrientedRestRef
#include "Resource/SkeletonAsset.h"
#include "Resource/AnimationResource.h"  // AnimClipData (ToNamedClipData)

#include <DirectXMath.h>
#include <vector>
#include <cmath>
#include <algorithm>

using namespace DirectX;

// Full-pose bind-relative retarget.
//
// The naive per-bone formula (RetargetMath's RetargetLocalRotation) assumes the
// clip's local rotation is relative to the RECONSTRUCTED rest. It is not: a
// clip's local rotations are relative to the skeleton's ACTUAL rest
// (identity rotation for translation-only PMX binds), and the runtime composes
// them onto that actual rest. So the two rests stay distinct:
//   Bw = ACTUAL rest world orientation      (FK of restPoseLocal rotations)
//   Rw = RECONSTRUCTED oriented rest world  (from child directions)
// Per bone: take the source's world deviation from its ACTUAL rest, re-express
// it in the source's GEOMETRIC (Rw) frame, transfer it to the target's geometric
// frame, then realize it as a local rotation composed onto the target's ACTUAL
// pose. Rest->rest is exact and identity (src == trg) is a pure copy.
//
// Convention: Mul(a,b) = XMQuaternionMultiply(a,b) = "a THEN b"; worldRot =
// Mul(localRot, parentWorldRot) (engine row-major local*parent).

namespace Resource
{
    namespace
    {
        XMVECTOR Mul(FXMVECTOR a, FXMVECTOR b) { return XMQuaternionMultiply(a, b); }
        XMVECTOR Inv(FXMVECTOR q)              { return XMQuaternionInverse(q); }

        XMVECTOR BonePosV(const SkeletonAsset& sk, int b)
        {
            const XMFLOAT4X4& m = sk.bindPose[b];
            return XMVectorSet(m._41, m._42, m._43, 0.f);
        }

        XMVECTOR RestLocalRot(const SkeletonAsset& sk, uint32_t b)
        {
            XMVECTOR s, r, t;
            XMMatrixDecompose(&s, &r, &t, XMLoadFloat4x4(&sk.restPoseLocal[b]));
            return r;
        }

        // Canonical "down the bone" child ROLE (limb continuation). Use the role
        // child, not the first hierarchy child: MMD/PMX arms put twist and dummy
        // bones first, which point along the arm / arbitrarily and would mismatch a
        // clean rig (Mixamo hand->finger) -> flipped hands. Count = leaf (extend the
        // parent direction).
        HumanoidBone DirChildRole(HumanoidBone r)
        {
            switch (r)
            {
            case HumanoidBone::Hips:        return HumanoidBone::Spine;
            case HumanoidBone::Spine:       return HumanoidBone::Chest;
            case HumanoidBone::Chest:       return HumanoidBone::UpperChest;
            case HumanoidBone::UpperChest:  return HumanoidBone::Neck;
            case HumanoidBone::Neck:        return HumanoidBone::Head;
            case HumanoidBone::LeftShoulder:  return HumanoidBone::LeftUpperArm;
            case HumanoidBone::LeftUpperArm:  return HumanoidBone::LeftLowerArm;
            case HumanoidBone::LeftLowerArm:  return HumanoidBone::LeftHand;
            case HumanoidBone::LeftHand:      return HumanoidBone::LeftMiddleProximal;
            case HumanoidBone::RightShoulder: return HumanoidBone::RightUpperArm;
            case HumanoidBone::RightUpperArm: return HumanoidBone::RightLowerArm;
            case HumanoidBone::RightLowerArm: return HumanoidBone::RightHand;
            case HumanoidBone::RightHand:     return HumanoidBone::RightMiddleProximal;
            case HumanoidBone::LeftUpperLeg:  return HumanoidBone::LeftLowerLeg;
            case HumanoidBone::LeftLowerLeg:  return HumanoidBone::LeftFoot;
            case HumanoidBone::LeftFoot:      return HumanoidBone::LeftToes;
            case HumanoidBone::RightUpperLeg: return HumanoidBone::RightLowerLeg;
            case HumanoidBone::RightLowerLeg: return HumanoidBone::RightFoot;
            case HumanoidBone::RightFoot:     return HumanoidBone::RightToes;
            case HumanoidBone::LeftThumbMetacarpal:    return HumanoidBone::LeftThumbProximal;
            case HumanoidBone::LeftThumbProximal:      return HumanoidBone::LeftThumbDistal;
            case HumanoidBone::LeftIndexProximal:      return HumanoidBone::LeftIndexIntermediate;
            case HumanoidBone::LeftIndexIntermediate:  return HumanoidBone::LeftIndexDistal;
            case HumanoidBone::LeftMiddleProximal:     return HumanoidBone::LeftMiddleIntermediate;
            case HumanoidBone::LeftMiddleIntermediate: return HumanoidBone::LeftMiddleDistal;
            case HumanoidBone::LeftRingProximal:       return HumanoidBone::LeftRingIntermediate;
            case HumanoidBone::LeftRingIntermediate:   return HumanoidBone::LeftRingDistal;
            case HumanoidBone::LeftLittleProximal:     return HumanoidBone::LeftLittleIntermediate;
            case HumanoidBone::LeftLittleIntermediate: return HumanoidBone::LeftLittleDistal;
            case HumanoidBone::RightThumbMetacarpal:    return HumanoidBone::RightThumbProximal;
            case HumanoidBone::RightThumbProximal:      return HumanoidBone::RightThumbDistal;
            case HumanoidBone::RightIndexProximal:      return HumanoidBone::RightIndexIntermediate;
            case HumanoidBone::RightIndexIntermediate:  return HumanoidBone::RightIndexDistal;
            case HumanoidBone::RightMiddleProximal:     return HumanoidBone::RightMiddleIntermediate;
            case HumanoidBone::RightMiddleIntermediate: return HumanoidBone::RightMiddleDistal;
            case HumanoidBone::RightRingProximal:       return HumanoidBone::RightRingIntermediate;
            case HumanoidBone::RightRingIntermediate:   return HumanoidBone::RightRingDistal;
            case HumanoidBone::RightLittleProximal:     return HumanoidBone::RightLittleIntermediate;
            case HumanoidBone::RightLittleIntermediate: return HumanoidBone::RightLittleDistal;
            default: return HumanoidBone::Count;
            }
        }

        // Hand + finger roles reconstruct their roll from the PALM NORMAL (plane
        // through hand/indexProximal/littleProximal bind positions). Fingers curl
        // about an axis defined by the palm orientation; a world-up reference
        // gives the two rigs inconsistent curl axes whenever their palm rest
        // orientations differ (UE vs PMX A-pose) -> fingers bend sideways.
        bool IsLeftHandRole(HumanoidBone r)
        {
            const uint32_t ri = static_cast<uint32_t>(r);
            return r == HumanoidBone::LeftHand ||
                   (ri >= static_cast<uint32_t>(HumanoidBone::LeftThumbMetacarpal) &&
                    ri <= static_cast<uint32_t>(HumanoidBone::LeftLittleDistal));
        }
        bool IsRightHandRole(HumanoidBone r)
        {
            const uint32_t ri = static_cast<uint32_t>(r);
            return r == HumanoidBone::RightHand ||
                   (ri >= static_cast<uint32_t>(HumanoidBone::RightThumbMetacarpal) &&
                    ri <= static_cast<uint32_t>(HumanoidBone::RightLittleDistal));
        }

        // Palm-plane normal from bind positions. The same anatomical points on
        // both rigs (engine space is uniformly left-handed) give a consistent
        // frame, which is all the Gram-Schmidt roll tie-break needs.
        bool PalmNormal(const SkeletonAsset& sk, bool right, XMVECTOR& outN)
        {
            using H = HumanoidBone;
            const int h = sk.humanoid.Bone(right ? H::RightHand : H::LeftHand);
            const int i = sk.humanoid.Bone(right ? H::RightIndexProximal : H::LeftIndexProximal);
            const int l = sk.humanoid.Bone(right ? H::RightLittleProximal : H::LeftLittleProximal);
            if (h < 0 || i < 0 || l < 0) return false;
            const XMVECTOR ph = BonePosV(sk, h);
            const XMVECTOR n  = XMVector3Cross(
                XMVectorSubtract(BonePosV(sk, i), ph),
                XMVectorSubtract(BonePosV(sk, l), ph));
            if (XMVectorGetX(XMVector3LengthSq(n)) < 1e-10f) return false;
            outN = XMVector3Normalize(n);
            return true;
        }

        // Near-vertical roles (spine chain + legs) reconstruct their roll from a
        // world-FORWARD reference; world-up is degenerate for them and its fallback
        // threshold is crossed inconsistently between rigs (slight spine lean) ->
        // torso tilt. Horizontal bones (arms/feet/fingers) keep world-up.
        bool IsNearVerticalRole(HumanoidBone r)
        {
            switch (r)
            {
            case HumanoidBone::Hips:  case HumanoidBone::Spine: case HumanoidBone::Chest:
            case HumanoidBone::UpperChest: case HumanoidBone::Neck: case HumanoidBone::Head:
            case HumanoidBone::LeftUpperLeg:  case HumanoidBone::LeftLowerLeg:
            case HumanoidBone::RightUpperLeg: case HumanoidBone::RightLowerLeg:
                return true;
            default: return false;
            }
        }

        // Per-bone reconstructed oriented rest world rotation. For ROLE bones use
        // the canonical role child for direction (anatomically consistent across
        // rigs); fall back to extending the parent->bone direction; only non-role
        // bones (whose Rw the full-pose baker never reads) use a hierarchy child.
        void ComputeRw(const SkeletonAsset& sk, std::vector<XMVECTOR>& rw)
        {
            rw.assign(sk.boneCount, XMQuaternionIdentity());
            std::vector<int> boneRole(sk.boneCount, -1);
            for (uint32_t r = 0; r < kHumanoidBoneCount; ++r)
            {
                const int b = sk.humanoid.boneForRole[r];
                if (b >= 0 && static_cast<uint32_t>(b) < sk.boneCount) boneRole[b] = static_cast<int>(r);
            }

            // Palm-plane roll references for hand/finger roles (falls back to
            // world-up on mitten rigs with no index/little fingers).
            XMVECTOR palmL = XMVectorZero(), palmR = XMVectorZero();
            const bool havePalmL = PalmNormal(sk, false, palmL);
            const bool havePalmR = PalmNormal(sk, true,  palmR);

            for (uint32_t b = 0; b < sk.boneCount; ++b)
            {
                const XMVECTOR bonePos = BonePosV(sk, static_cast<int>(b));
                bool haveChild = false;
                XMVECTOR childPos = XMVectorZero();

                const int role = boneRole[b];
                if (role >= 0)
                {
                    const HumanoidBone rc = DirChildRole(static_cast<HumanoidBone>(role));
                    if (rc != HumanoidBone::Count)
                    {
                        const int cb = sk.humanoid.boneForRole[static_cast<uint32_t>(rc)];
                        if (cb >= 0 && static_cast<uint32_t>(cb) < sk.boneCount)
                        { childPos = BonePosV(sk, cb); haveChild = true; }
                    }
                    if (!haveChild && sk.parentIndex[b] >= 0)
                    {
                        // Extend the parent->bone direction (avoids twist/dummy children).
                        const XMVECTOR pp = BonePosV(sk, sk.parentIndex[b]);
                        childPos = XMVectorAdd(bonePos, XMVectorSubtract(bonePos, pp));
                        haveChild = true;
                    }
                }
                else
                {
                    for (uint32_t j = b + 1; j < sk.boneCount; ++j)
                        if (sk.parentIndex[j] == static_cast<int32_t>(b))
                        { childPos = BonePosV(sk, static_cast<int>(j)); haveChild = true; break; }
                }

                if (haveChild)
                {
                    XMVECTOR ref = XMVectorSet(0.f, 1.f, 0.f, 0.f);          // up (arms/feet)
                    if (role >= 0)
                    {
                        const HumanoidBone hr = static_cast<HumanoidBone>(role);
                        if (IsNearVerticalRole(hr))
                            ref = XMVectorSet(0.f, 0.f, 1.f, 0.f);           // forward (spine/legs)
                        else if (IsLeftHandRole(hr) && havePalmL)
                            ref = palmL;                                     // palm plane (hand/fingers)
                        else if (IsRightHandRole(hr) && havePalmR)
                            ref = palmR;
                    }
                    rw[b] = ReconstructOrientedRestRef(bonePos, childPos, ref);
                }
            }
        }

        // Actual rest world rotation per bone = FK of restPoseLocal rotations.
        void ComputeBw(const SkeletonAsset& sk, std::vector<XMVECTOR>& bw)
        {
            bw.assign(sk.boneCount, XMQuaternionIdentity());
            for (uint32_t b = 0; b < sk.boneCount; ++b)
            {
                const XMVECTOR r = RestLocalRot(sk, b);
                const int par = sk.parentIndex[b];
                bw[b] = (par < 0) ? r : Mul(r, bw[par]);
            }
        }

        // Full-frame alignment: build an orthonormal triad from (a1, a2raw) and
        // one from (t1, t2raw), and return the world rotation taking the first to
        // the second. A single-axis shortest arc leaves the ROLL unconstrained,
        // which is never enough for a bone whose children depend on that roll
        // (hand -> fingers, shin -> foot). Row-vector convention: Q = A^T * T,
        // applied as Mul(rest, Q) ("rest THEN Q", i.e. post-multiplied in world).
        XMVECTOR TriadAlign(XMVECTOR a1, XMVECTOR a2raw, XMVECTOR t1, XMVECTOR t2raw)
        {
            a1 = XMVector3Normalize(a1);
            XMVECTOR a2 = XMVector3Normalize(XMVectorSubtract(
                a2raw, XMVectorScale(a1, XMVectorGetX(XMVector3Dot(a2raw, a1)))));
            XMVECTOR a3 = XMVector3Cross(a1, a2);
            t1 = XMVector3Normalize(t1);
            XMVECTOR t2 = XMVector3Normalize(XMVectorSubtract(
                t2raw, XMVectorScale(t1, XMVectorGetX(XMVector3Dot(t2raw, t1)))));
            XMVECTOR t3 = XMVector3Cross(t1, t2);
            const XMMATRIX A(XMVectorSetW(a1, 0), XMVectorSetW(a2, 0),
                             XMVectorSetW(a3, 0), XMVectorSet(0, 0, 0, 1));
            const XMMATRIX T(XMVectorSetW(t1, 0), XMVectorSetW(t2, 0),
                             XMVectorSetW(t3, 0), XMVectorSet(0, 0, 0, 1));
            return XMQuaternionNormalize(XMQuaternionRotationMatrix(
                XMMatrixMultiply(XMMatrixTranspose(A), T)));
        }

        // A-pose -> T-pose reference compensation. Straightens each arm bone's
        // reconstructed (rw) and actual (bw) rest orientation so it points along
        // the horizontal plane (perpendicular to the body's up axis) - the T-pose
        // arm direction - instead of the A-pose down-tilt. Applied to the rig(s)
        // the user marks A-pose so both sides are compared from a common T-pose
        // reference. Only the arms need this (A/T-pose legs are identical).
        void ApplyArmTPoseCorrection(const SkeletonAsset& sk,
                                     std::vector<XMVECTOR>& rw, std::vector<XMVECTOR>& bw)
        {
            if (!sk.humanoid.Has(HumanoidBone::Head) || !sk.humanoid.Has(HumanoidBone::Hips))
                return;
            const XMVECTOR head = BonePosV(sk, sk.humanoid.Bone(HumanoidBone::Head));
            const XMVECTOR hips = BonePosV(sk, sk.humanoid.Bone(HumanoidBone::Hips));
            XMVECTOR bodyUp = XMVectorSubtract(head, hips);
            if (XMVectorGetX(XMVector3LengthSq(bodyUp)) < 1e-8f) bodyUp = XMVectorSet(0, 1, 0, 0);
            bodyUp = XMVector3Normalize(bodyUp);

            // Include the shoulders/clavicles: if only the arm is straightened to
            // horizontal while the clavicle keeps its A-pose droop, the shoulder
            // joint's bend reverses (arm ends up ABOVE a down-pointing clavicle) ->
            // a concave, hunched silhouette. Lifting the clavicle to flat too keeps
            // the shoulder line straight. (Flat, not a magic up-tilt: the bind-
            // relative delta absorbs the absolute rest-angle difference between rigs.)
            const HumanoidBone arms[] = {
                HumanoidBone::LeftShoulder,  HumanoidBone::LeftUpperArm,  HumanoidBone::LeftLowerArm,  HumanoidBone::LeftHand,
                HumanoidBone::RightShoulder, HumanoidBone::RightUpperArm, HumanoidBone::RightLowerArm, HumanoidBone::RightHand };
            const XMVECTOR unitY = XMVectorSet(0, 1, 0, 0);

            // TRUE LATERAL axis (shoulder line, perpendicular to body-up). The
            // T-pose target must zero the arms' AZIMUTH too, not just elevation:
            // a rest pose whose forearms sweep forward (UE holds hands slightly
            // in front) would otherwise keep that sweep, leaving the two rigs'
            // bend axes rotated apart -> elbows/fingers bend the wrong way.
            XMVECTOR lat = XMVectorZero();
            bool haveLat = false;
            {
                const int ual = sk.humanoid.Bone(HumanoidBone::LeftUpperArm);
                const int uar = sk.humanoid.Bone(HumanoidBone::RightUpperArm);
                if (ual >= 0 && uar >= 0)
                {
                    XMVECTOR l = XMVectorSubtract(BonePosV(sk, ual), BonePosV(sk, uar));
                    l = XMVectorSubtract(l, XMVectorScale(bodyUp,
                            XMVectorGetX(XMVector3Dot(l, bodyUp))));         // reject body-up
                    if (XMVectorGetX(XMVector3LengthSq(l)) > 1e-8f)
                    { lat = XMVector3Normalize(l); haveLat = true; }
                }
            }

            for (HumanoidBone role : arms)
            {
                const int b = sk.humanoid.boneForRole[static_cast<uint32_t>(role)];
                if (b < 0) continue;

                const XMVECTOR dA = XMVector3Rotate(unitY, rw[b]);           // current bone direction
                XMVECTOR dT;
                if (haveLat)
                {
                    // Snap to the pure lateral axis, per side (sign from the
                    // bone's current lateral component).
                    const float s = XMVectorGetX(XMVector3Dot(dA, lat)) >= 0.f ? 1.f : -1.f;
                    dT = XMVectorScale(lat, s);
                }
                else
                {
                    // Fallback: elevation-only straightening (horizontal proj).
                    dT = XMVectorSubtract(
                        dA, XMVectorScale(bodyUp, XMVectorGetX(XMVector3Dot(dA, bodyUp))));
                    if (XMVectorGetX(XMVector3LengthSq(dT)) < 1e-6f) continue;
                    dT = XMVector3Normalize(dT);
                }

                const bool isLeftHandB  = (role == HumanoidBone::LeftHand);
                const bool isRightHandB = (role == HumanoidBone::RightHand);

                XMVECTOR Q;
                XMVECTOR palmA;
                const bool handFrame =
                    (isLeftHandB || isRightHandB) && PalmNormal(sk, isRightHandB, palmA);
                if (handFrame)
                {
                    // HAND: a single-axis shortest arc leaves the ROLL (palm facing)
                    // unconstrained - a source whose hand swings far (UE ~70 deg) and a
                    // target that barely moves (~10 deg) end up with very different
                    // post-snap palm orientations, and the rigidly attached fingers curl
                    // about a ~90 deg wrong axis. Align the FULL frame instead:
                    // direction -> +-lateral AND palm normal -> canonical (left: -bodyUp,
                    // right: +bodyUp; the anatomical cross gives palm side on the left
                    // hand and back side on the right, so a T-pose palms-down is exactly
                    // this pair).
                    const XMVECTOR pT = XMVectorScale(bodyUp, isLeftHandB ? -1.f : 1.f);
                    Q = TriadAlign(dA, palmA, dT, pT);
                }
                else if (std::fabs(XMVectorGetX(XMVector3Dot(dT, unitY))) < 0.99f)
                {
                    // CLAVICLE / UPPER ARM / LOWER ARM: full frame, for the same reason
                    // as the hand. A shortest arc pins only the DIRECTION; the leftover
                    // ROLL about the bone axis is whatever the snap dragged along, and it
                    // differs between rigs by how far each rest arm had to swing in
                    // AZIMUTH. UE's forearm rest sweeps ~50 deg forward (hands held in
                    // front) while PMX's is purely lateral, which leaves the post-snap
                    // forearm frames ~19 deg apart about the arm axis - and that roll IS
                    // the elbow's bend plane, so the forearm swings out instead of forward.
                    // Aligning the full frame against the SAME world-up reference ComputeRw
                    // built these frames with gives exactly the canonical frame
                    // [+-lateral, world-up roll] on both rigs, so the geometric conjugation
                    // becomes the identity. (Guarded: if dT is parallel to world-up the
                    // triad is degenerate - fall through to the shortest arc.)
                    Q = TriadAlign(dA, unitY, dT, unitY);
                }
                else
                {
                    const float dot = XMVectorGetX(XMVector3Dot(dA, dT));
                    XMVECTOR axis = XMVector3Cross(dA, dT);
                    if (XMVectorGetX(XMVector3LengthSq(axis)) < 1e-10f)
                        Q = (dot > 0.f) ? XMQuaternionIdentity()
                                        : XMQuaternionRotationAxis(bodyUp, 3.14159265f);
                    else
                    {
                        axis = XMVector3Normalize(axis);
                        const float ang = std::acos(dot < -1.f ? -1.f : (dot > 1.f ? 1.f : dot));
                        Q = XMQuaternionRotationAxis(axis, ang);
                    }
                }
                rw[b] = XMQuaternionNormalize(Mul(rw[b], Q));
                bw[b] = XMQuaternionNormalize(Mul(bw[b], Q));

                // Finger rest canonicalization. Fingers transfer ABSOLUTE
                // orientations, so finger RESTS are canonicalized like the arms: a
                // rig whose bind fingers are pre-curled (UE's relaxed hands) would
                // otherwise keep that curl as a permanent offset (target = its
                // straight rest + delta != the source's absolute pose; fingers read as
                // bent toward -Y). Snap EACH finger bone's rest frame to the hand's
                // canonical triad (dir -> +-lateral, secondary -> canonical palm) so
                // the bind curl becomes MOTION. Secondary reference = the KNUCKLE-plane
                // normal cross(spread, hand->middle), built from base joints only and so
                // immune to the bind curl. Without palm data, fall back to rigid Q
                // propagation.
                const bool isLeftHand  = (role == HumanoidBone::LeftHand);
                const bool isRightHand = (role == HumanoidBone::RightHand);
                if (isLeftHand || isRightHand)
                {
                    using H = HumanoidBone;
                    const int hB   = sk.humanoid.Bone(isLeftHand ? H::LeftHand : H::RightHand);
                    const int midB = sk.humanoid.Bone(isLeftHand ? H::LeftMiddleProximal
                                                                 : H::RightMiddleProximal);
                    const int idxB = sk.humanoid.Bone(isLeftHand ? H::LeftIndexProximal
                                                                 : H::RightIndexProximal);
                    const int litB = sk.humanoid.Bone(isLeftHand ? H::LeftLittleProximal
                                                                 : H::RightLittleProximal);
                    const bool canonical = handFrame && hB >= 0 && midB >= 0 &&
                                           idxB >= 0 && litB >= 0;

                    XMVECTOR pnR = XMVectorZero(), tPalm = XMVectorZero();
                    if (canonical)
                    {
                        const XMVECTOR priH   = XMVectorSubtract(BonePosV(sk, midB), BonePosV(sk, hB));
                        const XMVECTOR spread = XMVectorSubtract(BonePosV(sk, idxB), BonePosV(sk, litB));
                        pnR   = XMVector3Cross(spread, priH);      // knuckle-plane normal
                        tPalm = XMVectorScale(bodyUp, isLeftHand ? -1.f : 1.f);
                    }

                    const uint32_t f0 = static_cast<uint32_t>(
                        isLeftHand ? H::LeftThumbMetacarpal : H::RightThumbMetacarpal);
                    const uint32_t f1 = static_cast<uint32_t>(
                        isLeftHand ? H::LeftLittleDistal : H::RightLittleDistal);
                    for (uint32_t fr = f0; fr <= f1; ++fr)
                    {
                        const int fb = sk.humanoid.boneForRole[fr];
                        if (fb < 0) continue;

                        XMVECTOR Qf = Q; // fallback: ride rigidly with the hand
                        if (canonical &&
                            XMVectorGetX(XMVector3LengthSq(pnR)) > 1e-10f)
                        {
                            // Per-bone bind direction: canonical role child, else
                            // extend away from the hierarchy parent (finger tips).
                            const HumanoidBone rc = DirChildRole(static_cast<HumanoidBone>(fr));
                            const int cb = (rc != HumanoidBone::Count)
                                           ? sk.humanoid.boneForRole[static_cast<uint32_t>(rc)] : -1;
                            XMVECTOR dirF;
                            if (cb >= 0)
                                dirF = XMVectorSubtract(BonePosV(sk, cb), BonePosV(sk, fb));
                            else if (sk.parentIndex[fb] >= 0)
                                dirF = XMVectorSubtract(BonePosV(sk, fb),
                                                        BonePosV(sk, sk.parentIndex[fb]));
                            else
                                dirF = XMVectorZero();

                            if (XMVectorGetX(XMVector3LengthSq(dirF)) > 1e-10f)
                                Qf = TriadAlign(dirF, pnR, dT, tPalm); // dT = hand's +-lateral target
                        }
                        rw[fb] = XMQuaternionNormalize(Mul(rw[fb], Qf));
                        bw[fb] = XMQuaternionNormalize(Mul(bw[fb], Qf));
                    }
                }
            }
        }

        // Reference-pose canonicalization for the LEGS - the leg analogue of
        // ApplyArmTPoseCorrection (RetargetOptions::canonicalizeLegs says why).
        //
        // Snap both rigs' UpperLeg/LowerLeg rest FRAMES (not just their directions:
        // a shortest arc would leave the roll - and so the knee bend axis -
        // unconstrained) to "straight down the body axis, world-forward roll".
        // The two rigs then share one leg reference and the source's ABSOLUTE leg
        // orientation transfers, as the arms do. Feet have their own pass
        // (ApplyFootRestCanonicalization).
        void ApplyLegRestCanonicalization(const SkeletonAsset& sk,
                                          std::vector<XMVECTOR>& rw, std::vector<XMVECTOR>& bw)
        {
            if (!sk.humanoid.Has(HumanoidBone::Head) || !sk.humanoid.Has(HumanoidBone::Hips))
                return;
            const XMVECTOR head = BonePosV(sk, sk.humanoid.Bone(HumanoidBone::Head));
            const XMVECTOR hips = BonePosV(sk, sk.humanoid.Bone(HumanoidBone::Hips));
            XMVECTOR bodyUp = XMVectorSubtract(head, hips);
            if (XMVectorGetX(XMVector3LengthSq(bodyUp)) < 1e-8f) bodyUp = XMVectorSet(0, 1, 0, 0);
            bodyUp = XMVector3Normalize(bodyUp);

            const XMVECTOR down = XMVectorNegate(bodyUp);          // canonical leg axis
            // Same world-FORWARD roll reference ComputeRw already uses for the
            // near-vertical roles, so the canonical frame comes out identical on
            // both rigs (up would be degenerate against a straight-down bone).
            const XMVECTOR fwdRef = XMVectorSet(0.f, 0.f, 1.f, 0.f);
            if (std::fabs(XMVectorGetX(XMVector3Dot(down, fwdRef))) > 0.99f) return;

            const XMVECTOR unitY = XMVectorSet(0, 1, 0, 0);
            const HumanoidBone legs[] = {
                HumanoidBone::LeftUpperLeg,  HumanoidBone::LeftLowerLeg,
                HumanoidBone::RightUpperLeg, HumanoidBone::RightLowerLeg };

            for (HumanoidBone role : legs)
            {
                const int b = sk.humanoid.boneForRole[static_cast<uint32_t>(role)];
                if (b < 0 || static_cast<uint32_t>(b) >= sk.boneCount) continue;

                const XMVECTOR dA = XMVector3Normalize(XMVector3Rotate(unitY, rw[b]));
                if (std::fabs(XMVectorGetX(XMVector3Dot(dA, fwdRef))) > 0.99f)
                    continue;  // degenerate: bone lies along the roll reference
                const XMVECTOR Q = TriadAlign(dA, fwdRef, down, fwdRef);
                rw[b] = XMQuaternionNormalize(Mul(rw[b], Q));
                bw[b] = XMQuaternionNormalize(Mul(bw[b], Q));
            }
        }

        // Reference-pose canonicalization for the FEET (RetargetOptions::canonicalizeFeet
        // says why).
        //
        // Snap both rigs' foot rest FRAMES to (body-forward, body-up roll). Forward
        // is derived from the rig itself - the horizontal part of its two feet's
        // rest directions summed, so left/right toe-out cancels and nothing has to
        // be assumed about which way the rig faces (Mixamo faces +Z, PMX -Z).
        void ApplyFootRestCanonicalization(const SkeletonAsset& sk,
                                           std::vector<XMVECTOR>& rw, std::vector<XMVECTOR>& bw)
        {
            using H = HumanoidBone;
            if (!sk.humanoid.Has(H::Head) || !sk.humanoid.Has(H::Hips)) return;
            XMVECTOR bodyUp = XMVectorSubtract(BonePosV(sk, sk.humanoid.Bone(H::Head)),
                                               BonePosV(sk, sk.humanoid.Bone(H::Hips)));
            if (XMVectorGetX(XMVector3LengthSq(bodyUp)) < 1e-8f) bodyUp = XMVectorSet(0, 1, 0, 0);
            bodyUp = XMVector3Normalize(bodyUp);

            const H feet[]  = { H::LeftFoot, H::RightFoot };
            const H toes[]  = { H::LeftToes, H::RightToes };

            XMVECTOR fwd = XMVectorZero();
            for (int i = 0; i < 2; ++i)
            {
                const int f = sk.humanoid.boneForRole[static_cast<uint32_t>(feet[i])];
                const int t = sk.humanoid.boneForRole[static_cast<uint32_t>(toes[i])];
                if (f < 0 || t < 0) continue;
                const XMVECTOR d = XMVectorSubtract(BonePosV(sk, t), BonePosV(sk, f));
                if (XMVectorGetX(XMVector3LengthSq(d)) > 1e-10f)
                    fwd = XMVectorAdd(fwd, XMVector3Normalize(d));
            }
            fwd = XMVectorSubtract(fwd, XMVectorScale(bodyUp, XMVectorGetX(XMVector3Dot(fwd, bodyUp))));
            if (XMVectorGetX(XMVector3LengthSq(fwd)) < 1e-8f) return;   // feet unmapped/vertical
            fwd = XMVector3Normalize(fwd);

            const XMVECTOR unitY = XMVectorSet(0, 1, 0, 0);
            for (H role : feet)
            {
                const int b = sk.humanoid.boneForRole[static_cast<uint32_t>(role)];
                if (b < 0 || static_cast<uint32_t>(b) >= sk.boneCount) continue;
                const XMVECTOR dA = XMVector3Normalize(XMVector3Rotate(unitY, rw[b]));
                if (std::fabs(XMVectorGetX(XMVector3Dot(dA, bodyUp))) > 0.99f) continue;
                const XMVECTOR Q = TriadAlign(dA, bodyUp, fwd, bodyUp);   // full frame
                rw[b] = XMQuaternionNormalize(Mul(rw[b], Q));
                bw[b] = XMQuaternionNormalize(Mul(bw[b], Q));
            }
        }

        // Reference-pose canonicalization for NECK + HEAD (canonicalizeNeck).
        // Same treatment as the legs, pointing up instead of down: snap the rest
        // frame to "straight up the body axis, world-forward roll".
        //
        // Deliberately NOT applied to Spine/Chest/UpperChest: a rig's spine curvature
        // is authored posture and the two rigs spread it over a different number of
        // bones (UE 5, PMX 2-3), so forcing a common straight reference makes the
        // chest markedly worse (4.5 -> 16.1 deg off the source).
        void ApplyNeckRestCanonicalization(const SkeletonAsset& sk,
                                           std::vector<XMVECTOR>& rw, std::vector<XMVECTOR>& bw)
        {
            using H = HumanoidBone;
            if (!sk.humanoid.Has(H::Head) || !sk.humanoid.Has(H::Hips)) return;
            XMVECTOR bodyUp = XMVectorSubtract(BonePosV(sk, sk.humanoid.Bone(H::Head)),
                                               BonePosV(sk, sk.humanoid.Bone(H::Hips)));
            if (XMVectorGetX(XMVector3LengthSq(bodyUp)) < 1e-8f) return;
            bodyUp = XMVector3Normalize(bodyUp);

            const XMVECTOR fwdRef = XMVectorSet(0.f, 0.f, 1.f, 0.f);
            if (std::fabs(XMVectorGetX(XMVector3Dot(bodyUp, fwdRef))) > 0.99f) return;

            const XMVECTOR unitY = XMVectorSet(0, 1, 0, 0);
            for (H role : { H::Neck, H::Head })
            {
                const int b = sk.humanoid.boneForRole[static_cast<uint32_t>(role)];
                if (b < 0 || static_cast<uint32_t>(b) >= sk.boneCount) continue;
                const XMVECTOR dA = XMVector3Normalize(XMVector3Rotate(unitY, rw[b]));
                if (std::fabs(XMVectorGetX(XMVector3Dot(dA, fwdRef))) > 0.99f) continue;
                const XMVECTOR Q = TriadAlign(dA, fwdRef, bodyUp, fwdRef);
                rw[b] = XMQuaternionNormalize(Mul(rw[b], Q));
                bw[b] = XMQuaternionNormalize(Mul(bw[b], Q));
            }
        }

        // ---- Twist redistribution (RetargetOptions::redistributeTwist) ----
        //
        // Roles whose axial roll belongs on a dedicated twist bone rather than on
        // the joint. Shoulders/hands are excluded on purpose: a clavicle has no
        // twist bone, and the bone between the wrist and the fingers on an MMD
        // rig is a dummy (an arbitrary IK/attach helper), not a twist joint.
        bool IsTwistCarrierRole(HumanoidBone r)
        {
            switch (r)
            {
            case HumanoidBone::LeftUpperArm:  case HumanoidBone::LeftLowerArm:
            case HumanoidBone::RightUpperArm: case HumanoidBone::RightLowerArm:
            case HumanoidBone::LeftUpperLeg:  case HumanoidBone::LeftLowerLeg:
            case HumanoidBone::RightUpperLeg: case HumanoidBone::RightLowerLeg:
                return true;
            default: return false;
            }
        }

        // The interposed twist bone between @p bone and its canonical role child
        // @p childBone, or -1. Found structurally rather than by name so it works on
        // any rig: it must be the child's only parent link back to the bone, carry no
        // role of its own, have no grant (append) source (that would make it a driven
        // cancel bone like MMD's shoulder-cancel bone, not a free twist joint), and
        // lie on the bone's axis.
        int FindTwistBone(const SkeletonAsset& sk, uint32_t bone, int childBone,
                          const std::vector<int>& boneRole)
        {
            if (childBone < 0 || static_cast<uint32_t>(childBone) >= sk.boneCount) return -1;
            const int tw = sk.parentIndex[childBone];
            if (tw < 0 || static_cast<uint32_t>(tw) >= sk.boneCount) return -1;
            if (static_cast<uint32_t>(tw) == bone) return -1;   // child hangs straight off the bone
            if (sk.parentIndex[tw] != static_cast<int32_t>(bone)) return -1;
            if (boneRole[tw] >= 0) return -1;
            if (sk.grantSource[tw] >= 0) return -1;

            const XMVECTOR base = BonePosV(sk, static_cast<int>(bone));
            const XMVECTOR d = XMVectorSubtract(BonePosV(sk, childBone), base);
            const XMVECTOR t = XMVectorSubtract(BonePosV(sk, tw), base);
            if (XMVectorGetX(XMVector3LengthSq(d)) < 1e-8f ||
                XMVectorGetX(XMVector3LengthSq(t)) < 1e-8f) return -1;
            const float c = XMVectorGetX(XMVector3Dot(XMVector3Normalize(d),
                                                      XMVector3Normalize(t)));
            return (c > 0.99f) ? tw : -1;
        }

        // Split a local rotation into swing and twist about @p axis (given in the
        // frame the local rotation acts in, i.e. the parent's rest frame), such
        // that local == Mul(twist, swing) - the twist applied first, about the
        // bone's own rest axis, which is the same rotation as swinging first and
        // then twisting about the swung axis.
        void SwingTwistSplit(FXMVECTOR local, FXMVECTOR axis,
                             XMVECTOR& outSwing, XMVECTOR& outTwist)
        {
            const XMVECTOR a = XMVector3Normalize(axis);
            XMVECTOR t = XMVectorSetW(
                XMVectorScale(a, XMVectorGetX(XMVector3Dot(local, a))),
                XMVectorGetW(local));
            if (XMVectorGetX(XMVector4LengthSq(t)) < 1e-12f)
            {
                // 180 deg swing: the twist is undefined, leave it all as swing.
                outTwist = XMQuaternionIdentity();
                outSwing = local;
                return;
            }
            t = XMQuaternionNormalize(t);
            outTwist = t;
            outSwing = XMQuaternionNormalize(Mul(Inv(t), local));
        }
    } // namespace

    bool RetargetBaker::IsRetargetRole(HumanoidBone role, const RetargetOptions& opts)
    {
        using H = HumanoidBone;
        const uint32_t ri = static_cast<uint32_t>(role);
        const bool isFinger = ri >= static_cast<uint32_t>(H::LeftThumbMetacarpal) &&
                              ri <= static_cast<uint32_t>(H::RightLittleDistal);

        // Toes are mapped for FOOT-direction reconstruction only (on PMX they are
        // often the toe-IK control bone), never retargeted - baking a rotation onto
        // an IK bone would fight the solver.
        if (role == H::LeftToes || role == H::RightToes) return false;

        bool isLower = false;
        switch (role)
        {
        case H::Hips:
        case H::LeftUpperLeg:  case H::LeftLowerLeg:  case H::LeftFoot:  case H::LeftToes:
        case H::RightUpperLeg: case H::RightLowerLeg: case H::RightFoot: case H::RightToes:
            isLower = true; break;
        default: break;
        }

        if (isLower)  return opts.lowerBody;
        if (isFinger) return opts.upperBody && opts.includeFingers;
        return opts.upperBody; // spine/chest/upperChest/neck/head/eyes/jaw/shoulders/arms/hands
    }

    ClipAsset RetargetBaker::BakeClip(const SkeletonAsset&   srcSkel,
                                      const ClipAsset&       srcClip,
                                      const SkeletonAsset&   trgSkel,
                                      const RetargetOptions& opts)
    {
        ClipAsset out;
        out.boneCount    = trgSkel.boneCount;
        out.frameCount   = srcClip.frameCount;
        out.duration     = srcClip.duration;
        out.frameRate    = srcClip.frameRate;
        out.events       = srcClip.events;
        out.notifyTracks = srcClip.notifyTracks;out.floatCurves=srcClip.floatCurves;
        out.nextNotifyId = srcClip.nextNotifyId;
        // Retargeted clips carry baked FK legs - flag them so IKSystem skips the
        // MMD leg CCD for entities playing this clip (leg IK weight = 0). Native
        // VMD clips keep the LegDriveMode::IK default and run the solver.
        out.legDriveMode = LegDriveMode::FK;

        const uint32_t fc = out.frameCount;
        const size_t   kf = static_cast<size_t>(out.boneCount) * fc;
        out.positions.resize(kf);
        out.rotations.resize(kf);
        out.scales.resize(kf);
        if (fc == 0) return out;

        // Init every target bone to its rest local TRS for every frame.
        std::vector<XMVECTOR> trgRestRot(trgSkel.boneCount);
        for (uint32_t b = 0; b < trgSkel.boneCount; ++b)
        {
            XMVECTOR s, r, t;
            XMFLOAT3 pos = { 0,0,0 }, scl = { 1,1,1 };
            XMFLOAT4 rot = { 0,0,0,1 };
            if (XMMatrixDecompose(&s, &r, &t, XMLoadFloat4x4(&trgSkel.restPoseLocal[b])))
            {
                XMStoreFloat3(&pos, t); XMStoreFloat4(&rot, r); XMStoreFloat3(&scl, s);
            }
            trgRestRot[b] = XMLoadFloat4(&rot);
            for (uint32_t f = 0; f < fc; ++f)
            {
                const size_t i = static_cast<size_t>(b) * fc + f;
                out.positions[i] = pos; out.rotations[i] = rot; out.scales[i] = scl;
            }
        }

        if (srcClip.rotations.empty() || srcClip.frameCount == 0) return out;

        // Frame-independent rests.
        std::vector<XMVECTOR> srcRW, trgRW, srcBW, trgBW;
        ComputeRw(srcSkel, srcRW); ComputeRw(trgSkel, trgRW);
        ComputeBw(srcSkel, srcBW); ComputeBw(trgSkel, trgBW);
        // Untouched copy: the twist pass needs the rig's REAL rest frames to place
        // its bone axes, not the straightened reference frames used by the delta.
        const std::vector<XMVECTOR> trgBWrest = trgBW;
        // A-pose -> T-pose reference straightening (arms), per the user's marking.
        if (opts.sourceAPose) ApplyArmTPoseCorrection(srcSkel, srcRW, srcBW);
        if (opts.targetAPose) ApplyArmTPoseCorrection(trgSkel, trgRW, trgBW);
        // Canonical leg reference for both rigs, independent of the A-pose marking:
        // leg splay is a rest-geometry difference, not an A/T-pose choice.
        if (opts.canonicalizeLegs)
        {
            ApplyLegRestCanonicalization(srcSkel, srcRW, srcBW);
            ApplyLegRestCanonicalization(trgSkel, trgRW, trgBW);
        }
        if (opts.canonicalizeFeet)
        {
            ApplyFootRestCanonicalization(srcSkel, srcRW, srcBW);
            ApplyFootRestCanonicalization(trgSkel, trgRW, trgBW);
        }
        if (opts.canonicalizeNeck)
        {
            ApplyNeckRestCanonicalization(srcSkel, srcRW, srcBW);
            ApplyNeckRestCanonicalization(trgSkel, trgRW, trgBW);
        }

        // Reverse map: target bone -> retargeted role (or -1).
        std::vector<int> trgBoneRole(trgSkel.boneCount, -1);
        for (uint32_t ri = 0; ri < kHumanoidBoneCount; ++ri)
        {
            const HumanoidBone role = static_cast<HumanoidBone>(ri);
            if (!IsRetargetRole(role, opts)) continue;
            const int tb = trgSkel.humanoid.boneForRole[ri];
            if (tb >= 0 && static_cast<uint32_t>(tb) < trgSkel.boneCount) trgBoneRole[tb] = static_cast<int>(ri);
        }

        // ---- 1) Rotation retarget (full pose, per frame) ----
        std::vector<XMVECTOR> awSrc(srcSkel.boneCount);
        std::vector<XMVECTOR> awTrg(trgSkel.boneCount);
        for (uint32_t f = 0; f < fc; ++f)
        {
            const uint32_t sf = (f < srcClip.frameCount) ? f : srcClip.frameCount - 1;

            // Source animated world rotations (FK of the source clip locals).
            for (uint32_t b = 0; b < srcSkel.boneCount; ++b)
            {
                const XMVECTOR lr = (b < srcClip.boneCount)
                    ? XMLoadFloat4(&srcClip.rotations[static_cast<size_t>(b) * srcClip.frameCount + sf])
                    : XMQuaternionIdentity();
                const int par = srcSkel.parentIndex[b];
                awSrc[b] = (par < 0) ? lr : Mul(lr, awSrc[par]);
            }

            // Target, root-first (parentIndex[b] < b).
            for (uint32_t b = 0; b < trgSkel.boneCount; ++b)
            {
                const int par = trgSkel.parentIndex[b];
                const XMVECTOR parW = (par < 0) ? XMQuaternionIdentity() : awTrg[par];

                XMVECTOR localRot = trgRestRot[b];
                const int role = trgBoneRole[b];
                if (role >= 0)
                {
                    const int s = srcSkel.humanoid.boneForRole[role];
                    if (s >= 0 && static_cast<uint32_t>(s) < srcSkel.boneCount)
                    {
                        // Source world deviation from ACTUAL rest (Aw = Bw*Dw, Dw
                        // outer/world), conjugated into the source GEOMETRIC frame.
                        const XMVECTOR DwSrc = Mul(Inv(srcBW[s]), awSrc[s]);
                        // world->local: Rw^-1 * Dw * Rw (express the world deviation in
                        // the source geometric frame's local axes).
                        const XMVECTOR Dgeo  = Mul(srcRW[s], Mul(DwSrc, Inv(srcRW[s])));
                        // local->world in the target geometric frame; realize as an
                        // animated world orientation (Aw = Bw*Dw).
                        const XMVECTOR DwTrg = Mul(Inv(trgRW[b]), Mul(Dgeo, trgRW[b]));
                        const XMVECTOR awB   = Mul(trgBW[b], DwTrg);
                        localRot = Mul(awB, Inv(parW));      // world -> local against animated parent
                        awTrg[b] = awB;
                    }
                    else awTrg[b] = Mul(localRot, parW);
                }
                else awTrg[b] = Mul(localRot, parW);

                XMStoreFloat4(&out.rotations[static_cast<size_t>(b) * fc + f], XMQuaternionNormalize(localRot));
            }
        }

        // ---- 1b) Twist redistribution (RetargetOptions::redistributeTwist) ----
        // The transfer writes each role bone's ABSOLUTE orientation, so all of the
        // limb's axial roll ends up on that one joint. Move the axial component onto
        // the twist bone, which sits on the same axis between the joint and its
        // child, so the twist bone's world orientation (and everything below it,
        // whose locals are already written) is bit-for-bit unchanged.
        if (opts.redistributeTwist)
        {
            struct TwistJob { uint32_t bone, twistBone; XMVECTOR axis, twistRest; };
            std::vector<TwistJob> jobs;
            for (uint32_t b = 0; b < trgSkel.boneCount; ++b)
            {
                const int role = trgBoneRole[b];
                if (role < 0 || !IsTwistCarrierRole(static_cast<HumanoidBone>(role))) continue;
                const HumanoidBone rc = DirChildRole(static_cast<HumanoidBone>(role));
                if (rc == HumanoidBone::Count) continue;
                const int cb = trgSkel.humanoid.boneForRole[static_cast<uint32_t>(rc)];
                const int tw = FindTwistBone(trgSkel, b, cb, trgBoneRole);
                if (tw < 0) continue;

                // Twist about the bone -> TWIST BONE direction, not bone -> child:
                // rigs do not always place the twist bone exactly on the joint axis
                // (a PMX arm-twist bone can be ~1.25 deg off), and rolling about the
                // twist bone's own offset keeps that bone - and its whole subtree -
                // exactly where it was.
                // The local rotation acts in the parent's rest frame, so express
                // the axis there too (identity for a PMX rig).
                const int par = trgSkel.parentIndex[b];
                XMVECTOR axis = XMVectorSubtract(BonePosV(trgSkel, tw),
                                                 BonePosV(trgSkel, static_cast<int>(b)));
                if (par >= 0) axis = XMVector3Rotate(axis, Inv(trgBWrest[par]));
                jobs.push_back({ b, static_cast<uint32_t>(tw),
                                 XMVector3Normalize(axis), trgRestRot[tw] });
            }

            for (const TwistJob& j : jobs)
            {
                for (uint32_t f = 0; f < fc; ++f)
                {
                    const size_t bi = static_cast<size_t>(j.bone)      * fc + f;
                    const size_t ti = static_cast<size_t>(j.twistBone) * fc + f;
                    XMVECTOR swing, twist;
                    SwingTwistSplit(XMLoadFloat4(&out.rotations[bi]), j.axis, swing, twist);
                    XMStoreFloat4(&out.rotations[bi], swing);
                    // Compose onto the twist bone's own rest so a non-identity rest
                    // (non-PMX rigs) keeps its world orientation.
                    XMStoreFloat4(&out.rotations[ti],
                                  XMQuaternionNormalize(Mul(j.twistRest, twist)));
                }
            }
        }

        // ---- 2) Root motion: Hips translation, hip-height scaled ----
        // Target bone = the profile's hips-TRANSLATION bone when it differs from
        // the Hips rotation bone (MMD: rotation -> lower-body bone, translation ->
        // center bone); -1 falls back to the Hips role bone (single-Hips rigs).
        if (opts.lowerBody && opts.bakeRootMotion &&
            srcSkel.humanoid.Has(HumanoidBone::Hips) && trgSkel.humanoid.Has(HumanoidBone::Hips))
        {
            const int shb = srcSkel.humanoid.Bone(HumanoidBone::Hips);
            int thb = trgSkel.humanoid.hipsTranslationBone;
            if (thb < 0 || static_cast<uint32_t>(thb) >= trgSkel.boneCount)
                thb = trgSkel.humanoid.Bone(HumanoidBone::Hips);
            if (static_cast<uint32_t>(shb) < srcClip.boneCount)
            {
                XMVECTOR ss, sr, st;
                XMMatrixDecompose(&ss, &sr, &st, XMLoadFloat4x4(&srcSkel.restPoseLocal[shb]));
                const XMVECTOR srcRestPos = st;
                const XMFLOAT3 trgRest = out.positions[static_cast<size_t>(thb) * fc + 0];
                const XMVECTOR trgRestPos = XMLoadFloat3(&trgRest);

                const float sh = srcSkel.humanoid.hipHeight;
                const float th = trgSkel.humanoid.hipHeight;
                const float scale = (sh > 1e-4f) ? (th / sh) : 1.f;

                // The delta is PARENT-LOCAL on each side. Route it through world
                // space using the ACTUAL rest world rotations, or a source whose
                // hips parent is rotated (UE root carries the Z-up->Y-up fix)
                // pushes the character sideways/up instead of crouching.
                const int sPar = srcSkel.parentIndex[shb];
                const int tPar = trgSkel.parentIndex[thb];
                const XMVECTOR sParRot = (sPar >= 0) ? srcBW[sPar] : XMQuaternionIdentity();
                const XMVECTOR tParInv = XMQuaternionInverse(
                    (tPar >= 0) ? trgBW[tPar] : XMQuaternionIdentity());

                for (uint32_t f = 0; f < fc; ++f)
                {
                    const uint32_t sf = (f < srcClip.frameCount) ? f : srcClip.frameCount - 1;
                    const XMVECTOR srcPos = XMLoadFloat3(
                        &srcClip.positions[static_cast<size_t>(shb) * srcClip.frameCount + sf]);
                    XMVECTOR delta = XMVectorSubtract(srcPos, srcRestPos);
                    delta = XMVector3Rotate(delta, sParRot);   // source parent -> world
                    delta = XMVector3Rotate(XMVectorScale(delta, scale), tParInv); // world -> target parent
                    const XMVECTOR outPos = XMVectorAdd(trgRestPos, delta);
                    XMStoreFloat3(&out.positions[static_cast<size_t>(thb) * fc + f], outPos);
                }
            }
        }

        if(opts.extractRootMotion){const int hips=trgSkel.humanoid.Has(HumanoidBone::Hips)?int(trgSkel.humanoid.Bone(HumanoidBone::Hips)):-1;const uint32_t bone=hips>=0?uint32_t(hips):0;Anim::ExtractRootMotion(out.positions,out.rotations,size_t(bone)*fc,fc,out.frameRate,out.duration,out.rootMotion);}
        // (No IK baking: the source is pure FK. The clip carries LegDriveMode::FK,
        // so IKSystem skips the MMD leg CCD per entity while this clip plays; the
        // runtime grant propagates the FK base-leg rotations to the D-bone chain.)

        return out;
    }

    // Up-axis normalization (see the header). Row-vector convention throughout:
    // world = local x parent, so a global rotation G is applied as M' = M x G
    // (and only ROOT locals change; non-root locals are parent-relative).
    bool RetargetBaker::ComputeUpAxisCorrection(const SkeletonAsset& sk, XMFLOAT4X4& outG)
    {
        const HumanoidProfile& hp = sk.humanoid;
        if (!hp.Has(HumanoidBone::Head) || !hp.Has(HumanoidBone::Hips)) return false;

        const XMVECTOR head = BonePosV(sk, hp.Bone(HumanoidBone::Head));
        const XMVECTOR hips = BonePosV(sk, hp.Bone(HumanoidBone::Hips));
        XMVECTOR up = XMVectorSubtract(head, hips);
        if (XMVectorGetX(XMVector3LengthSq(up)) < 1e-8f) return false;
        up = XMVector3Normalize(up);

        const XMVECTOR yAxis = XMVectorSet(0.f, 1.f, 0.f, 0.f);
        const float d = XMVectorGetX(XMVector3Dot(up, yAxis));
        if (d > 0.8f) return false; // already ~Y-up

        XMVECTOR axis = XMVector3Cross(up, yAxis);
        XMMATRIX G;
        if (XMVectorGetX(XMVector3LengthSq(axis)) < 1e-10f)
            G = XMMatrixRotationX(3.14159265f);            // exactly upside-down
        else
        {
            axis = XMVector3Normalize(axis);
            const float ang = std::acos(d < -1.f ? -1.f : (d > 1.f ? 1.f : d));
            G = XMMatrixRotationAxis(axis, ang);           // shortest arc up -> +Y
        }
        XMStoreFloat4x4(&outG, G);
        return true;
    }

    void RetargetBaker::ApplyUpAxisCorrection(SkeletonAsset& sk, const XMFLOAT4X4& correction)
    {
        const XMMATRIX G = XMLoadFloat4x4(&correction);
        for (uint32_t b = 0; b < sk.boneCount; ++b)
        {
            const XMMATRIX bind = XMMatrixMultiply(XMLoadFloat4x4(&sk.bindPose[b]), G);
            XMStoreFloat4x4(&sk.bindPose[b], bind);
            XMStoreFloat4x4(&sk.inverseBindPose[b], XMMatrixInverse(nullptr, bind));
            if (sk.parentIndex[b] < 0)
            {
                XMStoreFloat4x4(&sk.restPoseLocal[b],
                    XMMatrixMultiply(XMLoadFloat4x4(&sk.restPoseLocal[b]), G));
            }
        }
    }

    void RetargetBaker::ApplyUpAxisCorrectionToClip(ClipAsset& clip, const SkeletonAsset& sk,
                                                    const XMFLOAT4X4& correction)
    {
        const XMMATRIX G  = XMLoadFloat4x4(&correction);
        const XMVECTOR qG = XMQuaternionRotationMatrix(G);
        const uint32_t fc = clip.frameCount;
        const uint32_t bones = (std::min)(clip.boneCount, sk.boneCount);
        for (uint32_t b = 0; b < bones; ++b)
        {
            if (sk.parentIndex[b] >= 0) continue; // roots only
            for (uint32_t f = 0; f < fc; ++f)
            {
                const size_t i = static_cast<size_t>(b) * fc + f;
                const XMVECTOR q = XMLoadFloat4(&clip.rotations[i]);
                XMStoreFloat4(&clip.rotations[i],
                              XMQuaternionNormalize(XMQuaternionMultiply(q, qG)));
                const XMVECTOR p = XMLoadFloat3(&clip.positions[i]);
                XMStoreFloat3(&clip.positions[i], XMVector3Rotate(p, qG));
            }
        }
    }

    // ToNamedClipData: baked ClipAsset -> named-channel AnimClipData (see the header).
    AnimClipData RetargetBaker::ToNamedClipData(const ClipAsset&     baked,
                                                const SkeletonAsset& trgSkel,
                                                const char*          clipName)
    {
        AnimClipData out;
        if (clipName) strncpy_s(out.name, clipName, _TRUNCATE);
        out.frameCount   = baked.frameCount;
        out.duration     = baked.duration;
        out.frameRate    = baked.frameRate;
        out.events       = baked.events;
        out.notifyTracks = baked.notifyTracks;out.rootMotion=baked.rootMotion;out.floatCurves=baked.floatCurves;
        out.nextNotifyId = baked.nextNotifyId;
        // Baked keyframes are ABSOLUTE local TRS (never VMD-style offsets).
        out.positionsAreOffsets = false;
        out.rotationsAreOffsets = false;
        out.legDriveFK          = (baked.legDriveMode == LegDriveMode::FK);

        const uint32_t fc = baked.frameCount;
        if (fc == 0) return out;

        // Real bones only: a model's virtual bones are evaluated from the pose every
        // frame - an .ianim channel for one would be noise.
        const uint32_t bones = (std::min)(baked.boneCount, trgSkel.RealBoneCount());
        for (uint32_t b = 0; b < bones; ++b)
        {
            // Decompose the bone's rest local TRS for the static-channel test.
            XMVECTOR s, r, t;
            XMFLOAT3 restPos = { 0,0,0 }, restScl = { 1,1,1 };
            XMFLOAT4 restRot = { 0,0,0,1 };
            if (XMMatrixDecompose(&s, &r, &t, XMLoadFloat4x4(&trgSkel.restPoseLocal[b])))
            {
                XMStoreFloat3(&restPos, t); XMStoreFloat4(&restRot, r); XMStoreFloat3(&restScl, s);
            }

            // Skip channels that never leave the rest pose. The baker writes the
            // decomposed rest verbatim for untouched bones, so a tight epsilon
            // catches them all without risking real motion.
            constexpr float kEps = 1e-6f;
            bool moves = false;
            for (uint32_t f = 0; f < fc && !moves; ++f)
            {
                const size_t i = static_cast<size_t>(b) * fc + f;
                const XMFLOAT3& p = baked.positions[i];
                const XMFLOAT4& q = baked.rotations[i];
                const XMFLOAT3& sc = baked.scales[i];
                moves = std::fabs(p.x - restPos.x) > kEps || std::fabs(p.y - restPos.y) > kEps ||
                        std::fabs(p.z - restPos.z) > kEps ||
                        std::fabs(q.x - restRot.x) > kEps || std::fabs(q.y - restRot.y) > kEps ||
                        std::fabs(q.z - restRot.z) > kEps || std::fabs(q.w - restRot.w) > kEps ||
                        std::fabs(sc.x - restScl.x) > kEps || std::fabs(sc.y - restScl.y) > kEps ||
                        std::fabs(sc.z - restScl.z) > kEps;
            }
            if (!moves) continue;

            AnimClipChannel ch;
            strncpy_s(ch.name, trgSkel.boneNames[b], _TRUNCATE);
            ch.positions.resize(fc);
            ch.rotations.resize(fc);
            ch.scales.resize(fc);
            for (uint32_t f = 0; f < fc; ++f)
            {
                const size_t i = static_cast<size_t>(b) * fc + f;
                ch.positions[f] = baked.positions[i];
                ch.rotations[f] = baked.rotations[i];
                ch.scales[f]    = baked.scales[i];
            }
            out.channels.push_back(std::move(ch));
        }
        return out;
    }
}
