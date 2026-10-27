#include "Renderer/Renderer.h"

// engine graphics / backend
#include "Graphics/GraphicsDX12.h"
#include "System/HostPolicy.h"
#include "Graphics/AfterimageSystem.h"
#include "Graphics/WaterRippleSystem.h"

// render passes
#include "RenderGraph/RenderPass/GBufferPass.h"

// ECS components + systems
#include "ECS/StaticMeshComponents.h"   // a StaticMesh owner's outline is its sections'
#include "ECS/MaterialReflectionSync.h"
#include "ECS/MaterialSchema.h"
#include "Renderer/GatherGateStats.h"   // the patch gate's frames.jsonl record

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
#include <type_traits>   // is_trivially_copyable: the candidate cache's static_assert

using namespace DirectX;

// CPU-side cbuffer mirrors live in Renderer.h (RendererDetail namespace) so the
// triple-buffered FrameCB<T> members can be instantiated in the class layout.
// Layouts there MUST stay in sync with the matching HLSL.
using PerViewCB       = RendererDetail::PerViewCB;
using LightCB         = RendererDetail::LightCB;


// Renderer_Scene.cpp: BuildRenderScene, the ECS -> DrawPacket orchestrator, and the
// state it keeps between the gathers and the emit: the material snapshots, the
// candidate classification and the static / dynamic candidate caches. The steps it
// calls live in Renderer_Lights, _SceneSets, _SceneGather, _Materials, _EmitPlan and
// _EmitBatches. All members belong to class Renderer (Renderer/Renderer.h).

// The material bits and the material-packing helpers (MATERIAL_GPU_FLAGS, the
// binding / runtime keys, HashMatParams) are in RendererSceneShared.h, shared
// with the other scene TUs.
#include "Renderer/RendererSceneShared.h"

// ==== Material snapshots ====
// A snapshot is a value copy of a candidate's MaterialComponent, taken while `mc`
// still points into live pool storage; the emit works from the copy, so
// ComponentPool::Erase's swap-with-back cannot make it name another entity's
// material. The table persists across frames (see m_matSnapshots), keyed by
// MatSnapKey.
uint64_t Renderer::MatSnapKey(const DrawCandidate& c)
{
    // Folds the MaterialBindingKey fields into one word. Never 0: 0 marks an
    // empty probe slot.
    uint64_t k = c.matHash * 0x9E3779B97F4A7C15ull;
    k ^= c.texBaseColor  + 0x165667B19E3779F9ull + (k << 6) + (k >> 2);
    k ^= c.texSurfaceMap + 0x27D4EB2F165667C5ull + (k << 6) + (k >> 2);
    k ^= c.texNormalMap  + 0x85EBCA77C2B2AE63ull + (k << 6) + (k >> 2);
    k ^= (uint64_t(c.customPSID) << 32) ^ uint64_t(c.customVSID);
    k ^= (c.matBits & MB_HAS_MATERIAL) ? 0xD6E8FEB86659FD93ull : 0ull;

    // PER_FRAME_PARAMS materials key on IDENTITY (the component's address), not
    // content. An engine system rewrites their frameParams every frame and the
    // snapshot carries this frame's values, so two faces with the same authored
    // content must not share one: a content key would hand both the same head frame.
    // One such material keeps one slot for its life and the hit path refreshes it in
    // place (RefreshVolatileSnapshot); they are per-entity instances, so no sharing is
    // lost. The persistent material slot follows the same rule
    // (MaterialBindingKey::identity).
    if (c.matBits & MB_FRAME_PARAMS)
    {
        k = reinterpret_cast<uintptr_t>(c.mc) * 0x9E3779B97F4A7C15ull;
        k ^= (k >> 29);
        k *= 0xBF58476D1CE4E5B9ull;
        k ^= (k >> 32);
        k |= 1ull;   // never 0: 0 marks an empty probe slot
    }
    return k ? k : 1ull;
}

uint32_t Renderer::AcquireMatSnapshot(const MaterialComponent* mc, uint64_t key,
                                    std::vector<MatSnapshotCopy>* deferredCopies)
{
    if (!mc) return 0xFFFFFFFFu;

    // Rebuild at 50% load: linear probing degrades badly past that. Tombstones count
    // toward the load, or a table full of them never rebuilds and the probe loops
    // below never end.
    // Only the LIVE entries choose the new size: under a quarter live, rebuild at the
    // same size, which drops the tombstones. Doubling instead would grow the index with
    // every retirement ever made (nothing else empties it; GCMatSnapshots clears only
    // at its cap). The next rebuild is then more than a quarter of the table's
    // insertions away, so the same-size rebuild stays amortised O(1).
    const size_t live     = m_matSnapshots.size() - m_matSnapFree.size();
    const size_t occupied = live + m_matSnapTombs;
    if (m_matSnapTable.empty() || occupied * 2 >= m_matSnapTable.size())
    {
        const size_t want = m_matSnapTable.empty()            ? 1024u
                          : live * 4 >= m_matSnapTable.size() ? m_matSnapTable.size() * 2
                          :                                     m_matSnapTable.size();
        std::vector<MatSnapSlot> next(want);
        const uint64_t mask = want - 1;
        for (const MatSnapSlot& sl : m_matSnapTable)
        {
            if (!sl.key || sl.key == ~0ull) continue;   // drop tombstones
            size_t i = size_t(sl.key & mask);
            while (next[i].key) i = (i + 1) & mask;
            next[i] = sl;
        }
        m_matSnapTable.swap(next);
        m_matSnapTombs = 0;   // rehash drops them
    }

    constexpr uint64_t kTomb = ~0ull;
    const uint64_t mask = m_matSnapTable.size() - 1;
    size_t i = size_t(key & mask);
    size_t firstTomb = SIZE_MAX;
    // BOUNDED. An unbounded probe over a table with no empty slot never returns.
    for (size_t probes = 0; probes < m_matSnapTable.size() &&
                            m_matSnapTable[i].key; ++probes)
    {
        if (m_matSnapTable[i].key == kTomb)
        {
            if (firstTomb == SIZE_MAX) firstTomb = i;
            i = (i + 1) & mask;
            continue;
        }
        if (m_matSnapTable[i].key == key)
        {
            const uint32_t idx = m_matSnapTable[i].idx;
            m_matSnapEpoch[idx] = m_materialFrameEpoch;
            // An identity-keyed slot holds a material whose CONTENT changes
            // every frame, so a hit must refresh it. Content-keyed slots must
            // not be touched: identical key means identical content.
            if (mc->HasPerFrameParams())
            {
                if (deferredCopies) deferredCopies->emplace_back(idx, mc);
                else RefreshVolatileSnapshot(idx, *mc, /*allowParamsOnly=*/false);
            }
            return idx;
        }
        i = (i + 1) & mask;
    }

    // Miss: reuse retired storage when there is any. Assigning into an existing
    // MaterialComponent reuses its strings' capacity; a fresh one pays ~25 heap
    // allocations (~1 ms a frame at ~195 misses).
    uint32_t idx;
    if (!m_matSnapFree.empty())
    {
        idx = m_matSnapFree.back();
        m_matSnapFree.pop_back();
        m_matSnapshots[idx].mat.Assign(*mc);
        m_matSnapshots[idx].scalarHash = HashMatScalars(*mc);
        m_matSnapshots[idx].paramMapValid = false;   // rebuilt by the next whole refresh
    }
    else
    {
        idx = static_cast<uint32_t>(m_matSnapshots.size());
        m_matSnapshots.push_back(MatSnapshot{ Sys::SharedSnapshot<MaterialComponent>(*mc),
                                              HashMatScalars(*mc) });
        m_matSnapEpoch.push_back(0);
        m_matSnapKeyOf.push_back(0);
    }
    m_matSnapEpoch[idx] = m_materialFrameEpoch;
    m_matSnapKeyOf[idx] = key;
    // Reuse the first deleted bucket found during probing. Otherwise every
    // insertion leaves tombstones behind and needlessly grows the hash table.
    if (firstTomb != SIZE_MAX) {
        i = firstTomb;
        --m_matSnapTombs;
    }
    m_matSnapTable[i]   = MatSnapSlot{ key, idx };
    return idx;
}

void Renderer::RefreshVolatileSnapshot(uint32_t idx, const MaterialComponent& src,
                                       bool allowParamsOnly)
{
    static const bool s_verify = Sys::CommandLine::Get().Has("-matsnapparamverify");
    MatSnapshot& snap = m_matSnapshots[idx];
    const uint64_t scalars = HashMatScalars(src);
    if (s_verify && allowParamsOnly)
    {
        // Why a refresh falls back to the whole copy (diagnostic, verify only).
        if (scalars != snap.scalarHash) m_matSnapWhyScalar.fetch_add(1, std::memory_order_relaxed);
        else if (!snap.mat.MutableIfUnique()) m_matSnapWhyLeased.fetch_add(1, std::memory_order_relaxed);
    }
    if (allowParamsOnly && snap.paramMapValid && scalars == snap.scalarHash)
        if (MaterialComponent* dst = snap.mat.MutableIfUnique())
        {
            if (CopyMappedParamValues(snap.paramMap, src))
            {
                m_matSnapFastRefreshes.fetch_add(1, std::memory_order_relaxed);
                if (s_verify)
                {
                    // Oracle: everything the binding key covers, textures
                    // resolved to nothing on both sides, must now agree.
                    m_matSnapParamVerified.fetch_add(1, std::memory_order_relaxed);
                    if (HashMatParams(dst, ResolvedMaterialRuntime{}) !=
                        HashMatParams(&src, ResolvedMaterialRuntime{}) ||
                        dst->customParams != src.customParams ||
                        dst->frameParams != src.frameParams)
                        m_matSnapParamMismatches.fetch_add(1, std::memory_order_relaxed);
                }
                return;
            }
            if (s_verify) m_matSnapWhyKeys.fetch_add(1, std::memory_order_relaxed);
        }
    snap.mat.Assign(src);
    snap.scalarHash = scalars;
    // Whole copy done: the snapshot is unique now (Assign copied-on-write if it
    // was leased), so map the source's order onto its fresh nodes.
    MaterialComponent* dst = snap.mat.MutableIfUnique();
    snap.paramMapValid = dst && BuildParamMap(snap.paramMap, *dst, src);
    m_matSnapFullRefreshes.fetch_add(1, std::memory_order_relaxed);
}

// Retire snapshots nothing has referenced for a couple of frames, returning
// their STORAGE (not their memory) to the free list.
void Renderer::RetireMatSnapshots()
{
    constexpr uint64_t kGrace = 2;
    if (m_materialFrameEpoch <= kGrace || m_matSnapTable.empty()) return;
    const uint64_t cutoff = m_materialFrameEpoch - kGrace;
    const uint64_t mask   = m_matSnapTable.size() - 1;

    for (uint32_t idx = 0; idx < m_matSnapEpoch.size(); ++idx)
    {
        if (m_matSnapEpoch[idx] == 0 || m_matSnapEpoch[idx] >= cutoff) continue;
        const uint64_t key = m_matSnapKeyOf[idx];
        if (key)
        {
            size_t i = size_t(key & mask);
            for (size_t probes = 0; probes < m_matSnapTable.size() &&
                                    m_matSnapTable[i].key; ++probes)
            {
                if (m_matSnapTable[i].key == key && m_matSnapTable[i].idx == idx)
                {
                    m_matSnapTable[i].key = ~0ull;   // tombstone
                    ++m_matSnapTombs;
                    break;
                }
                i = (i + 1) & mask;
            }
        }
        m_matSnapEpoch[idx] = 0;   // retired
        m_matSnapKeyOf[idx] = 0;
        m_matSnapFree.push_back(idx);
    }
}

void Renderer::GCMatSnapshots()
{
    // A cap on storage, live and retired slots alike, so the copies a one-off spike
    // leaves on the free list are released once instead of kept.
    // No age test, deliberately: retired slots carry epoch 0, so an age test reads
    // them as the oldest of all and clears the whole table the frame after any
    // retirement - voiding the dynamic-row cache and the retained matSnaps and
    // re-copying every snapshot (an 8-11 ms build against ~1.2 ms). A live slot is
    // never old here: RetireMatSnapshots runs right after this in the same build and
    // retires every slot unreferenced for its grace, keeping the storage for the next
    // miss.
    constexpr size_t kMaxSnapshots = 4096;
    if (m_matSnapshots.size() <= kMaxSnapshots) return;

    m_matSnapshots.clear();
    m_matSnapEpoch.clear();
    m_matSnapTable.clear();
    m_matSnapKeyOf.clear();
    m_matSnapFree.clear();
    m_matSnapTombs = 0;
    // Every cached candidate's matSnap now names nothing. Force one full gather.
    m_candCacheValid = false;
    m_dynCandCacheValid = false;
    // The retained matSnaps name nothing THIS frame either: the patch decision was
    // taken before this ran, so the patch arm checks m_matSnapClearGen rather than
    // trusting m_candCacheValid.
    ++m_matSnapClearGen;
    m_candCacheRetained = false;
}

Renderer::ClassifyInputs Renderer::CaptureClassifyInputs(size_t cullBegin,
        const XMFLOAT3& camForward) const
{
    ClassifyInputs in;
    in.mainFrustum           = FrameViewWrite().view.boundingFrustum;
    in.cameraPos             = FrameViewWrite().camera.position;
    in.camForward            = camForward;
    in.cullBegin             = cullBegin;
    in.gpuCulling            = m_gpuCullingEnabled;
    in.shadowArgsFrameActive = SceneEmitWrite().shadowArgsFrameActive;
    return in;
}

