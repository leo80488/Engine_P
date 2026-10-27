#pragma once

// SkinnedMeshSubsystem - the Renderer's skeletal-animation and GPU-skinning state.
//
// Owns:
//   - the skeleton / clip / morph-clip registries
//   - the pose ring and the skinned-vertex ring (per-frame GPU scratch)
//   - the animation ECS systems (Animation, AnimGraph, IK, IKRig, HeadTrack, Facial,
//     CharacterState, ChainPhysics, LocalToWorld, Socket, Follow, SkinMatrix), and the
//     modules' chain hooks (borrowed)
//   - the SkinningPass (compute dispatch: rest pose -> skinned)
//   - the per-frame job list and morph-weight pool, the descriptor-slot and morph-remap
//     caches, the anim-culling sets, the shared skinned-source cache, and every skinned
//     GPU buffer it creates
//
// Init() once at startup, BuildSkinJobs() each frame, RegisterSkinnedMesh(Full) at
// entity creation. Renderer forwards its accessors to this member.

#include "ECS/AnimationComponents.h"     // MeshSkinnedComponent, SkinningOutputComponent
#include "ECS/AnimGraphSystem.h"
#include "ECS/AnimationSystem.h"         // AnimationSystem, LocalToWorldSystem, SkinMatrixSystem
#include "ECS/IKSystem.h"
#include "ECS/HeadTrackSystem.h"
#include "ECS/FacialAnimationSystem.h"
#include "Anim/AnimChainHook.h"
#include "ECS/IKRigSystem.h"
#include "ECS/CharacterStateSystem.h"
#include "ECS/SocketSystem.h"
#include "ECS/FollowSystem.h"
#include "Physics/ChainPhysicsSystem.h"
#include "Resource/SkeletonAsset.h"      // SkeletonRegistry, ClipLibrary, BlendVertex
#include "Graphics/SkinningBuffers.h"    // PoseRingBuffer, SkinnedVertexRing
#include "Scene/SceneLoader.h"           // SceneLoader::SkinnedMeshPending
#include "RenderGraph/RenderPass/SkinningPass.h" // SkinDispatchDesc, SkinningPass
#include "ECS/ECS.h"                     // Entity

#include <cstdint>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class IGraphicsDevice;
class MeshManager;

class SkinnedMeshSubsystem
{
public:
    struct SharedSourceDerived
    {
        DirectX::XMFLOAT3     aabbMin{}, aabbMax{};
        float                 restExtent = 0.f;
        std::vector<uint16_t> influenceBones;
        uint32_t              morphTargetCount = 0;   // 0 = no morph block
        bool                  morphHasNormals  = false;
        std::vector<uint32_t> morphSlotOfLocal;
        std::vector<int16_t>  morphLocalOfSlot;
        std::vector<float>    morphMaxDelta;
        bool                  valid = false;
    };

    struct SourceBuild;

    // Init pose / vertex rings, register per-frame ring slots in the bindless
    // table, construct ECS animation systems, and create the SkinningPass.
    // Must run once during Renderer::Compile().
    void Init(IGraphicsDevice& gfx, MeshManager& meshMgr);

    // Walks every MeshSkinnedComponent in @p world and builds the per-entity
    // SkinDispatchDesc list (the write half, SkinJobsWrite). Also patches each entity's
    // MeshDescriptor to point at this frame's skinned output streams. Called from
    // Renderer::TickAnimationChain (the Animation phase).
    void BuildSkinJobs(World& world, MeshManager& meshMgr);

    // Uploads rest-pose + blend data for one mesh and adds MeshSkinnedComponent +
    // SkinningOutputComponent to @p e. Returns false when a buffer create fails; that
    // happens before the component is (re)placed, so a re-registered entity keeps its
    // previous, still-owned buffer set. The previous set is released at the point of no
    // return, just before AddComponent.
    bool RegisterSkinnedMesh(IGraphicsDevice& gfx,
                             World& world, Entity e,
                             const DirectX::XMFLOAT3* positions,
                             const DirectX::XMFLOAT3* normals,
                             const BlendVertex*       blendData,
                             uint32_t                 vertexCount,
                             uint32_t                 baseMeshDescIdx);

