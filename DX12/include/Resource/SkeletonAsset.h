#pragma once
#include "Anim/Core/ClipTracks.h"

// SkeletonAsset  - bone hierarchy + bind pose (immutable after load, shared across entities)
// ClipAsset      - animation keyframes, SOA (immutable after import)
// MorphClipAsset - morph-weight animation clip, SOA
// BlendVertex    - per-vertex bone influences packed in 24 bytes
// SkeletonRegistry / ClipLibrary / MorphClipLibrary - global registries addressed by uint32_t index
//
// DESIGN RULE: bones are NEVER ECS entities; they are uint32_t indices inside SkeletonAsset,
// stored root-first (parentIndex[i] < i for all i > 0) so every system traverses them in one
// dependency-free pass.

#include "Anim/IK/IKEnableTrack.h"
#include "ECS/NotifyTypes.h"  // NotifyTrack - clip-authored AnimNotify data (Unreal-style)
#include "Resource/HumanoidBone.h"  // HumanoidProfile - canonical retarget role map

// kMaxMorphTargets / ENGINE_MAX_MORPH_TARGETS - the morph-target cap, defined ONCE and shared
// with Skin.cs.hlsl; read that file before changing the number. The path is relative because the
// shared define must live under shaders/ (package_game.py bundles it for runtime DXC compiles)
// and no /I reaches it.
#include "../../shaders/morph_limits.hlsli"

#include "System/Check.h"

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <DirectXMath.h>

static constexpr uint32_t kInvalidSkeletonIndex = ~0u;
static constexpr uint32_t kInvalidClipIndex     = ~0u;

// BlendVertex - 24 bytes per mesh vertex, up to 8 influences: boneIndices[i] = 0-based bone index
// in SkeletonAsset (uint16), boneWeights[i] = normalised weight, 255 = 1.0.
static constexpr uint32_t MAX_BONES_PER_VERTEX = 8;

struct BlendVertex
{
    uint16_t boneIndices[MAX_BONES_PER_VERTEX]; // 16 bytes
    uint8_t  boneWeights[MAX_BONES_PER_VERTEX]; // 8 bytes
};
static_assert(sizeof(BlendVertex) == 24, "BlendVertex must be exactly 24 bytes");

// SkeletonAsset - load-time data, immutable after load, shared by every entity using the
// skeleton; stored in SkeletonRegistry by index.
struct SkeletonAsset
{
    static constexpr uint32_t MAX_BONES = 2048;

    struct VirtualBoneDef
    {
        uint32_t index = 0;   // its bone
        uint32_t from  = 0;   // its parent (a real bone)
        uint32_t to    = 0;   // the real bone whose model transform it takes
        // Its REST local pose (restPoseLocal[index] decomposed at load): what it is while
        // not evaluated (-vb_off, AnimationSystem::SetVirtualBonesEnabled(false)) and in a
        // frame whose `from` matrix cannot be inverted (a bone animated to zero scale).
        DirectX::XMFLOAT3 restPos{ 0.f, 0.f, 0.f };
        DirectX::XMFLOAT4 restRot{ 0.f, 0.f, 0.f, 1.f };
        DirectX::XMFLOAT3 restScl{ 1.f, 1.f, 1.f };
    };

    struct BoneAABB
    {
        DirectX::XMFLOAT3 localMin = {  1e30f,  1e30f,  1e30f };
        DirectX::XMFLOAT3 localMax = { -1e30f, -1e30f, -1e30f };
    };

    struct IKLink
    {
        uint32_t            boneIndex      = 0;
        bool                hasAngleLimit  = false;
        DirectX::XMFLOAT3  minAngle       = { 0.f, 0.f, 0.f }; // radians
        DirectX::XMFLOAT3  maxAngle       = { 0.f, 0.f, 0.f }; // radians
    };
    struct IKChain
    {
        uint32_t            ikBoneIndex     = 0;   // the IK bone (e.g. the left leg IK)
        uint32_t            targetBoneIndex = 0;   // the effector (e.g. the left ankle)
        uint32_t            loopCount       = 0;   // CCD iterations
        float               angleLimit      = 0.f; // per-iteration clamp (radians)
        std::vector<IKLink> links;                  // chain from effector toward root
    };

    static constexpr uint32_t MAX_MORPH_TARGETS = kMaxMorphTargets;

