// Renderer_EmitBatches.cpp - the second half of the scene emit: the stable
// instance slots (acquire / free / reset, staging a record, the draw-local
// lookups), the material-snapshot oracle switch (DX12_VERIFY_MATSNAP) and
// BuildScene_EmitBatches - the emit nest and the post-nest tail, a pure
// consumer of the EmitContext BuildScene_PlanEmit produced.

#include "Renderer/Renderer.h"

// engine graphics / backend
#include "Graphics/GraphicsDX12.h"
#include "Renderer/MaterialPermutation.h"   // MaterialShaderReflection
#include "System/HostPolicy.h"
#include <intrin.h>   // __rdtsc - [EMITNEST] owner-nest breakdown (harness diagnostic)

// render passes
#include "RenderGraph/RenderPass/GBufferPass.h"

// ECS components + systems
#include "ECS/MaterialSchema.h"

// engine systems
#include "System/JobDefaults.h"
#include "System/CommandLine.h" // -phaselog_every gate for the staging census
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

// The material bits and the material-packing helpers Renderer_Scene.cpp and
// its splits share.
#include "Renderer/RendererSceneShared.h"

// DX12_VERIFY_MATSNAP=1 arms the material-snapshot oracle. A namespace-scope
// bool initialised once, not a function-local static: a static in the
// per-batch path pays MSVC's thread-safe-init guard on every call.
static const bool s_verifyMatSnap = []{
    char* b = nullptr; size_t n = 0;
    bool on = false;
    if (_dupenv_s(&b, &n, "DX12_VERIFY_MATSNAP") == 0 && b) { on = (b[0]=='1'); std::free(b); }
    return on;
}();

// Same reason MatBufChecksumArmed exists: the -brslane ladder (FramePipeline)
// refuses under this oracle (it dereferences a MaterialComponent pool pointer
// inside the emit nest), and one switch keeps the two sides from disagreeing.
bool Renderer::MatSnapOracleArmed() { return s_verifyMatSnap; }


// ==== Stable instance slots (design: the instance-slot members in Renderer.h) ====


uint32_t Renderer::AcquireInstanceSlot(Entity e)
{
    const uint32_t id = static_cast<uint32_t>(e);
    if (id >= m_instSlotOfEntity.size())
    {
        const size_t grown = (std::max)(static_cast<size_t>(id) + 1,
                                        m_instSlotOfEntity.size() * 2);
        m_instSlotOfEntity.resize(grown, ~0u);
    }
    uint32_t slot = m_instSlotOfEntity[id];
    if (slot != ~0u) return slot;

    // Free list first (bounded reuse), then the high-water. Static overflow
    // returns ~0u: the caller degrades the candidate to the per-frame dynamic
    // segment, so an over-budget scene still renders correctly (slower).
    if (!m_freeInstSlots.empty())
    {
        slot = m_freeInstSlots.back();
        m_freeInstSlots.pop_back();
    }
    else if (m_instSlotNext < m_staticInstanceCap)
    {
        slot = m_instSlotNext++;
    }
    else
    {
        // Static lane exhausted: correct but a performance cliff - the entity is
        // re-staged into the dynamic segment every frame, losing the retained-instance
        // win. _ONCE because this runs per candidate per frame; the counter below
        // (reset each frame in BuildScene_PlanEmit) is what the pool panel graphs.
        ++m_instStaticLaneDeniedLastFrame;
        (void)ENGINE_CHECK_ONCE(false,
            "Renderer: static instance lane exhausted (%u/%u) — entity %u and "
            "every later static instance degrade to the per-frame dynamic "
            "segment and are re-staged every frame (retained-instance "
            "optimization bypassed). Raise RendererPoolCaps::maxInstances or "
            "lower Renderer::kDynamicInstanceReserve.",
            m_instSlotNext, m_staticInstanceCap, id);
        return ~0u;
    }

    if (m_instSlotShadow.size() <= slot)
    {
        m_instSlotShadow.resize(slot + 1);
        m_instSlotLastEmit.resize(slot + 1, 0u);
    }
    // Fresh (or recycled) slot: lastEmit=0 makes the emit's fresh test stage
    // the full record with prevWorld = world (zero TAA velocity on first sight).
    m_instSlotLastEmit[slot] = 0u;
    m_instSlotOfEntity[id]   = slot;
    // Reverse map updated at (re)assignment only: a freed slot keeps its old
    // entity until reuse, so a picking readback up to frames-in-flight stale
    // still resolves the entity that was rendered.
    m_instanceSlotToEntity[slot] = e;
    return slot;
}

void Renderer::FreeInstanceSlot(Entity e)
{
    const uint32_t id = static_cast<uint32_t>(e);
    if (id >= m_instSlotOfEntity.size()) return;
    const uint32_t slot = m_instSlotOfEntity[id];
    if (slot == ~0u) return;
    m_instSlotOfEntity[id] = ~0u;
    // Deferred reuse (>= frames-in-flight): in-flight remaps may still index
    // the slot, and the picking readback may still map it to this entity.
    m_instSlotsPendingReuse.emplace_back(slot, m_instFrameStamp);
}

// Full invalidate on world clear, flag flip or desync; safe mid-frame (see the declaration).
void Renderer::ResetInstanceSlots()
{
    m_instSlotOfEntity.assign(m_instSlotOfEntity.size(), ~0u);
    m_instSlotShadow.clear();
    m_instSlotLastEmit.clear();
    m_freeInstSlots.clear();
    m_instSlotsPendingReuse.clear();
    m_instSlotNext        = 0;
    InstCopyWrite().clear();
    m_instanceStagingUsed = 0;
}

// Append one record to the dirty-staging ring (m_instanceBuffer[frameSlot],
// repurposed) and extend the open copy range when both the staging append and
// the destination slot are contiguous: full-invalidate frames coalesce into a
// handful of CopyBufferRegions because slots are handed out in emit order.
void Renderer::StageInstanceRecord(uint32_t frameSlot, uint32_t slot,
                                   const GPUInstanceData& rec, bool shadowAlreadyUpdated)
{
    auto* staging = static_cast<GPUInstanceData*>(m_instanceBufferMapped[frameSlot]);
    // Capacity proof: each emitted instance stages at most once and the emit
    // loop caps instances at m_maxInstances == staging capacity.
    if (staging && m_instanceStagingUsed < m_maxInstances)
    {
        staging[m_instanceStagingUsed] = rec;
        bool merged = false;
        if (!InstCopyWrite().empty())
        {
            InstanceCopyRange& last = InstCopyWrite().back();
            if (last.frameSlot == frameSlot &&
                last.dstSlot + last.count == slot &&
                last.stagingFirst + last.count == m_instanceStagingUsed)
            {
                ++last.count;
                merged = true;
            }
        }
        if (!merged)
            InstCopyWrite().push_back(
                { frameSlot, m_instanceStagingUsed, slot, 1u });
        ++m_instanceStagingUsed;
    }
    // Dynamic-segment slots have no shadow (rewritten wholesale every frame).
    if (!shadowAlreadyUpdated && slot < m_staticInstanceCap && slot < m_instSlotShadow.size())
        m_instSlotShadow[slot] = rec;
}

uint32_t Renderer::SlotForDrawLocalInstance(uint32_t drawLocal) const
{
    if (!m_retainInstances) return drawLocal;
    // READ buffer: callers run on the render half with a draw-local index taken
    // from a DrawPacket, so it must be the same frame's table as the packets.
    if (drawLocal < InstanceRemapRead().size()) return InstanceRemapRead()[drawLocal];
    return drawLocal; // defensive: unmapped tail behaves like identity
}

Entity Renderer::EntityForDrawLocalInstance(uint32_t drawLocal) const
{
    const uint32_t slot = SlotForDrawLocalInstance(drawLocal);
    return (slot < m_maxInstances) ? m_instanceSlotToEntity[slot] : NullEntity;
}