    // All-in-one registration path: creates UV/index/tangent buffers, builds
    // the base MeshDescriptor, then delegates to RegisterSkinnedMesh. Also
    // attaches SkeletonComponent + AnimationComponent (+ auto-detected
    // ChainPhysicsComponent on hair/skirt bones) to the character root.
    bool RegisterSkinnedMeshFull(IGraphicsDevice& gfx,
                                 MeshManager&     meshMgr,
                                 World&           world,
                                 const SceneLoader::SkinnedMeshPending& pending);

    // Re-registers the world-scoped UV0-UV3 / tangent / index buffer slots and the base
    // MeshDescriptor of components restored from an Editor Play snapshot: the GPUBuffer
    // handles outlive the snapshot, but MeshDescriptorHeap::OnWorldClear invalidates the
    // numeric slots cached in the component. Returns the number of meshes rebound.
    uint32_t RebindRestoredWorldDescriptors(World& world, MeshManager& meshMgr);

    // Registries.
    SkeletonRegistry&  GetSkeletonRegistry()  { return m_skeletonRegistry;  }
    ClipLibrary&       GetClipLibrary()       { return m_clipLibrary;       }
    MorphClipLibrary&  GetMorphClipLibrary()  { return m_morphClipLibrary;  }

    // ECS systems (may be null before Init).
    AnimGraphSystem* GetAnimGraphSystem() { return m_animGraphSystem.get(); }
    AnimationSystem*     GetAnimationSystem()    { return m_animSystem.get();    }
    IKRigSystem*         GetIKRigSystem() { return m_ikRigSystem.get(); }
    IKSystem*            GetIKSystem()           { return m_ikSystem.get();      }
    HeadTrackSystem*     GetHeadTrackSystem()    { return m_headTrackSystem.get(); }
    // Morph-target (blend shape) layer, not face shading (SDF face shadows).
    FacialAnimationSystem* GetFacialAnimationSystem() { return m_facialSystem.get(); }
    // A module's chain stages (Anim/AnimChainHook.h), called in the order added. Not owned.
    void AddChainHook(IAnimChainHook* hook) { m_chainHooks.push_back(hook); }
    const std::vector<IAnimChainHook*>& GetChainHooks() const { return m_chainHooks; }
    CharacterStateSystem* GetCharacterStateSystem() { return m_characterStateSystem.get(); }
    void SetAnimationClipSystem(Resource::AnimationClipSystem* cs);
    void                 SetPhysicsSystem(DX12Physics::PhysicsSystem* p) { m_physics = p; }
    DX12Physics::PhysicsSystem* GetPhysicsSystem() { return m_physics; }
    ChainPhysicsSystem*  GetChainPhysicsSystem() { return m_chainPhysicsSystem.get(); }
    LocalToWorldSystem*  GetLocalToWorldSystem() { return m_localToWorldSystem.get(); }
    SocketSystem*        GetSocketSystem()       { return m_socketSystem.get();  }
    FollowSystem*        GetFollowSystem()       { return m_followSystem.get();  }
    SkinMatrixSystem*    GetSkinMatrixSystem()   { return m_skinMatrixSystem.get(); }

    // GPU rings (non-const: callers write into them each frame).
    PoseRingBuffer&    GetPoseRingBuffer()    { return m_poseBuffer; }
    SkinnedVertexRing& GetVertexRing()        { return m_vertRing;   }

