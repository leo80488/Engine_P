// Renderer_SceneGather.cpp - BuildRenderScene's gather: the per-entity
// material sync (BuildScene_SyncMaterials), the scene BVH cull
// (BuildScene_CullBVH), the debug wireframes, and the candidate gathers -
// primitives and lights, mesh library refs, billboards (with the debug and
// glyph icon bindless slots), beams and afterimages.

#include "Renderer/Renderer.h"
#include "ECS/LightUtil.h"   // light direction derived from transform (+Z)

// engine graphics / backend
#include "Graphics/GraphicsDX12.h"
#include "System/HostPolicy.h"   // Sys::IsEditorHost (EditorOnlyTag culling)
#include "Graphics/BeamSystem.h"
#include "Graphics/AfterimageSystem.h"
#include "Graphics/WaterRippleSystem.h"

// render passes
#include "RenderGraph/RenderPass/DebugWirePass.h"

// ECS components + systems
#include "ECS/ProxyVolumeComponent.h"
#include "ECS/BillboardComponent.h"
#include "ECS/BeamComponent.h"
#include "ECS/MaterialSchema.h"
#include "ECS/CharacterControllerComponent.h"   // KCC capsule debug-wire
#include "Physics/PhysicsSystem.h"               // PhysicsSystem::DrawDebug (the Physics debug category)

// scene
#include "Scene/MeshSpawner.h"

// engine systems
#include "System/JobDefaults.h"
#include "System/CommandLine.h" // harness A/B and verify flags, -phaselog_every
#include "System/TaskSystem.h"
#include "System/TaskParallelSort.h"
#include "System/TaskGraph.h"

// STL / DirectXMath
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cstring>
#include <execution>
#include <cmath>
#include <string_view>
#include <type_traits>

using namespace DirectX;

// CPU-side cbuffer mirrors and their layout static_asserts live in Renderer.h
// (RendererDetail namespace).
using PerViewCB       = RendererDetail::PerViewCB;
using LightCB         = RendererDetail::LightCB;

// The material bits and the material-packing helpers Renderer_Scene.cpp and
// its splits share.
#include "Renderer/RendererSceneShared.h"


void Renderer::BuildScene_SyncMaterials(World& world)
{
    ZoneScopedN("BS.SyncMaterials");
    auto* pMaterial = world.GetPool<MaterialComponent>();
    // No material system this frame -> nothing pending (Promote below re-counts).
    m_matTexUnsettled  = 0;
    if (m_texSys && m_resMgr && pMaterial)
    {
        auto& matEnts = pMaterial->Entities();
        auto& matData = pMaterial->Data();
        const size_t matN = matData.size();

        // Phase 1 - bind each authored material to its shared runtime resource.
        for (size_t mi = 0; mi < matN; ++mi)
        {
            const Entity e = matEnts[mi];
            if (!world.IsAlive(e)) continue;

            // Dirty gate. The runtime key hashes only STRUCTURAL fields (texture /
            // shader paths, custom bindings), never dynamic scalar params (see
            // BuildMaterialRuntimeKey), so a bound material whose authored layout is
            // unchanged keeps its key and re-deriving it every frame is pure waste. A
            // NOT-yet-bound material (first appearance: load / deserialize / prefab /
            // VFX spawn) always syncs regardless of the dirty flag, so no load path has
            // to remember to mark dirty. The bound test reads the m_matBoundBits byte
            // mirror: a map probe here was the steady-state cost of the whole loop.
            const bool bound = e < m_matBoundBits.size() && m_matBoundBits[e] != 0;
            if (bound && !matData[mi].IsDirty())
            {
#ifndef NDEBUG
                // Debug-only invariant (compiled out of Release): a clean, bound material
                // MUST still hash to its stored key. A mismatch means some code mutated a
                // key-affecting field without SetDirty, the one failure a pure dirty gate
                // cannot tolerate. Log the entity and self-heal, so it is caught at author
                // time instead of shipping as a silently stale binding. The map probe here
                // also cross-checks the byte mirror.
                auto bindIt = m_entityMaterialRuntimeBindings.find(e);
                const uint64_t sk = BuildMaterialShaderKey(matData[mi]);
                if (bindIt == m_entityMaterialRuntimeBindings.end())
                {
                    LOG_ERROR("BS.SyncMaterials: entity %u bound-bit set without a map "
                              "entry — mirror out of sync, self-healing",
                              static_cast<uint32_t>(e));
                    SyncMaterialTextures(e, matData[mi]);
                }
                else if (BuildMaterialRuntimeKey(matData[mi], sk) != bindIt->second.resourceKey)
                {
                    LOG_ERROR("BS.SyncMaterials: entity %u mutated a material key field "
                              "without SetDirty (missing SetEditDirty at the mutation "
                              "site) — self-healing this frame",
                              static_cast<uint32_t>(e));
                    SyncMaterialTextures(e, matData[mi]);
                }
#endif
                continue;
            }

            SyncMaterialTextures(e, matData[mi]);
            matData[mi].SetDirty(false);
            // This entity's material identity may have changed: m_sceneMatDirty surfaces it
            // to the retained consumers (candidate cache, DDGI scene AS, spot-caster
            // tracking, instance dirty stamps, the add classifier).
            if (m_retainedSceneDiff) m_sceneMatDirty.push_back(e);
        }

        // Phase 2 - promote async-ready textures ONCE per shared resource, not
        // once per binding entity (identical layouts share one resource).
        PromoteRuntimeMaterialTextures();

        // The entities bound to a resource still settling (only on a frame that
        // has one): each retained cache asks whether its own rows bind any
        // (TexUnsettledTouches) instead of re-gathering the scene.
        m_matTexUnsettledEntities.clear();
        if (m_matTexUnsettled != 0 && !m_matTexUnsettledKeys.empty())
            for (const auto& [ent, binding] : m_entityMaterialRuntimeBindings)
                if (m_matTexUnsettledKeys.count(binding.resourceKey))
                    m_matTexUnsettledEntities.push_back(ent);

        const size_t entityCount = m_entityMaterialRuntimeBindings.size();
        const size_t entryCount = m_runtimeMaterialResources.size();
        if (entityCount > m_runtimeMaterialEntityHighWater ||
            entryCount > m_runtimeMaterialResourceHighWater)
        {
            size_t ownedTextureRefs = 0;
            for (const auto& [_key, entry] : m_runtimeMaterialResources)
            {
                for (const MatTexEntry& texture : entry.engineTextures)
                    ownedTextureRefs +=
                        texture.handle != Resource::kInvalidTextureHandle;
                for (const auto& [_name, texture] : entry.customTextures)
                    ownedTextureRefs +=
                        texture.handle != Resource::kInvalidTextureHandle;
            }

            m_runtimeMaterialEntityHighWater = std::max(
                m_runtimeMaterialEntityHighWater,
                static_cast<uint32_t>(entityCount));
            m_runtimeMaterialResourceHighWater = std::max(
                m_runtimeMaterialResourceHighWater,
                static_cast<uint32_t>(entryCount));
            LOG_INFO("Renderer material runtime high-water: entities=%zu "
                     "resources=%zu sharedBindings=%zu ownedTextureRefs=%zu",
                     entityCount, entryCount,
                     entityCount >= entryCount ? entityCount - entryCount : 0,
                     ownedTextureRefs);
        }
    }

    // Flush the deferred texture releases queued by the previous OnWorldClear.
    // Order is critical: the Phase 1 bind loop above already Acquired every
    // texture path the NEW world uses, so a path shared with the outgoing world
    // has refcount 2 in TextureSystem; releasing now drops it to 1 and the slot
    // and its GPU upload survive. Paths NOT in the new world drop to 0 ->
    // FreeSlot -> m_pendingDestroy (TextureSystem::Tick frees on the next
    // BeginFrame, already deferred).
    if (m_texSys && !m_pendingMatTexReleaseAfterNextSync.empty())
    {
        for (Resource::TextureHandle h : m_pendingMatTexReleaseAfterNextSync)
            m_texSys->Release(h, m_gfx);
        m_pendingMatTexReleaseAfterNextSync.clear();
    }
}

