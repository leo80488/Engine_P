#pragma once

// ECS components for skeleton animation. Bones are NOT ECS entities.
//   SkeletonComponent        asset reference, on the character root (immutable after spawn)
//   AnimationComponent       playback state, on the character root (updated every frame)
//   MeshSkinnedComponent     GPU rest-pose buffers, on each mesh entity (immutable after upload)
//   SkinningOutputComponent  per-frame ring offsets, on each mesh entity (SkinMatrixSystem)
//   SocketComponent          named attachment points (world transforms by SocketSystem)

#include "ECS/ECS.h"
#include "Graphics/GraphicsStruct.h"  // RHI::GPUBuffer

// kMaxMorphTargets / ENGINE_MAX_MORPH_TARGETS - the morph-target cap shared with
// Skin.cs.hlsl. Included directly rather than through Resource/SkeletonAsset.h: ECS must not
// depend on Resource, and this file is a leaf with nothing in it but the number.
#include "../../shaders/morph_limits.hlsli"

#include <cstdint>
#include <cstring>
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>
#include <DirectXMath.h>

static constexpr uint32_t kInvalidAnimHandle = ~0u;

// SkeletonComponent - which SkeletonAsset this entity uses. Placed on the CHARACTER ROOT
// entity (not on mesh entities).
struct SkeletonComponent
{
    uint32_t assetIndex = kInvalidAnimHandle; // -> SkeletonRegistry::Get(assetIndex)
    uint32_t boneCount  = 0;                  // cached from SkeletonAsset

    // Per-frame: byte offset into PoseRingBuffer of this skeleton's skin matrices, written by
    // LocalToWorldSystem each frame. Defaults to the ~0u "no pose this frame" sentinel (NOT 0:
    // 0 is ring slot 0), so a fresh or reloaded skeleton not yet posed makes the skinned draw
    // gate fall back to the rest pose instead of skinning against another skeleton's slot.
    uint32_t poseByteOffset = ~0u;
};

// SkeletonRef - placed on MESH entities to link them to the character root that owns
// SkeletonComponent + AnimationComponent, so one skeleton drives several meshes (body,
// clothes, hair).
struct SkeletonRef
{
    Entity entity = NullEntity; // entity holding SkeletonComponent + AnimationComponent
};

// AnimationComponent - current playback state, mutated by AnimationSystem each frame.
struct AnimationComponent
{
    uint32_t primaryClip   = kInvalidAnimHandle; // -> ClipLibrary
    uint32_t secondaryClip = kInvalidAnimHandle; // kInvalidAnimHandle = no cross-fade active

    float primaryTime   = 0.f;    // playback position in seconds
    float secondaryTime = 0.f;
    float blendWeight   = 0.f;    // 0 = primary only, 1 = secondary only
    float speed         = 1.f;    // playback rate multiplier (negative = reverse)

    bool looping = true;
    bool paused  = false;
    // When true, an external driver (MotionDirector) owns primaryTime: the
    // AnimationSystem samples at primaryTime but does NOT advance/loop/clamp it.
    bool externalTime = false;
    // Runtime mirror of the ACTIVE clip's ClipAsset::legDriveMode (0 = IK, 1 = FK), written by
    // AnimationSystem every sample; IKSystem skips the MMD leg CCD for retargeted-FK clips. Never
    // serialized: re-derived from the clip each frame (during a cross-fade the primary clip wins).
    uint8_t activeLegDrive = 0;
    float legIKWeight = 1.f; // runtime, rebuilt each sampling frame
    uint64_t ikChainMask = ~uint64_t(0);
    std::vector<uint8_t> ikChainOverflow;

    uint32_t pendingEventMask = 0; // bitmask of AnimEvents fired this frame (consumed by game)

    // ---- Clip-driven AnimNotify cursor (Unreal AnimSequence-style) ----
    // TimelineSystem advances these to detect which notify times the clip crossed since last
    // frame. prevNotifyTime < 0 = uninitialized: on first sight, or after a clip switch
    // (lastNotifyClip != primaryClip), TimelineSystem snapshots the time without firing, so
    // opening a clip does not replay every notify at once.
    float    prevNotifyTime = -1.f;
    uint32_t lastNotifyClip = kInvalidAnimHandle;

