#pragma once

// HumanoidBone / HumanoidProfile - the canonical humanoid role layer for skeleton
// retargeting (PMX <-> Unity Humanoid / Unreal / VRM / Mixamo). Design:
// docs/design/RETARGET_ARCHITECTURE.md.
//
// Bones stay opaque uint32 indices in SkeletonAsset. A HumanoidProfile is a small
// POD table mapping each canonical role (HumanoidBone) to a bone index on THAT
// skeleton (-1 if absent), plus the oriented rest rotation the retarget math needs
// (PMX bind is translation-only, so HumanoidProfileBuilder synthesizes orientation
// from child positions).
//
// Role NAMES mirror the VRM 1.0 humanoid vocabulary, so the VrmImporter's humanoid
// map drops in untranslated. Role ORDINALS are serialized ABI (.iskel is indexed by
// role): APPEND ONLY, never reorder or remove.

#include <cstdint>
#include <type_traits>
#include <DirectXMath.h>

// ==== Canonical humanoid roles (55) ====
// 15 are REQUIRED (a valid humanoid maps all of them): Hips, Spine, Head, and the
// left / right UpperLeg, LowerLeg, Foot, UpperArm, LowerArm, Hand. The rest are
// optional.
enum class HumanoidBone : uint8_t
{
    // --- Torso / head (0..8) ---
    Hips = 0,          // REQUIRED
    Spine,             // REQUIRED
    Chest,             // optional
    UpperChest,        // optional
    Neck,              // optional
    Head,              // REQUIRED
    LeftEye,           // optional
    RightEye,          // optional
    Jaw,               // optional

    // --- Legs (9..16) ---
    LeftUpperLeg,      // REQUIRED
    LeftLowerLeg,      // REQUIRED
    LeftFoot,          // REQUIRED
    LeftToes,          // optional
    RightUpperLeg,     // REQUIRED
    RightLowerLeg,     // REQUIRED
    RightFoot,         // REQUIRED
    RightToes,         // optional

    // --- Arms (17..24) ---
    LeftShoulder,      // optional
    LeftUpperArm,      // REQUIRED
    LeftLowerArm,      // REQUIRED
    LeftHand,          // REQUIRED
    RightShoulder,     // optional
    RightUpperArm,     // REQUIRED
    RightLowerArm,     // REQUIRED
    RightHand,         // REQUIRED

    // --- Left fingers (25..39); the thumb uses VRM 1.0 Metacarpal / Proximal / Distal ---
    LeftThumbMetacarpal, LeftThumbProximal, LeftThumbDistal,
    LeftIndexProximal,   LeftIndexIntermediate,  LeftIndexDistal,
    LeftMiddleProximal,  LeftMiddleIntermediate, LeftMiddleDistal,
    LeftRingProximal,    LeftRingIntermediate,   LeftRingDistal,
    LeftLittleProximal,  LeftLittleIntermediate, LeftLittleDistal,

    // --- Right fingers (40..54) ---
    RightThumbMetacarpal, RightThumbProximal, RightThumbDistal,
    RightIndexProximal,   RightIndexIntermediate,  RightIndexDistal,
    RightMiddleProximal,  RightMiddleIntermediate, RightMiddleDistal,
    RightRingProximal,    RightRingIntermediate,   RightRingDistal,
    RightLittleProximal,  RightLittleIntermediate, RightLittleDistal,

    Count // = 55; sentinel, not a real role
};

static constexpr uint32_t kHumanoidBoneCount = static_cast<uint32_t>(HumanoidBone::Count);
static_assert(kHumanoidBoneCount == 55, "HumanoidBone count drifted — .iskel ABI change; bump handling");

