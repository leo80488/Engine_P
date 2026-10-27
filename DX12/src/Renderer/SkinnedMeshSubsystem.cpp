#include "Renderer/SkinnedMeshSubsystem.h"
#include "Graphics/AnimProfile.h"
#include <atomic>
#include <cstdlib>
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/MeshManager.h"
#include "ECS/ECS.h"
#include "Physics/ChainBoneNames.h"   // TriggersAutoAttach: the auto-attach name rule
#include "Resource/EngineDevSettings.h"   // PeekEngineDevSkinVertexBudget
#include "Settings/Scalability.h"         // the first boot's tier: the adapter-sized budget
#include "System/Check.h"        // ENGINE_CHECK_ONCE: kMaxSkinJobs overflow visibility
#include "System/HostPolicy.h"   // Sys::IsEditorHost: buffer-release policy
#include "System/Log.h"
#include "System/CommandLine.h"
#include "System/TaskSystem.h"     // BuildSkinJobs pass 2

#include <algorithm>
#include <cstring>
#include <unordered_set>

using DirectX::XMFLOAT3;
using DirectX::XMFLOAT4X4;

namespace
{
    // RAII buffer-upload batch scope. A CreateBuffer inside a batch defers its per-buffer
    // FlushUploadAndWait() (a full GPU fence round trip) until the outermost scope closes;
    // the counter nests, so wrapping a function and its caller loop gives one flush. RAII
    // runs EndBufferUploadBatch() on every early return.
    struct ScopedUploadBatch
    {
        IGraphicsDevice& gfx;
        explicit ScopedUploadBatch(IGraphicsDevice& g) : gfx(g) { gfx.BeginBufferUploadBatch(); }
        ~ScopedUploadBatch() { gfx.EndBufferUploadBatch(); }
        ScopedUploadBatch(const ScopedUploadBatch&) = delete;
        ScopedUploadBatch& operator=(const ScopedUploadBatch&) = delete;
    };

    // RAII cleanup of RegisterSkinnedMeshFull's locally created buffers (uv0..uv3 /
    // index / tangent) on every failure return after the first create. Unless Dismiss()
    // ran (the component took the handles and owns them through RecordOwnedBuffers), the
    // destructor frees every valid handle: the bindless slot first (FreeBuffer is keyed by
    // the buffer and no-ops when it was never registered), then the buffer (DestroyBuffer
    // defers internally).
    struct ScopedBufferGuard
    {
        IGraphicsDevice&    gfx;
        MeshDescriptorHeap& heap;
        RHI::GPUBuffer*     bufs[6];
        bool                dismissed = false;

        ScopedBufferGuard(IGraphicsDevice& g, MeshDescriptorHeap& h,
                          RHI::GPUBuffer* b0, RHI::GPUBuffer* b1,
                          RHI::GPUBuffer* b2, RHI::GPUBuffer* b3,
                          RHI::GPUBuffer* b4, RHI::GPUBuffer* b5)
            : gfx(g), heap(h), bufs{ b0, b1, b2, b3, b4, b5 } {}
        ~ScopedBufferGuard()
        {
            if (dismissed) return;
            for (RHI::GPUBuffer* b : bufs)
            {
                if (!b || !b->IsValid()) continue;
                heap.FreeBuffer(*b);
                gfx.DestroyBuffer(*b);
            }
        }
        void Dismiss() { dismissed = true; }
        ScopedBufferGuard(const ScopedBufferGuard&) = delete;
        ScopedBufferGuard& operator=(const ScopedBufferGuard&) = delete;
    };

    // The skinned vertex ring's budget for this run, and where it came from (@p source, for the
    // ring's init log). First match: -skinvertexbudget, the skinning.vertexBudget row (engine.json, or
    // -set:), else the adapter - the floor under 4 GB of video memory (the first boot's Low tier), the
    // default otherwise. A value out of range keeps the adapter's.
    uint32_t ResolveSkinVertexBudget(IGraphicsDevice& gfx, const char*& source)
    {
        const bool lowTier = Settings::Scalability::FirstBootLevel(
                                 Settings::Scalability::FirstBootVideoMemoryMB(gfx.GetAdapterInfo())) == 0;
        const uint32_t byAdapter = lowTier ? SkinnedVertexRing::kMinSkinnedVertsPerFrame
                                           : SkinnedVertexRing::kMaxSkinnedVertsPerFrame;
        source = lowTier ? "the adapter (Low tier)" : "the adapter";
        int requested = Resource::PeekEngineDevSkinVertexBudget();
        if (requested > 0) source = "skinning.vertexBudget";
        // -skinvertexbudget=N: this run's skinned vertex budget (vertices per frame), over engine.json's
        // skinning.vertexBudget; 1048576 .. 33554432.
        if (const int flag = Sys::CommandLine::Get().GetInt("-skinvertexbudget", 0); flag > 0)
        {
            requested = flag;
            source    = "-skinvertexbudget";
        }
        if (requested <= 0)
            return byAdapter;
        if (ENGINE_CHECK(requested >= static_cast<int>(SkinnedVertexRing::kMinSkinnedVertsPerFrame) &&
                         requested <= static_cast<int>(SkinnedVertexRing::kMaxSkinVertexBudget),
                         "SkinnedVertexRing: %s asks for %d vertices a frame, outside [%u, %u] - kept %u",
                         source, requested, SkinnedVertexRing::kMinSkinnedVertsPerFrame,
                         SkinnedVertexRing::kMaxSkinVertexBudget, byAdapter))
            return static_cast<uint32_t>(requested);
        source = lowTier ? "the adapter (Low tier)" : "the adapter";
        return byAdapter;
    }
}

void SkinnedMeshSubsystem::Init(IGraphicsDevice& gfx, MeshManager& meshMgr)
{
    m_meshMgr = &meshMgr;   // borrowed; OnEntityDestroyed recycles GPU slots through it
    m_gfx     = &gfx;       // borrowed; skinned GPU-buffer ownership destroys through it

    m_poseBuffer.Init(gfx);
    const char* budgetSource = nullptr;
    const uint32_t vertexBudget = ResolveSkinVertexBudget(gfx, budgetSource);
    m_vertRing.Init(gfx, vertexBudget, budgetSource);

    // Register every ring slot's pos / nrm / tan buffer in the bindless table; each frame
    // in flight has its own buffer, so the GPU reads frame N-1 while the CPU writes N.
    // Use the persistent (engine-lifetime) register path: the ring lives as long as the
    // app, so its bindless slots must survive every world reload. A world-scoped slot is
    // invalidated by MeshDescriptorHeap::OnWorldClear, and UpdateMeshThisFrame would then
    // point posBindlessIdx[] at the new world's mesh VB (skinned characters sample another
    // mesh's vertex bytes and vanish).
    auto& descHeap = meshMgr.GetDescriptorHeap();
    for (uint32_t i = 0; i < SkinnedVertexRing::kFramesInFlight; ++i)
    {
        // Names the slot directly: registration is per-slot setup, not a frame, so the ring
        // is not rotated to reach it.
        m_vertRing.posBindlessIdx[i] = descHeap.RegisterPersistentBuffer(m_vertRing.PosBufferForSlot(i));
        m_vertRing.nrmBindlessIdx[i] = descHeap.RegisterPersistentBuffer(m_vertRing.NrmBufferForSlot(i));
        m_vertRing.tanBindlessIdx[i] = descHeap.RegisterPersistentBuffer(m_vertRing.TanBufferForSlot(i));
    }
    m_vertRing.BeginFrame(0);

    m_animSystem         = std::make_unique<AnimationSystem>(m_skeletonRegistry, m_clipLibrary, m_morphClipLibrary);
    m_animGraphSystem = std::make_unique<AnimGraphSystem>(*m_animSystem, m_skeletonRegistry, m_clipLibrary);
    m_ikSystem           = std::make_unique<IKSystem>(*m_animSystem, m_skeletonRegistry);
    m_headTrackSystem    = std::make_unique<HeadTrackSystem>(*m_animSystem, m_skeletonRegistry);
    // SkeletonRegistry only: the facial layer resolves morph names to mesh slots and
    // never touches a pose, so unlike HeadTrackSystem it needs no AnimationSystem.
    m_facialSystem       = std::make_unique<FacialAnimationSystem>(m_skeletonRegistry);
    m_ikRigSystem = std::make_unique<IKRigSystem>(*m_animSystem,m_skeletonRegistry);
    m_ikRigSystem->SetGraphSystem(m_animGraphSystem.get());
    m_characterStateSystem = std::make_unique<CharacterStateSystem>(m_skeletonRegistry, nullptr);
    m_chainPhysicsSystem = std::make_unique<ChainPhysicsSystem>(*m_animSystem, m_skeletonRegistry);
    m_localToWorldSystem = std::make_unique<LocalToWorldSystem>(*m_animSystem, m_skeletonRegistry);
    m_socketSystem       = std::make_unique<SocketSystem>(m_poseBuffer, m_skeletonRegistry);
    m_followSystem       = std::make_unique<FollowSystem>();
    m_skinMatrixSystem   = std::make_unique<SkinMatrixSystem>(m_skeletonRegistry);

    auto pass = std::make_unique<SkinningPass>();
    pass->SetBuffers(&m_poseBuffer, &m_vertRing);
    m_skinningPass = pass.get();
    m_skinningPass->Init(gfx);
    m_skinPassOwned = std::move(pass);

    m_initialised = true;
    LOG_SUCCESS("SkinnedMeshSubsystem: initialised");
}

void SkinnedMeshSubsystem::SetAnimationClipSystem(Resource::AnimationClipSystem* cs)
{
    if (m_characterStateSystem) m_characterStateSystem->SetClipSystem(cs);
    if (m_animGraphSystem) m_animGraphSystem->SetClipSystem(cs);
    if(cs)cs->SetGraphSystem(m_animGraphSystem.get());
}

void SkinnedMeshSubsystem::OnWorldClear(bool idsPreserved, bool worldSurvives)
{
    if (m_animGraphSystem && !worldSurvives) m_animGraphSystem->Reset();
    m_skinnedMeshDescCache.clear();
    // Entity-keyed like everything else here: a mark carried across a clear would let a
    // recycled id's buffers skip the orphan park.
    m_unsnapshotted.clear();
    m_animVisibleSet.clear();
    // Keyed by (skeleton assetIndex, morph clip handle); both recycle onto different
    // assets in the next world, so the mapping must not survive.
    m_morphRemapCache.clear();
    // Nothing to clear for m_facialSystem, by design: it keeps no subsystem-side map.
    // Every name -> slot cache lives on FacialAnimationComponent, which dies with the
    // world or, for a surviving world, self-heals through FacialAnimationSystem's
    // "assetIndex changed, cached slot outside the asset, or name changed" guard. If it
    // ever grows an assetIndex-keyed map, clear it here.

    // Parked records survive every clear flavor, but their numeric mesh-desc slot dies
    // with the MeshDescriptorHeap rewind Renderer::OnWorldClear always triggers
    // (m_meshMgr.OnWorldClear, the worldSurvives flavor included): a later FreeUndoParked
    // calling FreeMesh on the stale number would wipe whatever live mesh the recycled slot
    // names. FreeBuffer is keyed by the buffer handle (a no-op lookup after the reset), so
    // those fields stay for a best-effort free.
    //
    // Do not clear the records here: EditHistory outlives these flows (it survives
    // Play / Stop, and a mid-play Lua Scene.Load reaches the idsPreserved=false path
    // without touching the undo stack). StructuralAction OnDiscard -> FreeUndoParked is
    // the single release point for parked sets; on an edit-mode scene change EditorLayer
    // clears the history and that path frees everything, in either call order (the
    // ownership registry is single-owner, so the second caller finds nothing).
    for (auto& [e, parked] : m_undoParked)
        parked.meshDescSlot = 0xFFFFFFFFu;

    if (worldSurvives)
    {
        // Bake-preview restore: the World and its skinned entities keep rendering; only the
        // descriptor caches needed the flush. Do not touch ownership, orphans, the pending
        // queue or the undo park here: that would queue live buffers for destruction.
        return;
    }

    // Drain the previous clear's queue first (one-world lag: nothing recorded since the
    // WaitIdle before that clear can reference these; the two-stage pattern of Renderer's
    // meshlib release).
    if (m_gfx && !m_pendingWorldRelease.empty())
    {
        for (RHI::GPUBuffer& b : m_pendingWorldRelease)
            if (b.IsValid()) m_gfx->DestroyBuffer(b);
        LOG_INFO("SkinnedMeshSubsystem: released %zu skinned GPU buffers from the previous world",
                 m_pendingWorldRelease.size());
        m_pendingWorldRelease.clear();
    }

    if (!idsPreserved)
    {
        // Scene change: World::Clear fires no destroy listeners, so this is the only place
        // the outgoing world's skinned buffers can be reclaimed. Undo-parked entities keep
        // their owned records through any sweep: FreeUndoParked / UnparkForUndo are their only
        // exits.
        std::unordered_map<Entity, OwnedGpuBuffers> kept;
        if (m_playSnapshotActive)
        {
            // Mid-play Lua Scene.Load: the EnterPlay snapshot still holds value-copies of these
            // handles, and Stop restores + rebinds them. Nothing may reach the destruction queue:
            // divert to the orphan list; the Stop-time reconcile decides by reference.
            for (auto& [e, owned] : m_ownedGpuBuffers)
            {
                if (m_undoParked.find(e) != m_undoParked.end())
                {
                    kept.emplace(e, owned);
                    continue;
                }
                for (RHI::GPUBuffer& b : owned.bufs)
                    if (b.IsValid()) m_orphanedGpuBuffers.push_back(b);
            }
        }
        else
        {
            // Queue owned + orphans for the next clear's drain.
            for (auto& [e, owned] : m_ownedGpuBuffers)
            {
                if (m_undoParked.find(e) != m_undoParked.end())
                {
                    kept.emplace(e, owned);
                    continue;
                }
                for (RHI::GPUBuffer& b : owned.bufs)
                    if (b.IsValid()) m_pendingWorldRelease.push_back(b);
            }
            for (RHI::GPUBuffer& b : m_orphanedGpuBuffers)
                if (b.IsValid()) m_pendingWorldRelease.push_back(b);
            m_orphanedGpuBuffers.clear();
        }
        m_ownedGpuBuffers = std::move(kept);

        // Shared sources are world-scoped and borrowed by every instance, so the scene change
        // is the one place they are reclaimed. Not on the idsPreserved path: the restored
        // components still reference these handles.
        ReleaseSharedSources();
    }
    // idsPreserved (ExitPlay restore): keep owned + orphans, which the restored components
    // reference; RebindRestoredWorldDescriptors reconciles ownership right after
    // AdoptState.
}