void Renderer::BuildScene_CullBVH(World& world,
                                  std::vector<uint8_t>& bvhVisibleMask,
                                  std::vector<uint8_t>& bvhTestedMask)
{
    ZoneScopedN("BS.CullBVH");
    auto* pMeshLibRef = world.GetPool<MeshLibRef>();
    auto* pWorldAabb  = world.GetPool<WorldAabb>();
    auto* pSkinned    = world.GetPool<MeshSkinnedComponent>();
    auto pGet = [](auto* p, Entity e) { return p ? p->Get(e) : nullptr; };
    // O(1) exclusive bound on entity IDs. assign() keeps the member masks'
    // capacity across frames.
    const size_t idBound = world.EntityIdBound();
    bvhVisibleMask.assign(idBound, 0);
    bvhTestedMask .assign(idBound, 0);
    if (m_gpuCullingEnabled)
    {
        // Member scratch: capacity persists, zero steady-state allocation. In
        // retained mode the CONTENT also persists across frames (the leaf arrays ARE
        // the retained store); only the full path clears it.
        auto& bvhEntities = m_bvhScratchEntities;
        auto& bvhAABBs    = m_bvhScratchAABBs;
        auto& bvhTypes    = m_bvhScratchTypes;

        // Retained-leaf gate: patch only when the shared membership gate held
        // (continuous diff, no add/remove/dirtyOther, no tag-set boundary crossing)
        // or the frame only added dynamic leaves, the last full gather left no
        // unresolvable rows, and nothing forced a topology rebuild (OnWorldClear's
        // MarkRebuild). Everything else re-runs the full gather below.
        const SceneDiffView diff = GetSceneDiffView();
        // WorldAabb membership is invisible to the diff (not a SceneItemKey
        // field): a component removed from a still-alive static leaf would
        // leave its stale AABB in the tree. The pool count pins the full path
        // on any WorldAabb add/remove; adds also arrive via added/skippedRows.
        const uint32_t worldAabbCount = pWorldAabb
            ? static_cast<uint32_t>(pWorldAabb->Data().size()) : 0u;
        // A frame that only adds DYNAMIC leaves patches too: the new leaves go in
        // beside the tree as loose leaves and the next full build takes them in.
        // Their WorldAabbs are the count's expected growth.
        const bool addFrame = m_bvhAddPatchFrame;
        const bool usePatch =
            m_retainedBvh && m_bvhRetainedValid && (m_sceneMembershipStable || addFrame) &&
            m_bvhSkippedRows == 0 && (worldAabbCount == m_bvhWorldAabbCount || addFrame) &&
            !m_sceneBVH.NeedsRebuild() &&
            m_sceneBVH.GetLeafCount() == static_cast<uint32_t>(bvhEntities.size());
        m_bvhRetainedValid = false; // re-armed by whichever path completes below

        bool patched = false;
        if (usePatch)
        {
            ZoneScopedN("BS.CullBVH.Patch");
            bool refitAny = false;
            patched = true;
            if (addFrame)
            {
                PROF_SCOPE("BRS.CullBVH.AddLeaves");
                // The leaves the full gather would take for this frame's adds (a valid
                // MeshLibRef with a WorldAabb), with its type rule. A leaf-to-be without its
                // box, a WorldAabb count that moved by anything but the adds, or too many
                // loose leaves: the full path.
                uint32_t addedBoxes = 0;
                for (Entity e : *diff.added)
                {
                    if (!world.IsAlive(e)) continue;
                    const WorldAabb* aabb = pGet(pWorldAabb, e);
                    if (aabb) ++addedBoxes;
                    const MeshLibRef* mlr = pGet(pMeshLibRef, e);
                    if (!mlr || !mlr->IsValid()) continue;
                    // The full path's two pins: a leaf without its box, and one the scene
                    // store does not track (it would never see dirtyXform).
                    const uint32_t tid = static_cast<uint32_t>(e);
                    if (!aabb || tid >= m_sceneStore.tracked.size() || !m_sceneStore.tracked[tid])
                    { patched = false; break; }
                    const SceneBVH::AABB box{ aabb->min, aabb->max };
                    const SceneBVH::LeafType type = pGet(pSkinned, e) ? SceneBVH::LeafType::Skinned
                                                                      : SceneBVH::LeafType::Static;
                    const uint32_t li = static_cast<uint32_t>(bvhEntities.size());
                    bvhEntities.push_back(e);
                    bvhAABBs.push_back(box);
                    bvhTypes.push_back(type);
                    m_sceneBVH.AddLooseLeaf(e, box, type);
                    const uint32_t id = static_cast<uint32_t>(e);
                    if (id >= m_bvhEntityToLeaf.size()) m_bvhEntityToLeaf.resize(static_cast<size_t>(id) + 1, 0xFFFFFFFFu);
                    m_bvhEntityToLeaf[id] = li;
                    if (type == SceneBVH::LeafType::Skinned) m_bvhDynamicLeaves.push_back(li);
                }
                const uint32_t looseLimit = (std::max)(64u, m_sceneBVH.GetLeafCount() / 8u);
                if (patched && (worldAabbCount != m_bvhWorldAabbCount + addedBoxes ||
                                m_sceneBVH.GetLooseLeafCount() > looseLimit))
                    patched = false;
                if (patched) m_bvhWorldAabbCount = worldAabbCount;
            }
            // Refit one persisted leaf from the WorldAabb pool, the same source the
            // full gather reads (refreshed by TransformSystem before BuildRenderScene).
            // false = the leaf lost its AABB source mid-patch -> the caller bails to
            // the full gather.
            auto patchLeaf = [&](Entity e) -> bool
            {
                const uint32_t id = static_cast<uint32_t>(e);
                if (id >= m_bvhEntityToLeaf.size()) return true; // not a leaf
                const uint32_t li = m_bvhEntityToLeaf[id];
                if (li == 0xFFFFFFFFu) return true;              // not a leaf
                const WorldAabb* aabb = pGet(pWorldAabb, e);
                if (!aabb) return false;
                const SceneBVH::AABB na{ aabb->min, aabb->max };
                if (std::memcmp(&na, &bvhAABBs[li], sizeof(na)) != 0)
                {
                    bvhAABBs[li] = na;
                    m_sceneBVH.RefitLeaf(bvhEntities[li], na);
                    refitAny = true;
                }
                return true;
            };
            if (patched)
                for (Entity e : *diff.dirtyXform)
                    if (!patchLeaf(e)) { patched = false; break; }
            // Skinned leaves: bone animation moves WorldAabb without ANY transform
            // dirt (the diff cannot see it), so re-read them every patch frame. Small
            // N; the bit-compare inside patchLeaf keeps a rest-pose character from
            // forcing RefitInternal.
            if (patched)
                for (uint32_t li : m_bvhDynamicLeaves)
                    if (!patchLeaf(bvhEntities[li])) { patched = false; break; }
            if (patched)
            {
                if (refitAny) m_sceneBVH.RefitInternal();
                m_bvhRetainedValid = true;
            }
            // A bailed patch may already have refit some leaves. Harmless: the full
            // path below regathers fresh AABBs and its memcmp runs against
            // m_bvhPrevAABBs (the last FULL frame's baseline), so any real change
            // still triggers the whole-tree refit.
        }
        if (!patched)
        {
        // Own zone: BS.CullBVH.Patch shows only that a patch was attempted; a bail
        // lands here and pays a full re-gather + whole-tree refit, which would
        // otherwise hide inside the parent zone.
        ZoneScopedN("BS.CullBVH.Full");
        bvhEntities.clear();
        bvhAABBs.clear();
        bvhTypes.clear();

        // Order-independent hash of the leaf entity-ID set, accumulated during the
        // gather. Detects a wholesale set swap that keeps the leaf COUNT (world
        // reload between same-sized scenes; Play->Stop AdoptState restore), which
        // would otherwise take the incremental refit against the previous world's
        // stale tree (RefitLeaf updates AABBs but NOT m_leafEntities): FrustumCull
        // would return the OLD world's IDs and cull the new entities. XOR+ADD of a
        // multiplicatively mixed id is order-independent (gather order may differ
        // after a reload) yet still distinguishes {A,B} from {A,C}.
        uint64_t setSig = 0;
        uint32_t skippedRows = 0;
        if (pMeshLibRef)
        {
            auto& mlrEnts = pMeshLibRef->Entities();
            auto& mlrData = pMeshLibRef->Data();
            const size_t mlrN = mlrData.size();
            for (size_t k = 0; k < mlrN; ++k)
            {
                const Entity e = mlrEnts[k];
                if (!world.IsAlive(e)) continue;
                if (!mlrData[k].IsValid()) continue;
                const WorldAabb* aabb = pGet(pWorldAabb, e);
                // Patch-gate input: a valid ref without a WorldAabb becomes a leaf on
                // some LATER frame with no diff event (WorldAabb is not part of
                // SceneItemKey), so while any such row exists the patch path stays
                // disarmed.
                if (!aabb) { ++skippedRows; continue; }

                bvhEntities.push_back(e);
                bvhAABBs.push_back({ aabb->min, aabb->max });

                const MeshSkinnedComponent* skMesh = pGet(pSkinned, e);
                bvhTypes.push_back(skMesh ? SceneBVH::LeafType::Skinned
                                          : SceneBVH::LeafType::Static);

                const uint64_t h = (static_cast<uint64_t>(e) + 1ull) * 0x9E3779B97F4A7C15ull;
                setSig ^= h;
                setSig += h;
            }
        }

        const uint32_t entityCount = static_cast<uint32_t>(bvhEntities.size());

        const bool setChanged = (setSig != m_cullBvhSetSig);
        m_cullBvhSetSig = setSig;

        if (m_sceneBVH.NeedsRebuild() || m_sceneBVH.GetLeafCount() != entityCount || setChanged)
        {
            // Full SAH rebuild: first frame, entity count changed, or the entity SET
            // changed (reload / snapshot restore) even at an unchanged count.
            m_sceneBVH.Build(bvhEntities.data(), bvhAABBs.data(), bvhTypes.data(), entityCount);
            m_bvhPrevAABBs = bvhAABBs;
        }
        else
        {
            // Incremental refit: same entity set, leaves just moved - update leaf
            // AABBs, then propagate bottom-up. Skip both when no leaf AABB changed since
            // last frame (the bit-compare is exact: a static scene recomputes identical
            // WorldAabb floats every frame), so a fully static scene pays only the
            // gather + FrustumCull.
            const bool aabbsChanged =
                m_bvhPrevAABBs.size() != bvhAABBs.size()
                || (entityCount > 0
                    && std::memcmp(m_bvhPrevAABBs.data(), bvhAABBs.data(),
                                   entityCount * sizeof(SceneBVH::AABB)) != 0);
            if (aabbsChanged)
            {
                for (uint32_t i = 0; i < entityCount; ++i)
                    m_sceneBVH.RefitLeaf(bvhEntities[i], bvhAABBs[i]);
                m_sceneBVH.RefitInternal();
                m_bvhPrevAABBs = bvhAABBs;
            }
        }

        // ---- (Re)index the persisted leaves for the patch path ----
        // m_bvhPrevAABBs stays: the flag-off fallback needs its memcmp refit-skip,
        // and every full frame re-baselines it; on patch frames the dirty feed
        // subsumes it (per-leaf compare), so the two can never disagree at a full
        // frame.
        m_bvhSkippedRows = skippedRows;
        if (m_retainedBvh)
        {
            m_bvhEntityToLeaf.assign(idBound, 0xFFFFFFFFu);
            m_bvhDynamicLeaves.clear();
            for (uint32_t i = 0; i < entityCount; ++i)
            {
                const uint32_t id = static_cast<uint32_t>(bvhEntities[i]);
                if (id < m_bvhEntityToLeaf.size())
                    m_bvhEntityToLeaf[id] = i;
                if (bvhTypes[i] == SceneBVH::LeafType::Skinned)
                    m_bvhDynamicLeaves.push_back(i);
                // Untracked leaves (no GlobalTransform -> absent from the scene store)
                // never receive dirtyXform: pin the full path.
                if (id >= m_sceneStore.tracked.size() ||
                    !m_sceneStore.tracked[id])
                    ++m_bvhSkippedRows;
            }
            m_bvhWorldAabbCount = worldAabbCount;
            m_bvhRetainedValid  = diff.valid;
        }
        } // end full-gather path

        // The tree above is built / refit unconditionally: Renderer_DDGI's
        // QueryAABB probe capture needs it every frame regardless of culling. Only
        // the frustum cull + mask fill below are skippable, and only when nothing
        // reads the masks this frame (see BvhCullMasksNeeded).
        if (BvhCullMasksNeeded())
        {
            auto& visibleEntities = m_bvhScratchVisible;
            visibleEntities.clear();
            m_sceneBVH.FrustumCull(FrameViewWrite().view.boundingFrustum, visibleEntities);

            for (Entity e : visibleEntities) if (e < bvhVisibleMask.size()) bvhVisibleMask[e] = 1;
            for (Entity e : bvhEntities)     if (e < bvhTestedMask .size()) bvhTestedMask [e] = 1;
        }
#ifdef TRACY_ENABLE
        TracyPlot("BS.CullBVH.MasksNeeded", BvhCullMasksNeeded() ? 1.0 : 0.0);
#endif
        // Stamp the tree as this frame's: Renderer::CullView queries it only for
        // the stamped frame.
        m_bvhStampFrame = FrameViewWrite().currentFrame;
        m_bvhStampValid = true;
    }
    else
    {
        // Culling disabled -> the persisted leaves go stale (no gather runs);
        // force a full gather when it comes back on.
        m_bvhRetainedValid = false;
        m_bvhStampValid    = false;   // so CullView never queries the stale tree
    }
}

