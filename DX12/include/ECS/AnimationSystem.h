#pragma once
#include "Anim/Core/PoseOps.h"

// The CPU animation chain:
//   AnimationSystem    samples clip keyframes into per-entity LocalPose scratch.
//   LocalToWorldSystem accumulates local transforms root-first and writes skin matrices
//                      (world x inverseBindPose) into PoseRingBuffer.
//   SkinMatrixSystem   allocates per-mesh output slices in SkinnedVertexRing.
//
// Call order each frame (Renderer::TickAnimationChain, before GPU submission):
//   1. AnimationSystem::Update(world, clocks)   (then IK, post-IK grants, chain physics, ...)
//   2. LocalToWorldSystem::Update(world, poseRing)
//   3. SkinMatrixSystem::Update(world, poseRing, vertRing)
// After step 3 each skinned mesh's SkinningOutputComponent holds the byte offsets
// SkinnedMeshSubsystem::BuildSkinJobs needs (skin dispatch + MeshDescriptor patch).

#include "ECS/ECS.h"
#include "ECS/AnimationComponents.h"
#include "Resource/SkeletonAsset.h"
#include "Graphics/SkinningBuffers.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <DirectXMath.h>

namespace Anim { struct IAnimPoseSource; }
namespace GameplayTime { struct Clocks; }
struct LocomotionComponent;
struct AnimLayerComponent;

// ==== AnimationSystem ====
// Reads:  AnimationComponent, SkeletonComponent
// Writes: internal LocalPose scratch (read by LocalToWorldSystem)
class AnimationSystem
{
public:
    using LocalPose = Anim::LocalPose;

    void SetPoseSource(Anim::IAnimPoseSource* source) { m_poseSource = source; }

    explicit AnimationSystem(SkeletonRegistry& skeletons,
                             ClipLibrary&       clips,
                             MorphClipLibrary&  morphClips)
        : m_skeletons(skeletons), m_clips(clips), m_morphClips(morphClips) {}

    // Advance all animations and sample keyframes into m_localPoseCache. Applies pre-IK
    // grants (e.g. the MMD waist-cancel bone). Must be called before IKSystem. Each entity's
    // clocks advance by its own dt (GameplayTime::EntityDt of @p clocks). If @p activeSet is not
    // null, entities outside it still advance their clocks (attack clocks, cross-fades, notify
    // times are gameplay state) but are not sampled (the morph-only pass, for entities with no
    // animator clip, ignores it).
    void Update(World& world, const GameplayTime::Clocks& clocks,
                const std::unordered_set<Entity>* activeSet = nullptr);

    // Apply post-IK grants (D-bones that copy IK-solved rotations).
    // Must be called AFTER IKSystem::Update() and before LocalToWorldSystem.
    void ApplyPostIKGrants(World& world);

    // An editor preview's temporary pose (PreviewPoseOverrideComponent): each entry's rotation
    // applied in the bone's own frame on top of the final local pose. Runs AFTER chain physics,
    // BEFORE LocalToWorldSystem, so the bone turns by exactly what was asked. No component in
    // the World = one pool lookup.
    void ApplyPreviewPoseOverrides(World& world);

    // The procedural-write journal, shared with the preview overrides above: a system that
    // composes a rotation INTO the persistent local cache records the bone's rotation first, and
    // the start of the next Update puts every recorded rotation back, newest first - so a
    // skeleton with no pose source starts each frame from its un-posed pose instead of
    // compounding its own output. Serial callers only (the animation chain's serial steps).
    // Returns false for a bone the cache does not hold; the caller must not write it then.
    bool JournalBoneRotation(World& world, Entity e, uint32_t bone);

    // Returns the LocalPose array for entity e (valid until the next Update() or
    // ClearLocalPoseCache). Returns nullptr while no Update has posed e (no SkeletonComponent
    // with an asset, never in the active set, or its cache cleared).
    const LocalPose* GetLocalPose(Entity e) const;

    // Mutable accessor for the pose writers that run after Update (IK, chain physics, head and
    // face tracking); one that composes onto the cache needs JournalBoneRotation or the base-pose
    // predicates below.
    LocalPose* GetMutableLocalPose(Entity e);

    // Drop CPU pose scratch so the next Update rebuilds every skeleton from
    // its current clip/rest pose. Play-mode Stop uses this after chain physics
    // has written rotations into the cache; otherwise an unanimated skeleton
    // could seed the next Play session from the previous simulation result.
    void ClearLocalPoseCache();
    // The single-entity form, for ChainPhysicsSystem::ResetPreviewEntity (the whole-cache form
    // above would reset every character in the scene). Serial callers only (never while
    // Update's workers hold the slot); the next Update re-seeds @p e.
    void ClearLocalPoseCache(Entity e);

    // ---- Base-pose source predicates ----
    // "Does something re-establish this entity's LocalPose from authored data EVERY frame?"
    // Four sources do: an IAnimPoseSource that owns the entity (SetPoseSource), the primary clip,
    // locomotion, and an active AnimLayer. When none applies, Update seeds the pose once and leaves
    // it alone (the kInvalidAnimHandle - 1u sentinel in the rest-pose fallback pass), so any system
    // that WRITES poses must re-stamp its own clean base (ChainPhysicsSystem, HeadTrackSystem) or
    // journal each write (JournalBoneRotation, FaceTrackingSystem), or it composes onto its
    // previous output and drifts.
    // Update's pass-1 gate and pass-3 skip use these, and so do ChainPhysicsSystem's resetRot gate,
    // HeadTrackSystem, IKSystem and IKRigSystem; keep the test here, on the system that owns the
    // rule - a private copy goes stale.
    // The pooled overloads take already-fetched component pointers so the hot per-entity passes
    // keep using pool walks; the World overload is the convenience form for outside callers.
    static bool LocoIsPoseSource(const LocomotionComponent*      loco,
                                 const AnimPoseOverrideComponent* poseOverride);
    bool LayerIsPoseSource(const AnimLayerComponent* layers) const;
    bool HasLivePoseSource(World& world, Entity e) const;
    // The ClipLibrary clip at @p index; nullptr when out of range.
    const ClipAsset* GetClip(uint32_t index) const { return index<m_clips.Count()?&m_clips.Get(index):nullptr; }