    // Pass + jobs.
    SkinningPass*                        GetSkinningPass()     { return m_skinningPass; }
    // Half-keyed: BuildSkinJobs fills the write side on the game half; SkinningPass and
    // Render_ComputePrepass read the published one.
    // Do not publish at the end of BuildSkinJobs: it runs in the scheduler's Animation
    // phase, early in game frame F+1, while the render half still renders F. The jobs are
    // published with the rest of the scene payload (the end of Renderer::BeginFrame, or
    // Renderer::FinishFramePreparation when preparation is deferred).
    std::vector<SkinDispatchDesc>&       SkinJobsWrite()   { return m_skinPayload[m_skinWrite].jobs; }
    const std::vector<SkinDispatchDesc>& SkinJobsRead()    { return m_skinPayload[m_skinRead ].jobs; }
    std::vector<float>&                  MorphPoolWrite()  { return m_skinPayload[m_skinWrite].morphWeights; }
    // Flips the buffers and re-points SkinningPass at the published ones. The pass holds
    // raw pointers, so re-pointing is part of publishing, not a separate step.
    //
    // Split along the same index / payload seam as Renderer::PublishRenderScene, because
    // frame preparation can leave main: BuildSkinJobs(F+1) runs in the Animation phase
    // inside the -animoverlap window and writes SkinJobsWrite() while SkinningPass still
    // reads SkinJobsRead() for frame F. Main advances the write cursor at hand-off
    // (AdvanceSkinJobsWrite); whoever finishes preparation publishes the read cursor
    // (PublishSkinJobsRead).
    uint32_t AdvanceSkinJobsWrite();
    void     PublishSkinJobsRead(uint32_t emitIdx);
    void PublishSkinJobs();

    // True while the write and read halves are different vectors, i.e. BuildSkinJobs
    // cannot be appending to the array SkinningPass dispatches from. PublishSkinJobs makes
    // it true and keeps it true. Pinned by -animoverlapverify, which runs BuildSkinJobs
    // beside a live render half.
    bool SkinJobHalvesDistinct() const { return m_skinWrite != m_skinRead; }

    // Per-frame state accessed by culling code.
    std::unordered_set<Entity>& GetAnimVisibleSet() { return m_animVisibleSet; }

    // Entities admitted only because they fall inside a shadow cascade: off screen, but
    // casting into one (94 of 100 characters in a crowd scene, each paying full animation
    // and cloth for a shadow-resolution silhouette). Consumers use it to reduce quality,
    // never to skip: a shadow caster that stops animating pops in the shadow map.
    std::unordered_set<Entity>& GetAnimShadowOnlySet() { return m_animShadowOnlySet; }
    bool IsAnimCullingEnabled() const                { return m_animCullingEnabled; }
    void SetAnimCullingEnabled(bool v)               { m_animCullingEnabled = v; }

    // Entity -> skinned-output MeshDescriptor slot cache (cleared on OnWorldClear).
    std::unordered_map<Entity, uint32_t>& GetSkinnedMeshDescCache() { return m_skinnedMeshDescCache; }

    // Clears the entity-keyed caches (called from Renderer::OnWorldClear). The skinned
    // vertex ring needs no re-register: Init registers it with
    // MeshDescriptorHeap::RegisterPersistentBuffer, so posBindlessIdx[] / nrmBindlessIdx[] /
    // tanBindlessIdx[] stay valid across world reloads.
    //
    // @p idsPreserved mirrors Renderer::OnWorldClear's flag:
    //  - false (scene change): the outgoing world's skinned GPU buffers are released with
    //    a one-world lag, the two-stage pattern of Renderer's meshlib release (an inline
    //    free, even after a full sync, gives a delayed TDR through stale bindless SRVs).
    //    World::Clear fires no destroy listeners, so this is the only place they can be
    //    freed.
    //  - true (ExitPlay snapshot restore): buffers are kept, since the restored components
    //    reference the same handles; the follow-up RebindRestoredWorldDescriptors
    //    reconciles ownership and queues what the restored world does not reference.
    //
    // @p worldSurvives = true is the bake-preview restore (EditorLayer releases a preview
    // meshlib and calls Renderer::OnWorldClear only to flush descriptor caches; the World
    // and its skinned entities keep rendering): caches are flushed, but the ownership
    // sweep, the pending drain and the undo-park drop are skipped.
    void OnWorldClear(bool idsPreserved, bool worldSurvives = false);