void Renderer::BuildScene_DebugWireframes(World& world)
{
#ifdef WITH_HARNESS
    // Test hook: -debug_wire_aabbs draws every entity's AABB, as the editor's
    // AABB debug category does. Nothing else turns the debug wires on in a run
    // (the editor's DebugDrawSystem sets these flags before the frame; this
    // runs after it).
    static const bool s_wireAabbs = Sys::CommandLine::Get().Has("-debug_wire_aabbs");
    if (s_wireAabbs && m_debugWirePass)
    {
        m_debugWirePass->enabled   = true;
        m_debugWirePass->showAABBs = true;
    }
#ifdef JPH_DEBUG_RENDERER
    // -debug_wire_physics draws the Jolt bodies and the character capsules (PhysicsSystem::DrawDebug) and
    // logs "[PHYS_DEBUG] bodies=N drawn=N ... mismatch=0|1" every 30 frames: drawn is what the draw walked,
    // bodies Physics.BodyCount(). A later -debug_wire_physics=off turns it back off.
    static const bool s_wirePhysics = Sys::CommandLine::Get().Has("-debug_wire_physics") &&
                                      Sys::CommandLine::Get().Get("-debug_wire_physics") != "off";
    if (s_wirePhysics && m_debugWirePass)
    {
        m_debugWirePass->enabled     = true;
        m_debugWirePass->showPhysics = true;
    }
#endif
#endif
    if (m_debugWirePass && m_debugWirePass->enabled)
    {
        m_debugWirePass->Clear();
        m_skin.GetIKRigSystem()->DrawDebug(world,*m_debugWirePass);

        if (m_debugWirePass->showAABBs)
        {
            for (Entity e : world.GetEntities())
            {
                if (!world.IsAlive(e)) continue;
                // The Chain Physics Preview's stage (y = -4096) is never main-viewport
                // content.
                if (world.HasComponent<PreviewStageTag>(e)) continue;
                const WorldAabb* wa = world.GetComponent<WorldAabb>(e);
                if (wa && wa->min.x <= wa->max.x)
                    m_debugWirePass->AddAABB(wa->min, wa->max, 0xFF00FF00); // green
            }
        }

        if (m_debugWirePass->showFrustum)
        {
            // Compute frustum corners from inverse viewProj.
            XMMATRIX invVP = XMLoadFloat4x4(&FrameViewWrite().view.viewProjMatrixNoJitter);
            invVP = XMMatrixInverse(nullptr, invVP);

            XMFLOAT3 frustumCorners[8];
            static const XMFLOAT3 kNdcCorners[8] = {
                {-1, -1, 0}, { 1, -1, 0}, {-1,  1, 0}, { 1,  1, 0}, // near
                {-1, -1, 1}, { 1, -1, 1}, {-1,  1, 1}, { 1,  1, 1}, // far
            };
            for (int i = 0; i < 8; ++i)
            {
                XMVECTOR v = XMVector3TransformCoord(
                    XMLoadFloat3(&kNdcCorners[i]), invVP);
                XMStoreFloat3(&frustumCorners[i], v);
            }
            m_debugWirePass->AddFrustum(frustumCorners, 0xFF00FFFF); // yellow
        }

        if (m_debugWirePass->showCapsules)
        {
            for (Entity e : world.GetEntities())
            {
                if (!world.IsAlive(e)) continue;
                // The preview stage KEEPS its CapsuleColliderComponent (the chain
                // solver's own collider); it is not main-viewport debug content.
                if (world.HasComponent<PreviewStageTag>(e)) continue;
                const auto* capComp = world.GetComponent<CapsuleColliderComponent>(e);
                if (!capComp || capComp->count == 0) continue;

                // Find the skeleton: on this entity or via SkeletonRef.
                Entity skelEntity = e;
                const auto* skelComp = world.GetComponent<SkeletonComponent>(e);
                if (!skelComp)
                {
                    const SkeletonRef* ref = world.GetComponent<SkeletonRef>(e);
                    if (ref && ref->entity != NullEntity)
                    {
                        skelEntity = ref->entity;
                        skelComp = world.GetComponent<SkeletonComponent>(skelEntity);
                    }
                }
                if (!skelComp || skelComp->assetIndex == kInvalidSkeletonIndex) continue;

                const SkeletonAsset& skel = m_skin.GetSkeletonRegistry().Get(skelComp->assetIndex);
                AnimationSystem::LocalPose* poses = m_skin.GetAnimationSystem()
                    ? m_skin.GetAnimationSystem()->GetMutableLocalPose(skelEntity) : nullptr;
                if (!poses) continue;

                // Skeleton entity's world transform (skeleton space -> world space).
                const GlobalTransform* gt = world.GetComponent<GlobalTransform>(skelEntity);
                XMMATRIX entityWorld = gt ? XMLoadFloat4x4(&gt->matrix) : XMMatrixIdentity();

                // The radius must carry the entity's world scale, or a scaled-up character
                // draws correctly spaced endpoints with a far too thin radius.
                // ChainPhysicsSystem builds its colliders in MODEL space (bone matrices
                // without the entity transform, raw def.radius) and the entity transform is
                // applied at skinning, so the effective WORLD radius is exactly def.radius x
                // entity scale. Bone scale is excluded: it moves the endpoints, but the sim
                // never applies it to the radius. Max of the three basis lengths: exact
                // under the uniform scale a capsule requires anyway.
                float entityScale = 1.f;
                {
                    const float sx = XMVectorGetX(XMVector3Length(entityWorld.r[0]));
                    const float sy = XMVectorGetX(XMVector3Length(entityWorld.r[1]));
                    const float sz = XMVectorGetX(XMVector3Length(entityWorld.r[2]));
                    entityScale = (std::max)(sx, (std::max)(sy, sz));
                    if (!(entityScale > 1e-6f)) entityScale = 1.f;   // degenerate / NaN guard
                }

                // Bone world matrix: local pose chain x entity transform.
                auto boneWorldMat = [&](uint32_t bi) -> XMMATRIX {
                    if (bi >= skel.boneCount) return XMMatrixIdentity();
                    uint32_t chain[SkeletonAsset::MAX_BONES];
                    uint32_t depth = 0;
                    int32_t cur = static_cast<int32_t>(bi);
                    while (cur >= 0 && depth < skel.boneCount)
                    { chain[depth++] = static_cast<uint32_t>(cur); cur = skel.parentIndex[cur]; }
                    XMMATRIX w = XMMatrixIdentity();
                    for (uint32_t d = depth; d > 0; --d)
                    {
                        auto& p = poses[chain[d-1]];
                        XMMATRIX S = XMMatrixScaling(p.scl.x, p.scl.y, p.scl.z);
                        XMMATRIX R = XMMatrixRotationQuaternion(XMLoadFloat4(&p.rot));
                        XMMATRIX T = XMMatrixTranslation(p.pos.x, p.pos.y, p.pos.z);
                        w = S * R * T * w;
                    }
                    return XMMatrixMultiply(w, entityWorld);
                };

                for (int ci = 0; ci < capComp->count; ++ci)
                {
                    const auto& def = capComp->capsules[ci];
                    if (!def.enabled) continue;
                    XMMATRIX wA = boneWorldMat(def.boneA);
                    XMMATRIX wB = boneWorldMat(def.boneB);
                    XMVECTOR posA = XMVectorAdd(wA.r[3], XMVector3TransformNormal(XMLoadFloat3(&def.offsetA), wA));
                    XMVECTOR posB = XMVectorAdd(wB.r[3], XMVector3TransformNormal(XMLoadFloat3(&def.offsetB), wB));
                    XMFLOAT3 wa, wb;
                    XMStoreFloat3(&wa, posA);
                    XMStoreFloat3(&wb, posB);
                    m_debugWirePass->AddCapsule(wa, wb, def.radius * entityScale, 0xFFFF8800);
                }
            }
        }

        // CharacterController (KCC) capsules, shown under Show Collision (the
        // gameplay collision shape) beside the static mesh-collider wireframe that
        // CollisionMesh::EmitDebugWireframe adds. PhysicsSystem writes the capsule
        // FEET into the transform (the Jolt shape is offset up by halfHeight +
        // radius via mShapeOffset) and the KCC never rotates the capsule, so the two
        // hemisphere-segment centres are rebuilt from the entity's world position +
        // the component's dims.
        if (m_debugWirePass->showCollision)
        {
            if (auto* ccPool = world.GetPool<CharacterControllerComponent>())
            {
                const auto& ccEnts = ccPool->Entities();
                auto&       ccData = ccPool->Data();
                for (size_t i = 0; i < ccEnts.size(); ++i)
                {
                    const Entity e = ccEnts[i];
                    if (!world.IsAlive(e)) continue;
                    const CharacterControllerComponent& cc = ccData[i];

                    XMFLOAT3 feet;
                    if (const GlobalTransform* gt = world.GetComponent<GlobalTransform>(e))
                        feet = { gt->matrix._41, gt->matrix._42, gt->matrix._43 };
                    else if (const LocalTransform* lt = world.GetComponent<LocalTransform>(e))
                        feet = lt->translation;
                    else
                        continue;

                    const float r  = cc.capsuleRadius;
                    const float hh = cc.capsuleHalfHeight;
                    const XMFLOAT3 segLo{ feet.x, feet.y + r,            feet.z };
                    const XMFLOAT3 segHi{ feet.x, feet.y + 2.f * hh + r, feet.z };
                    m_debugWirePass->AddCapsule(segLo, segHi, r, 0xFF00FF00); // green
                }
            }
        }

#ifdef JPH_DEBUG_RENDERER
        if (m_debugWirePass->showPhysics)
            if (DX12Physics::PhysicsSystem* physics = m_skin.GetPhysicsSystem())
            {
                const DX12Physics::DebugDrawStats drawn =
                    physics->DrawDebug(*m_debugWirePass, FrameViewWrite().view.cameraPosition);
#if WITH_HARNESS
                static int s_physicsDebugFrames = 0;
                if ((s_physicsDebugFrames++ % 30) == 0)
                    LOG_INFO("[PHYS_DEBUG] bodies=%u drawn=%u asleep=%u active=%u characters=%u wire=%d mismatch=%d",
                             physics->GetBodyCount(), drawn.bodies, drawn.asleep, physics->GetActiveBodyCount(),
                             drawn.characters, m_debugWirePass->HasLines() ? 1 : 0,
                             drawn.bodies == physics->GetBodyCount() ? 0 : 1);
#endif
            }
#endif

        if (m_debugWirePass->showReflectionProbes)
        {
            // Iterate the probe pool directly (Entities() / Data()).
            auto* probePool = world.GetPool<ReflectionProbeComponent>();
            if (probePool)
            {
                const auto& ents = probePool->Entities();
                auto&       data = probePool->Data();
                for (size_t i = 0; i < ents.size(); ++i)
                {
                    Entity e = ents[i];
                    if (!world.IsAlive(e)) continue;
                    const GlobalTransform* gt = world.GetComponent<GlobalTransform>(e);
                    if (!gt) continue;

                    const ReflectionProbeComponent& comp = data[i];
                    const XMFLOAT3 pos = { gt->matrix._41, gt->matrix._42, gt->matrix._43 };

                    // Orthonormal probe basis (scale stripped, matches the shader).
                    XMVECTOR bx = XMVector3Normalize(XMVectorSet(gt->matrix._11, gt->matrix._12, gt->matrix._13, 0));
                    XMVECTOR by = XMVector3Normalize(XMVectorSet(gt->matrix._21, gt->matrix._22, gt->matrix._23, 0));
                    XMVECTOR bz = XMVector3Normalize(XMVectorSet(gt->matrix._31, gt->matrix._32, gt->matrix._33, 0));
                    XMFLOAT3 xf, yf, zf; XMStoreFloat3(&xf, bx); XMStoreFloat3(&yf, by); XMStoreFloat3(&zf, bz);
                    const XMFLOAT4X4 probeRot(
                        xf.x, xf.y, xf.z, 0, yf.x, yf.y, yf.z, 0,
                        zf.x, zf.y, zf.z, 0, pos.x, pos.y, pos.z, 1);

                    // Influence volume (yellow): OBB (box) or sphere.
                    if (comp.influenceShape == EInfluenceShape::Sphere)
                        m_debugWirePass->AddSphere(pos, comp.influenceOuterRadius, 0xFF00E0FF);
                    else
                        m_debugWirePass->AddOBB(probeRot, comp.influenceHalfExtents, 0xFF00E0FF);

                    // Proxy volume (blue), only when an explicit proxy is bound.
                    if (Entity pe = comp.proxyVolume.Resolve(world); pe != NullEntity)
                    {
                        const ProxyVolumeComponent* pv = world.GetComponent<ProxyVolumeComponent>(pe);
                        const GlobalTransform* pgt = world.GetComponent<GlobalTransform>(pe);
                        if (pv && pgt)
                        {
                            XMVECTOR px = XMVector3Normalize(XMVectorSet(pgt->matrix._11, pgt->matrix._12, pgt->matrix._13, 0));
                            XMVECTOR py = XMVector3Normalize(XMVectorSet(pgt->matrix._21, pgt->matrix._22, pgt->matrix._23, 0));
                            XMVECTOR pz = XMVector3Normalize(XMVectorSet(pgt->matrix._31, pgt->matrix._32, pgt->matrix._33, 0));
                            XMVECTOR pcen = XMVectorAdd(
                                XMVectorSet(pgt->matrix._41, pgt->matrix._42, pgt->matrix._43, 0),
                                XMVectorAdd(XMVectorAdd(XMVectorScale(px, pv->centerOffsetLS.x),
                                                        XMVectorScale(py, pv->centerOffsetLS.y)),
                                            XMVectorScale(pz, pv->centerOffsetLS.z)));
                            XMFLOAT3 pcenf; XMStoreFloat3(&pcenf, pcen);
                            if (pv->shape == EProxyShape::Sphere)
                            {
                                m_debugWirePass->AddSphere(pcenf, pv->sphereRadius, 0xFFFF8000);
                            }
                            else if (pv->shape == EProxyShape::OrientedBox)
                            {
                                XMFLOAT3 pxf, pyf, pzf; XMStoreFloat3(&pxf, px); XMStoreFloat3(&pyf, py); XMStoreFloat3(&pzf, pz);
                                const XMFLOAT4X4 proxRot(
                                    pxf.x, pxf.y, pxf.z, 0, pyf.x, pyf.y, pyf.z, 0,
                                    pzf.x, pzf.y, pzf.z, 0, pcenf.x, pcenf.y, pcenf.z, 1);
                                m_debugWirePass->AddOBB(proxRot, pv->halfExtents, 0xFFFF8000);
                            }
                            // EProxyShape::Infinite -> nothing to draw.
                        }
                    }

                    // Capture point (white).
                    XMVECTOR capv = XMVectorAdd(XMLoadFloat3(&pos),
                        XMVectorAdd(XMVectorAdd(XMVectorScale(bx, comp.captureOffsetLS.x),
                                                XMVectorScale(by, comp.captureOffsetLS.y)),
                                    XMVectorScale(bz, comp.captureOffsetLS.z)));
                    XMFLOAT3 capf; XMStoreFloat3(&capf, capv);
                    m_debugWirePass->AddSphere(capf, 0.15f, 0xFFFFFFFF);
                }
            }
        }

        // ---- DDGI volumes + probe grid ----
        // Yellow volume AABB + an RGB-coded cross per probe, to check placement and
        // spacing.
        if (m_debugWirePass->showDDGIVolumes)
        {
            auto* volPool = world.GetPool<DDGIVolumeComponent>();
            if (volPool)
            {
                const auto& ents = volPool->Entities();
                auto&       data = volPool->Data();
                for (size_t i = 0; i < ents.size(); ++i)
                {
                    if (!world.IsAlive(ents[i])) continue;
                    const DDGIVolumeComponent& v = data[i];

                    // Volume AABB (yellow).
                    XMFLOAT3 mn{ v.origin.x - v.extent.x,
                                 v.origin.y - v.extent.y,
                                 v.origin.z - v.extent.z };
                    XMFLOAT3 mx{ v.origin.x + v.extent.x,
                                 v.origin.y + v.extent.y,
                                 v.origin.z + v.extent.z };
                    m_debugWirePass->AddAABB(mn, mx, 0xFF00FFFF);

                    // Per-probe crosses only when the volume's debugDraw flag is set (off by
                    // default).
                    if (!v.debugDraw) continue;
                    const float spacingX = (v.probeCountsX > 1)
                        ? (2.0f * v.extent.x) / float(v.probeCountsX - 1) : 0.0f;
                    const float spacingY = (v.probeCountsY > 1)
                        ? (2.0f * v.extent.y) / float(v.probeCountsY - 1) : 0.0f;
                    const float spacingZ = (v.probeCountsZ > 1)
                        ? (2.0f * v.extent.z) / float(v.probeCountsZ - 1) : 0.0f;
                    const float crossSize = std::min({ spacingX, spacingY, spacingZ }) * 0.18f;
                    for (uint32_t z = 0; z < v.probeCountsZ; ++z)
                    for (uint32_t y = 0; y < v.probeCountsY; ++y)
                    for (uint32_t x = 0; x < v.probeCountsX; ++x)
                    {
                        XMFLOAT3 p{
                            mn.x + spacingX * float(x),
                            mn.y + spacingY * float(y),
                            mn.z + spacingZ * float(z) };
                        // RGB-encoded probe coord (R = +x, G = +y, B = +z).
                        const uint8_t r = uint8_t(255.0f * float(x) / std::max(1u, v.probeCountsX - 1));
                        const uint8_t g = uint8_t(255.0f * float(y) / std::max(1u, v.probeCountsY - 1));
                        const uint8_t b = uint8_t(255.0f * float(z) / std::max(1u, v.probeCountsZ - 1));
                        const uint32_t color = 0xFF000000u | (r) | (g << 8) | (b << 16);
                        m_debugWirePass->AddCross(p, crossSize, color);
                    }
                }
            }
        }

        // ---- Decal projection volumes ----
        // Each DecalComponent projects onto surfaces inside the unit cube
        // [-0.5,0.5]^3 in decal-local space, transformed by the entity world matrix
        // (see the decal resolve in BuildScene_UploadLights and the
        // `abs(localPos) > 0.5` clip in DecalApply.cs.hlsl). Draw that exact OBB so
        // the region a decal affects is visible. The decal-local +Z axis
        // (decalForwardWS) is the projection direction; a cyan arrow from the box
        // centre to the +Z face shows which way it faces.
        if (m_debugWirePass->showDecals)
        {
            auto* pDecal = world.GetPool<DecalComponent>();
            auto* pGT    = world.GetPool<GlobalTransform>();
            auto* pMatDW = world.GetPool<MaterialComponent>();
            if (pDecal && pGT)
            {
                const auto& ents = pDecal->Entities();
                auto&       data = pDecal->Data();
                for (size_t i = 0; i < ents.size(); ++i)
                {
                    const Entity e = ents[i];
                    if (!world.IsAlive(e)) continue;
                    const GlobalTransform* gt = pGT->Get(e);
                    if (!gt) continue;
                    const DecalComponent& dc = data[i];

                    // Orange when the instance contributes a decal this frame: a DecalComponent
                    // library asset (dc.material) or an entity Material with domain == Decal
                    // (the same precedence as the gather in BuildScene_UploadLights). Dim grey
                    // otherwise, so empty placements stay visible while debugging.
                    const MaterialComponent* dwMc = pMatDW ? pMatDW->Get(e) : nullptr;
                    const bool contributes =
                        (dwMc && dwMc->IsDecalDomain()) || dc.material;
                    const uint32_t boxColor = contributes ? 0xFF0080FFu : 0x80808080u;
                    m_debugWirePass->AddOBB(gt->matrix, { 0.5f, 0.5f, 0.5f }, boxColor);

                    // Projection-direction arrow along decal-local +Z, transformed
                    // to world. Tip at the +Z face; small 4-line head so the facing
                    // direction reads unambiguously from any viewing angle.
                    const XMMATRIX M = XMLoadFloat4x4(&gt->matrix);
                    auto localToWorld = [&](float x, float y, float z) {
                        XMFLOAT3 o;
                        XMStoreFloat3(&o, XMVector3TransformCoord(XMVectorSet(x, y, z, 1.f), M));
                        return o;
                    };
                    const uint32_t arrowColor = 0xFFFFFF00u; // cyan
                    const XMFLOAT3 ctr = localToWorld(0.f, 0.f, 0.f);
                    const XMFLOAT3 tip = localToWorld(0.f, 0.f, 0.5f);
                    m_debugWirePass->AddLine(ctr, tip, arrowColor);
                    m_debugWirePass->AddLine(tip, localToWorld( 0.08f, 0.f,    0.40f), arrowColor);
                    m_debugWirePass->AddLine(tip, localToWorld(-0.08f, 0.f,    0.40f), arrowColor);
                    m_debugWirePass->AddLine(tip, localToWorld( 0.f,   0.08f,  0.40f), arrowColor);
                    m_debugWirePass->AddLine(tip, localToWorld( 0.f,  -0.08f,  0.40f), arrowColor);
                }
            }
        }
    }
}

