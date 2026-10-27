// Renderer_SceneSets.cpp - the per-frame entity sets BuildRenderScene reads:
// the scene dirty diff (BuildScene_DirtyDiff, ClassifySceneAdds), the tag-set
// boundary check, and the player shadow, hair shadow, face overlay and dissolve
// sets.

#include "Renderer/Renderer.h"

// engine graphics / backend
#include "Graphics/GraphicsDX12.h"
#include "System/HostPolicy.h"   // Sys::IsEditorHost (EditorOnlyTag culling)
#include "Graphics/AfterimageSystem.h"
#include "Graphics/WaterRippleSystem.h"

// ECS components + systems
#include "ECS/PlayerComponent.h"                // per-object (player) shadow resolve
#include "ECS/PerObjectShadowComponent.h"       // explicit per-object shadow opt-in tag
#include "ECS/HairShadowCasterComponent.h"      // hair (bangs -> face) shadow opt-in tag
#include "ECS/FaceOverlayComponent.h"           // brows/lashes-through-hair opt-in tag
#include "ECS/DissolveComponent.h"              // per-entity noise dissolve tag

// scene
#include "Scene/MeshSpawner.h"

// engine systems
#include "System/JobDefaults.h"
#include "System/CommandLine.h"
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

namespace
{
    constexpr float kFaceOverlayMinDistance = 0.005f;
    constexpr float kFaceOverlayMaxDistance = 0.5f;

    // Hair-shadow tuning distances are authored in metres for a scale-1
    // character. Convert them by the tagged root's world scale so shrinking a
    // character also shrinks its screen-mark offsets, map padding, and bias.
    // The arithmetic mean is exact for uniform scale and remains predictable
    // for non-uniform authoring transforms; row lengths discard reflection sign.
    float ExtractUniformWorldScale(const XMFLOAT4X4& m)
    {
        const float sx = std::sqrt(m._11 * m._11 + m._12 * m._12 + m._13 * m._13);
        const float sy = std::sqrt(m._21 * m._21 + m._22 * m._22 + m._23 * m._23);
        const float sz = std::sqrt(m._31 * m._31 + m._32 * m._32 + m._33 * m._33);
        const float scale = (sx + sy + sz) / 3.0f;
        return std::isfinite(scale) ? std::clamp(scale, 0.0f, 1.0e4f) : 1.0f;
    }

    // The shared graphics root signature already reserves a fourth DWORD at
    // b0. FaceOverlay does not use the usual prevPosInfo value, so pack its two
    // per-draw settings into that DWORD without adding another GPU resource.
    uint32_t PackFaceOverlayParams(const FaceOverlayComponent& component)
    {
        const float alpha = std::clamp(component.alpha, 0.0f, 1.0f);
        const float distance = std::clamp(component.maxHairDistance,
                                          kFaceOverlayMinDistance,
                                          kFaceOverlayMaxDistance);
        const float distance01 = (distance - kFaceOverlayMinDistance) /
                                 (kFaceOverlayMaxDistance - kFaceOverlayMinDistance);

        const uint32_t alpha16 = static_cast<uint32_t>(std::lround(alpha * 65535.0f));
        const uint32_t distance16 = static_cast<uint32_t>(std::lround(distance01 * 65535.0f));
        return alpha16 | (distance16 << 16u);
    }
}