    // EnterPlay / ExitPlay bracket (set from Renderer::CaptureEditorWorldResources /
    // RestoreEditorWorldResources). While true, an editor Play snapshot holds value-copies
    // of skinned components: their GPU handles must never reach m_pendingWorldRelease (a
    // mid-play Lua Scene.Load runs the idsPreserved=false path). The sweep diverts them to
    // the orphan list and the Stop-time reconcile decides by reference.
    void SetPlaySnapshotActive(bool active) { m_playSnapshotActive = active; }

    // Unless @p e is parked for undo, frees its GPU slots and owned buffers and drops its
    // desc-slot cache entry; always drops it from the anim-visible set. Fired from World's
    // entity-destroy listener (wired by Renderer::SubscribeToWorld): without it a
    // CreateEntity that recycles the id inherits stale mesh-desc indices. @p mesh is the
    // entity's MeshSkinnedComponent as it died (the live one on the immediate path, a copy
    // on the deferred one), or null when it had none.
    void OnEntityDestroyed(Entity e, const MeshSkinnedComponent* mesh);

    // ---- Editor-undo slot parking ----
    // Deleting a skinned mesh frees its descriptor-heap mesh slot + its six bindless
    // buffer slots (OnEntityDestroyed); after a delete an undo can resurrect, the restored
    // component would point at freed / recycled slots -> GPU hang. ParkForUndo (call it
    // before DestroyEntity) captures the slots, OnEntityDestroyed then skips freeing them,
    // and a value-copy restore reuses the same valid slots and draws at once. The
    // refcounted GPU buffers are held too, so the slots stay backed after the pool copy is
    // erased.
    void ParkForUndo(World& world, Entity e);
    // Undo restored the entity: the live component re-owns the slots. Forgets the parked
    // record without freeing (the slots are in use again).
    void UnparkForUndo(Entity e);
    // The undo action was dropped without resurrecting: frees the parked slots.
    void FreeUndoParked(MeshManager& meshMgr, Entity e);

    // ---- Buffers that were never snapshotted ----
    // The editor host parks every released owned buffer on the orphan list, because a Play
    // snapshot or an undo record might still hold a value-copy of the component
    // (ReleaseOwnedBuffers). The Chain Physics Preview stage can prove neither does: it is
    // torn down before EnterPlay's CloneState, its spawn is not undo-recorded and it is
    // never undo-parked; parking it would pile ~10 buffers per close / subject change onto
    // the orphan list for the session.
    // Call this before the entity is destroyed: the ReleaseOwnedBuffers its
    // OnEntityDestroyed runs then queues the buffers on the deferred DestroyBuffer path.
    // Do not free the buffers here instead: OnEntityDestroyed frees the bindless slots
    // first and MeshDescriptorHeap::FreeBuffer finds a slot by resource pointer, so a
    // buffer destroyed first leaks its slot and leaves a stale pointer in the heap's dedup
    // map. Ignored (the ordinary park runs) while a Play snapshot is active or the entity
    // is undo-parked. The mark is consumed by the next release and dropped on a world
    // clear.
    void MarkBuffersUnsnapshotted(Entity e);
    // Harness census: entities owning skinned buffers, released buffers on the orphan
    // list, and buffers the unsnapshotted path has released since boot.
    uint32_t OwnedGpuBufferEntityCount() const
    { return static_cast<uint32_t>(m_ownedGpuBuffers.size()); }
    uint32_t OrphanedGpuBufferCount() const
    { return static_cast<uint32_t>(m_orphanedGpuBuffers.size()); }
    uint64_t UnsnapshottedBuffersReleased() const { return m_unsnapshottedReleased; }
    // Entries in the shared skinned-source cache (world-scoped, released only
    // by a scene-changing world clear).
    uint32_t SharedSourceCount() const
    { return static_cast<uint32_t>(m_sharedSources.size()); }
    // Whether a part keyed @p sourceKey would take the shared path right now
    // (SkinnedMeshPending::sharedOnly). Game half, like registration.
    bool HasSharedSource(uint64_t sourceKey) const
    { return sourceKey != 0 && m_sharedSources.find(sourceKey) != m_sharedSources.end(); }