void Renderer::BuildScene_GatherPrimitivesAndLights(World& world,
        std::vector<DrawCandidate>& candidates, const XMFLOAT3& camForwardThisFrame,
        bool dynamicOnly)
{
    ZoneScopedN("BS.GatherPrimLights");
    auto* pLightData  = world.GetPool<LightData>();
    auto* pVolLight   = world.GetPool<VolumetricLightComponent>();
    auto* pGlobalXf   = world.GetPool<GlobalTransform>();
    auto* pMeshHandle = world.GetPool<MeshHandle>();
    auto* pVisibility = world.GetPool<VisibilityComponent>();
    auto* pMaterial   = world.GetPool<MaterialComponent>();
    auto pGet = [](auto* p, Entity e) { return p ? p->Get(e) : nullptr; };
    auto* lb = m_lightCB.Upload(m_gfx);
    // Clear the directional-sun COLOUR before the gather. The CB is
    // triple-buffered and never wholesale-cleared, so a scene that loses its
    // last directional light (deleted, or none + TOD off) would otherwise ghost
    // the previous frame's sun in both Lighting.ps and DDGI (which reads the
    // same LightCB sun). The gather below and the TOD override re-fill it, so
    // this only matters when there is no sun ("no directional -> no sun").
    // lightDir is left as is: a stale direction is harmless at zero colour, and
    // zeroing it would feed normalize(0) = NaN into Lighting.ps.
    if (lb) { lb->lightColor[0] = lb->lightColor[1] = lb->lightColor[2] = 0.0f; }
    // Same reasoning for the untinted mirror the sky/cloud passes read: a
    // sunless frame must not hand them the previous frame's sun colour.
    m_sunColorTOA = { 0.0f, 0.0f, 0.0f };
    // ---- Light gather ----
    // Pool walk + an explicit creation-order sort. m_frameLights order is
    // load-bearing: shadow slices go to the FIRST kMaxCasters, the LightCB sun
    // is the LAST directional. LightData::gatherSeq (stamped on first sight,
    // immune to pool compaction) gives a deterministic order, so the sun is the
    // most recently CREATED directional: file order for a loaded scene, and
    // predictable after runtime spawns.
    struct LightRow { Entity e; const LightData* ld; uint32_t seq; };
    static thread_local std::vector<LightRow> t_lightRows;
    t_lightRows.clear();
    if (pLightData)
    {
        auto&       ldata = pLightData->Data();
        const auto& lents = pLightData->Entities();
        t_lightRows.reserve(lents.size());
        for (std::size_t li = 0; li < lents.size() && li < ldata.size(); ++li)
        {
            if (!world.IsAlive(lents[li])) continue;
            if (ldata[li].gatherSeq == 0) ldata[li].gatherSeq = ++m_lightGatherSeq;
            t_lightRows.push_back({ lents[li], &ldata[li], ldata[li].gatherSeq });
        }
        std::sort(t_lightRows.begin(), t_lightRows.end(),
                  [](const LightRow& a, const LightRow& b) { return a.seq < b.seq; });
    }
    for (const LightRow& lrow : t_lightRows)
    {
        const Entity e = lrow.e;
        const LightData* ld = lrow.ld;
        if (!ld->enabled) continue;
        {
            const GlobalTransform* lgt = pGet(pGlobalXf, e);
            // Direction is the entity's forward (+Z) axis (see LightUtil.h). A light
            // with no transform cannot be aimed, so it gets the straight-down fallback.
            XMFLOAT3 wPos = { 0,0,0 }, wDir = { 0.f, -1.f, 0.f };
            if (lgt)
            {
                wPos = { lgt->matrix._41, lgt->matrix._42, lgt->matrix._43 };
                wDir = LightUtil::DirectionFromWorldF(lgt->matrix);
            }
            if (ld->type == LightType::Directional && lb)
            {
                XMVECTOR dir = XMVector3Normalize(XMLoadFloat3(&wDir));
                XMStoreFloat3(reinterpret_cast<XMFLOAT3*>(lb->lightDir), dir);
                // SetLightCBSun (not a raw lightColor write) filters the sun reaching
                // surfaces by the atmosphere it crossed: an authored white sun lands warm,
                // like Unreal's Atmosphere Sun Light. It must run HERE, not at the end of
                // the build: BuildScene_UpdateDDGI reads LightCB.lightColor later in the
                // same build, so tinting later would light DDGI's indirect bounce with a
                // white sun while raster went warm. lightDir is written first: the
                // transmittance depends on it.
                SetLightCBSun(world, { ld->color.x * ld->intensity,
                                       ld->color.y * ld->intensity,
                                       ld->color.z * ld->intensity });
            }
            m_frameLights.emplace_back(ResolvedLight{
                wPos, ld->radius, ld->color, ld->intensity,
                wDir, ld->spotAngle, ld->type,
                ld->castsShadow, 0xFFFFFFFFu });
            m_frameLights.back().key = lrow.seq;
            m_frameLights.back().sourceRadius =
                ld->type == LightType::Directional ? 0.f : std::max(0.f, ld->sourceRadius);

            // VolumetricLightComponent opts the light into the fog (sun=CSM god-rays, point/spot=froxel shafts).
            const VolumetricLightComponent* vc = pGet(pVolLight, e);
            const bool volumetric = vc && vc->enabled;
            // The path tracers' medium: every light scatters, scaled or opted out by the component.
            const float mediumScale = vc ? (vc->enabled ? std::max(vc->intensityScale, 0.f) : 0.f) : 1.f;
            m_frameLights.back().mediumScale = mediumScale;
            if (ld->type == LightType::Directional) m_sunMediumScale = mediumScale;   // the last one is the sun

            if (volumetric && ld->type == LightType::Directional)
            {
                // The sun's god-rays go through the fog pass's SetSun (a flag + scale); with
                // several volumetric directionals the last in gather order sets the scale.
                m_sunVolumetric        = true;
                m_sunVolumetricScale   = vc->intensityScale;
            }
            else if (volumetric && ld->type != LightType::Directional)
            {
                VolumetricFogPass::VolLight vl{};
                vl.position       = wPos;
                vl.radius         = ld->radius;
                vl.color          = ld->color;
                vl.intensity      = ld->intensity * vc->intensityScale;
                vl.direction      = wDir;
                vl.spotAngle      = ld->spotAngle;
                vl.type           = static_cast<uint32_t>(ld->type);
                vl.shadowSliceIdx = 0xFFFFFFFFu; // filled after slice assignment in UploadLights
                VolLightsWrite().push_back(vl);
            }
        }
    }

    // ---- Primitive mesh entities (MeshHandle + GlobalTransform) ----
    // Pool iteration (Entities() / Data(), never GetEntities() + per-entity Get)
    // keeps the dynamicOnly early-out O(primitives) instead of O(every entity id
    // ever issued). Candidate order follows the MeshHandle pool;
    // BuildScene_SortAndEmit normalizes it. Scene-mesh entities are in the
    // MeshLibRef gather, not here.
    if (!pMeshHandle) return;
    {
        const auto&  mhEnts = pMeshHandle->Entities();
        const auto&  mhData = pMeshHandle->Data();
        const size_t mhN    = mhData.size();
        for (size_t k = 0; k < mhN; ++k)
        {
            const Entity      e  = mhEnts[k];
            const MeshHandle& mh = mhData[k];
            if (!world.IsAlive(e)) continue;
            if (!mh.IsValid() ||
                mh.gpuMeshID >= static_cast<uint32_t>(PrimitiveMeshType::Count))
                continue;

            // Candidate-cache patch frames (dynamicOnly): this entity's candidates were
            // restored from the cache, so re-gathering it here would double-draw.
            if (dynamicOnly)
            {
                const uint32_t id = static_cast<uint32_t>(e);
                if (id < m_candEntityToIdx.size() &&
                    m_candEntityToIdx[id] != 0xFFFFFFFFu)
                    continue;
            }
            // Honour VisibilityComponent (explicit + inheritedHidden propagation from TransformSystem).
            uint32_t primViewMask  = ViewBit::All;
            bool     primMainPass  = true;
            bool     primCastShadow = true;
            if (const VisibilityComponent* v = pGet(pVisibility, e))
            {
                if (!v->IsEffectivelyVisible()) continue;
                primViewMask   = v->viewMask;
                primMainPass   = v->RendersInMainPass();
                primCastShadow = v->CastsShadow();
            }

            const auto& mesh = m_meshMgr.GetPrimitive(mh.gpuMeshID);
            // Not resident yet = transient: counts as unresolved, so the retained
            // candidate cache rebuilds every frame until it resolves.
            if (mesh.meshDescSlot == RHI::kInvalidBufferIndex)
            { ++m_candUnresolved; continue; }

            // GlobalTransform is the single source of truth.
            const GlobalTransform* gt = pGet(pGlobalXf, e);
            if (!gt) continue;
            const XMFLOAT4X4 worldMtx = gt->matrix;

            DrawCandidate c;
            c.entity       = e;
            c.perObjectShadowCaster = m_playerShadowSet.count(e) ? 1u : 0u;
            c.hairShadowCaster      = m_hairShadowSet.count(e)   ? 1u : 0u;
            if (const auto overlay = m_faceOverlayParams.find(e);
                overlay != m_faceOverlayParams.end())
            {
                c.faceOverlay       = 1u;
                c.faceOverlayParams = overlay->second;
            }
            if (m_hasDissolve)
                if (const auto dis = m_dissolveParams.find(e);
                    dis != m_dissolveParams.end())
                {
                    c.dissolveParams    = dis->second.params;
                    c.dissolveEdgeColor = dis->second.edgeColor;
                }
            c.viewMask          = primViewMask;
            c.renderInMainPass  = primMainPass;
            c.castShadow        = primCastShadow ? 1u : 0u;
            c.meshDescSlot = mesh.meshDescSlot;
            c.indexCount   = mesh.indexCount;
            XMStoreFloat4x4(&c.worldMatrix,
                XMMatrixTranspose(XMLoadFloat4x4(&worldMtx)));
            // Primitives are unit-sized and centred: pivot == centroid for the depth sort.
            c.worldCenter = { worldMtx.m[3][0], worldMtx.m[3][1], worldMtx.m[3][2] };
            c.mc = pGet(pMaterial, e);
            c.matBits = ComputeMatBits(c.mc);
            if (c.mc)
            {
                // Resolve the runtime material ONCE, then read every slot off it.
                const ResolvedMaterialRuntime rt = ResolveMaterialRuntime(e);
                c.texBaseColor  = rt.EngineTex(MaterialComponent::BASECOLORMAP).gpuHandle;
                c.texSurfaceMap = rt.EngineTex(MaterialComponent::SURFACEMAP).gpuHandle;
                c.texNormalMap  = rt.EngineTex(MaterialComponent::NORMALMAP).gpuHandle;
                c.customPSID    = ResolveCustomPSID(e, *c.mc);
                c.customVSID    = ResolveCustomVSID(e, *c.mc);
                // Resolve first: reflection sync can seed customParams, and the
                // effective material hash must include those seeded defaults.
                c.matHash       = HashMatKeys(c.mc, rt, c.orderKey);
            }
            ApplyBlendMode(c, camForwardThisFrame);
            candidates.push_back(c);
            // Custom candidate for outlines: push-then-mutate avoids full DrawCandidate memcpy.
            // Editor picking-outline forces this path on even when the material flag is off.
            const bool materialOutline = c.mc && c.mc->IsOutlineEnabled()
                                         && c.filter == DrawFilter::Opaque;
            // Picking outline rides on sub-passes 2+3 (silhouette mask + screen-space),
            // so it works for transparent entities too; the material outline cannot,
            // because inverted-hull blending assumes opaque depth.
            const bool pickingOutline  = c.mc
                                         && m_pickingOutlineEntity != NullEntity
                                         && e == m_pickingOutlineEntity
                                         && (c.filter == DrawFilter::Opaque
                                             || c.filter == DrawFilter::Transparent
                                             || c.filter == DrawFilter::Water);
            if (materialOutline || pickingOutline)
            {
                // Picking outline takes visual priority over material outline.
                const bool  drawPicking        = pickingOutline;
                const float outlinePixels      = drawPicking ? 0.0f
                                                 : c.mc->outlinePixels;
                const bool  screenSpaceOutline = drawPicking ? true
                                                 : c.mc->IsOutlineScreenSpaceEnabled();
                candidates.push_back(c);
                DrawCandidate& outline = candidates.back();
                outline.filter             = DrawFilter::Custom;
                outline.perm               = {};
                outline.outlinePixels      = outlinePixels;
                outline.screenSpaceOutline = screenSpaceOutline;
                outline.isPickingOutline   = drawPicking;
            }
        }
    }
}