// ==== Retained-scene dirty diff ====
// Runs first in BuildRenderScene, at the read point every gather uses (after
// TransformSystem propagate, physics interpolation and follow), so the
// snapshotted keys are what this frame's consumers see. Large pools compare
// disjoint rows in parallel; list publication keeps dense/pool order, including
// entities that carry both supported mesh types.
void Renderer::BuildScene_DirtyDiff(World& world)
{
    ZoneScopedN("BS.DirtyDiff");

    // Last frame's material-dirty list (filled by BuildScene_SyncMaterials,
    // which runs after us) rolls into telemetry before this frame's clear.
    m_sceneDiffPeakMat = (std::max)(m_sceneDiffPeakMat,
                                    static_cast<uint32_t>(m_sceneMatDirty.size()));
    m_sceneMatDirty.clear();

    m_sceneDirty.clear();
    m_sceneDirtyXform.clear();
    m_sceneDirtyOther.clear();
    m_sceneAdded.clear();
    m_sceneRemoved.clear();

    if (!m_retainedSceneDiff) return;

    SceneItemStore& st = m_sceneStore;
    st.frameStamp++;

    auto* pGT   = world.GetPool<GlobalTransform>();
    auto* pVis  = world.GetPool<VisibilityComponent>();
    auto* pLib  = world.GetPool<MeshLibRef>();
    auto* pPrim = world.GetPool<MeshHandle>();
    auto* pSkin = world.GetPool<MeshSkinnedComponent>();
    auto* pDis  = world.GetPool<DissolveComponent>();
    auto* pDisabled     = world.GetPool<DisabledTag>();
    auto* pEditorOnly   = world.GetPool<EditorOnlyTag>();
    auto* pHiddenInGame = world.GetPool<HiddenInGameTag>();
    if (!pGT) return;

    const bool full = st.fullInvalidate;
    st.fullInvalidate = false;

    // Destroy-listener accelerator: those entities are already untracked (the
    // listener cleared the flag); surface them in this frame's removed list.
    for (Entity e : m_sceneRemovedEvents) m_sceneRemoved.push_back(e);
    m_sceneRemovedEvents.clear();

    auto ensure = [&st](uint32_t id)
    {
        if (id >= st.keys.size())
        {
            const size_t grown = (std::max)(static_cast<size_t>(id) + 1,
                                            st.keys.size() * 2);
            st.keys.resize(grown);
            st.seenStamp.resize(grown, 0);
            st.tracked.resize(grown, 0);
        }
    };

    // Grow before dispatch: each pool has unique entity IDs. Pools run in
    // their original order so an entity carrying both mesh types is visited
    // twice in exactly the same sequence as the serial path.
    bool parallelDiff = TaskSystem::Get().GetWorkerCount() >= 2;
    bool forceDiffJobs = Sys::JobSplittingDefaultsEnabled(), verifyDiff = false;
#if WITH_HARNESS
    const auto& diffCmd = Sys::CommandLine::Get();
    parallelDiff = parallelDiff && !diffCmd.Has("-dirtydiffserial");
    forceDiffJobs = diffCmd.Has("-dirtydiffjobs") || Sys::JobSplittingDefaultsEnabled();
    verifyDiff = diffCmd.Has("-dirtydiffverify");
    const int alternate = diffCmd.GetInt("-dirtydiffab", 0);
    static uint64_t s_abFrame = 0;
    if (alternate != 0) parallelDiff = parallelDiff && ((++s_abFrame + alternate) % 2 == 0);
#endif
#ifdef TRACY_ENABLE
    ZoneTransientN(diffModeZone, parallelDiff ? "BS.DirtyDiff.Parallel" : "BS.DirtyDiff.Serial", true);
#endif
    // Result bits: changed, transform-only, newly tracked. Workers mutate
    // disjoint store rows; list publication and trackedCount remain serial.
    auto visit = [&](Entity e, uint64_t meshKey) -> uint8_t
    {
        const GlobalTransform* gt = pGT->Get(e);
        if (!gt) return 0;
        const uint32_t id = static_cast<uint32_t>(e);
        ensure(id); // parallel dispatch pre-grows to every entity in this pool

        SceneItemKey k{};
        k.world   = gt->matrix;
        k.meshKey = meshKey;
        if (const VisibilityComponent* v = pVis ? pVis->Get(e) : nullptr)
        {
            k.viewMask        = v->viewMask;
            k.visFlags        = v->flags;
            k.inheritedHidden = v->inheritedHidden ? 1u : 0u;
            k.renderLayer     = v->renderLayer;
        }
        else
        {
            k.viewMask = ViewBit::All;
            k.visFlags = VisibilityComponent::Visible |
                         VisibilityComponent::CastShadow |
                         VisibilityComponent::RenderInMainPass;
        }
        k.lane = ((pSkin && pSkin->Get(e)) || (pDis && pDis->Get(e))) ? 1u : 0u;
        if (pDisabled     && pDisabled->Get(e))     k.tagBits |= 1u;
        if (pEditorOnly   && pEditorOnly->Get(e))   k.tagBits |= 2u;
        if (pHiddenInGame && pHiddenInGame->Get(e)) k.tagBits |= 4u;

        st.seenStamp[id] = st.frameStamp;
        const bool wasTracked = st.tracked[id] != 0;
        if (full || !wasTracked ||
            std::memcmp(&st.keys[id], &k, sizeof(SceneItemKey)) != 0)
        {
            uint8_t result = 1;
            // Classify: world-matrix-only changes are the high-frequency case
            // and retained consumers patch them in place; anything else
            // (mesh identity / visibility / tags) can change membership and
            // forces those consumers to re-gather.
            if (wasTracked && !full)
            {
                constexpr size_t kRest = sizeof(SceneItemKey) - sizeof(DirectX::XMFLOAT4X4);
                const bool restSame =
                    std::memcmp(reinterpret_cast<const uint8_t*>(&st.keys[id]) + sizeof(DirectX::XMFLOAT4X4),
                                reinterpret_cast<const uint8_t*>(&k) + sizeof(DirectX::XMFLOAT4X4),
                                kRest) == 0;
                if (restSame) result |= 2;
            }
            st.keys[id] = k;
            if (!wasTracked)
            {
                st.tracked[id] = 1;
                result |= 4;
            }
            return result;
        }
        return 0;
    };
    auto publish = [&](Entity e, uint8_t result) {
        if (!(result & 1)) return;
        ((result & 2) ? m_sceneDirtyXform : m_sceneDirtyOther).push_back(e);
        m_sceneDirty.push_back(e);
        if (result & 4) { ++st.trackedCount; m_sceneAdded.push_back(e); }
    };
    auto processPool = [&](const auto* pool, auto meshKey) {
        if (!pool) return;
        const auto& entities = pool->Entities();
        const auto& data = pool->Data();
        const bool parallel = parallelDiff && (forceDiffJobs || entities.size() >= 2048);
        auto visitRow = [&](size_t i) -> uint8_t {
            return data[i].IsValid() ? visit(entities[i], meshKey(data[i])) : 0;
        };
        if (!parallel) {
            for (size_t i = 0; i < entities.size(); ++i) publish(entities[i], visitRow(i));
            return;
        }
        uint32_t maxEntity = 0;
        for (Entity e : entities) maxEntity = (std::max)(maxEntity, static_cast<uint32_t>(e));
        if (!entities.empty()) ensure(maxEntity);
        thread_local std::vector<uint8_t> t_resultScratch;
        t_resultScratch.resize(entities.size()); // retain size/capacity; every row overwritten
        uint8_t* const results = t_resultScratch.data(); // bind caller TLS before dispatch
        const uint32_t lanes = (std::min)(4u, TaskSystem::Get().GetWorkerCount());
        auto runJobs = [&] {
            ZoneScopedN("BS.DirtyDiff.Work");
            TaskSystem::Get().ParallelFor(0, lanes, [&](uint32_t lane) {
                ZoneScopedN("BS.DirtyDiff.Chunk");
                const size_t begin = entities.size() * lane / lanes;
                const size_t end = entities.size() * (lane + 1) / lanes;
                ZoneValue(end - begin);
                for (size_t i = begin; i < end; ++i) results[i] = visitRow(i);
            });
        };
        if (verifyDiff) {
            ZoneScopedN("BS.DirtyDiff.Oracle");
            const auto keysBefore = st.keys;
            const auto seenBefore = st.seenStamp;
            const auto trackedBefore = st.tracked;
            std::vector<uint8_t> reference(entities.size());
            for (size_t i = 0; i < entities.size(); ++i) reference[i] = visitRow(i);
            const auto keysReference = st.keys;
            const auto seenReference = st.seenStamp;
            const auto trackedReference = st.tracked;
            st.keys = keysBefore; st.seenStamp = seenBefore; st.tracked = trackedBefore;
            runJobs();
            const bool matches = reference == t_resultScratch && st.seenStamp == seenReference &&
                st.tracked == trackedReference && (st.keys.empty() ||
                std::memcmp(st.keys.data(), keysReference.data(), st.keys.size() * sizeof(SceneItemKey)) == 0);
            ENGINE_CHECK(matches, "dirty diff parallel oracle mismatch");
            LOG_INFO("[DIRTY_DIFF_ORACLE] rows=%zu lanes=%u match=%u", entities.size(), lanes, matches ? 1u : 0u);
        }
        else runJobs();
        // Preserve pool/dense order and duplicate cross-pool visits exactly.
        ZoneScopedN("BS.DirtyDiff.Publish");
        for (size_t i = 0; i < entities.size(); ++i) publish(entities[i], results[i]);
    };
    processPool(pLib, [](const MeshLibRef& r) { return (uint64_t(r.libHandle.Index()) << 32) | uint64_t(r.meshId); });
    processPool(pPrim, [](const MeshHandle& r) { return 0x8000000000000000ull | uint64_t(r.gpuMeshID); });

    // Membership sweep: tracked but not seen this frame = component removed or
    // entity destroyed without (or before) the listener. O(maxEntityId) over a
    // dense byte + word array: microseconds at 16k ids.
    const uint32_t n = static_cast<uint32_t>(st.tracked.size());
    for (uint32_t id = 0; id < n; ++id)
    {
        if (st.tracked[id] && st.seenStamp[id] != st.frameStamp)
        {
            st.tracked[id] = 0;
            st.trackedCount--;
            m_sceneRemoved.push_back(static_cast<Entity>(id));
        }
    }

    // Telemetry: rolling peaks + a throttled log line every 300 frames.
    m_sceneDiffPeakDirty  = (std::max)(m_sceneDiffPeakDirty,
                                       static_cast<uint32_t>(m_sceneDirty.size()));
    m_sceneDiffPeakAdd    = (std::max)(m_sceneDiffPeakAdd,
                                       static_cast<uint32_t>(m_sceneAdded.size()));
    m_sceneDiffPeakRemove = (std::max)(m_sceneDiffPeakRemove,
                                       static_cast<uint32_t>(m_sceneRemoved.size()));
    static uint32_t s_diffLogTick = 0;
    if ((++s_diffLogTick % 300u) == 0)
    {
        LOG_DIAG(ENGINE_LOG_SCENE_DIFF,
                 "BS.DirtyDiff: tracked=%u dirty=%zu(pk %u) add=%zu(pk %u) "
                 "rem=%zu(pk %u) matDirty pk=%u%s",
                 st.trackedCount, m_sceneDirty.size(), m_sceneDiffPeakDirty,
                 m_sceneAdded.size(), m_sceneDiffPeakAdd,
                 m_sceneRemoved.size(), m_sceneDiffPeakRemove,
                 m_sceneDiffPeakMat, full ? " [FULL]" : "");
        m_sceneDiffPeakDirty = m_sceneDiffPeakAdd =
        m_sceneDiffPeakRemove = m_sceneDiffPeakMat = 0;
    }
}