    uint32_t boneCount = 0;

    // The model's VIRTUAL bones (its VB records, Resource/ModelVirtualBones.h) come after the real
    // ones: [realBoneCount, boneCount). 0 = none were applied (every bone is real); read it through
    // RealBoneCount(). A system that finds bones by NAME for a purpose of its own (chain physics,
    // humanoid roles, retargeting, the rig hash) looks at the real ones only.
    uint32_t realBoneCount = 0;

    std::vector<VirtualBoneDef> virtualBones;
    // Resource::VirtualBonesSignature of the VB records this asset was loaded with (0 = none): the
    // Skeleton Editor compares it with the model file's to count the placed instances that have not
    // got a saved change yet.
    uint64_t virtualBoneSignature = 0;

    // Root-first topological order baked at import: parentIndex[i] = parent bone (-1 for a root).
    // INVARIANT: parentIndex[i] < i (i > 0).
    int32_t parentIndex[MAX_BONES]{};

    DirectX::XMFLOAT4X4 inverseBindPose[MAX_BONES]; // model-space -> bone-space
    DirectX::XMFLOAT4X4 bindPose[MAX_BONES];        // bone-space  -> model-space (for SocketSystem)
    DirectX::XMFLOAT4X4 restPoseLocal[MAX_BONES];   // parent-relative transform at rest

    // Bone names, for lookups by name (sockets, chain physics, retargeting, the editor). Per-frame
    // evaluation and GPU skinning work on indices only.
    char boneNames[MAX_BONES][64]{};

    // FNV-32 of the bone name -> bone index; filled at import.
    std::unordered_map<uint32_t, uint32_t> nameToIndex;

    // Per-bone rest-pose AABB in bind-MODEL space (the space the skin matrices map from), for the
    // skinned mesh AABB proxy; computed at import from the vertices each bone influences. A bone
    // that influences no vertex stays degenerate (min > max).
    BoneAABB boneRestAABBs[MAX_BONES]{};
    bool     hasBoneAABBs = false;  // true after import computes per-bone AABBs

    // PMX grant ("fuyo"): a bone copies the rotation of a source bone, e.g. a D-bone.
    // grantSource[i] = the source bone (-1 = no grant); grantRatio[i] = blend ratio (1.0 for
    // D-bones). Filled at import from the PMX bone data, or by SkeletonImporter::BuildGrantTable's
    // name heuristic.
    int32_t grantSource[MAX_BONES]{};
    float   grantRatio[MAX_BONES]{};

    // PMX transform order: the evaluation order of grants and IK, lower first. From the PMX bone data.
    int32_t transformOrder[MAX_BONES]{};

    // PMX IK chain definitions, parsed from the PMX bone data.
    std::vector<IKChain> ikChains;

    // Morph target names, in the morph delta buffer's order; MorphClipAsset channels bind to them by
    // name. NOT its own number: the cap is kMaxMorphTargets (shaders/morph_limits.hlsli), the one
    // MorphComponent::MAX, MorphClipAsset::MAX_MORPHS and Skin.cs.hlsl read. Face.* /
    // FacialAnimationSystem resolve names against this table, so it MUST be the full cap: a name
    // table shorter than the weight pool leaves GPU slots that exist and can never be addressed.
    uint32_t morphTargetCount = 0;
    char morphTargetNames[MAX_MORPH_TARGETS][64]{};   // 64 KB at the 1024 cap

    // No sockets here: a model's sockets are its SO records (Resource/ModelSockets.h), derived into
    // SocketComponent at spawn.

    // Canonical humanoid role map for retargeting (PMX <-> Unity/Unreal/VRM/Mixamo). Built at import
    // by HumanoidProfileBuilder; serialized into .iskel when ISKEL_FLAG_HUMANOID is set. The default
    // (all roles -1, valid=0) means "not a recognized humanoid": every role lookup returns -1. See
    // HumanoidBone.h and docs/design/RETARGET_ARCHITECTURE.md.
    HumanoidProfile humanoid;

    uint32_t RealBoneCount() const { return realBoneCount ? realBoneCount : boneCount; }
};