    // An AnimLayer's per-bone weights (1 in the mask, 0 out) for @p mode (BoneMaskMode):
    // UpperBody = the humanoid Spine role's subtree, LowerBody = the rest, CustomRoot =
    // @p customRoot (a bone name, exact) and its subtree, FullBody = every bone. False = the seed
    // did not resolve and @p w is all 1 (the full-body fallback the layer then plays). Public for
    // the Skeleton Editor's mask preview and Lua Skeleton.BoneMask.
    static bool BuildBoneMask(const SkeletonAsset& sk, uint8_t mode, const std::string& customRoot,
                              std::vector<float>& w);

    // Whether the virtual bones are posed each frame. Starts as !-vb_off; switchable at runtime
    // so their cost is measured in ONE process (Lua AnimStats.SetVirtualBones,
    // tools/bench_virtual_bones.py). Off: each virtual bone is written at its rest local every
    // frame (SkeletonAsset::VirtualBoneDef::restPos/Rot/Scl), not left at its last pose.
    static void SetVirtualBonesEnabled(bool on);
    static bool VirtualBonesEnabled();

private:
    // One m_overrideRestore entry: a bone's rotation from before an override or journaled write
    // (the restore contract is on m_overrideRestore).
    struct OverrideRestore
    {
        Entity            e = NullEntity;
        uint32_t          generation = 0;   // World::GetGeneration - a recycled id is not restored
        uint32_t          bone = 0;
        DirectX::XMFLOAT4 rot{ 0.f, 0.f, 0.f, 1.f };
    };

    void SampleClip(const ClipAsset& clip, float time,
                    uint32_t boneCount, LocalPose* out, bool looping = true) const;

    // @p looping: wrap the last frame onto the first; otherwise clamp to the clip, so its end
    // holds the last frame.
    void SampleMorphClip(const MorphClipAsset& mc, float time, bool looping,
                         MorphComponent* out) const;

    void EnsureCacheSize(Entity e);

    // First thing in Update: puts every m_overrideRestore rotation back, newest first, and empties
    // the list. An entry whose entity died or was recycled drops that id's cached pose instead.
    void RestorePreviewPoseOverrides(World& world);

    Anim::IAnimPoseSource* m_poseSource = nullptr;

    SkeletonRegistry& m_skeletons;
    ClipLibrary&      m_clips;
    MorphClipLibrary& m_morphClips;

    // Per-entity scratch LocalPose storage, a flat array indexed by entity id: no hash lookups,
    // and parallel workers write disjoint slots.
    std::vector<std::vector<LocalPose>> m_localPoseCache; // [entityID] -> LocalPose[]
    std::vector<uint32_t>               m_lastClipCache;  // [entityID] -> last primaryClip

    // What ApplyPreviewPoseOverrides (and JournalBoneRotation, e.g. from
    // FaceTrackingSystem::ApplyPose) changed, put back at the start of the next Update. The cache
    // is PERSISTENT (a skeleton with no pose source is stamped to rest once), so an override
    // composed into it in place would compound every frame (a 10 deg turn spins at 600 deg/s).
    // Restored, the next frame starts from the un-posed pose; between frames the cache shows the
    // final one (the editor's Live local reads it).
    std::vector<OverrideRestore> m_overrideRestore;
};

// ==== LocalToWorldSystem ====
// Reads:  SkeletonComponent, AnimationSystem local poses, SkeletonAsset parents /
//         inverseBindPose
// Writes: PoseRingBuffer (skin matrices, one float4x4 per bone)
//         SkeletonComponent.poseByteOffset (~0u = no pose this frame)
class LocalToWorldSystem
{
public:
    explicit LocalToWorldSystem(AnimationSystem& animSys, SkeletonRegistry& skeletons)
        : m_animSys(animSys), m_skeletons(skeletons) {}

    // Accumulate each skeleton's local poses root-first and write its skin matrices and
    // SkeletonComponent::poseByteOffset. Must be called after AnimationSystem::Update().
    void Update(World& world, PoseRingBuffer& poseBuffer,
                const std::unordered_set<Entity>* activeSet = nullptr);

private:
    AnimationSystem&  m_animSys;
    SkeletonRegistry& m_skeletons;
};

// ==== SkinMatrixSystem ====
// Propagates the skeleton's poseByteOffset to each skinned mesh and allocates its
// SkinnedVertexRing output slices. LocalToWorldSystem already fused the skin matrices, so
// the PoseRingBuffer parameter is unused (unnamed at the definition).
// Writes: SkinningOutputComponent.poseByteOffset / outPos / outNrm / outTanByteOffset
class SkinMatrixSystem
{
public:
    explicit SkinMatrixSystem(SkeletonRegistry& skeletons)
        : m_skeletons(skeletons) {}

    // Propagate pose validity and allocate per-mesh output slices.
    // Must be called after LocalToWorldSystem::Update().
    void Update(World& world, PoseRingBuffer& poseBuffer, SkinnedVertexRing& vertRing);

private:
    SkeletonRegistry& m_skeletons;
};