    bool IKChainEnabled(size_t i) const {
        return i < 64 ? (ikChainMask & (uint64_t(1) << i)) != 0
                      : i - 64 < ikChainOverflow.size() && ikChainOverflow[i - 64] != 0;
    }
};

// Name for the runtime half of the AnimationSet (authored data) + Animator (runtime state)
// split; AnimationComponent remains the type the engine uses.
using AnimatorComponent = AnimationComponent;

// AnimPoseOverrideComponent - "I own this actor's pose, not the state machine."
//
// AnimationComponent::externalTime is NOT pose authority: it only stops the primaryTime
// advance. An active AnimatorTransitionComponent still completes and overwrites primaryClip
// and primaryTime (stealing an actor mid-shot), and locomotion still supplies the base pose.
// Both show only in Play (CharacterAnimDriverTickSystem is RunMask::Play), so a shot that
// scrubs fine in the editor falls apart on Play. The Sequencer adds this token on bind and
// removes it on release. CharacterAnimDriver parks on its presence; AnimationSystem,
// AnimGraphSystem and AnimationClipSystem honour the Base bit of `layers`. The other bits are
// reserved for partial (additive / IK-only) overrides.
struct AnimPoseOverrideComponent
{
    enum Layer : uint8_t
    {
        Base       = 1 << 0,
        Additive   = 1 << 1,
        Locomotion = 1 << 2,
        IK         = 1 << 3,
        Morph      = 1 << 4,
        All        = 0xFF,
    };

    // Entity id of the owner (a sequencer player). Diagnostics only: the last writer wins, and
    // WHO holds an actor is the first question when a character refuses to move.
    uint32_t owner  = 0u;
    uint8_t  layers = All;
};

// AnimationClipReference / AnimationSetComponent - authored, serializable animation asset
// references. Runtime ClipLibrary indices and Resource handles deliberately do NOT live here:
// AnimationClipSystem owns those bindings and reconciles them from these stable (name, path)
// entries, so editor snapshots, scene serialization and Lua state stay independent of
// process-local handles.
struct AnimationClipReference
{
    std::string name = "Default"; // stable Lua/editor alias ("Idle", "Run", ...)
    std::string path;              // project-relative .ianim asset path
    // Optional selectors inside a multi-clip .ianim. Empty sourceClip means
    // the first bone clip. Empty sourceMorphClip auto-matches the selected
    // bone clip name, then falls back to the first morph clip.
    std::string sourceClip;
    std::string sourceMorphClip;
    bool        looping = true;
    float       speed   = 1.f;
};

struct AnimationSetComponent
{
    std::vector<AnimationClipReference> clips;
    std::string defaultClip;
    bool        playDefaultOnLoad = false;

    AnimationClipReference* Find(std::string_view name)
    {
        auto it = std::find_if(clips.begin(), clips.end(),
            [name](const AnimationClipReference& clip) { return clip.name == name; });
        return it != clips.end() ? &*it : nullptr;
    }

    const AnimationClipReference* Find(std::string_view name) const
    {
        auto it = std::find_if(clips.begin(), clips.end(),
            [name](const AnimationClipReference& clip) { return clip.name == name; });
        return it != clips.end() ? &*it : nullptr;
    }

    AnimationClipReference& Set(std::string name, std::string path,
                                bool looping = true, float speed = 1.f,
                                std::string sourceClip = {},
                                std::string sourceMorphClip = {})
    {
        if (AnimationClipReference* existing = Find(name))
        {
            existing->path            = std::move(path);
            existing->sourceClip      = std::move(sourceClip);
            existing->sourceMorphClip = std::move(sourceMorphClip);
            existing->looping         = looping;
            existing->speed           = speed;
            return *existing;
        }
        clips.push_back({ std::move(name), std::move(path),
                          std::move(sourceClip), std::move(sourceMorphClip),
                          looping, speed });
        return clips.back();
    }
};

// Runtime-only transition metadata for Animator.Play(name, blendSeconds).
// AnimationSystem advances this state and promotes secondary -> primary when
// the cross-fade completes. It is intentionally not registered for scene I/O.
struct AnimatorTransitionComponent
{
    std::string currentClipName;
    std::string targetClipName;
    float       duration      = 0.f;
    float       elapsed       = 0.f;
    float       targetSpeed   = 1.f;
    bool        targetLooping = true;
    bool        active        = false;
};