// Classifies this frame's adds (see the declaration); runs right after
// BuildScene_DirtyDiff.
void Renderer::ClassifySceneAdds(World& world)
{
    static const bool s_off = Sys::CommandLine::Get().Has("-gather_noaddpath");
    m_addsSorted.clear();
    m_addsDynamicOnly = m_addsSkinnedOnly = m_addsOtherOwn = m_addsMatOwn = false;
    if (s_off || !m_retainedSceneDiff || m_sceneAdded.empty() || !m_sceneRemoved.empty()) return;

    const auto* pSkin = world.GetPool<MeshSkinnedComponent>();
    bool skinned = true;
    for (Entity e : m_sceneAdded)
    {
        const uint32_t id = static_cast<uint32_t>(e);
        if (id >= m_sceneStore.keys.size() || m_sceneStore.keys[id].lane != 1u) return;   // a static add
        if (!(pSkin && pSkin->Get(e))) skinned = false;
    }
    m_addsSorted = m_sceneAdded;
    std::sort(m_addsSorted.begin(), m_addsSorted.end());
    m_addsDynamicOnly = true;
    m_addsSkinnedOnly = skinned;
    m_addsOtherOwn = std::all_of(m_sceneDirtyOther.begin(), m_sceneDirtyOther.end(), [this](Entity e) {
        return std::binary_search(m_addsSorted.begin(), m_addsSorted.end(), e);
    });
}

// The matDirty half, after SyncMaterials filled the list.
void Renderer::ClassifySceneAddsMaterials()
{
    m_addsMatOwn = m_addsDynamicOnly &&
        std::all_of(m_sceneMatDirty.begin(), m_sceneMatDirty.end(), [this](Entity e) {
            return std::binary_search(m_addsSorted.begin(), m_addsSorted.end(), e);
        });
}

// ==== Retained tag sets ====
// Tag-set seed mixer (the ComputeCandidateSetSig finalizer). Seeds are summed
// commutatively so pool iteration order cannot flap the signature.
static uint64_t TagSeedMix(uint64_t v)
{
    v *= 0xFF51AFD7ED558CCDull; v ^= v >> 33;
    v *= 0xC4CEB9FE1A85EC53ull; v ^= v >> 33;
    return v;
}