// The 15 required roles. A skeleton missing any of these cannot be a retarget
// source or target (HumanoidProfile::valid stays 0).
inline bool IsRequiredHumanoidBone(HumanoidBone b)
{
    switch (b)
    {
    case HumanoidBone::Hips:
    case HumanoidBone::Spine:
    case HumanoidBone::Head:
    case HumanoidBone::LeftUpperLeg:  case HumanoidBone::LeftLowerLeg:  case HumanoidBone::LeftFoot:
    case HumanoidBone::RightUpperLeg: case HumanoidBone::RightLowerLeg: case HumanoidBone::RightFoot:
    case HumanoidBone::LeftUpperArm:  case HumanoidBone::LeftLowerArm:  case HumanoidBone::LeftHand:
    case HumanoidBone::RightUpperArm: case HumanoidBone::RightLowerArm: case HumanoidBone::RightHand:
        return true;
    default:
        return false;
    }
}
static constexpr uint32_t kRequiredHumanoidBoneCount = 15;

// Canonical (VRM 1.0) role name, for the VRM map join, the inspector and logging.
// Returns "" for HumanoidBone::Count / out of range.
inline const char* HumanoidRoleName(HumanoidBone b)
{
    switch (b)
    {
    case HumanoidBone::Hips:        return "hips";
    case HumanoidBone::Spine:       return "spine";
    case HumanoidBone::Chest:       return "chest";
    case HumanoidBone::UpperChest:  return "upperChest";
    case HumanoidBone::Neck:        return "neck";
    case HumanoidBone::Head:        return "head";
    case HumanoidBone::LeftEye:     return "leftEye";
    case HumanoidBone::RightEye:    return "rightEye";
    case HumanoidBone::Jaw:         return "jaw";
    case HumanoidBone::LeftUpperLeg:  return "leftUpperLeg";
    case HumanoidBone::LeftLowerLeg:  return "leftLowerLeg";
    case HumanoidBone::LeftFoot:      return "leftFoot";
    case HumanoidBone::LeftToes:      return "leftToes";
    case HumanoidBone::RightUpperLeg: return "rightUpperLeg";
    case HumanoidBone::RightLowerLeg: return "rightLowerLeg";
    case HumanoidBone::RightFoot:     return "rightFoot";
    case HumanoidBone::RightToes:     return "rightToes";
    case HumanoidBone::LeftShoulder:  return "leftShoulder";
    case HumanoidBone::LeftUpperArm:  return "leftUpperArm";
    case HumanoidBone::LeftLowerArm:  return "leftLowerArm";
    case HumanoidBone::LeftHand:      return "leftHand";
    case HumanoidBone::RightShoulder: return "rightShoulder";
    case HumanoidBone::RightUpperArm: return "rightUpperArm";
    case HumanoidBone::RightLowerArm: return "rightLowerArm";
    case HumanoidBone::RightHand:     return "rightHand";
    case HumanoidBone::LeftThumbMetacarpal:   return "leftThumbMetacarpal";
    case HumanoidBone::LeftThumbProximal:     return "leftThumbProximal";
    case HumanoidBone::LeftThumbDistal:       return "leftThumbDistal";
    case HumanoidBone::LeftIndexProximal:     return "leftIndexProximal";
    case HumanoidBone::LeftIndexIntermediate: return "leftIndexIntermediate";
    case HumanoidBone::LeftIndexDistal:       return "leftIndexDistal";
    case HumanoidBone::LeftMiddleProximal:     return "leftMiddleProximal";
    case HumanoidBone::LeftMiddleIntermediate: return "leftMiddleIntermediate";
    case HumanoidBone::LeftMiddleDistal:       return "leftMiddleDistal";
    case HumanoidBone::LeftRingProximal:     return "leftRingProximal";
    case HumanoidBone::LeftRingIntermediate: return "leftRingIntermediate";
    case HumanoidBone::LeftRingDistal:       return "leftRingDistal";
    case HumanoidBone::LeftLittleProximal:     return "leftLittleProximal";
    case HumanoidBone::LeftLittleIntermediate: return "leftLittleIntermediate";
    case HumanoidBone::LeftLittleDistal:       return "leftLittleDistal";
    case HumanoidBone::RightThumbMetacarpal:   return "rightThumbMetacarpal";
    case HumanoidBone::RightThumbProximal:     return "rightThumbProximal";
    case HumanoidBone::RightThumbDistal:       return "rightThumbDistal";
    case HumanoidBone::RightIndexProximal:     return "rightIndexProximal";
    case HumanoidBone::RightIndexIntermediate: return "rightIndexIntermediate";
    case HumanoidBone::RightIndexDistal:       return "rightIndexDistal";
    case HumanoidBone::RightMiddleProximal:     return "rightMiddleProximal";
    case HumanoidBone::RightMiddleIntermediate: return "rightMiddleIntermediate";
    case HumanoidBone::RightMiddleDistal:       return "rightMiddleDistal";
    case HumanoidBone::RightRingProximal:     return "rightRingProximal";
    case HumanoidBone::RightRingIntermediate: return "rightRingIntermediate";
    case HumanoidBone::RightRingDistal:       return "rightRingDistal";
    case HumanoidBone::RightLittleProximal:     return "rightLittleProximal";
    case HumanoidBone::RightLittleIntermediate: return "rightLittleIntermediate";
    case HumanoidBone::RightLittleDistal:       return "rightLittleDistal";
    default: return "";
    }
}