// One-frame/asynchronous command mailbox written by Lua or the editor and
// consumed by AnimationClipSystem once the named clip is bound to the entity's
// skeleton. Keeping the request separate avoids putting strings/asset handles
// into the hot AnimatorComponent sampled every frame.
struct AnimatorPlayRequestComponent
{
    std::string clipName;
    float       blendDuration = 0.2f;
    bool        restart       = false;
};

// MeshSkinnedComponent - GPU buffer handles for a skinned mesh, immutable after upload.
// Everything MeshComponent holds, plus the skinning rest-pose and blend-weight streams.
// Static streams (uploaded once at registration):
//   restPosBuffer  : StructuredBuffer<float3>, 12 B/vertex - rest-pose positions
//   restNrmBuffer  : StructuredBuffer<float3>, 12 B/vertex - rest-pose normals
//   blendBuffer    : ByteAddressBuffer, BlendVertex[], 8 B/vertex
//   uvBuffer       : StructuredBuffer<float2>, 8 B/vertex - UV set 0
//   tangentBuffer  : StructuredBuffer<float4>, 16 B/vertex - tangents (optional)
//   indexBuffer    : ByteAddressBuffer, uint32[] - triangle indices
// The GPU skinning pass writes the per-frame output into SkinnedVertexRing; the skinned
// MeshDescriptor references it.
struct MeshSkinnedComponent
{
    // ---- Rest-pose + blend buffers (static, immutable after upload) ----
    RHI::GPUBuffer blendBuffer;      // 8 B/vertex:  BlendVertex (indices + weights)
    RHI::GPUBuffer restPosBuffer;    // 12 B/vertex: float3 rest-pose positions
    RHI::GPUBuffer restNrmBuffer;    // 12 B/vertex: float3 rest-pose normals

    // ---- Static mesh streams (UV, tangent, index - like MeshComponent) ----
    RHI::GPUBuffer uvBuffer;         // 8 B/vertex:  float2 UV set 0
    RHI::GPUBuffer uv1Buffer;        // 8 B/vertex:  float2 UV set 1 (optional; SDF face / lightmap)
    RHI::GPUBuffer uv2Buffer;        // optional additional UV set (float2)
    RHI::GPUBuffer uv3Buffer;        // optional additional UV set (float2)
    RHI::GPUBuffer tangentBuffer;    // 16 B/vertex: float4 tangent (may be invalid if absent)
    RHI::GPUBuffer indexBuffer;      // 4 B/index:   uint32 triangle indices

    // ---- Counts (like MeshComponent) ----
    uint32_t vertexCount        = 0;
    uint32_t indexCount         = 0;

    // ---- Source-buffer ownership ----
    // 0  = this component OWNS the source buffers above; they are recorded in
    //      SkinnedMeshSubsystem::m_ownedGpuBuffers and freed with the entity.
    // !0 = the buffers are BORROWED from the subsystem's shared-source cache (one copy per
    //      identical character, see SkinnedMeshPending::sourceKey), which owns them for the
    //      world's lifetime: entity destroy / re-register / undo-drop must NOT free them or their
    //      bindless slots. meshDescriptorIdx stays per-instance either way - BuildSkinJobs
    //      patches this instance's own skinning output into it every frame.
    uint64_t sharedSourceKey = 0;

    // ---- Mesh descriptor tracking ----
    uint32_t meshDescriptorIdx  = ~0u; // stable slot in MeshDescriptorHeap

    // Copy of the base MeshDescriptor used for patching (cached at registration).
    // UV/index/tangent streams come from here; pos/nrm are overridden per frame.
    RHI::MeshDescriptor baseMeshDesc{};

    // ---- Bounding volume (like MeshComponent.aabb) ----
    DirectX::XMFLOAT3 aabbMin = {  0.f,  0.f,  0.f };
    DirectX::XMFLOAT3 aabbMax = {  0.f,  0.f,  0.f };
    // Max |restPos| over this mesh part (model-local). Fed to the skinning divergence clamp
    // (per-skeleton max -> SkinDispatchDesc::charExtent).
    float restExtent = 0.f;

    // ---- Per-mesh bone influence list ----
    // Unique bone indices with non-zero weight on at least one vertex of this mesh, filled at
    // registration from the BlendVertex data, for a per-mesh bone AABB. Nothing reads it at
    // present.
    std::vector<uint16_t> influenceBones;