// Checks that no dirtyXform entity crossed a retained tag-set boundary (see the
// declaration), with the flat epoch-stamped ancestor memo. Features that are off
// are skipped: their sets are empty by construction, and checking them would fail
// the shared gate every frame the player moves. -tagboundaryref runs only
// BuildScene_TagSetBoundaryStableRef; -tagboundaryverify runs both and counts
// disagreements.
bool Renderer::BuildScene_TagSetBoundaryStable(World& world) const
{
    if (m_sceneDirtyXform.empty()) return true;
    static const bool s_verify = Sys::CommandLine::Get().Has("-tagboundaryverify");
    static const bool s_ref    = Sys::CommandLine::Get().Has("-tagboundaryref");
    if (s_ref) return BuildScene_TagSetBoundaryStableRef(world);
    ZoneScopedN("BS.TagSetBoundary");
    auto* pParent = world.GetPool<Parent>();
    auto* pPlayer = world.GetPool<PlayerComponent>();
    auto* pPObj   = world.GetPool<PerObjectShadowComponent>();
    auto* pHair   = world.GetPool<HairShadowCasterComponent>();
    auto* pFace   = world.GetPool<FaceOverlayComponent>();

    if (++m_tagBoundaryEpoch == 0)   // wrapped: every stamp is stale again
    {
        std::fill(m_tagBoundaryStamp.begin(), m_tagBoundaryStamp.end(), 0u);
        m_tagBoundaryEpoch = 1;
    }
    const uint32_t epoch = m_tagBoundaryEpoch;
    auto& stamp = m_tagBoundaryStamp;
    auto& ancs  = m_tagBoundaryAnc;
    auto& chain = m_tagBoundaryChain;
    auto resolve = [&](Entity e) -> TagBoundaryAnc
    {
        chain.clear();
        TagBoundaryAnc base{};
        Entity cur = e;
        for (int depth = 0; cur != NullEntity && depth < 512; ++depth)
        {
            const size_t id = static_cast<size_t>(cur);
            if (id < stamp.size() && stamp[id] == epoch) { base = ancs[id]; break; }
            chain.push_back(cur);
            const Parent* par = pParent ? pParent->Get(cur) : nullptr;
            cur = (par && par->entity != NullEntity) ? par->entity : NullEntity;
        }
        for (size_t i = chain.size(); i-- > 0; )
        {
            const Entity n = chain[i];
            if ((pPObj && pPObj->Get(n)) || (pPlayer && pPlayer->Get(n)))
                base.player = 1;
            if (pHair && pHair->Get(n)) base.hair = 1;
            if (const FaceOverlayComponent* fc = pFace ? pFace->Get(n) : nullptr)
            {
                base.face       = 1;
                base.faceParams = PackFaceOverlayParams(*fc);
            }
            const size_t id = static_cast<size_t>(n);
            if (id >= stamp.size())
            {
                stamp.resize(id + 1, 0u);
                ancs.resize(id + 1);
            }
            // emplace semantics of the reference: the FIRST write of a node
            // within one call wins (a chain never revisits a stamped node).
            if (stamp[id] != epoch) { stamp[id] = epoch; ancs[id] = base; }
        }
        return base;
    };

    bool stable = true;
    for (Entity e : m_sceneDirtyXform)
    {
        const TagBoundaryAnc a = resolve(e);
        if (m_perObjectShadowEnabled &&
            (m_playerShadowSet.count(e) != 0) != (a.player != 0))
        { stable = false; break; }
        if (m_hairShadowEnabled &&
            (m_hairShadowSet.count(e) != 0) != (a.hair != 0))
        { stable = false; break; }
        const auto it = m_faceOverlayParams.find(e);
        if ((it != m_faceOverlayParams.end()) != (a.face != 0)) { stable = false; break; }
        if (a.face && it->second != a.faceParams) { stable = false; break; }
    }
    if (s_verify)
    {
        ++m_tagBoundaryChecks;
        if (BuildScene_TagSetBoundaryStableRef(world) != stable) ++m_tagBoundaryMismatches;
        if (m_tagBoundaryChecks % 120 == 0)
            LOG_INFO("[TAGBOUNDARY] checks=%llu mismatches=%llu dirty=%zu",
                     static_cast<unsigned long long>(m_tagBoundaryChecks),
                     static_cast<unsigned long long>(m_tagBoundaryMismatches),
                     m_sceneDirtyXform.size());
    }
    return stable;
}

bool Renderer::BuildScene_TagSetBoundaryStableRef(World& world) const
{
    if (m_sceneDirtyXform.empty()) return true;
    ZoneScopedN("BS.TagSetBoundary");
    auto* pParent = world.GetPool<Parent>();
    auto* pPlayer = world.GetPool<PlayerComponent>();
    auto* pPObj   = world.GetPool<PerObjectShadowComponent>();
    auto* pHair   = world.GetPool<HairShadowCasterComponent>();
    auto* pFace   = world.GetPool<FaceOverlayComponent>();

    // Ancestor-or-self tag resolution, memoized so a dragged subtree resolves
    // each unique chain node once (siblings share every ancestor).
    struct Anc { uint8_t player = 0, hair = 0, face = 0; uint32_t faceParams = 0; };
    std::unordered_map<Entity, Anc> memo;
    std::vector<Entity> chain;
    auto resolve = [&](Entity e) -> Anc
    {
        chain.clear();
        Anc base{};
        Entity cur = e;
        // Depth cap breaks hierarchy cycles (invalid data; the BFS builders
        // carry the same guard).
        for (int depth = 0; cur != NullEntity && depth < 512; ++depth)
        {
            auto it = memo.find(cur);
            if (it != memo.end()) { base = it->second; break; }
            chain.push_back(cur);
            const Parent* par = pParent ? pParent->Get(cur) : nullptr;
            cur = (par && par->entity != NullEntity) ? par->entity : NullEntity;
        }
        // Fill root-downward: at each node the inherited state is overridden
        // by its own tags; face params resolve to the NEAREST tagged
        // ancestor-or-self, matching the BFS inheritance exactly.
        for (size_t i = chain.size(); i-- > 0; )
        {
            const Entity n = chain[i];
            if ((pPObj && pPObj->Get(n)) || (pPlayer && pPlayer->Get(n)))
                base.player = 1;
            if (pHair && pHair->Get(n)) base.hair = 1;
            if (const FaceOverlayComponent* fc = pFace ? pFace->Get(n) : nullptr)
            {
                base.face       = 1;
                base.faceParams = PackFaceOverlayParams(*fc);
            }
            memo.emplace(n, base);
        }
        return base;
    };

    for (Entity e : m_sceneDirtyXform)
    {
        const Anc a = resolve(e);
        if (m_perObjectShadowEnabled &&
            (m_playerShadowSet.count(e) != 0) != (a.player != 0))
            return false;
        if (m_hairShadowEnabled &&
            (m_hairShadowSet.count(e) != 0) != (a.hair != 0))
            return false;
        const auto it = m_faceOverlayParams.find(e);
        if ((it != m_faceOverlayParams.end()) != (a.face != 0)) return false;
        if (a.face && it->second != a.faceParams) return false;
    }
    return true;
}