void Renderer::BuildScene_ClassifyCandidates(const ClassifyInputs& in, World* world,
        std::vector<DrawCandidate>& candidates)
{
    ZoneScopedN("BS.CandClassify");
    const size_t cullBegin = in.cullBegin;
    const std::vector<uint8_t>& bvhTestedMask  = m_bvhTestedMask;
    const std::vector<uint8_t>& bvhVisibleMask = m_bvhVisibleMask;
    const XMFLOAT3& camForwardThisFrame = in.camForward;
    // Rows [0, cullBegin) come from the primitive gather (and the cache): never
    // camera-culled, only their transparent depth is re-derived (cached rows carry
    // their snapshot frame's depth). Rows [cullBegin, end) are MeshLibRef rows:
    // frustum cull + shadow fallback, dropped rows compacted out. Billboards / beams /
    // afterimages are appended after this (they cull and depth-sort themselves).
    // `live` = the main form. The lane form (world == nullptr) reads the box from the
    // candidate and the material predicates from matBits only: `mc` is a raw pool
    // pointer whose next writer is main's F+1 game half.
    const bool live = world != nullptr;
    bool verifyVisibility = false;
#if WITH_HARNESS
    verifyVisibility = Sys::CommandLine::Get().Has("-classifycacheverify");
#endif
    // Only the last geometric query is retained, on this call's stack.
    // All sections of an entity share the frame-stable WorldAabb. Keep main
    // and shadow queries separate; BVH masks and secondary views bypass this.
    struct VisibilityMemo { Entity entity{}; bool valid = false, visible = false; };
    VisibilityMemo mainMemo, shadowMemo;
    uint32_t visibilityHits = 0;
    auto memoVisibility = [&](VisibilityMemo& memo, Entity entity, auto&& test) {
        if (memo.valid && memo.entity == entity) {
            ++visibilityHits;
            if (verifyVisibility) ENGINE_CHECK(memo.visible == test(), "classify visibility cache mismatch");
            return memo.visible;
        }
        const bool visible = test();
        memo = {entity, true, visible};
        return visible;
    };
    auto mainVisible = [&](Entity entity, const WorldAabb& aabb) {
        return memoVisibility(mainMemo, entity, [&] {
            return FrustumTestAABB(in.mainFrustum, aabb.min, aabb.max);
        });
    };
    auto shadowVisible = [&](Entity entity, const BoundingBox& box) {
        return memoVisibility(shadowMemo, entity, [&] { return ShadowRetains(box); });
    };
    struct VisibilityReport {
        bool verify; uint32_t& hits;
        ~VisibilityReport() {
            if (verify && hits) LOG_INFO("[CLASSIFY_CACHE_ORACLE] hits=%u match=1", hits);
        }
    } visibilityReport{verifyVisibility, visibilityHits};
    // The secondary-view duplicates. Cleared here because this function is their only
    // producer; the caller appends them right after it returns. clear() keeps
    // capacity, so an open PiP pays no steady-state allocation.
    m_secondaryCands.clear();
    auto* pWorldAabb = live ? world->GetPool<WorldAabb>() : nullptr;
    // The row's box. Main: the entity's live WorldAabb. Lane: the copy the gather /
    // patch took from that pool earlier in this build (hasAabb says whether there was
    // one). `tmp` is the caller's storage.
    auto boxOf = [&](const DrawCandidate& c, WorldAabb& tmp) -> const WorldAabb* {
        if (live) return pWorldAabb ? pWorldAabb->Get(c.entity) : nullptr;
        if (!c.hasAabb) return nullptr;
        tmp.min = c.aabbMin;
        tmp.max = c.aabbMax;
        return &tmp;
    };
    auto inMask = [](const std::vector<uint8_t>& m, Entity e) -> bool {
        return e < m.size() && m[e] != 0;
    };
    // Same expression ApplyBlendMode uses (m_camera + this frame's forward).
    auto refreshDepth = [&](DrawCandidate& c) {
        const float dx = c.worldCenter.x - in.cameraPos.x;
        const float dy = c.worldCenter.y - in.cameraPos.y;
        const float dz = c.worldCenter.z - in.cameraPos.z;
        c.depth = dx * camForwardThisFrame.x
                + dy * camForwardThisFrame.y
                + dz * camForwardThisFrame.z;
    };

    // Off-screen transparent cull (both paths below). A transparent draw has no GPU
    // visibility test anywhere (it sits outside every indirect group, cmd-group meta
    // 0xFFFFFFFF, and TransparentPass draws the list as-is), so this CPU frustum test
    // is the only thing between an off-screen transparent and an emit + depth-sort
    // slot + draw call every frame. Only DISPOSABLE packets drop; anything another
    // view consumes is kept:
    //   * alpha-test casters: the shadow passes walk the Transparent list and
    //     ClassifyShadowDrawGroup admits exactly castShadow && ALPHA_TEST
    //     (blend / additive feed nothing off-screen);
    //   * per-object / hair casters: their fitted shadow map can be visible while
    //     the caster is off-frustum;
    //   * WPO (customVSID != 0): displacement is unbounded by any CPU-published AABB
    //     (the cull CS exempts it the same way, kInstanceFlagNoGeomCull);
    //   * water keeps its own filter (wave displacement can leave the resting AABB).
    // The lane form never increments its mismatch counter (it has nothing live to
    // cross-check against), and m_frameStats is main's from the hand-off on.
    uint32_t laneMismatch = 0;
    uint32_t& matMismatch = live ? m_frameStats.matBitsMismatches : laneMismatch;
    // A material predicate. Main: the cached bit cross-checked against *mc
    // (MatBitCheck; `liveValue` is only evaluated there). Lane: the cached bit; the
    // main form's cross-check is what says the two agree at this program point, and
    // -laneclassifyverify compares whole rows.
    auto matBit = [&](const DrawCandidate& c, uint16_t bit, auto&& liveValue,
                      const char* name) -> bool {
        if (!live) return (c.matBits & bit) != 0;
        return MatBitCheck(matMismatch, c.matBits, bit, liveValue(), name);
    };
    auto isDisposableTransparent = [&](const DrawCandidate& c) {
        return c.filter == DrawFilter::Transparent
            && c.customVSID == 0
            && !c.perObjectShadowCaster
            && !c.hairShadowCaster
            && !(c.castShadow && matBit(c, MB_ALPHA_TEST,
                     [&] { return c.mc && c.mc->IsAlphaTestEnabled(); }, "ALPHA_TEST"));
    };

    const size_t n = candidates.size();
    for (size_t r = 0; r < cullBegin && r < n; ++r)
        if (IsDepthSortedFilter(candidates[r].filter))
            refreshDepth(candidates[r]);

    // Shadow-args frames (emit-once, see the general loop below): the general loop's
    // drop / convert branch is off there, and running it would copy two 248-byte
    // DrawCandidates per row (~3.5 MB a frame at ~7k rows) mostly to reproduce its input.
    // This loop refreshes transparent depth in place and handles only what the GPU cull
    // cannot: disposable transparents, off-screen skinned casters and their outlines.
    if (in.shadowArgsFrameActive)
    {
        // Compacting, but copy-free until something is dropped: `w` tracks `r` until the
        // first removal, so a frame that culls nothing pays no DrawCandidate copies.
        // The row loop is memory-bound and stays serial: running its decisions on worker
        // lanes saved 0.07 ms on main for +0.34 ms-core.
        PROF_SCOPE("BRS.Classify.Rows");
        size_t w = cullBegin;
        for (size_t r = cullBegin; r < n; ++r)
        {
            DrawCandidate& c = candidates[r];
            bool drop = false;
            if (IsDepthSortedFilter(c.filter)) refreshDepth(c);

            // SKINNED draws still need this pass. The cull CS can only zero an instance
            // count; it cannot move a draw from the Opaque filter to the Shadow one, and that
            // conversion has to happen before SortAndEmit. The CS does frustum-test skinned
            // sections (they publish animated bounds), so an off-screen one that slipped past
            // would be compacted to zero instances: what this saves is packet count and the
            // authored shadow-bias bucket, not correctness. Skinned entities are not static-lane
            // (IsStaticLaneInstance) and re-gather every frame, so testing them does not make
            // the retained static packet set camera-dependent. Their WorldAabb is the animated
            // one (TickAnimationChain's bone merge, earlier this frame).
            // CONVERT, never drop: off-screen characters must keep casting into the cascades;
            // DrawFilter::Shadow means shadow caster only.
            // Every `continue` below means KEEP, so the compaction runs in this guard's
            // destructor rather than at the bottom of the loop.
            struct KeepOnExit {
                std::vector<DrawCandidate>& v; size_t& w; size_t r; bool& drop;
                ~KeepOnExit() {
                    if (drop) return;
                    if (w != r) v[w] = v[r];
                    ++w;
                }
            } _keep{ candidates, w, r, drop };

            // Disposable transparents (isDisposableTransparent) are the other thing this path
            // must cull: with no GPU visibility test, an off-screen one would still pay its
            // emit, depth-sort slot and draw. Kept transparents (alpha-test / per-object / hair
            // casters, WPO) are untouched. Skinned transparents are included: their WorldAabb
            // is the animated bound.
            if (isDisposableTransparent(c))
            {
                WorldAabb box;
                const WorldAabb* aabb = boxOf(c, box);
                if (!aabb) continue;            // no bounds -> no cull
                bool visible;
                if (in.gpuCulling && inMask(bvhTestedMask, c.entity))
                    visible = inMask(bvhVisibleMask, c.entity);
                else
                    visible = mainVisible(c.entity, *aabb);
                // About to leave the Transparent list: offer it to any live secondary view first
                // (one bool test when there is none).
                if (!visible)
                    NoteSecondaryViewCandidate(c, aabb->min, aabb->max);
                if (!visible) drop = true;
                continue;
            }

            if (!c.skinned) continue;
            if (c.filter == DrawFilter::Shadow) continue;
            if (matBit(c, MB_TRANSPARENT,
                    [&] { return c.mc && IsTransparentBlendMode(c.mc->userBlendMode); },
                    "TRANSPARENT")
             || matBit(c, MB_WATER,
                    [&] { return c.mc && c.mc->IsWaterShadingModel(); }, "WATER"))  continue;
            WorldAabb box;
            const WorldAabb* aabb = boxOf(c, box);
            if (!aabb) continue;                       // no bounds -> no cull
            if (mainVisible(c.entity, *aabb)) continue;

            // An outline the main view does not see (OutlineGeometryPass draws real geometry per
            // DrawFilter::Custom packet) has no shadow form to convert to: a live secondary view
            // that sees it keeps a copy (SecondaryViewCustom), the main view drops it. The gather
            // cannot (it deferred its cull, so isShadowOnly is never set on this path). The
            // editor's picking outline is exempt: an x-ray affordance for a deliberately selected
            // entity, authored to show through geometry.
            if (c.filter == DrawFilter::Custom)
            {
                if (!c.isPickingOutline)
                {
                    NoteSecondaryViewCandidate(c, aabb->min, aabb->max);
                    drop = true;
                }
                continue;
            }

            if (!c.castShadow || !ShadowRetentionValid()) continue;
            BoundingBox bb;
            bb.Center  = { (aabb->min.x + aabb->max.x) * 0.5f,
                           (aabb->min.y + aabb->max.y) * 0.5f,
                           (aabb->min.z + aabb->max.z) * 0.5f };
            bb.Extents = { (aabb->max.x - aabb->min.x) * 0.5f,
                           (aabb->max.y - aabb->min.y) * 0.5f,
                           (aabb->max.z - aabb->min.z) * 0.5f };
            if (!shadowVisible(c.entity, bb)) continue;
            // Before the conversion, which resets the permutation: the secondary-view copy is
            // taken from the candidate as it stands, not read back out of the Shadow list.
            NoteSecondaryViewCandidate(c, aabb->min, aabb->max);
            c.filter = DrawFilter::Shadow;
            c.perm   = {};
            if (matBit(c, MB_ALPHA_TEST,
                    [&] { return c.mc && c.mc->IsAlphaTestEnabled(); }, "ALPHA_TEST"))
                c.perm.Set(PermutationKey::ALPHA_TEST, true);
        }
        candidates.resize(w);
        return;
    }

    size_t w = cullBegin;
    for (size_t r = cullBegin; r < n; ++r)
    {
        DrawCandidate c = candidates[r];

        // Transparent and water materials never take the off-screen drop / shadow
        // conversion below (only the disposable-transparent drop); Masked counts as opaque
        // (IsTransparentBlendMode excludes it).
        const bool isTransparent =
            matBit(c, MB_TRANSPARENT,
                    [&] { return c.mc && IsTransparentBlendMode(c.mc->userBlendMode); },
                    "TRANSPARENT")
         || matBit(c, MB_WATER,
                    [&] { return c.mc && c.mc->IsWaterShadingModel(); }, "WATER");

        // Visibility: the BVH masks when GPU culling is on and the BVH holds the entity as a
        // leaf, else the entity's own WorldAabb (skinned entities are not BVH leaves; theirs
        // is the animated bound from TickAnimationChain's bone merge). No AABB means no cull.
        bool visible = true;
        WorldAabb vBox;
        const WorldAabb* vAabb = boxOf(c, vBox);
        if (in.gpuCulling && inMask(bvhTestedMask, c.entity))
            visible = inMask(bvhVisibleMask, c.entity);
        else if (vAabb)
            visible = mainVisible(c.entity, *vAabb);


        // Same disposable-transparent drop as the fast path above: the only transparent
        // cull in the pipeline (transparents never reach the cull CS). Non-disposable
        // transparents fall through to the depth refresh below.
        if (!visible && isDisposableTransparent(c))
        {
            // Last chance to keep a copy for a secondary view.
            if (vAabb) NoteSecondaryViewCandidate(c, vAabb->min, vAabb->max);
            continue;
        }

        // Off-screen opaque: convert to a shadow-only packet or drop. Never with
        // shadowArgsFrameActive (that frame took the fast path above): there static
        // packets keep their natural filter, so the packet set does not depend on the
        // camera - main-view visibility is the per-instance cull CS and shadow visibility
        // the per-cascade cull views. That also lets off-screen casters reach spot / point
        // / probe views and keep their authored ShadowCullMode bucket (the conversion below
        // forces bucket 0).
        if (!visible && !isTransparent && !in.shadowArgsFrameActive)
        {
            // Once, at the top of the block: everything below either drops `c` or rewrites it
            // into a lossy Shadow packet, so this is the last point where it still carries the
            // permutation, stencil ref and depth a shaded secondary view needs; one call
            // covers all five exits. An outline shell (no shadow form) is offered too, then
            // dropped - the editor's picking outline is an editor overlay no secondary view draws.
            if (vAabb && !(c.filter == DrawFilter::Custom && c.isPickingOutline))
                NoteSecondaryViewCandidate(c, vAabb->min, vAabb->max);
            // The shadow fallback needs a valid shadow frustum, both cast-shadow layers
            // (ApplyBlendMode already AND-ed the material bit into c.castShadow) and an AABB
            // inside the frustum.
            if (c.filter == DrawFilter::Custom) continue;
            if (!ShadowRetentionValid())        continue;
            if (!c.castShadow)                  continue;
            // Same entity, same box as vAabb.
            const WorldAabb* aabb = vAabb;
            if (!aabb) continue;
            BoundingBox bb;
            bb.Center  = { (aabb->min.x + aabb->max.x) * 0.5f,
                           (aabb->min.y + aabb->max.y) * 0.5f,
                           (aabb->min.z + aabb->max.z) * 0.5f };
            bb.Extents = { (aabb->max.x - aabb->min.x) * 0.5f,
                           (aabb->max.y - aabb->min.y) * 0.5f,
                           (aabb->max.z - aabb->min.z) * 0.5f };
            if (!shadowVisible(c.entity, bb)) continue;
            // Shadow-only conversion: reset what ApplyBlendMode derived (perm, stencil, cull
            // mode, depth) to the DrawCandidate defaults, which is what the in-gather conversion
            // leaves (it skips ApplyBlendMode), so ShadowPass bucketing and batch keys match
            // between the two paths.
            c.filter         = DrawFilter::Shadow;
            c.perm           = {};
            // Main keeps its unchecked live read; the lane has the bit.
            if (live ? (c.mc && c.mc->IsAlphaTestEnabled())
                     : (c.matBits & MB_ALPHA_TEST) != 0)
                c.perm.Set(PermutationKey::ALPHA_TEST, true);
            c.stencilRef     = 1;
            c.shadowCullMode = 0;
            c.depth          = 0.0f;
        }
        else if (IsDepthSortedFilter(c.filter))
        {
            refreshDepth(c);
        }
        candidates[w++] = c;
    }
    candidates.resize(w);
}

// The classify on the render-owner lane, called from PrepareFrameOnLane before the
// plan. It reads only candidate fields, m_laneClassifyIn (captured by main) and
// state whose next writer is past the drain that joins this lane (m_shadowFrustum and the
// secondary views' m_viewShadowFrustum, the BVH masks, the secondary-view frusta).
void Renderer::RunLaneClassify(std::vector<DrawCandidate>& candidates)
{
    ZoneScopedN("BRS.Classify.Lane");
    PROF_SCOPE("BRS.Classify.Lane");
    BuildScene_ClassifyCandidates(m_laneClassifyIn, nullptr, candidates);
    // Main's order: secondary duplicates, then the procedural rows.
    if (!m_secondaryCands.empty())
        candidates.insert(candidates.end(), m_secondaryCands.begin(), m_secondaryCands.end());
    candidates.insert(candidates.end(), m_proceduralScratch.begin(), m_proceduralScratch.end());
    ++m_laneClassifyFrames;
#if WITH_HARNESS
    if (!m_laneClassifyRefValid) return;
    m_laneClassifyRefValid = false;
    const std::vector<DrawCandidate>& ref = m_laneClassifyRef;
    const char* firstField = "";
    size_t firstRow = 0;
    const uint32_t mismatches = DiffCandidateLists(candidates, ref, firstField, firstRow);
    if (mismatches) ++m_laneClassifyMismatchFrames;
    ENGINE_CHECK_ONCE(mismatches == 0,
        "[LANECLASSIFY_ORACLE] the lane classify differs from main's: %u field "
        "mismatches, first %s at row %zu (rows lane=%zu main=%zu)",
        mismatches, firstField, firstRow, candidates.size(), ref.size());
    if (mismatches || m_laneClassifyFrames % 60 == 0)
        LOG_INFO("[LANECLASSIFY_ORACLE] frame=%llu rows=%zu refRows=%zu mismatches=%u "
                 "first=%s@%zu secondary=%zu aabbStale=%u bitsStale=%u badFrames=%llu match=%u",
                 static_cast<unsigned long long>(m_laneClassifyFrames),
                 candidates.size(), ref.size(), mismatches, firstField, firstRow,
                 m_secondaryCands.size(), m_laneClassifyAabbStale, m_laneClassifyBitsStale,
                 static_cast<unsigned long long>(m_laneClassifyMismatchFrames),
                 mismatches == 0 ? 1u : 0u);
#endif
}

// -laneclassifyverify's reference, built on main: the MAIN form on a copy (live
// WorldAabb lookups, MatBitCheck against *mc) plus the same appends. The census
// counts rows whose box / predicate disagrees with the pool right now: a box that
// changed without a transform change (which the retained cache assumes cannot
// happen) shows up here by name before it shows up as a row mismatch.
void Renderer::BuildLaneClassifyReference(World& world,
        const std::vector<DrawCandidate>& candidates,
        const std::vector<DrawCandidate>& procedural)
{
    ZoneScopedN("BRS.Classify.LaneOracle");
    m_laneClassifyRef.assign(candidates.begin(), candidates.end());
    BuildScene_ClassifyCandidates(m_laneClassifyIn, &world, m_laneClassifyRef);
    if (!m_secondaryCands.empty())
        m_laneClassifyRef.insert(m_laneClassifyRef.end(),
                                 m_secondaryCands.begin(), m_secondaryCands.end());
    m_laneClassifyRef.insert(m_laneClassifyRef.end(), procedural.begin(), procedural.end());
    m_laneClassifyRefValid = true;

    const auto* pAabb = world.GetPool<WorldAabb>();
    uint32_t aabbStale = 0, bitsStale = 0;
    for (size_t i = m_laneClassifyIn.cullBegin; i < candidates.size(); ++i)
    {
        const DrawCandidate& c = candidates[i];
        const WorldAabb* a = pAabb ? pAabb->Get(c.entity) : nullptr;
        const bool sameBox = a
            ? (c.hasAabb && std::memcmp(&a->min, &c.aabbMin, sizeof(XMFLOAT3)) == 0
                         && std::memcmp(&a->max, &c.aabbMax, sizeof(XMFLOAT3)) == 0)
            : !c.hasAabb;
        if (!sameBox) ++aabbStale;
        if (c.matBits != ComputeMatBits(c.mc)) ++bitsStale;
    }
    m_laneClassifyAabbStale = aabbStale;
    m_laneClassifyBitsStale = bitsStale;
}

// Snapshot the static lane of this frame's pre-classification candidates
// (IsStaticLaneInstance: tracked in the scene store with lane 0, no skin / dissolve
// component, no skinned prev-pos ring, no live dissolve params); everything else
// re-gathers every frame. Rows keep gather order, so an entity's base + outline
// copies stay adjacent (PatchCandidateCache relies on that).
void Renderer::SnapshotCandidateCache(const std::vector<DrawCandidate>& candidates,
                                      size_t primEnd, uint64_t setSig)
{
    ZoneScopedN("BS.CandSnapshot");
    static_assert(std::is_trivially_copyable_v<DrawCandidate>,
                  "candidate cache requires a trivially-copyable DrawCandidate");
    // hasAabb and the ninth matBits bit sit in what would be padding before matSnap,
    // orderKey in the padding after worldMatrix. The candidate arrays are the hottest
    // memory in the scene build: growing the row is a decision, not a side effect.
    static_assert(sizeof(DrawCandidate) == 248 && offsetof(DrawCandidate, matSnap) == 92 &&
                  offsetof(DrawCandidate, orderKey) == 76,
                  "DrawCandidate changed size / layout");
    m_candCache.clear();
    m_candCache.reserve(candidates.size());
    m_candEntityToIdx.assign(m_sceneStore.keys.size(), 0xFFFFFFFFu);
    m_candCachePrimCount = 0;
    // Where each cached row sits in THIS frame's candidate list (so the snapshot
    // acquisition's result can be written back once it has run), and the rows' share
    // of the scene triangle total.
    m_candCacheSrc.clear();
    m_candCacheTris = 0;

    // One definition of "static lane", shared with the retained emit.
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        const DrawCandidate& c = candidates[i];
        if (!IsStaticLaneInstance(c)) continue;
        const uint32_t id = static_cast<uint32_t>(c.entity);
        if (m_candEntityToIdx[id] == 0xFFFFFFFFu)
            m_candEntityToIdx[id] = static_cast<uint32_t>(m_candCache.size());
        m_candCache.push_back(c);
        m_candCacheSrc.push_back(static_cast<uint32_t>(i));
        if (c.filter != DrawFilter::Custom)   // same rule as the triScene loop
            m_candCacheTris += uint64_t(c.indexCount / 3u);
        if (i < primEnd) ++m_candCachePrimCount;
    }
    m_candCacheWriteBack = true;    // after this frame's mirror + snapshots
    m_candCacheRetained  = false;   // until then the rows carry neither
    // PrepareLanePlan's atlas prediction for the cached rows, which main may not hand
    // over itself (-laneassemble).
    m_candCacheAtlasEligible = std::any_of(m_candCache.begin(), m_candCache.end(),
                                           &Renderer::SurfaceAtlasEligible);
    m_candCacheSetSig     = setSig;
    m_candCacheStaticSig  = ComputeCandidateStaticSetSig();   // over the map just built
    m_candCachePickEntity = m_pickingOutlineEntity;
    // Arm only when the snapshot can be patched forward: while a dissolve is live,
    // subtree entities carry per-frame params the diff cannot see, so keep rebuilding
    // until it ends (the gate also requires !m_hasDissolve).
    m_candCacheValid = !m_hasDissolve;
}