    // SharedSourceDerived / DeriveSharedSource: the per-vertex results a part's shared
    // source carries (bounds, rest extent, influence bones, morph slot tables, per-target
    // reach), derived from the part's streams. Pure CPU on any thread (a preload's worker),
    // so the main step that builds the source only creates buffers. @p boneCount /
    // @p skelMorphCount: the skeleton's as registered, virtual bones included. Logs the
    // blend check.
    static void DeriveSharedSource(const SceneLoader::SkinnedMeshPending& pending, uint32_t boneCount,
                                   uint32_t skelMorphCount, SharedSourceDerived& out);
    // Shared-source preload: builds and caches a part's shared source (every buffer, SRV and
    // per-vertex result the shared path hands an instance) with no instance yet, so the
    // first spawn takes the shared path. @p pending carries the decoded streams, the
    // sourceKey and the registered skeleton (pending.entity is unused); @p derived is
    // DeriveSharedSource's result for it (nullptr: derived here). Returns true when the
    // source is live (built now, or already). Game half.
    bool PublishSharedSource(IGraphicsDevice& gfx, const SceneLoader::SkinnedMeshPending& pending,
                             const SharedSourceDerived* derived = nullptr);

    // The same source built over several calls, a few buffers each (a preload's budgeted
    // main steps): rest positions / normals / blend, then the UVs / index / tangent, then
    // the morph block - each its own upload batch, its buffers in a heap of their size -
    // and the source is cached on the last. @p pending must outlive the build.
    // BeginSourceBuild returns nullptr when there is nothing to build (no key, invalid, or
    // the source is live already). StepSourceBuild: true = more to do; false = finished
    // (cached, or failed: logged). A build dropped unfinished destroys what it created.
    // Game half, like registration.
    std::shared_ptr<SourceBuild> BeginSourceBuild(IGraphicsDevice& gfx, const SceneLoader::SkinnedMeshPending& pending,
                                                  const SharedSourceDerived* derived);
    bool StepSourceBuild(SourceBuild& build);
    // Runs every stage of a begun build at once on this thread - a TaskSystem worker
    // inside the device's loader upload scope (the stages touch only the device and the
    // build) - and leaves the cache insert to PublishSourceBuild. Returns false when a
    // stage failed (logged; the build frees what it made when it goes).
    static bool BuildSourceOnLoader(SourceBuild& build);
    // Game half: a BuildSourceOnLoader build into the cache, unless another
    // build of its key landed first (then its buffers go with the build).
    bool PublishSourceBuild(SourceBuild& build);

    // ---- Init state and morph pool weights ----
    bool IsInitialised() const { return m_initialised; }

    // False until a BuildSkinJobs has run after the first call (the call arms the copy),
    // and for an entity that drove no skinned mesh with morph targets.
    bool ReadPoolWeights(Entity skeleton, std::vector<std::pair<uint16_t, float>>& out);

private:
    struct SkinPayload
    {
        std::vector<SkinDispatchDesc> jobs;
        std::vector<float>            morphWeights;
        // Ring slot the jobs' prevPosSrcElementBase values index into: the slot last frame's
        // BuildSkinJobs wrote. 0xFFFFFFFF on the first frame and after a frame that built no
        // jobs.
        uint32_t                      prevPosSlot = 0xFFFFFFFFu;
        // Ring slot the jobs' own outPosByteOffset values were allocated from, i.e. the game
        // half's slot when this payload was built. The render half must bind that slot's UAV
        // or every offset in the payload addresses the wrong buffer (see the SPLITCHECK in
        // SkinningPass).
        uint32_t                      curPosSlot  = 0xFFFFFFFFu;
    };

    struct SkinRoot { Entity entity; size_t extentIndex; };

