#pragma once

// RetargetBaker - offline bake of a source animation clip onto a target skeleton
// through the canonical humanoid role map (e.g. Mixamo / UE -> PMX).
//
// Bake-first: the output is an ordinary ClipAsset bound to the target skeleton,
// so it plays through the unchanged runtime. All correctness lives in the
// per-bone bind-relative delta (RetargetMath.h); the baker resolves roles and
// walks frames.
//
// Scope:
//   Upper body: spine/chest/neck/head/shoulders/arms/hands/fingers, rotation only.
//   Lower body: Hips + legs + feet, plus root-motion translation (hip-height
//               scaled) on the profile's hips-translation bone (the PMX center
//               bone). Toes are mapped only to reconstruct the foot direction,
//               never retargeted. The FBX/Mixamo source is pure FK and the
//               retarget drives the target's FK/deform chain directly, so every
//               baked clip is marked LegDriveMode::FK and the target's own leg IK
//               does not run on it. Non-retargeted bones keep the target rest pose.

#include "Resource/HumanoidBone.h"

#include <cstdint>

struct SkeletonAsset;
struct ClipAsset;

namespace Resource
{
    struct AnimClipData;
    struct RetargetOptions
    {
        bool upperBody      = true;  // spine/chest/neck/head/shoulders/arms/hands
        bool lowerBody      = false; // Hips + legs + feet (toes are never retargeted). Default off = upper body only.
        bool includeFingers = true;  // fingers are safe FK; include when both rigs have them
        bool extractRootMotion = false; // cumulative horizontal/yaw output, stripped from baked Hips
        bool bakeRootMotion = true;  // Hips translation, scaled by hip-height ratio (needs lowerBody)

        // Reference-pose (T-pose) compensation. A rig authored in A-pose (arms
        // angled down at rest, typical for PMX) would otherwise accumulate the
        // source's arm-down motion on top of its own A-pose rest -> over-bent arms.
        // Marking a rig A-pose straightens its arms to a horizontal T-pose
        // reference before the delta, so both rigs are compared from a common
        // T-pose (Unity "Enforce T-Pose" / UE "Retarget Pose").
        bool sourceAPose = false;
        bool targetAPose = false;

        // Reference-pose compensation for the LEGS (the A-pose correction covers
        // arms only). Rigs disagree on rest leg splay - UE Manny/Quinn thighs point
        // ~2.5-3 deg OUTWARD, MMD/PMX thighs typically ~2-3 deg INWARD - and the
        // bind-relative delta preserves whatever the TARGET has. The ~5-6 deg per-leg
        // mismatch collapses the retargeted stance (knees/ankles ~3x closer together
        // than the source's; shins cross on deep poses). Canonicalizing snaps both
        // rigs' UpperLeg/LowerLeg rest frames to "straight down the body axis" so the
        // source's absolute leg orientation transfers. Off keeps each rig's own leg rest.
        bool canonicalizeLegs = true;

        // Same for the FEET: a heel->toe rest direction does not match across rigs.
        // Mixamo is within ~4 deg of PMX, but UE's SKM_Quinn rest foot pitches 29.4 deg
        // below horizontal vs a PMX rig's 39.5 deg and toes out 7.4 deg (12.4 deg apart
        // in total); the delta preserves that, so every frame comes out with the foot
        // over-pointed. Canonicalizing brings the UE Quinn -> PMX foot direction error
        // vs the source from ~8 deg to ~6 deg.
        bool canonicalizeFeet = true;

        // Neck + head: the rigs' neck rest directions differ enough that the head
        // sits at the wrong angle (9.3 deg off the source -> 2.7 deg canonicalized).
        // Not extended to spine/chest: canonicalizing those makes the chest much WORSE
        // (4.5 -> 16.1 deg), because a rig's spine curvature is real authored posture
        // and the two rigs spread it over a different number of bones.
        bool canonicalizeNeck = true;

        // Spread each limb bone's AXIAL rotation onto the rig's interposed twist
        // bone (MMD arm / wrist twist bones, UE *_twist_*) instead of leaving all of it
        // on the joint. The transfer writes absolute bone orientations, so all the roll
        // the source needs - plus a fixed offset from the reference-pose straightening -
        // lands on the arm / elbow bone as one hard step, while the mesh around the
        // elbow is skinned to the twist sub-bones 1/2/3, which take 25/50/75% of the
        // twist bone through MMD's rotation append so the roll becomes a gradient.
        // Without this the elbow is visibly wrung ("candy wrapper"): on UE Quinn -> PMX
        // every clip carried a constant ~37 deg of axial twist on the elbow (up to
        // 149 deg on combat clips).
        // The split is exact: the twist bone lies on the bone's own axis between it
        // and its child, so the child's world orientation is unchanged.
        bool redistributeTwist = true;
    };

    class RetargetBaker
    {
    public:
        // Bake @p srcClip (already bound to @p srcSkel - per-bone local TRS) onto
        // @p trgSkel. Both skeletons must carry a valid HumanoidProfile. Returns a
        // ClipAsset with boneCount == trgSkel.boneCount, frameCount ==
        // srcClip.frameCount, ready to register in a ClipLibrary. Bones outside the
        // retarget set (and roles missing on either rig) stay at the target rest pose.
        static ClipAsset BakeClip(const SkeletonAsset&   srcSkel,
                                  const ClipAsset&       srcClip,
                                  const SkeletonAsset&   trgSkel,
                                  const RetargetOptions& opts = {});

        // Which canonical roles are retargeted under the given options.
        static bool IsRetargetRole(HumanoidBone role, const RetargetOptions& opts);

        // ---- Up-axis normalization ----
        // UE exports FBX Z-up; Assimp's ConvertToLeftHanded flips handedness but NOT
        // the up axis, so UE skeletons arrive lying down (body up = -Z). The delta
        // math's fixed world reference axes need both rigs +Y up, so Z-up sources are
        // rotated upright once at load, before role mapping.
        // ComputeUpAxisCorrection derives body-up from the humanoid profile (Head -
        // Hips bind positions); returns false when the rig is already ~Y-up (or has no
        // valid profile), else @p outG = the global rotation that uprights the model
        // (e.g. RotX(+90) for -Z-up).
        static bool ComputeUpAxisCorrection(const SkeletonAsset& sk, DirectX::XMFLOAT4X4& outG);
        // Applies correction to the skeleton (bind/inverseBind all bones, root restPoseLocal).
        // Caller must re-run HumanoidProfileBuilder::FinalizeDerived afterwards.
        static void ApplyUpAxisCorrection(SkeletonAsset& sk, const DirectX::XMFLOAT4X4& correction);
        // Applies correction to a clip bound to @p sk: only ROOT-bone channels change
        // (non-root locals are parent-relative and unaffected by a global rotation).
        static void ApplyUpAxisCorrectionToClip(ClipAsset& clip, const SkeletonAsset& sk,
                                                const DirectX::XMFLOAT4X4& correction);

        // Convert a baked, target-bound ClipAsset into a named-channel
        // AnimClipData (channel names = target skeleton bone names) so it can be
        // persisted as an ordinary .ianim via SerializeAnimation and later
        // re-bound with BindToSkeleton (names match by construction). Channels
        // whose every frame equals the target rest pose are omitted
        // (BindToSkeleton restores rest for unkeyed bones), which drops most of a
        // PMX rig's 300+ untouched bones. Sets legDriveFK so the FK flag round-trips.
        static AnimClipData ToNamedClipData(const ClipAsset&     baked,
                                            const SkeletonAsset& trgSkel,
                                            const char*          clipName);
    };
}