    // ---- Cached GPU descriptor handles (SRV) ----
    // Set at registration; copied into each skin dispatch job.
    uint64_t blendSRVHandle   = 0;
    uint64_t restPosSRVHandle = 0;
    uint64_t restNrmSRVHandle = 0;
    // Rest-pose TANGENT (float4: xyz dir, w handedness). 0 = the mesh has no authored tangent
    // stream: skinning skips the tangent output and the vertex factory synthesises a basis.
    uint64_t restTanSRVHandle = 0;

    // ---- Morph targets (vertex morphs) ----
    // Dense layout: float3 deltas[morphTargetCount * vertexCount],
    // index localMorphIdx * vertexCount + vertexIdx. With morphHasNormals (.imorph v2) the same
    // count of NORMAL deltas follows in the same order: normal of (m, v) at
    // morphTargetCount * vertexCount + m * vertexCount + v.
    // morphTargetCount is this MESH's LOCAL target count, NOT the skeleton's: a sparse .imorph
    // block carries only the shapes that move this mesh (Resource/AssetHeader.h,
    // MORPHLIB_FLAG_SPARSE_SLOTS), so a 700-shape rig does not pay 700 zero-filled delta arrays
    // per part.
    RHI::GPUBuffer morphDeltaBuffer;
    uint64_t       morphDeltaSRVHandle = 0;
    uint32_t       morphTargetCount    = 0;
    bool           morphHasNormals     = false;

    // The two directions of the local<->skeleton morph mapping. Everything that names a morph
    // (the clip remap, the editor sliders, Face.*) works in SKELETON slot space
    // (SkeletonAsset::morphTargetNames); the GPU delta buffer and this job's slice of the morph
    // weight pool are in LOCAL space. BuildSkinJobs is the only translator.
    // A dense .imorph yields the identity mapping, so both are populated whenever
    // morphTargetCount > 0 and nothing downstream branches on the format.
    // morphSlotOfLocal: size == morphTargetCount, strictly increasing.
    // morphLocalOfSlot: size == the SkeletonAsset's morphTargetCount, -1 = this mesh does not
    //                   carry that shape. int16 suffices: the local count never exceeds
    //                   kMaxMorphTargets (1024).
    std::vector<uint32_t> morphSlotOfLocal;
    std::vector<int16_t>  morphLocalOfSlot;

    // Per LOCAL target, the farthest it moves a vertex at weight 1 (bind space).
    // BuildSkinJobs grows the part's WorldAabb by the sum of weight x this over the active
    // targets, so a shape key never carries geometry out of the bounds the culls test.
    std::vector<float> morphMaxDelta;
    // The box that growth last wrote into WorldAabb and the box it grew: a
    // WorldAabb nobody has rewritten since still holds the former, and is grown
    // again from the latter - never compounded frame over frame.
    DirectX::XMFLOAT3 morphBoundsBaseMin{}, morphBoundsBaseMax{};
    DirectX::XMFLOAT3 morphBoundsLastMin{}, morphBoundsLastMax{};
    bool              morphBoundsGrown = false;
};

// SkinningOutputComponent - per-frame allocation results on each skinned MESH entity. The
// ring offsets are written by SkinMatrixSystem, prevPosElementBase and the prevFrame* fields
// by SkinnedMeshSubsystem::BuildSkinJobs; read by BuildSkinJobs (skin jobs, MeshDescriptor
// patch) and the scene gather's skinned-draw gate.
struct SkinningOutputComponent
{
    // ~0u = no valid pose this frame (animation not active or not yet computed).
    uint32_t poseByteOffset       = ~0u; // byte offset into PoseRingBuffer for this entity's bones
    uint32_t outPosByteOffset     = 0;   // byte offset into SkinnedVertexRing position buffer
    uint32_t outNrmByteOffset     = 0;   // byte offset into SkinnedVertexRing normal buffer
    uint32_t prevPosElementBase   = ~0u; // prev-frame skinned position element base, 0xFFFFFFFF = none (a CPU batching key)
    // Byte offset into SkinnedVertexRing's TANGENT buffer. ~0u = this mesh has no tangent
    // stream: nothing is written and the MeshDescriptor keeps its bind-pose tangent.
    uint32_t outTanByteOffset     = ~0u;