uint32_t SkinnedMeshSubsystem::RebindRestoredWorldDescriptors(World& world,
                                                               MeshManager& meshMgr)
{
    m_skinnedMeshDescCache.clear();

    // Do not early-out on a null pool: the ownership reconcile must still run (a restore
    // into a world with no skinned meshes leaves every tracked play-session buffer
    // unreferenced; skipping it leaks on every Play / Stop of a character-spawning scene).
    auto* pool = world.GetPool<MeshSkinnedComponent>();

    auto& heap = meshMgr.GetDescriptorHeap();
    static const std::vector<Entity>               s_noEntities;
    static std::vector<MeshSkinnedComponent>       s_noMeshes;
    const auto& entities = pool ? pool->Entities() : s_noEntities;
    auto&       meshes   = pool ? pool->Data()     : s_noMeshes;
    uint32_t rebound = 0;
    uint32_t failed = 0;

    auto registerOptional = [&heap](const RHI::GPUBuffer& buffer) {
        return buffer.IsValid()
            ? heap.RegisterBuffer(buffer)
            : RHI::kInvalidBufferIndex;
    };

    for (size_t i = 0; i < meshes.size(); ++i)
    {
        const Entity e = entities[i];
        MeshSkinnedComponent& mesh = meshes[i];

        RHI::MeshDescriptor baseDesc = mesh.baseMeshDesc;
        baseDesc.uv0.bufferIndex     = registerOptional(mesh.uvBuffer);
        baseDesc.uv1.bufferIndex     = registerOptional(mesh.uv1Buffer);
        baseDesc.uv2.bufferIndex     = registerOptional(mesh.uv2Buffer);
        baseDesc.uv3.bufferIndex     = registerOptional(mesh.uv3Buffer);
        baseDesc.tangent.bufferIndex = registerOptional(mesh.tangentBuffer);
        baseDesc.indexBufferIndex    = registerOptional(mesh.indexBuffer);
        baseDesc.vertexCount         = mesh.vertexCount;

        // RegisterSkinnedMeshFull always creates UV0 + index. If either live GPU handle fails
        // to register, do not leave the snapshot's stale descriptor index active: the draw
        // path would fetch an unrelated post-clear slot.
        if (baseDesc.uv0.bufferIndex == RHI::kInvalidBufferIndex ||
            baseDesc.indexBufferIndex == RHI::kInvalidBufferIndex)
        {
            mesh.meshDescriptorIdx = RHI::kInvalidBufferIndex;
            ++failed;
            LOG_ERROR("SkinnedMeshSubsystem: failed to rebind restored mesh buffers (entity %u)", e);
            continue;
        }

        const uint32_t descIdx = heap.RegisterMesh(baseDesc);
        if (descIdx == RHI::kInvalidBufferIndex)
        {
            mesh.meshDescriptorIdx = RHI::kInvalidBufferIndex;
            ++failed;
            LOG_ERROR("SkinnedMeshSubsystem: descriptor heap full while rebinding restored entity %u", e);
            continue;
        }

        mesh.baseMeshDesc = baseDesc;
        mesh.meshDescriptorIdx = descIdx;
        m_skinnedMeshDescCache[e] = descIdx;
        ++rebound;
    }

    // ---- Ownership reconcile ----
    // The play session may have destroyed snapshotted entities (their buffers sit in
    // m_orphanedGpuBuffers and the restored components reference them again), spawned and
    // registered new skinned entities (owned records nothing references any more), or
    // re-registered existing ones. The restored pool is the single source of truth: a
    // tracked handle it references stays alive, everything else goes on the one-world-lag
    // release list. Owned records are then rebuilt from the pool.
    {
        std::unordered_set<uint32_t> live;
        auto addLive = [&live](const RHI::GPUBuffer& b) {
            if (b.IsValid()) live.insert(b.handleId);
        };
        for (const MeshSkinnedComponent& mesh : meshes)
        {
            addLive(mesh.blendBuffer);   addLive(mesh.restPosBuffer);
            addLive(mesh.restNrmBuffer); addLive(mesh.uvBuffer);
            addLive(mesh.uv1Buffer);     addLive(mesh.tangentBuffer);
            addLive(mesh.uv2Buffer);
            addLive(mesh.uv3Buffer);
            addLive(mesh.indexBuffer);   addLive(mesh.morphDeltaBuffer);
        }

        size_t queued = 0;
        auto releaseUnlessLive = [&](RHI::GPUBuffer& b) {
            if (!b.IsValid()) return;
            if (live.count(b.handleId)) return;   // restored world still uses it
            m_pendingWorldRelease.push_back(b);
            ++queued;
        };
        for (RHI::GPUBuffer& b : m_orphanedGpuBuffers)
            releaseUnlessLive(b);
        m_orphanedGpuBuffers.clear();

        // Undo-parked entities are not in the restored pool (deleted before the snapshot), but
        // EditHistory survives Play / Stop and a post-Stop Ctrl+Z resurrects the component
        // with these handles. Keep their records + buffers; FreeUndoParked releases them if
        // the record drops.
        std::unordered_map<Entity, OwnedGpuBuffers> kept;
        for (auto& [e, owned] : m_ownedGpuBuffers)
        {
            if (m_undoParked.find(e) != m_undoParked.end())
            {
                kept.emplace(e, owned);
                continue;
            }
            for (RHI::GPUBuffer& b : owned.bufs)
                releaseUnlessLive(b);
        }
        m_ownedGpuBuffers = std::move(kept);

        for (size_t i = 0; i < meshes.size(); ++i)
            RecordOwnedBuffers(entities[i], meshes[i]);

        if (queued)
            LOG_INFO("SkinnedMeshSubsystem: restore reconcile queued %zu play-session "
                     "skinned GPU buffers for release", queued);
    }

    LOG_INFO("SkinnedMeshSubsystem: rebound %u restored mesh descriptors (%u failed)",
             rebound, failed);
    return rebound;
}

const std::vector<int16_t>& SkinnedMeshSubsystem::GetMorphRemap(
        uint32_t assetIndex, uint32_t clipHandle,
        const SkeletonAsset& skelAsset, const MorphClipAsset& clip)
{
    const uint64_t key = (uint64_t(assetIndex) << 32) | uint64_t(clipHandle);
    auto it = m_morphRemapCache.find(key);
    if (it != m_morphRemapCache.end()) return it->second;

    // Build once: mesh target slot -> clip channel index (-1 = no channel of
    // that name). Both name lists belong to immutable loaded assets, and the
    // cache is dropped on world clear (asset indices recycle across worlds).
    std::unordered_map<std::string, uint32_t> byName;
    byName.reserve(clip.morphCount);
    for (uint32_t ci = 0; ci < clip.morphCount; ++ci)
        byName[clip.morphNames[ci]] = ci;

    std::vector<int16_t> remap(skelAsset.morphTargetCount, int16_t(-1));
    for (uint32_t ti = 0; ti < skelAsset.morphTargetCount; ++ti)
    {
        auto f = byName.find(skelAsset.morphTargetNames[ti]);
        if (f != byName.end() && f->second <= 0x7FFFu)
            remap[ti] = static_cast<int16_t>(f->second);
    }
    return m_morphRemapCache.emplace(key, std::move(remap)).first->second;
}

void SkinnedMeshSubsystem::ReleaseSharedSources()
{
    if (m_sharedSources.empty()) return;

    size_t bufs = 0;
    for (auto& [key, src] : m_sharedSources)
    {
        if (m_meshMgr)
        {
            auto& heap = m_meshMgr->GetDescriptorHeap();
            heap.FreeBuffer(src.bufs[3]);   // uv
            heap.FreeBuffer(src.bufs[4]);   // uv1
            heap.FreeBuffer(src.bufs[8]);   // uv2
            heap.FreeBuffer(src.bufs[9]);   // uv3
            heap.FreeBuffer(src.bufs[5]);   // tangent
            heap.FreeBuffer(src.bufs[6]);   // index
        }
        for (RHI::GPUBuffer& b : src.bufs)
        {
            if (!b.IsValid()) continue;
            ++bufs;
            // The policy of OnWorldClear's owned-buffer sweep: a live Play snapshot still holds
            // value-copies of these handles, so park them for the Stop-time reconcile; otherwise
            // queue them for the next clear's drain (one-world lag), never an immediate destroy.
            if (m_playSnapshotActive) m_orphanedGpuBuffers.push_back(b);
            else                      m_pendingWorldRelease.push_back(b);
        }
    }
    LOG_INFO("SkinnedMeshSubsystem: released %zu shared source entries (%zu buffers)",
             m_sharedSources.size(), bufs);
    m_sharedSources.clear();
}

void SkinnedMeshSubsystem::AttachSkeletonForPending(
    World& world, const SceneLoader::SkinnedMeshPending& pending)
{
    if (pending.skeletonIndex == kInvalidSkeletonIndex) return;

    const Entity rootEnt = (pending.rootEntity != NullEntity) ? pending.rootEntity
                                                              : pending.entity;
    const SkeletonAsset& skel = m_skeletonRegistry.Get(pending.skeletonIndex);

    if (!world.HasComponent<SkeletonComponent>(rootEnt))
    {
        SkeletonComponent sc{};
        sc.assetIndex = pending.skeletonIndex;
        sc.boneCount  = skel.boneCount;
        world.AddComponent<SkeletonComponent>(rootEnt, sc);
        world.AddComponent<AnimationComponent>(rootEnt, AnimationComponent{});

        // Auto-attach a MorphComponent when the skeleton ships morph targets,
        // so the editor inspector can author weights manually without first
        // needing to drop a .ianim morph clip onto the character.
        if (skel.morphTargetCount > 0 && !world.HasComponent<MorphComponent>(rootEnt))
        {
            MorphComponent mc{};
            mc.primaryMorphClip = kInvalidAnimHandle; // manual mode
            mc.count = (std::min)(skel.morphTargetCount,
                                   static_cast<uint32_t>(MorphComponent::MAX));
            world.AddComponent<MorphComponent>(rootEnt, mc);
            LOG_INFO("SkinnedMeshSubsystem: MorphComponent auto-attached to root %u (%u targets)",
                     rootEnt, mc.count);
        }

        bool hasPhysBones = false;
        // Real bones only: a virtual bone never makes a chain.
        for (uint32_t b = 0; b < skel.RealBoneCount() && !hasPhysBones; ++b)
            hasPhysBones = ChainPhysicsNames::TriggersAutoAttach(skel.boneNames[b]);
        if (hasPhysBones)
        {
            world.AddComponent<ChainPhysicsComponent>(rootEnt, ChainPhysicsComponent{});
            LOG_INFO("SkinnedMeshSubsystem: ChainPhysicsComponent → root entity %u (auto-detected)", rootEnt);
        }

        LOG_INFO("SkinnedMeshSubsystem: SkeletonComponent + AnimationComponent → root entity %u", rootEnt);
    }

    if (pending.entity != rootEnt)
    {
        SkeletonRef ref{};
        ref.entity = rootEnt;
        world.AddComponent<SkeletonRef>(pending.entity, ref);
    }
}

void SkinnedMeshSubsystem::ReleasePreviousSkinnedRegistration(
    World& world, MeshManager& meshMgr, Entity e)
{
    const MeshSkinnedComponent* prev = world.GetComponent<MeshSkinnedComponent>(e);
    if (!prev) return;

    auto& heap = meshMgr.GetDescriptorHeap();

    // The mesh descriptor slot is ALWAYS per-instance, shared sources or not.
    if (auto it = m_skinnedMeshDescCache.find(e); it != m_skinnedMeshDescCache.end())
        heap.FreeMesh(it->second);
    else if (prev->meshDescriptorIdx != RHI::kInvalidBufferIndex)
        heap.FreeMesh(prev->meshDescriptorIdx);

    // A borrower's bindless slots and buffers belong to the shared cache: freeing them
    // here would pull them from under the other instances still rendering from them.
    if (prev->sharedSourceKey == 0)
    {
        heap.FreeBuffer(prev->uvBuffer);
        heap.FreeBuffer(prev->uv1Buffer);
        heap.FreeBuffer(prev->uv2Buffer);
        heap.FreeBuffer(prev->uv3Buffer);
        heap.FreeBuffer(prev->tangentBuffer);
        heap.FreeBuffer(prev->indexBuffer);
    }
    m_skinnedMeshDescCache.erase(e);
    ReleaseOwnedBuffers(e);   // no-op for a borrower (nothing was recorded)
}

void SkinnedMeshSubsystem::RecordOwnedBuffers(Entity e, const MeshSkinnedComponent& c)
{
    // A borrower owns nothing: leaving it out of m_ownedGpuBuffers is what
    // keeps every existing sweep (world clear, play-snapshot reconcile, undo
    // drop) from freeing cache-owned buffers behind the other instances' backs.
    if (c.sharedSourceKey != 0)
    {
        m_ownedGpuBuffers.erase(e);
        return;
    }

    OwnedGpuBuffers& o = m_ownedGpuBuffers[e];
    o.bufs[0] = c.blendBuffer;
    o.bufs[1] = c.restPosBuffer;
    o.bufs[2] = c.restNrmBuffer;
    o.bufs[3] = c.uvBuffer;
    o.bufs[4] = c.uv1Buffer;
    o.bufs[8] = c.uv2Buffer;
    o.bufs[9] = c.uv3Buffer;
    o.bufs[5] = c.tangentBuffer;
    o.bufs[6] = c.indexBuffer;
    o.bufs[7] = c.morphDeltaBuffer;
}

void SkinnedMeshSubsystem::ReleaseOwnedBuffers(Entity e)
{
    auto it = m_ownedGpuBuffers.find(e);
    // A marked entity's buffers were provably never captured by a Play snapshot or an
    // undo record (MarkBuffersUnsnapshotted), so parking them would only pile them onto
    // the orphan list. Still refused while a snapshot is live, the one state the park
    // exists for. The mark is consumed either way.
    const bool unsnapshotted = m_unsnapshotted.erase(e) != 0 && !m_playSnapshotActive;
    if (it == m_ownedGpuBuffers.end()) return;
    for (RHI::GPUBuffer& b : it->second.bufs)
    {
        if (!b.IsValid()) continue;
        if (unsnapshotted && m_gfx)
        {
            // Deferred inside by kFrameCount frames, so a frame still in
            // flight that samples this character is safe. Reached AFTER
            // OnEntityDestroyed freed the bindless slots, which look the
            // resource up by pointer and must see it alive.
            m_gfx->DestroyBuffer(b);
            ++m_unsnapshottedReleased;
        }
        else if (Sys::IsEditorHost())
        {
            // An editor Play snapshot may still hold value-copies of these
            // handles (Stop restores them via RebindRestoredWorldDescriptors).
            // Park on the orphan list; the restore reconcile / scene-change
            // sweep decides their fate by actual reference.
            m_orphanedGpuBuffers.push_back(b);
        }
        else if (m_gfx)
        {
            // Game host: no snapshots exist, so free now (deferred inside).
            m_gfx->DestroyBuffer(b);
        }
    }
    m_ownedGpuBuffers.erase(it);
}

void SkinnedMeshSubsystem::MarkBuffersUnsnapshotted(Entity e)
{
    // Only an entity that actually OWNS buffers needs the mark; a borrower's
    // buffers belong to the shared-source cache and are never released per
    // entity (RecordOwnedBuffers).
    if (m_undoParked.find(e) != m_undoParked.end()) return;
    if (m_ownedGpuBuffers.find(e) != m_ownedGpuBuffers.end())
        m_unsnapshotted.insert(e);
}