// ==== BuildScene_EmitBatches: the emit nest and the post-nest tail ====
// Reads `candidates` and renderer members; re-derives none of PlanEmit's
// decisions.
void Renderer::BuildScene_EmitBatches(const std::vector<DrawCandidate>& candidates)
{
    EmitContext& ctx = m_emitCtx;
    // Was this context armed for this frame? Under -brslane PlanEmit and this
    // half run on two threads, and consuming the previous frame's context would
    // be a silent wrong picture.
    ENGINE_CHECK_ONCE(ctx.armedEpoch == m_materialFrameEpoch,
        "[EMITCTX] BuildScene_EmitBatches consumed a context armed for epoch "
        "%llu while this frame is epoch %llu - every latched flag, mapped "
        "pointer and plan row below is the PREVIOUS frame's",
        static_cast<unsigned long long>(ctx.armedEpoch),
        static_cast<unsigned long long>(m_materialFrameEpoch));
    // The seam, unpacked: bindings, not a second copy of the decisions. A latched
    // flag is bound by value and const: PlanEmit is its only writer and this body
    // must not change its mind mid-frame. An accumulator is bound by reference:
    // the nest writes it, the tail reads it, and the context stays the
    // authoritative copy.
    const bool earlyPlan            = ctx.earlyPlan;
    const bool planActive           = ctx.planActive;
    const bool planStampedMatSlots  = ctx.planStampedMatSlots;
#if WITH_HARNESS
    bool       verifySurfacePages   = ctx.verifySurfacePages;
#endif
    const bool parallelPlan         = ctx.parallelPlan;
    const bool verifyPlan           = ctx.verifyPlan;
    const bool overlapPlan          = ctx.overlapPlan;
    const bool prefixPlan           = ctx.prefixPlan;
    const bool verifyPrefixPlan     = ctx.verifyPrefixPlan;
    const bool widePrefix           = ctx.widePrefix;
    const bool deferMaterial        = ctx.deferMaterial;
    const bool verifyMaterialWrites = ctx.verifyMaterialWrites;
    const bool framePreparation     = ctx.framePreparation;
    const bool earlyMaterial        = ctx.earlyMaterial;
    const bool overlapMaterialTail  = ctx.overlapMaterialTail;
    const bool parallelEmit         = ctx.parallelEmit;
    const bool reuseEmitMemory      = ctx.reuseEmitMemory;
    const bool retainInst           = ctx.retainInst;
    const bool persistentCustom     = ctx.persistentCustom;
    const uint32_t frameSlot        = ctx.frameSlot;
    auto* const instances           = ctx.instances;
    auto* const matBuf              = ctx.matBuf;
    auto* const remapMapped         = ctx.remapMapped;
    const uint32_t* const sortedCandidates = ctx.sortedCandidates;
    CandidateEmitPlan* const emitPlan      = ctx.emitPlan;
    const uint32_t planCount        = ctx.planCount;
    const uint32_t activePlanLanes  = ctx.activePlanLanes;
    const uint32_t acquiresBeforeNest = ctx.acquiresBeforeNest;
    const uint32_t mapsBeforeNest     = ctx.mapsBeforeNest;
    const uint32_t planGatedBatches   = ctx.planGatedBatches;
    const uint32_t planMappedEntities = ctx.planMappedEntities;
    const uint32_t matSlotStamped     = ctx.matSlotStamped;
    const uint32_t planEpochBefore    = ctx.planEpochBefore;
    const uint32_t planEpochAfter     = ctx.planEpochAfter;
    const double   planMs             = ctx.planMs;
    uint32_t& dynCursor                  = ctx.dynCursor;
    bool&     dynOverflowWarned          = ctx.dynOverflowWarned;
    bool&     materialSlotOverflowWarned = ctx.materialSlotOverflowWarned;
    uint32_t& materialBatchCount         = ctx.materialBatchCount;
    uint32_t& secondaryPackets           = ctx.secondaryPackets;
    uint32_t& argsWritten                = ctx.argsWritten;
    uint32_t& secondaryArgsSkipped       = ctx.secondaryArgsSkipped;
    uint32_t& firstSkippedArg            = ctx.firstSkippedArg;
    uint32_t& snapDrivenWrites           = ctx.snapDrivenWrites;
    uint32_t& snapVerifiedWrites         = ctx.snapVerifiedWrites;
    uint32_t& reusedMaterialBindings     = ctx.reusedMaterialBindings;
    uint32_t& planVerifiedBatches        = ctx.planVerifiedBatches;
    uint32_t& planCachedSlots            = ctx.planCachedSlots;
    // Members, not thread_local, so both halves (possibly on two threads)
    // resolve them to the same object.
    const auto& indices  = m_sortIdx;
    auto& planScratch    = m_planScratch;

    // -brslane with a refused plan must never run the nest on the lane: without
    // the ResolveBatchPlan hoist the nest owns AcquirePersistentMatSlot /
    // AcquirePage / MapEntity, and main reads the surface-cache publish epoch
    // (DDGISceneAS::Gather) after the hand-off. The -brslane ladder refuses
    // -surfacepagesearlyoff at boot; a per-frame refusal (pass A's validator or
    // -batchplanrefuse) makes PlanEmit's fallback clear m_emitDeferred for that
    // frame, so the nest runs on main. This check is the tripwire for that
    // fallback: nothing downstream would notice (the emit-half oracle's `plan`
    // invariant is gated on planActive). A plan that itself ran on the lane
    // (m_planOnLaneThisFrame) is exempt: main reads nothing the nest allocates
    // after the hand-off. -brslanefallbackoff suppresses the fallback so this fires.
    ENGINE_CHECK_ONCE(!(m_emitDeferred && !planActive && !m_planOnLaneThisFrame),
        "[SURFACEPAGES] -brslane frame with a REFUSED batch plan: the legacy "
        "in-nest AcquirePersistentMatSlot/AcquirePage/MapEntity are about to "
        "run on the render-owner lane, which is the state the -brslane ladder "
        "refuses -surfacepagesearlyoff to prevent - main reads that publish "
        "epoch after the hand-off (the per-frame fallback in BuildScene_PlanEmit "
        "is what is supposed to have prevented this)");
    // The fallback's read-back: the frame PlanEmit moved to main must not run
    // the nest on a Render-context thread.
    ENGINE_CHECK_ONCE(!(ctx.laneFallback && Sys::ThreadCtx::Current() == Sys::ThreadCtx::Render),
        "[BRSLANE] the frame that fell back to main-thread emit is running the "
        "nest on a Render-context thread anyway");

    // SeamGuard releases planScope, then materialTailScope, then
    // earlyMaterialScope, then calls FinishSortEmitZones - after the last
    // statement and on every exit path, including oracles that throw.
    struct SeamGuard {
        Renderer* r;
        ~SeamGuard() {
            r->m_emitCtx.planScope.reset();
            r->m_emitCtx.materialTailScope.reset();
            r->m_emitCtx.earlyMaterialScope.reset();
            r->FinishSortEmitZones();
        }
    } seamGuard{ this };

    // The whole-emit Tracy zones are declared in both halves with the same names
    // and predicates, so per-name totals cover the whole emit; a capture shows
    // two zones per name per frame.
#ifdef TRACY_ENABLE
    ZoneTransientN(planFrameZone, parallelPlan ? "BS.SortAndEmit.ParallelPlan" : "BS.SortAndEmit.SerialPlan", true);
#endif
    ZoneNamedN(planPrefixZone, "BS.PlanPrefix.Stream", prefixPlan);
    ZoneNamedN(planFullZone, "BS.PlanPrefix.Barrier", !prefixPlan);
    ZoneNamedN(planOverlapZone, "BS.PlanPlacement.Overlap", overlapPlan);
    ZoneNamedN(planBarrierZone, "BS.PlanPlacement.Barrier", !overlapPlan);
    ZoneNamedN(materialGraphZone, "BS.MaterialCPU.Graph", deferMaterial);
    ZoneNamedN(materialSerialZone, "BS.MaterialCPU.Serial", !deferMaterial);
    ZoneNamedN(materialEarlyZone, "BS.MaterialCPU.Early", earlyMaterial);
    ZoneNamedN(materialLateZone, "BS.MaterialCPU.Late", deferMaterial && !earlyMaterial);
    ZoneNamedN(materialTailOverlapZone, "BS.MaterialTail.Overlap", overlapMaterialTail);
    ZoneNamedN(materialTailBarrierZone, "BS.MaterialTail.Barrier", !overlapMaterialTail);

    // -emitownerprofile: per-batch BS.Owner.* Tracy zones in the owner loop.
    // Diagnostic only: they perturb this short loop.
    static const bool s_profileOwner = Sys::CommandLine::Get().Has("-emitownerprofile");

    thread_local std::vector<MaterialCpuWrite> t_materialWriteScratch;
    auto& materialWrites = t_materialWriteScratch;
    materialWrites.clear();
    if (deferMaterial) materialWrites.reserve(512);
    constexpr size_t earlyChunkSize = 96;
    using EarlyMaterialChunk = std::array<MaterialCpuWrite, earlyChunkSize>;
    static thread_local std::array<std::unique_ptr<EarlyMaterialChunk>, 4> t_earlyScratch;
    auto& earlyChunks = t_earlyScratch;
    if (earlyMaterial)
        for (auto& chunk : earlyChunks) if (!chunk) chunk = std::make_unique<EarlyMaterialChunk>();
    std::vector<std::vector<uint8_t>> expectedMaterial;
    auto prepareMaterialOracle = [&](size_t begin, size_t end) {
        if (!verifyMaterialWrites) return;
        for (size_t n = begin; n < end; ++n) {
            const auto& write = materialWrites[n];
            expectedMaterial.emplace_back(write.bytes);
            std::memcpy(expectedMaterial.back().data(), write.target, write.bytes);
            auto reference = write; reference.target = expectedMaterial.back().data();
            RunMaterialCpuWrite(reference);
        }
    };
    size_t earlySubmitted = 0, earlyChunkCount = 0;
    uint32_t materialTailLanes = 0, materialTailQueued = 0;
    auto publishMaterialChunk = [&] {
        if (!earlyMaterial || earlyChunkCount == earlyChunks.size() ||
            materialWrites.size() - earlySubmitted < earlyChunkSize) return;
        auto* chunk = earlyChunks[earlyChunkCount].get();
        std::copy_n(materialWrites.data() + earlySubmitted, earlyChunkSize, chunk->data());
        prepareMaterialOracle(earlySubmitted, earlySubmitted + earlyChunkSize);
        ctx.earlyMaterialScope->Submit([this, chunk] {
            ZoneScopedN("BS.MaterialCPU.EarlyChunk");
            for (const auto& write : *chunk) RunMaterialCpuWrite(write);
        });
        earlySubmitted += earlyChunkSize;
        ++earlyChunkCount;
    };
    // Entity ownership is the parallelism boundary: duplicate candidates
    // (outline/secondary views) must observe each other's prevWorld and stamp
    // writes in the original sorted order. Allocation and publication stay
    // serial; workers never grow renderer containers or append copy ranges.
    struct InstanceJob {
        uint32_t candidate, drawLocal, material, slot = ~0u;
        bool slotValid, staticLane = false, aliasOnly = false;
    };
    struct StagedRecord { uint32_t drawLocal, slot; GPUInstanceData record; };
    struct EmitLane {
        std::vector<InstanceJob> jobs;
        std::vector<StagedRecord> staged;
        InstStageStats stats{};
    };
    thread_local std::array<EmitLane, 8> t_emitScratch;
    // Explicit pointer: a worker must not resolve the caller's TLS by name.
    auto* emitLanes = t_emitScratch.data();
    const uint32_t emitLaneCount = parallelEmit
        ? (std::min)(8u, TaskSystem::Get().GetWorkerCount()) : 1u;
    if (parallelEmit) for (uint32_t lane = 0; lane < emitLaneCount; ++lane) {
        emitLanes[lane].jobs.clear();
        emitLanes[lane].staged.clear();
        emitLanes[lane].stats = {};
    }
    uint32_t emitMaxEntity = 0;
    bool reuseStaticPrevWorld = reuseEmitMemory;
#if WITH_HARNESS
    reuseStaticPrevWorld = reuseStaticPrevWorld && !Sys::CommandLine::Get().Has("-staticprevcopy");
#endif
    auto allocateInstance = [&](uint32_t candidate, uint32_t drawLocal,
                                uint32_t material, bool slotValid) {
        InstanceJob job{candidate, drawLocal, material, ~0u, slotValid};
        const DrawCandidate& ck = candidates[candidate];
        if (retainInst) {
            uint32_t slot = ~0u;
            bool aliasOnly = false;   // read-only remap alias, never staged
            const bool staticEligible = parallelPlan ? emitPlan[drawLocal].staticEligible : IsStaticLaneInstance(ck);
            if (staticEligible) {
                if (prefixPlan) {
                    // Owner-only lookup observes earlier allocations of duplicate
                    // entities and never races a worker or requires a table copy.
                    const uint32_t entity = static_cast<uint32_t>(ck.entity);
                    if (entity < m_instSlotOfEntity.size()) slot = m_instSlotOfEntity[entity];
                } else slot = parallelPlan ? emitPlan[drawLocal].staticSlot : ~0u;
                if (slot == ~0u) slot = AcquireInstanceSlot(ck.entity);
                else ++planCachedSlots;
            }
            if (verifyPlan) {
                ENGINE_CHECK(staticEligible == IsStaticLaneInstance(ck), "emit plan changed static eligibility");
                if (staticEligible && slot != ~0u) {
                    const uint32_t entity = static_cast<uint32_t>(ck.entity);
                    ENGINE_CHECK(entity < m_instSlotOfEntity.size() && m_instSlotOfEntity[entity] == slot,
                                 "emit plan changed instance slot for entity %u", entity);
                }
            }
            const bool staticLane = (slot != ~0u);
            if (!staticLane)
            {
                if (dynCursor < m_maxInstances)
                    slot = dynCursor++;
                else
                {
                    // Pathological: more than kDynamicInstanceReserve dynamic instances AND
                    // the static segment exhausted. Alias slot 0 read-only: the draw uses slot
                    // 0's record (wrong transform for this instance) but there is no OOB read
                    // and no clobber of slot 0's live static record.
                    if (!dynOverflowWarned)
                    {
                        LOG_WARNING("Renderer: dynamic instance segment "
                                    "overflow (reserve %u) — instance "
                                    "aliased to slot 0",
                                    kDynamicInstanceReserve);
                        dynOverflowWarned = true;
                    }
                    slot      = 0;
                    aliasOnly = true;
                }
            }

            job.slot = slot;
            job.staticLane = staticLane;
            job.aliasOnly = aliasOnly;
        }
        return job;
    };
    auto emitInstance = [&](const InstanceJob& job, InstStageStats& stats, EmitLane* lane) {
        const uint32_t instIdx = job.drawLocal, thisMatIdx = job.material;
        const bool slotValid = job.slotValid;
        auto stageRecord = [&](uint32_t slot, const GPUInstanceData& rec) {
            if (!lane) { StageInstanceRecord(frameSlot, slot, rec); return; }
            lane->staged.push_back({instIdx, slot, rec});
            // Later candidates of THIS entity see the updated shadow, exactly
            // as they did when StageInstanceRecord was called inline.
            if (slot < m_staticInstanceCap && slot < m_instSlotShadow.size())
                m_instSlotShadow[slot] = rec;
        };
        ++stats.candidates;
        const DrawCandidate& ck = candidates[job.candidate];
        // Flatten the bounds lane here, before the retained / per-frame fork and
        // the aliasOnly and duplicate-emit early-outs: instIdx advances once per
        // candidate unconditionally, so InstAabbWrite()[dp.instanceOffset + inst] is
        // exact in every mode.
        if (parallelEmit) InstAabbWrite()[instIdx] = { ck.aabbMin, ck.aabbMax };
        else InstAabbWrite().push_back({ ck.aabbMin, ck.aabbMax });
        // Entity -> material slot for out-of-packet consumers (DDGISceneAS's RT
        // material proxy). Epoch-stamped, so stale entries need no clear; overflow
        // batches record "no slot" rather than aliasing slot 0.
        // Only when ResolveBatchPlan did not already stamp it: the consumer runs on
        // main between the two halves, so under -brslane a stamp from here lands
        // after the read (see StampEntityMaterialSlot for which producer owns which
        // frame). Under planActive this also keeps the emit workers off a
        // renderer-wide vector.
        if (!planStampedMatSlots)
            StampEntityMaterialSlot(ck.entity, thisMatIdx, slotValid);
        const uint32_t pwId = static_cast<uint32_t>(ck.entity);
        if (pwId >= m_prevWorldValid.size())
        {
            const size_t grown = (std::max)(static_cast<size_t>(pwId) + 1,
                                            m_prevWorldValid.size() * 2);
            m_prevWorldValid.resize(grown, 0);
            m_prevWorldDense.resize(grown);
        }

        if (retainInst)
        {
            // Retained path: stable slot + dirty staging.
            const uint32_t slot = job.slot;
            const bool staticLane = job.staticLane;
            const bool aliasOnly = job.aliasOnly;

            if (aliasOnly ||
                (staticLane && m_instSlotLastEmit[slot] == m_instFrameStamp))
            {
                // Duplicate candidate of an entity already emitted this frame (base +
                // outline / picking copy): same batch-key fields by construction, so the
                // staged record is already correct; only the remap entry differs per draw.
            }
            else
            {
                bool fresh = false, seenLastFrame = false, provablyUnchanged = false;
                if (staticLane)
                {
                    fresh = (m_instSlotLastEmit[slot] == 0u);
                    seenLastFrame =
                        (m_instSlotLastEmit[slot] + 1u == m_instFrameStamp);

                    // Provably-unchanged fast path: if the scene diff saw this entity go dirty
                    // neither this frame nor last, its transform, material and flags are
                    // unchanged and the 2-frame prevWorld rule has settled, so the GPU record is
                    // correct and the shadow need not be read (a settled 100-character scene
                    // walks ~7000 static candidates a frame and stages none; see [INSTSTAGE]
                    // under -phaselog_every). Only the emit stamp and the dense prev-world mirror
                    // are maintained. Fresh and !seenLastFrame fall through to the full path, so
                    // a culled, hidden or newly allocated slot is never skipped.
                    const uint32_t eid32 = static_cast<uint32_t>(ck.entity);
                    const uint32_t dstamp =
                        (eid32 < m_entityDirtyStamp.size()) ? m_entityDirtyStamp[eid32] : 0u;
                    provablyUnchanged =
                        !fresh && seenLastFrame && (dstamp + 1u < m_instFrameStamp);

                    if (provablyUnchanged && !m_verifyStageSkip)
                    {
                        ++stats.staticSkipped;
                        m_instSlotLastEmit[slot] = m_instFrameStamp;
                        // The full path wrote this mirror on the move / settle frame and clean
                        // static emits cannot change it, so keep it for later lane flips without
                        // copying another 64 bytes per idle entity.
                        if (!reuseStaticPrevWorld) {
                            m_prevWorldDense[pwId] = ck.worldMatrix;
                            m_prevWorldValid[pwId] = 1;
                        }
                        if (remapMapped) remapMapped[instIdx] = slot;
                        if (parallelEmit) InstanceRemapWrite()[instIdx] = slot;
            else InstanceRemapWrite().push_back(slot);
                        return;
                    }

                }

                GPUInstanceData rec;
                rec.world       = ck.worldMatrix;
                rec.meshDescIdx = ck.meshDescSlot;
                rec.materialIdx = thisMatIdx;
                // Per-instance visibility for the compaction CS. Written whether or not GPU
                // compaction is on: after a mid-session flag flip an undirtied record with
                // viewMask 0 would be culled everywhere; with compaction off nothing reads
                // it. Batch-uniform across an entity's duplicate emits (both ride the batch
                // key). instFlags' per-object-caster bit follows the same rule: the
                // shadow-view CS skips those instances while the per-object map owns the
                // character.
                rec.viewMask    = ck.viewMask;
                // Cull exemptions (GPUInstanceData.h): NoGeomCull for a custom VS, whose
                // WPO displacement no CPU-published AABB bounds. Skinned draws publish real
                // animated bounds per section every frame (SkinnedMeshSubsystem::BuildSkinJobs
                // -> SetMeshAABB), so they are frustum-culled - shadow views included - and
                // only skip Hi-Z (NoOcclusionCull). The cull never infers skinning from
                // prevPosInfo: that TAA field is 0xFFFFFFFF on a skinned mesh's first frame
                // and while paused.
                rec.instFlags   =
                    (ck.renderInMainPass      ? kInstanceFlagMainPass     : 0u) |
                    (ck.perObjectShadowCaster ? kInstanceFlagPerObjShadow : 0u) |
                    (ck.customVSID != 0       ? kInstanceFlagNoGeomCull   : 0u) |
                    (ck.skinned      ? kInstanceFlagNoOcclusionCull : 0u);
                rec.dissolveParams    = ck.dissolveParams;
                rec.dissolveEdgeColor = ck.dissolveEdgeColor;
                // .w is unused by the dissolve shading (it reads .rgb): it carries the
                // outline width, which frees OutlineGeometryPass's root constant [3] for
                // prevPosElementBase (root param 0 holds 4 dwords, 3 taken by mesh /
                // instance / material), so the outline VS computes velocity with GBuffer.vs's
                // code. Batch-uniform: outlinePixels is part of the batch key.
                rec.dissolveEdgeColor.w = ck.outlinePixels;

                if (staticLane)
                {
                    // prevWorld 2-frame rule: the move frame carries the old world (real
                    // velocity), the settle frame rewrites prev = world so TAA velocity returns
                    // to zero, then the record goes idle (no staging). A slot not emitted last
                    // frame (culled / hidden / fresh) restarts at zero velocity: moving while
                    // unseen must not smear on reappearance.
                    GPUInstanceData& sh = m_instSlotShadow[slot];
                    const bool moved = std::memcmp(&sh.world, &rec.world,
                                                   sizeof(rec.world)) != 0;
                    const bool wasMoving = std::memcmp(&sh.prevWorld, &sh.world,
                                                       sizeof(sh.world)) != 0;
                    if (fresh || !seenLastFrame) rec.prevWorld = rec.world;
                    else if (moved)              rec.prevWorld = sh.world;
                    else if (wasMoving)          rec.prevWorld = rec.world;
                    else                         rec.prevWorld = sh.prevWorld;

                    ++stats.staticExamined;
                    if (fresh) ++stats.staticFresh;
                    const bool differs =
                        fresh || std::memcmp(&sh, &rec, sizeof(GPUInstanceData)) != 0;

                    // Self-check (DX12_VERIFY_STAGESKIP=1): the full path has just read the
                    // shadow, so the fast path's "provably unchanged" claim is audited against
                    // it. A hit means the gate is unsound and would leave a stale instance on
                    // the GPU - a silent visual bug.
                    if (m_verifyStageSkip && provablyUnchanged && differs)
                    {
                        ENGINE_CHECK(false,
                            "SortAndEmit stage-skip gate is UNSOUND: entity %u slot %u was "
                            "called provably-unchanged but its record DIFFERS. The fast path "
                            "would have left a stale instance on the GPU.",
                            static_cast<uint32_t>(ck.entity), slot);
                    }

                    if (differs)
                    {
                        ++stats.staticStaged;
                        stageRecord(slot, rec);
                    }
                    m_instSlotLastEmit[slot] = m_instFrameStamp;
                    // Keep the dense prev-world cache coherent so lane flips (dissolve start,
                    // skin attach) and a retained-instances-off toggle resume with a sane prev
                    // world.
                    m_prevWorldDense[pwId] = ck.worldMatrix;
                    m_prevWorldValid[pwId] = 1;
                }
                else
                {
                    // Dynamic lane: prev world from the dense cache, record rewritten every
                    // frame.
                    rec.prevWorld = m_prevWorldValid[pwId]
                                        ? m_prevWorldDense[pwId]
                                        : ck.worldMatrix;
                    m_prevWorldDense[pwId] = ck.worldMatrix;
                    m_prevWorldValid[pwId] = 1;
                    ++stats.dynamicStaged;
                    stageRecord(slot, rec);
                    m_instanceSlotToEntity[slot] = ck.entity;
                }
            }

            if (remapMapped) remapMapped[instIdx] = slot;
            if (parallelEmit) InstanceRemapWrite()[instIdx] = slot;
            else InstanceRemapWrite().push_back(slot);
        }
        else if (instances)
        {
            ++stats.legacyStaged;
            // Per-frame path (retained instances off): compact into the upload slot.
            GPUInstanceData& inst = instances[instIdx];
            inst.world       = ck.worldMatrix;
            inst.meshDescIdx = ck.meshDescSlot;
            inst.materialIdx = thisMatIdx; // material slot in material buffer
            // GPU compaction never runs here (it needs the retained slot indirection),
            // so viewMask stays unused and zeroed.
            inst.viewMask    = 0;
            // instFlags still carries the cull exemptions: the per-command cull runs in
            // this mode too (its remapEnabled==0 branch reads instances directly). Only
            // those bits are written.
            inst.instFlags   =
                ((ck.customVSID != 0) ? kInstanceFlagNoGeomCull : 0u) |
                (ck.skinned ? kInstanceFlagNoOcclusionCull : 0u);
            // Per-instance dissolve (all-zero = shader branch off), written per
            // candidate: dissolving and intact instances of one material share a batch
            // and differ per record.
            inst.dissolveParams    = ck.dissolveParams;
            inst.dissolveEdgeColor = ck.dissolveEdgeColor;
            inst.dissolveEdgeColor.w = ck.outlinePixels;   // see the retained path

            // Prev-frame world for TAA velocity: the cached world, or the current one
            // on first sight (zero velocity); then cache this frame's. Duplicate
            // candidates of an entity share worldMatrix, so the overwrite is idempotent
            // within the frame. Dense array indexed by entity id.
            inst.prevWorld = m_prevWorldValid[pwId] ? m_prevWorldDense[pwId]
                                                    : ck.worldMatrix;
            m_prevWorldDense[pwId] = ck.worldMatrix;
            m_prevWorldValid[pwId] = 1;
        }
        if (!retainInst)
            m_instanceSlotToEntity[instIdx] = ck.entity;
    };

    // The nest's walk counters and material-slot memo. Local, not in the
    // context: nothing before the seam reads them, and `instIdx` is the counter
    // ResolveBatchPlan's instBegin is checked against - a copy in the context
    // would make that check a tautology.
    const int n = static_cast<int>(candidates.size());
    // `i` lives outside the for-header: after the loop, n - i is the number of
    // candidates the m_maxInstances cap dropped (reported after the loop). Every
    // exit path in the body sets `i = j` first, so it stays exact.
    int i = 0;
    // Frame-local, full-key equality: sorting clusters equal bindings across
    // different meshes. No slot eviction/reset occurs inside this emit loop.
    MaterialBindingKey lastBindingKey{};
    uint32_t lastBindingSlot = 0;
    bool lastBindingValid = false;
    uint32_t instIdx = 0;   // the nest's instance ordinal (see `i` above)

    uint32_t planRow = 0;
    // Set when the nest's cross-walk check stopped the frame short; the
    // rows-consumed check below then names the stop rather than reporting the
    // same event a second time under another name.
    bool planStopped = false;
    // Sentinel used when a row is missing; see the desync check in the nest.
    static const BatchPlanRow kEmptyPlanRow{};
#if WITH_HARNESS
    // Oracle counters. Every one of them is a DIFFERENCE, so the log line's
    // `match=` is the conjunction of zeros and a pack can pin it directly.
    uint32_t spBoundaryDiff = 0, spMatIdxDiff = 0, spFlagDiff = 0,
             spGateDiff = 0, spHashDiff = 0, spTdrDiff = 0, spNestBatches = 0;
    std::unordered_set<MaterialBindingKey, MaterialBindingKeyHash> spSeenKeys;
#endif

    // Latch the plan and sort-index generations the nest is about to read.
    // FramePipeline copies the published values (GetBatchPlanGen /
    // GetSortIdxGen) into the FramePacket, and FramePipeline::VerifyEmitLease
    // compares them with these (EmitLeaseMismatch) when it consumes the packet.
    m_batchPlanGenConsumed = m_batchPlanGen;
    m_sortIdxGenConsumed   = m_sortIdxGen;
    ENGINE_CHECK_ONCE(!parallelPlan || planScratch.size() >= planCount,
                      "BuildScene_SortAndEmit: emit plan holds %zu rows for %u planned candidates",
                      planScratch.size(), planCount);

    // [EMITNEST] owner-nest breakdown (-emitnestprofile, harness diagnostic).
    // Not Tracy: the BS.Owner.* zones cost ~0.5 ms/frame against a ~0.75 ms
    // nest, so the instrument would be most of the signal. __rdtsc is ~25
    // cycles (seven marks over ~2000 batches is ~0.05 ms), and the TSC->ms
    // factor comes per frame from one steady_clock bracket around the nest, so
    // no TSC frequency or invariant TSC is assumed.
    // Buckets: 0 other | 1 boundary | 2 material | 3 instPlan | 4 packet
    //          5 surfaceCache | 6 indirect.
    // The bracket is not [EMITZONE]'s `nest` slot: that slot ends at
    // EmitZoneMark(2), after BS.PlanPrefix.Drain, so it also holds the owner
    // waiting for the streamed plan lanes. Compare like with like.
    constexpr int   kEmitNestBuckets = 7;
    static double   s_emitNest[kEmitNestBuckets] = {};
    static double   s_emitNestWallMs = 0.0;
    static uint64_t s_emitNestBatches = 0, s_emitNestInstance = 0;
    static uint64_t s_emitNestBackSteps = 0;
    static int      s_emitNestN = 0;
    // [EMITRETAIN] (-emitretainprobe): the retention ceiling probe. Per batch it
    // digests the inputs a retained packet / plan would have to prove unchanged
    // and counts the batches (and their instances) whose digest matches last
    // frame's at the same ordinal: the upper bound on what a retained SortAndEmit
    // plan could skip. Nothing else reads the digests. A separate flag from
    // -emitnestprofile because the digest (~29 mixes per batch, ~0.05 ms/frame)
    // would inflate the `packet` bucket by half.
    static std::vector<uint64_t> s_emitDigestPrev, s_emitDigestCur;
    static uint64_t s_retStableBatch = 0, s_retStableInst = 0,
                    s_retTotalBatch = 0, s_retTotalInst = 0;
    s_emitDigestCur.clear();
    uint64_t retStableBatch = 0, retStableInst = 0;
    uint64_t nestAcc[kEmitNestBuckets] = {};
    uint64_t nestTick = 0, nestBatches = 0, nestInstances = 0, nestTscStart = 0;
    // Backwards TSC steps seen this frame. __rdtsc is not guaranteed monotone
    // across a core migration without an invariant, synchronised TSC, and a
    // hypervisor may rebase it. `now - nestTick` is unsigned, so one backwards
    // step would wrap to ~2^64 ticks and poison its bucket for the whole
    // -phaselog_every window: clamped to zero and counted instead.
    uint64_t nestBackSteps = 0;
    std::chrono::steady_clock::time_point nestWallStart{};
#if WITH_HARNESS
    static const bool s_nestProfile = Sys::CommandLine::Get().Has("-emitnestprofile");
    static const bool s_retainProbe = Sys::CommandLine::Get().Has("-emitretainprobe");
#else
    constexpr bool s_nestProfile = false;
    constexpr bool s_retainProbe = false;
#endif
    auto _nestMark = [&](int bucket) {
        if (!s_nestProfile) return;
        const uint64_t now = __rdtsc();
        if (now >= nestTick) nestAcc[bucket] += now - nestTick;
        else                 ++nestBackSteps;      // clamp to 0, count the step
        nestTick = now;
    };
    if (s_nestProfile) {
        nestWallStart = std::chrono::steady_clock::now();
        nestTscStart = nestTick = __rdtsc();
    }
    const bool emitProbe = s_nestProfile || s_retainProbe;
    {
    ZoneNamedN(prefixConsumeZone, "BS.PlanPrefix.Consume", prefixPlan);
    for (; i < n && instIdx < m_maxInstances; )
    {
        EnsurePlanPrefix(static_cast<uint32_t>(i));
        const DrawCandidate& first = candidates[indices[i]];
        _nestMark(0);

        // The nest's two counters must agree on every path, plan or not: `i`
        // advances at the loop tail (`i = j`), `instIdx` in the per-instance loop
        // below, so an early-out that leaves a batch before that loop would put
        // every later instance one slot off - a wrong picture, not a crash.
        // Plan rows are consumed by ordinal; `begin` and `instBegin` cross-check the
        // plan's walk against the nest's (instBegin was advanced by the plan's own
        // instance counter). Under planActive both hold by construction (pass A
        // validated contiguous rows; `j` comes from row.end), so a mismatch is a
        // tripwire for a future edit and stops the frame's emit: a short frame is
        // bounded and loud, whereas a per-batch fallback would re-run
        // AcquirePersistentMatSlot on a key the plan already stamped (silencing
        // StageMaterialSlot / PackCustomMaterialBindings and changing material
        // bytes) while the surface-cache block stays off.
        ENGINE_CHECK_ONCE(instIdx == static_cast<uint32_t>(i),
            "[SURFACEPAGES] the emit nest's instance ordinal left its candidate "
            "ordinal (instIdx=%u i=%d) - every instance after this batch is "
            "written to the wrong slot", instIdx, i);

        const bool rowPresent = planActive && planRow < m_batchPlan.size();
        const BatchPlanRow& row = rowPresent ? m_batchPlan[planRow] : kEmptyPlanRow;
        const bool rowValid = rowPresent &&
                              row.begin     == static_cast<uint32_t>(i) &&
                              row.instBegin == instIdx;
        if (planActive && !rowValid)
        {
            ENGINE_CHECK_ONCE(false,
                "[SURFACEPAGES] batch plan desync: nest at i=%d instIdx=%u, row %u "
                "of %zu begins at %u/%u - stopping this frame's emit at %llu "
                "packet(s) rather than half-degrading the allocators",
                i, instIdx, planRow, m_batchPlan.size(),
                rowPresent ? row.begin : 0u, rowPresent ? row.instBegin : 0u,
                static_cast<unsigned long long>(nestBatches));
            planStopped = true;
            break;
        }

        // A batch is the run of candidates with the same emit key (transparents
        // rarely batch: they are depth-sorted). The key has no depth and needs none:
        // a depth-sorted class arrives in painter's order, so a merged run is
        // adjacent in depth and its packet keeps their slice. In the scene emit
        // IsDepthSortedFilter feeds only the sort (key builder, fallback comparator,
        // depth refresh), never a batch-key or DrawPacket field, so
        // SecondaryViewBlend's membership changes only the order, and so how finely,
        // those duplicates batch - what CullWorldForPreview's per-packet re-sort
        // needs (see the predicate's note in RenderTypes.h).
        int j = i + 1;
        {
        ZoneNamedN(boundaryZone, "BS.Owner.BatchBoundary", s_profileOwner);
        if (rowValid) {
            j = static_cast<int>(row.end);
        } else if (parallelPlan) {
            while (j < static_cast<int>(planCount)) {
                EnsurePlanPrefix(static_cast<uint32_t>(j));
                if (emitPlan[j].startsBatch) break;
                ++j;
            }
        } else {
            while (j < n && (instIdx + static_cast<uint32_t>(j - i)) < m_maxInstances)
            {
                const DrawCandidate& cur = candidates[indices[j]];
                if (!SameEmitBatch(first, cur))
                    break;
                ++j;
            }
        }
        }
        _nestMark(1);
        if (verifyPlan) {
            int serialEnd = i + 1;
            while (serialEnd < n && (instIdx + static_cast<uint32_t>(serialEnd - i)) < m_maxInstances &&
                   SameEmitBatch(first, candidates[indices[serialEnd]])) ++serialEnd;
            ENGINE_CHECK(j == serialEnd, "emit plan batch boundary mismatch: start=%d planned=%d serial=%d", i, j, serialEnd);
            ++planVerifiedBatches;
        }

        const uint32_t batchStart = instIdx;
        const uint32_t batchCount = static_cast<uint32_t>(j - i);
        ++materialBatchCount;

        // THE batch's material, resolved once. This is the snapshot copy taken
        // at gather time - not `first.mc`, which is a pointer into pool storage
        // that swap-with-back can repoint at another entity's material.
        const MaterialComponent* batchMat =
            (first.matSnap < m_matSnapshots.size())
                ? m_matSnapshots[first.matSnap].mat.Read() : nullptr;

        const MaterialBindingKey bindingKey = BindingKeyOf(first);
        // A PER_FRAME_PARAMS material (a face): its own slot, its b8 repacked on the
        // ring every frame, its frame record written.
        const bool frameParamsBatch = bindingKey.identity != 0;

#if WITH_HARNESS
        // [SURFACEPAGES_ORACLE], part 1: boundary + material slot. Not "run the
        // emit twice and compare": each check re-derives one plan decision from the
        // sorted array by an independent route and counts disagreements.
        if (verifySurfacePages && rowValid)
        {
            ++spNestBatches;
            int serialEnd = i + 1;
            while (serialEnd < n &&
                   (instIdx + static_cast<uint32_t>(serialEnd - i)) < m_maxInstances &&
                   SameEmitBatch(first, candidates[indices[serialEnd]])) ++serialEnd;
            if (row.end != static_cast<uint32_t>(serialEnd)) ++spBoundaryDiff;
            // A READ-ONLY probe of the allocator's map: the oracle must not
            // disturb the state machine it is checking.
            auto vit = m_persistentMatSlots.find(bindingKey);
            if (vit != m_persistentMatSlots.end())
            {
                if (vit->second.slot != row.matIdx) ++spMatIdxDiff;
            }
            else if (row.slotValid) ++spMatIdxDiff;   // a valid slot is always mapped
            if (row.slotValid)
            {
                // firstSight must be true for exactly the FIRST row carrying
                // this key this frame, in emit order, and false for the rest.
                const bool firstHere = spSeenKeys.insert(bindingKey).second;
                if ((row.firstSight != 0) != firstHere) ++spFlagDiff;
            }
            if (row.needsBaseWrite && !row.firstSight) ++spFlagDiff;
        }
#endif

        // Persistent material slot. firstSight = first batch carrying this key this
        // frame (drives the once-per-key custom CBV/SRV repack); needsBaseWrite =
        // this frameSlot's buffer does not hold the record yet. Other batches (and
        // reused keys) skip every write and only reference the slot.
        bool firstSight = false, needsBaseWrite = false, slotValid = true;
        uint32_t thisMatIdx;
        _nestMark(0);
        {
        ZoneNamedN(materialZone, "BS.Owner.MaterialBinding", s_profileOwner);
        if (rowValid)
        {
            // ResolveBatchPlan already ran AcquirePersistentMatSlot for this batch. A
            // second call would report firstSight / needsBaseWrite false (the first call
            // stamps lastSeenEpoch), silencing StageMaterialSlot and
            // PackCustomMaterialBindings and changing the material bytes: read the row,
            // never re-acquire.
            thisMatIdx     = row.matIdx;
            firstSight     = row.firstSight     != 0;
            needsBaseWrite = row.needsBaseWrite != 0;
            slotValid      = row.slotValid      != 0;
        }
        else if (lastBindingValid && bindingKey == lastBindingKey)
        {
            // A prior successful lookup already stamped this frame's epoch
            // and frame bit. Repeated sightings have no remaining side effect.
            thisMatIdx = lastBindingSlot;
        }
        else
        {
            thisMatIdx = AcquirePersistentMatSlot(
                bindingKey, frameSlot, firstSight, needsBaseWrite, slotValid);
            lastBindingKey = bindingKey;
            lastBindingSlot = thisMatIdx;
            // Overflow must retry the original path, preserving its
            // first-sight reporting and reserved neutral slot semantics.
            lastBindingValid = slotValid;
        }
        if (!firstSight)
        {
            ++reusedMaterialBindings;
        }
        else
        {
            if (!slotValid && !materialSlotOverflowWarned)
            {
                LOG_WARNING("Renderer material buffer full (%u unique bindings); "
                            "overflow draws use slot 0 and further warnings are "
                            "suppressed for this frame", m_maxMaterials);
                materialSlotOverflowWarned = true;
            }

            // Never write through on overflow: slot 0 is the reserved neutral
            // record (terrain/water/no-material also read it). Overflow draws
            // degrade to that neutral appearance; existing draws remain
            // deterministic instead of changing underneath them.
            if (slotValid)
            {
            // Custom reflection only when the custom PS resolved; else the slots stay
            // zeroed. It is the reflection the material was synced from (permutation 0
            // merged with the variant it draws with), so a texture only that variant
            // samples is bound too.
            const ShaderReflect::Reflection* customRefl = nullptr;
            if (first.customPSID > 0 && m_gbufferPass)
                customRefl = batchMat
                    ? MaterialShaderReflection(m_gbufferPass->GetShaderLibrary(), first.customPSID, *batchMat)
                    : MaterialShaderInterface(m_gbufferPass->GetShaderLibrary(), first.customPSID);

            // The base record is written only when this frameSlot's staging buffer
            // lacks it. Content is immutable per key, so once all 3 staging buffers hold
            // it the write is skipped for the rest of the key's life; custom CBV/SRV
            // still re-pack on every first sight (per-frame ring allocations).
            // The same gate feeds the default-heap GPU table: StageMaterialSlot queues
            // exactly the slots written here and Render() copies them ahead of this
            // frame's first draw, so a steady state makes no copies. An editor slider
            // drag gives a new matHash -> new key -> fresh slot -> needsBaseWrite -> a
            // copy before the G-Buffer reads it.
            // The runtime is resolved from the entity at use, never cached (see
            // MatSnapshot), once per first-sight batch and shared by WriteMaterialSlot
            // and PackCustomMaterialBindings: the record and the custom bindings must
            // see the same runtime.
            const ResolvedMaterialRuntime firstRt = ResolveMaterialRuntime(first.entity);
            if (needsBaseWrite)
            {
                // The snapshot is the producer: `first.mc` is a raw pool pointer, and this
                // may run on the render-owner lane, where it must not be dereferenced.
                if (batchMat) ++snapDrivenWrites;
                if (deferMaterial && matBuf) {
                    // Refresh mutable shared snapshot cache before workers read it.
                    if (batchMat && (batchMat->flags & MaterialComponent::GPU_LINEAR_CACHE_DIRTY)) RefreshLinearCache(*batchMat);
                    materialWrites.push_back({&matBuf[thisMatIdx], sizeof(Resource::MaterialGPUData), true, batchMat, firstRt, customRefl, nullptr, first.matSnap, frameParamsBatch});
                }
                // The live-vs-snapshot oracle below needs the record written now.
                if (!deferMaterial || s_verifyMatSnap)
                    WriteMaterialSlot(matBuf, thisMatIdx, batchMat, firstRt, customRefl, frameParamsBatch);

                // Oracle, off unless DX12_VERIFY_MATSNAP=1. Snapshot storage is reused, so
                // a member that MaterialComponent's operator= fails to overwrite leaks stale
                // bytes into the GPU record - a subtly wrong material, not an error. Run
                // with it on after adding a member to MaterialComponent.
                if (s_verifyMatSnap && first.mc && batchMat != first.mc)
                {
                    ++snapVerifiedWrites;
                    static Resource::MaterialGPUData s_verify;
                    s_verify = Resource::MaterialGPUData{};
                    WriteMaterialSlot(&s_verify, 0, first.mc,
                                      ResolveMaterialRuntime(first.entity), customRefl,
                                      frameParamsBatch);
                    if (std::memcmp(&s_verify, &matBuf[thisMatIdx],
                                    sizeof(Resource::MaterialGPUData)) != 0)
                    {
                        ++ctx.stats.matSnapMismatches;
                        ENGINE_CHECK_ONCE(false,
                            "[SPLITCHECK][MATSNAP] slot %u: snapshot record differs "
                            "from the live one", thisMatIdx);
                    }
                }
                StageMaterialSlot(frameSlot, thisMatIdx);
                ++m_matSlotWindowBaseWrites;
            }

            // Frame record: a face's head frame for the deferred toon, from this frame's
            // snapshot: (HeadForward.xz, HeadRight.xz). First sight is once per key per
            // frame, and the key is this face's alone.
            if (frameParamsBatch && batchMat && m_matFrameRecordMapped[frameSlot])
            {
                const std::array<float, 4>* fwd = batchMat->FindGpuParam(MaterialComponent::kFrameHeadForward);
                const std::array<float, 4>* rgt = batchMat->FindGpuParam(MaterialComponent::kFrameHeadRight);
                float* rec = static_cast<float*>(m_matFrameRecordMapped[frameSlot])
                           + static_cast<size_t>(thisMatIdx) * 4u;
                rec[0] = fwd ? (*fwd)[0] : 0.0f;
                rec[1] = fwd ? (*fwd)[2] : 0.0f;
                rec[2] = rgt ? (*rgt)[0] : 0.0f;
                rec[3] = rgt ? (*rgt)[2] : 0.0f;
                ++m_matSlotWindowFrameRecords;
            }

            // The b8 / space3 packer, PackCustomMaterialBindings, is shared with the
            // preview (no entity, no persistent slot) so its two DEVICE_HUNG traps live
            // in one place. Slot-keyed here: it writes m_customMatCbvStore /
            // m_customMatSrvStore (or the per-frame rings) and the per-slot bookkeeping.
            CustomPackDest packDest;
            packDest.slot             = thisMatIdx;
            // A face's b8 carries this frame's frameParams, so it is repacked
            // every frame - onto the per-frame ring: the persistent store is
            // one copy per slot that frames still in flight read.
            packDest.persistentCustom = persistentCustom && !frameParamsBatch;
            const size_t writesBeforeCustom = materialWrites.size();
            PackCustomMaterialBindings(packDest, batchMat, firstRt,
                                       customRefl, first.customVSID,
                                       (first.matBits & MB_CUSTOM_SHADER) != 0,
                                       deferMaterial ? &materialWrites : nullptr);
            for (size_t wi = writesBeforeCustom; wi < materialWrites.size(); ++wi)
                materialWrites[wi].snapshotIndex = first.matSnap;
            }
        }

        }

        publishMaterialChunk();
        _nestMark(2);

        // Depth-prepass eligibility for this batch. Any instance that can discard
        // disqualifies the whole batch: a draw is one PSO + one depth state.
        bool batchHasDissolve = false;

        // Packet metadata is serial; instance finalization can be deferred
        // until every slot and output array has reached its final size.
        {
        ZoneNamedN(instancePlanZone, "BS.Owner.InstancePlan", s_profileOwner);
        for (int k = i; k < j; ++k, ++instIdx)
        {
            const uint32_t candidate = indices[k];
            const auto& ck = candidates[candidate];
            const bool hasDissolve = parallelPlan ? emitPlan[instIdx].hasDissolve :
                (!m_dissolveParams.empty() && m_dissolveParams.find(ck.entity) != m_dissolveParams.end());
            if (hasDissolve) batchHasDissolve = true;
            if (verifyPlan)
                ENGINE_CHECK(hasDissolve == (!m_dissolveParams.empty() && m_dissolveParams.find(ck.entity) != m_dissolveParams.end()),
                             "emit plan changed dissolve eligibility");
            const InstanceJob job = allocateInstance(candidate, instIdx, thisMatIdx, slotValid);
            if (parallelEmit) {
                const uint32_t entity = static_cast<uint32_t>(ck.entity);
                emitMaxEntity = (std::max)(emitMaxEntity, entity);
                // Entity blocks also keep adjacent dense-cache writes on the
                // same lane, reducing false sharing compared with entity % N.
                emitLanes[(entity / 256u) % emitLaneCount].jobs.push_back(job);
            } else {
                emitInstance(job, m_instStageStats, nullptr);
            }
        }

        }
#if WITH_HARNESS
        // [SURFACEPAGES_ORACLE], part 2: the dissolve tally the page gate used.
        if (verifySurfacePages && rowValid && (row.hasDissolve != 0) != batchHasDissolve)
            ++spFlagDiff;
#endif
        _nestMark(3);
        nestInstances += static_cast<uint64_t>(batchCount);
        if (s_retainProbe) {
            // Every field a retained packet would have to reproduce, plus the
            // two that decide WHERE it lands (batchStart) and how many
            // instances it owns. Listed explicitly rather than hashed off the
            // DrawPacket bytes: the struct has alignment padding and a raw
            // memcmp of indeterminate bytes would invent mismatches.
            uint64_t d = 0xcbf29ce484222325ull;
            auto mix = [&d](uint64_t v) { d = (d ^ v) * 0x100000001b3ull; };
            mix(first.meshDescSlot);     mix(batchStart);
            mix(batchCount);             mix(first.indexCount);
            mix(thisMatIdx);             mix(first.perm.bits);
            mix(static_cast<uint64_t>(first.filter));
            mix(first.texBaseColor);     mix(first.texSurfaceMap);
            mix(first.texNormalMap);
            mix(static_cast<uint64_t>(first.outlinePixels * 4096.0f));
            mix(first.screenSpaceOutline ? 1u : 0u);
            mix(first.isPickingOutline ? 1u : 0u);
            mix(first.stencilRef);       mix(first.mirrored);
            mix(first.shadowCullMode);   mix(first.castShadow);
            mix(first.viewMask);         mix(first.renderInMainPass ? 1u : 0u);
            mix(first.perObjectShadowCaster); mix(first.hairShadowCaster);
            mix(first.faceOverlay);      mix(first.faceOverlayParams);
            mix(first.prevPosElementBase);
            mix(first.customPSID);       mix(first.customVSID);
            mix(batchHasDissolve ? 1u : 0u);
            mix(thisMatIdx < m_matCustomCbvVA.size() ? m_matCustomCbvVA[thisMatIdx] : 0ull);
            mix(thisMatIdx < m_matCustomTexHandle.size() ? m_matCustomTexHandle[thisMatIdx] : 0ull);
            const size_t ord = s_emitDigestCur.size();
            if (ord < s_emitDigestPrev.size() && s_emitDigestPrev[ord] == d) {
                ++retStableBatch;
                retStableInst += batchCount;
            }
            s_emitDigestCur.push_back(d);
        }
        {
        ZoneNamedN(packetZone, "BS.Owner.PacketPublish", s_profileOwner);
        DrawPacket dp;
        dp.meshDescriptorIndex = first.meshDescSlot;
        dp.instanceOffset      = batchStart;
        dp.instanceCount       = batchCount;
        dp.vertexOrIndexCount  = first.indexCount;
        dp.materialIndex       = thisMatIdx;
        dp.permutation         = first.perm;
        dp.filter              = first.filter;
        dp.sortKey             = DrawPacket::MakeSortKey(
                                    static_cast<uint16_t>(first.perm.bits), 0, 0);
        dp.texBaseColor        = first.texBaseColor;
        dp.texSurfaceMap       = first.texSurfaceMap;
        dp.texNormalMap        = first.texNormalMap;
        dp.outlinePixels       = first.outlinePixels;
        dp.screenSpaceOutline  = first.screenSpaceOutline;
        dp.isPickingOutline    = first.isPickingOutline;
        dp.stencilRef          = first.stencilRef;
        dp.mirrored            = first.mirrored;
        dp.shadowCullMode      = first.shadowCullMode;
        dp.castShadow          = first.castShadow;
        dp.viewMask            = first.viewMask;
        dp.renderInMainPass    = first.renderInMainPass ? 1u : 0u;
        dp.perObjectShadowCaster = first.perObjectShadowCaster;
        dp.hairShadowCaster    = first.hairShadowCaster;
        dp.faceOverlay         = first.faceOverlay;
        dp.faceOverlayParams   = first.faceOverlayParams;
        dp.prevPosElementBase  = first.prevPosElementBase;
        // Safe only when nothing can discard: alpha-test clips in the PS, a custom
        // PS may discard arbitrarily, and dissolve clips per instance. A custom VS
        // is fine: the prepass reuses the same vsID, so its depth matches
        // bit-for-bit.
        dp.depthPrepassEligible =
            (!first.perm.Has(PermutationKey::ALPHA_TEST) &&
             first.customPSID == 0 &&
             !batchHasDissolve) ? 1u : 0u;
        // Its own bit because depthPrepassEligible also folds in ALPHA_TEST and
        // custom PS, and the surface cache (which requires a custom PS) needs the
        // dissolve term alone. Dissolve is per instance and not in the batch key, so
        // a dissolving and an intact batch can share a (meshDesc, material) pair;
        // the bake pass matches on that pair and must tell them apart, or it burns
        // dissolve holes into a persistent page.
        dp.batchHasDissolve    = batchHasDissolve ? 1u : 0u;
        // A masked custom material's shadow runs its own hook (the SHADOW_HOOK
        // variant of its PS, drawn per draw by the shadow passes). Under
        // -shadow_hook_off the caster uses Shadow.ps's engine-slot alpha.
        dp.shadowHookPS        = (first.customPSID != 0 &&
                                  first.perm.Has(PermutationKey::ALPHA_TEST) &&
                                  ShadowHookEnabled()) ? 1u : 0u;
        dp.customPSID          = first.customPSID;
        dp.customVSID          = first.customVSID;
        dp.meshletBlocks       = first.meshletBlocks;
        dp.customCbvVA         = (thisMatIdx < m_matCustomCbvVA.size())
                                 ? m_matCustomCbvVA[thisMatIdx]
                                 : 0ULL;
        dp.customTexTable      = (thisMatIdx < m_matCustomTexHandle.size())
                                 ? m_matCustomTexHandle[thisMatIdx]
                                 : 0ULL;

        // GPU-TDR guard. The index count has no readiness check on the skinned path
        // (BuildScene_GatherMeshLibRefs): a character whose mesh buffers are not
        // resident when its candidate was built can carry a zero or garbage (huge)
        // count, and DrawInstanced(huge, ...) hangs the GPU (DXGI_ERROR_DEVICE_HUNG
        // on a DRAWINSTANCED with no page fault). Skip the packet and log the entity
        // and count. If a hang persists without this line, suspect the vertex data
        // (a stale bindless mesh descriptor), not the count.
        if (dp.vertexOrIndexCount == 0 || dp.vertexOrIndexCount > kMaxDrawVerts ||
            dp.instanceCount      == 0 || dp.instanceCount      > kMaxDrawInstances)
        {
            LOG_WARNING("Renderer: skipped draw packet with bad count "
                        "(entity=%u vertexOrIndexCount=%u instanceCount=%u meshDesc=%u) "
                        "— mesh not ready / corrupt; prevented a likely GPU TDR.",
                        static_cast<uint32_t>(first.entity),
                        dp.vertexOrIndexCount, dp.instanceCount, dp.meshDescriptorIndex);
#if WITH_HARNESS
            // [SURFACEPAGES_ORACLE], part 3: the plan has to PREDICT this skip
            // or it would have acquired a page the nest never would.
            if (verifySurfacePages && rowValid && !row.tdrSkip) ++spTdrDiff;
#endif
            ++planRow;
            i = j;
            continue;
        }

        DrawPacketsWrite().push_back(dp);

        // Triangle census, once per packet: after the TDR guard (no garbage counts)
        // and not per candidate (~0.005 ms/frame here). Not in RenderPass::Execute:
        // several passes walk sub-spans of the same DrawPacketsWrite() vector, which
        // would multi-count.
        {
            const uint64_t tris = uint64_t(dp.vertexOrIndexCount / 3u)
                                * uint64_t(dp.instanceCount);
            switch (dp.filter)
            {
                case DrawFilter::Opaque:      ctx.stats.triOpaque      += tris; break;
                case DrawFilter::Transparent: ctx.stats.triTransparent += tris; break;
                case DrawFilter::Water:       ctx.stats.triWater       += tris; break;
                case DrawFilter::Custom:      ctx.stats.triOutline     += tris; break;
                case DrawFilter::Shadow:      ctx.stats.triShadowOnly  += tris; break;
                // Secondary-view duplicates go into no triangle bucket: they repeat
                // geometry the census already saw (as a Shadow caster) or dropped, and
                // counting them would make the frame's triangles jump when a PiP opens.
                // Their cost is on the [PREVIEW] world cull line. They are counted as
                // packets: the indirect / cull-command range at the tail must exclude
                // exactly them.
                case DrawFilter::SecondaryView:
                case DrawFilter::SecondaryViewBlend:
                case DrawFilter::SecondaryViewCustom:
                // The baking probe's copies, likewise.
                case DrawFilter::ProbeView:          ++secondaryPackets; break;
            }
            // Same predicate as the indirect-args `wantsMain` below, so the two never
            // disagree about "the main view". Secondary-view packets carry the entity's
            // viewMask (MainCamera set) and renderInMainPass 1, so their filter is what
            // keeps them out.
            if (dp.filter != DrawFilter::Shadow && !IsSecondaryViewFilter(dp.filter)
                && dp.renderInMainPass != 0
                && (dp.viewMask & ViewBit::MainCamera) != 0)
                ctx.stats.triMainView += tris;
        }
        _nestMark(4);

        // Material surface cache: page + entity registration. The page key is
        // (meshDescriptorIndex, materialIndex) = this batch's identity, the mesh
        // section x material granularity the atlas needs: sections overlap heavily
        // in UV space, so a material-only key would let them overwrite each other's
        // texels. Dead when the cache is disabled (IsEnabled() first; every callee
        // returns early).
        // Gates, each load-bearing:
        //  * custom shader with a resolved customPSID - a standard material's engine
        //    slots are already right in the proxy (a page would only lose
        //    precision), and a custom PS that failed to resolve draws the stock
        //    GBuffer PS, whose page would pass default output off as the material.
        //  * not MB_AUTHORED_RT - the authored standard slots already describe the
        //    RT surface.
        //  * Opaque + renderInMainPass - the bake reuses the deferred GBuffer
        //    variant; a translucent material's forward PS does not fit the 6-MRT
        //    bake PSO.
        //  * !batchHasDissolve - the bake would burn dissolve holes into a page that
        //    outlives the dissolve; the character keeps the alias proxy meanwhile.
        //  * identity UV (MB_IDENTITY_UV: texMulAdd == identity) - the RT sampler
        //    wraps, so with a page rect a uv outside [0,1] lands in the neighbouring
        //    page; an authored tiling is the only such case visible here.
        //  * viewMask has MainCamera - the bake pass walks the draw list with the
        //    same filter; a page nobody bakes would keep that pass active forever.
        //  * not alpha-tested - the bake drops the ALPHA_TEST permutation, which is
        //    also where the OPACITYMAP multiply lives (custom_surface.hlsli), so the
        //    page alpha would be unmasked and RTR / RTAO / RTShadow would see a solid
        //    quad; the alias proxy samples the real mask. A fix needs a bake-only
        //    permutation that keeps the mask multiply without the clip.
        //  * not PerFrameParams - a page is baked once, which would freeze a shader
        //    that shades with a frame param (EndfieldFace bends normals toward
        //    HeadCenter).
        // Under planActive ResolveBatchPlan already ran this block in the same order
        // (`page` is a leaf: only the MapEntity loop below reads it). `!planActive`,
        // not `!earlyPlan`: on a refused-plan frame this is the only page acquirer
        // and the material half above has fallen back too; the two must degrade
        // together or the frame gets a re-stamped material slot and no page.
        // TODO(MaterialSurfaceCache M5): a per-mesh UV-bounds field from import; a
        // mesh whose own uv0 leaves [0,1] with identity tiling still bleeds.
        if (!planActive &&
            m_surfaceCache.IsEnabled() && (first.matBits & MB_CUSTOM_SHADER) &&
            dp.customPSID != 0 &&
            !(first.matBits & MB_ALPHA_TEST) &&
            !(first.matBits & MB_FRAME_PARAMS) &&
            !(first.matBits & MB_AUTHORED_RT) &&
            dp.filter == DrawFilter::Opaque && dp.renderInMainPass && !batchHasDissolve &&
            (dp.viewMask & ViewBit::MainCamera) != 0 &&
            (first.matBits & MB_IDENTITY_UV))
        {
            // Content fingerprint (see MaterialSurfaceCache::AcquirePage): matHash
            // catches a recycled material slot, the index count a recycled
            // mesh-descriptor slot. Accepted risk: a released mesh whose slot goes to a
            // different mesh with the same index count and material keeps the stale page
            // (closing it needs a stable mesh id on DrawCandidate).
            const uint64_t contentHash =
                first.matHash ^ (static_cast<uint64_t>(dp.vertexOrIndexCount) * 0x9E3779B97F4A7C15ull);
            const uint32_t page = m_surfaceCache.AcquirePage(
                m_gfx, dp.meshDescriptorIndex, dp.materialIndex, contentHash);
            if (page != MaterialSurfaceCache::kInvalidPage)
            {
                // EVERY entity in the batch, not just `first`: a DrawPacket is an
                // INSTANCED batch and DDGISceneAS joins per-entity, so mapping
                // only the representative would leave the other instances of the
                // same section reflecting off the alias path.
                for (int k = i; k < j; ++k)
                    m_surfaceCache.MapEntity(
                        static_cast<uint32_t>(candidates[indices[k]].entity), page);
            }
        }
#if WITH_HARNESS
        // [SURFACEPAGES_ORACLE], part 4: the gate and the content fingerprint,
        // re-derived at the ONE point in the nest where every input to them is
        // final, and compared with what the plan recorded.
        if (verifySurfacePages && rowValid)
        {
            const bool gate =
                m_surfaceCache.IsEnabled() && (first.matBits & MB_CUSTOM_SHADER) &&
                dp.customPSID != 0 &&
                !(first.matBits & MB_ALPHA_TEST) &&
                !(first.matBits & MB_FRAME_PARAMS) &&
                !(first.matBits & MB_AUTHORED_RT) &&
                dp.filter == DrawFilter::Opaque && dp.renderInMainPass && !batchHasDissolve &&
                (dp.viewMask & ViewBit::MainCamera) != 0 &&
                (first.matBits & MB_IDENTITY_UV);
            if ((row.pageGate != 0) != gate) ++spGateDiff;
            if (row.tdrSkip) ++spTdrDiff;   // reached here => the guard did NOT fire
            if (gate)
            {
                const uint64_t ch = first.matHash ^
                    (static_cast<uint64_t>(dp.vertexOrIndexCount) * 0x9E3779B97F4A7C15ull);
                if (row.contentHash != ch) ++spHashDiff;
                if (row.matIdx != dp.materialIndex ||
                    first.meshDescSlot != dp.meshDescriptorIndex) ++spMatIdxDiff;
            }
        }
#endif
        _nestMark(5);

        // Also write IndirectDrawCommand for ExecuteIndirect path.
        if (m_indirectArgMapped[frameSlot])
        {
            auto* indirectArgs = static_cast<IndirectDrawCommand*>(m_indirectArgMapped[frameSlot]);
            uint32_t cmdIdx = static_cast<uint32_t>(DrawPacketsWrite().size()) - 1;
            // Unreachable while the emit loop caps instIdx at m_maxInstances (every
            // packet owns >= 1 instance, so packets <= instIdx). If that ever stops
            // holding, the packet ships without an arg slot and every later
            // ExecuteIndirect group offset points at the wrong command - so tripwire it.
            if (ENGINE_CHECK_ONCE(cmdIdx < m_maxInstances,
                    "Renderer: indirect arg slot %u >= m_maxInstances (%u); "
                    "packet %zu gets no arg entry and the ExecuteIndirect group "
                    "offsets are now off by one",
                    cmdIdx, m_maxInstances, DrawPacketsWrite().size()))
            {
                // A secondary-view packet gets no arg slot: nothing executes it (indirect
                // groups take Opaque only, the compaction and shadow metas stop at
                // indirectDrawCount, the CPU per-draw fallback never reads the args). The
                // slot is still the packet index, so a group's argOffset (di *
                // sizeof(IndirectDrawCommand)) still names the right command. The skipped
                // slots are a tail because DrawFilter is the sort key's high field and the
                // secondary filters (SecondaryView, SecondaryViewBlend, ProbeView) are its
                // largest values; the `else` below handles that ceasing to hold.
                if (IsSecondaryViewFilter(dp.filter) && !m_indirectArgTailBroken)
                {
                    if (firstSkippedArg == 0xFFFFFFFFu) firstSkippedArg = cmdIdx;
                    ++secondaryArgsSkipped;
                }
                else
                {
                    // A non-secondary packet after a skipped one means the secondary filters
                    // have stopped being the sorted tail (a new DrawFilter above ProbeView would
                    // do it). The slots skipped so far are inside the executed range holding the
                    // previous frame's commands, so they are back-filled with degenerate ones
                    // and the skip is off for the rest of the process.
                    if (firstSkippedArg != 0xFFFFFFFFu)
                    {
                        ENGINE_CHECK_ONCE(false,
                            "SortAndEmit: packet %u (filter %u) follows a skipped "
                            "secondary-view packet at %u - the secondary filters are "
                            "no longer the sorted tail, so the indirect arg array "
                            "goes back to one entry per packet.",
                            cmdIdx, static_cast<unsigned>(dp.filter), firstSkippedArg);
                        for (uint32_t bi = firstSkippedArg; bi < cmdIdx; ++bi)
                        {
                            const DrawPacket& sp = DrawPacketsWrite()[bi];
                            if (!IsSecondaryViewFilter(sp.filter)) continue;
                            IndirectDrawCommand& bc = indirectArgs[bi];
                            bc.meshDescIdx            = sp.meshDescriptorIndex;
                            bc.instanceOffset         = sp.instanceOffset;
                            bc.materialIndex          = sp.materialIndex;
                            bc.prevPosInfo            = sp.prevPosElementBase;
                            bc.vertexCountPerInstance = sp.vertexOrIndexCount;
                            bc.instanceCount          = 0;
                            bc.startVertexLocation    = 0;
                            bc.startInstanceLocation  = 0;
                            --secondaryArgsSkipped;
                        }
                        firstSkippedArg = 0xFFFFFFFFu;
                        m_indirectArgTailBroken = true;
                    }
                    argsWritten = cmdIdx + 1;
                    IndirectDrawCommand& ic = indirectArgs[cmdIdx];
                    ic.meshDescIdx     = dp.meshDescriptorIndex;
                    ic.instanceOffset  = dp.instanceOffset;
                    ic.materialIndex   = dp.materialIndex;
                    ic.prevPosInfo     = dp.prevPosElementBase;
                    ic.vertexCountPerInstance = dp.vertexOrIndexCount;
                    // A packet the main pass skips (renderInMainPass 0, viewMask without
                    // MainCamera, or a secondary-view duplicate, which sits in no indirect
                    // group) gets instanceCount 0: the slot stays 1:1 with DrawPacketsWrite() so
                    // group offsets stay contiguous, and ExecuteIndirect treats it as a no-op.
                    // Under GPU compaction the args are view-independent pristine input (the
                    // cull CS applies viewMask / instFlags), so the full count is written;
                    // Render() never hands un-culled full-count args to ExecuteIndirect (it
                    // degrades to the CPU per-draw path). Matches the triMainView predicate
                    // above.
                    const bool wantsMain =
                        dp.renderInMainPass != 0
                        && !IsSecondaryViewFilter(dp.filter)
                        && (dp.viewMask & ViewBit::MainCamera) != 0;
                    ic.instanceCount = (SceneEmitWrite().gpuCompactionFrameActive || wantsMain)
                                           ? dp.instanceCount : 0;
                    ic.startVertexLocation    = 0;
                    ic.startInstanceLocation  = 0;
                    // A meshlet-group command hides its cluster blocks in StartInstanceLocation
                    // (never part of SV_InstanceID, so the draw the shadow views and the
                    // fallbacks execute is unchanged) and its meshlet draw flags in word 3
                    // (prevPosInfo, unused by GBuffer.vs; the AS reads the no-cone bit). The
                    // cull CS rewrites it to DISPATCH_MESH(blocks, survivors, 1).
                    if (MeshletIndirectPacket(dp))
                    {
                        ic.prevPosInfo           = GBufferPass::MeshletDrawFlags(dp);
                        ic.startInstanceLocation = dp.meshletBlocks;
                    }
                    else if (ShadowRetainedGroup(dp) >= ShadowDrawGroup::MeshletOffset)
                    {
                        // A CSM meshlet caster only: the shadow cull CS reads the blocks here for a
                        // CSM view's DISPATCH_MESH; word 3 stays (the shadow AS needs no draw
                        // flags).
                        ic.startInstanceLocation = dp.meshletBlocks;
                    }
                }
            }
        }
        _nestMark(6);

        }
        ++nestBatches;
        ++planRow;
        i = j;
    }
    }

    // Under planActive the nest must not touch the surface cache
    // (ResolveBatchPlan already did). The counters are process-monotonic, so
    // this compares a bracket. _ONCE: a violation is a code change, not a data
    // condition.
    if (planActive)
    {
        const uint32_t acqAfter = m_surfaceCache.GetAcquireCallCount();
        const uint32_t mapAfter = m_surfaceCache.GetMapEntityCallCount();
        ENGINE_CHECK_ONCE(acqAfter == acquiresBeforeNest && mapAfter == mapsBeforeNest,
            "[SURFACEPAGES] the emit nest mutated MaterialSurfaceCache while "
            "-surfacepagesearly was on (AcquirePage %u->%u, MapEntity %u->%u) - "
            "the nest is no longer side-effect free and the hoist is not "
            "bit-identical", acquiresBeforeNest, acqAfter, mapsBeforeNest, mapAfter);
        // Skipped when the cross-walk check already stopped the frame: that stop
        // was reported with the batch it happened on, and one defect should not
        // become two findings.
        if (!planStopped)
            ENGINE_CHECK_ONCE(planRow == static_cast<uint32_t>(m_batchPlan.size()),
                "[SURFACEPAGES] the nest consumed %u of %zu plan rows - the two "
                "walks disagree about how many batches this frame has",
                planRow, m_batchPlan.size());
    }
#if WITH_HARNESS
    if (verifySurfacePages)
    {
        const uint32_t diffs = spBoundaryDiff + spMatIdxDiff + spFlagDiff +
                               spGateDiff + spHashDiff + spTdrDiff;
        const bool rowsAgree = planRow == static_cast<uint32_t>(m_batchPlan.size());
        const uint32_t movedEpoch = (planEpochAfter != planEpochBefore) ? 1u : 0u;
        const uint32_t match = (diffs == 0 && rowsAgree) ? 1u : 0u;
        LOG_INFO("[SURFACEPAGES_ORACLE] frame=%llu batches=%zu nestBatches=%u "
                 "gated=%u mapped=%u acquires=%u epochBefore=%u epochAfter=%u "
                 "movedEpoch=%u pagesInUse=%u pagesBaked=%u pending=%u "
                 "boundaryDiff=%u matIdxDiff=%u flagDiff=%u gateDiff=%u "
                 "hashDiff=%u tdrDiff=%u rows=%u match=%u",
                 static_cast<unsigned long long>(g_surfacePagesFrame),
                 m_batchPlan.size(), spNestBatches,
                 planGatedBatches, planMappedEntities, planGatedBatches,
                 planEpochBefore, planEpochAfter, movedEpoch,
                 m_surfaceCache.GetPagesInUse(), m_surfaceCache.GetPagesBaked(),
                 m_surfaceCache.GetPendingBakes(),
                 spBoundaryDiff, spMatIdxDiff, spFlagDiff, spGateDiff,
                 spHashDiff, spTdrDiff, planRow, match);
        ENGINE_CHECK(match == 1u,
            "[SURFACEPAGES_ORACLE] mismatch on frame %llu: boundary=%u matIdx=%u "
            "flags=%u gate=%u hash=%u tdr=%u rows=%u/%zu",
            static_cast<unsigned long long>(g_surfacePagesFrame),
            spBoundaryDiff, spMatIdxDiff, spFlagDiff, spGateDiff, spHashDiff,
            spTdrDiff, planRow, m_batchPlan.size());
    }
#endif
    // [SURFACEPAGES] steady-state line (-phaselog_every): the plan stage's own
    // wall time, so its cost is never inferred from [EMITZONE]'s `preamble`
    // slot, which it shares.
    {
        static const int s_spEvery = []{
            const auto& cl = Sys::CommandLine::Get();
            return cl.Has("-phaselog_every") ? cl.GetInt("-phaselog_every", 60) : 0;
        }();
        static uint64_t s_spLogFrame = 0;
        if (earlyPlan && s_spEvery > 0 && ++s_spLogFrame % static_cast<uint64_t>(s_spEvery) == 0)
            LOG_INFO("[SURFACEPAGES] batches=%zu gated=%u mapped=%u epoch=%u "
                     "moved=%u pagesInUse=%u planMs=%.3f active=%u stamped=%u "
                     "nestInst=%llu fallback=%llu",
                     m_batchPlan.size(), planGatedBatches, planMappedEntities,
                     planEpochAfter, (planEpochAfter != planEpochBefore) ? 1u : 0u,
                     m_surfaceCache.GetPagesInUse(), planMs, unsigned(planActive),
                     matSlotStamped,
                     static_cast<unsigned long long>(nestInstances),
                     // Running count of lane fallbacks: a steady -brslane run reads 0 forever,
                     // and a run that took the refusal says so on a line a pack already reads.
                     static_cast<unsigned long long>(m_emitLaneFallbackFrames));
    }

    if (emitProbe) {
        if (s_nestProfile) {
            // ONE steady_clock pair per frame converts this frame's ticks to ms.
            const double wallMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - nestWallStart).count();
            const uint64_t nestTscEnd = __rdtsc();
            uint64_t ticks = 0;
            if (nestTscEnd >= nestTscStart) ticks = nestTscEnd - nestTscStart;
            else                            ++nestBackSteps;   // same guard, frame bracket
            // Charge whatever the last mark did not cover (the TDR-guard
            // `continue` path and the loop tail) to bucket 0, not to nothing.
            uint64_t marked = 0;
            for (uint64_t v : nestAcc) marked += v;
            if (ticks > marked) nestAcc[0] += ticks - marked;
            const double msPerTick = ticks ? wallMs / static_cast<double>(ticks) : 0.0;
            for (int b = 0; b < kEmitNestBuckets; ++b)
                s_emitNest[b] += static_cast<double>(nestAcc[b]) * msPerTick;
            s_emitNestWallMs   += wallMs;
            s_emitNestBackSteps += nestBackSteps;
        }
        s_emitNestBatches  += nestBatches;
        s_emitNestInstance += nestInstances;
        s_retStableBatch += retStableBatch;   s_retStableInst += retStableInst;
        s_retTotalBatch  += nestBatches;      s_retTotalInst  += nestInstances;
        static const int s_every = []{
            const auto& cl = Sys::CommandLine::Get();
            return cl.Has("-phaselog_every") ? cl.GetInt("-phaselog_every", 60) : 0;
        }();
        if (s_every > 0 && ++s_emitNestN % s_every == 0) {
            const double inv = 1.0 / static_cast<double>(s_every);
            // name=value, because expect.logLineFields is what gates the pack
            // and it only reads `name=<number>` pairs. Everything is PER FRAME.
            if (s_nestProfile) {
            // Bucket sanity: the seven buckets partition the bracket nestMs measures
            // (bucket 0 takes the remainder), so their sum must reproduce nestMs.
            // bucketPct is that ratio and goes red when a bucket is poisoned: one
            // wrapped subtraction (~2^64 ticks) puts it in the billions, where a
            // per-bucket bound would still look plausible. tscBack counts the steps
            // that cause it.
            double bucketSum = 0.0;
            for (double v : s_emitNest) bucketSum += v;
            const double bucketPct = s_emitNestWallMs > 0.0
                ? 100.0 * bucketSum / s_emitNestWallMs : 0.0;
            LOG_INFO("[EMITNEST] batches=%.0f instances=%.0f nestMs=%.4f "
                     "other=%.4f boundary=%.4f material=%.4f instPlan=%.4f "
                     "packet=%.4f surfaceCache=%.4f indirect=%.4f "
                     "bucketSumMs=%.4f bucketPct=%.2f tscBack=%llu "
                     "(per frame; nestMs excludes BS.PlanPrefix.Drain)",
                     static_cast<double>(s_emitNestBatches) * inv,
                     static_cast<double>(s_emitNestInstance) * inv,
                     s_emitNestWallMs * inv,
                     s_emitNest[0] * inv, s_emitNest[1] * inv, s_emitNest[2] * inv,
                     s_emitNest[3] * inv, s_emitNest[4] * inv, s_emitNest[5] * inv,
                     s_emitNest[6] * inv,
                     bucketSum * inv, bucketPct,
                     static_cast<unsigned long long>(s_emitNestBackSteps));
            ENGINE_CHECK_ONCE(s_emitNestBackSteps == 0,
                "[EMITNEST] %llu backwards TSC step(s) over %d frames - the bucket "
                "breakdown is clamped, not trusted",
                static_cast<unsigned long long>(s_emitNestBackSteps), s_every);
            }
            if (s_retainProbe) {
            LOG_INFO("[EMITRETAIN] batches=%.0f stable=%.0f stablePct=%.1f "
                     "instances=%.0f stableInstances=%.0f stableInstPct=%.1f "
                     "(per frame; the CEILING for a retained emit plan)",
                     static_cast<double>(s_retTotalBatch) * inv,
                     static_cast<double>(s_retStableBatch) * inv,
                     s_retTotalBatch ? 100.0 * double(s_retStableBatch) / double(s_retTotalBatch) : 0.0,
                     static_cast<double>(s_retTotalInst) * inv,
                     static_cast<double>(s_retStableInst) * inv,
                     s_retTotalInst ? 100.0 * double(s_retStableInst) / double(s_retTotalInst) : 0.0);
            }
            for (double& v : s_emitNest) v = 0.0;
            s_emitNestWallMs = 0.0; s_emitNestBatches = 0; s_emitNestInstance = 0;
            s_emitNestBackSteps = 0;
            s_retStableBatch = s_retStableInst = s_retTotalBatch = s_retTotalInst = 0;
        }
        if (s_retainProbe) s_emitDigestPrev.swap(s_emitDigestCur);
    }
    if (prefixPlan) {
        ZoneScopedN("BS.PlanPrefix.Drain");
        ctx.planScope->Join(); // also drains ranges beyond a truncated output prefix
    }