// ==== Per-object shadow sets ====
// Resolves the player subtree (PerObjectShadowComponent / PlayerComponent roots +
// descendants) into m_playerShadowSet and merges their world AABBs.
// PerObjectShadowPass renders only the tagged packets; UploadFrameData fits the
// reversed-Z ortho VP to the merged AABB. Runs before the gather (so candidates
// can be tagged) and is read-only during the parallel MeshLibRef gather.
// The BFS membership is retained: it reruns only when the seed signature (root set
// + master toggle; the tag pools live outside SceneItemKey) changes or the shared
// membership gate failed. The merged AABB is recomputed every frame (it tracks
// skinned animation).
void Renderer::BuildScene_ComputePlayerShadowSet(World& world)
{
    ZoneScopedN("BS.PlayerShadowSet");
    m_hasPlayerShadowCaster = false;

    auto* pPlayer = world.GetPool<PlayerComponent>();
    auto* pTag    = world.GetPool<PerObjectShadowComponent>();

    // Seed roots: explicit opt-in tags (editor: Add Component -> "Per-Object
    // Shadow") plus the player avatar if one exists. Each root's whole Children
    // subtree is rendered into the high-res map. The tag is the primary path:
    // without it a project with no PlayerComponent could never use this feature.
    std::vector<Entity> roots;
    if (pTag)    for (Entity e : pTag->Entities())    roots.push_back(e);
    if (pPlayer) for (Entity e : pPlayer->Entities()) roots.push_back(e);
    const size_t nRoots = roots.size();

    // Diagnostic: logs [PerObjShadow] only when the root count, set size or AABB
    // presence changes.
    static int s_dbgRoots = -1, s_dbgSet = -1, s_dbgHas = -1;
    auto dbgLog = [&]() {
        const int nr = (int)nRoots, ns = (int)m_playerShadowSet.size(),
                  nh = m_hasPlayerShadowCaster ? 1 : 0;
        if (nr != s_dbgRoots || ns != s_dbgSet || nh != s_dbgHas) {
            LOG_INFO("[PerObjShadow] enabled=%d roots=%d setSize=%d hasAabb=%d aabb=(%.2f,%.2f,%.2f)-(%.2f,%.2f,%.2f)",
                     m_perObjectShadowEnabled ? 1 : 0, nr, ns, nh,
                     m_playerShadowAabbMin.x, m_playerShadowAabbMin.y, m_playerShadowAabbMin.z,
                     m_playerShadowAabbMax.x, m_playerShadowAabbMax.y, m_playerShadowAabbMax.z);
            s_dbgRoots = nr; s_dbgSet = ns; s_dbgHas = nh;
        }
    };

    // Retained-membership gate. Seed BEFORE the early-outs: an enable flip or the
    // last root's removal must change the signature so the stale set is cleared on
    // the rebuild path.
    uint64_t seed = m_perObjectShadowEnabled ? 0x517CC1B727220A95ull : 0ull;
    for (Entity e : roots) seed += TagSeedMix(0x100000000ull | e);
    const bool retainSet = m_retainedTagSets && m_tagSetsRetainedValid &&
                           m_sceneMembershipStable && seed == m_playerSetSeedSig;
    m_playerSetSeedSig = seed;
    if (!retainSet)
        m_playerShadowSet.clear();

    if (!m_perObjectShadowEnabled || roots.empty()) { dbgLog(); return; }

    auto* pChildren  = world.GetPool<Children>();
    auto* pWorldAabb = world.GetPool<WorldAabb>();

    if (!retainSet)
    {
        // BFS every root subtree into the caster set.
        std::vector<Entity> stack = roots;
        while (!stack.empty())
        {
            const Entity e = stack.back();
            stack.pop_back();
            if (!m_playerShadowSet.insert(e).second) continue; // cycle / re-visit guard
            if (pChildren)
                if (const Children* ch = pChildren->Get(e))
                    for (Entity c : ch->entities) stack.push_back(c);
        }
    }

    // Character world AABB. The skinned skeleton's WorldAabb is unreliable here
    // (degenerate bone rest-AABBs -> +-FLT_MAX boxes that processAABB copies to every
    // child), and a fixed fallback box does not scale with the character (a scaled-up
    // avatar's head gets clipped). Tiers:
    //   PRIMARY   - each mesh sub-entity's bind-pose mesh AABB (MeshLibrary entry)
    //               x its world matrix: correct, and scales with the transform.
    //               Bind-pose bounds cover the animated head; the radius margins
    //               downstream absorb pose overshoot.
    //   SECONDARY - any sane WorldAabb (primitive players without a MeshLibRef).
    //   LAST      - the root's world position + a default box.
    auto* pMeshRef  = world.GetPool<MeshLibRef>();
    auto* pGlobalXf = world.GetPool<GlobalTransform>();

    // Reject casters farther than kMaxCasterDist from the camera. The map is ONE
    // 4096^2 ortho slab sized to the merged AABB, so a single caster that fell out of
    // the world (Y = -13000) spreads it over kilometres and every real shadow in it
    // collapses to sub-pixel, with no error anywhere: each caster is well-formed, and
    // kMaxExtent bounds only a caster's size, not its position. A caster this far
    // away cannot resolve in the per-object map anyway and CSM covers it: at 150 m
    // the map is down to ~14 texels per metre, and a falling entity (55 m/s terminal
    // velocity) is caught within ~3 s, before it visibly degrades the fit.
    constexpr float kMaxCasterDist  = 150.0f;
    const XMFLOAT3   camP           = FrameViewWrite().view.cameraPosition;
    uint32_t         rejectedFar    = 0;
    auto nearEnough = [&](float x, float y, float z) {
        const float dx = x - camP.x, dy = y - camP.y, dz = z - camP.z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (!std::isfinite(d2) || d2 > kMaxCasterDist * kMaxCasterDist)
        { ++rejectedFar; return false; }
        return true;
    };

    XMFLOAT3 lo{}, hi{};
    bool first = true;
    auto expand = [&](float mnx, float mny, float mnz, float mxx, float mxy, float mxz) {
        if (first) { lo = { mnx, mny, mnz }; hi = { mxx, mxy, mxz }; first = false; }
        else {
            lo.x = std::min(lo.x, mnx); lo.y = std::min(lo.y, mny); lo.z = std::min(lo.z, mnz);
            hi.x = std::max(hi.x, mxx); hi.y = std::max(hi.y, mxy); hi.z = std::max(hi.z, mxz);
        }
    };

    // 1) PRIMARY - real mesh bounds (bind-pose entry AABB x world transform).
    if (pMeshRef && pGlobalXf && m_meshLib)
    {
        for (Entity e : m_playerShadowSet)
        {
            const MeshLibRef*      mr = pMeshRef->Get(e);
            const GlobalTransform* gt = pGlobalXf->Get(e);
            if (!mr || !mr->IsValid() || !gt) continue;
            const auto* entry = m_meshLib->GetEntry(mr->libHandle, mr->meshId);
            if (!entry) continue;
            if (!nearEnough(gt->matrix.m[3][0], gt->matrix.m[3][1], gt->matrix.m[3][2])) continue;
            const XMFLOAT3& amn = entry->aabbMin;
            const XMFLOAT3& amx = entry->aabbMax;
            if (amn.x > amx.x || amn.y > amx.y || amn.z > amx.z) continue; // empty entry
            const XMMATRIX m  = XMLoadFloat4x4(&gt->matrix);
            const float xs[2] = { amn.x, amx.x }, ys[2] = { amn.y, amx.y }, zs[2] = { amn.z, amx.z };
            for (int ix = 0; ix < 2; ++ix)
            for (int iy = 0; iy < 2; ++iy)
            for (int iz = 0; iz < 2; ++iz)
            {
                XMFLOAT3 p;
                XMStoreFloat3(&p, XMVector3TransformCoord(XMVectorSet(xs[ix], ys[iy], zs[iz], 1.0f), m));
                if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z))
                    expand(p.x, p.y, p.z, p.x, p.y, p.z);
            }
        }
    }

    // 2) SECONDARY - sane WorldAabb. The extent cap is load-bearing: a skeleton
    // with no usable bone rest-AABBs makes processAABB emit a +-FLT_MAX box
    // (finite and min < max, so finiteness alone does not reject it) copied to
    // every child.
    constexpr float kMaxExtent = 1.0e4f; // 10 km; any character box bigger = garbage
    if (pWorldAabb)
    {
        auto validAabb = [&](const WorldAabb& a) {
            return std::isfinite(a.min.x) && std::isfinite(a.min.y) && std::isfinite(a.min.z)
                && std::isfinite(a.max.x) && std::isfinite(a.max.y) && std::isfinite(a.max.z)
                && a.min.x <= a.max.x && a.min.y <= a.max.y && a.min.z <= a.max.z
                && (a.max.x - a.min.x) < kMaxExtent
                && (a.max.y - a.min.y) < kMaxExtent
                && (a.max.z - a.min.z) < kMaxExtent;
        };
        for (Entity e : m_playerShadowSet)
        {
            const WorldAabb* wa = pWorldAabb->Get(e);
            if (!wa || !validAabb(*wa)) continue;
            if (!nearEnough(0.5f * (wa->min.x + wa->max.x),
                            0.5f * (wa->min.y + wa->max.y),
                            0.5f * (wa->min.z + wa->max.z))) continue;
            expand(wa->min.x, wa->min.y, wa->min.z, wa->max.x, wa->max.y, wa->max.z);
        }
    }

    // 3) LAST - root world position + a default box (only when no tier above hit).
    if (first && pGlobalXf)
    {
        if (const GlobalTransform* gt = pGlobalXf->Get(roots.front()))
        {
            const float px = gt->matrix.m[3][0], py = gt->matrix.m[3][1], pz = gt->matrix.m[3][2];
            if (std::isfinite(px) && std::isfinite(py) && std::isfinite(pz))
            {
                const float h = 1.5f; // asymmetric: covers feet- OR centre-pivot
                expand(px - h, py - 0.5f, pz - h, px + h, py + 3.0f, pz + h);
            }
        }
    }

    // Warn once: a rejected caster is usually an entity that fell through the
    // collision mesh, and without this line the only symptom is shadows quietly
    // fading away.
    if (rejectedFar > 0)
    {
        static bool s_warned = false;
        if (!s_warned)
        {
            s_warned = true;
            LOG_WARNING("PerObjectShadow: %u caster(s) sit further than %.0f m from the "
                        "camera and were EXCLUDED from the shadow fit. Either an entity "
                        "fell out of the world (check for a KCC airborne warning) or the "
                        "camera is simply far from the casters. Including them would "
                        "spread the 4096x4096 map over a huge area and fade every real "
                        "shadow to nothing; per-object shadow degrades to CSM instead.",
                        rejectedFar, kMaxCasterDist);
        }
    }

    if (!first)
    {
        m_playerShadowAabbMin   = lo;
        m_playerShadowAabbMax   = hi;
        m_hasPlayerShadowCaster = true;
    }
    dbgLog();
}