    struct UndoParkedSlots
    {
        uint32_t       meshDescSlot = 0xFFFFFFFFu;
        RHI::GPUBuffer uvBuffer, uv1Buffer, uv2Buffer, uv3Buffer, tangentBuffer, indexBuffer;
        // Copied from the component: a borrower's bindless slots + buffers are
        // cache-owned, so dropping the undo record must free neither.
        uint64_t       sharedSourceKey = 0;
    };

    // ---- Skinned GPU-buffer ownership ----
    struct OwnedGpuBuffers
    {
        RHI::GPUBuffer bufs[10]; // blend, restPos, restNrm, uv0, uv1, tangent, index, morph, uv2, uv3
    };

    // ---- Shared source geometry (see SkinnedMeshPending::sourceKey) ----
    struct SharedSkinnedSource
    {
        // Same order as OwnedGpuBuffers::bufs.
        RHI::GPUBuffer bufs[10]; // blend, restPos, restNrm, uv0, uv1, tangent, index, morph, uv2, uv3

        uint64_t blendSRVHandle   = 0;
        uint64_t restPosSRVHandle = 0;
        uint64_t restNrmSRVHandle = 0;
        uint64_t restTanSRVHandle = 0;
        uint64_t morphDeltaSRVHandle = 0;

        // Bindless slot indices are deliberately not cached: the heap rewinds its table on
        // every world clear, so a stored index goes stale; RegisterBuffer dedupes by resource
        // pointer, so re-registering per instance is cheap and always correct.

        // Derived per-vertex results, identical for every instance; each is an
        // O(vertexCount) pass (restExtent sqrt, AABB min/max, an influence-bone set insert per
        // vertex per influence), so it runs once per part.
        DirectX::XMFLOAT3     aabbMin{}, aabbMax{};
        float                 restExtent = 0.f;
        std::vector<uint16_t> influenceBones;

        uint32_t vertexCount      = 0;
        uint32_t indexCount       = 0;
        uint32_t morphTargetCount = 0;
        bool     morphHasNormals  = false;

        // The mesh's local <-> skeleton morph slot tables. Derived only from the .imorph
        // block + the .iskel morph list, so identical for every instance of this part, like
        // influenceBones.
        std::vector<uint32_t> morphSlotOfLocal;
        std::vector<int16_t>  morphLocalOfSlot;
        std::vector<float>    morphMaxDelta;   // per local target, its farthest displacement
    };

    // ---- Skinned GPU-buffer ownership ----
    void RecordOwnedBuffers(Entity e, const MeshSkinnedComponent& c);
    // RHI::GPUBuffer is a plain handle with no RAII. The subsystem tracks, keyed by
    // entity, the up-to-10 buffers RegisterSkinnedMesh(Full) creates per part (blend /
    // restPos / restNrm / uv / uv1 / tangent / index / morphDelta / uv2 / uv3) and is
    // their only destroyer; a missed one orphans a whole character set and pins its
    // 16 MiB buffer-batch heap.
    //  - re-registration / non-parked entity destroy / dropped undo record ->
    //    ReleaseOwnedBuffers: the Editor host moves the handles to m_orphanedGpuBuffers
    //    (a Play snapshot may still reference them; RebindRestoredWorldDescriptors
    //    reconciles by reference); the Game host destroys them at once (no snapshots).
    //  - scene change (OnWorldClear idsPreserved=false): owned + orphans move to
    //    m_pendingWorldRelease, destroyed at the next world clear (one-world lag, as for
    //    the meshlib).
    // Undo parking keeps the owned record in place: a value-copy resurrection reuses the
    // same GPU handles.
    void ReleaseOwnedBuffers(Entity e);