void Renderer::BuildScene_GatherMeshLibRefs(World& world,
        std::vector<DrawCandidate>& candidates,
        const std::vector<uint8_t>& bvhTestedMask,
        const std::vector<uint8_t>& bvhVisibleMask,
        const XMFLOAT3& camForwardThisFrame,
        bool deferCull, bool dynamicOnly)
{
    ZoneScopedN("BS.GatherMeshLib");
    auto* pMeshLibRef = world.GetPool<MeshLibRef>();
    auto* pGlobalXf   = world.GetPool<GlobalTransform>();
    auto* pVisibility = world.GetPool<VisibilityComponent>();
    auto* pSkinned    = world.GetPool<MeshSkinnedComponent>();
    auto* pSkinOut    = world.GetPool<SkinningOutputComponent>();
    auto* pWorldAabb  = world.GetPool<WorldAabb>();
    auto* pMaterial   = world.GetPool<MaterialComponent>();
    auto pGet = [](auto* p, Entity e) { return p ? p->Get(e) : nullptr; };
    auto inMask = [](const std::vector<uint8_t>& m, Entity e) -> bool {
        return e < m.size() && m[e] != 0;
    };
    if (m_meshLib)
    {
        struct MlrJob {
            Entity                 e;
            const MeshLibRef*      mlr;
            const GlobalTransform* gt;
            uint32_t               meshDescSlot;
            uint32_t               indexCount;
            uint32_t               viewMask;        // copied into DrawPacket
            bool                   castShadowVis;   // VisibilityComponent.CastShadow bit
            bool                   renderMainPass;  // VisibilityComponent.RenderInMainPass bit
            // Custom shader dynamic IDs, resolved before processJob: processJob runs
            // on parallel workers and must not call ResolveCustomPSID/VSID (they mutate
            // the MaterialComponent and register into shared shader libraries), so the
            // worker only copies the ids.
            uint32_t               customPSID;
            uint32_t               customVSID;
            // Per-object (player) shadow caster bit from m_playerShadowSet, resolved
            // before processJob so the workers never touch the shared unordered_set.
            bool                   perObjShadow;
            // Hair (bangs) shadow caster bit from m_hairShadowSet, resolved the same
            // way.
            bool                   hairShadow;
            // Brows/lashes-through-hair overlay settings, resolved the same way.
            bool                   faceOverlay;
            uint32_t               faceOverlayParams;
            // Per-instance dissolve params from m_dissolveParams (all zero when the
            // entity is not dissolving).
            DirectX::XMFLOAT4      dissolveParams;
            DirectX::XMFLOAT4      dissolveEdgeColor;
            // GPU-skinned draw (skinningActive), copied into DrawCandidate::skinned:
            // the instance record then skips Hi-Z occlusion culling
            // (kInstanceFlagNoOcclusionCull).
            bool                   skinned;
            // SkinningOutputComponent::prevPosElementBase for THIS frame. Safe to read
            // here: BuildSkinJobs runs in the scheduler's Animation phase
            // (Renderer::TickAnimationChain), well before BuildRenderScene. 0xFFFFFFFF
            // when there is no valid previous deformation: the VS then falls back to
            // zero bone motion.
            uint32_t               prevPosElementBase;
        };
        // Bind a LOCAL reference to owner TLS. Workers capture this reference,
        // never resolve the TLS name on their own thread. Every live job is
        // rebuilt, so no World pointer survives into the next frame/reload.
        thread_local std::vector<MlrJob> t_retainedMlrJobs;
        auto& mlrJobs = t_retainedMlrJobs;
        mlrJobs.clear();
        // Structural-hide tag pools, read once. BVH leaves deliberately include
        // hidden entities, so a visibility flip refits instead of forcing a
        // structural rebuild.
        auto* pDisabledTag    = world.GetPool<DisabledTag>();
        // EditorOnly / HiddenInGame entities are culled by a shipped game and kept
        // by an authoring host. Runtime check, NOT #ifdef WITH_EDITOR: this TU is in
        // EngineCore.lib, compiled once for every exe without WITH_EDITOR (see
        // System/HostPolicy.h). Null pools in an editor host keep the
        // `p && p->Get(e)` tests free.
        const bool cullEditorOnly = !Sys::IsEditorHost();
        auto* pEditorOnlyTag  = cullEditorOnly ? world.GetPool<EditorOnlyTag>()   : nullptr;
        auto* pHiddenInGame   = cullEditorOnly ? world.GetPool<HiddenInGameTag>() : nullptr;
        if (pMeshLibRef)
        {
            // Sub-zone: the job-list pre-pass, owned by the main thread (mesh
            // registration and shader resolves never run on workers; see Two passes).
            ZoneScopedN("BS.Gather.Serial");
            PROF_SCOPE("BRS.Gather.Serial");
            auto& mlrEnts = pMeshLibRef->Entities();
            auto& mlrData = pMeshLibRef->Data();
            const size_t mlrN = mlrData.size();
            mlrJobs.reserve(mlrN);

            // ---- Two passes ----
            // Most of this loop is READS (pool lookups, the visibility / tag filters,
            // the player / hair / overlay / dissolve sets); only two steps mutate shared
            // state: mesh registration (MeshManager caches) and the custom-shader
            // resolve (an operator[] insert, a registration, a reflection sync, a flag
            // write). As one serial loop it cost ~0.8 ms of main-thread time at 3,000
            // skinned sections a frame. So evalJob runs twice:
            //   1. PARALLEL, serialOK=false: an entity is evaluated completely unless
            //      it reaches one of those steps with work still due - a non-skinned
            //      mesh (kRegister: everything but slot + index count is filled) or a
            //      material whose ids / reflection are not settled (kSerialShader);
            //   2. SERIAL, in the original order: kReady jobs are appended as
            //      evaluated, kRegister jobs register their mesh, kSerial* entities run
            //      evalJob again with serialOK=true.
            // Same jobs in the same order as one serial loop: a kReady result cannot be
            // changed by an earlier entity's registration or sync (those touch that
            // entity's own material and ITS shader key, and a shared unsettled key sends
            // both entities to pass 2). -gatherpreserial (or < 512 rows) = the single
            // serial loop.
            enum : uint8_t { kSkip = 0, kReady = 1, kSerial = 2, kSerialShader = 3, kRegister = 4 };
            // The steady-state answer of ResolveCustomPSID + ResolveCustomVSID,
            // READ-ONLY: false whenever either would insert, register, sync or
            // write (the owner then calls them for real in pass 2).
            auto peekCustomIDs = [this](const MaterialComponent& m, uint32_t& ps, uint32_t& vs) -> bool
            {
                return PeekCustomShaderIDs(m, ps, vs);
            };
            auto evalJob = [&](size_t k, bool serialOK, MlrJob& out) -> uint8_t
            {
                bool needsRegister = false;
                    const Entity e = mlrEnts[k];
                    // Candidate-cache patch frames (dynamicOnly): entities restored from the
                    // cache are skipped; only the dynamic lane re-gathers.
                    if (dynamicOnly)
                    {
                        const uint32_t id = static_cast<uint32_t>(e);
                        if (id < m_candEntityToIdx.size() &&
                            m_candEntityToIdx[id] != 0xFFFFFFFFu)
                            return kSkip;
                    }
                    if (!world.IsAlive(e)) return kSkip;
                    const MeshLibRef& mlr = mlrData[k];
                    if (!mlr.IsValid()) return kSkip;
                    const GlobalTransform* gt = pGet(pGlobalXf, e);
                    if (!gt) return kSkip;

                    // Author-intent filter. An entity without a VisibilityComponent is fully
                    // visible in every view: a missing mask must not make it silently
                    // disappear.
                    uint32_t entityViewMask  = ViewBit::All;
                    bool     entityCastShadow = true;
                    bool     entityMainPass   = true;
                    if (const VisibilityComponent* v = pGet(pVisibility, e))
                    {
                        if (!v->IsEffectivelyVisible()) return kSkip;
                        entityViewMask   = v->viewMask;
                        entityCastShadow = v->CastsShadow();
                        entityMainPass   = v->RendersInMainPass();
                    }
                    // Structural-hide tags: always skip Disabled; outside an editor host also
                    // skip EditorOnly / HiddenInGame.
                    if (pDisabledTag    && pDisabledTag   ->Get(e)) return kSkip;
                    if (pEditorOnlyTag  && pEditorOnlyTag ->Get(e)) return kSkip;
                    if (pHiddenInGame   && pHiddenInGame  ->Get(e)) return kSkip;

                    // MeshDesc slot: skinned + active -> skinning slot; skinned + inactive ->
                    // meshlib (rest pose); non-skinned -> meshlib. BuildSkinJobs patches the
                    // skinning slot to SkinnedVertexRing.
                    const MeshSkinnedComponent*    skMesh  = pGet(pSkinned, e);
                    const SkinningOutputComponent* skinOut = pGet(pSkinOut, e);
                    // This gate MUST match BuildSkinJobs' patch gate (which also requires the
                    // blend / restPos / restNrm SRVs). Otherwise a skinned mesh whose input
                    // buffers are not resident yet would be emitted as a skinned draw that
                    // BuildSkinJobs never patches off the placeholder descriptor, and it would
                    // draw through a garbage descriptor. Not-yet-ready skinned meshes fall
                    // through to the rest-pose meshlib slot instead.
                    const bool skinningActive =
                        skMesh && skinOut && skinOut->poseByteOffset != ~0u
                        && skMesh->meshDescriptorIdx != RHI::kInvalidBufferIndex
                        && skMesh->blendSRVHandle && skMesh->restPosSRVHandle
                        && skMesh->restNrmSRVHandle;

                    uint32_t slot = RHI::kInvalidBufferIndex;
                    uint32_t indexCountForDraw = 0;
                    if (skinningActive)
                    {
                        slot              = skMesh->meshDescriptorIdx;
                        indexCountForDraw = skMesh->indexCount;
                    }
                    else
                    {
                        // Mesh registration mutates MeshManager's caches, so only the owner
                        // (serialOK) makes it: pass 1 leaves slot + index count to pass 2
                        // (kRegister), which makes exactly these calls, in order, with the same
                        // skip on failure. A library still loading is transient: it counts as
                        // unresolved, so the retained candidate cache keeps rebuilding until it
                        // resolves.
                        if (!serialOK)
                            needsRegister = true;
                        else
                        {
                            slot = m_meshMgr.RegisterMeshLibMesh(*m_meshLib, mlr);
                            if (slot == RHI::kInvalidBufferIndex)
                            { ++m_candUnresolved; return kSkip; }
                            const auto* entry = m_meshLib->GetEntry(mlr.libHandle, mlr.meshId);
                            if (!entry) { ++m_candUnresolved; return kSkip; }
                            indexCountForDraw = entry->indexCount;
                        }
                    }
                    // Custom shader ids are resolved here, before processJob, so the parallel
                    // workers only copy them; without this the imported-mesh path ignores a
                    // material's custom VS/PS. Pass 1 only peeks and defers to pass 2 while a
                    // registration is due.
                    const MaterialComponent* mcJob = pGet(pMaterial, e);
                    uint32_t jobPSID = 0u, jobVSID = 0u;
                    if (mcJob)
                    {
                        if (serialOK)
                        {
                            jobPSID = ResolveCustomPSID(e, *mcJob);
                            jobVSID = ResolveCustomVSID(e, *mcJob);
                        }
                        else if (!peekCustomIDs(*mcJob, jobPSID, jobVSID))
                            return kSerialShader;   // a registration / sync / flag write is still due
                    }
                    // Player / bangs / overlay / dissolve membership is resolved here (read-only)
                    // so processJob never reads the shared sets.
                    const bool jobPerObj   = m_playerShadowSet.count(e) != 0;
                    const bool jobHair     = m_hairShadowSet.count(e)   != 0;
                    const auto overlayIt = m_faceOverlayParams.find(e);
                    const bool jobOverlay = overlayIt != m_faceOverlayParams.end();
                    const uint32_t jobOverlayParams = jobOverlay ? overlayIt->second : 0u;
                    DirectX::XMFLOAT4 jobDissolve     = { 0.0f, 0.0f, 0.0f, 0.0f };
                    DirectX::XMFLOAT4 jobDissolveEdge = { 0.0f, 0.0f, 0.0f, 0.0f };
                    if (m_hasDissolve)
                        if (const auto disIt = m_dissolveParams.find(e);
                            disIt != m_dissolveParams.end())
                        {
                            jobDissolve     = disIt->second.params;
                            jobDissolveEdge = disIt->second.edgeColor;
                        }
                    out = MlrJob{ e, &mlr, gt, slot, indexCountForDraw,
                                        entityViewMask, entityCastShadow, entityMainPass,
                                        jobPSID, jobVSID, jobPerObj, jobHair,
                                        jobOverlay, jobOverlayParams,
                                        jobDissolve, jobDissolveEdge, skinningActive,
                                        skinningActive ? skinOut->prevPosElementBase
                                                       : 0xFFFFFFFFu };
                    return needsRegister ? kRegister : kReady;
            };

            static const bool s_serialPre = Sys::CommandLine::Get().Has("-gatherpreserial");
            if (s_serialPre || mlrN < 512)
            {
                for (size_t k = 0; k < mlrN; ++k)
                {
                    MlrJob j;
                    if (evalJob(k, true, j) == kReady) mlrJobs.push_back(j);
                }
            }
            else
            {
                // Owner TLS, captured by reference (the same rule as mlrJobs).
                thread_local std::vector<MlrJob>  t_pre;
                thread_local std::vector<uint8_t> t_kind;
                auto& pre  = t_pre;
                auto& kind = t_kind;
                pre.resize(mlrN);
                kind.resize(mlrN);
                {
                    PROF_SCOPE("BRS.Gather.PreEval");
                    TaskSystem::Get().ParallelFor(0, static_cast<uint32_t>(mlrN), [&](uint32_t k)
                    {
                        kind[k] = evalJob(k, false, pre[k]);
                    });
                }
                uint32_t nReady = 0, nMesh = 0, nShader = 0;
                for (size_t k = 0; k < mlrN; ++k)
                {
                    if (kind[k] == kReady)
                    {
                        mlrJobs.push_back(pre[k]);
                        ++nReady;
                    }
                    else if (kind[k] == kRegister)
                    {
                        // The one step pass 1 left out, exactly as the serial
                        // body does it (same call, same skips, same counter).
                        ++nMesh;
                        MlrJob& j = pre[k];
                        const uint32_t slot = m_meshMgr.RegisterMeshLibMesh(*m_meshLib, *j.mlr);
                        if (slot == RHI::kInvalidBufferIndex) { ++m_candUnresolved; continue; }
                        const auto* entry = m_meshLib->GetEntry(j.mlr->libHandle, j.mlr->meshId);
                        if (!entry) { ++m_candUnresolved; continue; }
                        j.meshDescSlot = slot;
                        j.indexCount   = entry->indexCount;
                        mlrJobs.push_back(j);
                    }
                    else if (kind[k] >= kSerial)
                    {
                        ++nShader;
                        MlrJob j;
                        if (evalJob(k, true, j) == kReady) mlrJobs.push_back(j);
                    }
                }
                static const int s_every = Sys::CommandLine::Get().Has("-phaselog_every")
                    ? Sys::CommandLine::Get().GetInt("-phaselog_every", 60) : 0;
                static uint64_t s_n = 0;
                if (s_every > 0 && ++s_n % static_cast<uint64_t>(s_every) == 0)
                    LOG_INFO("[GATHER_PRE] entities=%zu ready=%u registered=%u serial=%u dynamicOnly=%d",
                             mlrN, nReady, nMesh, nShader, dynamicOnly ? 1 : 0);
            }
        }

        // Per-entity work; used from both serial fallback and parallel tasks.
        auto processJob = [&](const MlrJob& j, std::vector<DrawCandidate>& out)
        {
            // Frustum cull with shadow fallback. In retained mode (deferCull)
            // BuildScene_ClassifyCandidates does this instead, so the emitted
            // candidate is camera-independent and cacheable.
            bool isShadowOnly = false;
            // Non-deferred gather only: a live secondary view can see this candidate
            // although the main camera cannot.
            bool wantSecondary = false;
            // WHICH secondary volumes see it (kSeenByPiP: the PiP's SecondaryView
            // form; kSeenByProbe: the baking probe's ProbeView form).
            uint32_t secondarySeen = 0;
            if (!deferCull)
            {
            // Transparent and water materials skip the camera-cull drop (alpha-blend
            // rarely casts shadow). Masked counts as opaque: it draws in the GBuffer
            // pass and keeps the normal cull path (IsTransparentBlendMode excludes it).
            const MaterialComponent* mcPre        = pGet(pMaterial, j.e);
            const bool               isTransparent =
                mcPre && (IsTransparentBlendMode(mcPre->userBlendMode)
                          || mcPre->IsWaterShadingModel());

            // Is this draw outside the camera frustum? BVH leaves answer from the cull
            // masks. Anything the BVH did not test falls back to its own WorldAabb; a
            // skinned entity's box is written from the ANIMATED pose by the bone merge
            // in TickAnimationChain earlier this frame. No AABB = no cull.
            // The GPU cull frustum-tests skinned sections too (BuildSkinJobs ->
            // SetMeshAABB publishes animated bounds; only Hi-Z is skipped), so this CPU
            // test is a packet-count optimisation: it turns an off-screen draw into a
            // shadow-only one before SortAndEmit, which the cull CS cannot do (it can
            // only zero an instance count, not move a draw between filters).
            bool outOfMainFrustum = false;
            const WorldAabb* cullAabb = pGet(pWorldAabb, j.e);
            if (m_gpuCullingEnabled && inMask(bvhTestedMask, j.e))
                outOfMainFrustum = !inMask(bvhVisibleMask, j.e);
            else if (cullAabb)
                outOfMainFrustum = !FrustumTestAABB(FrameViewWrite().view.boundingFrustum,
                                                    cullAabb->min, cullAabb->max);

            {
                if (outOfMainFrustum && !isTransparent)
                {
                    // Off-screen is not "gone": keep it as a SHADOW CASTER (DrawFilter::Shadow
                    // below). Dropping it would delete the shadow of everything behind the
                    // camera. With no live secondary view wantSecondary is false and
                    // `if (!shadowOk && !wantSecondary) return;` drops exactly the non-casters;
                    // a live PiP adds the third outcome: the main view is done with this
                    // candidate, but the secondary view can see it.
                    const bool shadowOk = [&]() -> bool
                    {
                        if (!ShadowRetentionValid()) return false;
                        // The shadow-only fallback needs both layers to cast: VisibilityComponent
                        // and the material.
                        if (!j.castShadowVis) return false;
                        const MaterialComponent* mcSh = pGet(pMaterial, j.e);
                        if (mcSh && !mcSh->IsCastingShadow()) return false;
                        if (!cullAabb) return false;
                        BoundingBox bb;
                        bb.Center  = { (cullAabb->min.x + cullAabb->max.x) * 0.5f,
                                       (cullAabb->min.y + cullAabb->max.y) * 0.5f,
                                       (cullAabb->min.z + cullAabb->max.z) * 0.5f };
                        bb.Extents = { (cullAabb->max.x - cullAabb->min.x) * 0.5f,
                                       (cullAabb->max.y - cullAabb->min.y) * 0.5f,
                                       (cullAabb->max.z - cullAabb->min.z) * 0.5f };
                        return ShadowRetains(bb);
                    }();
                    // Test m_secondaryCullActive FIRST: SecondaryFrustaSee lives in another TU
                    // and LTCG is off, so the call is real - one per OUT-OF-FRUSTUM candidate
                    // every frame, even with no PiP open. The bool makes this lane free when
                    // nothing is live (the rule NoteSecondaryViewCandidate states for the
                    // retained path).
                    if (m_secondaryCullActive && cullAabb)
                        secondarySeen = SecondaryFrustaSee(cullAabb->min, cullAabb->max);
                    wantSecondary = secondarySeen != 0;
                    if (!shadowOk && !wantSecondary) return;
                    isShadowOnly = shadowOk;
                }
            }
            } // !deferCull

            DrawCandidate c;
            c.entity       = j.e;
            c.perObjectShadowCaster = j.perObjShadow ? 1u : 0u;
            c.hairShadowCaster      = j.hairShadow   ? 1u : 0u;
            c.faceOverlay           = j.faceOverlay  ? 1u : 0u;
            c.faceOverlayParams     = j.faceOverlayParams;
            c.dissolveParams        = j.dissolveParams;
            c.dissolveEdgeColor     = j.dissolveEdgeColor;
            c.meshDescSlot = j.meshDescSlot;
            c.indexCount   = j.indexCount;
            c.skinned      = j.skinned ? 1u : 0u;
            // TAA motion vectors for animated geometry: without this GBuffer.vs takes
            // its "no previous deformation" fallback for every skinned draw, bone motion
            // produces zero velocity, and TAA smears a swinging limb. Feeds root
            // constant [3] on both the direct and the ExecuteIndirect path.
            c.prevPosElementBase = j.prevPosElementBase;
            // Stamp the entity's visibility decisions onto the candidate so per-pass
            // routing (ShadowPass viewMask, main-pass opt-out) is a bitfield check
            // downstream, with no per-pass ECS lookups.
            c.viewMask          = j.viewMask;
            c.renderInMainPass  = j.renderMainPass;
            c.castShadow        = j.castShadowVis ? 1u : 0u;
            XMStoreFloat4x4(&c.worldMatrix,
                XMMatrixTranspose(XMLoadFloat4x4(&j.gt->matrix)));
            // Depth-sort centroid: WorldAabb centre when present, else pivot fallback.
            if (const WorldAabb* aabb = pGet(pWorldAabb, j.e))
            {
                c.worldCenter = { (aabb->min.x + aabb->max.x) * 0.5f,
                                  (aabb->min.y + aabb->max.y) * 0.5f,
                                  (aabb->min.z + aabb->max.z) * 0.5f };
                // Same pointer, 24 more bytes: the scene-voxel cull in the render half
                // reads this instead of re-reading the World. Left at the inverted default
                // (hasAabb 0) when there is no box.
                c.aabbMin = aabb->min;
                c.aabbMax = aabb->max;
                c.hasAabb = 1;
            }
            else
            {
                c.worldCenter = { j.gt->matrix.m[3][0],
                                  j.gt->matrix.m[3][1],
                                  j.gt->matrix.m[3][2] };
            }
            c.mc = pGet(pMaterial, j.e);
            c.matBits = ComputeMatBits(c.mc);
            if (c.mc)
            {
                // Resolve once (read-only map lookups: safe on parallel workers).
                const ResolvedMaterialRuntime rt = ResolveMaterialRuntime(j.e);
                c.texBaseColor  = rt.EngineTex(MaterialComponent::BASECOLORMAP).gpuHandle;
                c.texSurfaceMap = rt.EngineTex(MaterialComponent::SURFACEMAP).gpuHandle;
                c.texNormalMap  = rt.EngineTex(MaterialComponent::NORMALMAP).gpuHandle;
                c.matHash       = HashMatKeys(c.mc, rt, c.orderKey);
                // Custom shader ids were resolved before processJob (see MlrJob).
                c.customPSID    = j.customPSID;
                c.customVSID    = j.customVSID;
            }
            // Meshlet gate, geometric half: a library entry with resident meshlets
            // (the slot's sidecar, written by the serial RegisterMeshLibMesh - a plain
            // read here), not skinned, no custom VS. The path half is the render
            // latch's.
            c.meshletBlocks = MeshletBlocksForSlot(c.meshDescSlot, c.skinned != 0, c.customVSID);

            // Water can never be shadow-only: the cull exemption keeps isShadowOnly
            // false for it, and ApplyBlendMode clears castShadow. Belt-and-braces here
            // because this branch OVERWRITES filter / perm: reaching it with a water
            // candidate would strip DrawFilter::Water and WATER_SURFACE, and the lake
            // would vanish (no pass claims Shadow-filtered draws in the main view)
            // while a solid slab of it appeared in the CSM.
            const bool shadowConvert = isShadowOnly
                                    && !(c.mc && c.mc->IsWaterShadingModel());

            // ---- Secondary-view packet, shadow-convert shape ----
            // `c` is about to become a Shadow caster the CSM needs, and that conversion
            // is LOSSY (perm / stencil ref / cull mode / depth are reset), so the
            // secondary view gets a SECOND packet built from the candidate as it stands
            // now. It is constructed inside this branch, so a scene with no secondary
            // view pays nothing per candidate. The secondary duplicate lands before
            // `c`.
            if (wantSecondary && shadowConvert)
            {
                DrawCandidate secBase = c;
                ApplyBlendMode(secBase, camForwardThisFrame);
                // The baking probe's copy, from the same shaded form (its own filter:
                // neither view reads the other's).
                if ((secondarySeen & kSeenByProbe) && WantsProbeCopy(secBase))
                {
                    out.push_back(secBase);
                    out.back().filter = DrawFilter::ProbeView;
                }
                if ((secondarySeen & kSeenByPiP) && RemapToSecondaryFilter(secBase)) out.push_back(secBase);
            }

            if (shadowConvert)
            {
                c.filter = DrawFilter::Shadow;
                c.perm   = {};
                if (MatBitCheck(m_frameStats.matBitsMismatches, c.matBits,
                        MB_ALPHA_TEST,
                        c.mc && c.mc->IsAlphaTestEnabled(), "ALPHA_TEST"))
                    c.perm.Set(PermutationKey::ALPHA_TEST, true);
            }
            else
            {
                ApplyBlendMode(c, camForwardThisFrame);
            }

            // ---- Secondary-view packet, in-place shape ----
            // `c` was NOT shadow-converted, so it reached this point ONLY because
            // wantSecondary rescued it from the drop: it is not a main-view packet at
            // all and is remapped in place rather than duplicated.
            if (wantSecondary && !shadowConvert)
            {
                // The baking probe's copy first, while `c` still carries its natural
                // filter. Probe only: `c` itself becomes that copy.
                const bool forProbe = (secondarySeen & kSeenByProbe) && WantsProbeCopy(c);
                if (!(secondarySeen & kSeenByPiP))
                {
                    if (!forProbe) return;   // no opaque form: only the probe could see it
                    c.filter = DrawFilter::ProbeView;
                }
                else
                {
                    if (forProbe)
                    {
                        out.push_back(c);
                        out.back().filter = DrawFilter::ProbeView;
                    }
                    if (!RemapToSecondaryFilter(c))
                    {
                        // No secondary form for this filter (Water / Custom): the
                        // candidate was only alive for the secondary view, so drop
                        // it rather than emit it into the main lists.
                        return;
                    }
                }
            }
            out.push_back(c);

            // Outline path: push-then-mutate avoids full DrawCandidate memcpy.
            // Editor picking-outline forces this path on even when the material flag is off.
            const bool materialOutline = !isShadowOnly && c.mc && c.mc->IsOutlineEnabled()
                                         && c.filter == DrawFilter::Opaque;
            // Picking outline works for both opaque and transparent (sub-pass 2/3 path).
            const bool pickingOutline  = !isShadowOnly && c.mc
                                         && IsPickingOutlined(j.e)
                                         && (c.filter == DrawFilter::Opaque
                                             || c.filter == DrawFilter::Transparent
                                             || c.filter == DrawFilter::Water);
            if (materialOutline || pickingOutline)
            {
                // Picking outline takes visual priority; must flow through sub-passes 2+3.
                const bool  drawPicking        = pickingOutline;
                const float outlinePixels      = drawPicking ? 0.0f
                                                 : c.mc->outlinePixels;
                const bool  screenSpaceOutline = drawPicking ? true
                                                 : c.mc->IsOutlineScreenSpaceEnabled();
                out.push_back(c);
                DrawCandidate& outline = out.back();
                outline.filter             = DrawFilter::Custom;
                outline.perm               = {};
                outline.outlinePixels      = outlinePixels;
                outline.screenSpaceOutline = screenSpaceOutline;
                outline.isPickingOutline   = drawPicking;
            }
        };

        const uint32_t N = static_cast<uint32_t>(mlrJobs.size());
        // Below this the fork/join overhead exceeds the work savings.
        constexpr uint32_t kParallelThreshold = 128;
        const unsigned numWorkers = TaskSystem::Get().GetWorkerCount();

        if (N >= kParallelThreshold && numWorkers > 1)
        {
            // Sub-zone: worker fan-out + the main thread's wait for the chunks.
            ZoneScopedN("BS.Gather.Parallel");
            PROF_SCOPE("BRS.Gather.Parallel");
            const uint32_t numChunks = (std::min)(static_cast<uint32_t>(numWorkers), N / 128u);
            const uint32_t chunkSize = (N + numChunks - 1) / numChunks;

            // Retained, cache-line-isolated per-worker shards (see m_gatherLanes).
            if (m_gatherLanes.size() < numChunks) m_gatherLanes.resize(numChunks);
            for (uint32_t t = 0; t < numChunks; ++t) m_gatherLanes[t].v.clear();

            // The owner keeps one coarse chunk; graph handles own completion
            // and drain borrowed inputs if either submission or the caller throws.
            Sys::CpuTaskScope gatherTasks(numChunks - 1);
            for (uint32_t t = 1; t < numChunks; ++t)
            {
                const uint32_t lo = (std::min)(N, t * chunkSize);
                const uint32_t hi = (std::min)(N, lo + chunkSize);
                gatherTasks.Submit([&, t, lo, hi]()
                {
                    ZoneScopedN("BS.Gather.Chunk.Worker");
                    ZoneValue(hi - lo);
                    auto& out = m_gatherLanes[t].v;
                    out.reserve((hi - lo) * 2 + 8);
                    for (uint32_t i = lo; i < hi; ++i)
                        processJob(mlrJobs[i], out);
                });
            }

            static bool s_reportedGather = false;
            if (!s_reportedGather)
            {
                s_reportedGather = true;
                LOG_INFO("[SPLITAUDIT][GATHERCHUNK] parallel path: %u chunks over %u jobs "
                         "(chunk 0 inline on the caller, %u pushed)",
                         numChunks, N, numChunks - 1);
            }

            // Chunk 0 on the calling thread. processJob only appends into this
            // lane's own vector, so it is the same work a worker would do.
            {
                auto& out0 = m_gatherLanes[0].v;
                ZoneScopedN("BS.Gather.Chunk.Caller");
                const uint32_t hi0 = (std::min)(N, chunkSize);
                out0.reserve(hi0 * 2 + 8);
                for (uint32_t i = 0; i < hi0; ++i)
                    processJob(mlrJobs[i], out0);
            }

            {
                ZoneScopedN("BS.Gather.JoinWait");
                gatherTasks.Join();
            }

            // Reserve once, copy-construct directly into unused storage.
            // resize+memcpy first default-initializes every DrawCandidate and
            // then overwrites it. Lane order is the chunk order.
            ZoneNamedN(gatherMergeZone, "BS.Gather.Merge", true);
            PROF_SCOPE("BRS.Gather.Merge");
            size_t total = 0;
            for (uint32_t i = 0; i < numChunks; ++i) total += m_gatherLanes[i].v.size();
            candidates.reserve(candidates.size() + total);
            for (uint32_t i = 0; i < numChunks; ++i)
            {
                const auto& lane = m_gatherLanes[i].v;
                candidates.insert(candidates.end(), lane.begin(), lane.end());
            }
        }
        else
        {
            for (uint32_t i = 0; i < N; ++i)
                processJob(mlrJobs[i], candidates);
        }
    }
}