// Hair (bangs -> face) shadow caster set. Mirrors
// BuildScene_ComputePlayerShadowSet but seeds only from HairShadowCasterComponent
// roots (no implicit player path: a purely opt-in, per-character feature) and
// merges a much smaller (head-sized) AABB. The Hair variant of
// PerObjectShadowPass renders the tagged packets; UploadFrameData fits the
// reversed-Z ortho VP to the merged AABB; only NPR face pixels sample the result
// in Lighting.ps.
void Renderer::BuildScene_ComputeHairShadowSet(World& world)
{
    ZoneScopedN("BS.HairShadowSet");
    m_hasHairShadowCaster = false;
    m_hairShadowWorldScale = 1.0f;

    auto* pTag = world.GetPool<HairShadowCasterComponent>();

    // Retained-membership gate (see the player set). The artistic params and world
    // scale below stay per-frame raw reads (an inspector scrub must preview live), as
    // does the merged AABB; only the BFS is retained.
    uint64_t seed = m_hairShadowEnabled ? 0x2545F4914F6CDD1Dull : 0ull;
    if (pTag) for (Entity e : pTag->Entities()) seed += TagSeedMix(0x200000000ull | e);
    const bool retainSet = m_retainedTagSets && m_tagSetsRetainedValid &&
                           m_sceneMembershipStable && seed == m_hairSetSeedSig;
    m_hairSetSeedSig = seed;
    if (!retainSet)
        m_hairShadowSet.clear();

    if (!m_hairShadowEnabled || !pTag || pTag->Entities().empty()) return;

    // Per-frame artistic params come from the FIRST tagged component (the map is
    // global, like the player one: multi-character setups share one strength).
    std::vector<Entity> roots;
    for (Entity e : pTag->Entities()) roots.push_back(e);

    auto* pGlobalXf = world.GetPool<GlobalTransform>();
    if (pGlobalXf)
        if (const GlobalTransform* gt = pGlobalXf->Get(roots.front()))
            m_hairShadowWorldScale = ExtractUniformWorldScale(gt->matrix);

    if (const HairShadowCasterComponent* hc = pTag->Get(roots.front()))
    {
        m_hairShadowStrength = std::clamp(hc->strength, 0.0f, 1.0f);
        m_hairShadowFitScale = std::clamp(hc->fitScale, 0.25f, 4.0f);
        m_hairShadowMode     = std::clamp(hc->mode, 0, 2);
        // +-8 -> up to ~36 mm / 60 mm view-space shift. The reference constants
        // (0.0045 / 0.0075 at scale 1) are calibrated to DanbaidongRP's ~1.6 m
        // characters at portrait distance; larger models or stronger looks need
        // several x that before the mark clears the fringe's own silhouette.
        m_hairMarkOffsetX    = std::clamp(hc->screenOffsetX, -8.0f, 8.0f);
        m_hairMarkOffsetY    = std::clamp(hc->screenOffsetY, -8.0f, 8.0f);
        m_hairMarkDepthPush  = std::clamp(hc->depthPush, 0.0f, 8.0f);
    }

    if (!retainSet)
    {
        auto* pChildren = world.GetPool<Children>();

        // BFS every root subtree into the caster set.
        std::vector<Entity> stack = roots;
        while (!stack.empty())
        {
            const Entity e = stack.back();
            stack.pop_back();
            if (!m_hairShadowSet.insert(e).second) continue; // cycle / re-visit guard
            if (pChildren)
                if (const Children* ch = pChildren->Get(e))
                    for (Entity c : ch->entities) stack.push_back(c);
        }
    }

    // Merge the bangs AABB with the player set's three tiers (the skinned
    // WorldAabb is unreliable; see there).
    auto* pMeshRef   = world.GetPool<MeshLibRef>();
    auto* pWorldAabb = world.GetPool<WorldAabb>();

    XMFLOAT3 lo{}, hi{};
    bool first = true;
    auto expand = [&](float mnx, float mny, float mnz, float mxx, float mxy, float mxz) {
        if (first) { lo = { mnx, mny, mnz }; hi = { mxx, mxy, mxz }; first = false; }
        else {
            lo.x = std::min(lo.x, mnx); lo.y = std::min(lo.y, mny); lo.z = std::min(lo.z, mnz);
            hi.x = std::max(hi.x, mxx); hi.y = std::max(hi.y, mxy); hi.z = std::max(hi.z, mxz);
        }
    };

    // 1) PRIMARY - real mesh bounds (bind-pose entry AABB x world transform).
    if (pMeshRef && pGlobalXf && m_meshLib)
    {
        for (Entity e : m_hairShadowSet)
        {
            const MeshLibRef*      mr = pMeshRef->Get(e);
            const GlobalTransform* gt = pGlobalXf->Get(e);
            if (!mr || !mr->IsValid() || !gt) continue;
            const auto* entry = m_meshLib->GetEntry(mr->libHandle, mr->meshId);
            if (!entry) continue;
            const XMFLOAT3& amn = entry->aabbMin;
            const XMFLOAT3& amx = entry->aabbMax;
            if (amn.x > amx.x || amn.y > amx.y || amn.z > amx.z) continue; // empty entry
            const XMMATRIX m  = XMLoadFloat4x4(&gt->matrix);
            const float xs[2] = { amn.x, amx.x }, ys[2] = { amn.y, amx.y }, zs[2] = { amn.z, amx.z };
            for (int ix = 0; ix < 2; ++ix)
            for (int iy = 0; iy < 2; ++iy)
            for (int iz = 0; iz < 2; ++iz)
            {
                XMFLOAT3 p;
                XMStoreFloat3(&p, XMVector3TransformCoord(XMVectorSet(xs[ix], ys[iy], zs[iz], 1.0f), m));
                if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z))
                    expand(p.x, p.y, p.z, p.x, p.y, p.z);
            }
        }
    }

    // 2) SECONDARY - sane WorldAabb, but only when tier 1 found nothing (the player
    // set merges it unconditionally). For skinned characters processAABB stamps the
    // whole-skeleton box onto the skeleton's children, exactly the entity the bangs
    // tag sits on, so an unconditional merge would balloon the head-sized fit to
    // character size, collapse the map's texel density and push the (slab-scaled)
    // biases past the millimetre bangs -> forehead gap: the shadow silently vanishes.
    constexpr float kMaxExtent = 1.0e4f;
    if (first && pWorldAabb)
    {
        auto validAabb = [&](const WorldAabb& a) {
            return std::isfinite(a.min.x) && std::isfinite(a.min.y) && std::isfinite(a.min.z)
                && std::isfinite(a.max.x) && std::isfinite(a.max.y) && std::isfinite(a.max.z)
                && a.min.x <= a.max.x && a.min.y <= a.max.y && a.min.z <= a.max.z
                && (a.max.x - a.min.x) < kMaxExtent
                && (a.max.y - a.min.y) < kMaxExtent
                && (a.max.z - a.min.z) < kMaxExtent;
        };
        for (Entity e : m_hairShadowSet)
        {
            const WorldAabb* wa = pWorldAabb->Get(e);
            if (!wa || !validAabb(*wa)) continue;
            expand(wa->min.x, wa->min.y, wa->min.z, wa->max.x, wa->max.y, wa->max.z);
        }
    }

    // 3) LAST - root world position + a head-sized default box.
    if (first && pGlobalXf)
    {
        if (const GlobalTransform* gt = pGlobalXf->Get(roots.front()))
        {
            const float px = gt->matrix.m[3][0], py = gt->matrix.m[3][1], pz = gt->matrix.m[3][2];
            if (std::isfinite(px) && std::isfinite(py) && std::isfinite(pz))
            {
                const float h = 0.3f * std::max(m_hairShadowWorldScale, 1.0e-4f);
                expand(px - h, py - h, pz - h, px + h, py + h, pz + h);
            }
        }
    }

    if (!first)
    {
        m_hairShadowAabbMin   = lo;
        m_hairShadowAabbMax   = hi;
        m_hasHairShadowCaster = true;
    }
}