    // WHERE THIS ENTITY'S SKINNED POSITIONS LANDED LAST FRAME, and which frame that was. The
    // skinned vertex ring is kFramesInFlight deep and each slot is written only by its own frame,
    // so the previous slot still holds them: the vertex shaders read them through
    // MeshDescriptor::prevPosition instead of re-skinning every vertex with the previous pose.
    // The offset is NOT stable across frames (AllocPosition is a per-frame bump allocator in pool
    // order; a spawn or a cull shifts everyone after it), hence stored, not assumed.
    // prevFrameIndex != the previous build means "not skinned last frame" (spawned, culled, or
    // an invalid pose), which falls back to zero motion.
    uint32_t prevFrameOutPosByteOffset = ~0u;
    uint32_t prevFrameVertexCount      = 0;
    uint64_t prevFrameIndex            = 0;
};

// MorphComponent - current morph/blend-shape weights for one entity.
//   primaryMorphClip -> index into MorphClipLibrary (set by AnimationClipSystem).
//   weights[]        -> live output written by AnimationSystem each frame, parallel to
//                       MorphClipAsset::morphNames[]; the GPU skinning/morph pass reads them.
// With an AnimationComponent on the entity the morph time follows
// AnimationComponent::primaryTime; without one it advances on MorphComponent::time.
struct MorphComponent
{
    static constexpr uint32_t MAX = kMaxMorphTargets;

    uint32_t primaryMorphClip = kInvalidAnimHandle; // -> MorphClipLibrary

    float time   = 0.f;   // independent playback time (used only when no AnimationComponent)
    bool  paused = false;
    bool  looping = true;
    uint8_t _pad[2]{};

    uint32_t count      = 0;           // number of active morph channels
    // Live output, written by AnimationSystem each frame.
    // MAX is kMaxMorphTargets from shaders/morph_limits.hlsli, the one constant that also sizes
    // SkeletonAsset::morphTargetNames[] (the table Face.* resolves against),
    // MorphClipAsset::MAX_MORPHS and the clamp in Skin.cs.hlsl. Do not give any of them its own
    // literal: a rig between two different caps passes the C++ bounds and silently loses shapes
    // on the GPU. weights[] is 4 KB at the 1024 cap, on the character ROOT only; the GPU upload
    // is not sized by MAX (SkinningPass packs per-job regions at the job's own morphCount).
    float    weights[MAX]{};           // per-target weight in [0, 1]
};
static_assert(MorphComponent::MAX == kMaxMorphTargets,
              "MorphComponent::MAX must be the shared morph cap "
              "(shaders/morph_limits.hlsli)");

// PreviewPoseOverrideComponent - an editor preview's TEMPORARY pose (the Skeleton Editor's
// bone manipulation, to see the skin deform). Per bone a rotation applied in the bone's own
// frame on top of the FINAL local pose (clip, grants, IK, chains) by
// AnimationSystem::ApplyPreviewPoseOverrides, after chain physics and before LocalToWorld,
// so the bone turns by exactly what was asked. Only on a preview stage's skeleton; never
// serialized, never an asset; kept across clip changes; the panel's Reset Pose removes it.
struct PreviewPoseOverrideComponent
{
    struct Entry
    {
        uint32_t          bone = 0;
        DirectX::XMFLOAT4 rot  = { 0.f, 0.f, 0.f, 1.f };   // quaternion (xyzw), the bone's own frame
    };
    std::vector<Entry> entries;
};

// SocketComponent - named attachment points on this character's skeleton. Combines the
// sockets this entity publishes (definition) with the per-frame world transforms SocketSystem
// writes (cache). Followers reference sockets by index via FollowSocketComponent and read the
// cache directly, with no pose-buffer round-trip. POD + fixed-size, so it stays cache-friendly
// in a ComponentPool. Per-entity, not per-asset: two characters sharing a skeleton can publish
// different socket sets, and a prefab can override or extend a base list.
// Socket::localOffset is applied in BONE-local space (before the bone's current world
// matrix), matching the editor gizmo.
struct SocketComponent
{
    // Room for the entity's own sockets plus the model's derived ones (from its SO records),
    // which share the list.
    static constexpr uint32_t MAX = 32;