#if WITH_HARNESS
    if (verifyPrefixPlan) {
        for (uint32_t k = 0; prefixPlan && k < planCount; ++k) {
            const auto& c = candidates[sortedCandidates[k]];
            const auto& row = emitPlan[k];
            ENGINE_CHECK(row.staticSlot == ~0u &&
                row.staticEligible == (retainInst && IsStaticLaneInstance(c)) &&
                row.hasDissolve == (!m_dissolveParams.empty() && m_dissolveParams.find(c.entity) != m_dissolveParams.end()) &&
                row.startsBatch == (k == 0 || !SameEmitBatch(candidates[sortedCandidates[k - 1]], c)),
                "streamed plan row oracle mismatch");
        }
        // `wide=` names the budget that ran: min(16, workers) vs min(4, workers),
        // which agree at <= 4 workers, so a lane count alone cannot tell. Like
        // `stream=`, it reads 0 when no streamed plan ran.
        LOG_INFO("[PLAN_PREFIX_ORACLE] count=%u ranges=%u consumed=%u stream=%u wide=%u match=1",
                 prefixPlan ? planCount : 0u, prefixPlan ? activePlanLanes : 0u,
                 ctx.nextPlanLane, prefixPlan ? 1u : 0u,
                 (prefixPlan && widePrefix) ? 1u : 0u);
    }