// ==== -brssplit: the static half of the scene build, published early ====
// The retained candidate cache is the static half: its per-frame upkeep is
// camera-independent, so worker lanes can run it before BuildRenderScene
// (contract: the m_brsEarly* block in Renderer.h).

// One definition of the mirror bit, shared by the early lanes, the patch fixup and
// the serial mirror loop.
uint8_t Renderer::MirrorBitFor(const DirectX::XMFLOAT4X4& m)
{
    const float det3 =
          m._11 * (m._22 * m._33 - m._23 * m._32)
        - m._12 * (m._21 * m._33 - m._23 * m._31)
        + m._13 * (m._21 * m._32 - m._22 * m._31);
    return (det3 < 0.0f) ? 1u : 0u;
}

bool Renderer::BrsSplitEnabled()
{
    static const bool s_on = Sys::CommandLine::Get().Has("-brssplit");
    return s_on;
}

// One lane of the static refresh, shared by the early lanes and
// PatchCandidateCache so the two cannot drift.
void Renderer::RefreshCandCacheRange(const ComponentPool<MaterialComponent>* pMaterial,
                                     size_t begin, size_t end, bool writeUnchanged,
                                     bool doMirror, bool& valid,
                                     uint32_t& ptrWrites, uint32_t& bitWrites)
{
    for (size_t i = begin; i < end; ++i)
    {
        auto& c = m_candCache[i];
        const auto* fresh = pMaterial ? pMaterial->Get(c.entity) : nullptr;
        if ((fresh != nullptr) != (c.mc != nullptr)) { valid = false; continue; }
        const uint16_t bits = ComputeMatBits(fresh);
        // Do not dirty cache lines just to restore the same pointer / bits; the
        // predicates are still recomputed.
        if (writeUnchanged || c.mc != fresh) { c.mc = fresh; ++ptrWrites; }
        if (writeUnchanged || c.matBits != bits) { c.matBits = bits; ++bitWrites; }
        if (doMirror) c.mirrored = MirrorBitFor(c.worldMatrix);
    }
}

// Publish point: App calls it immediately before TickPhase::SecondaryPhysics and
// joins right after that phase. The join must precede every writer of what the lanes
// read (PreRender's FaceShading writes MaterialComponent content). Chain physics runs
// in TickPhase::Animation and SecondaryPhysics is empty, so the owner has almost
// nothing to overlap and returns at once.
// TODO(Frame_Job_Graph_Plan M4.3): a publish window with owner work to overlap
// (cross-phase overlap); this one overlaps ~0 ms.
void Renderer::PrepareStaticSceneEarly(World& world)
{
    if (!BrsSplitEnabled()) return;
    if (m_brsEarlyPending) return;          // a join was skipped; do not stack
    m_brsEarlyResultLive = false;
    m_brsEarlyReady      = false;
    // The patch arm's preconditions that are known here, tested early so a frame that
    // is known to full-gather pays for no lanes. m_hasDissolve is the previous frame's
    // value at this point; the patch arm re-tests the current one.
    if (!m_retainedCandidates || !m_retainedSceneDiff || !m_candCacheValid ||
        m_candCache.empty() || m_hasDissolve ||
        TaskSystem::Get().GetWorkerCount() < 2)
        return;
#if WITH_HARNESS
    // The candidate-patch A/B knobs rewrite the same rows with a different
    // policy; running both would attribute one path's cost to the other.
    const auto& patchCmd = Sys::CommandLine::Get();
    if (patchCmd.Has("-candpatchwrite") || patchCmd.Has("-candpatchserial") ||
        patchCmd.GetInt("-candpatchab", 0) != 0)
        return;
#endif
    const auto* pMaterial = world.GetPool<MaterialComponent>();
    // Pool guard: any Add or Erase moves at least one of these three (Erase is
    // swap-with-back, which pops both vectors). Re-checked at consume time.
    m_brsEarlyMatData   = pMaterial ? static_cast<const void*>(pMaterial->Data().data()) : nullptr;
    m_brsEarlyMatDense  = pMaterial ? static_cast<const void*>(pMaterial->Entities().data()) : nullptr;
    m_brsEarlyMatSize   = pMaterial ? pMaterial->Size() : size_t(0);
    m_brsEarlyCacheSize = m_candCache.size();
    m_brsEarlyLanes     = (std::min)(kBrsEarlyLanes, TaskSystem::Get().GetWorkerCount());
    m_brsEarlyLaneResults = {};
    ++m_brsEarlyAttempts;

    m_brsEarlyScope.emplace(m_brsEarlyLanes);
    const size_t count = m_candCache.size();
    const uint32_t lanes = m_brsEarlyLanes;
    for (uint32_t lane = 0; lane < lanes; ++lane)
    {
        // Every lane owns a contiguous, disjoint range of m_candCache and one
        // result slot. The ECS pools are read-only until the join; nothing in
        // the phases this overlaps writes MaterialComponent storage.
        m_brsEarlyScope->Submit([this, pMaterial, lane, lanes, count] {
            ZoneScopedN("BS.Split.StaticPrep");
            auto& r = m_brsEarlyLaneResults[lane];
            RefreshCandCacheRange(pMaterial, count * lane / lanes,
                                  count * (lane + 1) / lanes,
                                  /*writeUnchanged=*/false, /*doMirror=*/true,
                                  r.valid, r.ptrWrites, r.bitWrites);
        });
    }
    m_brsEarlyPending = true;
}

// Join point. Called by App right after TickPhase::SecondaryPhysics, and again
// (idempotently) at the top of BuildRenderScene and of OnWorldClear as a safety
// net: nothing may touch m_candCache while the lanes hold it.
void Renderer::JoinStaticSceneEarly()
{
    if (!m_brsEarlyPending)
    {
        if (m_brsEarlyScope) m_brsEarlyScope.reset();
        return;
    }
    ZoneScopedN("BS.Split.Merge");
    m_brsEarlyPending = false;
    if (m_brsEarlyScope)
    {
        try { m_brsEarlyScope->Join(); }
        catch (...) { m_brsEarlyScope.reset(); m_brsEarlyResultLive = false; throw; }
        m_brsEarlyScope.reset();
    }
    bool valid = true;
    for (uint32_t lane = 0; lane < m_brsEarlyLanes; ++lane)
        valid = valid && m_brsEarlyLaneResults[lane].valid;
    m_brsEarlyResultLive = valid;   // a null-ness flip means the caller rebuilds
}

// Called inside PatchCandidateCache. Anything that moved the MaterialComponent pool
// since the publish makes the lanes' pointers untrustworthy; the serial walk runs
// instead.
bool Renderer::ConsumeStaticSceneEarly(const ComponentPool<MaterialComponent>* pMaterial)
{
    // Cleared first, on every route out: BuildRenderScene reads m_brsEarlyReady again
    // (brsSplit) to decide that the cached rows already carry their mirror bits. A
    // stale `true` from an earlier call (a second build in one frame, or a throw that
    // skipped the clear at its end) would skip the mirror recompute on a build that
    // took the serial walk.
    m_brsEarlyReady = false;
    if (!m_brsEarlyResultLive) return false;
    m_brsEarlyResultLive = false;   // one frame only, whatever the answer
    const void* data  = pMaterial ? static_cast<const void*>(pMaterial->Data().data()) : nullptr;
    const void* dense = pMaterial ? static_cast<const void*>(pMaterial->Entities().data()) : nullptr;
    const size_t size = pMaterial ? pMaterial->Size() : size_t(0);
    if (data != m_brsEarlyMatData || dense != m_brsEarlyMatDense ||
        size != m_brsEarlyMatSize || m_candCache.size() != m_brsEarlyCacheSize)
    {
        ++m_brsEarlyRejected;
        return false;
    }
    ++m_brsEarlyUsed;
    m_brsEarlyReady = true;
    return true;
}

// Patch-frame upkeep of the cache. mc is re-resolved because the
// MaterialComponent pool can reallocate / compact with no tracked membership event
// (adds on untracked entities). A null-ness flip means the baked classification
// (blend mode / perm / matHash) is stale -> false, and the caller runs the full
// gather.
bool Renderer::PatchCandidateCache(World& world)
{
    const auto* pMaterial  = world.GetPool<MaterialComponent>();
    auto* pGlobalXf  = world.GetPool<GlobalTransform>();
    auto* pWorldAabb = world.GetPool<WorldAabb>();

    // -brssplit: this whole walk may already have run on workers.
    // ConsumeStaticSceneEarly re-tests the pool guard and returns false the moment
    // anything moved; the serial walk below then runs.
    const bool useEarly = ConsumeStaticSceneEarly(pMaterial);

    // The re-resolve exists because the pool can move its storage (growth,
    // swap-with-back erase) with no tracked event. The same pool object at the same
    // StructureVersion() as when the pointers were taken means nothing moved, appeared
    // or vanished: skip the walk. In-place content edits are the SetDirty contract
    // (matDirty sends the frame to the full gather), the one the cached matHash relies
    // on. -candpatchverify still checks every row against the pool every frame.
    const bool poolUnchanged = !m_candRetainOffThisFrame && !useEarly &&
        static_cast<const void*>(pMaterial) == m_candCacheMatPool &&
        (!pMaterial || pMaterial->StructureVersion() == m_candCacheMatPoolVersion);
    if (poolUnchanged) ++m_candMatRefreshSkips;

    if (!useEarly && !poolUnchanged)
    {
        ZoneScopedN("BS.CandPatch.Materials");
        bool parallel = (Sys::JobSplittingDefaultsEnabled() || m_candCache.size() >= 2048) && TaskSystem::Get().GetWorkerCount() >= 2;
        bool writeUnchanged = false, verify = false;
#if WITH_HARNESS
        const auto& cmd = Sys::CommandLine::Get();
        if (cmd.Has("-candpatchjobs")) parallel = TaskSystem::Get().GetWorkerCount() >= 2;
        parallel = parallel && !cmd.Has("-candpatchserial");
        writeUnchanged = cmd.Has("-candpatchwrite");
        verify = cmd.Has("-candpatchverify");
        const int alternate = cmd.GetInt("-candpatchab", 0);
        static uint64_t s_abFrame = 0;
        if (alternate != 0) {
            parallel = parallel && ((++s_abFrame + alternate) % 2 == 0);
            writeUnchanged = !parallel;
        }
#endif
#ifdef TRACY_ENABLE
        ZoneTransientN(patchModeZone, parallel ? "BS.CandPatch.ParallelMaterials"
            : "BS.CandPatch.SerialMaterials", true);
#endif
        struct PatchResult { bool valid = true; uint32_t pointerWrites = 0, bitWrites = 0; };
        std::array<PatchResult, 4> results{};
        const uint32_t lanes = parallel ? (std::min)(4u, TaskSystem::Get().GetWorkerCount()) : 1u;
        // Only this build owns the cache; the ECS pools are read-only until
        // the join. Each lane owns a contiguous candidate range and one result.
        // Re-resolve even unchanged entries: pool growth/compaction can move
        // material storage without a dirty event on a tracked scene entity.
        auto patchRange = [&](uint32_t lane) {
            ZoneScopedN("BS.CandPatch.MaterialChunk");
            const size_t begin = m_candCache.size() * lane / lanes;
            const size_t end = m_candCache.size() * (lane + 1) / lanes;
            ZoneValue(end - begin);
            auto& result = results[lane];
            // Shared body (RefreshCandCacheRange). doMirror is false: a mirror bit changes
            // only with worldMatrix, and the transform fixup below recomputes those.
            RefreshCandCacheRange(pMaterial, begin, end, writeUnchanged,
                                  /*doMirror=*/false, result.valid,
                                  result.pointerWrites, result.bitWrites);
        };
        if (parallel) TaskSystem::Get().ParallelFor(0, lanes, patchRange);
        else patchRange(0);
        uint32_t pointerWrites = 0, bitWrites = 0;
        for (uint32_t lane = 0; lane < lanes; ++lane) {
            if (!results[lane].valid) return false; // caller rebuilds after all workers joined
            pointerWrites += results[lane].pointerWrites;
            bitWrites += results[lane].bitWrites;
        }
        static const int s_every = Sys::CommandLine::Get().GetInt("-phaselog_every", 0);
        static uint64_t s_patchFrame = 0;
        if (s_every > 0 && ++s_patchFrame % s_every == 0)
            LOG_INFO("[CAND_PATCH] candidates=%zu lanes=%u pointerWrites=%u bitWrites=%u verified=%u early=0",
                m_candCache.size(), lanes, pointerWrites, bitWrites, verify ? 1u : 0u);
        // The pointers are now the pool's as it stands.
        m_candCacheMatPool        = pMaterial;
        m_candCacheMatPoolVersion = pMaterial ? pMaterial->StructureVersion() : 0;
        // The re-resolve can move matBits, which the atlas prediction reads.
        if (bitWrites)
            m_candCacheAtlasEligible = std::any_of(m_candCache.begin(), m_candCache.end(),
                                                   &Renderer::SurfaceAtlasEligible);
    }
    else if (poolUnchanged)
    {
        static const int s_every = Sys::CommandLine::Get().GetInt("-phaselog_every", 0);
        static uint64_t s_skipFrame = 0;
        if (s_every > 0 && ++s_skipFrame % s_every == 0)
            LOG_INFO("[CAND_PATCH] candidates=%zu skipped=1 poolVersion=%llu skips=%llu",
                m_candCache.size(), static_cast<unsigned long long>(m_candCacheMatPoolVersion),
                static_cast<unsigned long long>(m_candMatRefreshSkips));
    }
    else
    {
        static const int s_every = Sys::CommandLine::Get().GetInt("-phaselog_every", 0);
        static uint64_t s_earlyFrame = 0;
        uint32_t pointerWrites = 0, bitWrites = 0;
        for (uint32_t lane = 0; lane < m_brsEarlyLanes; ++lane) {
            pointerWrites += m_brsEarlyLaneResults[lane].ptrWrites;
            bitWrites     += m_brsEarlyLaneResults[lane].bitWrites;
        }
        if (s_every > 0 && ++s_earlyFrame % s_every == 0)
            LOG_INFO("[CAND_PATCH] candidates=%zu lanes=%u pointerWrites=%u bitWrites=%u verified=0 early=1",
                m_candCache.size(), m_brsEarlyLanes, pointerWrites, bitWrites);
    }

    // The oracle is outside the branch on purpose: it has to be able to say the
    // early lanes produced the same answer, which is the whole point of it.
#if WITH_HARNESS
    if (Sys::CommandLine::Get().Has("-candpatchverify")) {
        ZoneScopedN("BS.CandPatch.MaterialOracle");
        for (const auto& c : m_candCache) {
            const auto* fresh = pMaterial ? pMaterial->Get(c.entity) : nullptr;
            ENGINE_CHECK(c.mc == fresh && c.matBits == ComputeMatBits(fresh),
                "candidate patch material mismatch for entity %u", static_cast<uint32_t>(c.entity));
        }
    }
#endif

    // Transform-only dirt: same sources the full gather reads (gt->matrix +
    // WorldAabb, both refreshed by TransformSystem before BuildRenderScene).
    const SceneDiffView diff = GetSceneDiffView();
    if (!diff.valid) return false;
    for (Entity e : *diff.dirtyXform)
    {
        const uint32_t id = static_cast<uint32_t>(e);
        if (id >= m_candEntityToIdx.size()) continue;
        const uint32_t idx = m_candEntityToIdx[id];
        if (idx == 0xFFFFFFFFu) continue;
        const GlobalTransform* gt = pGlobalXf ? pGlobalXf->Get(e) : nullptr;
        if (!gt) continue;
        XMFLOAT4X4 wt;
        XMStoreFloat4x4(&wt, XMMatrixTranspose(XMLoadFloat4x4(&gt->matrix)));
        // Centroid convention differs per source slice (primitive = pivot, MeshLibRef =
        // WorldAabb centre with pivot fallback): mirror both.
        const XMFLOAT3 pivot = { gt->matrix.m[3][0],
                                 gt->matrix.m[3][1],
                                 gt->matrix.m[3][2] };
        XMFLOAT3 centre = pivot;
        // Inverted default = "no box", matching the gather. Refreshed here for
        // the same reason worldCenter is: the entity moved this frame.
        XMFLOAT3 aabbMin{  1.0f, 0.0f, 0.0f };
        XMFLOAT3 aabbMax{ -1.0f, 0.0f, 0.0f };
        uint8_t  hasAabb = 0;
        if (const WorldAabb* aabb = pWorldAabb ? pWorldAabb->Get(e) : nullptr)
        {
            centre = { (aabb->min.x + aabb->max.x) * 0.5f,
                       (aabb->min.y + aabb->max.y) * 0.5f,
                       (aabb->min.z + aabb->max.z) * 0.5f };
            aabbMin = aabb->min;
            aabbMax = aabb->max;
            hasAabb = 1;
        }
        for (uint32_t k = idx;
             k < m_candCache.size() && m_candCache[k].entity == e; ++k)
        {
            m_candCache[k].worldMatrix = wt;
            m_candCache[k].worldCenter =
                (k < m_candCachePrimCount) ? pivot : centre;
            // NOT split on the prim / MeshLibRef boundary the centroid uses: the box belongs
            // to the entity, and a primitive entity has no WorldAabb, so it gets the inverted
            // default either way. Splitting would write the default over a real box on patch
            // frames where the full gather writes the real one.
            m_candCache[k].aabbMin = aabbMin;
            m_candCache[k].aabbMax = aabbMax;
            m_candCache[k].hasAabb = hasAabb;
            // The cache keeps its mirror bits between frames, and with -brssplit the early
            // lane computed this one from the previous matrix: recompute it here, the only
            // site that rewrites worldMatrix on a patch frame, or a moved mirrored entity
            // carries a stale sign into the batch key.
            m_candCache[k].mirrored = MirrorBitFor(wt);
        }
    }
    return true;
}