    // ---- Shared source geometry (see SkinnedMeshPending::sourceKey) ----
    // Builds one keyed part's source from its decoded streams: the buffers and the
    // derived data. Nothing is kept on failure. No entity and no bindless registration
    // (the shared path registers per instance, deduplicated by resource).
    bool BuildSharedSource(IGraphicsDevice& gfx, const SceneLoader::SkinnedMeshPending& pending,
                           const SharedSourceDerived* derived, SharedSkinnedSource& out);
    // BuildSharedSource's stages (1 rest + blend, 2 uvs / index / tangent, 3 the
    // morph block + the derived results): false = a required buffer failed.
    static bool BuildSourceStage(IGraphicsDevice& gfx, const SceneLoader::SkinnedMeshPending& pending,
                                 const SharedSourceDerived& derived, int stage, SharedSkinnedSource& s);
    static uint64_t SourceStageHeapBytes(const SceneLoader::SkinnedMeshPending& pending,
                                         const SharedSourceDerived& derived, int stage);
    friend struct SourceBuild;

    // Free every cache entry's buffers + bindless slots. World-scoped, so this
    // runs only on a real teardown (never on the ExitPlay id-preserving path,
    // where the restored components still reference these handles).
    void ReleaseSharedSources();

    // ---- Registration helpers and morph remap ----
    // Attach SkeletonComponent/AnimationComponent/MorphComponent/ChainPhysics
    // to the character root (and SkeletonRef to the part). Shared by the normal
    // and shared-source registration paths so the two cannot drift apart.
    void AttachSkeletonForPending(World& world,
                                  const SceneLoader::SkinnedMeshPending& pending);

    // Recycle whatever the entity held from a PREVIOUS registration: its mesh
    // descriptor slot always, and its bindless slots + GPU buffers only when it
    // owned them (sharedSourceKey == 0).
    void ReleasePreviousSkinnedRegistration(World& world, MeshManager& meshMgr, Entity e);

    // Morph channel remap: mesh target slot -> clip channel (-1 = unmatched), cached per
    // (skeleton assetIndex, morph clip handle). The inputs are immutable loaded assets, so
    // it is resolved once instead of a name -> channel map per part per frame. Cleared on
    // world clear (asset indices recycle).
    const std::vector<int16_t>& GetMorphRemap(uint32_t assetIndex, uint32_t clipHandle,
                                              const SkeletonAsset& skelAsset,
                                              const MorphClipAsset& clip);

    // ---- Registries, systems and skinning state ----
    // Registries.
    SkeletonRegistry m_skeletonRegistry;
    ClipLibrary      m_clipLibrary;
    MorphClipLibrary m_morphClipLibrary;

    // GPU rings.
    PoseRingBuffer    m_poseBuffer;
    SkinnedVertexRing m_vertRing;

    // ECS systems.
    std::unique_ptr<AnimationSystem>    m_animSystem;
    std::unique_ptr<AnimGraphSystem> m_animGraphSystem;
    std::unique_ptr<IKSystem>           m_ikSystem;
    std::unique_ptr<HeadTrackSystem>    m_headTrackSystem;
    // Holds no per-world map: every cache lives on FacialAnimationComponent behind the
    // HeadTrack-style "cached slot fell outside the asset or the name changed" guard, so
    // OnWorldClear needs no hook for it.
    std::unique_ptr<FacialAnimationSystem> m_facialSystem;
    std::vector<IAnimChainHook*> m_chainHooks;
    std::unique_ptr<IKRigSystem> m_ikRigSystem;
    std::unique_ptr<CharacterStateSystem> m_characterStateSystem;
    // Borrowed; App sets it through SetPhysicsSystem after PhysicsSystem::Init.
    DX12Physics::PhysicsSystem*         m_physics = nullptr;
    std::unique_ptr<ChainPhysicsSystem> m_chainPhysicsSystem;
    std::unique_ptr<LocalToWorldSystem> m_localToWorldSystem;
    std::unique_ptr<SocketSystem>       m_socketSystem;
    std::unique_ptr<FollowSystem>       m_followSystem;
    std::unique_ptr<SkinMatrixSystem>   m_skinMatrixSystem;

    // SkinningPass (owned; non-owning pointer exposed to Render()).
    std::unique_ptr<SkinningPass> m_skinPassOwned;
    SkinningPass*                 m_skinningPass = nullptr;