// Detected source rig convention (also the .iskel-serialized value).
enum class HumanoidConvention : uint8_t
{
    Unknown = 0,
    PmxJp,           // MMD PMX Japanese semi-standard names
    Mixamo,          // "mixamorig:" prefixed
    UnrealMannequin, // pelvis / spine_01 / thigh_l ...
    GenericEnglish,  // Unity / hand-named FBX
    Vrm0,            // VRM 0.x humanoid map
    Vrm1,            // VRM 1.0 humanoid map
};

// ==== HumanoidProfile ====
// Per-skeleton role table on SkeletonAsset, built at import by
// HumanoidProfileBuilder. POD, fixed-size, blob-serialized verbatim into the
// .iskel tail (gated by ISKEL_FLAG_HUMANOID).
struct HumanoidProfile
{
    // Role -> bone index on this skeleton. -1 = role absent.
    int32_t boneForRole[kHumanoidBoneCount];

    // Hips TRANSLATION bone (root motion) when it differs from the Hips ROTATION bone.
    // MMD has no single Hips: pelvis rotation goes to the lower-body bone
    // (= boneForRole[Hips]) while root translation lives on the center bone.
    // -1 = same as boneForRole[Hips] (Unity / VRM / Mixamo single-Hips rigs).
    int32_t hipsTranslationBone;

    // Oriented rest rotation (model space) of each mapped role bone; identity for
    // absent roles. The bind orientation the bind-relative delta retarget consumes -
    // PMX stores none, so it is synthesized from child-bone directions at build time.
    DirectX::XMFLOAT4 restModelRot[kHumanoidBoneCount];

    float   hipHeight;   // rest-pose hip-joint height in model space (thigh roots, else Hips; translation scale ref)
    uint8_t convention;  // HumanoidConvention
    uint8_t valid;       // 1 if all 15 required roles are mapped
    uint8_t userEdited;  // 1 = mapping hand-tuned in the editor; auto-detect must not clobber it
    uint8_t _pad[1];

    HumanoidProfile() { Reset(); }

    void Reset()
    {
        for (uint32_t i = 0; i < kHumanoidBoneCount; ++i)
        {
            boneForRole[i]  = -1;
            restModelRot[i] = DirectX::XMFLOAT4(0.f, 0.f, 0.f, 1.f);
        }
        hipsTranslationBone = -1;
        hipHeight  = 0.f;
        convention = static_cast<uint8_t>(HumanoidConvention::Unknown);
        valid      = 0;
        userEdited = 0;
        _pad[0]    = 0;
    }

    int32_t Bone(HumanoidBone r) const { return boneForRole[static_cast<uint32_t>(r)]; }
    bool    Has(HumanoidBone r)  const { return boneForRole[static_cast<uint32_t>(r)] >= 0; }
    void    Set(HumanoidBone r, int32_t boneIdx) { boneForRole[static_cast<uint32_t>(r)] = boneIdx; }
    bool    IsValid() const { return valid != 0; }
};

// Blob-copied into .iskel: must stay trivially copyable.
static_assert(std::is_trivially_copyable_v<HumanoidProfile>,
              "HumanoidProfile must be trivially copyable (blob-serialized into .iskel)");