void SkinnedMeshSubsystem::OnEntityDestroyed(Entity e, const MeshSkinnedComponent* mesh)
{
    // Recycle the world-scoped GPU slots this skinned mesh owned before its component and
    // buffers are torn down; otherwise a per-entity delete leaks the MeshDescriptorHeap
    // mesh slot + its six bindless buffer slots (uv0..uv3 / tangent / index) until the
    // next world reload (~80 delete + reload cycles of a skinned character exhaust
    // kMaxBuffers). MeshDescriptorHeap defers reuse by kFrameCount frames, so freeing from
    // the mid-frame destroy listener is safe. Slots parked for a possible undo are not
    // freed and keep their desc-cache entry: a value-copy resurrection reuses them, and
    // FreeUndoParked releases them if the undo action is dropped.
    const bool parked = m_undoParked.find(e) != m_undoParked.end();

    if (m_meshMgr && !parked)
    {
        auto& heap = m_meshMgr->GetDescriptorHeap();

        // The desc cache is a fast path, not the ownership record, so it needs the component
        // fallback ReleasePreviousSkinnedRegistration has. BuildSkinJobs fills it only for
        // parts skinned this frame (it bails before the write on no SkinningOutputComponent,
        // zero vertices, missing blend / rest SRVs, poseByteOffset == ~0u, or past
        // kMaxSkinJobs); such an entity still holds a real RegisterMesh slot in
        // meshDescriptorIdx, and keying the release on the cache alone leaks it silently.
        // FreeMesh ignores persistent / out-of-range / ~0u slots.
        if (auto it = m_skinnedMeshDescCache.find(e); it != m_skinnedMeshDescCache.end())
            heap.FreeMesh(it->second);
        else if (mesh && mesh->meshDescriptorIdx != RHI::kInvalidBufferIndex)
            heap.FreeMesh(mesh->meshDescriptorIdx);

        if (const MeshSkinnedComponent* m = mesh)
            {
                // A borrower's bindless slots are cache-owned and used by every other instance of
                // this character: recycling them here would repoint live descriptors at whatever
                // claims the slot next. ReleaseSharedSources frees them at world teardown.
                if (m->sharedSourceKey == 0)
                {
                    heap.FreeBuffer(m->uvBuffer);
                    heap.FreeBuffer(m->uv1Buffer);
                    heap.FreeBuffer(m->uv2Buffer);
                    heap.FreeBuffer(m->uv3Buffer);
                    heap.FreeBuffer(m->tangentBuffer);
                    heap.FreeBuffer(m->indexBuffer);
                }
            }
    }

    if (!parked)
    {
        m_skinnedMeshDescCache.erase(e);   // parked: keep the slot association
        // Release the GPU buffers themselves: the descriptor frees above recycle bindless
        // slot indices only. Parked entities keep their owned record so a value-copy
        // resurrection reuses the same live handles.
        ReleaseOwnedBuffers(e);
    }

    m_animVisibleSet.erase(e);
}

void SkinnedMeshSubsystem::ParkForUndo(World& world, Entity e)
{
    const MeshSkinnedComponent* comp = world.GetComponent<MeshSkinnedComponent>(e);
    if (!comp) return;

    UndoParkedSlots p;
    if (auto it = m_skinnedMeshDescCache.find(e); it != m_skinnedMeshDescCache.end())
        p.meshDescSlot = it->second;
    else
        p.meshDescSlot = comp->meshDescriptorIdx;
    p.uvBuffer       = comp->uvBuffer;
    p.uv1Buffer      = comp->uv1Buffer;
    p.uv2Buffer      = comp->uv2Buffer;
    p.uv3Buffer      = comp->uv3Buffer;
    p.tangentBuffer  = comp->tangentBuffer;
    p.indexBuffer    = comp->indexBuffer;
    p.sharedSourceKey = comp->sharedSourceKey;
    m_undoParked[e] = std::move(p);
}

void SkinnedMeshSubsystem::UnparkForUndo(Entity e)
{
    // The restored live component owns the slots again: drop the parked record without
    // freeing.
    m_undoParked.erase(e);

}

void SkinnedMeshSubsystem::FreeUndoParked(MeshManager& meshMgr, Entity e)
{
    auto it = m_undoParked.find(e);
    if (it == m_undoParked.end()) return;

    auto& heap = meshMgr.GetDescriptorHeap();
    if (it->second.meshDescSlot != 0xFFFFFFFFu)
        heap.FreeMesh(it->second.meshDescSlot);   // always per-instance
    if (it->second.sharedSourceKey == 0)
    {
        heap.FreeBuffer(it->second.uvBuffer);
        heap.FreeBuffer(it->second.uv1Buffer);
        heap.FreeBuffer(it->second.uv2Buffer);
        heap.FreeBuffer(it->second.uv3Buffer);
        heap.FreeBuffer(it->second.tangentBuffer);
        heap.FreeBuffer(it->second.indexBuffer);
    }

    m_undoParked.erase(it);
    m_skinnedMeshDescCache.erase(e);

    // The undo record is gone for good, so nothing can resurrect this entity: release its
    // parked GPU buffers through the ownership policy (orphan-listed in the editor until
    // the next reconcile, destroyed at once in the Game host).
    ReleaseOwnedBuffers(e);
}