void Renderer::BuildScene_GatherBillboards(World& world,
        std::vector<DrawCandidate>& candidates, const XMFLOAT3& camForwardThisFrame)
{
    auto* pBillboard = world.GetPool<BillboardComponent>();
    auto* pGlobalXf  = world.GetPool<GlobalTransform>();
    auto* pLightData = world.GetPool<LightData>();
    auto* pMaterial  = world.GetPool<MaterialComponent>();
    auto pGet = [](auto* p, Entity e) { return p ? p->Get(e) : nullptr; };

    // Gameplay billboards only. Light icons are drawn by the editor-only
    // DebugIconPass (driven by DebugDrawSystem), off the gameplay draw path;
    // see editor_debug_draw_system.md.
    if (m_meshMgr.GetBillboardMeshDescSlot() != RHI::kInvalidBufferIndex && pBillboard)
    {
        XMVECTOR camPos = XMLoadFloat3(&FrameViewWrite().view.cameraPosition);

        auto& bbEnts = pBillboard->Entities();
        auto& bbData = pBillboard->Data();
        const size_t bbN = bbData.size();
        for (size_t bi = 0; bi < bbN; ++bi)
        {
            const Entity e = bbEnts[bi];
            if (!world.IsAlive(e)) continue;
            const auto* bb = &bbData[bi];
            const GlobalTransform* gt = pGet(pGlobalXf, e);
            if (!gt) continue;

            // A BillboardComponent on a light is not rendered: the light's icon comes
            // from DebugIconPass, so drawing it here would double-draw.
            if (pGet(pLightData, e)) continue;

            XMFLOAT3 pos = { gt->matrix._41, gt->matrix._42, gt->matrix._43 };
            float size = bb->worldSize;

            // Camera world axes = view matrix columns (orthogonal: inv = transpose).
            float crx = FrameViewWrite().view.viewMatrix._11, cry = FrameViewWrite().view.viewMatrix._21, crz = FrameViewWrite().view.viewMatrix._31;
            float cux = FrameViewWrite().view.viewMatrix._12, cuy = FrameViewWrite().view.viewMatrix._22, cuz = FrameViewWrite().view.viewMatrix._32;
            float cfx = FrameViewWrite().view.viewMatrix._13, cfy = FrameViewWrite().view.viewMatrix._23, cfz = FrameViewWrite().view.viewMatrix._33;

            XMFLOAT4X4 billboardWorld;
            XMStoreFloat4x4(&billboardWorld, XMMatrixIdentity());
            // Row 0 = camera right x size
            billboardWorld._11 = crx * size;
            billboardWorld._12 = cry * size;
            billboardWorld._13 = crz * size;
            // Row 1 = camera up x size
            billboardWorld._21 = cux * size;
            billboardWorld._22 = cuy * size;
            billboardWorld._23 = cuz * size;
            // Row 2 = camera forward
            billboardWorld._31 = cfx;
            billboardWorld._32 = cfy;
            billboardWorld._33 = cfz;
            // Row 3 = position
            billboardWorld._41 = pos.x;
            billboardWorld._42 = pos.y;
            billboardWorld._43 = pos.z;

            DrawCandidate c;
            c.entity       = e;
            c.meshDescSlot = m_meshMgr.GetBillboardMeshDescSlot();
            c.indexCount   = m_meshMgr.GetBillboardQuad().indexCount;

            // Transpose for GPU (row-vector mul); pivot==centroid for billboard depth sort.
            XMStoreFloat4x4(&c.worldMatrix,
                XMMatrixTranspose(XMLoadFloat4x4(&billboardWorld)));
            c.worldCenter = { pos.x, pos.y, pos.z };

            c.mc = pGet(pMaterial, e);
            if (c.mc)
            {
                const ResolvedMaterialRuntime rt = ResolveMaterialRuntime(e);
                c.texBaseColor = rt.EngineTex(MaterialComponent::BASECOLORMAP).gpuHandle;
                c.matHash      = HashMatKeys(c.mc, rt, c.orderKey);
            }
            // Route by BillboardMode
            PermutationKey perm{};
            perm.Set(PermutationKey::BILLBOARD, true);

            switch (bb->mode)
            {
            case BillboardMode::Opaque:
                c.filter = DrawFilter::Opaque;
                break;
            case BillboardMode::AlphaClip:
                c.filter = DrawFilter::Opaque;
                perm.Set(PermutationKey::ALPHA_TEST, true);
                break;
            case BillboardMode::Transparent:
                c.filter = DrawFilter::Transparent;
                perm.Set(PermutationKey::ALPHA_BLEND, true);
                break;
            case BillboardMode::Additive:
                c.filter = DrawFilter::Transparent;
                perm.Set(PermutationKey::ADDITIVE_BLEND, true);
                break;
            }
            c.perm = perm;

            // View-space Z along this frame's camForward (m_view is a frame stale
            // during the scene build).
            XMVECTOR ePos    = XMLoadFloat3(&pos);
            XMVECTOR forward = XMLoadFloat3(&camForwardThisFrame);
            c.depth = XMVectorGetX(XMVector3Dot(XMVectorSubtract(ePos, camPos), forward));

            candidates.push_back(c);
        }
    }

}