// LegDriveMode - how a clip's legs reach the skeleton:
//   IK (default): the native VMD path - the leg IK bone's translation drives the MMD CCD leg
//                 solver (IKSystem), and the rotation grant propagates to the D-bones.
//   FK:           a retargeted clip - leg rotations are baked FK into the base bones (leg /
//                 knee / ankle); IKSystem must SKIP the entity so the solver never fights the
//                 baked legs. Grants still propagate the FK result to the deform (D) chain.
// AnimationSystem mirrors the active clip's mode into AnimationComponent::activeLegDrive each
// frame; IKSystem reads the mirror.
enum class LegDriveMode : uint8_t { IK = 0, FK = 1 };

// ClipAsset - animation keyframes, SOA, index boneIdx * frameCount + frameIdx.
struct ClipAsset
{
    struct AnimEvent
    {
        float    time;
        uint32_t nameHash;
    };

    uint32_t boneCount  = 0;
    uint32_t frameCount = 0;
    float    duration   = 0.f; // total clip length in seconds
    float    frameRate  = 30.f;

    // IK = native/VMD, FK = retargeted: RetargetBaker sets FK on the clips it bakes, and
    // BindToSkeleton sets it from the .ianim ANIM_FLAG_LEG_FK (AnimClipData::legDriveFK) so a saved
    // bake keeps it; every other clip is IK.
    LegDriveMode legDriveMode = LegDriveMode::IK;

    // SOA keyframes: [boneIdx * frameCount + frameIdx]
    std::vector<DirectX::XMFLOAT3> positions;  // boneCount x frameCount
    std::vector<DirectX::XMFLOAT4> rotations;  // boneCount x frameCount (quaternion xyzw)
    std::vector<DirectX::XMFLOAT3> scales;     // boneCount x frameCount

    std::vector<AnimEvent> events;

    // AnimNotify tracks (Unreal AnimSequence-style): authored on the clip asset and carried into the
    // runtime clip by AnimClipData::BindToSkeleton; TimelineSystem fires them against
    // AnimationComponent::primaryTime (ECS/TimelineSystem.h). Time-based and skeleton-independent,
    // so one authored set drives every entity that plays the clip.
    std::vector<IKEnableTrack> ikEnableTracks;
    std::vector<NotifyTrack> notifyTracks;
    std::vector<Anim::RootMotionKey> rootMotion;
    std::vector<Anim::FloatCurve> floatCurves;
    uint32_t                 nextNotifyId = 1; // editor id allocator, persisted with the clip
};

// MorphClipAsset - runtime morph (blend-shape) animation clip, SOA: weights in [0, 1], index
// morphIdx * frameCount + frameIdx. morphNames[i] binds to the mesh morph target of that name.
struct MorphClipAsset
{
    // IN-MEMORY ONLY, the shared cap (shaders/morph_limits.hlsli). The .ianim container writes its
    // morph channel list length-prefixed, so this number is not part of the on-disk format: raising
    // it changes what a clip can HOLD, not what a file says.
    static constexpr uint32_t MAX_MORPHS = kMaxMorphTargets;

    uint32_t morphCount = 0;
    uint32_t frameCount = 0;
    float    duration   = 0.f;
    float    frameRate  = 30.f;
    // The clip's own name (MorphClipData::name, the source's), for the editor's clip list; "" = none
    // given.
    char     name[64]{};

    char morphNames[MAX_MORPHS][64]{};  // morph target name per channel (64 KB at the 1024 cap)

    // SOA weight data: weights[morphIdx * frameCount + frameIdx]
    std::vector<float> weights;
};

// The two caps this header owns must BOTH be the shared one. MorphComponent::MAX and Skin.cs.hlsl
// are checked against these in SkinningPass.cpp, the only translation unit that sees all four.
static_assert(SkeletonAsset::MAX_MORPH_TARGETS == kMaxMorphTargets,
              "SkeletonAsset::MAX_MORPH_TARGETS must be the shared morph cap "
              "(shaders/morph_limits.hlsli)");
static_assert(MorphClipAsset::MAX_MORPHS == kMaxMorphTargets,
              "MorphClipAsset::MAX_MORPHS must be the shared morph cap "
              "(shaders/morph_limits.hlsli)");
// FacialAnimationComponent stores a resolved mesh slot in an int16_t with -1 /
// -2 sentinels, so the cap can never reach 32767.
static_assert(kMaxMorphTargets < 32767u,
              "morph slots are carried in an int16_t with negative sentinels");