#endif

    if (!materialWrites.empty()) {
        ZoneScopedN("BS.MaterialCPU.Writes");
        auto& expected = expectedMaterial;
        if (verifyMaterialWrites) {
            std::vector<std::pair<uintptr_t, uintptr_t>> ranges;
            for (const auto& write : materialWrites) {
                const auto start = reinterpret_cast<uintptr_t>(write.target);
                ranges.emplace_back(start, start + write.bytes);
            }
            std::sort(ranges.begin(), ranges.end());
            for (size_t i = 1; i < ranges.size(); ++i)
                ENGINE_CHECK(ranges[i-1].second <= ranges[i].first, "material CPU targets overlap");
            prepareMaterialOracle(earlySubmitted, materialWrites.size());
        }
        const uint32_t count = static_cast<uint32_t>(materialWrites.size() - earlySubmitted);
        const uint32_t lanes = (std::max)(1u, (std::min)(4u, (std::min)(TaskSystem::Get().GetWorkerCount(), count / 64u)));
        materialTailLanes = lanes;
        auto writeLane = [&](uint32_t lane) {
            ZoneScopedN("BS.MaterialCPU.Chunk");
            for (uint32_t i = uint64_t(count) * lane / lanes; i < uint64_t(count) * (lane + 1) / lanes; ++i)
                RunMaterialCpuWrite(materialWrites[earlySubmitted + i]);
        };
        if (framePreparation) {
            QueueFrameMaterialWrites(materialWrites, verifyMaterialWrites);
        } else if (overlapMaterialTail && count >= 64 && TaskSystem::Get().GetWorkerCount() > 1) {
            // The owner loop is finished: no further vector growth or material
            // binding mutation. Capture only the stable descriptor span by
            // value, never writeLane or its block-local counters by reference.
            const MaterialCpuWrite* const writes = materialWrites.data() + earlySubmitted;
            ctx.materialTailScope.emplace(lanes);
            materialTailQueued = lanes;
            for (uint32_t lane = 0; lane < lanes; ++lane) {
                const uint32_t lo = uint64_t(count) * lane / lanes;
                const uint32_t hi = uint64_t(count) * (lane + 1) / lanes;
                ctx.materialTailScope->Submit([this, writes, lo, hi] {
                    ZoneScopedN("BS.MaterialTail.Chunk");
                    for (uint32_t i = lo; i < hi; ++i) RunMaterialCpuWrite(writes[i]);
                });
            }
        } else {
            Sys::CpuTaskScope scope(lanes - 1);
            for (uint32_t lane = 1; lane < lanes; ++lane) scope.Submit([&, lane] { writeLane(lane); });
            writeLane(0);
            scope.Join();
        }
        if (!overlapMaterialTail) ctx.earlyMaterialScope->Join();
        if (verifyMaterialWrites && !overlapMaterialTail && !framePreparation) {
            for (size_t i = 0; i < materialWrites.size(); ++i)
                ENGINE_CHECK(std::memcmp(expected[i].data(), materialWrites[i].target, materialWrites[i].bytes) == 0,
                             "material CPU byte oracle mismatch");
            const auto baseCount = std::count_if(materialWrites.begin(), materialWrites.end(), [](const auto& w) { return w.baseRecord; });
            LOG_INFO("[MATERIAL_CPU_ORACLE] writes=%zu base=%zu b8=%zu lanes=%u earlyChunks=%zu match=1", materialWrites.size(), size_t(baseCount), size_t(materialWrites.size()-baseCount), lanes, earlyChunkCount);
        }
    }

    if (parallelEmit && instIdx > 0)
    {
        ZoneScopedN("BS.EmitInstances");
        InstAabbWrite().resize(instIdx);
        if (retainInst) InstanceRemapWrite().resize(instIdx);
        if (emitMaxEntity >= m_entityMatSlot.size()) {
            m_entityMatSlot.resize(size_t(emitMaxEntity) + 1, m_maxMaterials);
            m_entityMatSlotEpoch.resize(size_t(emitMaxEntity) + 1, 0);
        }
        if (emitMaxEntity >= m_prevWorldValid.size()) {
            const size_t grown = (std::max)(size_t(emitMaxEntity) + 1, m_prevWorldValid.size() * 2);
            m_prevWorldValid.resize(grown, 0);
            m_prevWorldDense.resize(grown);
        }
        bool graphEmit = false;
#if WITH_HARNESS
        graphEmit = Sys::CommandLine::Get().Has("-emitgraph");
#endif
        ZoneNamedN(graphEmitZone, "BS.EmitSchedule.Graph", graphEmit);
        ZoneNamedN(legacyEmitZone, "BS.EmitSchedule.Legacy", !graphEmit);
        auto runEmitJobs = [&] {
            ZoneScopedN("BS.EmitInstances.Work");
            auto runLane = [&](uint32_t laneIndex) {
                ZoneScopedN("BS.EmitInstances.Lane");
                auto& lane = emitLanes[laneIndex];
                ZoneValue(lane.jobs.size());
                for (const auto& job : lane.jobs) emitInstance(job, lane.stats, &lane);
            };
            if (!graphEmit || emitLaneCount < 2) {
                TaskSystem::Get().ParallelFor(0, emitLaneCount, runLane);
                return;
            }
            // Preserve entity ownership and within-lane draw order. Job count
            // is a coarse cost hint, not measured execution time. Submit larger
            // lanes first; owner handles the smallest while workers run.
            std::array<uint32_t, 8> order{};
            for (uint32_t i = 0; i < emitLaneCount; ++i) order[i] = i;
            std::sort(order.begin(), order.begin() + emitLaneCount,
                [&](uint32_t a, uint32_t b) {
                    const auto ac = emitLanes[a].jobs.size(), bc = emitLanes[b].jobs.size();
                    return ac != bc ? ac > bc : a < b;
                });
            Sys::CpuTaskScope scope(emitLaneCount - 1);
            for (uint32_t i = 0; i + 1 < emitLaneCount; ++i) {
                const uint32_t lane = order[i];
                if (!emitLanes[lane].jobs.empty()) scope.Submit([&, lane] { runLane(lane); });
            }
            runLane(order[emitLaneCount - 1]);
            scope.Join();
        };
#if WITH_HARNESS
        static const bool s_verifyEmit = Sys::CommandLine::Get().Has("-emitverify");
        static uint64_t s_verifyFrame = 0;
        if (s_verifyEmit && (++s_verifyFrame <= 3 || s_verifyFrame % 60 == 0))
        {
            ZoneScopedN("BS.EmitInstances.Oracle");
            // Same starting state, global draw order vs entity-owned lanes.
            // This intentionally expensive oracle is never enabled in timings.
            const auto prevWorld = m_prevWorldDense;
            const auto prevValid = m_prevWorldValid;
            const auto shadows = m_instSlotShadow;
            const auto stamps = m_instSlotLastEmit;
            const auto materials = m_entityMatSlot;
            const auto epochs = m_entityMatSlotEpoch;
            const std::vector<Entity> slotEntities(std::begin(m_instanceSlotToEntity), std::end(m_instanceSlotToEntity));
            auto fingerprint = [&] {
                uint64_t hash = 14695981039346656037ull;
                auto bytes = [&](const void* data, size_t size) {
                    const auto* p = static_cast<const unsigned char*>(data);
                    for (size_t b = 0; b < size; ++b) { hash ^= p[b]; hash *= 1099511628211ull; }
                };
                auto vectorBytes = [&](const auto& values) {
                    bytes(std::data(values), std::size(values) * sizeof(values[0]));
                };
                vectorBytes(m_prevWorldDense); vectorBytes(m_prevWorldValid);
                vectorBytes(m_instSlotShadow); vectorBytes(m_instSlotLastEmit);
                vectorBytes(m_entityMatSlot); vectorBytes(m_entityMatSlotEpoch);
                vectorBytes(m_instanceSlotToEntity); vectorBytes(InstAabbWrite());
                vectorBytes(InstanceRemapWrite());
                if (remapMapped) bytes(remapMapped, instIdx * sizeof(*remapMapped));
                if (!retainInst && instances) bytes(instances, instIdx * sizeof(*instances));
                for (uint32_t laneIndex = 0; laneIndex < emitLaneCount; ++laneIndex) {
                    const auto& lane = emitLanes[laneIndex];
                    bytes(&lane.stats, sizeof(lane.stats));
                    for (const auto& record : lane.staged) {
                        bytes(&record.drawLocal, sizeof(record.drawLocal));
                        bytes(&record.slot, sizeof(record.slot));
                        bytes(&record.record, sizeof(record.record));
                    }
                }
                return hash;
            };
            std::vector<std::pair<const InstanceJob*, uint32_t>> orderedJobs;
            orderedJobs.reserve(instIdx);
            for (uint32_t laneIndex = 0; laneIndex < emitLaneCount; ++laneIndex)
                for (const auto& job : emitLanes[laneIndex].jobs)
                    orderedJobs.emplace_back(&job, laneIndex);
            std::sort(orderedJobs.begin(), orderedJobs.end(),
                [](const auto& a, const auto& b) { return a.first->drawLocal < b.first->drawLocal; });
            const bool reusePrevBeforeOracle = reuseStaticPrevWorld;
            reuseStaticPrevWorld = false; // original writes are the reference
            for (const auto& [job, laneIndex] : orderedJobs) {
                auto& lane = emitLanes[laneIndex];
                emitInstance(*job, lane.stats, &lane);
            }
            const uint64_t serialHash = fingerprint();
            reuseStaticPrevWorld = reusePrevBeforeOracle;
            m_prevWorldDense = prevWorld; m_prevWorldValid = prevValid;
            m_instSlotShadow = shadows; m_instSlotLastEmit = stamps;
            m_entityMatSlot = materials; m_entityMatSlotEpoch = epochs;
            std::copy(slotEntities.begin(), slotEntities.end(), std::begin(m_instanceSlotToEntity));
            // Every emitted output element is overwritten by the worker run.
            for (uint32_t laneIndex = 0; laneIndex < emitLaneCount; ++laneIndex) {
                emitLanes[laneIndex].stats = {};
                emitLanes[laneIndex].staged.clear();
            }
            // Poison reused output storage: equality with the reference now
            // also proves that every live element is overwritten, instead of
            // accidentally passing with values left by the reference run.
            if (!InstAabbWrite().empty())
                std::memset(InstAabbWrite().data(), 0xCD,
                    InstAabbWrite().size() * sizeof(InstAabbWrite()[0]));
            std::fill(InstanceRemapWrite().begin(), InstanceRemapWrite().end(), ~0u);
            runEmitJobs();
            const uint64_t parallelHash = fingerprint();
            ENGINE_CHECK(serialHash == parallelHash,
                "instance emit oracle mismatch: serial=%llu parallel=%llu",
                static_cast<unsigned long long>(serialHash), static_cast<unsigned long long>(parallelHash));
            LOG_INFO("[EMIT_ORACLE] instances=%u lanes=%u match=%u", instIdx, emitLaneCount,
                     serialHash == parallelHash ? 1u : 0u);
        }
        else
#endif
            runEmitJobs();
        // Preserve the original draw-local staging order (and copy coalescing),
        // not worker completion order. Sparse dirty records avoid an O(N *
        // sizeof(GPUInstanceData)) temporary for settled static scenes.
        {
            ZoneScopedN("BS.EmitInstances.Publish");
            // Bulk publication is the default only under the all-jobs preset
            // (JobSplittingDefaultsEnabled): a steady A/B measured it ~2.5% slower in
            // this zone. The serial publisher stays for rollback and comparison.
            bool bulkPublish = Sys::JobSplittingDefaultsEnabled(), verifyPublish = false;
            uint32_t copyGrain = 2048;
#if WITH_HARNESS
            const auto& publishCmd = Sys::CommandLine::Get();
            bulkPublish = Sys::JobSplittingDefaultsEnabled() || publishCmd.Has("-instancepublishverify");
            static uint64_t s_verifyPublishEpoch = 0;
            verifyPublish = publishCmd.Has("-instancepublishverify") &&
                (++s_verifyPublishEpoch <= 3 || s_verifyPublishEpoch % 60 == 0);
            // -instancepublishforcejobs: a 512-record copy grain, so small correctness
            // scenes still exercise the multi-job publish.
            if (publishCmd.Has("-instancepublishforcejobs")) copyGrain = 512;
#endif
#ifdef TRACY_ENABLE
            ZoneTransientN(publishModeZone, bulkPublish ? "BS.Publish.Bulk" : "BS.Publish.Legacy", true);
#endif
            thread_local std::vector<const StagedRecord*> t_orderedStaging;
            t_orderedStaging.clear();
            // Each entity-owned lane appends in increasing drawLocal order.
            // Merge those runs instead of sorting the full pointer array again.
            struct StagedCursor { const StagedRecord* current; const StagedRecord* end; };
            std::array<StagedCursor, 8> stagingHeap{};
            size_t heapSize = 0, stagedCount = 0;
            for (uint32_t laneIndex = 0; laneIndex < emitLaneCount; ++laneIndex) {
                const auto& lane = emitLanes[laneIndex];
                stagedCount += lane.staged.size();
                if (!lane.staged.empty())
                    stagingHeap[heapSize++] = {lane.staged.data(), lane.staged.data() + lane.staged.size()};
                m_instStageStats.candidates += lane.stats.candidates;
                m_instStageStats.staticExamined += lane.stats.staticExamined;
                m_instStageStats.staticStaged += lane.stats.staticStaged;
                m_instStageStats.staticSkipped += lane.stats.staticSkipped;
                m_instStageStats.staticFresh += lane.stats.staticFresh;
                m_instStageStats.dynamicStaged += lane.stats.dynamicStaged;
                m_instStageStats.legacyStaged += lane.stats.legacyStaged;
            }
            t_orderedStaging.reserve(stagedCount);
            {
                ZoneScopedN("BS.Publish.MergeRuns");
                auto later = [](const StagedCursor& a, const StagedCursor& b) {
                    return a.current->drawLocal > b.current->drawLocal;
                };
                std::make_heap(stagingHeap.begin(), stagingHeap.begin() + heapSize, later);
                while (heapSize) {
                    std::pop_heap(stagingHeap.begin(), stagingHeap.begin() + heapSize, later);
                    auto cursor = stagingHeap[--heapSize];
                    t_orderedStaging.push_back(cursor.current++);
                    if (cursor.current != cursor.end) {
                        stagingHeap[heapSize++] = cursor;
                        std::push_heap(stagingHeap.begin(), stagingHeap.begin() + heapSize, later);
                    }
                }
            }
            auto* staging = static_cast<GPUInstanceData*>(m_instanceBufferMapped[frameSlot]);
            const uint32_t firstStaging = m_instanceStagingUsed;
            const uint32_t copyCount = staging && firstStaging < m_maxInstances
                ? static_cast<uint32_t>((std::min)(t_orderedStaging.size(), size_t(m_maxInstances - firstStaging))) : 0;
#if WITH_HARNESS
            std::vector<GPUInstanceData> expectedRecords;
            std::vector<InstanceCopyRange> expectedRanges;
            std::vector<GPUInstanceData> expectedShadows;
            uint32_t expectedUsed = 0;
            if (verifyPublish && bulkPublish) {
                const auto originalRanges = InstCopyWrite();
                const auto originalShadows = m_instSlotShadow;
                for (const auto* record : t_orderedStaging)
                    StageInstanceRecord(frameSlot, record->slot, record->record);
                if (copyCount) expectedRecords.assign(staging + firstStaging, staging + firstStaging + copyCount);
                expectedRanges = InstCopyWrite();
                expectedUsed = m_instanceStagingUsed;
                expectedShadows = m_instSlotShadow;
                InstCopyWrite() = originalRanges;
                m_instanceStagingUsed = firstStaging;
                m_instSlotShadow = originalShadows;
                if (copyCount) std::memset(staging + firstStaging, 0xCD, copyCount * sizeof(GPUInstanceData));
            }
#endif
            if (!bulkPublish) {
                for (const auto* record : t_orderedStaging)
                    StageInstanceRecord(frameSlot, record->slot, record->record);
            }
            else {
                // All static shadows are already final: duplicates route to the
                // same entity lane and run in draw-local order. Publish only the
                // upload records, without rewriting the shadow a second time.
                // Metadata stays owner-only; preserve append/coalescing exactly.
                auto& ranges = InstCopyWrite();
                for (uint32_t r = 0; r < copyCount; ++r) {
                    const uint32_t slot = t_orderedStaging[r]->slot;
                    if (!ranges.empty() && ranges.back().frameSlot == frameSlot &&
                        ranges.back().dstSlot + ranges.back().count == slot &&
                        ranges.back().stagingFirst + ranges.back().count == firstStaging + r)
                        ++ranges.back().count;
                    else ranges.push_back({frameSlot, firstStaging + r, slot, 1u});
                }
                // Bind the owner's TLS storage before capture; workers must not
                // resolve orderedStaging through their own thread-local vector.
                const auto* records = t_orderedStaging.data();
                auto copyRecords = [=](uint32_t begin, uint32_t end) {
                    ZoneScopedN("BS.Publish.Copy");
                    for (uint32_t r = begin; r < end; ++r)
                        staging[firstStaging + r] = records[r]->record;
                };
                const uint32_t lanes = (std::max)(1u, (std::min)(
                    TaskSystem::Get().GetWorkerCount(), copyCount / copyGrain));
                if (lanes > 1) {
                    const uint32_t chunk = (copyCount + lanes - 1) / lanes;
                    Sys::CpuTaskScope copies(lanes - 1);
                    for (uint32_t lane = 1; lane < lanes; ++lane) {
                        const uint32_t begin = lane * chunk;
                        copies.Submit([=] { copyRecords(begin, (std::min)(copyCount, begin + chunk)); });
                    }
                    copyRecords(0, chunk);
                    copies.Join();
                }
                else copyRecords(0, copyCount);
                m_instanceStagingUsed = firstStaging + copyCount;
            }
#if WITH_HARNESS
            if (verifyPublish && bulkPublish) {
                bool match = expectedUsed == m_instanceStagingUsed && expectedRanges.size() == InstCopyWrite().size();
                for (size_t r = 0; match && r < expectedRanges.size(); ++r) {
                    const auto& a = expectedRanges[r]; const auto& b = InstCopyWrite()[r];
                    match = a.frameSlot == b.frameSlot && a.stagingFirst == b.stagingFirst &&
                        a.dstSlot == b.dstSlot && a.count == b.count;
                }
                match = match && (!copyCount || std::memcmp(expectedRecords.data(), staging + firstStaging,
                    copyCount * sizeof(GPUInstanceData)) == 0);
                match = match && expectedShadows.size() == m_instSlotShadow.size() &&
                    (expectedShadows.empty() || std::memcmp(expectedShadows.data(), m_instSlotShadow.data(),
                        expectedShadows.size() * sizeof(GPUInstanceData)) == 0);
                ENGINE_CHECK(match, "instance publication oracle mismatch");
                LOG_INFO("[INSTANCE_PUBLISH] staged=%u grain=%u match=%u", copyCount, copyGrain, match ? 1u : 0u);
            }
#endif
            static const int s_every = Sys::CommandLine::Get().GetInt("-phaselog_every", 0);
            static uint64_t s_frame = 0;
            if (s_every > 0 && ++s_frame % s_every == 0)
                LOG_INFO("[EMIT_JOBS] instances=%u lanes=%u staged=%zu", instIdx, emitLaneCount, t_orderedStaging.size());
        }
    }

    // Instance records/AABBs/remaps and staging ranges do not consume material
    // bytes. Join both material producers here before packet routing or any
    // later renderer phase can consume/recycle their frame-local targets.
    if (overlapMaterialTail) {
        {
            ZoneScopedN("BS.MaterialTail.Join");
            if (ctx.materialTailScope) ctx.materialTailScope->Join();
            ctx.earlyMaterialScope->Join();
        }
        if (verifyMaterialWrites) {
            for (size_t i = 0; i < materialWrites.size(); ++i)
                ENGINE_CHECK(std::memcmp(expectedMaterial[i].data(), materialWrites[i].target, materialWrites[i].bytes) == 0,
                             "overlapped material CPU byte oracle mismatch");
            const auto baseCount = std::count_if(materialWrites.begin(), materialWrites.end(), [](const auto& w) { return w.baseRecord; });
            LOG_INFO("[MATERIAL_CPU_ORACLE] writes=%zu base=%zu b8=%zu lanes=%u earlyChunks=%zu match=1", materialWrites.size(), size_t(baseCount), size_t(materialWrites.size()-baseCount), materialTailLanes, earlyChunkCount);
        }
        if (verifyMaterialWrites)
            LOG_INFO("[MATERIAL_TAIL_ORACLE] writes=%zu lanes=%u queued=%u overlap=1 match=1", materialWrites.size(), materialTailLanes, materialTailQueued);
    }

    if (parallelPlan) {
        static const int s_every = Sys::CommandLine::Get().GetInt("-phaselog_every", 0);
        static uint64_t s_frame = 0;
        if (s_every > 0 && ++s_frame % s_every == 0)
            LOG_INFO("[EMIT_PLAN] candidates=%u batches=%u cachedSlots=%u verifiedBatches=%u",
                     planCount, materialBatchCount, planCachedSlots, planVerifiedBatches);
    }

    EmitZoneMark(2);
    // Instance-budget truncation: the loop stops when instIdx reaches
    // m_maxInstances, so every remaining candidate gets no DrawPacket - which
    // looks like a broken mesh load or an over-eager cull, so it is logged.
    // Reported on the first overflow and then only when it gets worse: unlike
    // materialSlotOverflowWarned's per-frame latch (a report site inside a
    // loop), this runs once per frame and an over-budget scene stays over
    // budget. Drop order follows the sort key: DrawFilter is key0[63:56] and
    // transparents sort far -> near, so the highest filters (secondary views,
    // Water, Custom) go first, then the nearest transparents. Depth-aware
    // eviction would be a behaviour change of its own.
    m_instanceEmitHighWater = (std::max)(m_instanceEmitHighWater, instIdx);
    const uint32_t droppedCandidates = static_cast<uint32_t>(n - i);
    if (droppedCandidates > 0)
    {
        m_instanceDropLastFrame = droppedCandidates;
        ++m_instanceOverflowFrames;
        if (droppedCandidates > m_instanceDropHighWater)
        {
            m_instanceDropHighWater = droppedCandidates;
            LOG_WARNING("Renderer: instance budget exhausted — %u/%u instances "
                        "emitted, %u of %d candidates DROPPED (no draw packet, "
                        "those objects are simply absent from the frame); "
                        "%u frames over budget so far. Raise "
                        "RendererPoolCaps::maxInstances. Note the drop order follows "
                        "the batch sort key, so Custom-filter and NEAR "
                        "transparent draws are lost first.",
                        instIdx, m_maxInstances, droppedCandidates, n,
                        m_instanceOverflowFrames);
        }
    }

    EmitZoneMark(3);
    // End-of-frame instance-slot maintenance.
    if (retainInst)
    {
        // Entities untracked this frame (mesh component removed without a destroy;
        // the destroy listener handled destroys, and free is idempotent).
        for (Entity e : m_sceneRemoved) FreeInstanceSlot(e);

        // Retire parked slots whose frames-in-flight lag expired.
        size_t w = 0;
        for (size_t r = 0; r < m_instSlotsPendingReuse.size(); ++r)
        {
            const auto& p = m_instSlotsPendingReuse[r];
            if (m_instFrameStamp - p.second >= kInstSlotReuseLagFrames)
                m_freeInstSlots.push_back(p.first);
            else
                m_instSlotsPendingReuse[w++] = p;
        }
        m_instSlotsPendingReuse.resize(w);

        if (m_instSlotNext > m_instSlotHighWater)
        {
            m_instSlotHighWater = m_instSlotNext;
            LOG_INFO("Renderer: instance slot high-water %u / %u static "
                     "(dyn used %u / %u this frame, staged %u records)",
                     m_instSlotHighWater, m_staticInstanceCap,
                     dynCursor - m_staticInstanceCap, kDynamicInstanceReserve,
                     m_instanceStagingUsed);
        }
    }

    // Editor Profiler feed: counters this function already computed.
    ctx.stats.matBatches       = materialBatchCount;
    if (snapVerifiedWrites > 0)
        LOG_INFO("[MATSNAP_ORACLE] comparisons=%u mismatches=%u",
                 snapVerifiedWrites, ctx.stats.matSnapMismatches);
    // Proof of life for the MatSnapshot check: without it "0 mismatches" and
    // "the check never ran" look the same.
    {
        static bool s_reportedMatBits = false;
        if (!s_reportedMatBits && materialBatchCount > 0)
        {
            s_reportedMatBits = true;
            LOG_INFO("[SPLITCHECK] MatSnapshot verify ACTIVE: %u batches, "
                     "%u snapshot-driven record writes, mismatches(first frame): "
                     "bits=%u record=%u",
                     materialBatchCount, snapDrivenWrites,
                     ctx.stats.matBitsMismatches, ctx.stats.matSnapMismatches);
        }
    }
    EmitZoneMark(6);
    ctx.stats.drawPackets      = static_cast<uint32_t>(DrawPacketsWrite().size());
    ctx.stats.staticInstSlots  = m_instSlotNext;
    ctx.stats.dynamicInstances = (dynCursor > m_staticInstanceCap)
                                  ? dynCursor - m_staticInstanceCap : 0u;
    ctx.stats.lights           = static_cast<uint32_t>(m_frameLights.size());
    ctx.stats.visibleDrawCmds = m_visibleDrawCmds;
    // AND-ed with the frame's compaction state: with indirectDraw or
    // gpuCompaction off there is no count buffer, and a ring primed under an
    // earlier configuration would publish a meaningless number. A field that
    // silently reads 0 because a feature is off is worse than an absent one.
    ctx.stats.visibleDrawCmdsValid = m_visibleDrawCmdsValid && SceneEmitWrite().gpuCompactionFrameActive;
    ctx.stats.instancesEmitted = ctx.stats.staticInstSlots
                                  + ctx.stats.dynamicInstances;

    // [MATCHURN]: material-slot churn over a window. A steady scene mints no
    // keys and writes no base records; each driven face writes one frame record
    // per frame. ([MATSLOT] belongs to the custom-material slot check.)
    if (++m_matSlotWindowFrames == kMatSlotWindowFrames)
    {
        LOG_INFO("[MATCHURN] frames=%u newKeys=%u baseWrites=%u frameRecords=%u liveKeys=%zu",
                 m_matSlotWindowFrames, m_matSlotWindowNewKeys, m_matSlotWindowBaseWrites,
                 m_matSlotWindowFrameRecords, m_persistentMatSlots.size());
        m_matSlotWindowFrames = m_matSlotWindowNewKeys = 0;
        m_matSlotWindowBaseWrites = m_matSlotWindowFrameRecords = 0;
    }
    // Tells a consumer whether triMainView means "visible" or merely
    // "submitted". Assigned on every path: nothing resets FrameStats, so a
    // conditional write would leave a previous frame's answer standing.
    ctx.stats.cpuCullBypassed = SceneEmitWrite().shadowArgsFrameActive;
    ctx.stats.gpuVisibleTriangles = m_visTriangles;
    ctx.stats.gpuVisibleInstances = m_visInstances;
    ctx.stats.gpuVisibleCommands  = m_visCommands;
    // AND-ed with this frame's compaction state for the same reason
    // visibleDrawCmdsValid is: with indirectDraw or gpuCompaction off the
    // compaction CS never runs, and a ring primed under an earlier
    // configuration would otherwise keep publishing a stale number that looks
    // entirely plausible.
    ctx.stats.gpuVisStatsValid = m_visStatsValid && SceneEmitWrite().gpuCompactionFrameActive;
    // Meshlet path: which path the last recorded frame took, and the
    // amplification shader's counters - assigned on every path (nothing resets
    // FrameStats). Valid only while the path draws: counters primed under an
    // earlier configuration must not keep publishing.
    if (m_gbufferPass)
    {
        const GBufferPass::MeshletFrameStats mf = m_gbufferPass->GetMeshletFrameStats();
        const GBufferPass::MeshletGpuStats   mg = m_gbufferPass->GetMeshletGpuStats();
        using namespace MeshletShared;
        ctx.stats.meshletPath        = (mf.active && !mf.indirect && mf.packets > 0) ? 1u
                                     : (mf.active && mf.indirect && (mf.indirectCmds > 0 || mf.groupFallback > 0))
                                           ? 2u : 0u;
        ctx.stats.meshletStatsValid  = mg.valid && ctx.stats.meshletPath != 0;
        ctx.stats.meshletTested      = mg.counters[kMeshletStatTested];
        ctx.stats.meshletLodRejected = mg.counters[kMeshletStatLodRejected];
        ctx.stats.meshletCullFrustum = mg.counters[kMeshletStatCullFrustum];
        ctx.stats.meshletCullCone    = mg.counters[kMeshletStatCullCone];
        ctx.stats.meshletCullHiZ     = mg.counters[kMeshletStatCullHiZ];
        ctx.stats.meshletVisible     = mg.counters[kMeshletStatVisible];
        ctx.stats.meshletStatsFrame  = mg.frame;
        ctx.stats.meshletTris        = mg.counters[kMeshletStatVisibleTris];
    }

    // End-of-frame GC: reclaim orphan slots so the map stays bounded to live keys.
    const uint32_t liveMatKeys = m_retainMaterialSlots ? SweepPersistentMatSlots()
                                                       : m_persistentMatSlotNext;

    const uint32_t customTableCount =
        m_customMatSrvRing.GetUsedDescriptors() /
        GraphicsDX12::kCustomMatTextureSlots;
    const uint32_t customCbvBytes = m_customMatCbvRing.GetUsedBytes();
    // slotHighWater = distinct slots ever handed out (what can hit
    // m_maxMaterials); liveMatKeys = keys live this frame after the GC.
    const uint32_t slotHighWater = m_persistentMatSlotNext;
    if (slotHighWater > m_materialBindingHighWater ||
        customTableCount > m_customMaterialTableHighWater ||
        customCbvBytes > m_customMaterialCbvHighWater)
    {
        m_materialBindingHighWater = std::max(m_materialBindingHighWater, slotHighWater);
        m_customMaterialTableHighWater =
            std::max(m_customMaterialTableHighWater, customTableCount);
        m_customMaterialCbvHighWater =
            std::max(m_customMaterialCbvHighWater, customCbvBytes);
        LOG_INFO("Renderer material high-water: candidates=%zu batches=%u "
                 "liveKeys=%u slotHighWater=%u reusedBindings=%u customTables=%u/%u "
                 "customCBV=%u/%u bytes",
                 candidates.size(), materialBatchCount, liveMatKeys, slotHighWater,
                 reusedMaterialBindings, customTableCount,
                 m_customMatSrvRing.GetCapacityDescriptors() /
                    GraphicsDX12::kCustomMatTextureSlots,
                 customCbvBytes,
                 m_customMatCbvRing.GetCapacityBytes());
    }

    // The indirect / cull-command range, minus the secondary tail. One number
    // is the indirect group build's loop bound, the fill length of both
    // per-command meta buffers and (through cullCmdCount in Render)
    // InstanceCull.cs's dispatch range. A secondary-view packet is in none of
    // them (groups take Opaque only; ClassifyShadowDrawGroup skips the
    // secondary filters), so counting them would dispatch ~200 no-op cull
    // threads per PiP frame. A tail trim is sound because packets are in sort
    // order and SecondaryView (5), SecondaryViewBlend (6) and ProbeView (7) are
    // the largest DrawFilter values. The check below verifies that and keeps
    // every packet when it fails: an under-count would leave a real Opaque
    // packet undrawn.
    {
        const uint32_t totalPackets = static_cast<uint32_t>(DrawPacketsWrite().size());
        uint32_t indirectCount = (secondaryPackets <= totalPackets)
                               ? (totalPackets - secondaryPackets) : totalPackets;
        if (indirectCount != totalPackets &&
            !IsSecondaryViewFilter(DrawPacketsWrite()[indirectCount].filter))
        {
            ENGINE_CHECK_ONCE(false,
                "SortAndEmit: %u secondary-view packet(s) of %u are NOT the sorted "
                "tail (packet %u has filter %u) - the indirect range cannot be "
                "trimmed safely, so it keeps every packet this frame.",
                secondaryPackets, totalPackets, indirectCount,
                static_cast<unsigned>(DrawPacketsWrite()[indirectCount].filter));
            indirectCount = totalPackets;
        }
        SceneEmitWrite().indirectDrawCount = indirectCount;

        // Arg-array cost, reported once per rise like the other high-waters.
        // `args` is the prefix the emit loop wrote; `perPacket` is one entry per
        // packet, still the number the buffer is sized for. `saved` is a per-frame
        // high-water latched with that frame's args / perPacket / secondarySkipped,
        // so the line satisfies perPacket - args == saved; `argsPeak` /
        // `perPacketPeak` are the sizing maxima.
        // argsWritten (what the loop wrote) and indirectCount (from the packet
        // counts) are computed independently and must agree: a divergence means a
        // consumer indexes a slot nobody filled.
        ENGINE_CHECK_ONCE(m_indirectArgTailBroken || !m_indirectArgMapped[frameSlot] ||
                          argsWritten == indirectCount,
            "SortAndEmit: the indirect arg array wrote %u slot(s) but the "
            "executed range is %u (of %u packet(s), %u secondary skipped) - a "
            "command inside the range was never filled.",
            argsWritten, indirectCount, totalPackets, secondaryArgsSkipped);
        // This frame's saving, clamped: argsWritten > totalPackets (reported by the
        // check above) must not underflow into a 4-billion floor.
        const uint32_t savedThisFrame =
            (argsWritten <= totalPackets) ? (totalPackets - argsWritten) : 0u;
        // Ties broken by the packet count, so a run that never saves anything (no
        // secondary view, e.g. boot_baseline) still latches a real frame rather
        // than (0, 0, 0).
        const bool savedRose = savedThisFrame > m_indirectArgSavedHighWater;
        const bool savedTieBigger =
            (savedThisFrame == m_indirectArgSavedHighWater &&
             totalPackets   >  m_indirectArgSavedPackets);
        if (savedRose || savedTieBigger)
        {
            m_indirectArgSavedHighWater   = savedThisFrame;
            m_indirectArgSavedArgs        = argsWritten;
            m_indirectArgSavedPackets     = totalPackets;
            m_indirectArgSecondarySkipped = secondaryArgsSkipped;
        }
        if (savedRose || savedTieBigger ||
            argsWritten  > m_indirectArgHighWater ||
            totalPackets > m_indirectArgPacketHighWater)
        {
            m_indirectArgHighWater       = (std::max)(m_indirectArgHighWater, argsWritten);
            m_indirectArgPacketHighWater = (std::max)(m_indirectArgPacketHighWater, totalPackets);
            // `saved` is the field a pack reads: logLineFields has only min / max
            // bounds and equality, so "args < perPacket" must be a floor on a
            // difference the engine computes. The first four fields are one frame's;
            // the two Peak fields may come from other frames.
            LOG_INFO("Renderer indirect arg high-water: args=%u perPacket=%u "
                     "saved=%u secondarySkipped=%u cap=%u argsPeak=%u "
                     "perPacketPeak=%u",
                     m_indirectArgSavedArgs, m_indirectArgSavedPackets,
                     m_indirectArgSavedHighWater, m_indirectArgSecondarySkipped,
                     m_maxInstances, m_indirectArgHighWater,
                     m_indirectArgPacketHighWater);
        }
    }

    // Build per-PSO groups for ExecuteIndirect batching (only opaque filter).
    SceneEmitWrite().indirectGroups.clear();
    if (m_indirectArgMapped[frameSlot] && SceneEmitWrite().indirectDrawCount > 0)
    {
        uint32_t groupStart = 0;
        for (uint32_t di = 0; di < SceneEmitWrite().indirectDrawCount; ++di)
        {
            const DrawPacket& dp = DrawPacketsWrite()[di];
            if (dp.filter != DrawFilter::Opaque) continue;

            const uint8_t meshlet = MeshletIndirectPacket(dp) ? 1 : 0;
            bool newGroup = SceneEmitWrite().indirectGroups.empty()
                || SceneEmitWrite().indirectGroups.back().meshlet        != meshlet
                || SceneEmitWrite().indirectGroups.back().perm           != dp.permutation
                || SceneEmitWrite().indirectGroups.back().stencilRef     != dp.stencilRef
                || SceneEmitWrite().indirectGroups.back().mirrored       != dp.mirrored
                || SceneEmitWrite().indirectGroups.back().depthPrepassEligible != dp.depthPrepassEligible
                || SceneEmitWrite().indirectGroups.back().customPSID     != dp.customPSID
                || SceneEmitWrite().indirectGroups.back().customVSID     != dp.customVSID
                || SceneEmitWrite().indirectGroups.back().customCbvVA    != dp.customCbvVA
                || SceneEmitWrite().indirectGroups.back().customTexTable != dp.customTexTable;

            if (newGroup)
            {
                IndirectGroup g;
                g.perm           = dp.permutation;
                g.stencilRef     = dp.stencilRef;
                g.mirrored       = dp.mirrored;
                g.depthPrepassEligible = dp.depthPrepassEligible;
                g.meshlet        = meshlet;
                g.customPSID     = dp.customPSID;
                g.customVSID     = dp.customVSID;
                g.customCbvVA    = dp.customCbvVA;
                g.customTexTable = dp.customTexTable;
                g.argOffset      = di * sizeof(IndirectDrawCommand);
                g.cmdCount       = 1;
                SceneEmitWrite().indirectGroups.push_back(g);
            }
            else
            {
                SceneEmitWrite().indirectGroups.back().cmdCount++;
            }
        }
    }

    EmitZoneMark(4);
    // Per-command group routing for the compaction CS: meta[cmd] =
    // groupBase<<16 | groupIdx (groupBase in bits 16..30, bit 31 = meshlet
    // group; bounds asserted at buffer creation). 0xFFFFFFFF marks commands
    // outside every indirect group (transparent / Custom / shadow-only): the CS
    // must not touch their remap ranges, because the CPU-path passes draw them
    // with full counts through the pristine lower half. groupBase is the
    // group's first command slot; the CS claims groupBase +
    // InterlockedAdd(count[groupIdx]) as each survivor's working-arg slot.
    if (SceneEmitWrite().gpuCompactionFrameActive)
    {
        auto* meta = static_cast<uint32_t*>(m_cmdGroupMetaMapped[frameSlot]);
        const uint32_t metaCount =
            (std::min)(SceneEmitWrite().indirectDrawCount, m_maxInstances);
        std::fill_n(meta, metaCount, 0xFFFFFFFFu);
        for (uint32_t gi = 0; gi < SceneEmitWrite().indirectGroups.size(); ++gi)
        {
            const IndirectGroup& g = SceneEmitWrite().indirectGroups[gi];
            const uint32_t base =
                g.argOffset / static_cast<uint32_t>(sizeof(IndirectDrawCommand));
            // Bit 31: a meshlet group - the CS writes its commands as DISPATCH_MESH.
            const uint32_t packed = (base << 16) | gi | (g.meshlet ? 0x80000000u : 0u);
            for (uint32_t c = 0; c < g.cmdCount && (base + c) < metaCount; ++c)
                meta[base + c] = packed;
        }
    }

    EmitZoneMark(5);
    // Shadow-bucket routing for the per-cascade cull views: meta[cmd] =
    // groupBase<<16 | bucket (the group meta's packing; bounds static_assert'd
    // at creation), 0xFFFFFFFF = casts into no CSM bucket. groupBase is the
    // bucket's first slot in every per-cascade working arg buffer (a compact
    // layout of its own); the CS claims groupBase + InterlockedAdd(count) per
    // surviving command. ClassifyShadowDrawGroup reads only batch-static fields
    // (the rules ShadowPass's ring path applies), so this per-packet fill is
    // the whole update.
    if (SceneEmitWrite().shadowArgsFrameActive)
    {
        auto* smeta = static_cast<uint32_t*>(m_shadowCmdMetaMapped[frameSlot]);
        const uint32_t metaCount =
            (std::min)(SceneEmitWrite().indirectDrawCount, m_maxInstances);

        // The retained layout has a meshlet twin per bucket (+MeshletOffset):
        // ShadowRetainedGroup, the same answer the pristine fill used.
        uint32_t caps[ShadowDrawGroup::RetainedCount] = {};
        for (uint32_t di = 0; di < metaCount; ++di)
        {
            const int g = ShadowRetainedGroup(DrawPacketsWrite()[di]);
            if (g >= 0) ++caps[g];
        }
        uint32_t running = 0;
        for (int g = 0; g < ShadowDrawGroup::RetainedCount; ++g)
        {
            SceneEmitWrite().shadowGroupBase[g] = running;
            SceneEmitWrite().shadowGroupCap[g]  = caps[g];
            running += caps[g];
        }
        std::fill_n(smeta, metaCount, 0xFFFFFFFFu);
        for (uint32_t di = 0; di < metaCount; ++di)
        {
            const int g = ShadowRetainedGroup(DrawPacketsWrite()[di]);
            if (g < 0) continue;
            smeta[di] = (SceneEmitWrite().shadowGroupBase[g] << 16) | static_cast<uint32_t>(g);
        }
    }
    else
    {
        // Keep the published layout inert: ShadowPass reads it only on frames
        // Render() armed the retained state, but a zeroed cap set makes any stale
        // read draw nothing rather than garbage.
        std::fill(std::begin(SceneEmitWrite().shadowGroupBase), std::end(SceneEmitWrite().shadowGroupBase), 0u);
        std::fill(std::begin(SceneEmitWrite().shadowGroupCap),  std::end(SceneEmitWrite().shadowGroupCap),  0u);
    }

    if (m_meshletVerify) VerifyMeshletGroups(frameSlot);   // after both metas

    // Instance-staging census (-phaselog_every). The interesting number is
    // staged / examined: the static lane rewrites a record only when its
    // 176-byte GPUInstanceData differs from the shadow copy, so in a settled
    // scene most of the walk produces no GPU write.
    {
        static const int s_every = []{
            const auto& cl = Sys::CommandLine::Get();
            return cl.Has("-phaselog_every") ? cl.GetInt("-phaselog_every", 60) : 0;
        }();
        if (s_every > 0)
        {
            static int s_n = 0;
            if ((s_n++ % s_every) == 0)
            {
                const auto& st = m_instStageStats;
                const uint32_t staged = st.staticStaged + st.dynamicStaged + st.legacyStaged;
                LOG_INFO("[INSTSTAGE] candidates %u -> staged %u (%.1f%%) | "
                         "static: skipped %u examined %u staged %u (fresh %u) | "
                         "dynamic %u | legacy %u%s",
                         st.candidates, staged,
                         st.candidates ? 100.0 * staged / st.candidates : 0.0,
                         st.staticSkipped, st.staticExamined, st.staticStaged,
                         st.staticFresh, st.dynamicStaged, st.legacyStaged,
                         m_verifyStageSkip ? "  [VERIFY MODE: skip disabled]" : "");
            }
        }
    }
}