// The steady-state answer of ResolveCustomPSID + ResolveCustomVSID, read-only.
bool Renderer::PeekCustomShaderIDs(const MaterialComponent& m, uint32_t& ps, uint32_t& vs) const
{
    ps = vs = 0u;
    if (!m.useCustomShader) return true;             // both return 0, no side effect
    const auto it = m_runtimeMaterialShaders.find(BuildMaterialShaderKey(m));
    if (it == m_runtimeMaterialShaders.end()) return false;   // operator[] would insert
    const RuntimeMaterialShaderState& st = it->second;
    const uint32_t epoch = m_gbufferPass ? m_gbufferPass->GetShaderLibrary().GetReloadEpoch() : 0u;
    if (!m.customShaderPath.empty())
    {
        if (st.customPSID < 0 && m_gbufferPass) return false;                        // would register
        if (st.customPSID > 0 && m_gbufferPass &&
            !MaterialReflectionSync::StageSynced(m, epoch, /*vsStage*/ false)) return false; // would sync
        ps = st.customPSID > 0 ? static_cast<uint32_t>(st.customPSID) : 0u;
    }
    if (st.customVSID < 0 && m_gbufferPass) return false;                            // would resolve
    if (st.customVSID > 0 && m_gbufferPass &&
        !MaterialReflectionSync::StageSynced(m, epoch, /*vsStage*/ true)) return false;
    if ((st.customPSID > 0 || st.customVSID > 0) && m.customParamsLegacyFormat)
        return false;                                                                // would clear the flag
    vs = st.customVSID > 0 ? static_cast<uint32_t>(st.customVSID) : 0u;
    return true;
}

// ==== Retained dynamic rows (contract: the member block in Renderer.h) ====
bool Renderer::DynRetainedEnabled()
{
    static const bool s_on = !Sys::CommandLine::Get().Has("-dynretainedoff");
    return s_on;
}

// Exactly the rows BuildScene_GatherMeshLibRefs(dynamicOnly) produces on a patch
// frame: those of entities the static cache does not hold (the gather's own skip
// test, on the m_candEntityToIdx SnapshotCandidateCache just built), in gather
// order. On a fallback frame every row from `from` on passes the test.
void Renderer::SnapshotDynCandCache(const std::vector<DrawCandidate>& candidates, size_t from)
{
    ZoneScopedN("BS.DynSnapshot");
    m_dynCandCache.clear();
    // ARMED ONLY WHEN SETTLED. A row whose material still has custom-shader work due
    // (a registration, a reflection sync after a reload, clearing
    // customParamsLegacyFormat) is one the gather would touch again next frame, so the
    // cache stays unarmed and the next patch frame gathers until nothing is pending.
    bool settled = true;
    for (size_t i = from; i < candidates.size(); ++i)
    {
        const DrawCandidate& c = candidates[i];
        const uint32_t id = static_cast<uint32_t>(c.entity);
        if (id < m_candEntityToIdx.size() && m_candEntityToIdx[id] != 0xFFFFFFFFu) continue;
        if (settled && c.mc && c.mc->useCustomShader)
        {
            uint32_t ps = 0, vs = 0;
            settled = PeekCustomShaderIDs(*c.mc, ps, vs) &&
                      ps == c.customPSID && vs == c.customVSID;
        }
        m_dynCandCache.push_back(c);
    }
    m_dynCandCacheMeshGen     = m_meshMgr.GetMeshLibDescGeneration();
    m_dynCandCacheShaderEpoch = m_gbufferPass ? m_gbufferPass->GetShaderLibrary().GetReloadEpoch() : 0u;
    m_dynCandCacheValid = settled;
}

// One lane's share of the dynamic-row patch: rows [lo, hi) are produced from their
// retained copy plus this frame's World state. Every write mirrors the gather's
// (processJob + ApplyBlendMode) expression for that field, so a row equals what
// the gather would build this frame. `src` non-null: copy the row from there first
// (the async jobs fuse the copy with the patch). A failure stops the lane and
// names the reason in r.fail; the caller falls back to the gather.
void Renderer::PatchDynRows(const DynPatchPools& pools, DrawCandidate* rows,
                            const DrawCandidate* src, size_t lo, size_t hi,
                            const XMFLOAT3& camPos, const XMFLOAT3& camForward,
                            DynPatchLaneResult& r) const
{
    ZoneScopedN("BS.DynPatch.Chunk");
    const auto* pGlobalXf  = pools.gt;
    const auto* pWorldAabb = pools.aabb;
    const auto* pSkinned   = pools.skinned;
    const auto* pSkinOut   = pools.skinOut;
    const auto* pMaterial  = pools.material;
    for (size_t i = lo; i < hi; ++i)
    {
        DrawCandidate& c = rows[i];
        if (src) c = src[i];
        const Entity e = c.entity;
        const GlobalTransform* gt = pGlobalXf ? pGlobalXf->Get(e) : nullptr;
        if (!gt) { r.fail = 1; return; }
        // The gather's skinningActive, verbatim: it picks the mesh slot, the
        // index count, `skinned` and whether prev-pos is read at all.
        const MeshSkinnedComponent*    sk = pSkinned ? pSkinned->Get(e) : nullptr;
        const SkinningOutputComponent* so = pSkinOut ? pSkinOut->Get(e) : nullptr;
        const bool active = sk && so && so->poseByteOffset != ~0u
            && sk->meshDescriptorIdx != RHI::kInvalidBufferIndex
            && sk->blendSRVHandle && sk->restPosSRVHandle && sk->restNrmSRVHandle;
        if (active != (c.skinned != 0)) { r.fail = 2; return; }
        if (active && (c.meshDescSlot != sk->meshDescriptorIdx ||
                       c.indexCount   != sk->indexCount))   { r.fail = 3; return; }
        c.prevPosElementBase = active ? so->prevPosElementBase : 0xFFFFFFFFu;

        const MaterialComponent* mc = pMaterial ? pMaterial->Get(e) : nullptr;
        if ((mc != nullptr) != (c.mc != nullptr)) { r.fail = 4; return; }
        const uint16_t bits = ComputeMatBits(mc);
        if (bits != c.matBits) { r.fail = 5; return; }
        c.mc = mc;
        if (mc && (bits & MB_FRAME_PARAMS))
        {
            const ResolvedMaterialRuntime rt = ResolveMaterialRuntime(e);
            c.texBaseColor  = rt.EngineTex(MaterialComponent::BASECOLORMAP).gpuHandle;
            c.texSurfaceMap = rt.EngineTex(MaterialComponent::SURFACEMAP).gpuHandle;
            c.texNormalMap  = rt.EngineTex(MaterialComponent::NORMALMAP).gpuHandle;
            c.matHash       = HashMatKeys(mc, rt, c.orderKey);
            ++r.rehash;
        }

        XMStoreFloat4x4(&c.worldMatrix,
            XMMatrixTranspose(XMLoadFloat4x4(&gt->matrix)));
        if (const WorldAabb* aabb = pWorldAabb ? pWorldAabb->Get(e) : nullptr)
        {
            c.worldCenter = { (aabb->min.x + aabb->max.x) * 0.5f,
                              (aabb->min.y + aabb->max.y) * 0.5f,
                              (aabb->min.z + aabb->max.z) * 0.5f };
            c.aabbMin = aabb->min;
            c.aabbMax = aabb->max;
            c.hasAabb = 1;
        }
        else
        {
            c.worldCenter = { gt->matrix.m[3][0], gt->matrix.m[3][1], gt->matrix.m[3][2] };
            c.aabbMin = {  1.0f, 0.0f, 0.0f };   // DrawCandidate's "no box" default
            c.aabbMax = { -1.0f, 0.0f, 0.0f };
            c.hasAabb = 0;
        }
        // ApplyBlendMode's view depth (it returns before this without a
        // material, leaving the default - which the copy still holds).
        if (c.mc)
        {
            const float dx = c.worldCenter.x - camPos.x;
            const float dy = c.worldCenter.y - camPos.y;
            const float dz = c.worldCenter.z - camPos.z;
            c.depth = dx * camForward.x
                    + dy * camForward.y
                    + dz * camForward.z;
        }
    }
}

// The patch's lane count - one rule for the synchronous and the job forms.
static uint32_t DynPatchLanes(size_t n)
{
    constexpr uint32_t kMaxLanes = 32;
    const uint32_t workers = TaskSystem::Get().GetWorkerCount();
    return (n >= 512 && workers >= 2)
        ? (std::min)({ workers, kMaxLanes, static_cast<uint32_t>(n / 256) }) : 1u;
}

// Appends the cached rows, then produces each one from its copy plus this
// frame's World state, on worker lanes (PatchDynRows). false = something
// structural moved: the appended rows are removed again and the caller runs
// the gather.
bool Renderer::PatchDynCandCache(World& world, std::vector<DrawCandidate>& candidates,
                                 const XMFLOAT3& camForward)
{
    // What the rows were resolved against moved: the gather would re-take
    // slots / re-sync reflection this frame.
    const uint32_t shaderEpoch = m_gbufferPass ? m_gbufferPass->GetShaderLibrary().GetReloadEpoch() : 0u;
    if (m_meshMgr.GetMeshLibDescGeneration() != m_dynCandCacheMeshGen ||
        shaderEpoch != m_dynCandCacheShaderEpoch)
    {
        m_dynCandCacheValid = false;
        m_dynPatchLastFail = 6;
        ++m_dynPatchFallbacks;
        return false;
    }
    const size_t n = m_dynCandCache.size();
    const size_t base = candidates.size();
    if (n == 0) { ++m_dynPatchFrames; return true; }
    candidates.insert(candidates.end(), m_dynCandCache.begin(), m_dynCandCache.end());
    DrawCandidate* const rows = candidates.data() + base;

    DynPatchPools pools;
    pools.gt       = world.GetPool<GlobalTransform>();
    pools.aabb     = world.GetPool<WorldAabb>();
    pools.skinned  = world.GetPool<MeshSkinnedComponent>();
    pools.skinOut  = world.GetPool<SkinningOutputComponent>();
    pools.material = world.GetPool<MaterialComponent>();
    const XMFLOAT3 camPos = FrameViewWrite().camera.position;

    std::array<DynPatchLaneResult, 32> results{};
    const uint32_t lanes = DynPatchLanes(n);
    auto patchRange = [&](uint32_t lane)
    {
        PatchDynRows(pools, rows, nullptr, n * lane / lanes, n * (lane + 1) / lanes,
                     camPos, camForward, results[lane]);
    };
    if (lanes > 1) TaskSystem::Get().ParallelFor(0, lanes, patchRange);
    else patchRange(0);

    uint32_t fail = 0, rehash = 0;
    for (uint32_t lane = 0; lane < lanes; ++lane)
    {
        if (results[lane].fail && !fail) fail = results[lane].fail;
        rehash += results[lane].rehash;
    }
    if (fail)
    {
        candidates.resize(base);
        m_dynCandCacheValid = false;
        m_dynPatchLastFail = fail;
        ++m_dynPatchFallbacks;
        return false;
    }
    ++m_dynPatchFrames;
    m_dynPatchRows   += n;
    m_dynPatchRehash += rehash;
    static const int s_every = Sys::CommandLine::Get().GetInt("-phaselog_every", 0);
    if (s_every > 0 && m_dynPatchFrames % static_cast<uint64_t>(s_every) == 0)
        LOG_INFO("[DYN_RETAINED] rows=%zu lanes=%u rehash=%u patchFrames=%llu fallbacks=%llu lastFail=%u",
                 n, lanes, rehash,
                 static_cast<unsigned long long>(m_dynPatchFrames),
                 static_cast<unsigned long long>(m_dynPatchFallbacks), m_dynPatchLastFail);
    return true;
}

// The same patch as worker jobs, kicked right after SyncMaterials. false = not
// kicked (nothing to do, or the generations already disagree; the synchronous
// call at the patch point then records the fallback).
bool Renderer::KickDynPatchAsync(World& world, const XMFLOAT3& camForward)
{
    m_dynAsyncKicked = false;
    const uint32_t shaderEpoch = m_gbufferPass ? m_gbufferPass->GetShaderLibrary().GetReloadEpoch() : 0u;
    const uint32_t meshGen = m_meshMgr.GetMeshLibDescGeneration();
    if (meshGen != m_dynCandCacheMeshGen || shaderEpoch != m_dynCandCacheShaderEpoch) return false;
    const size_t n = m_dynCandCache.size();
    if (n == 0) return false;
    // Keeps its size across frames: only a size change (a row count change)
    // pays the element initialisation; the jobs overwrite every row.
    if (m_dynAsyncRows.size() != n) m_dynAsyncRows.resize(n);

    DynPatchPools pools;
    pools.gt       = world.GetPool<GlobalTransform>();
    pools.aabb     = world.GetPool<WorldAabb>();
    pools.skinned  = world.GetPool<MeshSkinnedComponent>();
    pools.skinOut  = world.GetPool<SkinningOutputComponent>();
    pools.material = world.GetPool<MaterialComponent>();
    const XMFLOAT3 camPos = FrameViewWrite().camera.position;

    // At most 8 lanes by default (-dynpatchasynclanes=N; the synchronous form takes up
    // to 32): the jobs must finish inside main's window, and every lane beyond that
    // only competes with main's own work there. 4 lanes left main waiting ~0.17 ms at
    // the join; 8 joined in <0.01 ms, as fast as the synchronous rule's 14.
    static const int s_asyncLanes = Sys::CommandLine::Get().GetInt("-dynpatchasynclanes", 8);
    const uint32_t lanes = (std::max)(1u, (std::min)(DynPatchLanes(n),
                                                     static_cast<uint32_t>((std::max)(1, s_asyncLanes))));
    m_dynAsyncLanes = lanes;
    m_dynAsyncMeshGen = meshGen;
    m_dynAsyncShaderEpoch = shaderEpoch;
    for (uint32_t lane = 0; lane < lanes; ++lane) m_dynAsyncResults[lane] = {};
    DrawCandidate* const rows = m_dynAsyncRows.data();
    const DrawCandidate* const src = m_dynCandCache.data();
    m_dynAsyncScope.emplace(lanes);
    for (uint32_t lane = 0; lane < lanes; ++lane)
        m_dynAsyncScope->Submit([this, pools, rows, src, n, lane, lanes, camPos, camForward] {
            PatchDynRows(pools, rows, src, n * lane / lanes, n * (lane + 1) / lanes,
                         camPos, camForward, m_dynAsyncResults[lane]);
        });
    m_dynAsyncKicked = true;
    return true;
}

bool Renderer::JoinDynPatchAsync(std::vector<DrawCandidate>& candidates, bool use)
{
    if (!m_dynAsyncKicked) return false;
    m_dynAsyncKicked = false;
    if (m_dynAsyncScope)
    {
        ZoneScopedN("BS.DynPatch.Join");
        PROF_SCOPE("BRS.DynPatch.Join");
        try { m_dynAsyncScope->Join(); }
        catch (...) { m_dynAsyncScope.reset(); throw; }
        m_dynAsyncScope.reset();
    }
    if (!use) { ++m_dynAsyncDiscards; return false; }
    // What the synchronous patch tests up front, tested at the join too: the
    // rows were resolved against the generations of the KICK.
    const uint32_t shaderEpoch = m_gbufferPass ? m_gbufferPass->GetShaderLibrary().GetReloadEpoch() : 0u;
    if (m_meshMgr.GetMeshLibDescGeneration() != m_dynAsyncMeshGen ||
        shaderEpoch != m_dynAsyncShaderEpoch ||
        m_dynAsyncMeshGen != m_dynCandCacheMeshGen || m_dynAsyncShaderEpoch != m_dynCandCacheShaderEpoch)
    {
        m_dynCandCacheValid = false;
        m_dynPatchLastFail = 6;
        ++m_dynPatchFallbacks;
        return false;
    }
    const size_t n = m_dynAsyncRows.size();
    uint32_t fail = 0, rehash = 0;
    for (uint32_t lane = 0; lane < m_dynAsyncLanes; ++lane)
    {
        if (m_dynAsyncResults[lane].fail && !fail) fail = m_dynAsyncResults[lane].fail;
        rehash += m_dynAsyncResults[lane].rehash;
    }
    if (fail)
    {
        m_dynCandCacheValid = false;
        m_dynPatchLastFail = fail;
        ++m_dynPatchFallbacks;
        return false;
    }
    candidates.insert(candidates.end(), m_dynAsyncRows.begin(), m_dynAsyncRows.end());
    ++m_dynPatchFrames;
    ++m_dynAsyncFrames;
    m_dynPatchRows   += n;
    m_dynPatchRehash += rehash;
    static const int s_every = Sys::CommandLine::Get().GetInt("-phaselog_every", 0);
    if (s_every > 0 && m_dynPatchFrames % static_cast<uint64_t>(s_every) == 0)
        LOG_INFO("[DYN_RETAINED] rows=%zu lanes=%u rehash=%u patchFrames=%llu fallbacks=%llu lastFail=%u "
                 "async=1 asyncFrames=%llu discards=%llu",
                 n, m_dynAsyncLanes, rehash,
                 static_cast<unsigned long long>(m_dynPatchFrames),
                 static_cast<unsigned long long>(m_dynPatchFallbacks), m_dynPatchLastFail,
                 static_cast<unsigned long long>(m_dynAsyncFrames),
                 static_cast<unsigned long long>(m_dynAsyncDiscards));
    return true;
}