void SkinnedMeshSubsystem::BuildSkinJobs(World& world, MeshManager& meshMgr)
{
    ++m_skinBuildIndex;
    SkinJobsWrite().clear();

    // Hand the render half the slot last frame's positions landed in, then claim this
    // frame's; both are the game half's ring slot, the one SkinMatrixSystem's
    // AllocPosition hands out offsets into. Recorded, never derived as
    // (slot + kFramesInFlight - 1) % kFramesInFlight: under -slotkey=dxgi the ring slot is
    // the DXGI back-buffer index, which that arithmetic does not track. md.prevPosition
    // points at this slot.
    const uint32_t prevPosSlot = m_lastSkinPosSlot;
    m_skinPayload[m_skinWrite].prevPosSlot = prevPosSlot;
    m_lastSkinPosSlot = m_vertRing.GetFrameSlot();
    m_skinPayload[m_skinWrite].curPosSlot = m_lastSkinPosSlot;

    auto* pSkinOut     = world.GetPool<SkinningOutputComponent>();
    auto* pSkelRef     = world.GetPool<SkeletonRef>();
    auto* pSkelComp    = world.GetPool<SkeletonComponent>();
    auto* pMorphComp   = world.GetPool<MorphComponent>();
    auto* pAnimComp    = world.GetPool<AnimationComponent>();
    // Fetched once with the other pools so the per-section facial lookup is one
    // sparse-set index, not a world.GetComponent hash lookup. Null until the first
    // FacialAnimationComponent is added anywhere (the common case).
    auto* pFacial      = world.GetPool<FacialAnimationComponent>();
    auto  pGet = [](auto* p, Entity e) { return p ? p->Get(e) : nullptr; };

    // Morph-target slots the facial layer wrote this frame, published to AnimProfile
    // below: the only signal downstream of the component a facial regression can be
    // asserted against (a frozen face changes no drawPackets, engineErrors or luma).
    std::uint32_t facialSlotsWritten = 0;

    auto* pSkinnedMesh = world.GetPool<MeshSkinnedComponent>();
    if (!pSkinnedMesh) return;
    auto& skinEnts = pSkinnedMesh->Entities();
    auto& skinData = pSkinnedMesh->Data();
    const size_t skinN = skinData.size();
    // Reserve once: clear() keeps capacity, so this is a no-op after the first frame, and
    // the vector never regrows step by step (each regrow of a large block faults in fresh
    // pages).
    SkinJobsWrite().reserve(skinN);
    MorphPoolWrite().clear();
    Anim::AnimProfile::Get().skinSections = static_cast<std::uint32_t>(skinN);
    {
        // One-shot census: does this content actually carry morph targets?
        static bool s_census = false;
        if (!s_census && skinN > 0)
        {
            s_census = true;
            uint32_t withMorphs = 0, maxTargets = 0, totalTargets = 0;
            for (size_t i = 0; i < skinN; ++i)
            {
                const uint32_t mt = skinData[i].morphTargetCount;
                if (mt > 0) { ++withMorphs; totalTargets += mt; }
                maxTargets = (std::max)(maxTargets, mt);
            }
            LOG_INFO("MORPH CENSUS: %u of %zu skinned sections carry morph targets "
                     "(max %u per section, %u total). SkinDispatchDesc is %zu B; "
                     "weights are pooled, so a section with no morphs costs none.",
                     withMorphs, skinN, maxTargets, totalTargets,
                     sizeof(SkinDispatchDesc));
        }
    }

    // Per-skeleton character extent = max part rest extent. Skin.cs clamps any vertex
    // that flies beyond ~4x the character's own size back to its rest pose (guards the
    // identical-mesh + recycled-entity-id reload TDR); a per-part extent would falsely
    // clamp limb tips that legitimately swing far.
    // -morphnormalsoff: skin with the rest normal under every morph, ignoring the .imorph
    // normal deltas (an A/B arm).
    static const bool s_morphNormalsOff = Sys::CommandLine::Get().Has("-morphnormalsoff");
    // -skinextentdense: dense per-skeleton extent aggregation. Opt-in until repeated
    // captures show an animation / frame-time gain.
    static const bool s_extentDense = Sys::CommandLine::Get().Has("-skinextentdense");
    const bool denseExtents = s_extentDense;
    bool verifyExtents = false;
#if defined(WITH_HARNESS) && WITH_HARNESS
    static const bool s_extentVerify = Sys::CommandLine::Get().Has("-skinextentverify");
    verifyExtents = s_extentVerify;
#endif
    // These scopes span the remaining build, including extent consumption.
    ZoneNamedN(denseExtentZone, "Anim.SkinExtent.Dense", denseExtents);
    ZoneNamedN(legacyExtentZone, "Anim.SkinExtent.Legacy", !denseExtents);
    std::unordered_map<Entity, float> charExtentBySkel; // rollback / independent oracle
    if (!denseExtents || verifyExtents)
    {
        for (size_t si = 0; si < skinN; ++si)
        {
            Entity root = skinEnts[si];
            if (const SkeletonRef* r = pGet(pSkelRef, root); r && r->entity != NullEntity)
                root = r->entity;
            float& ce = charExtentBySkel[root];
            ce = (std::max)(ce, skinData[si].restExtent);
        }
    }
    if (denseExtents)
    {
        ZoneScopedN("Anim.SkinExtent.Aggregate");
        const size_t skeletonCount = pSkelComp ? pSkelComp->Data().size() : 0;
        m_skinRootExtents.assign(skeletonCount, 0.f);
        m_skinRoots.resize(skinN);
        // A malformed / partially constructed rig may refer to an entity with
        // no SkeletonComponent. Preserve grouping by that entity too. Normal
        // rigs never allocate hash nodes; vectors retain their high-water capacity.
        std::unordered_map<Entity, size_t> missingRoots;
        for (size_t si = 0; si < skinN; ++si)
        {
            Entity root = skinEnts[si];
            if (const SkeletonRef* r = pGet(pSkelRef, root); r && r->entity != NullEntity)
                root = r->entity;
            size_t slot;
            if (const auto* skeleton = pGet(pSkelComp, root))
                slot = static_cast<size_t>(skeleton - pSkelComp->Data().data());
            else
            {
                const auto [it, inserted] = missingRoots.try_emplace(root, m_skinRootExtents.size());
                slot = it->second;
                if (inserted) m_skinRootExtents.push_back(0.f);
            }
            m_skinRoots[si] = {root, slot};
            float& ce = m_skinRootExtents[slot];
            ce = (std::max)(ce, skinData[si].restExtent);
        }
        if (verifyExtents)
        {
            bool match = true;
            for (const auto& root : m_skinRoots)
                match &= m_skinRootExtents[root.extentIndex] == charExtentBySkel.at(root.entity);
            ENGINE_CHECK(match, "Skin extent dense aggregation differs from legacy");
            if (m_skinBuildIndex % 120 == 0)
                LOG_INFO("[SKIN_EXTENT_ORACLE] match=%u sections=%zu skeletons=%zu missing=%zu",
                    match ? 1u : 0u, skinN, skeletonCount, missingRoots.size());
        }
    }

    // ---- Three passes ----
    // Per section the work is independent; what is shared is the job vector and the morph
    // pool (appends), the descriptor and morph-remap caches (inserts), and the
    // MeshDescriptorHeap's dirty lists (UpdateMeshThisFrame and SetMeshAABB push onto
    // them). So:
    //   1. serial   - which sections get a job, in which slot, with which morph pool
    //                 slice, descriptor slot and morph remap;
    //   2. parallel - everything per section: the job, prev-frame bookkeeping, morph +
    //                 facial weights into the section's own pool slice, the patched
    //                 descriptor and the animated bounds (computed, not written);
    //   3. serial   - the descriptor-heap writes, in job order.
    // The result equals one serial loop (same jobs, pool bytes and heap writes, same
    // order), which costs ~0.6 ms of main thread at 3,000 skinned sections.
    // -skinjobsserial runs pass 2 on this thread (an A/B arm).
    struct SkinSel
    {
        uint32_t si;
        uint32_t morphCount, morphOffset;
        uint32_t descSlot;                       // ~0u = no descriptor patch
        const std::vector<int16_t>* remap;       // clip-mode morph remap, or null
    };
    static std::vector<SkinSel>                         s_sel;       // main thread only
    static std::vector<RHI::MeshDescriptor>             s_desc;
    static std::vector<MeshDescriptorHeap::MeshAABB>    s_aabb;
    static std::vector<uint8_t>                         s_aabbValid;
    s_sel.clear();
    uint32_t morphTotal = 0;
    for (size_t si = 0; si < skinN; ++si)
    {
        const Entity e = skinEnts[si];
        auto* mesh    = &skinData[si];
        auto* skinOut = pGet(pSkinOut, e);
        // A mesh skipped here is neither dispatched nor drawn skinned (the draw gate requires
        // the same skinOut + poseByteOffset != ~0u), so it falls back to the rest-pose meshlib
        // slot instead of drawing a stale ring offset.
        if (!skinOut) continue;
        if (mesh->vertexCount == 0) continue;
        if (!mesh->blendSRVHandle || !mesh->restPosSRVHandle || !mesh->restNrmSRVHandle) continue;
        if (skinOut->poseByteOffset == ~0u) continue;

        // Enforce the SkinningPass job cap at the source: Execute() drops jobs past
        // kMaxSkinJobs, and emitting one anyway would still patch this entity's
        // MeshDescriptor at a ring slice no dispatch writes (garbage beyond the cap).
        // Overflow parts take the rest-pose fallback like the "no active animation" path:
        // kill the draw gate and the prev-pos fetch.
        if (s_sel.size() >= SkinningPass::kMaxSkinJobs)
        {
            ENGINE_CHECK_ONCE(false,
                "BuildSkinJobs: > kMaxSkinJobs (%u) skinned parts, overflow falls back to rest pose",
                SkinningPass::kMaxSkinJobs);
            skinOut->poseByteOffset     = ~0u;
            skinOut->prevPosElementBase = 0xFFFFFFFFu;
            continue;
        }

        SkinSel sel{};
        sel.si = static_cast<uint32_t>(si);
        // Clamp to the pipeline's hard morph cap: MorphComponent::MAX == kMaxMorphTargets
        // (shaders/morph_limits.hlsli), also the size of this job's weight-pool slice. A mesh
        // may declare more targets; sending the raw count makes Skin.cs loop past the uploaded
        // weights (OOB read -> exploded verts -> TDR / DEVICE_HUNG). This is the mesh's local
        // target count (MeshSkinnedComponent::morphTargetCount): the shapes that actually move
        // this mesh.
        sel.morphCount  = (std::min)(mesh->morphTargetCount,
                                     static_cast<uint32_t>(MorphComponent::MAX));
        // This job's slice of the pool, by OFFSET (the pool is sized once,
        // below; nothing may keep a pointer into it across the resize).
        sel.morphOffset = morphTotal;
        morphTotal     += sel.morphCount;
        sel.descSlot    = ~0u;
        if (mesh->meshDescriptorIdx != ~0u)
        {
            auto it = m_skinnedMeshDescCache.find(e);
            if (it == m_skinnedMeshDescCache.end())
                it = m_skinnedMeshDescCache.emplace(e, mesh->meshDescriptorIdx).first;
            sel.descSlot = it->second;
        }
        sel.remap = nullptr;
        if (mesh->morphTargetCount > 0)
        {
            Entity skelEnt = denseExtents ? m_skinRoots[si].entity : e;
            if (!denseExtents)
            {
                const SkeletonRef* ref = pGet(pSkelRef, e);
                if (ref && ref->entity != NullEntity) skelEnt = ref->entity;
            }
            const SkeletonComponent* skelComp = pGet(pSkelComp, skelEnt);
            const MorphComponent*    morph    = pGet(pMorphComp, skelEnt);
            if (morph && morph->count > 0 && skelComp &&
                skelComp->assetIndex < m_skeletonRegistry.Count() &&
                morph->primaryMorphClip != kInvalidAnimHandle &&
                morph->primaryMorphClip < m_morphClipLibrary.Count())
            {
                // The name -> channel resolution depends only on (skeleton asset, morph clip), both
                // immutable once loaded, so it is cached; the unordered_map keeps the element's
                // address across later inserts, which lets pass 2 read it concurrently.
                sel.remap = &GetMorphRemap(skelComp->assetIndex, morph->primaryMorphClip,
                                           m_skeletonRegistry.Get(skelComp->assetIndex),
                                           m_morphClipLibrary.Get(morph->primaryMorphClip));
            }
        }
        s_sel.push_back(sel);
    }

    const uint32_t nJobs = static_cast<uint32_t>(s_sel.size());
    // resize() value-initializes the jobs and zero-fills the pool.
    SkinJobsWrite().resize(nJobs);
    MorphPoolWrite().resize(morphTotal);
    s_desc.resize(nJobs);
    s_aabb.resize(nJobs);
    s_aabbValid.assign(nJobs, 0);
    std::atomic<std::uint32_t> facialSlotsAtomic{ 0 };
    std::atomic<std::uint32_t> morphJobsAtomic{ 0 }, morphActiveAtomic{ 0 };   // AnimStats.morphJobs / morphActive

    auto fillJob = [&](uint32_t j)
    {
        const SkinSel& sel = s_sel[j];
        const size_t si = sel.si;
        const Entity e = skinEnts[si];
        auto* mesh    = &skinData[si];
        auto* skinOut = pGet(pSkinOut, e);   // non-null: pass 1 checked
        SkinDispatchDesc& job = SkinJobsWrite()[j];
        job.entity              = e;
        job.restPosSRVHandle    = mesh->restPosSRVHandle;
        job.restNrmSRVHandle    = mesh->restNrmSRVHandle;
        job.restTanSRVHandle    = mesh->restTanSRVHandle;
        job.blendSRVHandle      = mesh->blendSRVHandle;
        job.morphDeltaSRVHandle = mesh->morphDeltaSRVHandle;
        job.poseByteOffset      = skinOut->poseByteOffset;
        job.outPosByteOffset    = skinOut->outPosByteOffset;
        job.outNrmByteOffset    = skinOut->outNrmByteOffset;
        job.outTanByteOffset    = skinOut->outTanByteOffset;
        job.vertexCount         = mesh->vertexCount;
        job.morphCount          = sel.morphCount;
        job.morphWeightOffset   = sel.morphOffset;
        // The buffer's layout, so the mesh's FULL local count - never the clamp.
        job.morphNrmElemOffset  = (mesh->morphHasNormals && !s_morphNormalsOff)
                                      ? mesh->morphTargetCount * mesh->vertexCount : 0u;

        {
            Entity skelEnt;
            if (denseExtents)
            {
                const auto& root = m_skinRoots[si];
                skelEnt = root.entity;
                job.charExtent = m_skinRootExtents[root.extentIndex];
            }
            else
            {
                skelEnt = e;
                const SkeletonRef* ref = pGet(pSkelRef, e);
                if (ref && ref->entity != NullEntity) skelEnt = ref->entity;
                // find(), never operator[]: pass 2 runs on several threads and
                // must not insert (the map was fully built before pass 1).
                const auto ce = charExtentBySkel.find(skelEnt);
                job.charExtent = ce != charExtentBySkel.end() ? ce->second : 0.f;
            }

            // Last frame's skinned positions, read where they already are: the vertex ring is
            // kFramesInFlight deep and each slot is written only by its own frame, so the previous
            // slot still holds them. Only one uint per entity survives.
            const bool prevValid =
                skinOut->prevFrameOutPosByteOffset != ~0u &&
                skinOut->prevFrameVertexCount      == mesh->vertexCount &&
                skinOut->prevFrameIndex + 1        == m_skinBuildIndex &&
                prevPosSlot                        != 0xFFFFFFFFu;

            // The GPU does not read this; md.prevPosition (patched below) carries the buffer and
            // the base. The CPU keys on it (IsStaticLaneInstance, SortAndEmit's batch break), only
            // against the 0xFFFFFFFF sentinel.
            skinOut->prevPosElementBase =
                prevValid ? (skinOut->prevFrameOutPosByteOffset / 12u) : 0xFFFFFFFFu;
        }

        if (mesh->morphTargetCount > 0)
        {
            Entity skelEnt = denseExtents ? m_skinRoots[si].entity : e;
            if (!denseExtents)
            {
                const SkeletonRef* ref = pGet(pSkelRef, e);
                if (ref && ref->entity != NullEntity) skelEnt = ref->entity;
            }

            const SkeletonComponent* skelComp = pGet(pSkelComp, skelEnt);
            const MorphComponent*    morph    = pGet(pMorphComp, skelEnt);
            float* pool = MorphPoolWrite().data() + job.morphWeightOffset;   // this job's slice

            // Manual mode reads weights[] whatever `count` says: switching to a clip without
            // morphs (or unbinding) zeroes the weights and count, and weights written after that
            // must still land.
            const bool manualMode = morph && morph->primaryMorphClip == kInvalidAnimHandle;
            if (morph && (morph->count > 0 || manualMode) && skelComp &&
                skelComp->assetIndex < m_skeletonRegistry.Count())
            {
                const uint32_t nLocal =
                    (std::min)(job.morphCount,
                               static_cast<uint32_t>(mesh->morphSlotOfLocal.size()));
                if (sel.remap)
                {
                    // Clip mode: weights[] is indexed by clip channel; `remap` is
                    // SKELETON-slot indexed while the pool slice is LOCAL indexed,
                    // so the walk is over locals and the skeleton slot comes out of
                    // the mesh's own table. Bounded by job.morphCount - the length
                    // of this job's pool slice - never by MorphComponent::MAX.
                    const std::vector<int16_t>& remap = *sel.remap;
                    for (uint32_t l = 0; l < nLocal; ++l)
                    {
                        const uint32_t sl = mesh->morphSlotOfLocal[l];
                        if (sl >= remap.size()) continue;
                        const int16_t ch = remap[sl];
                        if (ch >= 0 && static_cast<uint32_t>(ch) < morph->count)
                            pool[l] = morph->weights[ch];
                    }
                }
                else
                {
                    // Manual mode (editor sliders / scripted): weights[] is indexed by skeleton morph
                    // slot, so the same local -> skeleton hop. Every slot is read: a rebind zeroes all of
                    // weights[].
                    const uint32_t wEnd = manualMode ? static_cast<uint32_t>(MorphComponent::MAX)
                                                     : (std::min)(morph->count,
                                                                  static_cast<uint32_t>(MorphComponent::MAX));
                    for (uint32_t l = 0; l < nLocal; ++l)
                    {
                        const uint32_t sl = mesh->morphSlotOfLocal[l];
                        if (sl < wEnd) pool[l] = morph->weights[sl];
                    }
                }
            }

            // ---- Facial layer (FacialAnimationComponent) ----
            // Outside the MorphComponent gate on purpose: this job's pool slice is claimed
            // unconditionally and zero-filled, so a character with no MorphComponent still has a
            // valid region to write. resolvedOut is skeleton-slot indexed; translate to this
            // mesh's local slot (-1 = the rig has the shape but this part does not). The CPU is
            // the only clamp in the pipeline (Skin.cs.hlsl has no upper bound), and a NaN must not
            // pass it ("> 0" first).
            if (const FacialAnimationComponent* fa = pGet(pFacial, skelEnt))
            {
                std::uint32_t written = 0;
                const uint32_t nOut =
                    (std::min)(fa->resolvedCount, static_cast<uint32_t>(fa->resolvedOut.size()));
                for (uint32_t r = 0; r < nOut; ++r)
                {
                    const uint32_t slot = fa->resolvedOut[r].slot;
                    if (slot >= mesh->morphLocalOfSlot.size()) continue;
                    const int16_t local = mesh->morphLocalOfSlot[slot];
                    if (local < 0) continue;
                    if (static_cast<uint32_t>(local) >= job.morphCount) continue;
                    const FacialAnimationComponent::Resolved& rv = fa->resolvedOut[r];
                    float w = rv.weight;
                    w = (w > 0.f) ? ((w < 1.f) ? w : 1.f) : 0.f;
                    // Blink alone (maxOnly) is a Max over the clip's weight, as in the fold; an Override
                    // group replaces it. A tracked slot (alpha < 1) blends over the clip, and floor is
                    // where a Blink landed on it. alpha == 1 takes w itself, never
                    // pool + (w - pool) * 1, which is not w in float.
                    if (rv.maxOnly)
                        pool[local] = (std::max)(pool[local], w);
                    else
                    {
                        const float a  = (rv.alpha > 0.f) ? ((rv.alpha < 1.f) ? rv.alpha : 1.f) : 0.f;
                        const float fl = (rv.floor > 0.f) ? ((rv.floor < 1.f) ? rv.floor : 1.f) : 0.f;
                        const float v  = (a >= 1.f) ? w : (1.f - a) * pool[local] + a * w;
                        pool[local] = (std::max)(v, fl);
                    }
                    ++written;   // counted on an actual write: AnimStats.facialSlots
                }
                if (written) facialSlotsAtomic.fetch_add(written, std::memory_order_relaxed);
            }

            // ---- The active list ----
            // The weights that move something, counted once per job: SkinningPass uploads exactly
            // these as (local, weight) pairs and Skin.cs walks only them. A job with none does no
            // morph work at all (AnimStats.morphJobs).
            std::uint32_t active = 0;
            float         pad    = 0.f;   // the farthest the active targets move a vertex
            const bool    hasMax = mesh->morphMaxDelta.size() >= job.morphCount;
            for (uint32_t l = 0; l < job.morphCount; ++l)
                if (pool[l] > kMorphActiveEpsilon)
                {
                    ++active;
                    if (hasMax) pad += pool[l] * mesh->morphMaxDelta[l];
                }
            job.morphActive = active;
            if (active)
            {
                morphJobsAtomic.fetch_add(1, std::memory_order_relaxed);
                morphActiveAtomic.fetch_add(active, std::memory_order_relaxed);
            }

            // ---- Morph displacement in the bounds ----
            // A shape key moves vertices where neither the joint hull nor a one-bone rig's rest
            // box looks. `pad` bounds how far (a length: a bone may turn the delta onto any axis).
            // The part's WorldAabb - what the CPU cull tests, and what the GPU cull's mesh AABB
            // below derives from - grows by it; no other skin job writes this entity's box. Its
            // owners (the bone-AABB merge, TransformSystem) rewrite it only when their inputs
            // change, so a box still holding last frame's grown value is regrown from the base it
            // grew from, never compounded.
            if (WorldAabb* wa = world.GetComponent<WorldAabb>(e))
            {
                auto same = [](const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b)
                { return a.x == b.x && a.y == b.y && a.z == b.z; };
                DirectX::XMFLOAT3 lo = wa->min, hi = wa->max;
                if (mesh->morphBoundsGrown && same(wa->min, mesh->morphBoundsLastMin) &&
                    same(wa->max, mesh->morphBoundsLastMax))
                {
                    lo = mesh->morphBoundsBaseMin;
                    hi = mesh->morphBoundsBaseMax;
                }
                float scale = 1.f;   // bind-space metres -> world
                if (const GlobalTransform* gt = world.GetComponent<GlobalTransform>(e))
                {
                    const auto& m = gt->matrix;
                    scale = std::sqrt((std::max)({ m._11 * m._11 + m._12 * m._12 + m._13 * m._13,
                                                   m._21 * m._21 + m._22 * m._22 + m._23 * m._23,
                                                   m._31 * m._31 + m._32 * m._32 + m._33 * m._33 }));
                }
                const float p = pad * scale;
                if (p > 0.f && std::isfinite(p))
                {
                    mesh->morphBoundsBaseMin = lo;
                    mesh->morphBoundsBaseMax = hi;
                    wa->min = { lo.x - p, lo.y - p, lo.z - p };
                    wa->max = { hi.x + p, hi.y + p, hi.z + p };
                    mesh->morphBoundsLastMin = wa->min;
                    mesh->morphBoundsLastMax = wa->max;
                    mesh->morphBoundsGrown   = true;
                }
                else if (mesh->morphBoundsGrown)
                {
                    wa->min = lo;   // back to the box it grew (or the owner's fresh one)
                    wa->max = hi;
                    mesh->morphBoundsGrown = false;
                }
            }
        }

        // Remember where this entity's positions landed, for next frame's
        // prev-position read. AFTER the prevValid test above, which consumes
        // the previous value.
        skinOut->prevFrameOutPosByteOffset = skinOut->outPosByteOffset;
        skinOut->prevFrameVertexCount      = mesh->vertexCount;
        skinOut->prevFrameIndex            = m_skinBuildIndex;

        if (sel.descSlot != ~0u)
        {
            RHI::MeshDescriptor& patchedDesc = s_desc[j];
            patchedDesc = mesh->baseMeshDesc;
            patchedDesc.position.bufferIndex = m_vertRing.UploadPosBindlessIdx();
            patchedDesc.position.byteOffset  = skinOut->outPosByteOffset;
            patchedDesc.position.byteStride  = sizeof(float) * 3;
            patchedDesc.position.format      = static_cast<uint32_t>(RHI::VertexFormat::Float3);
            patchedDesc.normal.bufferIndex   = m_vertRing.UploadNrmBindlessIdx();
            patchedDesc.normal.byteOffset    = skinOut->outNrmByteOffset;
            patchedDesc.normal.byteStride    = sizeof(float) * 3;
            patchedDesc.normal.format        = static_cast<uint32_t>(RHI::VertexFormat::Float3);
            // Tangent, when the mesh has one and the ring gave it a slice.
            // Without it the vertex factory keeps fetching the BIND-POSE
            // tangent while the normal is posed (the tangent frame stays rotated
            // by the bone; normal maps and anisotropic highlights shear).
            if (mesh->restTanSRVHandle != 0 && skinOut->outTanByteOffset != ~0u)
            {
                patchedDesc.tangent.bufferIndex = m_vertRing.UploadTanBindlessIdx();
                patchedDesc.tangent.byteOffset  = skinOut->outTanByteOffset;
                patchedDesc.tangent.byteStride  = sizeof(float) * 4;
                patchedDesc.tangent.format      = static_cast<uint32_t>(RHI::VertexFormat::Float4);
            }
            // THE PREVIOUS FRAME'S SLOT: the only stream naming a buffer written
            // by a different frame, and the only "has a previous position"
            // signal the vertex shaders test.
            if (skinOut->prevPosElementBase != 0xFFFFFFFFu)
            {
                patchedDesc.prevPosition.bufferIndex =
                    m_vertRing.posBindlessIdx[prevPosSlot];
                patchedDesc.prevPosition.byteOffset  = skinOut->prevPosElementBase * 12u;
                patchedDesc.prevPosition.byteStride  = sizeof(float) * 3;
                patchedDesc.prevPosition.format      =
                    static_cast<uint32_t>(RHI::VertexFormat::Float3);
            }
            patchedDesc.vertexCount          = mesh->vertexCount;

            // THE ANIMATED BOUNDS, in whatever space inst.world maps from:
            // inverse(world) x worldAabb, exact about that rather than assuming
            // this section's transform matches the skeleton root's. Never a
            // ~zero-extent box (a collapsed WorldAabb would delete the character
            // from every view): skip the publish, the slot keeps its old box.
            if (const WorldAabb* wa = world.GetComponent<WorldAabb>(e))
            {
                const GlobalTransform* gt = world.GetComponent<GlobalTransform>(e);
                const float waExt = (std::max)({ wa->max.x - wa->min.x,
                                                 wa->max.y - wa->min.y,
                                                 wa->max.z - wa->min.z });
                if (gt && wa->min.x <= wa->max.x && waExt > 1e-4f)
                {
                    const DirectX::XMMATRIX inv = DirectX::XMMatrixInverse(nullptr, DirectX::XMLoadFloat4x4(&gt->matrix));
                    DirectX::XMVECTOR lo = DirectX::XMVectorReplicate( FLT_MAX);
                    DirectX::XMVECTOR hi = DirectX::XMVectorReplicate(-FLT_MAX);
                    const float xs[2] = { wa->min.x, wa->max.x };
                    const float ys[2] = { wa->min.y, wa->max.y };
                    const float zs[2] = { wa->min.z, wa->max.z };
                    for (int ix = 0; ix < 2; ++ix)
                    for (int iy = 0; iy < 2; ++iy)
                    for (int iz = 0; iz < 2; ++iz)
                    {
                        DirectX::XMVECTOR c = DirectX::XMVector3TransformCoord(
                            DirectX::XMVectorSet(xs[ix], ys[iy], zs[iz], 1.f), inv);
                        lo = DirectX::XMVectorMin(lo, c);
                        hi = DirectX::XMVectorMax(hi, c);
                    }
                    DirectX::XMFLOAT3 l, h;
                    DirectX::XMStoreFloat3(&l, lo);
                    DirectX::XMStoreFloat3(&h, hi);
                    if (std::isfinite(l.x) && std::isfinite(h.x))
                    {
                        s_aabb[j]      = { l.x, l.y, l.z, h.x, h.y, h.z };
                        s_aabbValid[j] = 1;
                    }
                }
            }
        }
    };

    static const bool s_serialFill = Sys::CommandLine::Get().Has("-skinjobsserial");
    if (!s_serialFill && nJobs >= 64)
    {
        PROF_SCOPE("Anim.BuildSkinJobs.Fill");
        TaskSystem::Get().ParallelFor(0, nJobs, fillJob);
    }
    else
    {
        for (uint32_t j = 0; j < nJobs; ++j) fillJob(j);
    }

    // Pass 3: the descriptor-heap writes, in job order (they push onto the heap's
    // single-threaded dirty lists). position.bufferIndex is posBindlessIdx[current slot],
    // valid only this frame; UpdateMeshThisFrame writes only the active GPU slot, so the
    // next frame's CPU write cannot race late passes (e.g. OutlinePass) still reading the
    // shared MeshDescriptor buffer.
    {
        auto& heap = meshMgr.GetDescriptorHeap();
        for (uint32_t j = 0; j < nJobs; ++j)
        {
            const uint32_t slot = s_sel[j].descSlot;
            if (slot == ~0u) continue;
            heap.UpdateMeshThisFrame(slot, s_desc[j]);
            if (s_aabbValid[j]) heap.SetMeshAABB(slot, s_aabb[j]);
        }
    }
    facialSlotsWritten = facialSlotsAtomic.load(std::memory_order_relaxed);

    // Morph.GetPoolWeights' copy, once a script has asked for one (ReadPoolWeights).
    if (m_poolSnapshotArmed.load(std::memory_order_relaxed))
    {
        std::lock_guard<std::mutex> lock(m_poolSnapshotMutex);
        for (auto& [ent, rows] : m_poolSnapshot) rows.clear();
        const std::vector<float>& pool = MorphPoolWrite();
        for (uint32_t j = 0; j < nJobs; ++j)
        {
            const SkinSel& sel = s_sel[j];
            if (sel.morphCount == 0) continue;
            const Entity e = skinEnts[sel.si];
            const MeshSkinnedComponent& mesh = skinData[sel.si];
            Entity skelEnt = e;
            if (denseExtents) skelEnt = m_skinRoots[sel.si].entity;
            else if (const SkeletonRef* ref = pGet(pSkelRef, e); ref && ref->entity != NullEntity) skelEnt = ref->entity;
            std::vector<std::pair<uint16_t, float>>& rows = m_poolSnapshot[skelEnt];
            const uint32_t n = (std::min)(sel.morphCount, static_cast<uint32_t>(mesh.morphSlotOfLocal.size()));
            for (uint32_t l = 0; l < n; ++l)
            {
                const uint32_t slot = mesh.morphSlotOfLocal[l];
                if (slot > 0xFFFFu || sel.morphOffset + l >= pool.size()) continue;
                const float w = pool[sel.morphOffset + l];
                auto it = std::find_if(rows.begin(), rows.end(), [&](const auto& r) { return r.first == slot; });
                if (it == rows.end()) rows.emplace_back(static_cast<uint16_t>(slot), w);
                else if (w > it->second) it->second = w;
            }
        }
        for (auto it = m_poolSnapshot.begin(); it != m_poolSnapshot.end();)
            it = it->second.empty() ? m_poolSnapshot.erase(it) : std::next(it);
    }

    // How many morph slots the facial layer actually landed in the pool. Published
    // HERE rather than from FacialAnimationSystem on purpose: the system's own
    // resolvedCount only says what it computed, this says what reached a skin job.
    Anim::AnimProfile::Get().facialSlots = facialSlotsWritten;
    // The skin jobs with any active morph weight, and those weights.
    Anim::AnimProfile::Get().morphJobs   = morphJobsAtomic.load(std::memory_order_relaxed);
    Anim::AnimProfile::Get().morphActive = morphActiveAtomic.load(std::memory_order_relaxed);


    if (m_skinningPass)
    {
        // Pointed at the PUBLISHED buffers by PublishSkinJobs, not here: this
        // runs in the Animation phase, a frame ahead of what the render half
        // is drawing.
    }
}