// Bindless index == handleId: the SRV is published into the engine's t0
// space2 table at slot [handleId].
uint32_t Renderer::GetDebugIconBindless(const char* path)
{
    if (!path || !*path || !m_texSys || !m_resMgr) return 0xFFFFFFFFu;

    auto it = m_debugIconTex.find(path);
    if (it == m_debugIconTex.end())
    {
        // One Acquire per unique path, cached even on failure (no per-frame reload
        // spam for a missing file; a restart picks up a new texture).
        const Resource::TextureHandle h = m_texSys->Acquire(path, *m_resMgr, m_gfx);
        it = m_debugIconTex.emplace(std::string(path), h).first;
    }

    const Resource::TextureHandle h = it->second;
    if (h != Resource::kInvalidTextureHandle && m_texSys->IsReady(h))
    {
        if (const RHI::Texture* tex = m_texSys->GetTexture(h))
            return static_cast<uint32_t>(tex->handleId);
    }
    return 0xFFFFFFFFu;
}

// CreateTexture publishes the SRV into the bindless t0 space2 table at
// slot [handleId], so the index is directly a DebugIconPass texIdx (no
// .itex asset).
uint32_t Renderer::RegisterGlyphIcon(uint32_t codepoint, const void* rgba,
                                     uint32_t w, uint32_t h)
{
    if (!rgba || w == 0 || h == 0) return 0xFFFFFFFFu;
    if (auto it = m_glyphIconTex.find(codepoint); it != m_glyphIconTex.end())
        return it->second.texIdx;

    RHI::TextureDesc td{};
    td.type       = RHI::TextureDesc::Type::TEXTURE_2D;
    td.width      = w;
    td.height     = h;
    td.arraySize = 1;
    td.mipLevels = 1;
    td.format     = RHI::Format::R8G8B8A8_UNORM;
    td.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
    RHI::SubresourceData sub{ rgba, w * 4u, w * 4u * h };

    GlyphIconTex gi;
    if (!m_gfx.CreateTexture(td, gi.tex, &sub))
    {
        LOG_ERROR("Renderer: glyph icon texture creation failed (cp=0x%X)", codepoint);
        return 0xFFFFFFFFu;
    }
    gi.texIdx = static_cast<uint32_t>(gi.tex.handleId);
    return m_glyphIconTex.emplace(codepoint, std::move(gi)).first->second.texIdx;
}