// ==== Face overlay and dissolve sets ====
// Brows/lashes-through-hair overlay set (FaceOverlayComponent roots + their
// Children subtrees) -> m_faceOverlayParams. No AABB: FaceOverlayPass re-draws
// the tagged packets screen-space-gated. Each entity inherits settings from its
// nearest tagged ancestor; nested tags override their parent.
void Renderer::BuildScene_ComputeFaceOverlaySet(World& world)
{
    ZoneScopedN("BS.FaceOverlaySet");
    auto* pTag = world.GetPool<FaceOverlayComponent>();

    // Retained-membership gate. Unlike the shadow sets, the retained map stores
    // RESOLVED inheritance (packed params per entity), so the seed must cover
    // component content: an inspector edit rebuilds the map.
    uint64_t seed = 0;
    if (pTag)
    {
        const auto& ents = pTag->Entities();
        const auto& data = pTag->Data();
        for (size_t i = 0; i < ents.size(); ++i)
            seed += TagSeedMix(
                (uint64_t(PackFaceOverlayParams(data[i])) << 32) ^
                0x300000000ull ^ ents[i]);
    }
    const bool retainSet = m_retainedTagSets && m_tagSetsRetainedValid &&
                           m_sceneMembershipStable && seed == m_faceOverlaySeedSig;
    m_faceOverlaySeedSig = seed;
    if (retainSet)
    {
        m_hasFaceOverlay = !m_faceOverlayParams.empty();
        return;
    }

    m_faceOverlayParams.clear();
    m_hasFaceOverlay = false;

    if (!pTag || pTag->Entities().empty()) return;

    auto* pParent = world.GetPool<Parent>();
    auto* pChildren = world.GetPool<Children>();

    // Start only from top-most tagged entities. Descendant tags are visited as
    // part of the same traversal and replace the inherited settings there.
    std::vector<Entity> roots;
    roots.reserve(pTag->Entities().size());
    for (Entity e : pTag->Entities())
    {
        bool hasTaggedAncestor = false;
        std::unordered_set<Entity> seen;
        Entity current = e;
        while (pParent)
        {
            const Parent* parent = pParent->Get(current);
            if (!parent || parent->entity == NullEntity) break;
            if (!seen.insert(parent->entity).second) break;
            if (pTag->Get(parent->entity))
            {
                hasTaggedAncestor = true;
                break;
            }
            current = parent->entity;
        }
        if (!hasTaggedAncestor) roots.push_back(e);
    }

    struct StackEntry
    {
        Entity entity;
        uint32_t inheritedParams;
    };
    std::vector<StackEntry> stack;
    stack.reserve(roots.size());
    for (Entity root : roots)
        if (const FaceOverlayComponent* component = pTag->Get(root))
            stack.push_back({ root, PackFaceOverlayParams(*component) });

    std::unordered_set<Entity> visited;
    while (!stack.empty())
    {
        const StackEntry entry = stack.back();
        stack.pop_back();
        if (!visited.insert(entry.entity).second) continue;

        uint32_t params = entry.inheritedParams;
        if (const FaceOverlayComponent* component = pTag->Get(entry.entity))
            params = PackFaceOverlayParams(*component);
        m_faceOverlayParams[entry.entity] = params;

        if (pChildren)
            if (const Children* ch = pChildren->Get(entry.entity))
                for (Entity child : ch->entities)
                    stack.push_back({ child, params });
    }

    m_hasFaceOverlay = !m_faceOverlayParams.empty();
}