bool SkinnedMeshSubsystem::RegisterSkinnedMesh(IGraphicsDevice& gfx,
                                                World& world, Entity e,
                                                const XMFLOAT3* positions,
                                                const XMFLOAT3* normals,
                                                const BlendVertex* blendData,
                                                uint32_t vertexCount,
                                                uint32_t baseMeshDescIdx)
{
    if (!positions || !normals || !blendData || vertexCount == 0) return false;
    (void)baseMeshDescIdx;

    // Re-registration ordering: the previous buffer set is released just before
    // AddComponent below, the point of no return after every create succeeded. Do not
    // release earlier: a failure return would leave a live component whose buffers are
    // already on the release path (Game host: destroyed -> device removal on the next
    // draw).

    MeshSkinnedComponent comp{};
    comp.vertexCount       = vertexCount;
    comp.meshDescriptorIdx = baseMeshDescIdx;
    // baseMeshDesc filled in by caller (needs MeshManager), patched below.

    const uint64_t posBufSize   = static_cast<uint64_t>(vertexCount) * sizeof(XMFLOAT3);
    const uint64_t nrmBufSize   = static_cast<uint64_t>(vertexCount) * sizeof(XMFLOAT3);
    const uint64_t blendBufSize = static_cast<uint64_t>(vertexCount) * sizeof(BlendVertex);

    {
        RHI::GPUBufferDesc bd{};
        bd.size       = posBufSize;
        bd.stride     = sizeof(XMFLOAT3);
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        bd.miscFlags = RHI::ResourceMiscFlag::NONE;
        if (!gfx.CreateBuffer(bd, comp.restPosBuffer, positions))
        {
            LOG_ERROR("SkinnedMeshSubsystem::RegisterSkinnedMesh: restPosBuffer create failed (entity %u)", e);
            return false;
        }
        comp.restPosSRVHandle = gfx.GetBufferSRVGpuHandle(comp.restPosBuffer);
    }
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = nrmBufSize;
        bd.stride     = sizeof(XMFLOAT3);
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        bd.miscFlags = RHI::ResourceMiscFlag::NONE;
        if (!gfx.CreateBuffer(bd, comp.restNrmBuffer, normals))
        {
            LOG_ERROR("SkinnedMeshSubsystem::RegisterSkinnedMesh: restNrmBuffer create failed (entity %u)", e);
            // `comp` never reaches the World on this path: free the sibling created above or it
            // leaks.
            gfx.DestroyBuffer(comp.restPosBuffer);
            return false;
        }
        comp.restNrmSRVHandle = gfx.GetBufferSRVGpuHandle(comp.restNrmBuffer);
    }
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = blendBufSize;
        bd.stride     = sizeof(BlendVertex);
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_RAW;
        if (!gfx.CreateBuffer(bd, comp.blendBuffer, blendData))
        {
            LOG_ERROR("SkinnedMeshSubsystem::RegisterSkinnedMesh: blendBuffer create failed (entity %u)", e);
            // Same as above: both earlier creates succeeded, free them both.
            gfx.DestroyBuffer(comp.restPosBuffer);
            gfx.DestroyBuffer(comp.restNrmBuffer);
            return false;
        }
        comp.blendSRVHandle = gfx.GetBufferSRVGpuHandle(comp.blendBuffer);
    }

    if (vertexCount > 0)
    {
        comp.aabbMin = comp.aabbMax = positions[0];
        for (uint32_t i = 1; i < vertexCount; ++i)
        {
            comp.aabbMin.x = std::min(comp.aabbMin.x, positions[i].x);
            comp.aabbMin.y = std::min(comp.aabbMin.y, positions[i].y);
            comp.aabbMin.z = std::min(comp.aabbMin.z, positions[i].z);
            comp.aabbMax.x = std::max(comp.aabbMax.x, positions[i].x);
            comp.aabbMax.y = std::max(comp.aabbMax.y, positions[i].y);
            comp.aabbMax.z = std::max(comp.aabbMax.z, positions[i].z);
        }
    }

    {
        std::unordered_set<uint16_t> boneSet;
        for (uint32_t vi = 0; vi < vertexCount; ++vi)
        {
            for (uint32_t bi = 0; bi < MAX_BONES_PER_VERTEX; ++bi)
            {
                if (blendData[vi].boneWeights[bi] > 0)
                    boneSet.insert(blendData[vi].boneIndices[bi]);
            }
        }
        comp.influenceBones.assign(boneSet.begin(), boneSet.end());
        std::sort(comp.influenceBones.begin(), comp.influenceBones.end());
    }

    // Point of no return: every create succeeded, so the previous buffer set
    // (re-registration) is released through the ownership policy now; once the component
    // is replaced, the old handles live only in m_ownedGpuBuffers. This also recycles the
    // previous component's world-scoped heap slots and desc-cache entry, as
    // OnEntityDestroyed's non-parked path does; without it BuildSkinJobs keeps patching
    // the old mesh slot through the stale cache while the draw path reads the new,
    // unpatched one. A borrower's bindless slots and buffers belong to the cache and
    // survive (ReleasePreviousSkinnedRegistration).
    if (m_meshMgr)
        ReleasePreviousSkinnedRegistration(world, *m_meshMgr, e);
    else
        ReleaseOwnedBuffers(e);

    world.AddComponent<MeshSkinnedComponent>(e, std::move(comp));
    world.AddComponent<SkinningOutputComponent>(e, SkinningOutputComponent{});

    // Record what was just created so it can be destroyed later.
    // RegisterSkinnedMeshFull overwrites this at its end with the complete handle set;
    // recording here covers standalone callers.
    if (const MeshSkinnedComponent* stored = world.GetComponent<MeshSkinnedComponent>(e))
        RecordOwnedBuffers(e, *stored);

    LOG_INFO("SkinnedMeshSubsystem: registered skinned mesh for entity %u (%u verts)", e, vertexCount);
    return true;
}