    // A socket DERIVED from the model's SO records at spawn (SceneInstanceLoader::
    // DeriveModelSockets): rebuilt from the model file, never saved with the entity.
    static constexpr uint8_t kDerived = 1u << 0;
    // Runtime (SocketSystem): boneName names no bone of this skeleton - the socket
    // is not placed (its index is not trusted to mean anything).
    static constexpr uint8_t kBoneMissing = 1u << 1;
    // A MESH socket (the model's `SO mesh=<key>` record, derived onto the StaticMesh owner
    // placing that Mesh): no bone - SocketSystem places it at offset x the entity's world, as it
    // places every socket of an entity that has no skeleton.
    static constexpr uint8_t kMeshSocket = 1u << 2;

    struct Socket
    {
        char                name[64]    = {};         // user-facing id ("hand_r", "fx_root", ...)
        uint32_t            boneIndex   = 0;          // index into SkeletonAsset::boneNames / bindPose
        DirectX::XMFLOAT4X4 localOffset;              // bone-local offset (static) - runtime source of truth
        DirectX::XMFLOAT4X4 worldTransform;           // written every frame by SocketSystem
        // Editor authoring representation of the rotation. Translation is read directly from
        // localOffset._41/_42/_43; this field spares the inspector a quaternion -> euler decompose
        // every frame (which jitters near gimbal lock). On load the matrix is decomposed once to
        // seed it.
        DirectX::XMFLOAT3   rotationEulerDeg = { 0.f, 0.f, 0.f };
        // The bone BY NAME ("" = the index alone). SocketSystem re-resolves boneIndex from it
        // whenever the two disagree (a re-import that reordered the bones), and fills it in from a
        // valid index when it is empty, so the next save names the bone.
        char                boneName[64] = {};
        uint8_t             flags        = 0;         // kDerived | kBoneMissing | kMeshSocket

        Socket()
        {
            DirectX::XMStoreFloat4x4(&localOffset,    DirectX::XMMatrixIdentity());
            DirectX::XMStoreFloat4x4(&worldTransform, DirectX::XMMatrixIdentity());
        }
    };

    static constexpr uint32_t kNotFound = ~0u;

    // Authored sockets first (in their record's order - a VFX notify may address a socket by
    // raw INDEX), derived ones after.
    Socket   sockets[MAX];
    uint32_t count = 0;

    // Bumped whenever the socket LIST changes shape (add, remove, rename). A
    // FollowSocketComponent caches the index its socket name resolved to and compares
    // generations to know when that index went stale: removing a socket shifts every later
    // entry down, and a follower past it would silently retarget to its neighbour.
    // Runtime-only: a fresh load starts at 1 and forces exactly one re-lookup per follower.
    uint32_t generation = 1;

    // SocketSystem's: the skeleton (SkeletonComponent::assetIndex) and list
    // generation the bone indices were last resolved by name against - the
    // names are looked up again only when either moves. Runtime-only.
    uint32_t boneResolvedSkel = ~0u;
    uint32_t boneResolvedGen  = 0;

    // FNV-1a 32. Never returns 0: that value means "address this socket by raw index instead"
    // (the VFX bone-index path, and asset files without a name hash).
    static uint32_t HashName(const char* n) noexcept
    {
        if (!n || !*n) return 0u;
        uint32_t h = 2166136261u;
        for (const char* p = n; *p; ++p)
        {
            h ^= static_cast<uint32_t>(static_cast<unsigned char>(*p));
            h *= 16777619u;
        }
        return h ? h : 1u;
    }

    uint32_t FindByName(uint32_t nameHash) const
    {
        if (!nameHash) return kNotFound;
        for (uint32_t i = 0; i < count; ++i)
            if (HashName(sockets[i].name) == nameHash) return i;
        return kNotFound;
    }

    static bool SameName(const Socket& a, const Socket& b) noexcept
    {
        return std::strncmp(a.name, b.name, sizeof(a.name)) == 0;
    }

    uint32_t AuthoredCount() const noexcept
    {
        uint32_t n = 0;
        for (uint32_t i = 0; i < count; ++i) n += (sockets[i].flags & kDerived) ? 0u : 1u;
        return n;
    }

    // Both replacements work in place - no MAX-sized local (the loader that calls
    // them already runs deep in the main thread's stack) - so @p list must not
    // point into this component's own array.