#if WITH_HARNESS
// -dynretainedverify: the patched rows against what the dynamicOnly gather
// builds THIS frame, field by field (the struct has padding, so memcmp of a
// whole row would compare indeterminate bytes). Floats bitwise.
void Renderer::VerifyDynCandCache(World& world, const std::vector<DrawCandidate>& candidates,
                                  size_t from, const XMFLOAT3& camForward)
{
    ZoneScopedN("BS.DynPatch.Oracle");
    thread_local std::vector<DrawCandidate> t_ref;
    t_ref.clear();
    BuildScene_GatherMeshLibRefs(world, t_ref, m_bvhTestedMask, m_bvhVisibleMask,
                                 camForward, /*deferCull=*/true, /*dynamicOnly=*/true);
    const size_t n = candidates.size() - from;
    uint32_t mismatches = 0;
    const char* firstField = "";
    size_t firstRow = 0;
    auto same = [](const auto& a, const auto& b) { return std::memcmp(&a, &b, sizeof(a)) == 0; };
    auto check = [&](bool ok, const char* field, size_t row) {
        if (ok) return;
        if (!mismatches) { firstField = field; firstRow = row; }
        ++mismatches;
    };
    if (t_ref.size() != n) check(false, "rowCount", 0);
    for (size_t i = 0; i < (std::min)(n, t_ref.size()); ++i)
    {
        const DrawCandidate& a = candidates[from + i];
        const DrawCandidate& b = t_ref[i];
#define DYNCMP(f) check(same(a.f, b.f), #f, i)
        DYNCMP(entity); DYNCMP(meshDescSlot); DYNCMP(indexCount); DYNCMP(worldMatrix);
        DYNCMP(mc); DYNCMP(matBits); DYNCMP(matSnap); DYNCMP(texBaseColor);
        DYNCMP(texSurfaceMap); DYNCMP(texNormalMap); DYNCMP(matHash); DYNCMP(orderKey); DYNCMP(filter);
        DYNCMP(perm); DYNCMP(worldCenter); DYNCMP(aabbMin); DYNCMP(aabbMax);
        DYNCMP(hasAabb);
        DYNCMP(depth); DYNCMP(outlinePixels); DYNCMP(screenSpaceOutline);
        DYNCMP(isPickingOutline); DYNCMP(stencilRef); DYNCMP(mirrored);
        DYNCMP(shadowCullMode); DYNCMP(castShadow); DYNCMP(prevPosElementBase);
        DYNCMP(skinned); DYNCMP(meshletBlocks); DYNCMP(customPSID); DYNCMP(customVSID); DYNCMP(viewMask);
        DYNCMP(renderInMainPass); DYNCMP(perObjectShadowCaster); DYNCMP(hairShadowCaster);
        DYNCMP(faceOverlay); DYNCMP(faceOverlayParams); DYNCMP(dissolveParams);
        DYNCMP(dissolveEdgeColor);
#undef DYNCMP
    }
    ENGINE_CHECK(mismatches == 0,
        "[DYN_RETAINED_ORACLE] patched dynamic rows differ from the gather: %u field(s), "
        "first '%s' at row %zu (rows=%zu ref=%zu)", mismatches, firstField, firstRow, n, t_ref.size());
    static uint64_t s_verified = 0;
    if (++s_verified % 60 == 0)
        LOG_INFO("[DYN_RETAINED_ORACLE] rows=%zu ref=%zu mismatches=%u match=%u",
                 n, t_ref.size(), mismatches, mismatches == 0 ? 1u : 0u);
}
#endif

// ==== Candidate tag-set signatures ====
// Order-independent signatures over the per-frame BFS tag sets (player / hair
// shadow, face overlay). Their inputs are not part of SceneItemKey, so set churn
// would silently stale the cached tag bits: a change sends the frame to the full
// gather, or, on a dynamic-add frame whose static members' signature held, the
// dynamic rows to the dynamicOnly gather.
uint64_t Renderer::ComputeCandidateStaticSetSig() const
{
    auto mixed = [](uint64_t v) -> uint64_t {
        v *= 0xFF51AFD7ED558CCDull; v ^= v >> 33;
        v *= 0xC4CEB9FE1A85EC53ull; v ^= v >> 33;
        return v;
    };
    auto cached = [this](Entity e) {
        const uint32_t id = static_cast<uint32_t>(e);
        return id < m_candEntityToIdx.size() && m_candEntityToIdx[id] != 0xFFFFFFFFu;
    };
    // The terms of ComputeCandidateSetSig, for the cache's members only.
    uint64_t sig = 0;
    for (Entity e : m_playerShadowSet) if (cached(e)) sig += mixed(0x100000000ull | e);
    for (Entity e : m_hairShadowSet)   if (cached(e)) sig += mixed(0x200000000ull | e);
    for (const auto& [e, params] : m_faceOverlayParams)
        if (cached(e)) sig += mixed((uint64_t(params) << 34) ^ 0x300000000ull ^ e);
    return sig;
}

uint64_t Renderer::ComputeCandidateSetSig() const
{
    auto mixed = [](uint64_t v) -> uint64_t {
        v *= 0xFF51AFD7ED558CCDull; v ^= v >> 33;
        v *= 0xC4CEB9FE1A85EC53ull; v ^= v >> 33;
        return v;
    };
    // Commutative sum: the unordered sets rebuild in arbitrary order.
    uint64_t sig = 0;
    for (Entity e : m_playerShadowSet) sig += mixed(0x100000000ull | e);
    for (Entity e : m_hairShadowSet)   sig += mixed(0x200000000ull | e);
    for (const auto& [e, params] : m_faceOverlayParams)
        sig += mixed((uint64_t(params) << 34) ^ 0x300000000ull ^ e);
    return sig;
}

// Shared by the shadowArgsFrameActive and gpuCompactionFrameActive latches.
bool Renderer::GpuCompactionPreconditions(uint32_t frameSlot) const
{
    return m_gpuCompaction && RetainedInstancesActive() &&
           m_useIndirectDraw && m_gpuCullingEnabled && m_cullingPass &&
           m_indirectArgBuffer.IsValid() && m_indirectArgMapped[frameSlot] &&
           m_instanceRemapGPU.IsValid() && m_instanceRemapGPUHeapIdx != 0 &&
           m_drawCountBuffer.IsValid() && m_drawCountMapped[frameSlot] &&
           m_cmdGroupMetaMapped[frameSlot] != nullptr;
}

void Renderer::BuildRenderScene(World& world)
{
    // Per-phase CPU timing of this function ([BUILDZONE] every -phaselog_every
    // frames), the headless counterpart of [RENDERZONE] in Renderer::Render: this is
    // the largest block on the game thread and cannot move to the render thread (it
    // takes World&), and Tracy zones need the GUI.
    // Slot map, written out so the marks and the labels cannot drift apart:
    //   0 dirtyDiff  1 tagSets  2 syncMat  3 cullBVH  4 gatherOrPatch
    //   5 classify   6 vfx      7 matSnap  8 sortEmit 9 lights
    //   10 probes    11 snapGC  12 preGather  13 tail (scope exit)
    static double s_bz[kBuildZoneCount] = {};
    static int    s_bzN = 0;
    static uint32_t s_bzPatchFrames = 0;   // see the report lambda
    auto _bzT = std::chrono::steady_clock::now();
    // The same slots as engine-profiler stages, so the timeline and the all-idle
    // inventory say "BRS.GatherOrPatch" instead of one opaque RenderSystem block.
    // Regions run in CODE order, not slot order: kBzNext maps a mark to the slot of
    // the region that FOLLOWS it (a mark ends its slot; the stage chain names the next).
    static Diag::Prof::NameSlot s_bzProf[kBuildZoneCount] = {
        Diag::Prof::NameSlot{ "BRS.DirtyDiff" },  Diag::Prof::NameSlot{ "BRS.TagSets" },
        Diag::Prof::NameSlot{ "BRS.SyncMat" },    Diag::Prof::NameSlot{ "BRS.CullBVH" },
        Diag::Prof::NameSlot{ "BRS.GatherOrPatch" }, Diag::Prof::NameSlot{ "BRS.Classify" },
        Diag::Prof::NameSlot{ "BRS.Vfx" },        Diag::Prof::NameSlot{ "BRS.MatSnap" },
        Diag::Prof::NameSlot{ "BRS.SortEmit" },   Diag::Prof::NameSlot{ "BRS.Lights" },
        Diag::Prof::NameSlot{ "BRS.Probes" },     Diag::Prof::NameSlot{ "BRS.SnapGC" },
        Diag::Prof::NameSlot{ "BRS.PreGather" },  Diag::Prof::NameSlot{ "BRS.Tail" } };
    static constexpr int kBzNext[kBuildZoneCount] = { 1, 2, 3, 12, 5, 6, 7, 8, 9, 10, 13, 4, 11, 13 };
    Diag::Prof::StageChain _bzProf;
    _bzProf.Next(s_bzProf[0]);
    auto _bzMark = [&_bzT, &_bzProf](int i) {
        const auto n = std::chrono::steady_clock::now();
        s_bz[i] += std::chrono::duration<double, std::milli>(n - _bzT).count();
        _bzT = n;
        _bzProf.Next(s_bzProf[kBzNext[i]]);
    };
    // Same, with the next slot named explicitly: the classify slot is split around the
    // material snapshots, so slot 5 closes twice a frame with two different
    // successors, which kBzNext cannot express.
    auto _bzMarkTo = [&_bzT, &_bzProf](int i, int next) {
        const auto n = std::chrono::steady_clock::now();
        s_bz[i] += std::chrono::duration<double, std::milli>(n - _bzT).count();
        _bzT = n;
        _bzProf.Next(s_bzProf[next]);
    };
    struct BzReport
    {
        std::chrono::steady_clock::time_point& t;
        ~BzReport()
        {
            s_bz[kBuildZoneCount - 1] +=
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t).count();
            static const int s_every = []{
                const auto& cl = Sys::CommandLine::Get();
                return cl.Has("-phaselog_every") ? cl.GetInt("-phaselog_every", 60) : 0;
            }();
            if (s_every <= 0 || ++s_bzN % s_every != 0) return;
            double tot = 0.0;
            for (double v : s_bz) tot += v;
            // patchFrames: how many of these frames took the PATCH arm of gatherOrPatch. A
            // full gather and a patch over the dynamic rows are different work in the same
            // slot; without the count its time cannot be read.
            const uint32_t patchN = s_bzPatchFrames; s_bzPatchFrames = 0;
            // gatherOrPatch is ONE slot because the two are the arms of one
            // if/else: a full gather, or PatchCandidateCache on the retained
            // path. matSnap covers index acquisition plus refresh/copy/join;
            // candPatch is NOT in it.
            LOG_INFO("[BUILDZONE] per frame (total %.2f ms): dirtyDiff %.2f tagSets %.2f "
                     "syncMat %.2f cullBVH %.2f gatherOrPatch %.2f classify %.2f vfx %.2f "
                     "matSnap %.2f sortEmit %.2f lights %.2f probes %.2f snapGC %.2f preGather %.2f tail %.2f "
                     "| patchFrames %u/%d",
                     tot / s_every,
                     s_bz[0]/s_every, s_bz[1]/s_every, s_bz[2]/s_every,  s_bz[3]/s_every,
                     s_bz[4]/s_every, s_bz[5]/s_every, s_bz[6]/s_every,  s_bz[7]/s_every,
                     s_bz[8]/s_every,  s_bz[9]/s_every,  s_bz[10]/s_every, s_bz[11]/s_every,
                     s_bz[12]/s_every, s_bz[13]/s_every, patchN, s_every);
            for (double& v : s_bz) v = 0.0;
        }
    } _bzReport{ _bzT };

    // Container zone: the sub-phases have their own BS.* zones, so this
    // zone's SELF time = the un-zoned tail (probes / DDGI / terrain / grass /
    // water sync + debug wireframes + billboard/beam/afterimage gathers).
    ZoneScopedN("BuildRenderScene");

    // -brssplit safety net. App joins the static lanes right after
    // SecondaryPhysics; this is the belt-and-braces join for every path that
    // reaches a scene build without going through that phase (editor preview
    // builds, a scene swap, an early-out in the tick loop). Nothing below may
    // touch m_candCache while a lane still holds it. Idempotent.
    JoinStaticSceneEarly();

    DrawPacketsWrite().clear();

    // Bump the material frame epoch once per build. It stamps the retained
    // material-slot first-sight test (SortAndEmit) and the per-worker gather cache,
    // both consumed later this frame.
    ++m_materialFrameEpoch;
    // The retained-cache shortcuts' switch for THIS frame, decided once:
    // -candretainoff for the run, -candretainab=N for every other frame (one process
    // measures both under the same machine load).
    {
        static const bool s_candRetainOff = Sys::CommandLine::Get().Has("-candretainoff");
        bool retainOff = s_candRetainOff;
#if WITH_HARNESS
        static const int s_candRetainAB = Sys::CommandLine::Get().GetInt("-candretainab", 0);
        if (s_candRetainAB && (m_materialFrameEpoch + s_candRetainAB) % 2 == 0) retainOff = true;
#endif
        m_candRetainOffThisFrame = retainOff;
    }

    // ---- Material preview: APPLY, and it goes first ----
    // Same rule as ApplyProbeBakeFeedback's placement: the apply must run before
    // anything that could re-arm the lane, or a request would be handed over a second
    // time while its own result was still on the way back. Here it also publishes the
    // render half's counters into FrameStats.
    ApplyPreviewFeedback();

    // ---- The in-game PiP's pending latch, applied HERE and nowhere else ----
    // SetGamePiP / ClearGamePiP run wherever a game calls them (the harness block at
    // the tail of App::Run's frame loop, a Lua script, gameplay code), possibly while
    // the previous frame's render half is still inside RenderHalf reading Target::req /
    // result / needsRender. So they only write a game-owned pending struct; this is
    // the one point that turns it into an AcquireView / ReleaseView, inside the extract
    // window, after the park.
    ApplyPendingGamePiP();

    // ---- The preview mesh-library hand-backs, drained HERE ----
    // A detached library's mesh-descriptor and bindless VB/IB slots must be freed
    // where nothing has registered a slot for this frame yet, so a slot handed back
    // can never be one this frame is about to hand out. That is the top of
    // BuildRenderScene, not ExtractPreviewRequests (the last call, after this file's
    // MeshLibrary::Load for newly visible scene refs and the skinned / VFX
    // RegisterBuffer calls). In the editor the queuing side runs from OnUIRender inside
    // FramePipeline::BuildEarlyEditorUI (before this frame's BeginFrame) or RenderHalf (after
    // it), with the render half parked either way.
    DrainPreviewMeshLibHandbacks();

    // ---- The secondary cameras, BEFORE anything culls ----
    // BuildScene_ClassifyCandidates deletes every candidate outside the MAIN frustum
    // and keeps copies for the secondary views, so their frusta must be in hand before
    // it; otherwise a PiP pointed away from the player could only show what the main
    // camera sees. ExtractPreviewRequests, which mints the generation, still comes last.
    ExtractPreviewCameras(world);
    // The probe being baked this frame, for the same reason: the classify keeps it a
    // copy of what the main view drops (its faces are a secondary volume). After the
    // cameras: the auto-exposure readback they arm is the PiP's alone. The GPU records
    // are packed later (Phase 5).
    BuildScene_SelectProbes(world);

    // Current-frame camera forward for transparent sort (m_view is updated AFTER this function).
    const XMFLOAT3 camForwardThisFrame = FrameViewWrite().camera.forward;

    // The selection outline of a StaticMesh owner is its derived sections':
    // the owner draws nothing itself (IsPickingOutlined, read by the gathers).
    m_pickingOutlineSections.clear();
    if (m_pickingOutlineEntity != NullEntity && world.IsAlive(m_pickingOutlineEntity) &&
        world.HasComponent<StaticMeshComponent>(m_pickingOutlineEntity))
        if (const Children* ch = world.GetComponent<Children>(m_pickingOutlineEntity))
            for (Entity c : ch->entities)
                if (const MeshSectionOf* so = world.GetComponent<MeshSectionOf>(c);
                    so && so->owner == m_pickingOutlineEntity)
                    m_pickingOutlineSections.push_back(c);

    // ---- A runtime spawn's custom shaders, before anything reads them ----
    // (Renderer::QueueSpawnMaterialResolve). Here, before the diff and the bind: the
    // reflection sync rewrites the new materials' layout, and SyncMaterials below must
    // bind THAT one, or the next frame re-binds it as material dirt.
    ResolveSpawnMaterials(world);

    // ---- Phase 0: retained-scene dirty diff (see Renderer.h) ----
    BuildScene_DirtyDiff(world);
    ClassifySceneAdds(world);   // dynamic-only adds (see Renderer.h)
    _bzMark(0);

    // ---- Shared membership gate (see Renderer.h) ----
    // Consumed by the retained tag sets below and the retained BVH leaves in CullBVH.
    // Stamp continuity is consumed HERE, once: a second consumer-side check would
    // always see a gap. Per-store validity bools handle flag toggles.
    {
        const SceneDiffView d = GetSceneDiffView();
        const bool continuous =
            d.valid && (d.frameStamp == m_retainedSceneLastStamp + 1);
        m_retainedSceneLastStamp = d.valid ? d.frameStamp : 0;
        // Both consumers off -> skip the boundary walk entirely (flag-off frames pay
        // nothing for it).
        m_sceneMembershipStable = continuous &&
            (m_retainedBvh || m_retainedTagSets) &&
            d.added->empty() && d.removed->empty() && d.dirtyOther->empty() &&
            BuildScene_TagSetBoundaryStable(world);
        // The BVH alone may also patch a frame that only ADDS dynamic leaves: they go in
        // beside the tree (SceneBVH::AddLooseLeaf). The tag sets keep the strict gate: a
        // spawned character can carry their tags.
        m_bvhAddPatchFrame = continuous && m_retainedBvh && !m_sceneMembershipStable &&
                             m_addsDynamicOnly && m_addsOtherOwn;
    }

    // ---- Resolve the player subtree (per-object shadow casters) + its AABB. ----
    // Must precede the gather so candidates can be tagged; read-only afterwards.
    BuildScene_ComputePlayerShadowSet(world);

    // ---- Resolve the bangs subtree (hair -> face shadow casters) + its AABB. ----
    BuildScene_ComputeHairShadowSet(world);

    // ---- Resolve the brows/lashes subtree (through-hair overlay casters). ----
    BuildScene_ComputeFaceOverlaySet(world);

    // ---- Resolve DissolveComponent subtrees (per-instance dissolve params). ----
    BuildScene_ComputeDissolveSet(world);
    _bzMark(1);

    // The three retained sets above completed against this frame's diff: arm their
    // patch gate for next frame (dissolve is per-frame by contract).
    m_tagSetsRetainedValid = m_retainedTagSets && m_retainedSceneDiff;

    // ---- Sync material textures + NPR ramp scan + deferred-release flush. ----
    // BEFORE SyncMaterials, and that order is the whole safety argument: a
    // destroy and a respawn in the same gameplay phase reuse the entity id, and
    // the respawned entity has no binding until SyncMaterials runs below - so a
    // deferred erase here can only ever remove the dead entity's.
    DrainPendingBindingReleases();
    BuildScene_SyncMaterials(world);
    ClassifySceneAddsMaterials();   // are the material binds all the adds'?
    _bzMark(2);

    // ---- Retained-candidate flag + shadow-args latch ----
    // Both are decided before the BVH cull because BvhCullMasksNeeded() reads them:
    // with both armed nothing consumes the visible / tested masks and the frustum cull
    // is dead output. Every input is frame-stable (see GpuCompactionPreconditions),
    // and the latch is set before the classify pass, SortAndEmit's shadow meta and
    // Render()'s per-cascade dispatches, so every stage agrees.
    const bool retained = m_retainedCandidates && m_retainedSceneDiff;
    {
        const uint32_t frameSlot = m_gfx.GetUploadSlot();
        SceneEmitWrite().shadowArgsFrameActive =
            m_retainedShadowArgs && retained &&
            GpuCompactionPreconditions(frameSlot) &&
            m_shadowCmdMetaMapped[frameSlot] != nullptr &&
            m_shadowGroupCounts.IsValid() && m_shadowCountZeroUpload.IsValid() &&
            m_shadowWorkingArgs[0].IsValid() &&      // creation is all-or-nothing
            m_shadowRemapHeapIdx[0] != 0 &&
            m_shadowPass != nullptr && m_shadowSystem != nullptr;
    }

    // ---- Retained-candidate gate (same scheme as DDGISceneAS's) ----
    // Patch path only when the dirty feed is continuous and the frame carried nothing
    // membership- or appearance-affecting; anything else runs the full gather below.
    // Needs the scene diff: with it off `retained` is false and the cache never
    // engages.
    const SceneDiffView sceneDiff = GetSceneDiffView();
    const bool diffContinuous =
        sceneDiff.valid && (sceneDiff.frameStamp == m_candCacheLastStamp + 1);
    m_candCacheLastStamp = sceneDiff.valid ? sceneDiff.frameStamp : 0;
    const uint64_t candSetSig = retained ? ComputeCandidateSetSig() : 0;
    // A frame whose only dirt is dynamic-lane adds keeps the static cache
    // (ClassifySceneAdds). The full signature may move with them - a spawned
    // character's tags - so it is the static members' half that must hold.
    const bool addPatch = retained && m_addsDynamicOnly && m_addsOtherOwn && m_addsMatOwn;
    const bool sigHeld = candSetSig == m_candCacheSetSig ||
                         (addPatch && ComputeCandidateStaticSetSig() == m_candCacheStaticSig);
    // Textures still arriving matter to the cache only where one of its own rows
    // samples them (the static rows carry resolved texture handles).
    // -gather_noaddpath (the reverse arm): any unsettled texture vetoes the patch.
    static const bool s_noAddPath = Sys::CommandLine::Get().Has("-gather_noaddpath");
    const bool texSettled = m_matTexUnsettled == 0 ||
                            (!s_noAddPath && !TexUnsettledTouches(m_candEntityToIdx));
    const bool usePatch =
        retained && m_candCacheValid && diffContinuous &&
        m_candUnresolved == 0 && texSettled && !m_hasDissolve &&
        ((sceneDiff.added->empty()      && sceneDiff.removed->empty() &&
          sceneDiff.dirtyOther->empty() && sceneDiff.matDirty->empty()) || addPatch) &&
        sigHeld &&
        m_pickingOutlineEntity == m_candCachePickEntity;
    // The dynamic rows' retained copies know neither the adds nor a texture that has
    // settled, nor tag bits the signature says moved: this frame they come from the
    // dynamicOnly gather (every entity the static cache does not hold is evaluated)
    // and re-arm. Without this the new parts would be missing from the draw list - the
    // copies are replayed verbatim.
    const bool forceDynGather = usePatch &&
        (addPatch || m_matTexUnsettled != 0 || candSetSig != m_candCacheSetSig);
    if (forceDynGather)
    {
        m_dynCandCacheValid = false;
        m_candCacheSetSig   = candSetSig;   // the static half held; this is the set now
    }
    // Which terms sent this frame to the full gather (GatherGateStats.h;
    // frames.jsonl gatherFull / gatherFullReason). Every term, not just the
    // first that failed: a spawn vetoes it through several at once.
    {
        namespace G = GatherGateStats;
        uint32_t why = 0;
        if (!retained) why = G::kOff;
        else
        {
            if (!m_candCacheValid)                    why |= G::kInvalid;
            if (!diffContinuous)                      why |= G::kContinuity;
            if (m_candUnresolved != 0)                why |= G::kUnresolved;
            if (m_matTexUnsettled != 0)               why |= G::kTexUnsettled;
            if (m_hasDissolve)                        why |= G::kDissolve;
            if (!sceneDiff.added->empty())            why |= G::kAdded;
            if (!sceneDiff.removed->empty())          why |= G::kRemoved;
            if (!sceneDiff.dirtyOther->empty())       why |= G::kOther;
            if (!sceneDiff.matDirty->empty())         why |= G::kMat;
            if (candSetSig != m_candCacheSetSig)      why |= G::kSetSig;
            if (m_pickingOutlineEntity != m_candCachePickEntity) why |= G::kPick;
        }
        G::Note(G::cand, FrameViewWrite().currentFrame, !usePatch, why);
    }
    m_candCacheValid  = false; // re-armed by whichever path completes below
    m_candUnresolved  = 0;     // recounted by this frame's gathers
    // ---- The dynamic rows' patch starts HERE, as jobs ----
    // Every input of the gate above is final, and nothing main does until the patch
    // point writes what the patch reads (Renderer.h, "The dynamic rows' patch as
    // jobs"). Joined at the patch point, or discarded before anything writes
    // m_dynCandCache; the guard joins it on unwinding. -dynpatchasyncoff keeps the
    // synchronous patch.
    static const bool s_dynAsyncOff = Sys::CommandLine::Get().Has("-dynpatchasyncoff");
    bool dynAsync = false;
    bool dynAsyncFrame = !s_dynAsyncOff;