// Dissolve set (DissolveComponent roots + their Children subtrees) ->
// m_dissolveParams. Same inheritance as the face-overlay set: each entity
// inherits the packed params from its nearest tagged ancestor; nested tags
// override. Entities whose (inherited) amount <= 0 are not inserted: the gather
// leaves their instance dissolve fields zero and the shader branch stays off.
void Renderer::BuildScene_ComputeDissolveSet(World& world)
{
    ZoneScopedN("BS.DissolveSet");
    m_dissolveParams.clear();
    m_hasDissolve = false;

    auto* pTag = world.GetPool<DissolveComponent>();
    if (!pTag || pTag->Entities().empty()) return;

    auto* pParent   = world.GetPool<Parent>();
    auto* pChildren = world.GetPool<Children>();

    auto pack = [](const DissolveComponent& d) -> DissolveDrawParams
    {
        DissolveDrawParams p;
        p.params    = { d.amount, d.edgeWidth, d.noiseScale, d.edgeIntensity };
        p.edgeColor = { d.edgeColor.x, d.edgeColor.y, d.edgeColor.z, 0.0f };
        return p;
    };

    // Start only from top-most tagged entities; descendant tags are visited as
    // part of the same traversal and replace the inherited settings there.
    std::vector<Entity> roots;
    roots.reserve(pTag->Entities().size());
    for (Entity e : pTag->Entities())
    {
        bool hasTaggedAncestor = false;
        std::unordered_set<Entity> seen;
        Entity current = e;
        while (pParent)
        {
            const Parent* parent = pParent->Get(current);
            if (!parent || parent->entity == NullEntity) break;
            if (!seen.insert(parent->entity).second) break;
            if (pTag->Get(parent->entity))
            {
                hasTaggedAncestor = true;
                break;
            }
            current = parent->entity;
        }
        if (!hasTaggedAncestor) roots.push_back(e);
    }

    struct StackEntry
    {
        Entity entity;
        DissolveDrawParams inherited;
    };
    std::vector<StackEntry> stack;
    stack.reserve(roots.size());
    for (Entity root : roots)
        if (const DissolveComponent* component = pTag->Get(root))
            stack.push_back({ root, pack(*component) });

    std::unordered_set<Entity> visited;
    while (!stack.empty())
    {
        const StackEntry entry = stack.back();
        stack.pop_back();
        if (!visited.insert(entry.entity).second) continue;

        DissolveDrawParams params = entry.inherited;
        if (const DissolveComponent* component = pTag->Get(entry.entity))
            params = pack(*component);
        if (params.params.x > 0.0f)
            m_dissolveParams[entry.entity] = params;

        if (pChildren)
            if (const Children* ch = pChildren->Get(entry.entity))
                for (Entity child : ch->entities)
                    stack.push_back({ child, params });
    }

    m_hasDissolve = !m_dissolveParams.empty();
}