bool SkinnedMeshSubsystem::RegisterSkinnedMeshFull(IGraphicsDevice& gfx,
                                                    MeshManager&     meshMgr,
                                                    World&           world,
                                                    const SceneLoader::SkinnedMeshPending& pending)
{
    // A cold part: ~8 buffers created with initial data, each with its own committed
    // upload staging; a part whose source another instance uploaded takes the shared path.
    PROF_SCOPE("Skin.RegisterFull");
    const uint32_t vertexCount = static_cast<uint32_t>(pending.restPositions.size());
    const uint32_t indexCount  = static_cast<uint32_t>(pending.indices.size());

    // A keyed part whose source nobody built yet builds it now, standalone
    // (BuildSharedSource, the same code a preload uses), then attaches through the shared
    // path below like every later instance.
    if (pending.sourceKey != 0 && !pending.sharedOnly && vertexCount > 0 && indexCount > 0 &&
        m_sharedSources.find(pending.sourceKey) == m_sharedSources.end())
    {
        SharedSkinnedSource built;
        if (!BuildSharedSource(gfx, pending, nullptr, built)) return false;
        m_sharedSources.emplace(pending.sourceKey, std::move(built));
    }

    // ---- Shared-source fast path ----
    // Everything after this block - 8 CreateBuffer uploads plus four O(vertexCount)
    // passes (restExtent, blend validation, AABB, influence-bone set) - is a pure function
    // of the source asset, so a later instance of the same part reuses the first one's
    // result. Only the mesh descriptor slot is fresh, because BuildSkinJobs patches this
    // instance's own skinning output into it every frame. A sharedOnly part (the caller
    // saw the source live and decoded nothing) has no streams at all: the counts come from
    // the source.
    if (pending.sourceKey != 0 && ((vertexCount > 0 && indexCount > 0) || pending.sharedOnly))
    {
        auto shared = m_sharedSources.find(pending.sourceKey);
        if (shared == m_sharedSources.end() && pending.sharedOnly)
        {
            LOG_ERROR("SkinnedMeshSubsystem::RegisterSkinnedMeshFull: entity %u was not decoded because its "
                      "shared source 0x%016llx was live, and it is gone", pending.entity,
                      static_cast<unsigned long long>(pending.sourceKey));
            return false;
        }
        if (shared != m_sharedSources.end())
        {
            const SharedSkinnedSource& s = shared->second;

            // Re-register instead of caching slot indices: MeshDescriptorHeap rewinds its bindless
            // table on every world clear (the world-survives flavor included), so a cached index
            // would point at whatever claims the slot next. RegisterBuffer dedupes by resource
            // pointer, so this returns the slot every instance already uses: idempotent and always
            // current.
            auto& heap = meshMgr.GetDescriptorHeap();
            auto  reg  = [&heap](const RHI::GPUBuffer& b) {
                return b.IsValid() ? heap.RegisterBuffer(b) : RHI::kInvalidBufferIndex;
            };
            const uint32_t uvIdx  = reg(s.bufs[3]);
            const uint32_t uv1Idx = reg(s.bufs[4]);
            const uint32_t uv2Idx = reg(s.bufs[8]);
            const uint32_t uv3Idx = reg(s.bufs[9]);
            const uint32_t tanIdx = reg(s.bufs[5]);
            const uint32_t idxIdx = reg(s.bufs[6]);

            RHI::MeshDescriptor baseDesc{};
            if (uvIdx != RHI::kInvalidBufferIndex)
            {
                baseDesc.uv0.bufferIndex = uvIdx;
                baseDesc.uv0.byteStride  = sizeof(float) * 2;
                baseDesc.uv0.format      = static_cast<uint32_t>(RHI::VertexFormat::Float2);
            }
            if (uv1Idx != RHI::kInvalidBufferIndex)
            {
                baseDesc.uv1.bufferIndex = uv1Idx;
                baseDesc.uv1.byteStride  = sizeof(float) * 2;
                baseDesc.uv1.format      = static_cast<uint32_t>(RHI::VertexFormat::Float2);
            }
            if (uv2Idx != RHI::kInvalidBufferIndex)
            {
                baseDesc.uv2.bufferIndex = uv2Idx;
                baseDesc.uv2.byteStride  = sizeof(float) * 2;
                baseDesc.uv2.format      = static_cast<uint32_t>(RHI::VertexFormat::Float2);
            }
            if (uv3Idx != RHI::kInvalidBufferIndex)
            {
                baseDesc.uv3.bufferIndex = uv3Idx;
                baseDesc.uv3.byteStride  = sizeof(float) * 2;
                baseDesc.uv3.format      = static_cast<uint32_t>(RHI::VertexFormat::Float2);
            }
            if (tanIdx != RHI::kInvalidBufferIndex)
            {
                baseDesc.tangent.bufferIndex = tanIdx;
                baseDesc.tangent.byteStride  = sizeof(float) * 4;
                baseDesc.tangent.format      = static_cast<uint32_t>(RHI::VertexFormat::Float4);
            }
            if (idxIdx != RHI::kInvalidBufferIndex)
            {
                baseDesc.indexBufferIndex = idxIdx;
                baseDesc.indexFormat      = 1;
            }
            baseDesc.vertexCount = s.vertexCount;

            const uint32_t meshDescIdx = heap.RegisterMesh(baseDesc);
            if (meshDescIdx == RHI::kInvalidBufferIndex)
            {
                LOG_ERROR("SkinnedMeshSubsystem::RegisterSkinnedMeshFull: MeshDescriptorHeap "
                          "full on shared path (entity %u)", pending.entity);
                return false;
            }

            // Past the only failure point: recycle the previous registration.
            ReleasePreviousSkinnedRegistration(world, meshMgr, pending.entity);

            MeshSkinnedComponent comp{};
            comp.sharedSourceKey     = pending.sourceKey;
            comp.blendBuffer         = s.bufs[0];
            comp.restPosBuffer       = s.bufs[1];
            comp.restNrmBuffer       = s.bufs[2];
            comp.uvBuffer            = s.bufs[3];
            comp.uv1Buffer           = s.bufs[4];
            comp.uv2Buffer           = s.bufs[8];
            comp.uv3Buffer           = s.bufs[9];
            comp.tangentBuffer       = s.bufs[5];
            comp.indexBuffer         = s.bufs[6];
            comp.morphDeltaBuffer    = s.bufs[7];
            comp.blendSRVHandle      = s.blendSRVHandle;
            comp.restPosSRVHandle    = s.restPosSRVHandle;
            comp.restNrmSRVHandle    = s.restNrmSRVHandle;
            comp.restTanSRVHandle    = s.restTanSRVHandle;
            comp.morphDeltaSRVHandle = s.morphDeltaSRVHandle;
            comp.morphTargetCount    = s.morphTargetCount;
            comp.morphHasNormals     = s.morphHasNormals;
            // Same asset, same mapping (see SharedSkinnedSource).
            comp.morphSlotOfLocal    = s.morphSlotOfLocal;
            comp.morphLocalOfSlot    = s.morphLocalOfSlot;
            comp.morphMaxDelta       = s.morphMaxDelta;
            comp.vertexCount         = s.vertexCount;
            comp.indexCount          = s.indexCount;
            comp.aabbMin             = s.aabbMin;
            comp.aabbMax             = s.aabbMax;
            comp.restExtent          = s.restExtent;
            comp.influenceBones      = s.influenceBones;
            comp.meshDescriptorIdx   = meshDescIdx;
            comp.baseMeshDesc        = heap.GetMesh(meshDescIdx);

            world.AddComponent<MeshSkinnedComponent>(pending.entity, std::move(comp));
            world.AddComponent<SkinningOutputComponent>(pending.entity, SkinningOutputComponent{});

            AttachSkeletonForPending(world, pending);

            // Borrower: records nothing, so no sweep can free the cache's
            // buffers on this entity's behalf.
            if (const MeshSkinnedComponent* msc =
                    world.GetComponent<MeshSkinnedComponent>(pending.entity))
                RecordOwnedBuffers(pending.entity, *msc);

            return true;
        }
    }

    // Max |restPos| (model-local) for the skinning divergence clamp.
    float restExtent = 0.f;
    for (const auto& rp : pending.restPositions)
        restExtent = (std::max)(restExtent, std::sqrtf(rp.x * rp.x + rp.y * rp.y + rp.z * rp.z));

    if (vertexCount == 0 || indexCount == 0)
    {
        LOG_ERROR("SkinnedMeshSubsystem::RegisterSkinnedMeshFull: empty mesh for entity %u", pending.entity);
        return false;
    }
    if (pending.blendData.empty())
    {
        LOG_ERROR("SkinnedMeshSubsystem::RegisterSkinnedMeshFull: no blend data for entity %u", pending.entity);
        return false;
    }

    {
        const uint32_t blendCount = static_cast<uint32_t>(pending.blendData.size());
        uint32_t zeroWeightVerts = 0;
        uint32_t lowSumVerts = 0;
        uint32_t highSumVerts = 0;
        uint32_t maxBoneIdx = 0;
        const uint32_t boneCount = (pending.skeletonIndex < m_skeletonRegistry.Count())
            ? m_skeletonRegistry.Get(pending.skeletonIndex).boneCount : 0;
        uint32_t oobVerts = 0;

        for (uint32_t vi = 0; vi < blendCount; ++vi)
        {
            const BlendVertex& bv = pending.blendData[vi];
            // Sum all MAX_BONES_PER_VERTEX slots, not the first four: the importer fills
            // influences in bone-iteration order without sorting, so big weights can land in slots
            // 4..7 (VRChat-style rigs), and a four-slot sum reports a false BLEND ISSUE.
            uint32_t wsum = 0;
            for (uint32_t k = 0; k < MAX_BONES_PER_VERTEX; ++k) wsum += bv.boneWeights[k];
            if (wsum == 0) ++zeroWeightVerts;
            if (wsum < 240) ++lowSumVerts;
            if (wsum > 260) ++highSumVerts;
            for (uint32_t k = 0; k < MAX_BONES_PER_VERTEX; ++k)
            {
                if (bv.boneWeights[k] > 0)
                {
                    if (bv.boneIndices[k] > maxBoneIdx)
                        maxBoneIdx = bv.boneIndices[k];
                    if (boneCount > 0 && bv.boneIndices[k] >= boneCount)
                        ++oobVerts;
                }
            }
        }

        if (zeroWeightVerts > 0 || oobVerts > 0)
            LOG_ERROR("SkinnedMeshSubsystem: BLEND ISSUE entity %u — verts=%u blend=%u "
                      "zeroWeight=%u lowSum=%u highSum=%u maxBoneIdx=%u oob=%u boneCount=%u",
                      pending.entity, vertexCount, blendCount,
                      zeroWeightVerts, lowSumVerts, highSumVerts,
                      maxBoneIdx, oobVerts, boneCount);
        else
            LOG_INFO("SkinnedMeshSubsystem: blend OK entity %u — verts=%u blend=%u "
                     "maxBoneIdx=%u boneCount=%u",
                     pending.entity, vertexCount, blendCount,
                     maxBoneIdx, boneCount);
    }

    // Re-registration: the previous buffer set is not released here; RegisterSkinnedMesh
    // releases it at its own point of no return (just before AddComponent replaces the
    // component). A failure return before then leaves the entity rendering its old,
    // still-owned buffers.

    // Batch every GPU buffer upload of this skinned mesh (uv / index / tangent +
    // restPos / restNrm / blend via RegisterSkinnedMesh + morphDelta) into one
    // FlushUploadAndWait instead of 7 fence round trips. The caller loop nests another
    // scope (see SceneInstanceLoader) so a whole character-heavy scene's skinned uploads
    // share a single flush.
    ScopedUploadBatch _uploadBatch(gfx);

    RHI::GPUBuffer uvBuffer, uv1Buffer, uv2Buffer, uv3Buffer, indexBuffer, tangentBuffer;
    // Frees any of the six (bindless slot + buffer) on every exit until the
    // component takes ownership below.
    ScopedBufferGuard _bufGuard(gfx, meshMgr.GetDescriptorHeap(),
                                &uvBuffer, &uv1Buffer, &indexBuffer, &tangentBuffer,
                                &uv2Buffer, &uv3Buffer);
    uint32_t uvBindlessIdx      = RHI::kInvalidBufferIndex;
    uint32_t uv1BindlessIdx     = RHI::kInvalidBufferIndex;
    uint32_t uv2BindlessIdx     = RHI::kInvalidBufferIndex;
    uint32_t uv3BindlessIdx     = RHI::kInvalidBufferIndex;
    uint32_t indexBindlessIdx   = RHI::kInvalidBufferIndex;
    uint32_t tangentBindlessIdx = RHI::kInvalidBufferIndex;

    {
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(vertexCount) * sizeof(float) * 2;
        bd.stride     = sizeof(float) * 2;
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        bd.miscFlags = RHI::ResourceMiscFlag::NONE;
        const void* initData = pending.uvs.empty() ? nullptr : pending.uvs.data();
        if (!gfx.CreateBuffer(bd, uvBuffer, initData))
        {
            LOG_ERROR("SkinnedMeshSubsystem::RegisterSkinnedMeshFull: uvBuffer create failed (entity %u)", pending.entity);
            return false;
        }
        uvBindlessIdx = meshMgr.GetDescriptorHeap().RegisterBuffer(uvBuffer);
    }
    if (!pending.uvs1.empty())
    {
        if (pending.uvs1.size() != vertexCount)
        {
            LOG_ERROR("Skinned mesh UV1 count does not match vertex count");
            return false;
        }
        // Second UV set (SDF face shadow / lightmap). Static meshlib meshes get
        // this via MeshManager; skinned meshes need it wired here or FETCH_UV1
        // returns (0,0) in the GBuffer VS (breaks UV1-driven face SDF shading).
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(vertexCount) * sizeof(float) * 2;
        bd.stride     = sizeof(float) * 2;
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        bd.miscFlags = RHI::ResourceMiscFlag::NONE;
        if (!gfx.CreateBuffer(bd, uv1Buffer, pending.uvs1.data())) return false;
        uv1BindlessIdx = meshMgr.GetDescriptorHeap().RegisterBuffer(uv1Buffer);
        if (uv1BindlessIdx == RHI::kInvalidBufferIndex) return false;
    }
    if (!pending.uvs2.empty())
    {
        if (pending.uvs2.size() != vertexCount)
        {
            LOG_ERROR("Skinned mesh UV2 count does not match vertex count");
            return false;
        }
        // UV2 remains static while positions/normals are skinned each frame.
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(vertexCount) * sizeof(float) * 2;
        bd.stride     = sizeof(float) * 2;
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        bd.miscFlags = RHI::ResourceMiscFlag::NONE;
        if (!gfx.CreateBuffer(bd, uv2Buffer, pending.uvs2.data())) return false;
        uv2BindlessIdx = meshMgr.GetDescriptorHeap().RegisterBuffer(uv2Buffer);
        if (uv2BindlessIdx == RHI::kInvalidBufferIndex) return false;
    }
    if (!pending.uvs3.empty())
    {
        if (pending.uvs3.size() != vertexCount)
        {
            LOG_ERROR("Skinned mesh UV3 count does not match vertex count");
            return false;
        }
        // UV3 remains static while positions/normals are skinned each frame.
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(vertexCount) * sizeof(float) * 2;
        bd.stride     = sizeof(float) * 2;
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        bd.miscFlags = RHI::ResourceMiscFlag::NONE;
        if (!gfx.CreateBuffer(bd, uv3Buffer, pending.uvs3.data())) return false;
        uv3BindlessIdx = meshMgr.GetDescriptorHeap().RegisterBuffer(uv3Buffer);
        if (uv3BindlessIdx == RHI::kInvalidBufferIndex) return false;
    }
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(indexCount) * sizeof(uint32_t);
        bd.stride     = sizeof(uint32_t);
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_RAW;
        if (!gfx.CreateBuffer(bd, indexBuffer, pending.indices.data()))
        {
            LOG_ERROR("SkinnedMeshSubsystem::RegisterSkinnedMeshFull: indexBuffer create failed (entity %u)", pending.entity);
            return false;
        }
        indexBindlessIdx = meshMgr.GetDescriptorHeap().RegisterBuffer(indexBuffer);
    }
    // >= vertexCount, not !empty(): CreateBuffer reads vertexCount*16 bytes
    // straight out of the vector, so a short tangent stream is a heap OOB read.
    if (pending.tangents.size() >= vertexCount && vertexCount > 0)
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(vertexCount) * sizeof(float) * 4;
        bd.stride     = sizeof(float) * 4;
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        bd.miscFlags = RHI::ResourceMiscFlag::NONE;
        if (gfx.CreateBuffer(bd, tangentBuffer, pending.tangents.data()))
            tangentBindlessIdx = meshMgr.GetDescriptorHeap().RegisterBuffer(tangentBuffer);
    }

    RHI::MeshDescriptor baseDesc{};
    if (uvBindlessIdx != RHI::kInvalidBufferIndex)
    {
        baseDesc.uv0.bufferIndex = uvBindlessIdx;
        baseDesc.uv0.byteOffset  = 0;
        baseDesc.uv0.byteStride  = sizeof(float) * 2;
        baseDesc.uv0.format      = static_cast<uint32_t>(RHI::VertexFormat::Float2);
    }
    if (uv1BindlessIdx != RHI::kInvalidBufferIndex)
    {
        baseDesc.uv1.bufferIndex = uv1BindlessIdx;
        baseDesc.uv1.byteOffset  = 0;
        baseDesc.uv1.byteStride  = sizeof(float) * 2;
        baseDesc.uv1.format      = static_cast<uint32_t>(RHI::VertexFormat::Float2);
    }
    if (uv2BindlessIdx != RHI::kInvalidBufferIndex)
    {
        baseDesc.uv2.bufferIndex = uv2BindlessIdx;
        baseDesc.uv2.byteOffset  = 0;
        baseDesc.uv2.byteStride  = sizeof(float) * 2;
        baseDesc.uv2.format      = static_cast<uint32_t>(RHI::VertexFormat::Float2);
    }
    if (uv3BindlessIdx != RHI::kInvalidBufferIndex)
    {
        baseDesc.uv3.bufferIndex = uv3BindlessIdx;
        baseDesc.uv3.byteOffset  = 0;
        baseDesc.uv3.byteStride  = sizeof(float) * 2;
        baseDesc.uv3.format      = static_cast<uint32_t>(RHI::VertexFormat::Float2);
    }
    if (tangentBindlessIdx != RHI::kInvalidBufferIndex)
    {
        baseDesc.tangent.bufferIndex = tangentBindlessIdx;
        baseDesc.tangent.byteOffset  = 0;
        baseDesc.tangent.byteStride  = sizeof(float) * 4;
        baseDesc.tangent.format      = static_cast<uint32_t>(RHI::VertexFormat::Float4);
    }
    if (indexBindlessIdx != RHI::kInvalidBufferIndex)
    {
        baseDesc.indexBufferIndex = indexBindlessIdx;
        baseDesc.indexByteOffset  = 0;
        baseDesc.indexFormat      = 1;
    }
    baseDesc.vertexCount = vertexCount;

    const uint32_t baseMeshDescIdx = meshMgr.GetDescriptorHeap().RegisterMesh(baseDesc);
    if (baseMeshDescIdx == RHI::kInvalidBufferIndex)
    {
        LOG_ERROR("SkinnedMeshSubsystem::RegisterSkinnedMeshFull: MeshDescriptorHeap full (entity %u)", pending.entity);
        return false;
    }

    if (!RegisterSkinnedMesh(gfx, world, pending.entity,
                             pending.restPositions.data(),
                             pending.restNormals.data(),
                             pending.blendData.data(),
                             vertexCount,
                             baseMeshDescIdx))
    {
        // Nested registration failed before its point of no return: on a re-register the
        // entity still holds and owns its previous buffer set, so do not graft the new locals
        // onto it or re-record ownership. The guard reclaims the locals; the world-scoped mesh
        // slot RegisterMesh allocated above is recycled here (leaking one per failed attempt
        // walks toward kMaxMeshes within one world).
        meshMgr.GetDescriptorHeap().FreeMesh(baseMeshDescIdx);
        LOG_ERROR("SkinnedMeshSubsystem::RegisterSkinnedMeshFull: base registration "
                  "failed (entity %u) — previous state left intact", pending.entity);
        return false;
    }

    MeshSkinnedComponent* comp = world.GetComponent<MeshSkinnedComponent>(pending.entity);
    if (comp)
    {
        comp->uvBuffer      = std::move(uvBuffer);
        comp->uv1Buffer     = std::move(uv1Buffer);
        comp->uv2Buffer     = std::move(uv2Buffer);
        comp->uv3Buffer     = std::move(uv3Buffer);
        comp->indexBuffer   = std::move(indexBuffer);
        comp->tangentBuffer = std::move(tangentBuffer);
        // The component owns the six handles now (RecordOwnedBuffers at the end makes them
        // destroyable): stand the guard down. GPUBuffer "move" is a plain handle copy, so the
        // locals still look valid; without Dismiss the guard would free live buffers.
        _bufGuard.Dismiss();
        // Rest-pose tangent SRV for Skin.cs (t6 space2). Zero when the mesh has no authored
        // tangent stream, which opts out of the whole tangent path.
        comp->restTanSRVHandle = comp->tangentBuffer.IsValid()
                               ? gfx.GetBufferSRVGpuHandle(comp->tangentBuffer)
                               : 0;
        comp->indexCount    = indexCount;
        comp->restExtent    = restExtent;
        // Cache the base descriptor (UVs / index / tangent) so BuildSkinJobs
        // can override only position + normal streams each frame.
        comp->baseMeshDesc  = meshMgr.GetDescriptorHeap().GetMesh(baseMeshDescIdx);
    }

    AttachSkeletonForPending(world, pending);

    if (pending.morphCount > 0 && !pending.morphDeltas.empty())
    {
        auto* msc = world.GetComponent<MeshSkinnedComponent>(pending.entity);
        if (msc)
        {
            const uint64_t deltaBufSize = static_cast<uint64_t>(pending.morphDeltas.size()) * sizeof(DirectX::XMFLOAT3);
            RHI::GPUBufferDesc bd{};
            bd.size       = deltaBufSize;
            bd.stride     = sizeof(DirectX::XMFLOAT3);
            bd.usage      = RHI::Usage::DEFAULT;
            bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
            bd.miscFlags = RHI::ResourceMiscFlag::NONE;

            if (gfx.CreateBuffer(bd, msc->morphDeltaBuffer, pending.morphDeltas.data()))
            {
                msc->morphDeltaSRVHandle = gfx.GetBufferSRVGpuHandle(msc->morphDeltaBuffer);
                msc->morphTargetCount    = pending.morphCount;
                // The buffer holds the normal deltas too only when it is exactly
                // twice the positions (SkinnedMeshPending::morphDeltas).
                msc->morphHasNormals     = pending.morphHasNormals &&
                    pending.morphDeltas.size() == static_cast<size_t>(pending.morphCount) * vertexCount * 2u;

                // ---- Local <-> skeleton morph slot tables ----
                // The .imorph block is the authority on the forward direction; the loader always
                // fills it (identity for a dense file), so a missing table means a hand-built pending
                // and we fall back to the identity rather than drop morphs.
                msc->morphSlotOfLocal = pending.morphSlotOfLocal;
                if (msc->morphSlotOfLocal.size() != pending.morphCount)
                {
                    msc->morphSlotOfLocal.resize(pending.morphCount);
                    for (uint32_t l = 0; l < pending.morphCount; ++l)
                        msc->morphSlotOfLocal[l] = l;
                }

                // The reverse table is indexed by skeleton slot, so it is sized by the skeleton's
                // morph list: the space Face.*, the clip remap and the editor sliders use. A slot past
                // that list (a stale or hand-built block) is unreachable (logged).
                uint32_t skelMorphCount = 0;
                if (pending.skeletonIndex < m_skeletonRegistry.Count())
                    skelMorphCount = m_skeletonRegistry.Get(pending.skeletonIndex).morphTargetCount;
                for (uint32_t l = 0; l < pending.morphCount; ++l)
                    skelMorphCount = (std::max)(skelMorphCount, msc->morphSlotOfLocal[l] + 1u);
                skelMorphCount = (std::min)(skelMorphCount, kMaxMorphTargets);

                msc->morphLocalOfSlot.assign(skelMorphCount, int16_t(-1));
                uint32_t droppedSlots = 0;
                for (uint32_t l = 0; l < pending.morphCount; ++l)
                {
                    const uint32_t s = msc->morphSlotOfLocal[l];
                    if (s < skelMorphCount) msc->morphLocalOfSlot[s] = static_cast<int16_t>(l);
                    else                    ++droppedSlots;
                }
                if (droppedSlots > 0)
                    LOG_WARNING("SkinnedMeshSubsystem: entity %u — %u morph target(s) name a "
                                "skeleton slot past the rig's %u-name list; unreachable",
                                pending.entity, droppedSlots, skelMorphCount);

                // Each local target's farthest vertex displacement: what BuildSkinJobs grows the
                // part's bounds by, weighted.
                msc->morphMaxDelta.assign(pending.morphCount, 0.f);
                if (pending.morphDeltas.size() >= static_cast<size_t>(pending.morphCount) * vertexCount)
                    for (uint32_t l = 0; l < pending.morphCount; ++l)
                    {
                        const DirectX::XMFLOAT3* d = pending.morphDeltas.data() + static_cast<size_t>(l) * vertexCount;
                        float m2 = 0.f;
                        for (uint32_t v = 0; v < vertexCount; ++v)
                            m2 = (std::max)(m2, d[v].x * d[v].x + d[v].y * d[v].y + d[v].z * d[v].z);
                        msc->morphMaxDelta[l] = std::sqrt(m2);
                    }

                LOG_INFO("SkinnedMeshSubsystem: morph delta buffer entity %u — %u targets, %zu deltas%s",
                         pending.entity, pending.morphCount,
                         pending.morphDeltas.size(), msc->morphHasNormals ? " (+normals)" : "");
            }
            else
                LOG_WARNING("SkinnedMeshSubsystem: failed to create morph delta buffer for entity %u", pending.entity);
        }
    }

    // A keyed part never gets here: its source is built standalone and cached at the top
    // of this function, and it takes the shared path (the cache is the sole owner,
    // ReleaseSharedSources at world clear the sole destroyer). This is the unkeyed owner
    // path: a load outside any BatchScope.

    // Ownership registry: the subsystem is the only destroyer of these buffers (see
    // OwnedGpuBuffers in the header). Records every handle from the component so entity
    // destroy / world clear / undo drop free the GPU memory. A borrower
    // (sharedSourceKey != 0) records nothing (RecordOwnedBuffers).
    if (const MeshSkinnedComponent* msc =
            world.GetComponent<MeshSkinnedComponent>(pending.entity))
        RecordOwnedBuffers(pending.entity, *msc);

    LOG_INFO("SkinnedMeshSubsystem: RegisterSkinnedMeshFull entity %u — %u verts, %u idx, skeleton %u, morphs %u",
             pending.entity, vertexCount, indexCount, pending.skeletonIndex, pending.morphCount);
    return true;
}

