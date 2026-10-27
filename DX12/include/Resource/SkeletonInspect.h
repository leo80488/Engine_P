#pragma once
// Resource/SkeletonInspect.h - what a skeleton IS, as data:
//   * per bone: its categories (IK controller / effector / link, grant source / target, twist,
//     weighted, unweighted leaf, chain physics by name, humanoid role, mirrored or non-uniform
//     rest), depth, children, how many vertices it skins;
//   * the skeleton's statistics;
//   * the problems a skeleton can carry that the loader and the runtime log once or never: a
//     child listed before its parent (LocalToWorld's forward pass reads a parent it has not
//     computed), two names one clip channel binds both of (clips bind by Resource::BoneNameHash),
//     a skin weight naming a bone that does not exist, a bone deeper than the 64-ancestor walks.
// No ImGui, no renderer, no world: the Skeleton Editor's tree and
// `Game.exe -assettool=skeleton_report` read the same report, so the CLI is the test oracle for
// the UI.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct SkeletonAsset;
struct BlendVertex;

namespace Resource
{
    // Per-bone categories. Bits are stable: the JSON uses their NAMES
    // (SkeletonBoneFlagNames), never the values.
    enum SkeletonBoneFlag : uint32_t
    {
        kSkelBoneRoot           = 1u << 0,   // parentIndex < 0
        kSkelBoneLeaf           = 1u << 1,   // no children
        kSkelBoneIKBone         = 1u << 2,   // an ikChains[].ikBoneIndex (the controller: the leg IK bone)
        kSkelBoneIKTarget       = 1u << 3,   // an ikChains[].targetBoneIndex (the effector: the ankle)
        kSkelBoneIKLink         = 1u << 4,   // a link the CCD rotates
        kSkelBoneGrantSource    = 1u << 5,   // another bone copies this one's rotation (PMX grant)
        kSkelBoneGrantTarget    = 1u << 6,   // copies another bone's rotation (MMD D bones)
        kSkelBoneTwist          = 1u << 7,   // named as a twist bone (the PMX twist kanji / "twist")
        kSkelBoneWeighted       = 1u << 8,   // some vertex has a non-zero weight on it (weights known)
        kSkelBoneDummyLeaf      = 1u << 9,   // an unskinned leaf (PMX sinker / tip bones, "_end") (weights known)
        kSkelBoneChainName      = 1u << 10,  // ChainPhysicsNames::ClassifyBone != 0
        kSkelBoneChainAttach    = 1u << 11,  // ChainPhysicsNames::TriggersAutoAttach
        kSkelBoneHumanoid       = 1u << 12,  // mapped to a humanoid role
        kSkelBoneMirrored       = 1u << 13,  // rest local has a negative determinant
        kSkelBoneNonUniform     = 1u << 14,  // rest local scale is not uniform
        kSkelBoneVirtual        = 1u << 15,  // an appended virtual bone (SkeletonAsset::realBoneCount)
    };

    struct SkeletonBoneInfo
    {
        uint32_t flags          = 0;
        int32_t  parent         = -1;
        uint16_t depth          = 0;     // ancestors (root = 0); cycles stop at boneCount
        uint16_t childCount     = 0;
        uint32_t weightedVerts  = 0;     // vertices with a non-zero weight on this bone
        float    maxWeight      = 0.f;   // 0..1
        int16_t  humanoidRole   = -1;    // HumanoidBone, -1 = none
        int32_t  grantSource    = -1;    // SkeletonAsset::grantSource
        float    grantRatio     = 0.f;
        float    length         = 0.f;   // bind-space distance to the parent (model units)
    };

    enum class SkeletonIssueKind : uint8_t
    {
        BadParent,           // parentIndex out of range, or the bone itself
        OrderViolation,      // parentIndex > own index: LocalToWorld's forward pass reads it uncomputed
        EmptyName,
        DuplicateName,       // the same bytes twice: FollowSocket/HeadTrack find the first, a clip channel drives both
        NameHashCollision,   // different names, one BoneNameHash: a clip channel binds the wrong bone
        NearDuplicateName,   // equal ignoring ASCII case: clips bind case-sensitively
        MirroredRest,        // negative determinant in the rest local
        BindRestMismatch,    // FK of restPoseLocal is not bindPose (the reference pose moves the joint)
        BadGrant,            // grantSource out of range or the bone itself
        BadIKIndex,          // an ikChains index out of range
        DepthOver64,         // the IK / HeadTrack model-space walks stop at 64 ancestors
        SkinIndexOutOfRange, // a skin weight names a bone the skeleton does not have
        HumanoidIncomplete,  // the humanoid profile misses a required role (retarget refuses)
        ForkWithDummyLeaf,   // a chain-physics bone forks into an unweighted leaf and a real child
        Count
    };