    // The derived sockets OF ONE KIND become @p list: @p kind 0 = the model's skeleton sockets
    // (DeriveModelSockets), kMeshSocket = a Mesh's (DeriveMeshSockets); the other kind's stay
    // (an entity can be both a skeleton root and a StaticMesh owner). Appended after every kept
    // entry; a name a kept entry already has is skipped - the entity's own wins. A socket that
    // was there by the same name keeps its last world transform until SocketSystem places it
    // again (an anim-culled character is not re-placed, and its followers would jump to the
    // origin). Returns how many were taken (MAX caps the list). Bumps generation, unless an
    // empty list replaced nothing.
    uint32_t ReplaceDerived(const Socket* list, uint32_t n, uint8_t kind = 0)
    {
        auto ofKind = [kind](const Socket& s)
        { return (s.flags & kDerived) != 0 && (s.flags & kMeshSocket) == (kind & kMeshSocket); };
        std::vector<std::pair<uint32_t, DirectX::XMFLOAT4X4>> prior;
        uint32_t k = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            if (ofKind(sockets[i])) { prior.emplace_back(HashName(sockets[i].name), sockets[i].worldTransform); continue; }
            if (k != i) sockets[k] = sockets[i];
            ++k;
        }
        if (n == 0 && prior.empty()) return 0;   // nothing of that kind, nothing new: unchanged
        const uint32_t kept = k;
        uint32_t taken = 0;
        for (uint32_t j = 0; j < n && k < MAX; ++j)
        {
            bool shadowed = false;
            for (uint32_t a = 0; a < kept && !shadowed; ++a) shadowed = SameName(sockets[a], list[j]);
            if (shadowed) continue;
            sockets[k] = list[j];
            sockets[k].flags |= kDerived;
            const uint32_t h = HashName(sockets[k].name);
            for (const auto& [ph, world] : prior)
                if (ph == h) { sockets[k].worldTransform = world; break; }
            ++k;
            ++taken;
        }
        count = k;
        ++generation;
        return taken;
    }

    // The authored sockets become @p list (first, in its order); a derived one stays unless
    // @p list names it. Bumps generation (never resets it: a follower that cached this
    // generation must look its socket up again).
    void ReplaceAuthored(const Socket* list, uint32_t n)
    {
        if (n > MAX) n = MAX;
        // The derived ones the list does not name, to the front (in order)...
        uint32_t kept = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            if (!(sockets[i].flags & kDerived)) continue;
            bool named = false;
            for (uint32_t j = 0; j < n && !named; ++j) named = SameName(sockets[i], list[j]);
            if (!named) sockets[kept++] = sockets[i];
        }
        // ...then behind where the list goes (from the back: the ranges overlap)...
        const uint32_t room = MAX - n;
        if (kept > room) kept = room;
        for (uint32_t i = kept; i-- > 0;) sockets[n + i] = sockets[i];
        // ...and the list in front.
        for (uint32_t j = 0; j < n; ++j)
        {
            sockets[j] = list[j];
            sockets[j].flags &= static_cast<uint8_t>(~kDerived);
        }
        count = n + kept;
        ++generation;
    }

    // One more of the entity's own sockets, after its others (ahead of the derived
    // ones); a derived socket of that name goes - the entity's own wins.
    bool Add(const char* socketName, uint32_t boneIdx)
    {
        if (count >= MAX) return false;
        const uint32_t at = AuthoredCount();
        for (uint32_t i = count; i > at; --i) sockets[i] = sockets[i - 1];
        ++count;
        Socket& s = sockets[at];
        s = Socket{};
        s.boneIndex = boneIdx;
        if (socketName)
        {
            const size_t len = std::strlen(socketName);
            const size_t copy = (len < sizeof(s.name) - 1) ? len : (sizeof(s.name) - 1);
            std::memcpy(s.name, socketName, copy);
            s.name[copy] = '\0';
        }
        uint32_t k = at + 1;
        for (uint32_t i = at + 1; i < count; ++i)
            if (!SameName(sockets[i], s)) sockets[k++] = sockets[i];
        count = k;
        ++generation;
        return true;
    }
};

// PendingAnimBind - deferred animation binding for an async-loading clip.
// AnimationClipSystem::ResolvePendingBinds binds the clip once it is ready, then removes the
// component.
struct PendingAnimBind
{
    std::string animPath;
    uint32_t    handlePacked = 0; // AnimHandle::packed (avoids Resource:: dependency in ECS header)
};