uint32_t Renderer::GetGlyphIconBindless(uint32_t codepoint) const
{
    auto it = m_glyphIconTex.find(codepoint);
    return it != m_glyphIconTex.end() ? it->second.texIdx : 0xFFFFFFFFu;
}

void Renderer::BuildScene_GatherBeams(World& world,
        std::vector<DrawCandidate>& candidates, const XMFLOAT3& camForwardThisFrame)
{
    auto* pGlobalXf = world.GetPool<GlobalTransform>();
    auto* pMaterial = world.GetPool<MaterialComponent>();
    auto pGet = [](auto* p, Entity e) { return p ? p->Get(e) : nullptr; };
    if (m_beamSystem)
    {
        auto* pBeam = world.GetPool<BeamComponent>();
        if (pBeam)
        {
            auto& bEnts = pBeam->Entities();
            auto& bData = pBeam->Data();
            const size_t bN = bData.size();
            for (size_t bi = 0; bi < bN; ++bi)
            {
                const Entity e = bEnts[bi];
                if (!world.IsAlive(e)) continue;
                BeamComponent& beam = bData[bi];
                if (beam.controlPoints.size() < 2) continue;

                const GlobalTransform*   gt = pGet(pGlobalXf, e);
                const MaterialComponent* mc = pGet(pMaterial, e);
                if (!gt || !mc) continue;

                // Lazy slot acquisition. Released in OnEntityDestroyed.
                if (beam.beamSlot == 0xFFFFFFFFu)
                {
                    beam.beamSlot = m_beamSystem->Acquire();
                    if (beam.beamSlot == 0xFFFFFFFFu) continue; // pool exhausted
                }

                // BeamControlPoint and BeamControlPointGPU share the 32-byte layout: a
                // direct reinterpret.
                static_assert(sizeof(BeamControlPoint) == sizeof(BeamControlPointGPU),
                              "BeamControlPoint vs GPU layout drift");
                m_beamSystem->SetControlPoints(
                    beam.beamSlot,
                    reinterpret_cast<const BeamControlPointGPU*>(beam.controlPoints.data()),
                    static_cast<uint32_t>(beam.controlPoints.size()));

                BeamGenParamsGPU params{};
                params.globalRadiusScale = beam.globalRadiusScale;
                params.wobbleAmplitude   = beam.wobbleAmplitude;
                params.wobbleSpeed       = beam.wobbleSpeed;
                m_beamSystem->SetParams(beam.beamSlot, params);

                const uint32_t meshDescSlot = m_beamSystem->GetMeshDescSlot(beam.beamSlot);
                if (meshDescSlot == 0xFFFFFFFFu) continue;

                DrawCandidate c;
                c.entity       = e;
                c.meshDescSlot = meshDescSlot;
                c.indexCount   = BeamSystem::GetIndexCount();

                // World matrix, transposed for the HLSL row-vector convention.
                XMStoreFloat4x4(&c.worldMatrix,
                    XMMatrixTranspose(XMLoadFloat4x4(&gt->matrix)));

                // Centroid: avg control-point positions for transparent depth sort.
                XMFLOAT3 centroid{ 0, 0, 0 };
                for (const auto& cp : beam.controlPoints)
                {
                    centroid.x += cp.position.x;
                    centroid.y += cp.position.y;
                    centroid.z += cp.position.z;
                }
                const float invN = 1.0f / static_cast<float>(beam.controlPoints.size());
                centroid.x *= invN; centroid.y *= invN; centroid.z *= invN;
                c.worldCenter = centroid;

                c.mc            = mc;
                const ResolvedMaterialRuntime rt = ResolveMaterialRuntime(e);
                c.texBaseColor  = rt.EngineTex(MaterialComponent::BASECOLORMAP).gpuHandle;
                c.texSurfaceMap = rt.EngineTex(MaterialComponent::SURFACEMAP).gpuHandle;
                c.texNormalMap  = rt.EngineTex(MaterialComponent::NORMALMAP).gpuHandle;

                ApplyBlendMode(c, camForwardThisFrame);
                c.customPSID = ResolveCustomPSID(e, *mc);
                c.customVSID = ResolveCustomVSID(e, *mc);
                c.matHash    = HashMatKeys(mc, rt, c.orderKey);

                // Depth (view-space Z) for transparent painter's-algorithm sort.
                {
                    const float dx = c.worldCenter.x - FrameViewWrite().camera.position.x;
                    const float dy = c.worldCenter.y - FrameViewWrite().camera.position.y;
                    const float dz = c.worldCenter.z - FrameViewWrite().camera.position.z;
                    c.depth = dx * camForwardThisFrame.x
                            + dy * camForwardThisFrame.y
                            + dz * camForwardThisFrame.z;
                }

                candidates.push_back(c);
            }
        }
    }
}

void Renderer::BuildScene_GatherAfterimages(
        std::vector<DrawCandidate>& candidates, const XMFLOAT3& camForwardThisFrame)
{
    if (m_afterimageSystem && m_afterimageSystem->IsInitialised())
    {
        auto& slots = m_afterimageSystem->GetSlotsMutable();
        for (uint32_t ai = 0; ai < AfterimageSystem::kMaxSnapshots; ++ai)
        {
            auto& slot = slots[ai];
            if (!slot.alive) continue;
            if (slot.vertexCount == 0 || slot.indexCount == 0) continue;
            if (slot.meshDescSlot == 0xFFFFFFFFu)              continue;

            DrawCandidate c;
            c.entity       = NullEntity; // no ECS backing: picking ignores it
            c.meshDescSlot = slot.meshDescSlot;
            c.indexCount   = slot.indexCount;

            // World matrix, transposed for the HLSL row-vector convention (as in the
            // beam path).
            XMStoreFloat4x4(&c.worldMatrix,
                XMMatrixTranspose(XMLoadFloat4x4(&slot.worldMatrix)));
            c.worldCenter = slot.worldCenter;

            c.mc = &slot.material;
            // No textures: the ghost PS is purely procedural.
            c.texBaseColor  = 0;
            c.texSurfaceMap = 0;
            c.texNormalMap  = 0;

            ApplyBlendMode(c, camForwardThisFrame);
            c.customPSID = ResolveCustomPSID(NullEntity, slot.material);
            c.customVSID = ResolveCustomVSID(NullEntity, slot.material);
            // Standalone ghost material: no entity-bound runtime textures.
            c.matHash    = HashMatKeys(&slot.material, ResolvedMaterialRuntime{}, c.orderKey);

            // View-space Z for transparent painter sort.
            {
                const float dx = c.worldCenter.x - FrameViewWrite().camera.position.x;
                const float dy = c.worldCenter.y - FrameViewWrite().camera.position.y;
                const float dz = c.worldCenter.z - FrameViewWrite().camera.position.z;
                c.depth = dx * camForwardThisFrame.x
                        + dy * camForwardThisFrame.y
                        + dz * camForwardThisFrame.z;
            }

            candidates.push_back(c);
        }
    }
}