// ==== DeriveSharedSource: a part's per-vertex results, pure CPU ====
void SkinnedMeshSubsystem::DeriveSharedSource(const SceneLoader::SkinnedMeshPending& pending, uint32_t boneCount,
                                              uint32_t skelMorphCountIn, SharedSourceDerived& d)
{
    PROF_SCOPE("Skin.DeriveSource");
    d = SharedSourceDerived{};
    const uint32_t vertexCount = static_cast<uint32_t>(pending.restPositions.size());
    const unsigned long long key = static_cast<unsigned long long>(pending.sourceKey);
    if (vertexCount == 0 || pending.blendData.size() < vertexCount) return;

    // Blend-data sanity check, the same one RegisterSkinnedMeshFull runs.
    {
        const uint32_t blendCount = static_cast<uint32_t>(pending.blendData.size());
        uint32_t zeroWeightVerts = 0, lowSumVerts = 0, highSumVerts = 0, maxBoneIdx = 0, oobVerts = 0;
        for (uint32_t vi = 0; vi < blendCount; ++vi)
        {
            const BlendVertex& bv = pending.blendData[vi];
            // All MAX_BONES_PER_VERTEX slots (importers fill influences unsorted).
            uint32_t wsum = 0;
            for (uint32_t k = 0; k < MAX_BONES_PER_VERTEX; ++k) wsum += bv.boneWeights[k];
            if (wsum == 0) ++zeroWeightVerts;
            if (wsum < 240) ++lowSumVerts;
            if (wsum > 260) ++highSumVerts;
            for (uint32_t k = 0; k < MAX_BONES_PER_VERTEX; ++k)
            {
                if (bv.boneWeights[k] == 0) continue;
                if (bv.boneIndices[k] > maxBoneIdx) maxBoneIdx = bv.boneIndices[k];
                if (boneCount > 0 && bv.boneIndices[k] >= boneCount) ++oobVerts;
            }
        }
        if (zeroWeightVerts > 0 || oobVerts > 0)
            LOG_ERROR("SkinnedMeshSubsystem: BLEND ISSUE entity %u (source 0x%016llx) — verts=%u blend=%u "
                      "zeroWeight=%u lowSum=%u highSum=%u maxBoneIdx=%u oob=%u boneCount=%u",
                      pending.entity, key, vertexCount, blendCount, zeroWeightVerts, lowSumVerts, highSumVerts,
                      maxBoneIdx, oobVerts, boneCount);
        else
            LOG_INFO("SkinnedMeshSubsystem: blend OK entity %u (source 0x%016llx) — verts=%u blend=%u "
                     "maxBoneIdx=%u boneCount=%u", pending.entity, key, vertexCount, blendCount, maxBoneIdx,
                     boneCount);
    }

    // The bounds and the divergence clamp's extent.
    d.aabbMin = d.aabbMax = pending.restPositions[0];
    for (uint32_t i = 0; i < vertexCount; ++i)
    {
        const XMFLOAT3& p = pending.restPositions[i];
        d.aabbMin.x = (std::min)(d.aabbMin.x, p.x); d.aabbMax.x = (std::max)(d.aabbMax.x, p.x);
        d.aabbMin.y = (std::min)(d.aabbMin.y, p.y); d.aabbMax.y = (std::max)(d.aabbMax.y, p.y);
        d.aabbMin.z = (std::min)(d.aabbMin.z, p.z); d.aabbMax.z = (std::max)(d.aabbMax.z, p.z);
        d.restExtent = (std::max)(d.restExtent, std::sqrtf(p.x * p.x + p.y * p.y + p.z * p.z));
    }
    {
        std::unordered_set<uint16_t> boneSet;
        for (uint32_t vi = 0; vi < vertexCount; ++vi)
            for (uint32_t bi = 0; bi < MAX_BONES_PER_VERTEX; ++bi)
                if (pending.blendData[vi].boneWeights[bi] > 0) boneSet.insert(pending.blendData[vi].boneIndices[bi]);
        d.influenceBones.assign(boneSet.begin(), boneSet.end());
        std::sort(d.influenceBones.begin(), d.influenceBones.end());
    }

    // The morph tables, built as RegisterSkinnedMeshFull builds them.
    if (pending.morphCount > 0 && !pending.morphDeltas.empty())
    {
        d.morphTargetCount = pending.morphCount;
        d.morphHasNormals  = pending.morphHasNormals &&
            pending.morphDeltas.size() == static_cast<size_t>(pending.morphCount) * vertexCount * 2u;
        d.morphSlotOfLocal = pending.morphSlotOfLocal;
        if (d.morphSlotOfLocal.size() != pending.morphCount)
        {
            d.morphSlotOfLocal.resize(pending.morphCount);
            for (uint32_t l = 0; l < pending.morphCount; ++l) d.morphSlotOfLocal[l] = l;
        }
        uint32_t skelMorphCount = skelMorphCountIn;
        for (uint32_t l = 0; l < pending.morphCount; ++l)
            skelMorphCount = (std::max)(skelMorphCount, d.morphSlotOfLocal[l] + 1u);
        skelMorphCount = (std::min)(skelMorphCount, kMaxMorphTargets);
        d.morphLocalOfSlot.assign(skelMorphCount, int16_t(-1));
        uint32_t droppedSlots = 0;
        for (uint32_t l = 0; l < pending.morphCount; ++l)
        {
            const uint32_t slot = d.morphSlotOfLocal[l];
            if (slot < skelMorphCount) d.morphLocalOfSlot[slot] = static_cast<int16_t>(l);
            else                       ++droppedSlots;
        }
        if (droppedSlots > 0)
            LOG_WARNING("SkinnedMeshSubsystem: source 0x%016llx — %u morph target(s) name a skeleton slot past "
                        "the rig's %u-name list; unreachable", key, droppedSlots, skelMorphCount);
        d.morphMaxDelta.assign(pending.morphCount, 0.f);
        if (pending.morphDeltas.size() >= static_cast<size_t>(pending.morphCount) * vertexCount)
            for (uint32_t l = 0; l < pending.morphCount; ++l)
            {
                const XMFLOAT3* dl = pending.morphDeltas.data() + static_cast<size_t>(l) * vertexCount;
                float m2 = 0.f;
                for (uint32_t v = 0; v < vertexCount; ++v)
                    m2 = (std::max)(m2, dl[v].x * dl[v].x + dl[v].y * dl[v].y + dl[v].z * dl[v].z);
                d.morphMaxDelta[l] = std::sqrt(m2);
            }
    }
    d.valid = true;
}

// ==== BuildSharedSource: one keyed part's source set, no entity ====
bool SkinnedMeshSubsystem::BuildSharedSource(IGraphicsDevice& gfx,
                                             const SceneLoader::SkinnedMeshPending& pending,
                                             const SharedSourceDerived* derivedIn,
                                             SharedSkinnedSource& out)
{
    PROF_SCOPE("Skin.BuildSource");
    const uint32_t vertexCount = static_cast<uint32_t>(pending.restPositions.size());
    const uint32_t indexCount  = static_cast<uint32_t>(pending.indices.size());
    const unsigned long long key = static_cast<unsigned long long>(pending.sourceKey);
    if (vertexCount == 0 || indexCount == 0)
    {
        LOG_ERROR("SkinnedMeshSubsystem::BuildSharedSource: empty mesh (source 0x%016llx, entity %u)",
                  key, pending.entity);
        return false;
    }
    if (pending.blendData.empty())
    {
        LOG_ERROR("SkinnedMeshSubsystem::BuildSharedSource: no blend data (source 0x%016llx, entity %u)",
                  key, pending.entity);
        return false;
    }
    if (pending.restNormals.size() < vertexCount || pending.blendData.size() < vertexCount)
    {
        LOG_ERROR("SkinnedMeshSubsystem::BuildSharedSource: %zu normals / %zu blend vertices for %u positions "
                  "(source 0x%016llx)", pending.restNormals.size(), pending.blendData.size(), vertexCount, key);
        return false;
    }

    // The per-vertex results: the caller's (a preload's worker), else here.
    SharedSourceDerived localDerived;
    const SharedSourceDerived* derived = derivedIn;
    if (!derived || !derived->valid)
    {
        const bool registered = pending.skeletonIndex < m_skeletonRegistry.Count();
        DeriveSharedSource(pending,
                           registered ? m_skeletonRegistry.Get(pending.skeletonIndex).boneCount : 0u,
                           registered ? m_skeletonRegistry.Get(pending.skeletonIndex).morphTargetCount : 0u,
                           localDerived);
        derived = &localDerived;
    }

    // Every creation in ONE upload batch (one flush for the part); nothing is
    // kept unless all the required ones succeed. The DEFAULT buffers are PLACED
    // in one heap: the spawn's loader already holds a heap batch open (this
    // nests into it); built alone, the heap is sized to this part - each
    // buffer's 64 KB-aligned size - instead of 16 MB.
    const uint64_t heapBytes = SourceStageHeapBytes(pending, *derived, 1) + SourceStageHeapBytes(pending, *derived, 2) +
                               SourceStageHeapBytes(pending, *derived, 3);
    struct HeapBatch
    {
        IGraphicsDevice& g;
        HeapBatch(IGraphicsDevice& gg, uint64_t bytes) : g(gg) { g.BeginBufferHeapBatch(bytes); }
        ~HeapBatch() { g.EndBufferHeapBatch(); }
    } heapBatch{ gfx, heapBytes };
    ScopedUploadBatch uploadBatch(gfx);
    SharedSkinnedSource s;
    struct Guard
    {
        IGraphicsDevice& g; SharedSkinnedSource& s; bool keep = false;
        ~Guard() { if (!keep) for (RHI::GPUBuffer& b : s.bufs) if (b.IsValid()) g.DestroyBuffer(b); }
    } guard{ gfx, s };
    for (int stage = 1; stage <= 3; ++stage)
        if (!BuildSourceStage(gfx, pending, *derived, stage, s)) return false;

    guard.keep = true;
    out = std::move(s);
    LOG_INFO("SkinnedMeshSubsystem: shared source 0x%016llx built — %u verts, %u idx, skeleton %u, morphs %u",
             key, vertexCount, indexCount, pending.skeletonIndex, pending.morphCount);
    return true;
}