#if WITH_HARNESS
    // -dynpatchasyncab=N: every other frame patches synchronously (same-process A/B).
    static const int s_dynAsyncAB = Sys::CommandLine::Get().GetInt("-dynpatchasyncab", 0);
    if (s_dynAsyncAB && (m_materialFrameEpoch + s_dynAsyncAB) % 2 == 0) dynAsyncFrame = false;
#endif
    if (usePatch && DynRetainedEnabled() && m_dynCandCacheValid &&
        dynAsyncFrame && !BrsSplitEnabled())
        dynAsync = KickDynPatchAsync(world, camForwardThisFrame);
    struct DynAsyncGuard {
        Renderer* r;
        ~DynAsyncGuard() {
            if (r->m_dynAsyncKicked)
                try { std::vector<DrawCandidate> none; r->JoinDynPatchAsync(none, false); }
                catch (...) {}
        }
    } dynAsyncGuard{ this };

    // ---- Phase 0.5: BVH + frustum cull - dense per-entity visible / tested bitmasks ----
    // Masks live on the Renderer (m_bvh*Mask) so their capacity persists.
    BuildScene_CullBVH(world, m_bvhVisibleMask, m_bvhTestedMask);

    // ---- Debug wireframes (not gated on culling: Clear() is needed every frame) ----
    BuildScene_DebugWireframes(world);
    _bzMark(3);

    // ---- Phase 1: collect draw candidates ----
    // A MEMBER, not a local: at 248 B per DrawCandidate and ~8.5k candidates on the
    // heaviest scene, a local is a ~2 MB malloc + free every frame, which goes straight
    // to VirtualAlloc / VirtualFree with hundreds of first-touch page faults. clear()
    // keeps the capacity across frames.
    std::vector<DrawCandidate>& candidates = m_candScratch;
    candidates.clear();
    // Adaptive reserve: use last frame's count to avoid reallocation.
    static uint32_t s_lastCandidateCount = 256;
    candidates.reserve(s_lastCandidateCount + 64);

    // m_frameLights / VolLightsWrite(): adaptive reserve from last frame's count.
    static uint32_t s_lastFrameLightCount = 32;
    static uint32_t s_lastVolLightCount   = 8;
    m_frameLights.clear();
    VolLightsWrite().clear();
    if (m_frameLights.capacity()      < s_lastFrameLightCount + 8)
        m_frameLights.reserve(s_lastFrameLightCount + 8);
    if (VolLightsWrite().capacity() < s_lastVolLightCount + 4)
        VolLightsWrite().reserve(s_lastVolLightCount + 4);
    m_sunVolumetric      = false;
    m_sunVolumetricScale = 1.0f;
    m_sunMediumScale     = 1.0f;

    // The snapshot table persists across frames: cached candidates carry matSnap
    // indices from earlier frames. GCMatSnapshots clears it only past its cap (and
    // bumps m_matSnapClearGen so the retained indices are dropped); RetireMatSnapshots
    // returns the storage of slots unreferenced for its grace.
    _bzMark(12);  // pre-gather: the region between DebugWireframes and the snapshot GC
    GCMatSnapshots();
    RetireMatSnapshots();
    _bzMark(11);  // GCMatSnapshots + RetireMatSnapshots (the snapGC slot)

    static const bool s_matSnapLate = Sys::CommandLine::Get().Has("-matsnaplate");
    // ---- The classify goes to the lane with the plan it feeds ----
    // It reads candidate fields, the frame's frusta and a few flags (all captured in
    // ClassifyInputs), so on the lane it runs in its World-free form (box from the
    // candidate, predicates from matBits). Only with the plan on the lane (its output
    // is PlanEmit's input), only on the retained path (the only one that classifies
    // after the gather) and only in the default order (-matsnaplate classifies before
    // the snapshots, before anything could be handed over).
    // -laneclassifyoff keeps it on main.
    static const bool s_laneClassifyOff = Sys::CommandLine::Get().Has("-laneclassifyoff");
    bool classifyOnLane = retained && m_emitDeferred && m_brsPlanLane &&
                          !s_laneClassifyOff && !s_matSnapLate;
#if WITH_HARNESS
    // -laneclassifyab=N: every other frame keeps the classify on main, so one
    // process measures both placements under the same machine load.
    static const int s_laneClassifyAB = Sys::CommandLine::Get().GetInt("-laneclassifyab", 0);
    if (s_laneClassifyAB && (m_materialFrameEpoch + s_laneClassifyAB) % 2 == 0)
        classifyOnLane = false;
#endif

    // ---- The spot-caster tracking and the light prep follow it ----
    // The tracking has to see the UNclassified rows (off-screen casters still
    // invalidate their lights), so it can only leave main together with the classify;
    // the light prep consumes what the tracking produced this same frame, so it goes
    // with it. -lanelightoff keeps both here, and so do main's own light-placement
    // experiments (-earlylightprep, -lightprepab).
    static const bool s_laneLightOff = Sys::CommandLine::Get().Has("-lanelightoff");
    bool lightExperiment = false;
#if WITH_HARNESS
    {
        const auto& lc = Sys::CommandLine::Get();
        lightExperiment = lc.Has("-earlylightprep") || lc.GetInt("-lightprepab", 0) != 0;
    }
#endif
    const bool lightPrepOnLane = classifyOnLane && !s_laneLightOff && !lightExperiment;
    {
        static bool s_logged = false;
        if (!s_logged && m_emitDeferred && m_brsPlanLane)
        {
            s_logged = true;
            LOG_INFO("[LANECLASSIFY] enabled=%u retained=%u off=%u matSnapLate=%u "
                     "lightPrep=%u lightOff=%u lightExperiment=%u",
                     classifyOnLane ? 1u : 0u, retained ? 1u : 0u,
                     s_laneClassifyOff ? 1u : 0u, s_matSnapLate ? 1u : 0u,
                     lightPrepOnLane ? 1u : 0u, s_laneLightOff ? 1u : 0u,
                     lightExperiment ? 1u : 0u);
        }
    }
    // ---- The cached rows' matSnap retention, decided HERE (GC / retire just ran) ----
    // Usable when the write-back happened, no snapshot-table clear came in between
    // (GCMatSnapshots bumps m_matSnapClearGen) and every slot the rows name still
    // holds the key it held then. -matsnaplate classifies before the snapshots, which
    // breaks the row mapping the write-back uses, and -brssplit rewrites the cache from
    // its own early lanes: both re-acquire every row.
    static const bool s_retainLateOrder = Sys::CommandLine::Get().Has("-matsnaplate");
    bool retainUsable = !m_candRetainOffThisFrame && m_candCacheRetained &&
                        m_candCacheSnapGen == m_matSnapClearGen && !s_retainLateOrder &&
                        !BrsSplitEnabled();
    if (retainUsable)
        for (const auto& [idx, key] : m_candCacheSnapKeys)
            if (idx >= m_matSnapKeyOf.size() || m_matSnapKeyOf[idx] != key)
            { retainUsable = false; break; }
    // -laneassemble (opt-in): the lane, not main, copies the ~7,000 cached rows into
    // the list it classifies; main hands over [dynamic prims | dynamic meshlib] and the
    // lane builds the patch-frame layout around it. Off by default: main saves ~0.1 ms
    // but the lane pays ~0.16 ms (it copies the dynamic rows a second time, and
    // workers are busy with the next frame's Animation phase), and on a GPU-bound frame
    // the lane's time before its first submit is what the GPU waits on.
    // TODO(Frame_Job_Graph_Plan M10.3): re-measure -laneassemble once the lane submits
    // earlier.
    static const bool s_laneAssembleOn = Sys::CommandLine::Get().Has("-laneassemble");
    bool laneAssemble = usePatch && retainUsable && lightPrepOnLane && s_laneAssembleOn;
#if WITH_HARNESS
    // -laneassembleab=N: every other frame copies on main (same-process A/B;
    // it arms the lane copy on the other frames by itself).
    static const int s_laneAssembleAB = Sys::CommandLine::Get().GetInt("-laneassembleab", 0);
    if (s_laneAssembleAB)
        laneAssemble = usePatch && retainUsable && lightPrepOnLane &&
                       (m_materialFrameEpoch + s_laneAssembleAB) % 2 != 0;