    enum class SkeletonIssueSeverity : uint8_t { Info, Warning, Error };

    struct SkeletonIssue
    {
        SkeletonIssueKind     kind     = SkeletonIssueKind::Count;
        SkeletonIssueSeverity severity = SkeletonIssueSeverity::Info;
        int32_t               bone     = -1;   // the bone it is about (-1 = the skeleton)
        int32_t               other    = -1;   // the second bone when there is one
        std::string           detail;
    };

    struct SkeletonStats
    {
        uint32_t bones = 0, roots = 0, leaves = 0, maxDepth = 0;
        uint32_t ikChains = 0, ikLinks = 0;
        uint32_t grantTargets = 0, twist = 0;
        bool     weightsKnown = false;
        uint32_t skinStreams = 0, skinVertices = 0, maxInfluences = 0;
        uint32_t weighted = 0, unweighted = 0, dummyLeaves = 0;
        uint32_t chainName = 0;
        bool     chainAutoAttach = false;   // loading this skeleton auto-adds a ChainPhysicsComponent
        uint32_t humanoidMapped = 0;
        bool     humanoidValid = false;
        float    modelSize = 0.f;           // diagonal of the bind joints' box (model units)
    };

    struct SkeletonReport
    {
        std::vector<SkeletonBoneInfo> bones;
        std::vector<SkeletonIssue>    issues;
        SkeletonStats                 stats;

        uint32_t Count(SkeletonIssueSeverity s) const;
        uint32_t Count(SkeletonIssueKind k) const;
    };

    // One skinned mesh's per-vertex influences (the .iskel's blend streams).
    struct SkinWeightStream
    {
        const BlendVertex* data  = nullptr;
        std::size_t        count = 0;
    };

    // @p weights empty = unknown: the weighted / dummy-leaf categories and the
    // skin checks are skipped (stats.weightsKnown false), never guessed.
    SkeletonReport InspectSkeleton(const SkeletonAsset& skel, const std::vector<SkinWeightStream>& weights);

    // The humanoid profile as the runtime would use it - AnimationSystem's lazy build's
    // rule: the file's when it is valid or user-edited (a user-edited empty one stays
    // empty), else built the way that build (and the retarget window) builds it.
    // Returns "file", "built" or "none" (nothing mapped).
    const char* EnsureHumanoidProfile(SkeletonAsset& skel);

    const char* SkeletonIssueKindName(SkeletonIssueKind k);            // "order_violation", ...
    const char* SkeletonIssueSeverityName(SkeletonIssueSeverity s);    // "info" | "warning" | "error"
    SkeletonIssueSeverity SkeletonIssueDefaultSeverity(SkeletonIssueKind k);
    // The names of the set bits, lowest first ("root", "leaf", "ik_link", ...).
    std::vector<const char*> SkeletonBoneFlagNames(uint32_t flags);

    // One bone clip against a skeleton, by playback's own binding (AnimClipData::ChannelBinding: a
    // channel drives the bone of EXACTLY its name - FNV-32, case-sensitive). A clip none of whose
    // channels names a bone binds anyway and plays the rest pose, silently. `-assettool=compat` and
    // the Skeleton Editor's animation list both report this.
    struct AnimClipData;
    struct ClipFit
    {
        std::string              clip;
        uint32_t                 channels = 0, boundBones = 0, bones = 0, frames = 0;
        float                    duration = 0.f;
        std::vector<std::string> unmatched;          // the first 32 tracks no bone takes
        std::size_t              unmatchedCount = 0;
        std::vector<std::string> duplicates;         // track names given twice
        bool Fits() const { return boundBones > 0; }  // else it plays the rest pose
    };
    ClipFit FitClip(const AnimClipData& clip, const SkeletonAsset& skel);
}