// MorphClipLibrary - owns every loaded MorphClipAsset; addressed by uint32_t index.
class MorphClipLibrary
{
public:
    // Takes an rvalue, never a value: under the MSVC x64 ABI a by-value parameter of a
    // non-trivially-copyable type is materialised in the CALLER's stack frame, and MorphClipAsset is
    // a 64 KB name table at the 1024 cap (see SkeletonRegistry::Register).
    uint32_t              Register(MorphClipAsset&& clip);
    const MorphClipAsset& Get(uint32_t index) const;
    uint32_t              Count() const { return static_cast<uint32_t>(m_clips.size()); }

private:
    std::vector<MorphClipAsset> m_clips;
};

// SkeletonRegistry - owns every loaded SkeletonAsset; addressed by uint32_t index.
// Single-threaded: registrations happen at load time only.
class SkeletonRegistry
{
public:
    // Register a new SkeletonAsset; the returned index stays valid.
    // Do not make the parameter by-value: SkeletonAsset is large (fixed arrays, a 64 KB morph name
    // table at the 1024 cap) and not trivially copyable, so the MSVC x64 ABI would materialise a full
    // copy in the CALLER's stack frame. The caller, SceneInstanceLoader::Load, already runs deep under
    // App::Run -> Scheduler -> ScriptSystem -> Lua -> LoadPrefab; the copy overflows the 1 MB
    // main-thread stack, and the STATUS_STACK_OVERFLOW callstack points at the D3D12 driver (inside
    // MeshLibrary::Load), naming nothing here.
    uint32_t       Register(SkeletonAsset&& asset);

    SkeletonAsset& Get(uint32_t index);
    const SkeletonAsset& Get(uint32_t index) const;

    uint32_t Count() const { return static_cast<uint32_t>(m_skeletons.size()); }

private:
    std::vector<SkeletonAsset> m_skeletons;
};

// ClipLibrary - owns every loaded ClipAsset; addressed by uint32_t index.
class ClipLibrary
{
public:
    uint32_t         Register(ClipAsset clip);
    const ClipAsset& Get(uint32_t index) const;
    // Mutable access for the animation editor, which live-updates a bound clip's notify tracks
    // during preview ("editor sees == game sees"). @p index must be < Count(). Do NOT mutate the SOA
    // keyframe arrays at runtime: AnimationSystem reads them every frame.
    ClipAsset&       GetMutable(uint32_t index);
    uint32_t         Count() const { return static_cast<uint32_t>(m_clips.size()); }

private:
    std::vector<ClipAsset> m_clips;
};

// ==== Inline implementations ====
inline uint32_t SkeletonRegistry::Register(SkeletonAsset&& asset)
{
    uint32_t idx = static_cast<uint32_t>(m_skeletons.size());
    m_skeletons.push_back(std::move(asset));
    return idx;
}
inline SkeletonAsset& SkeletonRegistry::Get(uint32_t index)
{
    return m_skeletons[index];
}
inline const SkeletonAsset& SkeletonRegistry::Get(uint32_t index) const
{
    return m_skeletons[index];
}

inline uint32_t ClipLibrary::Register(ClipAsset clip)
{
    uint32_t idx = static_cast<uint32_t>(m_clips.size());
    m_clips.push_back(std::move(clip));
    return idx;
}
inline const ClipAsset& ClipLibrary::Get(uint32_t index) const
{
    return m_clips[index];
}
inline ClipAsset& ClipLibrary::GetMutable(uint32_t index)
{
    // Checked in every build: a bad index would write past m_clips. A reference has no null to
    // return, so a miss gets a cleared scratch clip (the caller's write lands nowhere, a read sees an
    // empty clip). Logged once per process: the animation editor calls this every frame.
    if (!ENGINE_CHECK_ONCE(index < m_clips.size(),
            "ClipLibrary::GetMutable: clip %u out of range (%zu clips)", index, m_clips.size()))
    {
        static ClipAsset s_scratch;
        s_scratch = ClipAsset{};
        return s_scratch;
    }
    return m_clips[index];
}

inline uint32_t MorphClipLibrary::Register(MorphClipAsset&& clip)
{
    uint32_t idx = static_cast<uint32_t>(m_clips.size());
    m_clips.push_back(std::move(clip));
    return idx;
}
inline const MorphClipAsset& MorphClipLibrary::Get(uint32_t index) const
{
    return m_clips[index];
}