#endif
    {
        static bool s_assembleLogged = false;
        if (!s_assembleLogged && laneAssemble)
        {
            s_assembleLogged = true;
            LOG_INFO("[LANEASSEMBLE] first frame=%llu cached=%zu",
                     static_cast<unsigned long long>(m_materialFrameEpoch), m_candCache.size());
        }
    }

    bool patched = false;
    size_t cullBegin = 0;      // classify slice start (MeshLibRef entries)
    if (usePatch)
    {
        ZoneScopedN("BS.CandPatch");
        { PROF_SCOPE("BRS.Patch.Cache");
        patched = PatchCandidateCache(world); }
        if (patched)
        {
            ++s_bzPatchFrames;
            // [cached prims | lights + dynamic prims | cached meshlib | dynamic meshlib]
            // Prim entries must all precede cullBegin (they are never camera-culled); the sort
            // normalizes the order difference from a full gather.
            if (!laneAssemble)
            { PROF_SCOPE("BRS.Patch.Copy");
            candidates.insert(candidates.end(), m_candCache.begin(),
                              m_candCache.begin() + m_candCachePrimCount); }
            { PROF_SCOPE("BRS.Gather.PrimLights");
            BuildScene_GatherPrimitivesAndLights(world, candidates,
                                                 camForwardThisFrame,
                                                 /*dynamicOnly=*/true); }
            cullBegin = candidates.size();
            if (!laneAssemble)
            { PROF_SCOPE("BRS.Patch.Copy");
            candidates.insert(candidates.end(),
                              m_candCache.begin() + m_candCachePrimCount,
                              m_candCache.end()); }
            // The dynamic rows come from their retained copies when nothing structural moved;
            // otherwise from the dynamicOnly gather, whose rows then re-arm the copies.
            const size_t dynBegin = candidates.size();
            bool dynPatched = false;
            if (dynAsync)
            {
                // The jobs kicked above. GCMatSnapshots may have voided the dynamic cache since
                // (it runs after the kick): then the rows are discarded and the gather runs.
                PROF_SCOPE("BRS.Patch.Dynamic");
                dynPatched = JoinDynPatchAsync(candidates, /*use=*/m_dynCandCacheValid);
            }
            else if (DynRetainedEnabled() && m_dynCandCacheValid)
            {
                PROF_SCOPE("BRS.Patch.Dynamic");
                dynPatched = PatchDynCandCache(world, candidates, camForwardThisFrame);
            }
            if (!dynPatched)
            {
                BuildScene_GatherMeshLibRefs(world, candidates,
                                             m_bvhTestedMask, m_bvhVisibleMask,
                                             camForwardThisFrame,
                                             /*deferCull=*/true, /*dynamicOnly=*/true);
                if (DynRetainedEnabled()) SnapshotDynCandCache(candidates, dynBegin);
            }
#if WITH_HARNESS
            else if (Sys::CommandLine::Get().Has("-dynretainedverify"))
                VerifyDynCandCache(world, candidates, dynBegin, camForwardThisFrame);
#endif
            m_candCacheValid = true; // cache stays authoritative next frame
        }
    }
    if (!patched)
    {
        // The static patch failed after the dynamic one was kicked: join and drop it
        // before this path re-snapshots m_dynCandCache.
        if (m_dynAsyncKicked) JoinDynPatchAsync(candidates, /*use=*/false);
        BuildScene_GatherPrimitivesAndLights(world, candidates,
                                             camForwardThisFrame,
                                             /*dynamicOnly=*/false);
        const size_t primEnd = candidates.size();
        cullBegin = primEnd;

        // ---- MeshLibRef entities: serial gather + parallel OBB cull + candidate build ----
        BuildScene_GatherMeshLibRefs(world, candidates, m_bvhTestedMask,
                                     m_bvhVisibleMask, camForwardThisFrame,
                                     /*deferCull=*/retained, /*dynamicOnly=*/false);
        if (retained)
        {
            SnapshotCandidateCache(candidates, primEnd, candSetSig);
            // The rows' material pointers were resolved from the pool as it stands now; a
            // later patch frame compares against this.
            const auto* pMatPool = world.GetPool<MaterialComponent>();
            m_candCacheMatPool        = pMatPool;
            m_candCacheMatPoolVersion = pMatPool ? pMatPool->StructureVersion() : 0;
            // After it: the dynamic rows are the ones whose entity it did NOT
            // take, which is the dynamicOnly gather's own skip test.
            if (DynRetainedEnabled()) SnapshotDynCandCache(candidates, primEnd);
        }
        else m_dynCandCacheValid = false;
    }
    // ---- The static half's mirror bits are already in (-brssplit or retention) ----
    // On a split frame every row from m_candCache carries a mirror bit a worker
    // computed; on a retaining frame (retainNow, below) the one the cache kept. The
    // transform patch refreshed the rows it rewrote. The dynamic rows are mirrored here,
    // where the layout is still known exactly:
    //     [ cachePrims | dynPrims | cacheMeshLib | dynMeshLib ]
    // cullBegin is the boundary between the two prim slices, and the cached MeshLibRef
    // slice is (cacheSize - primCount) rows long. The serial mirror loop further down
    // then covers only what the classify appends (secondary-view duplicates,
    // billboards, beams, afterimages), from brsMirrorResumeFrom. The bit is a pure
    // function of worldMatrix, which nothing between here and SortAndEmit (its only
    // reader) writes.
    size_t brsMirrorResumeFrom = 0;
    const bool brsSplit = patched && m_brsEarlyReady;
    // ---- The cached rows keep their mirror bit and matSnap ----
    // On a patch frame that is not a split frame, when retainUsable (decided before
    // the patch; nothing since touched the snapshot table).
    const bool retainNow = patched && !brsSplit && retainUsable;
    if (patched && m_candCacheRetained && !retainNow) ++m_candRetainFallbacks;
    if (retainNow) ++m_candRetainFrames;
    // Cached rows that carry a matSnap the acquisition below must NOT trust
    // this frame (it skips any row whose matSnap is set): clear them, so the
    // walk re-acquires every one and the write-back re-establishes the set.
    if (patched && !retainNow && (m_candCacheHasSnaps || !m_candCacheVolatile.empty()))
    {
        PROF_SCOPE("BRS.CandRetain.Reset");
        for (size_t i = 0; i < m_candCachePrimCount && i < candidates.size(); ++i)
            candidates[i].matSnap = 0xFFFFFFFFu;
        const size_t meshEnd = cullBegin + (m_candCache.size() - m_candCachePrimCount);
        for (size_t i = cullBegin; i < meshEnd && i < candidates.size(); ++i)
            candidates[i].matSnap = 0xFFFFFFFFu;
    }
    // Cached rows occupy [0, cachePrims) and [cullBegin, cacheMeshEnd) of the
    // candidate list on a patch frame; everything else was produced this frame. When
    // the lane assembles, NONE of them is in the list: both ranges are empty.
    const bool laneAssembleNow = laneAssemble && patched;
    const size_t cachePrimsNow   = laneAssembleNow ? 0 : m_candCachePrimCount;
    const size_t cacheMeshEndNow = !patched ? 0
        : laneAssembleNow ? cullBegin : cullBegin + (m_candCache.size() - cachePrimsNow);
    if (brsSplit || retainNow)
    {
        ZoneScopedN("BS.Split.Merge");
        for (size_t i = cachePrimsNow; i < cullBegin; ++i)
            candidates[i].mirrored = MirrorBitFor(candidates[i].worldMatrix);
        for (size_t i = cacheMeshEndNow; i < candidates.size(); ++i)
            candidates[i].mirrored = MirrorBitFor(candidates[i].worldMatrix);
        brsMirrorResumeFrom = candidates.size();
    }
    // After the if/else, not inside the !patched arm, so the patch path's cost lands
    // in gatherOrPatch rather than in classify.
    _bzMark(4);

    // ---- Pre-cull scene triangle total (part 1: primitives + MeshLibRefs) ----
    // Measured HERE: BuildScene_ClassifyCandidates below compacts the vector, after
    // which the un-culled set is gone. Only on the retained path: with it off the
    // gathers cull inline and no pre-cull set ever exists, so the stat is published as
    // absent rather than as a plausible wrong value.
    m_frameStats.triSceneValid = retained;
    m_frameStats.triScene      = 0;
    if (retained)
    {
        uint64_t sceneTris = 0;
        auto sumRows = [&](size_t lo, size_t hi) {
            for (size_t i = lo; i < hi; ++i)
                if (candidates[i].filter != DrawFilter::Custom)   // outline shells duplicate their mesh
                    sceneTris += uint64_t(candidates[i].indexCount / 3u);
        };
        if (retainNow)
        {
            // The cached rows' share was summed when the cache was built.
            sceneTris = m_candCacheTris;
            sumRows(cachePrimsNow, cullBegin);
            sumRows(cacheMeshEndNow, candidates.size());
        }
        else
            sumRows(0, candidates.size());
        m_frameStats.triScene = sceneTris;
    }

    if (lightPrepOnLane)
    {
        // What the tracking reads off the renderer, as it stands here.
        m_laneSpotRetainedDiff    = m_retainedSceneDiff;
        m_laneSpotMatTexUnsettled = m_matTexUnsettled != 0;
    }
    else
    {
        // Sub-scope: a serial walk of every candidate, about half of BRS.Classify.
        PROF_SCOPE("BRS.Classify.SpotCasters");
        TrackSpotShadowCasters(candidates, m_retainedSceneDiff, m_matTexUnsettled != 0,
                               /*laneForm=*/false);
    }

    // ---- Material snapshots BEFORE the classify ----
    // The snapshots read *mc (World memory), so they must finish before the hand-off;
    // the classify reads only candidate fields and the frame's frusta, so it can move
    // to the lane. So the procedural rows are gathered into their own scratch first,
    // mirror + snapshots run over the unclassified rows plus that scratch, then the
    // classify compacts and the secondary-view duplicates and the scratch append - the
    // same row order as classifying first. Snapshots taken for rows the classify then
    // drops change which slots the table holds, never an output byte (matSnap is in no
    // sort or batch key). -matsnaplate is the reverse arm (classify first).
    _bzMarkTo(5, 6);   // classify part 1 (tri total + spot casters) -> vfx
    std::vector<DrawCandidate>& procedural = m_proceduralScratch;
    procedural.clear();
    // ---- Billboard entities -> additional DrawCandidates ----
    BuildScene_GatherBillboards(world, procedural, camForwardThisFrame);
    // ---- Beam entities -> CS-generated tube DrawCandidates ----
    BuildScene_GatherBeams(world, procedural, camForwardThisFrame);
    // ---- Afterimage snapshots -> DrawCandidates (procedural ghost PS) ----
    BuildScene_GatherAfterimages(procedural, camForwardThisFrame);

    // The classify (part 2) and everything that follows it: the secondary
    // duplicates, then the procedural rows, then their triangle total.
    auto classifyAndAppend = [&]
    {
    // Camera-dependent cull + shadow fallback + transparent depth, only when the
    // gathers deferred it (retained); otherwise the gathers classified in place.
    if (retained)
    {
        BuildScene_ClassifyCandidates(CaptureClassifyInputs(cullBegin, camForwardThisFrame),
                                      &world, candidates);
        // -brssplit: the classify COMPACTS in place, so the index ranges the
        // split used above are gone - but every surviving row kept its mirror
        // bit through the move. Resume from the post-compaction size, which is
        // where the secondary duplicates and the procedural gathers append.
        if (brsSplit) brsMirrorResumeFrom = candidates.size();
        // ---- The secondary-view duplicates ----
        // Appended HERE, between the classify compaction and `proceduralCandBegin`: before
        // the classify they would be fed back into the frustum test that rejected their
        // originals; after proceduralCandBegin they would count into triScene part 2 and
        // the scene triangle total would jump when a PiP opened. Empty (and free) when no
        // WorldCull view is live.
        if (!m_secondaryCands.empty())
            candidates.insert(candidates.end(),
                              m_secondaryCands.begin(), m_secondaryCands.end());
    }

    // Everything appended past this point is procedural and never went through the
    // cull, but it does become packets counted into triMainView; left out of triScene
    // it would make Culled% go negative in a VFX-heavy scene.
    const size_t proceduralCandBegin = candidates.size();
    candidates.insert(candidates.end(), procedural.begin(), procedural.end());

    // ---- Pre-cull scene triangle total (part 2: the procedural gathers) ----
    if (m_frameStats.triSceneValid)
    {
        uint64_t extra = 0;
        for (size_t i = proceduralCandBegin; i < candidates.size(); ++i)
            if (candidates[i].filter != DrawFilter::Custom)
                extra += uint64_t(candidates[i].indexCount / 3u);
        m_frameStats.triScene += extra;
    }

    s_lastCandidateCount = static_cast<uint32_t>(candidates.size());
    s_lastFrameLightCount = static_cast<uint32_t>(m_frameLights.size());
    s_lastVolLightCount   = static_cast<uint32_t>(VolLightsWrite().size());
    };   // classifyAndAppend

    // The reverse arm (-matsnaplate): classify first, then mirror + snapshots over
    // everything (the scratch is already appended, so it is not walked separately
    // below).
    if (s_matSnapLate)
    {
        _bzMarkTo(6, 5);
        classifyAndAppend();
        _bzMarkTo(5, 7);
    }
    else
        _bzMarkTo(6, 7);
    // Rows the snapshot / mirror passes must ALSO cover: the procedural scratch,
    // while it is still separate (the default order).
    std::vector<DrawCandidate>* const extraRows = s_matSnapLate ? nullptr : &procedural;

    // ---- Mirror flag: sign(det3(world)) - raster winding + batch key ----
    // One pass here rather than at each gather site: this point is after every
    // producer (primitives, MeshLibRef, the cache patch, billboards, beams,
    // afterimages) and after the push-then-mutate outline copies, so no path can
    // forget it. worldMatrix is stored transposed, but det is invariant under
    // transpose. Same formula as the RT path (DDGISceneAS: det3 < 0 ->
    // TRIANGLE_FRONT_COUNTERCLOCKWISE).
    bool scenePrepGraph = false;
    bool scenePrepVerify = false;
    bool earlyLightData = false;
    bool lightDataPrepared = false;
#if WITH_HARNESS
    static const bool s_requestPrepGraph = Sys::CommandLine::Get().Has("-sceneprepgraph");
    static const bool s_requestPrepVerify = Sys::CommandLine::Get().Has("-sceneprepverify");
    static const int s_prepAB = Sys::CommandLine::Get().GetInt("-sceneprepab", 0);
    scenePrepGraph = s_requestPrepGraph || (s_prepAB > 0 && ((m_materialFrameEpoch + s_prepAB) % 2 == 0));
    scenePrepVerify = s_requestPrepVerify;
    const auto& lightPrepCmd = Sys::CommandLine::Get();
    const int lightPrepAB = lightPrepCmd.GetInt("-lightprepab", 0);
    earlyLightData = lightPrepCmd.Has("-earlylightprep") ||
        (lightPrepAB && (m_materialFrameEpoch + lightPrepAB) % 2 == 0);
    // Both A/B sides use the same CPU preparation graph; only the placement of
    // independent owner light work changes. Resource/material sync stays late.
    if (earlyLightData || lightPrepAB) scenePrepGraph = true;
#endif
    ZoneNamedN(lightEarlyZone, "BS.LightPlacement.Early", earlyLightData);
    ZoneNamedN(lightLateZone, "BS.LightPlacement.Late", !earlyLightData);
    {
    ZoneNamedN(prepGraphZone, "BS.PrepareCPU.Graph", scenePrepGraph);
    ZoneNamedN(prepLegacyZone, "BS.PrepareCPU.Legacy", !scenePrepGraph);
    auto mirror = [](DrawCandidate& c)
    {
        // Shared definition (Renderer::MirrorBitFor), as a lambda so the
        // scene-preparation graph's chunks below can call it.
        c.mirrored = MirrorBitFor(c.worldMatrix);
    };
    // brsMirrorResumeFrom is 0 unless the split / retention already mirrored the rows
    // before it.
    if (!scenePrepGraph)
    {
        for (size_t i = brsMirrorResumeFrom; i < candidates.size(); ++i)
            mirror(candidates[i]);
        if (extraRows)
            for (DrawCandidate& c : *extraRows) mirror(c);
    }
    // Start mirrors before the owner acquires material indices. These touch
    // different scalar fields: acquisition reads mc/matHash/matBits and writes
    // matSnap; mirror jobs read worldMatrix and write only mirrored. MatSnapKey
    // does not read mirrored. No candidate storage growth occurs in this phase.
    std::optional<Sys::CpuTaskScope> prepScope;
    const uint32_t prepLanes = (std::min)(4u, TaskSystem::Get().GetWorkerCount());
    auto addPrepChunks = [prepLanes](std::vector<std::function<void()>>& branches,
                                   size_t count, bool materials, auto fn) {
        const size_t grain = materials ? 64 : 2048;
        const size_t chunks = (std::min)(size_t(prepLanes), (count + grain - 1) / grain);
        for (size_t lane = 0; lane < chunks; ++lane)
        {
            const size_t lo = count * lane / chunks, hi = count * (lane + 1) / chunks;
            branches.emplace_back([lo, hi, materials, fn] {
                ZoneNamedN(materialZone, "BS.PrepareCPU.MaterialChunk", materials);
                ZoneNamedN(mirrorZone, "BS.PrepareCPU.MirrorChunk", !materials);
                for (size_t i = lo; i < hi; ++i) fn(i);
            });
        }
    };
    if (scenePrepGraph)
    {
        prepScope.emplace(1 + TaskSystem::Get().GetWorkerCount());
        std::vector<std::function<void()>> branches;
        branches.reserve(prepLanes);
        DrawCandidate* rows = candidates.data();
        addPrepChunks(branches, candidates.size(), false, [rows, mirror](size_t i) { mirror(rows[i]); });
        if (extraRows && !extraRows->empty())
        {
            DrawCandidate* extra = extraRows->data();
            addPrepChunks(branches, extraRows->size(), false, [extra, mirror](size_t i) { mirror(extra[i]); });
        }
        prepScope->Start(Sys::RunCpuBatch(std::move(branches)));
    }

    // ---- Phase 2: material snapshots, then sort + emit ----
    // Table mutation is SERIAL. Do not fold it into the gathers: the MeshLibRef gather
    // runs across TaskSystem workers, so several threads would push_back into one vector
    // and mutate one dedup map. Existing volatile slots are refreshed in parallel only
    // after all storage growth finishes. Rows that already carry a matSnap (retained
    // cache rows) are skipped.
    {
        ZoneScopedN("BS.MatSnapshots");
        // Per-frame memo: see acquireRow for why it cannot outlive one frame.
        std::memset(m_matSnapMemo, 0, sizeof(m_matSnapMemo));

        bool serialRefresh = false;
#if WITH_HARNESS
        static const bool s_forceSerial = Sys::CommandLine::Get().Has("-matsnapserial");
        serialRefresh = s_forceSerial;
#endif
        // This build owns the scratch until its join. Refresh workers only
        // copy components: no World mutation, table growth, callbacks or
        // recursive BuildRenderScene calls are permitted in these jobs.
        thread_local std::vector<MatSnapshotCopy> t_refreshes;
        t_refreshes.clear();

        // A volatile refresh may copy only customParams values unless its material went
        // through SetDirty this frame (SyncMaterials' matDirty, complete by now). matDirty
        // is only collected with the retained scene diff, so without it every refresh
        // stays a whole copy. -matsnapparamoff is the reverse arm (always a whole copy).
        static const bool s_paramRefreshOff = Sys::CommandLine::Get().Has("-matsnapparamoff");
        const bool paramRefresh = !s_paramRefreshOff && m_retainedSceneDiff;
        thread_local std::vector<const MaterialComponent*> t_dirtyMats;
        t_dirtyMats.clear();
        if (paramRefresh && !m_sceneMatDirty.empty())
            if (const auto* pMat = world.GetPool<MaterialComponent>())
            {
                for (Entity e : m_sceneMatDirty)
                    if (const MaterialComponent* dm = pMat->Get(e)) t_dirtyMats.push_back(dm);
                std::sort(t_dirtyMats.begin(), t_dirtyMats.end());
            }
        m_matSnapFastRefreshes.store(0, std::memory_order_relaxed);
        m_matSnapFullRefreshes.store(0, std::memory_order_relaxed);

        auto acquireRow = [&](DrawCandidate& c)
        {
            if (!c.mc || c.matSnap != 0xFFFFFFFFu) return;

            // Direct-mapped memo keyed on the material pointer: ~8,500 candidates a frame
            // resolve ~600 distinct snapshots and arrive in gather order (the sort that groups
            // materials is in SortAndEmit), so most lookups repeat. Valid for one frame only:
            // retirement can hand an index to a different material, but RetireMatSnapshots
            // runs once before this pass and nothing inside it retires; a mid-pass rehash
            // moves slots, not indices. Cleared at the top of the pass.
            const size_t memoSlot =
                (reinterpret_cast<uintptr_t>(c.mc) >> 4) & (kMatSnapMemo - 1);
            MatSnapMemo& mm = m_matSnapMemo[memoSlot];
            if (mm.mc == c.mc && mm.hash == c.matHash)
            {
                // The epoch stamp is NOT optional on a memo hit: it is what
                // keeps the snapshot alive through RetireMatSnapshots.
                m_matSnapEpoch[mm.idx] = m_materialFrameEpoch;
                c.matSnap = mm.idx;
                return;
            }
            c.matSnap = AcquireMatSnapshot(c.mc, MatSnapKey(c),
                                          serialRefresh ? nullptr : &t_refreshes);
            mm.mc = c.mc; mm.hash = c.matHash; mm.idx = c.matSnap;
        };
        if (retainNow)
        {
            // The cached rows already carry their matSnap. Walk only the rows produced this
            // frame plus the cached rows whose material is volatile (the hit refreshes their
            // snapshot), in list order, and stamp the retained slots (what the skipped
            // acquisitions would have done) so RetireMatSnapshots keeps them.
            // A volatile cached row is re-armed (~0) before its acquisition: a lane-assembled
            // frame acquires it IN the cache, and the next frame's copy must not carry that
            // slot past the refresh. With the lane assembling the cached rows are not in
            // `candidates`; the volatile ones are acquired in m_candCache, so the lane's copy
            // carries this frame's matSnap.
            const size_t cachePrims = m_candCachePrimCount;
            auto volatileRow = [&](uint32_t k) -> DrawCandidate& {
                if (laneAssembleNow) return m_candCache[k];
                return candidates[k < cachePrims ? k : cullBegin + (k - cachePrims)];
            };
            auto acquireVolatile = [&](uint32_t k) {
                DrawCandidate& c = volatileRow(k);
                c.matSnap = 0xFFFFFFFFu;
                acquireRow(c);
            };
            const std::vector<uint32_t>& vol = m_candCacheVolatile;
            size_t vi = 0;
            for (; vi < vol.size() && vol[vi] < cachePrims; ++vi) acquireVolatile(vol[vi]);
            for (size_t i = cachePrimsNow; i < cullBegin; ++i) acquireRow(candidates[i]);
            for (; vi < vol.size(); ++vi) acquireVolatile(vol[vi]);
            for (size_t i = cacheMeshEndNow; i < candidates.size(); ++i) acquireRow(candidates[i]);
            for (const auto& [idx, key] : m_candCacheSnapKeys)
                m_matSnapEpoch[idx] = m_materialFrameEpoch;
        }
        else
            for (DrawCandidate& c : candidates) acquireRow(c);
        if (extraRows)
            for (DrawCandidate& c : *extraRows) acquireRow(c);   // the procedural scratch

        // Allocation, retirement and index acquisition are finished. After memo
        // collisions a slot can appear several times: keep its LAST writer and give each
        // destination to one job. lastRefresh records the final occurrence per destination
        // (every consulted entry is overwritten first, so it is never cleared), then the
        // list is compacted in place. Destinations are distinct afterwards, so the copy
        // order does not matter.
        thread_local std::vector<size_t> t_lastRefresh;
        t_lastRefresh.resize(m_matSnapshots.size());
        auto compactRefreshes = [&](std::vector<MatSnapshotCopy>& copies) {
            for (size_t i = 0; i < copies.size(); ++i) t_lastRefresh[copies[i].first] = i;
            size_t unique = 0;
            for (size_t i = 0; i < copies.size(); ++i)
                if (t_lastRefresh[copies[i].first] == i) copies[unique++] = copies[i];
            copies.resize(unique);
        };
        compactRefreshes(t_refreshes);
        const bool parallel = t_refreshes.size() >= 64;
        {
            ZoneScopedN("BS.MatSnapshots.Refresh");
            ZoneValue(t_refreshes.size());
            // Capture this thread's storage explicitly: naming the TLS vector
            // inside a worker would resolve to that worker's empty vector.
            const MatSnapshotCopy* jobs = t_refreshes.data();
            const MaterialComponent* const* dirtyBegin = t_dirtyMats.data();
            const MaterialComponent* const* dirtyEnd = dirtyBegin + t_dirtyMats.size();
            auto copy = [this, jobs, paramRefresh, dirtyBegin, dirtyEnd](uint32_t i) {
                const auto& [idx, source] = jobs[i];
                const bool paramsOnly = paramRefresh &&
                    !std::binary_search(dirtyBegin, dirtyEnd, source);
                RefreshVolatileSnapshot(idx, *source, paramsOnly);
            };
            if (scenePrepGraph)
            {
                // Both arrays have stable storage until scope.Join(). Leaf
                // jobs write disjoint rows; only the owner calls SortAndEmit.
                if (parallel)
                {
                    // Ready leaf jobs need no coroutine launch/resume hop.
                    // Scale the expensive material copies independently of the
                    // cheap mirror loop, and keep the owner doing useful work.
                    const uint32_t count = static_cast<uint32_t>(t_refreshes.size());
                    const uint32_t chunks = (std::min)(TaskSystem::Get().GetWorkerCount(), (count + 15u) / 16u);
                    auto run = [copy](uint32_t lo, uint32_t hi) {
                        ZoneScopedN("BS.PrepareCPU.MaterialChunk");
                        for (uint32_t i = lo; i < hi; ++i) copy(i);
                    };
                    const bool ownerDoesLights = earlyLightData && chunks > 1;
                    for (uint32_t lane = ownerDoesLights ? 0u : 1u; lane < chunks; ++lane)
                        prepScope->Submit([run, count, lane, chunks] {
                            run(count * lane / chunks, count * (lane + 1) / chunks);
                        });
                    if (!ownerDoesLights) run(0, count / chunks);
                }
                else
                    for (uint32_t i = 0; i < t_refreshes.size(); ++i) copy(i);
                if (earlyLightData) {
                    const LightPrepInputs in = CaptureLightPrepInputs();
                    BuildScene_PrepareLightData(in, false);
                    LogLightPrepDigest(in, "early");
                    lightDataPrepared = true;
                }
                prepScope->Join();
                if (scenePrepVerify)
                {
                    bool match = true;
                    auto check = [&match](const DrawCandidate& c)
                    {
                        // Preserve the serial expression and input precision.
                        const auto& m = c.worldMatrix;
                        const float det = m._11 * (m._22*m._33 - m._23*m._32)
                            - m._12 * (m._21*m._33 - m._23*m._31)
                            + m._13 * (m._21*m._32 - m._22*m._31);
                        match = match && c.mirrored == (det < 0.0f ? 1u : 0u);
                    };
                    for (const auto& c : candidates) check(c);
                    if (extraRows)
                        for (const auto& c : *extraRows) check(c);
                    if (!ENGINE_CHECK(match, "Scene preparation graph mirror mismatch"))
                        throw std::runtime_error("Scene preparation graph oracle failed");
                    if (m_materialFrameEpoch % 60 == 0)
                        LOG_INFO("[SCENE_PREP_GRAPH] candidates=%zu refreshes=%zu match=%u", candidates.size(), t_refreshes.size(), match ? 1u : 0u);
                }
            }
            else if (parallel)
                TaskSystem::Get().ParallelFor(0, static_cast<uint32_t>(t_refreshes.size()), copy);
            else
                for (uint32_t i = 0; i < t_refreshes.size(); ++i) copy(i);
        }
        // SortAndEmit, the first snapshot consumer, runs only after the join.
        // -phaselog_every=N: log [MATSNAP_JOBS] every N frames.
        static const int s_copyLogEvery = Sys::CommandLine::Get().GetInt("-phaselog_every", 0);
        if (s_copyLogEvery > 0 && m_materialFrameEpoch % s_copyLogEvery == 0)
            LOG_INFO("[MATSNAP_JOBS] parallel=%u copies=%zu serialBaseline=%u paramsOnly=%u whole=%u paramRefresh=%u",
                     parallel ? 1u : 0u, t_refreshes.size(), serialRefresh ? 1u : 0u,
                     m_matSnapFastRefreshes.load(std::memory_order_relaxed),
                     m_matSnapFullRefreshes.load(std::memory_order_relaxed), paramRefresh ? 1u : 0u);
        static const bool s_paramVerify = Sys::CommandLine::Get().Has("-matsnapparamverify");
        if (s_paramVerify && m_materialFrameEpoch % 60 == 0)
            LOG_INFO("[MATSNAP_PARAM] verified=%llu mismatches=%llu wholeBecause scalar=%llu leased=%llu keys=%llu",
                     static_cast<unsigned long long>(m_matSnapParamVerified.load()),
                     static_cast<unsigned long long>(m_matSnapParamMismatches.load()),
                     static_cast<unsigned long long>(m_matSnapWhyScalar.load()),
                     static_cast<unsigned long long>(m_matSnapWhyLeased.load()),
                     static_cast<unsigned long long>(m_matSnapWhyKeys.load()));
        ENGINE_CHECK_ONCE(m_matSnapParamMismatches.load(std::memory_order_relaxed) == 0,
                          "[MATSNAP_PARAM] a params-only refresh left a snapshot that differs "
                          "from its source");
    }

    } // BS.PrepareCPU.Graph / .Legacy cover identical preparation work.

    // ---- Write the cache's mirror bits + matSnaps back ----
    // Here, after the mirror and the snapshot acquisition and before the classify
    // compacts the list. On the frame that built the cache the rows are found through
    // m_candCacheSrc; on a patch frame that could not use the retention (and so
    // re-acquired every row) they sit at their fixed ranges. A volatile material keeps
    // ~0 in the cache: it must re-acquire every frame. (-brssplit rewrites the cache
    // from its early lanes and skips this.)
    if (retained && !m_candRetainOffThisFrame && !s_retainLateOrder && !BrsSplitEnabled() &&
        (m_candCacheWriteBack || (patched && !retainNow)))
    {
        ZoneScopedN("BS.CandRetain.WriteBack");
        PROF_SCOPE("BRS.CandRetain.WriteBack");
        m_candCacheSnapKeys.clear();
        m_candCacheVolatile.clear();
        const size_t cachePrims = m_candCachePrimCount;
        bool anySnap = false;
        for (size_t k = 0; k < m_candCache.size(); ++k)
        {
            const size_t ci = m_candCacheWriteBack ? m_candCacheSrc[k]
                : (k < cachePrims ? k : cullBegin + (k - cachePrims));
            const DrawCandidate& src = candidates[ci];
            DrawCandidate& dst = m_candCache[k];
            dst.mirrored = src.mirrored;
            if (src.mc && (src.matBits & MB_FRAME_PARAMS))
            {
                dst.matSnap = 0xFFFFFFFFu;
                m_candCacheVolatile.push_back(static_cast<uint32_t>(k));
                continue;
            }
            dst.matSnap = src.matSnap;
            if (src.matSnap != 0xFFFFFFFFu)
            {
                m_candCacheSnapKeys.emplace_back(src.matSnap, m_matSnapKeyOf[src.matSnap]);
                anySnap = true;
            }
        }
        std::sort(m_candCacheSnapKeys.begin(), m_candCacheSnapKeys.end());
        m_candCacheSnapKeys.erase(std::unique(m_candCacheSnapKeys.begin(), m_candCacheSnapKeys.end()),
                                  m_candCacheSnapKeys.end());
        m_candCacheHasSnaps = anySnap;
        m_candCacheRetained = true;
        m_candCacheSnapGen  = m_matSnapClearGen;
    }
    m_candCacheWriteBack = false;   // m_candCacheSrc is this frame's only