// The 64 KB-aligned bytes one stage's DEFAULT buffers occupy in a heap.
uint64_t SkinnedMeshSubsystem::SourceStageHeapBytes(const SceneLoader::SkinnedMeshPending& pending,
                                                    const SharedSourceDerived& derived, int stage)
{
    auto aligned = [](uint64_t b) { return (b + 0xFFFFull) & ~0xFFFFull; };
    const uint64_t vertexCount = pending.restPositions.size();
    const uint64_t indexCount  = pending.indices.size();
    switch (stage)
    {
    case 1:  return 2 * aligned(vertexCount * sizeof(XMFLOAT3)) + aligned(vertexCount * sizeof(BlendVertex));
    case 2:  return aligned(vertexCount * sizeof(float) * 2) *
                    (1 + !pending.uvs1.empty() + !pending.uvs2.empty() + !pending.uvs3.empty()) +
                    aligned(indexCount * sizeof(uint32_t)) +
                    (pending.tangents.size() >= vertexCount ? aligned(vertexCount * sizeof(float) * 4) : 0u);
    default: return derived.morphTargetCount > 0 ? aligned(uint64_t(pending.morphDeltas.size()) * sizeof(XMFLOAT3)) : 0u;
    }
}

bool SkinnedMeshSubsystem::BuildSourceStage(IGraphicsDevice& gfx, const SceneLoader::SkinnedMeshPending& pending,
                                            const SharedSourceDerived& derived, int stage, SharedSkinnedSource& s)
{
    const uint32_t vertexCount = static_cast<uint32_t>(pending.restPositions.size());
    const uint32_t indexCount  = static_cast<uint32_t>(pending.indices.size());
    const unsigned long long key = static_cast<unsigned long long>(pending.sourceKey);
    auto make = [&gfx](RHI::GPUBuffer& b, uint64_t size, uint32_t stride, RHI::ResourceMiscFlag misc,
                       const void* data) -> bool
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = size;
        bd.stride     = stride;
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        bd.miscFlags = misc;
        return gfx.CreateBuffer(bd, b, data);
    };
    // bufs: [0] blend [1] restPos [2] restNrm [3] uv0 [4] uv1 [5] tangent [6] index [7] morphDelta [8] uv2 [9] uv3
    if (stage == 1)
    {
        if (!make(s.bufs[1], uint64_t(vertexCount) * sizeof(XMFLOAT3), sizeof(XMFLOAT3), RHI::ResourceMiscFlag::NONE,
                  pending.restPositions.data()) ||
            !make(s.bufs[2], uint64_t(vertexCount) * sizeof(XMFLOAT3), sizeof(XMFLOAT3), RHI::ResourceMiscFlag::NONE,
                  pending.restNormals.data()) ||
            !make(s.bufs[0], uint64_t(vertexCount) * sizeof(BlendVertex), sizeof(BlendVertex),
                  RHI::ResourceMiscFlag::BUFFER_RAW, pending.blendData.data()))
        {
            LOG_ERROR("SkinnedMeshSubsystem::BuildSharedSource: rest / blend buffer create failed (source 0x%016llx)", key);
            return false;
        }
        s.restPosSRVHandle = gfx.GetBufferSRVGpuHandle(s.bufs[1]);
        s.restNrmSRVHandle = gfx.GetBufferSRVGpuHandle(s.bufs[2]);
        s.blendSRVHandle   = gfx.GetBufferSRVGpuHandle(s.bufs[0]);
        return true;
    }
    if (stage == 2)
    {
        // UV0 always (zeros when the mesh has none, as RegisterSkinnedMeshFull makes it).
        if (!make(s.bufs[3], uint64_t(vertexCount) * sizeof(float) * 2, sizeof(float) * 2, RHI::ResourceMiscFlag::NONE,
                  pending.uvs.empty() ? nullptr : pending.uvs.data()))
        {
            LOG_ERROR("SkinnedMeshSubsystem::BuildSharedSource: uvBuffer create failed (source 0x%016llx)", key);
            return false;
        }
        const std::vector<DirectX::XMFLOAT2>* extraUv[3]  = { &pending.uvs1, &pending.uvs2, &pending.uvs3 };
        RHI::GPUBuffer*                       extraBuf[3] = { &s.bufs[4], &s.bufs[8], &s.bufs[9] };
        for (int k = 0; k < 3; ++k)
        {
            if (extraUv[k]->empty()) continue;
            if (extraUv[k]->size() != vertexCount)
            {
                LOG_ERROR("Skinned mesh UV%d count does not match vertex count (source 0x%016llx)", k + 1, key);
                return false;
            }
            if (!make(*extraBuf[k], uint64_t(vertexCount) * sizeof(float) * 2, sizeof(float) * 2,
                      RHI::ResourceMiscFlag::NONE, extraUv[k]->data()))
                return false;
        }
        if (!make(s.bufs[6], uint64_t(indexCount) * sizeof(uint32_t), sizeof(uint32_t),
                  RHI::ResourceMiscFlag::BUFFER_RAW, pending.indices.data()))
        {
            LOG_ERROR("SkinnedMeshSubsystem::BuildSharedSource: indexBuffer create failed (source 0x%016llx)", key);
            return false;
        }
        // >= vertexCount: CreateBuffer reads vertexCount*16 bytes out of the vector.
        if (pending.tangents.size() >= vertexCount &&
            make(s.bufs[5], uint64_t(vertexCount) * sizeof(float) * 4, sizeof(float) * 4, RHI::ResourceMiscFlag::NONE,
                 pending.tangents.data()))
            s.restTanSRVHandle = gfx.GetBufferSRVGpuHandle(s.bufs[5]);
        return true;
    }

    // Stage 3: the per-vertex results every instance shares, and the morph block.
    s.vertexCount    = vertexCount;
    s.indexCount     = indexCount;
    s.aabbMin        = derived.aabbMin;
    s.aabbMax        = derived.aabbMax;
    s.restExtent     = derived.restExtent;
    s.influenceBones = derived.influenceBones;
    if (derived.morphTargetCount > 0)
    {
        if (make(s.bufs[7], uint64_t(pending.morphDeltas.size()) * sizeof(XMFLOAT3), sizeof(XMFLOAT3),
                 RHI::ResourceMiscFlag::NONE, pending.morphDeltas.data()))
        {
            s.morphDeltaSRVHandle = gfx.GetBufferSRVGpuHandle(s.bufs[7]);
            s.morphTargetCount    = derived.morphTargetCount;
            s.morphHasNormals     = derived.morphHasNormals;
            s.morphSlotOfLocal    = derived.morphSlotOfLocal;
            s.morphLocalOfSlot    = derived.morphLocalOfSlot;
            s.morphMaxDelta       = derived.morphMaxDelta;
            LOG_INFO("SkinnedMeshSubsystem: morph delta buffer source 0x%016llx — %u targets, %zu deltas%s", key,
                     pending.morphCount, pending.morphDeltas.size(), s.morphHasNormals ? " (+normals)" : "");
        }
        else
            LOG_WARNING("SkinnedMeshSubsystem: failed to create morph delta buffer for source 0x%016llx", key);
    }
    return true;
}

// A source built over several calls (BeginSourceBuild / StepSourceBuild).
struct SkinnedMeshSubsystem::SourceBuild
{
    IGraphicsDevice*                       gfx      = nullptr;
    const SceneLoader::SkinnedMeshPending* pending  = nullptr;
    SharedSourceDerived                    derived;
    SharedSkinnedSource                    s;
    int                                    stage    = 1;      // the next one; 4 = over
    bool                                   finished = false;  // cached: the cache owns the buffers
    bool                                   built    = false;  // every stage made on a loader worker (BuildSourceOnLoader)
    ~SourceBuild()
    {
        if (finished || !gfx) return;
        for (RHI::GPUBuffer& b : s.bufs) if (b.IsValid()) gfx->DestroyBuffer(b);
    }
};

std::shared_ptr<SkinnedMeshSubsystem::SourceBuild>
SkinnedMeshSubsystem::BeginSourceBuild(IGraphicsDevice& gfx, const SceneLoader::SkinnedMeshPending& pending,
                                       const SharedSourceDerived* derived)
{
    const uint32_t vertexCount = static_cast<uint32_t>(pending.restPositions.size());
    if (pending.sourceKey == 0 || vertexCount == 0 || pending.indices.empty() ||
        pending.restNormals.size() < vertexCount || pending.blendData.size() < vertexCount)
        return nullptr;
    if (m_sharedSources.find(pending.sourceKey) != m_sharedSources.end()) return nullptr;
    auto b = std::make_shared<SourceBuild>();
    b->gfx     = &gfx;
    b->pending = &pending;
    if (derived && derived->valid)
        b->derived = *derived;
    else
    {
        const bool registered = pending.skeletonIndex < m_skeletonRegistry.Count();
        DeriveSharedSource(pending,
                           registered ? m_skeletonRegistry.Get(pending.skeletonIndex).boneCount : 0u,
                           registered ? m_skeletonRegistry.Get(pending.skeletonIndex).morphTargetCount : 0u,
                           b->derived);
    }
    return b;
}

bool SkinnedMeshSubsystem::StepSourceBuild(SourceBuild& b)
{
    if (b.finished || b.stage > 3 || !b.gfx || !b.pending) return false;
    PROF_SCOPE("Skin.BuildSourceStage");
    IGraphicsDevice& gfx = *b.gfx;
    // Stage 3 without a morph block creates nothing: it rides with stage 2.
    const int last = SourceStageHeapBytes(*b.pending, b.derived, 3) ? 3 : 2;
    {
        struct HeapBatch
        {
            IGraphicsDevice& g;
            HeapBatch(IGraphicsDevice& gg, uint64_t bytes) : g(gg) { g.BeginBufferHeapBatch(bytes); }
            ~HeapBatch() { g.EndBufferHeapBatch(); }
        } heapBatch{ gfx, SourceStageHeapBytes(*b.pending, b.derived, b.stage) };
        ScopedUploadBatch uploadBatch(gfx);
        if (!BuildSourceStage(gfx, *b.pending, b.derived, b.stage, b.s))
        {
            LOG_WARNING("SkinnedMeshSubsystem: source 0x%016llx stage %d failed - not cached",
                        static_cast<unsigned long long>(b.pending->sourceKey), b.stage);
            b.stage = 4;   // the destructor frees what was made
            return false;
        }
        if (b.stage == 2 && last == 2) BuildSourceStage(gfx, *b.pending, b.derived, 3, b.s);
    }
    if (b.stage < last) { ++b.stage; return true; }
    b.stage = 4;
    // Every stage in: the cache owns the set from here - unless another build of
    // the same key landed first (then this one's buffers go with it).
    if (m_sharedSources.find(b.pending->sourceKey) == m_sharedSources.end())
    {
        m_sharedSources.emplace(b.pending->sourceKey, std::move(b.s));
        b.finished = true;
        LOG_INFO("SkinnedMeshSubsystem: shared source 0x%016llx built in stages — %zu verts, %zu idx, skeleton %u, "
                 "morphs %u", static_cast<unsigned long long>(b.pending->sourceKey), b.pending->restPositions.size(),
                 b.pending->indices.size(), b.pending->skeletonIndex, b.pending->morphCount);
    }
    return false;
}

bool SkinnedMeshSubsystem::BuildSourceOnLoader(SourceBuild& b)
{
    if (b.finished || b.built || b.stage > 3 || !b.gfx || !b.pending) return false;
    PROF_SCOPE("Skin.BuildSourceOnLoader");
    IGraphicsDevice& gfx = *b.gfx;
    // Each stage's buffers placed in a heap of their size, as StepSourceBuild
    // places them; the loader scope is the one upload batch for all of them.
    for (; b.stage <= 3; ++b.stage)
    {
        gfx.BeginBufferHeapBatch(SourceStageHeapBytes(*b.pending, b.derived, b.stage));
        const bool ok = BuildSourceStage(gfx, *b.pending, b.derived, b.stage, b.s);
        gfx.EndBufferHeapBatch();
        if (!ok)
        {
            LOG_WARNING("SkinnedMeshSubsystem: source 0x%016llx stage %d failed on the loader - not cached",
                        static_cast<unsigned long long>(b.pending->sourceKey), b.stage);
            b.stage = 4;   // the destructor frees what was made
            return false;
        }
    }
    b.built = true;
    return true;
}

bool SkinnedMeshSubsystem::PublishSourceBuild(SourceBuild& b)
{
    if (!b.built || b.finished || !b.pending) return false;
    b.built = false;
    if (m_sharedSources.find(b.pending->sourceKey) != m_sharedSources.end()) return false;
    m_sharedSources.emplace(b.pending->sourceKey, std::move(b.s));
    b.finished = true;
    LOG_INFO("SkinnedMeshSubsystem: shared source 0x%016llx built on a loader worker - %zu verts, %zu idx, skeleton %u, "
             "morphs %u", static_cast<unsigned long long>(b.pending->sourceKey), b.pending->restPositions.size(),
             b.pending->indices.size(), b.pending->skeletonIndex, b.pending->morphCount);
    return true;
}

bool SkinnedMeshSubsystem::PublishSharedSource(IGraphicsDevice& gfx, const SceneLoader::SkinnedMeshPending& pending,
                                               const SharedSourceDerived* derived)
{
    if (pending.sourceKey == 0) return false;
    if (m_sharedSources.find(pending.sourceKey) != m_sharedSources.end()) return true;
    SharedSkinnedSource built;
    if (!BuildSharedSource(gfx, pending, derived, built)) return false;
    m_sharedSources.emplace(pending.sourceKey, std::move(built));
    return true;
}

uint32_t SkinnedMeshSubsystem::AdvanceSkinJobsWrite()
{
    const uint32_t emitIdx = m_skinWrite;
    m_skinWrite ^= 1u;
    return emitIdx;
}

void SkinnedMeshSubsystem::PublishSkinJobsRead(uint32_t emitIdx)
{
    m_skinRead = emitIdx;
    if (m_skinningPass)
    {
        m_skinningPass->SetJobs(&m_skinPayload[m_skinRead].jobs);
        m_skinningPass->SetMorphWeightPool(&m_skinPayload[m_skinRead].morphWeights);
        m_skinningPass->SetCurPosSlot(m_skinPayload[m_skinRead].curPosSlot);
    }
}

void SkinnedMeshSubsystem::PublishSkinJobs()
{
    PublishSkinJobsRead(AdvanceSkinJobsWrite());
}

bool SkinnedMeshSubsystem::ReadPoolWeights(Entity skeleton, std::vector<std::pair<uint16_t, float>>& out)
{
    out.clear();
    m_poolSnapshotArmed.store(true, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(m_poolSnapshotMutex);
    const auto it = m_poolSnapshot.find(skeleton);
    if (it == m_poolSnapshot.end()) return false;
    out = it->second;
    return true;
}