    // Per-frame job list built by BuildSkinJobs, consumed by SkinningPass::Execute.
    // morphWeights backs SkinDispatchDesc::morphWeightOffset (refilled each build; only
    // jobs with morph targets use it). The jobs and the weights share that index space,
    // so they are one payload: split them and the offsets index the wrong pool.
    SkinPayload m_skinPayload[2];
    uint32_t    m_skinWrite = 0;
    uint32_t    m_skinRead  = 0;

    // Morph.GetPoolWeights: what BuildSkinJobs last wrote into the pool, per skeleton
    // entity and skeleton morph slot (the max over the mesh parts sharing a slot). Nothing
    // is copied until the first read arms it; from then on every BuildSkinJobs refreshes
    // it and a read answers the latest.
    std::atomic<bool> m_poolSnapshotArmed{ false };
    std::mutex        m_poolSnapshotMutex;
    std::unordered_map<Entity, std::vector<std::pair<uint16_t, float>>> m_poolSnapshot;

    // Caches keyed by entity.
    std::unordered_map<Entity, uint32_t>      m_skinnedMeshDescCache;

    // Owner-only scratch, rebuilt from the current pools each BuildSkinJobs.
    // Dense indices never survive a build (pool erase can swap its last slot).
    std::vector<SkinRoot> m_skinRoots;
    std::vector<float> m_skinRootExtents;
    // Monotonic BuildSkinJobs counter. Only ever compared for "was the last
    // build the immediately preceding one", which is what decides whether an
    // entity's previous-frame skinned positions are still addressable.
    uint64_t m_skinBuildIndex = 0;
    // Ring slot the PREVIOUS BuildSkinJobs allocated its positions out of.
    uint32_t m_lastSkinPosSlot = 0xFFFFFFFFu;

    // Editor-undo: slots held alive across a delete so a resurrect can reuse them.
    // Holds the GPU buffers (refcount) so FreeBuffer still works on discard even
    // after the component pool copy is gone. Keyed by the (parked) entity id,
    // which the undo action reserves so it can't be reissued meanwhile.
    std::unordered_map<Entity, UndoParkedSlots> m_undoParked;

    // Anim-culling: the previous frame's visible set that gated animation work.
    std::unordered_set<Entity> m_animVisibleSet;
    bool                       m_animCullingEnabled = true;
    std::unordered_set<Entity> m_animShadowOnlySet;

    // One entry per distinct character mesh part; every instance of that part borrows
    // these handles instead of uploading its own copy. The cache owns them: entity
    // destroy, re-registration and undo-drop all skip a component whose
    // sharedSourceKey != 0, and OnWorldClear releases the whole cache with the rest of the
    // world's GPU memory. That is the lifetime MeshLibrary gives static meshes, and why
    // this needs no refcount: borrowing is world-scoped, not entity-scoped.
    std::unordered_map<uint64_t, SharedSkinnedSource> m_sharedSources;

    std::unordered_map<uint64_t, std::vector<int16_t>> m_morphRemapCache;
    std::unordered_map<Entity, OwnedGpuBuffers> m_ownedGpuBuffers;
    std::vector<RHI::GPUBuffer>                 m_orphanedGpuBuffers;
    std::vector<RHI::GPUBuffer>                 m_pendingWorldRelease;
    bool                                        m_playSnapshotActive = false;
    // Entities whose next ReleaseOwnedBuffers destroys rather than parks
    // (MarkBuffersUnsnapshotted). Cleared on every world clear: entity ids recycle across
    // one.
    std::unordered_set<Entity>                  m_unsnapshotted;
    uint64_t                                    m_unsnapshottedReleased = 0;

    // Borrowed (from Init): lets OnEntityDestroyed recycle the destroyed skinned mesh's
    // MeshDescriptorHeap slot + unique bindless buffer slots.
    MeshManager* m_meshMgr = nullptr;
    // Borrowed (from Init): buffer destruction (deferred internally).
    IGraphicsDevice* m_gfx = nullptr;

    bool m_initialised = false;
};