#if WITH_HARNESS
    // -candretainverify: every cached row that skipped the acquisition names a
    // slot that holds ITS key (what the acquisition would have found), and its
    // kept mirror bit is its matrix's.
    if (retainNow)
    {
        static const bool s_retainVerify = Sys::CommandLine::Get().Has("-candretainverify");
        if (s_retainVerify)
        {
            uint32_t checked = 0, badSnap = 0, badMirror = 0;
            auto check = [&](const DrawCandidate& c) {
                if (c.mirrored != MirrorBitFor(c.worldMatrix)) ++badMirror;
                if (!c.mc || (c.matBits & MB_FRAME_PARAMS)) return;
                ++checked;
                if (c.matSnap >= m_matSnapKeyOf.size() || m_matSnapKeyOf[c.matSnap] != MatSnapKey(c))
                    ++badSnap;
            };
            if (laneAssembleNow)   // the rows the lane will copy
                for (const DrawCandidate& c : m_candCache) check(c);
            else
            {
                for (size_t i = 0; i < cachePrimsNow; ++i) check(candidates[i]);
                for (size_t i = cullBegin; i < cacheMeshEndNow; ++i) check(candidates[i]);
            }
            ENGINE_CHECK_ONCE(badSnap == 0 && badMirror == 0,
                "[CAND_RETAIN] retained cache rows disagree: badSnap=%u badMirror=%u of %u",
                badSnap, badMirror, checked);
            if (m_materialFrameEpoch % 60 == 0 || badSnap || badMirror)
                LOG_INFO("[CAND_RETAIN] frames=%llu fallbacks=%llu refreshSkips=%llu checked=%u "
                         "badSnap=%u badMirror=%u slots=%zu volatile=%zu match=%u",
                         static_cast<unsigned long long>(m_candRetainFrames),
                         static_cast<unsigned long long>(m_candRetainFallbacks),
                         static_cast<unsigned long long>(m_candMatRefreshSkips),
                         checked, badSnap, badMirror, m_candCacheSnapKeys.size(),
                         m_candCacheVolatile.size(), (badSnap || badMirror) ? 0u : 1u);
        }
    }
#endif
    // The default order classifies AFTER the snapshots.
    if (!s_matSnapLate)
    {
        _bzMarkTo(7, 5);
        if (classifyOnLane)
        {
            // Hand the classify to the lane. Main still does everything that does not depend
            // on its output.
            ENGINE_CHECK_ONCE(!m_classifyOnLaneThisFrame && !m_lightPrepOnLaneThisFrame &&
                              !m_laneAssembleThisFrame,
                "[LANECLASSIFY] the previous frame's lane classify / light prep / "
                "assembly never ran");
            // With -laneassemble the lane's list starts with the cached prims, so its
            // classify boundary sits that many rows further on.
            const size_t laneCullBegin =
                laneAssembleNow ? m_candCachePrimCount + cullBegin : cullBegin;
            m_laneClassifyIn = CaptureClassifyInputs(laneCullBegin, camForwardThisFrame);
            m_classifyOnLaneThisFrame = true;
            m_laneAssembleThisFrame   = laneAssembleNow;
            m_laneDynPrimCount        = static_cast<uint32_t>(cullBegin);
            // Read by BuildScene_UploadLights below (main keeps only the capture and
            // ClusterPass META); consumed by the lane.
            m_lightPrepOnLaneThisFrame = lightPrepOnLane;
            // triScene part 2 sums the procedural rows, which are exactly what
            // the lane appends past the classify - no need to wait for it.
            if (m_frameStats.triSceneValid)
            {
                uint64_t extra = 0;
                for (const DrawCandidate& c : procedural)
                    if (c.filter != DrawFilter::Custom)
                        extra += uint64_t(c.indexCount / 3u);
                m_frameStats.triScene += extra;
            }
            // A reserve hint: the unclassified size is an upper bound.
            s_lastCandidateCount = static_cast<uint32_t>(candidates.size() + procedural.size() +
                                                         (laneAssembleNow ? m_candCache.size() : 0));
            s_lastFrameLightCount = static_cast<uint32_t>(m_frameLights.size());
            s_lastVolLightCount   = static_cast<uint32_t>(VolLightsWrite().size());
#if WITH_HARNESS
            static const bool s_laneClassifyVerify =
                Sys::CommandLine::Get().Has("-laneclassifyverify");
            if (s_laneClassifyVerify)
            {
                if (laneAssembleNow)
                {
                    std::vector<DrawCandidate> assembled;
                    AssembleCachedRowsInto(assembled, candidates, static_cast<uint32_t>(cullBegin));
                    BuildLaneClassifyReference(world, assembled, procedural);
                }
                else
                    BuildLaneClassifyReference(world, candidates, procedural);
            }
            // -laneassembleverify: main assembles the same list at the hand-off,
            // the lane compares its own row by row - any write to the cache or
            // to main's list between the two shows up here.
            static const bool s_laneAssembleVerify =
                Sys::CommandLine::Get().Has("-laneassembleverify");
            if (s_laneAssembleVerify && laneAssembleNow)
            {
                AssembleCachedRowsInto(m_laneAssembleRef, candidates, static_cast<uint32_t>(cullBegin));
                m_laneAssembleRefValid = true;
            }
#endif
        }
        else
            classifyAndAppend();
        _bzMarkTo(5, 8);
    }
    else
        _bzMarkTo(7, 8);
#if WITH_HARNESS
    // ---- -brssplitverify: the byte-identity oracle ----
    // The split relocates exactly three per-candidate fields (mc, matBits, mirrored);
    // the sort, emit, instance and material writes are unchanged, over the same array
    // in the same order. Proving those three fields match the serial path therefore
    // proves the DrawPacket stream is byte-identical; comparing a second packet list
    // would compare the emit code with itself.
    {
        static const bool s_verifySplit = Sys::CommandLine::Get().Has("-brssplitverify");
        if (s_verifySplit)
        {
            ZoneScopedN("BS.Split.Oracle");
            uint32_t mirrorBad = 0, matBad = 0;
            for (const DrawCandidate& c : candidates)
                if (c.mirrored != MirrorBitFor(c.worldMatrix)) ++mirrorBad;
            if (classifyOnLane)   // not appended yet: the lane does it
                for (const DrawCandidate& c : procedural)
                    if (c.mirrored != MirrorBitFor(c.worldMatrix)) ++mirrorBad;
            // Only on a split frame: with the retained cache disarmed the cache
            // rows are last-gather leftovers and mean nothing.
            if (brsSplit)
            {
                const auto* pMat = world.GetPool<MaterialComponent>();
                for (const DrawCandidate& c : m_candCache)
                {
                    const auto* fresh = pMat ? pMat->Get(c.entity) : nullptr;
                    if (c.mc != fresh || c.matBits != ComputeMatBits(fresh)) ++matBad;
                }
            }
            const bool match = (mirrorBad == 0 && matBad == 0);
            ENGINE_CHECK(match,
                "[BRS_SPLIT_ORACLE] static/dynamic split diverged from the "
                "serial path: mirrorBad=%u matBad=%u (split=%u)",
                mirrorBad, matBad, brsSplit ? 1u : 0u);
            LOG_INFO("[BRS_SPLIT_ORACLE] frame=%llu split=%u cands=%zu cached=%zu "
                     "resume=%zu mirrorBad=%u matBad=%u match=%u",
                     static_cast<unsigned long long>(m_materialFrameEpoch),
                     brsSplit ? 1u : 0u, candidates.size(), m_candCache.size(),
                     brsMirrorResumeFrom, mirrorBad, matBad, match ? 1u : 0u);
        }
    }
#endif
    BuildScene_SortAndEmit(candidates);
    _bzMark(8);

    // Phase 4: upload lights + sync IBL.
    BuildScene_UploadLights(world, lightDataPrepared);
#if WITH_HARNESS
    if (earlyLightData && m_materialFrameEpoch % 60 == 0)
        LOG_INFO("[LIGHT_PREP] early=1 lights=%zu spotViews=%u pointViews=%u",
            m_frameLights.size(), SceneEmitWrite().spotCullCount, SceneEmitWrite().pointCullCount);
#endif
    _bzMark(9);

    // Phase 5: pack reflection probes into GPU StructuredBuffer (placement/bounds/count only),
    // the ones BuildScene_SelectProbes chose at the top of this function.
    BuildScene_UploadProbes(world);
    _bzMark(10);

    // Phase 5b: DDGI volume scan + tick; must run before LightingPass binds the DDGI
    // SRVs.
    BuildScene_UpdateDDGI(world);

    // Phase 6: terrain heightmap sync + CB upload + arm TerrainPass.
    BuildScene_SyncTerrain(world);

    // Phase 6b: grass + water. Both read TerrainComponent's mutable heightmap fields
    // that SyncTerrain just refreshed; keep them after it.
    BuildScene_SyncGrass(world);
    // Ripple sim Sync must run BEFORE SyncWater so each ripple-enabled body's
    // WaterComponent::rippleFieldSRV + sim-window footprint are published for the
    // water draw to capture. Camera XZ seeds where the local sim window sits.
    if (m_waterRippleSystem)
        m_waterRippleSystem->Sync(world, FrameViewWrite().worldDeltaTime,
                                  FrameViewWrite().view.cameraPosition.x, FrameViewWrite().view.cameraPosition.z);
    BuildScene_SyncWater(world);

    // ---- Material preview: EXTRACT, and it goes LAST ----
    // Mirrors ExtractProbeBakeRequest's placement at the tail of the probe selection.
    // Everything the record half needs is a value copy by this point, so the render
    // half's material-preview pass reads no World at all.
    ExtractPreviewRequests();

    // -brssplit: the early result is good for exactly one frame. Clearing it
    // here rather than at the next publish means a frame that never reaches
    // PrepareStaticSceneEarly (a paused editor, a scene swap) cannot consume a
    // stale one.
    m_brsEarlyReady = false;
    if (BrsSplitEnabled())
    {
        static const int s_every = Sys::CommandLine::Get().GetInt("-phaselog_every", 0);
        static uint64_t s_splitFrame = 0;
        if (s_every > 0 && ++s_splitFrame % s_every == 0)
            LOG_INFO("[BRS_SPLIT] attempts=%llu used=%llu rejected=%llu lanes=%u cached=%zu",
                     static_cast<unsigned long long>(m_brsEarlyAttempts),
                     static_cast<unsigned long long>(m_brsEarlyUsed),
                     static_cast<unsigned long long>(m_brsEarlyRejected),
                     m_brsEarlyLanes, m_candCache.size());
    }
}
