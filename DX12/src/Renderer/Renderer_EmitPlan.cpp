// Renderer_EmitPlan.cpp - the first half of the scene emit: the BRS lane's
// switches and plan (the lease check, cached-row assembly, PrepareLanePlan),
// BuildScene_SortAndEmit and the emit-stats hand-over, the emit-half digest
// (WITH_HARNESS), the [EMITZONE] marks, and BuildScene_PlanEmit - the sort, the
// preamble and ResolveBatchPlan, which fill the EmitContext that
// BuildScene_EmitBatches (Renderer_EmitBatches.cpp) consumes.

#include "Renderer/Renderer.h"

// engine graphics / backend
#include "Graphics/GraphicsDX12.h"
#include "System/HostPolicy.h"
#include "Diagnostics/FrameTiming.h"   // brsPlanMs / brsEmitMs (-brslane split)

// ECS components + systems
#include "ECS/MaterialSchema.h"

// engine systems
#include "System/JobDefaults.h"
#include "System/CommandLine.h" // flag reads (-brslane, -surfacepagesearly, harness switches)
#include "System/PathUtil.h"    // DiagnosticsDir: -sortdump
#include <cstdio>
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
#include <type_traits>   // is_trivially_copyable: AssembleLaneCandidates' memcpy

using namespace DirectX;

// CPU-side cbuffer mirrors and their layout static_asserts live in Renderer.h
// (RendererDetail namespace).
using PerViewCB       = RendererDetail::PerViewCB;
using LightCB         = RendererDetail::LightCB;

// The material bits and the material-packing helpers Renderer_Scene.cpp and
// its splits share.
#include "Renderer/RendererSceneShared.h"


// ==== Lane and hoist switches ====
// SurfacePagesEarlyActive (-surfacepagesearly): is the batch plan resolved ahead
// of the emit nest? On, ResolveBatchPlan walks the sorted candidates once,
// performs every AcquirePersistentMatSlot / MaterialSurfaceCache::AcquirePage /
// MapEntity the nest would, in the same order, and records them in m_batchPlan;
// the nest reads those rows and mutates neither allocator. Off, the nest
// allocates itself. -surfacepagesearlyoff wins, so one pack can override a
// corpus-wide --extra-arg. Latched once: if the answer changed between frames the
// nest and the plan would disagree about who owns the allocators.
//
// The lane's host-aware default is computed by FramePipeline::Configure and
// published once through SetBrsLaneHostDefault, before either latched predicate
// is asked; BrsLaneWanted, the -brslane implication in SurfacePagesEarlyActive
// and [FRAME_EXECUTOR] default= all read this stored answer. Until it arrives the
// answer is false, so an exe that never runs Configure gets no default lane
// rather than a lane without its refusal ladder.
namespace {
bool g_brsLaneHostDefault    = false;  // FramePipeline's answer
bool g_brsLaneHostConfigured = false;  // ... has arrived
bool g_laneDefaultLatched    = false;  // ... and a predicate has frozen on it
}  // namespace

void Renderer::SetBrsLaneHostDefault(bool laneIsDefaultHere)
{
    // Order tripwire: both predicates latch on first call, so a host default that
    // arrives after one of them answered is read by nobody and the run silently
    // takes the other arm. Nothing queries before FramePipeline::Configure publishes
    // it, so this guards future edits and has no positive control.
    ENGINE_CHECK_ONCE(!g_laneDefaultLatched,
        "-brslane: the host-aware default arrived after the flag predicates "
        "had already latched; this run's lane arm was chosen without it");
    g_brsLaneHostDefault    = laneIsDefaultHere;
    g_brsLaneHostConfigured = true;
}

// Is the lane the default shape for this host (kBrsLaneDefault and the host
// default FramePipeline published)? Not "is it on": -brslane / -brslaneoff
// override it, and that is BrsLaneWanted's business.
bool Renderer::BrsLaneDefaultActive()
{
    return kBrsLaneDefault && g_brsLaneHostDefault;
}

// The hoist's effective default: its own constant, or the lane's implication.
// [FRAME_EXECUTOR] default= compares SurfacePagesEarlyActive() against this, so a
// default Game run (kBrsLaneDefault true) reads default=1 while
// kSurfacePagesEarlyDefault stays false (the hoist alone is a measured cost; see
// the header).
bool Renderer::SurfacePagesEarlyDefaultActive()
{
    return kSurfacePagesEarlyDefault || BrsLaneDefaultActive();
}

bool Renderer::SurfacePagesEarlyActive()
{
    static const bool s_on = []{
        const auto& cl = Sys::CommandLine::Get();
        ENGINE_CHECK_ONCE(g_brsLaneHostConfigured,
            "-surfacepagesearly: the hoist's default was latched before App "
            "published the host-aware lane default; the -brslane implication "
            "is being evaluated against a stale 'no lane here'");
        g_laneDefaultLatched = true;
        if (cl.Has("-surfacepagesearlyoff")) return false;
        // kSurfacePagesEarlyDefault lets the hoist's default change in one edit, which
        // [FRAME_EXECUTOR]'s default= also reports.
        // -brslane implies the hoist: the lane-side nest must mutate neither allocator
        // (AcquirePersistentMatSlot's map and MaterialSurfaceCache's page table are
        // main's, and DDGISceneAS::Gather reads the cache's publish epoch on main after
        // the hand-off), and ResolveBatchPlan moves both onto main. It is written here,
        // not as a second decision in FramePipeline, so the two cannot disagree.
        // -brslane refuses under -surfacepagesearlyoff, so that escape still wins; a
        // refused -brslane still runs the hoist, and [FRAME_EXECUTOR] reports
        // surfacepagesearly=1.
        // The host-aware lane default is a term too: a default lane (no -brslane on the
        // command line) must get the hoist, or every frame would put the allocators back
        // in the nest and the per-frame fallback would silently run every frame on main.
        // Host-aware rather than kBrsLaneDefault alone, because the implication stops
        // where the lane stops: -rendermode=serial, -renderowneroff and an
        // -editoruilate editor boot get no lane, so they get no hoist either.
        return cl.Has("-surfacepagesearly") || cl.Has("-brslane") ||
               SurfacePagesEarlyDefaultActive();
    }();
    return s_on;
}

// -brslane: the command line's answer, or the host default's (-brslaneoff wins).
// The other terms of the decision are FramePipeline::Configure's refusal ladder,
// which evaluates them once and calls SetBrsLaneEnabled; the executor summary and
// the ladder both read this one answer.
bool Renderer::BrsLaneWanted()
{
    static const bool s_on = []{
        const auto& cl = Sys::CommandLine::Get();
        ENGINE_CHECK_ONCE(g_brsLaneHostConfigured,
            "-brslane: 'wanted' was latched before App published the "
            "host-aware lane default; this run decided its arm without it");
        g_laneDefaultLatched = true;
        if (cl.Has("-brslaneoff")) return false;
        return cl.Has("-brslane") || BrsLaneDefaultActive();
    }();
    return s_on;
}

// Every term compares a value the packet carried (snapshotted at the hand-off)
// with one the emit path latched. No logging: FramePipeline::VerifyEmitLease owns
// the check (see the declaration).
uint32_t Renderer::EmitLeaseMismatch(uint32_t uploadSlot, uint32_t emitIdx,
                                     uint32_t batchPlanGen, uint32_t sortGen) const
{
    uint32_t mask = 0;
    if (uploadSlot   != m_frameUploadSlot)       mask |= kEmitLeaseUploadSlot;
    if (emitIdx      != m_emitIdx)               mask |= kEmitLeaseEmitIdx;
    if (batchPlanGen != m_batchPlanGenConsumed)  mask |= kEmitLeaseBatchPlan;
    if (sortGen      != m_sortIdxGenConsumed)    mask |= kEmitLeaseSortIdx;
    return mask;
}

// State both halves read (BuildScene_PlanEmit and BuildScene_EmitBatches): the
// [EMITZONE] accumulators are here; g_surfacePagesFrame and kMaxDraw* are in
// RendererSceneShared.h.
namespace {

// [EMITZONE]'s per-slot accumulators, shared by Renderer::EmitZoneMark and
// Renderer::FinishSortEmitZones.
constexpr int kSeZoneCount = 10;
double g_seAcc[kSeZoneCount] = {};
int    g_seN = 0;

} // namespace


// ==== The seam: BuildScene_PlanEmit -> EmitContext -> BuildScene_EmitBatches ====
// BuildScene_SortAndEmit is cut at EmitZoneMark(8), after the sort, the preamble
// and ResolveBatchPlan: PlanEmit fills an explicit EmitContext (m_emitCtx) and
// EmitBatches consumes it, on main or on the lane. Nothing crosses by [&] lambda
// capture - it would dangle once the halves run on different threads, and
// CpuTaskScope's worker guard does not catch "constructed on main, joined on the
// lane" (a lane is an owner thread). What crosses is an EmitContext field, a
// member function (EmitZoneMark / EnsurePlanPrefix / FinishSortEmitZones), or a
// lambda re-created in EmitBatches over the context.
//
// SurfaceAtlasEligible: the surface-cache page gate minus its two per-batch
// terms (tdrSkip, dissolve) - a superset, so PrepareLanePlan's atlas prediction
// creates the atlas no later than the plan-on-main arm would.
bool Renderer::SurfaceAtlasEligible(const DrawCandidate& first)
{
    return (first.matBits & MB_CUSTOM_SHADER) && first.customPSID != 0 &&
        !(first.matBits & MB_ALPHA_TEST) &&
        !(first.matBits & MB_FRAME_PARAMS) &&
        !(first.matBits & MB_AUTHORED_RT) &&
        first.filter == DrawFilter::Opaque && first.renderInMainPass &&
        (first.viewMask & ViewBit::MainCamera) != 0 &&
        (first.matBits & MB_IDENTITY_UV);
}

uint32_t Renderer::DiffCandidateLists(const std::vector<DrawCandidate>& a,
                                      const std::vector<DrawCandidate>& b,
                                      const char*& firstField, size_t& firstRow)
{
    uint32_t mismatches = 0;
    firstField = "";
    firstRow = 0;
    auto same = [](const auto& x, const auto& y) { return std::memcmp(&x, &y, sizeof(x)) == 0; };
    auto check = [&](bool ok, const char* field, size_t row) {
        if (ok) return;
        if (!mismatches) { firstField = field; firstRow = row; }
        ++mismatches;
    };
    if (a.size() != b.size()) check(false, "rowCount", 0);
    for (size_t i = 0; i < (std::min)(a.size(), b.size()); ++i)
    {
        const DrawCandidate& x = a[i];
        const DrawCandidate& y = b[i];
#define DCCMP(f) check(same(x.f, y.f), #f, i)
        DCCMP(entity); DCCMP(meshDescSlot); DCCMP(indexCount); DCCMP(worldMatrix);
        DCCMP(mc); DCCMP(matBits); DCCMP(hasAabb); DCCMP(matSnap); DCCMP(texBaseColor);
        DCCMP(texSurfaceMap); DCCMP(texNormalMap); DCCMP(matHash); DCCMP(orderKey); DCCMP(filter);
        DCCMP(perm); DCCMP(worldCenter); DCCMP(aabbMin); DCCMP(aabbMax);
        DCCMP(depth); DCCMP(outlinePixels); DCCMP(screenSpaceOutline);
        DCCMP(isPickingOutline); DCCMP(stencilRef); DCCMP(mirrored);
        DCCMP(shadowCullMode); DCCMP(castShadow); DCCMP(prevPosElementBase);
        DCCMP(skinned); DCCMP(meshletBlocks); DCCMP(customPSID); DCCMP(customVSID); DCCMP(viewMask);
        DCCMP(renderInMainPass); DCCMP(perObjectShadowCaster); DCCMP(hairShadowCaster);
        DCCMP(faceOverlay); DCCMP(faceOverlayParams); DCCMP(dissolveParams);
        DCCMP(dissolveEdgeColor);
#undef DCCMP
    }
    return mismatches;
}

// The patch-frame layout [cached prims | dynamic prims | cached meshlib |
// dynamic meshlib], from the cache and main's list of the rows it produced this
// frame ([dynamic prims | dynamic meshlib]).
void Renderer::AssembleCachedRowsInto(std::vector<DrawCandidate>& out,
                                      const std::vector<DrawCandidate>& dynamicRows,
                                      uint32_t dynPrimCount) const
{
    const auto cacheMesh = m_candCache.begin() + m_candCachePrimCount;
    const auto dynMesh   = dynamicRows.begin() + dynPrimCount;
    out.clear();
    out.reserve(m_candCache.size() + dynamicRows.size());
    out.insert(out.end(), m_candCache.begin(), cacheMesh);
    out.insert(out.end(), dynamicRows.begin(), dynMesh);
    out.insert(out.end(), cacheMesh, m_candCache.end());
    out.insert(out.end(), dynMesh, dynamicRows.end());
}

// On the lane, first thing in PrepareFrameOnLane: the same layout, assembled
// into m_candLaneScratch, which keeps its size between frames so in steady state
// the copy is the only write (rebuilding it would value-initialise ~7,000 rows
// first). The ~2.6 MB of trivially-copyable rows is copied in pieces shared with
// workers: this is time before the frame's first submit, which a GPU-bound frame
// waits on. Everything after it (tracking, classify, plan, nest) reads the list.
std::vector<Renderer::DrawCandidate>& Renderer::AssembleLaneCandidates()
{
    ZoneScopedN("BRS.Assemble.Lane");
    PROF_SCOPE("BRS.Assemble.Lane");
    static_assert(std::is_trivially_copyable_v<DrawCandidate>);
    const std::vector<DrawCandidate>& dyn = m_candScratch;
    const size_t cp = m_candCachePrimCount;
    const size_t cm = m_candCache.size() - cp;
    const size_t dp = m_laneDynPrimCount;
    const size_t dm = dyn.size() - dp;
    const size_t total = cp + dp + cm + dm;
    std::vector<DrawCandidate>& out = m_candLaneScratch;
    if (out.size() != total) out.resize(total);
    struct Seg { DrawCandidate* dst; const DrawCandidate* src; size_t n; };
    const Seg segs[4] = {
        { out.data(),                m_candCache.data(),      cp },
        { out.data() + cp,           dyn.data(),              dp },
        { out.data() + cp + dp,      m_candCache.data() + cp, cm },
        { out.data() + cp + dp + cm, dyn.data() + dp,         dm },
    };
    // Pieces of at most kPiece rows, round-robin over the lanes.
    constexpr size_t kPiece = 1024;   // ~250 KB
    const uint32_t lanes = (std::min)(4u, (std::max)(1u, TaskSystem::Get().GetWorkerCount()));
    auto runLane = [&segs, lanes](uint32_t lane) {
        size_t piece = 0;
        for (const Seg& sg : segs)
            for (size_t off = 0; off < sg.n; off += kPiece, ++piece)
                if (piece % lanes == lane)
                    std::memcpy(sg.dst + off, sg.src + off,
                                sizeof(DrawCandidate) * (std::min)(kPiece, sg.n - off));
    };
    if (lanes > 1 && total >= 2 * kPiece)
    {
        Sys::CpuTaskScope scope(lanes - 1);
        for (uint32_t lane = 1; lane < lanes; ++lane) scope.Submit([&runLane, lane] { runLane(lane); });
        runLane(0);
        scope.Join();
    }
    else
        runLane(0);
    ++m_laneAssembleFrames;
#if WITH_HARNESS
    if (m_laneAssembleRefValid)
    {
        m_laneAssembleRefValid = false;
        const char* firstField = "";
        size_t firstRow = 0;
        const uint32_t mismatches = DiffCandidateLists(out, m_laneAssembleRef, firstField, firstRow);
        if (mismatches) ++m_laneAssembleBadFrames;
        ENGINE_CHECK_ONCE(mismatches == 0,
            "[LANEASSEMBLE_ORACLE] the lane's assembled list differs from main's: %u field "
            "mismatches, first %s at row %zu", mismatches, firstField, firstRow);
        if (mismatches || m_laneAssembleFrames % 60 == 0)
            LOG_INFO("[LANEASSEMBLE_ORACLE] frames=%llu rows=%zu cached=%zu dynPrims=%u mismatches=%u "
                     "first=%s@%zu badFrames=%llu lanes=%u match=%u",
                     static_cast<unsigned long long>(m_laneAssembleFrames), out.size(),
                     m_candCache.size(), m_laneDynPrimCount, mismatches, firstField, firstRow,
                     static_cast<unsigned long long>(m_laneAssembleBadFrames), lanes,
                     mismatches == 0 ? 1u : 0u);
    }
#endif
    return out;
}

// Main's share of a lane plan (-brsplanlane): what main reads before the
// hand-off or what must not happen on the lane - the GPU-compaction latch and the
// shadow-args clamp (UploadFrameData reads them on main), the two generations the
// FramePacket carries, PlanEmit's ctx.stats seed (m_frameStats is main's from the
// hand-off on), and the surface-cache atlas (AcquirePage creates it lazily; a
// CreateTexture on the lane would interleave bindless handles with main's).
void Renderer::PrepareLanePlan(const std::vector<DrawCandidate>& candidates)
{
    ZoneScopedN("BRS.PlanLane.Main");
    m_planOnLaneThisFrame = true;
    const uint32_t frameSlot = m_frameUploadSlot;
    SceneEmitWrite().gpuCompactionFrameActive = GpuCompactionPreconditions(frameSlot);
    SceneEmitWrite().shadowArgsFrameActive =
        SceneEmitWrite().shadowArgsFrameActive && SceneEmitWrite().gpuCompactionFrameActive;
    // The meshlet path's frame latch, beside the GPU-compaction latch.
    SceneEmitWrite().meshletFrameActive = MeshletPathPreconditions(SceneEmitWrite().meshletReason);
    SceneEmitWrite().meshletCullMask    = m_meshletCullMask;
    // geometry.lodScale: the player's multiplier on both taus - exact (the LOD test
    // is homogeneous in tau), and 0 stays 0.
    SceneEmitWrite().meshletLodTau      = m_meshletLodTau * m_meshletLodScale;
    SceneEmitWrite().shadowMeshletFrameActive =
        SceneEmitWrite().meshletFrameActive && SceneEmitWrite().shadowArgsFrameActive &&
        m_shadowPass && m_shadowPass->MeshletPsoReady();
    SceneEmitWrite().meshletShadowLodTau = m_meshletShadowLodTau * m_meshletLodScale;
    ++m_sortIdxGen;
    if (SurfacePagesEarlyActive()) ++m_batchPlanGen;
    m_emitStatsSeed = m_frameStats;
    if (m_surfaceCache.IsEnabled() && !m_surfaceCache.AtlasReady())
    {
        const auto eligible = &Renderer::SurfaceAtlasEligible;
        bool any = std::any_of(candidates.begin(), candidates.end(), eligible);
        // The cached rows are not in main's list when the lane assembles it; their
        // answer is kept with the cache.
        if (!any && m_laneAssembleThisFrame) any = m_candCacheAtlasEligible;
        // With the classify on the lane these rows are still unclassified and the
        // procedural ones still in their own scratch. Classify only drops a row or turns
        // it into a Shadow / secondary packet, never an Opaque one, so this is still a
        // superset: the atlas may be created a frame or two early (an off-screen
        // eligible row), never late, and never on the lane.
        if (!any && m_classifyOnLaneThisFrame)
            any = std::any_of(m_proceduralScratch.begin(), m_proceduralScratch.end(), eligible);
        if (any) m_surfaceCache.PrepareAtlas(m_gfx);
    }
}

void Renderer::BuildScene_SortAndEmit(const std::vector<DrawCandidate>& candidates)
{
    ZoneScopedN("BS.SortAndEmit");
    // The throw path must join the scopes of both halves. The three CpuTaskScopes
    // are created in BuildScene_PlanEmit and normally joined by
    // BuildScene_EmitBatches' SeamGuard; a throw inside PlanEmit (SubmitCpu refusing
    // during shutdown, AcquirePage's ThrowIfFailed in pass B, bad_alloc) never
    // reaches EmitBatches, and the plan lanes would stay in flight holding raw
    // pointers into m_sortIdx and m_planScratch, which the next BuildRenderScene
    // resizes. Same order as the SeamGuard: planScope, materialTailScope,
    // earlyMaterialScope, then the [EMITZONE] report.
    struct PlanEmitThrowGuard {
        Renderer* r;
        bool      armed = true;
        ~PlanEmitThrowGuard() {
            if (!armed) return;
            r->m_emitCtx.planScope.reset();
            r->m_emitCtx.materialTailScope.reset();
            r->m_emitCtx.earlyMaterialScope.reset();
            r->FinishSortEmitZones();
        }
    } planEmitThrowGuard{ this };
    // -brsplanlane: a deferred frame hands the whole emit half to the lane, plan
    // included. Main only does what must be decided before the hand-off
    // (PrepareLanePlan) and folds the lane's previous publish, where the default arm
    // does.
    if (m_emitDeferred && m_brsPlanLane)
    {
        planEmitThrowGuard.armed = false;   // nothing was started here
        PrepareLanePlan(candidates);
        ConsumeEmitStats();
        return;
    }
    {
        ZoneScopedN("BRS.PlanEmit");
        const auto t0 = std::chrono::steady_clock::now();
        BuildScene_PlanEmit(candidates);
        Diag::FrameTiming::Get().brsPlanMs = static_cast<float>(
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count());
    }
    // PlanEmit returned: the scopes are EmitBatches' to join (on this thread or on
    // the lane). Disarm rather than join: the prefix lanes must still be running
    // when the nest starts consuming them.
    planEmitThrowGuard.armed = false;
    // -brslane: the emit half is owed to the lane, whose PrepareFrameOnLane calls the
    // line below with this same vector (m_candScratch, whose next writer is
    // BuildRenderScene(F+1), after the Animation-boundary drain that joins the lane).
    // Unless the batch plan was refused this frame: BuildScene_PlanEmit has then
    // cleared m_emitDeferred and the emit half runs here, on main, in the default
    // path's position, because a refused plan puts both allocators and the
    // surface-cache publish epoch back in the nest and main reads that epoch
    // (DDGISceneAS::Gather) after the hand-off.
    if (!m_emitDeferred) RunEmitHalf(candidates);
    // The one place m_frameStats changes, at the same program position in both arms:
    //   default   - RunEmitHalf just published this frame's census, so every reader
    //               downstream (debug HUD, Lua Debug.Stats, frames.jsonl) sees it at
    //               this point in the frame.
    //   -brslane  - what is folded is the lane's publish of F-1. Main is inside
    //               BeginFrame(F), whose publication guard refused to proceed while
    //               m_laneEmitOwed was set, so the lane is done with F-1 and its slot
    //               is quiet. The census is one frame old, and statsFrame says so.
    // Do not fold at the head of BeginFrame instead: that lands before
    // BuildScene_ClassifyCandidates writes matBitsMismatches, and the two arms would
    // disagree about that field.
    ConsumeEmitStats();
}

// FrameStats across the seam (the member block beside m_frameStats says why).
// Every field is either emit-owned (copied by FoldEmitStats) or main-owned (left
// out and named in that member block); a field that is neither is silently stale
// or clobbered under -brslane. The static_assert stops FrameStats from growing
// without someone classifying the new field.
static_assert(sizeof(Renderer::FrameStats) == 208,
    "FrameStats changed size: classify the new field as emit-owned (add it to "
    "Renderer::FoldEmitStats) or main-owned (leave it out and name it in the "
    "member block beside m_frameStats), then update this number.");

void Renderer::FoldEmitStats(FrameStats& dst, const FrameStats& src)
{
    // the triangle census (zeroed in PlanEmit, accumulated by the nest)
    dst.triOpaque            = src.triOpaque;
    dst.triTransparent       = src.triTransparent;
    dst.triWater             = src.triWater;
    dst.triOutline           = src.triOutline;
    dst.triShadowOnly        = src.triShadowOnly;
    dst.triMainView          = src.triMainView;
    // the post-nest tail's assigned-once counters
    dst.drawPackets          = src.drawPackets;
    dst.staticInstSlots      = src.staticInstSlots;
    dst.dynamicInstances     = src.dynamicInstances;
    dst.lights               = src.lights;
    dst.instancesEmitted     = src.instancesEmitted;
    dst.cpuCullBypassed      = src.cpuCullBypassed;
    dst.visibleDrawCmds      = src.visibleDrawCmds;
    dst.visibleDrawCmdsValid = src.visibleDrawCmdsValid;
    dst.gpuVisibleTriangles  = src.gpuVisibleTriangles;
    dst.gpuVisibleInstances  = src.gpuVisibleInstances;
    dst.gpuVisibleCommands   = src.gpuVisibleCommands;
    dst.gpuVisStatsValid     = src.gpuVisStatsValid;
    dst.meshletPath          = src.meshletPath;
    dst.meshletTested        = src.meshletTested;
    dst.meshletLodRejected   = src.meshletLodRejected;
    dst.meshletCullFrustum   = src.meshletCullFrustum;
    dst.meshletCullCone      = src.meshletCullCone;
    dst.meshletCullHiZ       = src.meshletCullHiZ;
    dst.meshletVisible       = src.meshletVisible;
    dst.meshletStatsFrame    = src.meshletStatsFrame;
    dst.meshletTris          = src.meshletTris;
    dst.meshletStatsValid    = src.meshletStatsValid;
    // the material counters (matBatches assigned, matSnapMismatches summed)
    dst.matBatches           = src.matBatches;
    dst.matBitsMismatches    = src.matBitsMismatches;
    dst.matSnapMismatches    = src.matSnapMismatches;
    // and the tag that says which frame all of the above belongs to
    dst.statsFrame           = src.statsFrame;
    dst.statsValid           = src.statsValid;
    dst.statsFromLane        = src.statsFromLane;
    // DELIBERATELY NOT COPIED (main-owned, written outside the emit half):
    //   triScene / triSceneValid   worldUIVerts
    //   previewViewsRendered / previewViewDraws
}

void Renderer::PublishEmitStats(float emitMs)
{
    const uint64_t seq      = m_emitStatsSeq.load(std::memory_order_relaxed);
    const uint64_t consumed = m_emitStatsConsumed.load(std::memory_order_relaxed);
    // At most one publish may be outstanding, which is what makes two slots enough:
    // main consumes at the end of BuildScene_SortAndEmit(F+1), and the lane cannot
    // start F+1's emit half before main hands it F+1. A fire means an edit broke
    // that ordering and this publish would overwrite a slot main has not read.
    ENGINE_CHECK_ONCE(seq - consumed <= 1u,
        "[EMITSTATS] publish %llu with %llu consumed - a second emit half "
        "finished before main folded the first, so the two-slot ring is short",
        static_cast<unsigned long long>(seq),
        static_cast<unsigned long long>(consumed));
    EmitStatsSlot& slot = m_emitStatsSlot[seq & 1u];
    slot.stats  = m_emitCtx.stats;
    // Read off the thread, not off m_brsLaneEnabled: ThreadCtx is set by whoever
    // installed the scope, so this records where the half actually ran (the same
    // distinction EmitHalfInvariantMask's thread bit uses).
    slot.stats.statsFromLane = Sys::ThreadCtx::Current() == Sys::ThreadCtx::Render;
    slot.emitMs = emitMs;
    slot.planMs = m_planOnLaneThisFrame ? m_lanePlanMs : -1.0f;   // a lane plan's time; -1 when the plan ran on main
    // Release: everything written into the slot, and everything the emit half wrote
    // into m_emitCtx.stats before it, happens-before main's acquire in
    // ConsumeEmitStats. The Animation-boundary drain orders this too; the publish
    // does not rely on it.
    m_emitStatsSeq.store(seq + 1u, std::memory_order_release);
}

void Renderer::ConsumeEmitStats()
{
    const uint64_t seq = m_emitStatsSeq.load(std::memory_order_acquire);
    if (seq == m_emitStatsConsumed.load(std::memory_order_relaxed))
        return;   // nothing new: frame 0 under -brslane before the lane's
                  // first publish, or a frame whose emit half never ran
    const EmitStatsSlot& slot = m_emitStatsSlot[(seq - 1u) & 1u];
    FoldEmitStats(m_frameStats, slot.stats);
    // brsEmitMs rides the same publish: FrameTiming is read by main (HUD, editor
    // Profiler, frames.jsonl), so the lane must not store into it directly.
    Diag::FrameTiming::Get().brsEmitMs = slot.emitMs;
    // A lane plan's time, published the same way (one frame old, like brsEmitMs); a
    // main plan wrote brsPlanMs itself.
    if (slot.planMs >= 0.0f) Diag::FrameTiming::Get().brsPlanMs = slot.planMs;
    m_emitStatsConsumed.store(seq, std::memory_order_relaxed);
}

// The emit half and its timing, called from one place on the default path and
// from PrepareFrameOnLane on the lane, so both arms are measured and published
// identically.
void Renderer::RunEmitHalf(const std::vector<DrawCandidate>& candidates)
{
    ZoneScopedN("BRS.EmitBatches");
    const auto t0 = std::chrono::steady_clock::now();
    BuildScene_EmitBatches(candidates);
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    // Publish the census here, at the end of the emit half, not in
    // FinishFramePreparation: on the default path App runs that after the debug HUD
    // has called GetFrameStats(), which would add a one-frame lag. Under -brslane the
    // two points are adjacent in PrepareFrameOnLane. brsEmitMs travels with it
    // because main reads Diag::FrameTiming.
    PublishEmitStats(static_cast<float>(ms));
    // The digest (ReportEmitHalfDigest) is not taken here: the frame's material
    // transaction is submitted but not committed yet - FrameTaskContext::Finish,
    // inside FinishFramePreparation, memcpys the worker-owned scratch into the mapped
    // material table - so a digest here would hash however much had landed. It is
    // taken from FinishFramePreparation, the one point both arms reach after
    // everything this frame writes is sealed.
}

#if defined(WITH_HARNESS) && WITH_HARNESS
namespace {
// GPU virtual addresses and descriptor-heap handles are process-scoped: two runs
// of the same binary over the same scene get different ones, so folding them raw
// makes every cross-run comparison differ. An address is folded as its
// first-seen ordinal within the frame instead, which keeps how many distinct
// buffers the emit half referenced, which packets shared one and in what order
// they were first touched. A packet that points at a different texture still
// moves the digest; a rebased heap does not.
struct AddrNormalizer
{
    std::unordered_map<uint64_t, uint32_t> seen;
    uint32_t operator()(uint64_t addr)
    {
        if (addr == 0) return 0;   // 0 is a VALUE ("no bind"), not an address
        auto [it, inserted] = seen.emplace(addr, static_cast<uint32_t>(seen.size()) + 1u);
        (void)inserted;
        return it->second;
    }
};

// The same FNV-1a 64 offset basis as kMatBufFnvBasis in Renderer_Frame.cpp, so two
// instruments over the same bytes give the same answer.
constexpr uint64_t kBrsLaneFnvBasis = 14695981039346656037ull;

// Folds n bytes at p into the running FNV-1a 64 hash h (start from kBrsLaneFnvBasis).
inline uint64_t BrsLaneFold(uint64_t h, const void* p, size_t n)
{
    const auto* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
template <class T> inline uint64_t BrsLaneFoldValue(uint64_t h, const T& v)
{ return BrsLaneFold(h, &v, sizeof(T)); }

// The three mapped regions (m_instanceBufferMapped / m_materialBufferMapped /
// m_indirectArgMapped) are write-combined UPLOAD memory: a byte-at-a-time read is
// an uncached round trip per load. Folding ~2.8 MB straight off the mapping
// costs 250-375 ms per sampled frame, a quarter-second hitch once a second at
// -brslaneverify=60. So memcpy first (wide aligned loads, the one pattern WC
// memory tolerates) and fold the CPU copy; the staging buffer is a member, so a
// steady run allocates nothing.
inline uint64_t BrsLaneFoldMapped(uint64_t h, std::vector<uint8_t>& stage,
                                  const void* mapped, size_t n)
{
    if (!mapped || n == 0) return h;
    if (stage.size() < n) stage.resize(n);
    std::memcpy(stage.data(), mapped, n);
    return BrsLaneFold(h, stage.data(), n);
}
} // namespace
#endif

// ==== -brslaneverify: the emit half's byte oracle ====
// ReportEmitHalfDigest folds everything the emit half produced into one digest
// per sampled frame, for comparing the same scene across runs and arms; it is
// armed by its own flag, not by -brslane, so the baseline arm prints the same
// number. A serial replay on main is not possible: the nest allocates instance
// slots (AcquireInstanceSlot, the dynamic cursor), consumes one-way state
// (m_batchPlan rows against its instIdx cross-walk, the plan-prefix cursor) and
// commits the material transaction through a FrameTaskContext that seals once.
// Five sub-digests (dp, inst, mat, arg, sc), so a cross-run diff names which
// output moved; `match=` covers the invariants checkable within one run.
// Called from FinishFramePreparation, after everything the frame writes is
// sealed; a run without a FrameTaskContext (-framepreparelegacy) prints no
// digest, and -brslane refuses there too.

// The four invariant bits (no logging; see the declaration):
//   thread - the half ran on the thread the run's shape says: onLane must match
//            both m_brsLaneEnabled and the current ThreadCtx (Render), which is
//            set by whoever installed the scope, not by the command line.
//   index  - the payload just published is this frame's emit buffer
//            (FinishFramePreparation's own checks cover the arm-specific
//            pre-publish relation). leasedEmitIdx must be produced elsewhere and
//            earlier - the packet's emitIdx on the lane arm, the write index
//            captured before the publish on the default arm; m_emitIdx would just
//            read back what PublishSceneRead stored. -brslaneselftest corrupts it.
//   epoch  - BuildScene_PlanEmit armed the context for this frame, not the
//            previous one; silent without this.
//   plan   - the nest touched neither allocator: the two surface-cache call
//            counters ResolveBatchPlan recorded still match.
uint32_t Renderer::EmitHalfInvariantMask(bool onLane, uint32_t leasedEmitIdx) const
{
#if defined(WITH_HARNESS) && WITH_HARNESS
    uint32_t mask = 0;
    const bool onRenderThread = Sys::ThreadCtx::Current() == Sys::ThreadCtx::Render;
    if (onLane != m_brsLaneEnabled || onLane != onRenderThread) mask |= kEmitHalfThread;
    if (m_sceneRead != leasedEmitIdx)                           mask |= kEmitHalfIndex;
    if (m_emitCtx.armedEpoch != m_materialFrameEpoch)           mask |= kEmitHalfEpoch;
    if (m_emitCtx.planActive &&
        (m_emitCtx.acquiresBeforeNest != m_surfaceCache.GetAcquireCallCount() ||
         m_emitCtx.mapsBeforeNest     != m_surfaceCache.GetMapEntityCallCount()))
        mask |= kEmitHalfPlan;
    return mask;
#else
    (void)onLane; (void)leasedEmitIdx; return 0;
#endif
}

void Renderer::ReportEmitHalfDigest(uint64_t frame, bool onLane, uint32_t leasedEmitIdx)
{
#if defined(WITH_HARNESS) && WITH_HARNESS
    ZoneScopedN("BRS.LaneOracle");
    const auto oracleBegin = std::chrono::steady_clock::now();
    struct OracleCost {
        Renderer* r; std::chrono::steady_clock::time_point t0;
        ~OracleCost() { r->m_brsLaneOracleMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count(); }
    } oracleCost{ this, oracleBegin };
    const uint32_t slot = m_frameUploadSlot;
    // 1. The draw list: order and every field, plus the remap. Field by field, not
    //    sizeof(DrawPacket): the struct has padding nobody writes and five
    //    process-scoped addresses (see AddrNormalizer), both of which would differ
    //    run to run.
    uint64_t dp = kBrsLaneFnvBasis;
    AddrNormalizer addr;
    const auto& packets = DrawPacketsWrite();
    dp = BrsLaneFoldValue(dp, static_cast<uint64_t>(packets.size()));
    for (const DrawPacket& p : packets)
    {
        dp = BrsLaneFoldValue(dp, p.meshDescriptorIndex);
        dp = BrsLaneFoldValue(dp, p.instanceOffset);
        dp = BrsLaneFoldValue(dp, p.instanceCount);
        dp = BrsLaneFoldValue(dp, p.vertexOrIndexCount);
        dp = BrsLaneFoldValue(dp, p.materialIndex);
        dp = BrsLaneFoldValue(dp, p.permutation);
        dp = BrsLaneFoldValue(dp, static_cast<uint32_t>(p.filter));
        dp = BrsLaneFoldValue(dp, p.sortKey);
        dp = BrsLaneFoldValue(dp, addr(p.texBaseColor));
        dp = BrsLaneFoldValue(dp, addr(p.texSurfaceMap));
        dp = BrsLaneFoldValue(dp, addr(p.texNormalMap));
        dp = BrsLaneFoldValue(dp, p.outlinePixels);
        const uint32_t flags =
            (uint32_t(p.screenSpaceOutline)    << 0) | (uint32_t(p.isPickingOutline)      << 1) |
            (uint32_t(p.mirrored)              << 2) | (uint32_t(p.castShadow)            << 3) |
            (uint32_t(p.renderInMainPass)      << 4) | (uint32_t(p.perObjectShadowCaster) << 5) |
            (uint32_t(p.hairShadowCaster)      << 6) | (uint32_t(p.faceOverlay)           << 7) |
            (uint32_t(p.depthPrepassEligible)  << 8) | (uint32_t(p.batchHasDissolve)      << 9) |
            (uint32_t(p.stencilRef)           << 16) | (uint32_t(p.shadowCullMode)       << 24);
        dp = BrsLaneFoldValue(dp, flags);
        dp = BrsLaneFoldValue(dp, p.viewMask);
        dp = BrsLaneFoldValue(dp, p.faceOverlayParams);
        dp = BrsLaneFoldValue(dp, p.prevPosElementBase);
        dp = BrsLaneFoldValue(dp, p.customPSID);
        dp = BrsLaneFoldValue(dp, p.customVSID);
        dp = BrsLaneFoldValue(dp, addr(p.customCbvVA));
        dp = BrsLaneFoldValue(dp, addr(p.customTexTable));
    }
    const auto& remap = InstanceRemapWrite();
    dp = BrsLaneFoldValue(dp, static_cast<uint64_t>(remap.size()));
    if (!remap.empty())
        dp = BrsLaneFold(dp, remap.data(), remap.size() * sizeof(uint32_t));
    // 2. the instance bytes actually staged into this frame's upload slot.
    uint64_t inst = kBrsLaneFnvBasis;
    // From the emit half's own stats, not m_frameStats: under -brslane this runs on
    // the lane before main folds (in BuildScene_SortAndEmit(F+1)), so m_frameStats
    // there still describes the previous frame, while on the default path it is
    // already this frame's. m_emitCtx.stats is this frame's in both arms.
    const uint32_t instances = (std::min)(m_emitCtx.stats.instancesEmitted, m_maxInstances);
    inst = BrsLaneFoldMapped(inst, m_brsLaneStage, m_instanceBufferMapped[slot],
                             size_t(instances) * sizeof(GPUInstanceData));
    // 3. the material table, up to the highest slot ever handed out - the same
    //    bound [MATBUF] uses, and for the same reason (everything above it is
    //    the creation-time pre-fill and would hide a difference behind it).
    uint64_t mat = kBrsLaneFnvBasis;
    mat = BrsLaneFoldMapped(mat, m_brsLaneStage, m_materialBufferMapped[slot],
                            size_t(m_persistentMatSlotNext) * sizeof(Resource::MaterialGPUData));
    // 4. the indirect args, the groups they were cut into, and the shadow
    //    bucket layout the two routing metas were filled from.
    uint64_t arg = kBrsLaneFnvBasis;
    const auto& se = SceneEmitWrite();
    arg = BrsLaneFoldValue(arg, se.indirectDrawCount);
    arg = BrsLaneFoldValue(arg, static_cast<uint64_t>(se.indirectGroups.size()));
    for (const IndirectGroup& g : se.indirectGroups)   // same padding/address
    {                                                  // reasoning as DrawPacket
        arg = BrsLaneFoldValue(arg, g.perm);
        arg = BrsLaneFoldValue(arg, uint32_t(g.stencilRef) | (uint32_t(g.mirrored) << 8) |
                                    (uint32_t(g.depthPrepassEligible) << 16) | (uint32_t(g.meshlet) << 24));
        arg = BrsLaneFoldValue(arg, g.customPSID);
        arg = BrsLaneFoldValue(arg, g.customVSID);
        arg = BrsLaneFoldValue(arg, addr(g.customCbvVA));
        arg = BrsLaneFoldValue(arg, addr(g.customTexTable));
        arg = BrsLaneFoldValue(arg, g.argOffset);
        arg = BrsLaneFoldValue(arg, g.cmdCount);
    }
    const uint32_t args = (std::min)(se.indirectDrawCount, m_maxInstances);
    arg = BrsLaneFoldMapped(arg, m_brsLaneStage, m_indirectArgMapped[slot],
                            size_t(args) * sizeof(IndirectDrawCommand));
    arg = BrsLaneFold(arg, se.shadowGroupBase, sizeof(se.shadowGroupBase));
    arg = BrsLaneFold(arg, se.shadowGroupCap,  sizeof(se.shadowGroupCap));
    // 5. the surface cache's decisions - the epoch DDGISceneAS::Gather read on
    //    main after the hand-off, the two call counters the hoist moved, and
    //    the census the HUD and frames.jsonl read.
    const uint32_t scEpoch = m_surfaceCache.GetPublishEpoch();
    uint64_t sc = kBrsLaneFnvBasis;
    sc = BrsLaneFoldValue(sc, scEpoch);
    sc = BrsLaneFoldValue(sc, m_surfaceCache.GetAcquireCallCount());
    sc = BrsLaneFoldValue(sc, m_surfaceCache.GetMapEntityCallCount());
    sc = BrsLaneFoldValue(sc, m_emitCtx.stats.drawPackets);
    sc = BrsLaneFoldValue(sc, m_emitCtx.stats.staticInstSlots);
    sc = BrsLaneFoldValue(sc, m_emitCtx.stats.dynamicInstances);
    sc = BrsLaneFoldValue(sc, m_emitCtx.stats.matBatches);
    uint64_t all = BrsLaneFoldValue(kBrsLaneFnvBasis, dp);
    all = BrsLaneFoldValue(all, inst);
    all = BrsLaneFoldValue(all, mat);
    all = BrsLaneFoldValue(all, arg);
    all = BrsLaneFoldValue(all, sc);

    // ---- Within-run invariants and their positive control ----
    // -brslaneselftest=N: on frame N, run the predicate against the side that did not
    // finish the frame, then the side that did, then let the real check below see
    // the wrong one (the pack's error budget is exactly 1). `clean=0` is asserted
    // beside `detected=`, so a predicate that always answers "wrong" fails.
    if (m_brsLaneSelfTestFrame >= 0 &&
        frame == static_cast<uint64_t>(m_brsLaneSelfTestFrame))
    {
        const uint32_t detected = EmitHalfInvariantMask(!onLane, leasedEmitIdx);
        const uint32_t clean    = EmitHalfInvariantMask(onLane, leasedEmitIdx);
        // The index bit's own control, logged as separate fields so the four that packs
        // pin keep their meaning. There are two scene buffers, so `^ 1` is the only other
        // index, and the payload cannot have been published into it.
        const uint32_t detectedIdx = EmitHalfInvariantMask(onLane, leasedEmitIdx ^ 1u);
        LOG_INFO("[BRSLANE_SELFTEST] frame=%llu corrupted=1 detected=%u expected=%u "
                 "clean=%u match=%u detectedIdx=%u expectedIdx=%u matchIdx=%u",
                 static_cast<unsigned long long>(frame),
                 detected, unsigned(kEmitHalfThread), clean,
                 unsigned(detected == kEmitHalfThread && clean == 0u),
                 detectedIdx, unsigned(kEmitHalfIndex),
                 unsigned(detectedIdx == kEmitHalfIndex));
        onLane = !onLane;
    }
    const uint32_t mask = EmitHalfInvariantMask(onLane, leasedEmitIdx);
    const bool match = mask == 0;
    ENGINE_CHECK(match,
        "[BRS_LANE_ORACLE] frame=%llu lane=%u thread=%u index=%u epoch=%u "
        "plan=%u - the emit half did not finish on the half of the seam it was "
        "handed", static_cast<unsigned long long>(frame), unsigned(onLane),
        unsigned(!(mask & kEmitHalfThread)), unsigned(!(mask & kEmitHalfIndex)),
        unsigned(!(mask & kEmitHalfEpoch)), unsigned(!(mask & kEmitHalfPlan)));
    LOG_INFO("[BRS_LANE_ORACLE] frame=%llu lane=%u digest=0x%016llx "
             "dp=0x%016llx inst=0x%016llx mat=0x%016llx arg=0x%016llx sc=0x%016llx "
             "packets=%u instances=%u matSlots=%u args=%u scEpoch=%u "
             "ctxViolations=%llu match=%u",
             static_cast<unsigned long long>(frame), unsigned(onLane),
             static_cast<unsigned long long>(all),
             static_cast<unsigned long long>(dp), static_cast<unsigned long long>(inst),
             static_cast<unsigned long long>(mat), static_cast<unsigned long long>(arg),
             static_cast<unsigned long long>(sc),
             m_emitCtx.stats.drawPackets, instances, m_persistentMatSlotNext, args, scEpoch,
             static_cast<unsigned long long>(Sys::ThreadCtx::Violations().load()),
             unsigned(match));
#else
    (void)frame; (void)onLane; (void)leasedEmitIdx;
#endif
}

// [EMITZONE] slot mark: charges the time since the last mark to `slot`. Both
// halves call it; the time point it advances is the context's.
void Renderer::EmitZoneMark(int slot)
{
    static_assert(kSeZoneCount == kSortEmitZoneCount,
                  "the file-scope [EMITZONE] accumulator and the slot map disagree");
    const auto now = std::chrono::steady_clock::now();
    g_seAcc[slot] += std::chrono::duration<double, std::milli>(now - m_emitCtx.seT).count();
    m_emitCtx.seT = now;
}

// Charges the tail slot and prints [EMITZONE] every -phaselog_every frames (and
// [EMIT_PLAN_AB] under -batchplanab). Called from a destructor - EmitBatches'
// seam guard, or PlanEmit's throw guard - so an early return or a throwing oracle
// still charges the tail and prints, after the three scope joins.
void Renderer::FinishSortEmitZones()
{
    EmitContext& ctx = m_emitCtx;
    if (ctx.seAlternating)
    {
        // Same-process A/B without a Tracy connection. Measure before
        // printing either profiler report so its own I/O is excluded.
        static std::array<std::vector<double>, 2> s_samples;
        s_samples[ctx.sePlanned ? 1 : 0].push_back(std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - ctx.seStart).count());
        if (s_samples[0].size() + s_samples[1].size() >= 120)
        {
            for (uint32_t mode = 0; mode < 2; ++mode)
            {
                auto& values = s_samples[mode];
                if (values.empty()) continue;
                double total = 0;
                for (double value : values) total += value;
                std::sort(values.begin(), values.end());
                LOG_INFO("[EMIT_PLAN_AB] planned=%u count=%zu meanMs=%.6f p50Ms=%.6f p95Ms=%.6f",
                    mode, values.size(), total / values.size(),
                    (values[(values.size() - 1) / 2] + values[values.size() / 2]) * 0.5,
                    values[(values.size() - 1) * 95 / 100]);
                values.clear();
            }
        }
    }
    g_seAcc[kSeZoneCount - 1] +=
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - ctx.seT).count();
    static const int s_every = []{
        const auto& cl = Sys::CommandLine::Get();
        return cl.Has("-phaselog_every") ? cl.GetInt("-phaselog_every", 60) : 0;
    }();
    if (s_every <= 0 || ++g_seN % s_every != 0) return;
    double tot = 0.0;
    for (double v : g_seAcc) tot += v;
    LOG_INFO("[EMITZONE] per frame (total %.2f ms): keys %.2f sort %.2f "
             "preamble %.2f nest %.2f truncTelem %.2f slotMaint %.2f "
             "groupRoute %.2f shadowRoute %.2f tail %.2f",
             tot / s_every, g_seAcc[0]/s_every, g_seAcc[1]/s_every, g_seAcc[8]/s_every,
             g_seAcc[2]/s_every, g_seAcc[3]/s_every, g_seAcc[4]/s_every, g_seAcc[5]/s_every,
             g_seAcc[6]/s_every, g_seAcc[9]/s_every);
    for (double& v : g_seAcc) v = 0.0;
}

// The streamed plan prefix's consumer: waits until the plan lanes have written
// the emitPlan rows up to `candidate`. Driven from both halves off the same
// cursor - ResolveBatchPlan's pass A in PlanEmit and the nest in EmitBatches - so
// it is a member function over the context rather than two lambdas.
void Renderer::EnsurePlanPrefix(uint32_t candidate)
{
    EmitContext& ctx = m_emitCtx;
    // Armed for this frame? The early-out reads ctx.prefixPlan, which holds last
    // frame's answer if the arming block has not run yet: `false` there makes this
    // waiter a no-op and the caller walks emitPlan rows the plan lanes are still
    // writing - no crash, no warning, a frame built off a half-written plan (see
    // EmitContext::prefixArmedEpoch). Free when it passes.
    ENGINE_CHECK_ONCE(ctx.prefixArmedEpoch == m_materialFrameEpoch,
        "[EMITCTX] EnsurePlanPrefix ran against a context armed for epoch %llu "
        "while this frame is epoch %llu - prefixPlan/readyPlanEnd/planBounds are "
        "the PREVIOUS frame's, so the streamed plan is being consumed unwaited",
        static_cast<unsigned long long>(ctx.prefixArmedEpoch),
        static_cast<unsigned long long>(m_materialFrameEpoch));
    if (!ctx.prefixPlan || candidate < ctx.readyPlanEnd) return;
    ZoneScopedN("BS.PlanPrefix.Wait");
    while (candidate >= ctx.readyPlanEnd) {
        ENGINE_CHECK(ctx.nextPlanLane < ctx.activePlanLanes, "plan prefix out of range");
        ctx.planHandles[ctx.nextPlanLane].Wait();
        ctx.readyPlanEnd = ctx.planBounds[++ctx.nextPlanLane];
    }
}


// Half one: the sort, the preamble and ResolveBatchPlan. Ends at EmitZoneMark(8),
// the lane hand-off point, having written every field of m_emitCtx that the nest
// and the post-nest tail may read.
void Renderer::BuildScene_PlanEmit(const std::vector<DrawCandidate>& candidates)
{
    EmitContext& ctx = m_emitCtx;
    // True when this runs on the lane for a frame main already handed over
    // (PrepareLanePlan did main's share).
    const bool planOnLane = m_planOnLaneThisFrame;
#if WITH_HARNESS
    // -emitctxstale=<epoch>: withhold the context's two arming stamps for one frame
    // so the consumers' tripwires are exercised. Nothing else changes - the frame
    // still renders, and the pack asserts both that the checks fired and that the
    // run completed. The value is the material frame epoch (one per
    // BuildRenderScene), the only per-frame number in scope this early.
    static const uint64_t s_ctxStaleEpoch = []() -> uint64_t {
        const auto& cl = Sys::CommandLine::Get();
        const int v = cl.Has("-emitctxstale") ? cl.GetInt("-emitctxstale", 0) : 0;
        return v > 0 ? static_cast<uint64_t>(v) : 0ull;
    }();
    const bool ctxStale = s_ctxStaleEpoch != 0 && m_materialFrameEpoch == s_ctxStaleEpoch;
    if (ctxStale)
        LOG_INFO("[EMITCTX] harness -emitctxstale: epoch %llu runs with the "
                 "context's arming stamps withheld - both consumer tripwires "
                 "must fire",
                 static_cast<unsigned long long>(m_materialFrameEpoch));
#else
    constexpr bool ctxStale = false;
#endif
    // The hoist, latched once (see SurfacePagesEarlyActive).
    const bool earlyPlan = SurfacePagesEarlyActive();
#if WITH_HARNESS
    bool     verifySurfacePages = false;
    uint32_t surfacePagesEvery  = 1;
    {
        const auto& spCmd = Sys::CommandLine::Get();
        if (spCmd.Has("-surfacepagesverify"))
        {
            verifySurfacePages = true;
            const int every = spCmd.GetInt("-surfacepagesverify", 1);
            surfacePagesEvery = every > 0 ? static_cast<uint32_t>(every) : 1u;
        }
    }
    ++g_surfacePagesFrame;
    verifySurfacePages = verifySurfacePages && earlyPlan &&
                         (g_surfacePagesFrame % surfacePagesEvery == 0);
#endif

    bool parallelPlan = (Sys::JobSplittingDefaultsEnabled() || candidates.size() >= 2048) && TaskSystem::Get().GetWorkerCount() >= 2;
    bool verifyPlan = false;
    bool alternatingPlan = false;
#if WITH_HARNESS
    const auto& planCmd = Sys::CommandLine::Get();
    parallelPlan = parallelPlan && !planCmd.Has("-batchplanserial");
    const int alternate = planCmd.GetInt("-batchplanab", 0);
    alternatingPlan = alternate != 0;
    static uint64_t s_alternatePlanFrame = 0;
    if (alternate != 0) parallelPlan = parallelPlan && ((++s_alternatePlanFrame + alternate) % 2 == 0);
    verifyPlan = parallelPlan && planCmd.Has("-batchplanverify");
#endif
#ifdef TRACY_ENABLE
    ZoneTransientN(planFrameZone, parallelPlan ? "BS.SortAndEmit.ParallelPlan" : "BS.SortAndEmit.SerialPlan", true);
#endif
    bool overlapPlan = parallelPlan && Sys::JobSplittingDefaultsEnabled();
    bool verifyOverlapPlan = false;
#if WITH_HARNESS
    const auto& overlapCmd = Sys::CommandLine::Get();
    overlapPlan = parallelPlan && (overlapCmd.Has("-planoverlap") || Sys::JobSplittingDefaultsEnabled());
    verifyOverlapPlan = overlapCmd.Has("-planoverlapverify");
#endif
    bool prefixPlan = parallelPlan && Sys::JobSplittingDefaultsEnabled();
    bool verifyPrefixPlan = false, widePrefix = Sys::JobSplittingDefaultsEnabled();
#if WITH_HARNESS
    const auto& prefixCmd = Sys::CommandLine::Get();
    widePrefix = prefixCmd.Has("-planprefixwide") || Sys::JobSplittingDefaultsEnabled();
    // -planprefixnarrow: ask for the narrow prefix budget (planLanes, min(4,
    // workers)) instead of the wide one (min(16, workers)). With job splitting on by
    // default, -planprefix and -planprefixwide both select the wide budget, so
    // without this the narrow one has no coverage. Narrow caps at 4, so a pack
    // asking for it is machine-independent on any host with >= 4 workers; `wide=` on
    // [PLAN_PREFIX_ORACLE] reports which budget ran (the two agree at 2 workers).
    if (prefixCmd.Has("-planprefixnarrow")) widePrefix = false;
    prefixPlan = parallelPlan &&
        (prefixCmd.Has("-planprefix") || prefixCmd.Has("-planprefixwide") || Sys::JobSplittingDefaultsEnabled());
    if (prefixCmd.Has("-planprefixoff")) prefixPlan = false;
    verifyPrefixPlan = prefixCmd.Has("-planprefixverify");
    if (prefixPlan) overlapPlan = true;
#endif
    ZoneNamedN(planPrefixZone, "BS.PlanPrefix.Stream", prefixPlan);
    ZoneNamedN(planFullZone, "BS.PlanPrefix.Barrier", !prefixPlan);
    ZoneNamedN(planOverlapZone, "BS.PlanPlacement.Overlap", overlapPlan);
    ZoneNamedN(planBarrierZone, "BS.PlanPlacement.Barrier", !overlapPlan);
    // [EMITZONE] sub-phase breakdown, same shape as [BUILDZONE] one level up.
    // Slot map, written once so the marks and the log labels cannot drift:
    //   0 keys | 1 sort | 8 preamble | 2 nest | 3 truncTelem | 4 slotMaint
    //   5 groupRoute | 6 shadowRoute | 9 tail (scope exit)
    // Anchor marks on code text, not line numbers. The nest slot is the nest alone;
    // the work between the sort and the nest is the preamble.
    // Noise floor: the nest slot spreads ~0.2 ms across windows of the same binary
    // and drifts ~0.25 ms over a session, so a smaller claim needs a paired,
    // interleaved A/B with the spread reported.
    ctx.seT = std::chrono::steady_clock::now();
    // The [EMITZONE] frame bracket, read by FinishSortEmitZones from the context.
    ctx.seStart       = ctx.seT;
    ctx.seAlternating = alternatingPlan;
    ctx.sePlanned     = parallelPlan;

    bool deferMaterial = Sys::JobSplittingDefaultsEnabled() ||
        (candidates.size() >= 2048 && TaskSystem::Get().GetWorkerCount() >= 2);
    bool verifyMaterialWrites = false;
#if WITH_HARNESS
    const auto& materialCmd = Sys::CommandLine::Get();
    if (materialCmd.Has("-materialcpujobs")) deferMaterial = true;
    verifyMaterialWrites = materialCmd.Has("-materialcpuverify");
#endif
    // -brslane: main holds no FrameTaskContext on a deferred frame - a context binds
    // to its creating thread (FrameTaskContext::CheckOwner throws for any other), and
    // this frame's material writes go through the lane's. m_emitDeferred promises one
    // will exist when the nest reaches QueueFrameMaterialWrites, which keeps
    // deferMaterial / earlyMaterial / overlapMaterialTail identical to the default
    // arm instead of falling back to the serial material path.
    const bool framePreparation = (m_cpuFrame != nullptr) || m_emitDeferred;
    if (framePreparation) deferMaterial = true;
    ZoneNamedN(materialGraphZone, "BS.MaterialCPU.Graph", deferMaterial);
    ZoneNamedN(materialSerialZone, "BS.MaterialCPU.Serial", !deferMaterial);
    // The early material lane: publish immutable coarse chunks while the owner
    // continues slot/packet planning. Never expose the growing scratch vector.
    bool earlyMaterial = deferMaterial && Sys::JobSplittingDefaultsEnabled();
#if defined(WITH_HARNESS) && WITH_HARNESS
    earlyMaterial = deferMaterial &&
        (materialCmd.Has("-materialearly") || Sys::JobSplittingDefaultsEnabled());
    // -materialearlyoff: the late-only material path, asked for explicitly; it never
    // turns the early lane on. The early lane publishes up to 4 x 96 writes during
    // the owner nest and takes them from the tail lane: AIBench's ~396 writes leave
    // the tail 12, below its `count >= 64` threshold, so with earlyMaterial on by
    // default the overlapped tail lane has no coverage without this flag
    // (material_tail_ai vs material_tail_early).
    if (materialCmd.Has("-materialearlyoff")) earlyMaterial = false;
#endif
    if (framePreparation) earlyMaterial = false;
    ZoneNamedN(materialEarlyZone, "BS.MaterialCPU.Early", earlyMaterial);
    ZoneNamedN(materialLateZone, "BS.MaterialCPU.Late", deferMaterial && !earlyMaterial);
    bool overlapMaterialTail = deferMaterial && TaskSystem::Get().GetWorkerCount() > 1;
#if WITH_HARNESS
    if (materialCmd.Has("-materialtailoverlap")) overlapMaterialTail = deferMaterial;
#endif
    if (framePreparation) overlapMaterialTail = false;
    ZoneNamedN(materialTailOverlapZone, "BS.MaterialTail.Overlap", overlapMaterialTail);
    ZoneNamedN(materialTailBarrierZone, "BS.MaterialTail.Barrier", !overlapMaterialTail);
    // Created after every borrowed input; joined by EmitBatches' seam guard, or by
    // the throw guard if PlanEmit throws.
    ctx.earlyMaterialScope.emplace(earlyMaterial ? 4 : 0);
    // -brslane: this scope is created here on main and joined by EmitBatches' seam
    // guard, which on a deferred frame runs on the lane. CpuTaskScope's constructor
    // guard only refuses worker threads; AllowOwnerHandoff declares the crossing
    // intended and ordered (the hand-off is the last statement of BeginFrame, the
    // join is in PrepareFrameOnLane, the packet between them). Also marked on a
    // fallback frame: the mark only widens what is permitted, and m_emitDeferred is
    // still true here.
    if (m_emitDeferred) ctx.earlyMaterialScope->AllowOwnerHandoff();
    // ctx.materialTailScope is emplaced by EmitBatches and joined by its seam guard:
    // it outlives the material-write span and drains before the build returns,
    // exceptions included.

    bool parallelEmit = (Sys::JobSplittingDefaultsEnabled() || candidates.size() >= 2048) && TaskSystem::Get().GetWorkerCount() >= 2;
    bool reuseEmitMemory = true;
#if WITH_HARNESS
    reuseEmitMemory = !Sys::CommandLine::Get().Has("-emitmemoryoff");
    if (Sys::CommandLine::Get().Has("-emitjobs"))
        parallelEmit = TaskSystem::Get().GetWorkerCount() >= 2;
    parallelEmit = parallelEmit && !Sys::CommandLine::Get().Has("-emitserial");
#endif
    m_instStageStats = InstStageStats{};
    // Arm the emit half's private FrameStats. The nest accumulates into it (on the
    // lane under -brslane), so it is zeroed here, on the same side of the seam;
    // zeroing m_frameStats would hand a game-half reader a counter mid-accumulation.
    // Main only sees the published copy. Seeded from m_frameStats (on a lane plan,
    // from the copy PrepareLanePlan took on main), so a path that skips the tail
    // republishes the standing values; the main-owned fields ride along unused
    // (FoldEmitStats does not copy them back).
    ctx.stats = planOnLane ? m_emitStatsSeed : m_frameStats;
    // FrameViewEmit(), not FrameViewWrite(): they are the same object for the whole
    // of BeginFrame (m_emitIdx == m_sceneWrite until the hand-off advances one), and
    // the emit one still names this frame after main has run ahead.
    ctx.stats.statsFrame    = FrameViewEmit().currentFrame;
    ctx.stats.statsValid    = true;
    ctx.stats.statsFromLane = false;   // stamped for real by PublishEmitStats
    // FrameStats is never memset (the tail of BuildScene_EmitBatches assigns each
    // field), so the triangle accumulators are zeroed here or a frame that emits
    // nothing would keep the previous frame's totals.
    ctx.stats.triOpaque      = 0;
    ctx.stats.triTransparent = 0;
    ctx.stats.triWater       = 0;
    ctx.stats.triOutline     = 0;
    ctx.stats.triShadowOnly  = 0;
    ctx.stats.triMainView    = 0;
    // Sort a compact record, not a bare index array: comparing indices chases a
    // ~250 B DrawCandidate at a random offset per comparison (~180k comparator calls
    // at ~13k candidates - cache-miss traffic, not compute). The record holds a
    // 128-bit prefix of the comparator's most-significant fields plus the index; an
    // exact key tie falls through to the full comparator, which compares the prefix
    // fields again in the same order, so the two always agree.
    struct SortRec { uint64_t key0; uint64_t key1; uint32_t idx; };
    // s_sortRec lives entirely inside the sort, so it stays thread_local. Its
    // projection (m_sortIdx) is read by the nest and crosses the seam (see the
    // member's note in Renderer.h).
    thread_local std::vector<SortRec>  t_sortRec;
    auto& s_sortIdx = m_sortIdx;
    const size_t candCount = candidates.size();
    t_sortRec.resize(candCount);
    // IEEE-754 float -> order-preserving uint32 (ascending). -0.0 is folded to
    // +0.0 first: the comparator's `a.depth != b.depth` treats the two as EQUAL
    // and tiebreaks on entity, so a key that separated them would contradict it.
    auto floatKeyAsc = [](float f) -> uint32_t
    {
        uint32_t u;
        std::memcpy(&u, &f, sizeof(u));
        if (u == 0x80000000u) u = 0u;
        return (u & 0x80000000u) ? ~u : (u | 0x80000000u);
    };
    for (size_t k = 0; k < candCount; ++k)
    {
        const DrawCandidate& c = candidates[k];
        // [63:56] filter - the comparator's first field for every class.
        uint64_t key0 = static_cast<uint64_t>(static_cast<uint8_t>(c.filter)) << 56;
        uint64_t key1 = 0;
        if (IsDepthSortedFilter(c.filter))
        {
            // Painter's order: depth descending (the complemented ascending key), then
            // entity ascending - both fit, so this class never falls back. Complement at 32
            // bits before widening so the result lands in [55:24]; complementing after the
            // widen would also set [63:56] and clobber the filter (and trips MSVC C4319).
            const uint32_t depthDesc = ~floatKeyAsc(c.depth);
            key0 |= static_cast<uint64_t>(depthDesc) << 24;  // [55:24]
            key1  = static_cast<uint64_t>(c.entity) << 32;
        }
        else
        {
            key0 |= static_cast<uint64_t>(c.perm.bits)   << 24;  // [55:24]
            key0 |= static_cast<uint64_t>(c.mirrored)    << 16;  // [23:16]
            key0 |= static_cast<uint64_t>(c.stencilRef)  << 8;   // [15:8]
            key1  = (static_cast<uint64_t>(c.orderKey) << 32) | c.meshDescSlot;
            // The resource ids (texture handles, matHash) and the entity do
            // not fit - they stay in the fallback comparator below.
        }
        t_sortRec[k] = { key0, key1, static_cast<uint32_t>(k) };
    }
    EmitZoneMark(0);
    const auto lessRecord = [&candidates](const SortRec& ra, const SortRec& rb)
        {
            if (ra.key0 != rb.key0) return ra.key0 < rb.key0;
            if (ra.key1 != rb.key1) return ra.key1 < rb.key1;
            // Prefix tie - the whole comparator. The packed fields compare equal here, so it
            // falls through to the fields the 128-bit prefix could not carry.
            const DrawCandidate& a = candidates[ra.idx];
            const DrawCandidate& b = candidates[rb.idx];
            if (a.filter != b.filter)
                return static_cast<uint8_t>(a.filter) < static_cast<uint8_t>(b.filter);
            if (IsDepthSortedFilter(a.filter))
            {
                // Painter's order; entity-id tiebreaker pins equal-depth pairs (prevents flicker).
                if (a.depth != b.depth) return a.depth > b.depth;
                return a.entity < b.entity;
            }
            // PSO state first (most expensive to switch).
            if (a.perm != b.perm)
                return a.perm.bits < b.perm.bits;
            // Mirrored is PSO state too (front-face convention) and a batch-key field:
            // sorting on it keeps mirrored runs contiguous instead of fragmenting every batch
            // they interleave with.
            if (a.mirrored      != b.mirrored)      return a.mirrored      < b.mirrored;
            if (a.stencilRef    != b.stencilRef)    return a.stencilRef    < b.stencilRef;
            // Then the material's content and the mesh. Within a PSO group this decides which
            // draw comes later, and the later of two coplanar layers wins GBuffer's
            // GREATER_EQUAL. The order key has no resource id in it, so creating a texture
            // cannot reshuffle materials; grouping by texture buys nothing because the
            // material passes read textures bindless through the material record.
            if (a.orderKey      != b.orderKey)      return a.orderKey      < b.orderKey;
            if (a.meshDescSlot  != b.meshDescSlot)  return a.meshDescSlot  < b.meshDescSlot;
            // The resource ids only split what the content key could not -
            // a 32-bit collision, two runtime texture overrides - so every
            // batch identity (SameEmitBatch) stays one contiguous run.
            if (a.texBaseColor  != b.texBaseColor)  return a.texBaseColor  < b.texBaseColor;
            if (a.texSurfaceMap != b.texSurfaceMap) return a.texSurfaceMap < b.texSurfaceMap;
            if (a.texNormalMap  != b.texNormalMap)  return a.texNormalMap  < b.texNormalMap;
            if (a.matHash       != b.matHash)       return a.matHash       < b.matHash;
            // The instances of one batch in entity order: a total order, so
            // neither the sort's lane split nor the worker count can move them.
            return a.entity < b.entity;
        };
    thread_local std::vector<SortRec> t_sortScratch;
    static const bool s_legacySort = Sys::CommandLine::Get().Has("-sortlegacy");
    if (s_legacySort)
        std::sort(std::execution::par_unseq, t_sortRec.begin(), t_sortRec.end(), lessRecord);
    else
        Sys::TaskParallelSort(t_sortRec, t_sortScratch, lessRecord, true);
    EmitZoneMark(1);   // the sort itself
    // Project to the plain index array the emit sites read (one sequential pass,
    // negligible next to the sort).
    s_sortIdx.resize(candCount);
    for (size_t k = 0; k < candCount; ++k) s_sortIdx[k] = t_sortRec[k].idx;
#if WITH_HARNESS
    // -sortdump=<frame>: that frame's order of the non-depth-sorted candidates, one
    // line each, to <artifacts>/sortdump.txt. tests/unit/test_opaque_order.py
    // compares it across texture-id layouts (-texpool_pad): the order must not move.
    {
        static const int s_dumpFrame = Sys::CommandLine::Get().GetInt("-sortdump", -1);
        if (s_dumpFrame >= 0 && ctx.stats.statsFrame == static_cast<FrameIndex>(s_dumpFrame))
        {
            const std::string path = Sys::DiagnosticsDir() + "/sortdump.txt";
            FILE* f = nullptr;
            if (fopen_s(&f, path.c_str(), "w") == 0 && f)
            {
                for (size_t k = 0; k < candCount; ++k)
                {
                    const DrawCandidate& c = candidates[t_sortRec[k].idx];
                    if (IsDepthSortedFilter(c.filter)) continue;
                    std::fprintf(f, "%zu filter=%u entity=%u perm=%08x mirrored=%u stencil=%u order=%08x "
                        "mesh=%u bc=%llx sm=%llx nm=%llx matHash=%016llx ps=%u vs=%u\n",
                        k, static_cast<unsigned>(c.filter), c.entity, c.perm.bits, c.mirrored,
                        c.stencilRef, c.orderKey, c.meshDescSlot,
                        static_cast<unsigned long long>(c.texBaseColor),
                        static_cast<unsigned long long>(c.texSurfaceMap),
                        static_cast<unsigned long long>(c.texNormalMap),
                        static_cast<unsigned long long>(c.matHash), c.customPSID, c.customVSID);
                }
                std::fclose(f);
                LOG_INFO("[SORTDUMP] frame=%d candidates=%zu -> %s", s_dumpFrame, candCount, path.c_str());
            }
        }
    }
#endif
    // Stamp the permutation. The nest latches the generation it read and the
    // FramePacket carries the one published here; across threads that pair is the
    // only thing that tells "the lane read this frame's permutation" from an empty or
    // stale one, which would otherwise silently draw the wrong things.
    if (!planOnLane) ++m_sortIdxGen;   // lane plan: PrepareLanePlan reserved it
    const auto& indices = s_sortIdx;

    // ---- The frame's upload buffers and per-frame emit state ----
    // The upload slot comes from the lease latched at the head of BeginFrame, not
    // from m_gfx.GetUploadSlot(): every mapped pointer below derives from it, and a
    // re-read after main acquired F+1's slot would move all of them at once.
    const uint32_t frameSlot = m_frameUploadSlot;
    // On the lane the device slot may already be F+1's; PrepareFrameOnLane checked
    // the lease against the packet instead.
    ENGINE_CHECK_ONCE(planOnLane || frameSlot == m_gfx.GetUploadSlot(),
                      "BuildScene_SortAndEmit: upload-slot lease %u != device slot %u",
                      frameSlot, m_gfx.GetUploadSlot());
    auto* instances = static_cast<GPUInstanceData*>(m_instanceBufferMapped[frameSlot]);
    auto* matBuf   = static_cast<Resource::MaterialGPUData*>(m_materialBufferMapped[frameSlot]);
    // Slot 0 is the reserved neutral no-material record (see m_persistentMatSlotNext):
    // staging holds default PBR content there from buffer creation, but the
    // DEFAULT-heap table only receives staged spans, so stage slot 0 once per frame
    // buffer or its GPU copy stays uninitialized.
    if (matBuf && !(m_matSlot0Staged & (1u << frameSlot)))
    {
        StageMaterialSlot(frameSlot, 0);
        m_matSlot0Staged |= static_cast<uint8_t>(1u << frameSlot);
    }
    // `instIdx`, the emit walk's instance ordinal, lives only in
    // BuildScene_EmitBatches: BatchPlanRow::instBegin is what it is compared against.
    // Serial emission appends. Workers overwrite every element after resize, so keep
    // size as well as capacity to avoid value-initializing the same AABBs each frame;
    // shrinking / growing still uses the actual count.
    if (!parallelEmit || !reuseEmitMemory) InstAabbWrite().clear();
    uint32_t materialBatchCount = 0;
    // Packets that are secondary-view duplicates. Only the preview graph draws them
    // (CullWorldForPreview finds them by filter), so they must not extend the
    // indirect / cull-command range (see the indirectDrawCount assignment in
    // BuildScene_EmitBatches' tail).
    uint32_t secondaryPackets   = 0;
    // The indirect arg array's bookkeeping for this frame:
    //   argsWritten          one past the last slot written - the prefix every
    //                        consumer indexes into.
    //   secondaryArgsSkipped how many packets got no slot.
    //   firstSkippedArg      the first such slot, 0xFFFFFFFF while none was skipped -
    //                        the back-fill's start point if the sorted-tail
    //                        assumption ever fails.
    uint32_t argsWritten          = 0;
    uint32_t secondaryArgsSkipped = 0;
    uint32_t firstSkippedArg      = 0xFFFFFFFFu;
    uint32_t snapDrivenWrites   = 0;   // base writes fed by the snapshot
    uint32_t snapVerifiedWrites = 0;
    // Per-frame, on the emit half's own copy like the triangle census: the nest
    // increments matSnapMismatches and the tail reads both. This zero also discards
    // what BuildScene_ClassifyCandidates counted into matBitsMismatches (carried in
    // by the seed), in both arms; the classify-side counter is a separate defect with
    // its own consumer (transparent_reflections asserts matBitsMismatches == 0).
    ctx.stats.matBitsMismatches = 0;
    ctx.stats.matSnapMismatches = 0;
    uint32_t reusedMaterialBindings = 0;
    bool materialSlotOverflowWarned = false;
    // Per-frame instance telemetry (see the getters in Renderer.h), reset
    // unconditionally so the pool panel reads 0, not a stale value, when retained
    // instances are toggled off.
    m_instStaticLaneDeniedLastFrame = 0;
    m_instanceDropLastFrame         = 0;

    // Stable instance slots (see the Renderer.h member block). Retained mode:
    // `instances` is the dirty-staging ring (records appended compactly, copied into
    // the DEFAULT buffer by Render()), the draw-local order goes into the remap
    // buffer, and only changed records are staged. Off: every instance is compacted
    // and rewritten each frame (remap buffers untouched, PerViewCB idx stays 0).
    const bool retainInst = RetainedInstancesActive();
    if (retainInst != m_instanceRetainWasOn)
    {
        m_instanceRetainReset = true;   // runtime flag flip -> full re-upload
        m_instanceRetainWasOn = retainInst;
    }
    if (retainInst)
    {
        if (!InstCopyWrite().empty())
        {
            // Staged last frame but never drained (a frame was built without rendering), so
            // the DEFAULT buffer missed those copies and the CPU shadows are wrong:
            // re-upload everything this frame.
            LOG_WARNING("Renderer: undrained instance copy ranges (%zu) — "
                        "forcing full instance re-upload",
                        InstCopyWrite().size());
            m_instanceRetainReset = true;
        }
        if (m_instanceRetainReset)
        {
            ResetInstanceSlots();
            m_instanceRetainReset = false;
        }
        ++m_instFrameStamp;
        // Stamp everything the scene diff calls dirty this frame. The emit loop
        // (BuildScene_EmitBatches) uses "not dirty this frame and not dirty last frame"
        // as its proof that a static instance GPU record is already correct. Last frame
        // counts too: the prevWorld 2-frame rule rewrites the record on the settle frame
        // (wasMoving -> prevWorld = world), so an entity that only stopped moving last
        // frame still has a pending change.
        {
            static const bool s_verify = []{
                char* v = nullptr; size_t n = 0;
                _dupenv_s(&v, &n, "DX12_VERIFY_STAGESKIP");
                const bool on = (v && v[0] == '1'); free(v); return on; }();
            m_verifyStageSkip = s_verify;

            uint32_t maxId = 0;
            auto note = [&](const std::vector<Entity>& v) {
                for (Entity e : v) maxId = (std::max)(maxId, static_cast<uint32_t>(e));
            };
            note(m_sceneDirtyXform); note(m_sceneDirtyOther);
            note(m_sceneAdded);      note(m_sceneMatDirty);
            if (maxId >= m_entityDirtyStamp.size())
                m_entityDirtyStamp.resize(maxId + 1, 0u);
            auto stamp = [&](const std::vector<Entity>& v) {
                for (Entity e : v)
                    m_entityDirtyStamp[static_cast<uint32_t>(e)] = m_instFrameStamp;
            };
            stamp(m_sceneDirtyXform); stamp(m_sceneDirtyOther);
            stamp(m_sceneAdded);      stamp(m_sceneMatDirty);
        }
        m_instanceStagingUsed = 0;
        if (!parallelEmit || !reuseEmitMemory) InstanceRemapWrite().clear();
        InstanceRemapWrite().reserve(candidates.size());
    }
    else
    {
        InstanceRemapWrite().clear();     // SlotForDrawLocalInstance -> identity
    }

    // SameEmitBatch is one shared function (RendererSceneShared.h), so pass A of
    // ResolveBatchPlan and the nest's boundary walk cannot drift.
    // m_planScratch is a member, not thread_local: it crosses the plan -> nest seam,
    // whose far side may run on the lane.
    auto& planScratch = m_planScratch;
    const uint32_t planCount = static_cast<uint32_t>((std::min)(candidates.size(), size_t(m_maxInstances)));
    if (parallelPlan) planScratch.resize(planCount);
    // Bind the buffer explicitly; no worker resolves planScratch/m_sortIdx.
    CandidateEmitPlan* const emitPlan = parallelPlan ? planScratch.data() : nullptr;
    const uint32_t* const sortedCandidates = indices.data();
    const uint32_t planLanes = (std::min)(4u, TaskSystem::Get().GetWorkerCount());
    // First prefix is deliberately bounded so the owner can start early;
    // the remaining work is balanced over the available coarse lanes.
    const uint32_t prefixBudget = widePrefix
        ? (std::min)(16u, TaskSystem::Get().GetWorkerCount()) : planLanes;
    const uint32_t prefixLanes = (std::min)(prefixBudget, (std::max)(1u, planCount / 1024u));
    std::array<uint32_t, 17> planBounds{};
    const uint32_t activePlanLanes = prefixPlan ? prefixLanes : planLanes;
    for (uint32_t lane = 0; lane <= activePlanLanes; ++lane) {
        planBounds[lane] = prefixPlan && activePlanLanes > 1
            ? (lane == 0 ? 0u : 1024u + static_cast<uint32_t>(uint64_t(planCount - 1024u) * (lane - 1) / (activePlanLanes - 1)))
            : static_cast<uint32_t>(uint64_t(planCount) * lane / (std::max)(1u, activePlanLanes));
    }
    auto preparePlanLane = [this, &candidates, sortedCandidates, emitPlan,
                            retainInst, prefixPlan, planBounds](uint32_t lane) {
        ZoneScopedN("BS.EmitPlan.Chunk");
        const uint32_t begin = planBounds[lane], end = planBounds[lane + 1];
        ZoneValue(end - begin);
        for (uint32_t k = begin; k < end; ++k) {
            const auto& c = candidates[sortedCandidates[k]];
            CandidateEmitPlan plan;
            plan.staticEligible = retainInst && IsStaticLaneInstance(c);
            // Prefix consumers allocate/grow this table concurrently. Only the
            // full-barrier path may read it from a worker.
            if (!prefixPlan) {
                const uint32_t entity = static_cast<uint32_t>(c.entity);
                if (plan.staticEligible && entity < m_instSlotOfEntity.size())
                    plan.staticSlot = m_instSlotOfEntity[entity];
            }
            plan.hasDissolve = !m_dissolveParams.empty() && m_dissolveParams.find(c.entity) != m_dissolveParams.end();
            plan.startsBatch = k == 0 || !SameEmitBatch(candidates[sortedCandidates[k - 1]], c);
            emitPlan[k] = plan;
        }
    };
    // Instance reset is complete. Candidates, tracked scene keys and dissolve
    // parameters stay immutable until all jobs drain; slot lookup is read only on
    // the full-barrier path (prefix jobs never touch the mutable table). ctx.planScope
    // drains before the borrowed inputs on exceptions too (the throw guard).
    // Arm the plan-prefix group here, not at the seam: EnsurePlanPrefix is driven by
    // ResolveBatchPlan's pass A below as well as by the nest, so every field it reads
    // must be in the context before that first call. Armed later they would hold the
    // previous frame's values (on frame 0: prefixPlan false, the waiter a no-op, and
    // pass A walking rows the plan lanes are still writing). One block ending in the
    // stamp, so "which fields the waiter needs" and "written for this frame" are the
    // same question.
    ctx.prefixPlan      = prefixPlan;
    ctx.planBounds      = planBounds;
    ctx.activePlanLanes = activePlanLanes;
    ctx.planHandles = {};
    ctx.readyPlanEnd = 0;  ctx.nextPlanLane = 0;
    if (!ctxStale) ctx.prefixArmedEpoch = m_materialFrameEpoch;
    if (overlapPlan) {
        ZoneScopedN("BS.EmitPlan.Publish");
        ctx.planScope.emplace(activePlanLanes);
        // Same crossing as earlyMaterialScope above: EmitBatches' Join (and the seam
        // guard behind it) may run on the lane.
        if (m_emitDeferred) ctx.planScope->AllowOwnerHandoff();
        for (uint32_t lane = 0; lane < activePlanLanes; ++lane)
            ctx.planHandles[lane] = ctx.planScope->Submit([preparePlanLane, lane] { preparePlanLane(lane); });
    }


    // Latch GPU compaction for this frame (see Renderer.h) before the arg writes, the
    // per-command meta fill, the per-view CB write (UploadFrameData, after
    // BuildRenderScene) and the Render() dispatch, so all four agree. Latched, the
    // indirect args carry full instanceCounts and the cull CS does the
    // wantsMain/viewMask zero-out; otherwise the nest (BuildScene_EmitBatches) zeroes
    // them on the CPU.
    // GpuCompactionPreconditions is shared with the shadow-args latch (armed earlier,
    // in BuildRenderScene), which evaluates the same frame-stable inputs.
    // A lane plan was latched by PrepareLanePlan on main, before UploadFrameData.
    if (!planOnLane)
    {
        SceneEmitWrite().gpuCompactionFrameActive = GpuCompactionPreconditions(frameSlot);
        // Coherence clamp: the shadow-args latch implies these preconditions. If they
        // diverge, drop the shadow frame - classify already ran emit-once, which degrades
        // safely (the ring path draws a caster superset; the main path handles
        // off-screen commands itself).
        SceneEmitWrite().shadowArgsFrameActive = SceneEmitWrite().shadowArgsFrameActive && SceneEmitWrite().gpuCompactionFrameActive;
        // The meshlet path's frame latch, beside the GPU-compaction latch.
        SceneEmitWrite().meshletFrameActive = MeshletPathPreconditions(SceneEmitWrite().meshletReason);
        SceneEmitWrite().meshletCullMask    = m_meshletCullMask;
        SceneEmitWrite().meshletLodTau      = m_meshletLodTau * m_meshletLodScale;
        // The CSM meshlet path rides the meshlet latch and the shadow-args latch.
        SceneEmitWrite().shadowMeshletFrameActive =
            SceneEmitWrite().meshletFrameActive && SceneEmitWrite().shadowArgsFrameActive &&
            m_shadowPass && m_shadowPass->MeshletPsoReady();
        SceneEmitWrite().meshletShadowLodTau = m_meshletShadowLodTau * m_meshletLodScale;
    }

    // Per-frame dynamic segment cursor, after the static segment (skinned /
    // billboard / beam / afterimage / dissolving / untracked, rewritten wholesale
    // each frame).
    uint32_t dynCursor = m_staticInstanceCap;
    bool     dynOverflowWarned = false;
    auto* remapMapped = retainInst
        ? static_cast<uint32_t*>(m_instanceRemapMapped[frameSlot]) : nullptr;

    // Retained (persistent) material slots. Without retention the map is cleared
    // every frame, so slots compact to 0..N and every visible material rewrites its
    // 816 B. With it the map persists: a key's slot content is immutable (any
    // appearance change -> a different key), so each slot is written into a given
    // frameSlot buffer only once (PersistentMatSlot::frameMask). Orphan keys are
    // reclaimed by SweepPersistentMatSlots at end of frame; a full reset
    // (!m_retainMaterialSlots or world reload) rebuilds the map and free list.
    // The persistent custom store needs retained slots: per-frame compaction
    // reassigns slot indices and would rewrite live store content inside the
    // frames-in-flight window.
    const bool persistentCustom = m_persistentCustomMatStore &&
        m_retainMaterialSlots && m_customMatCbvStore.IsValid();

    // Park slots (rather than renumber from 0) whenever GPU-side per-slot state
    // outlives the frame that wrote it: the custom CBV/SRV store, and the shared
    // DEFAULT material buffer, whose slot content frames in flight still read when a
    // world reload resets the map mid-frame.
    const bool parkSlotsOnReset = persistentCustom || MaterialGpuBufferActive();

    if (!m_retainMaterialSlots || m_persistentMatSlotsReset)
    {
        if (parkSlotsOnReset)
        {
            // World reload happens mid-frame: in-flight frames may still draw
            // old-world packets whose custom CBV/SRV store content lives at
            // these slots. Park EVERY slot (live map + free list) through the
            // deferred-reuse queue instead of resetting the high-water, so no
            // slot is rewritten before its retire lag expires.
            for (const auto& kv : m_persistentMatSlots)
                m_matSlotsPendingReuse.emplace_back(kv.second.slot,
                                                    m_materialFrameEpoch);
            for (uint32_t s : m_freeMatSlots)
                m_matSlotsPendingReuse.emplace_back(s, m_materialFrameEpoch);
            m_freeMatSlots.clear();
            m_persistentMatSlots.clear();
        }
        else
        {
            m_persistentMatSlots.clear();
            m_freeMatSlots.clear();
            // 1, not 0: slot 0 stays the reserved neutral no-material record
            // across resets (see m_persistentMatSlotNext).
            m_persistentMatSlotNext = 1;
            // This reset re-zeroes the high-water, so parked slots above it must not
            // resurrect into the free list later.
            m_matSlotsPendingReuse.clear();
        }
        m_persistentMatSlotsReset = false;
    }
    m_persistentMatSlots.reserve(std::min<size_t>(candidates.size(), m_maxMaterials));

    // Per-material CBV VA + tex-table. Ring mode clears them each frame so unused
    // materials read 0; persistent mode keeps them as stable per-slot values,
    // (re)set at slot assignment and at pack time, never zero-filled wholesale.
    if (m_matCustomCbvVA.size() < m_maxMaterials)
        m_matCustomCbvVA.resize(m_maxMaterials, 0ULL);
    if (m_matCustomTexHandle.size() < m_maxMaterials)
        m_matCustomTexHandle.resize(m_maxMaterials, 0ULL);
    if (!persistentCustom)
    {
        std::fill(m_matCustomCbvVA.begin(), m_matCustomCbvVA.end(), 0ULL);
        std::fill(m_matCustomTexHandle.begin(), m_matCustomTexHandle.end(), 0ULL);
    }
    // Per-slot custom store bookkeeping (persists across frames; inert in ring mode).
    if (m_matSlotCustomPacked.size() < m_maxMaterials)
        m_matSlotCustomPacked.resize(m_maxMaterials, 0u);
    if (m_matSlotCustomOversize.size() < m_maxMaterials)
        m_matSlotCustomOversize.resize(m_maxMaterials, 0u);
    if (m_matSlotSrvBlock.size() < m_maxMaterials)
        m_matSlotSrvBlock.resize(m_maxMaterials, ~0u);
    if (!persistentCustom)
    {
        // Ring mode zero-fills the VA arrays above; a packed bit left set from a
        // prior persistent-mode frame would suppress the repack after a flag
        // toggle back ON, leaving those slots with stable ZERO VAs forever.
        std::fill(m_matSlotCustomPacked.begin(), m_matSlotCustomPacked.end(),
                  static_cast<uint8_t>(0));
        std::fill(m_matSlotCustomOversize.begin(), m_matSlotCustomOversize.end(),
                  static_cast<uint8_t>(0));
    }

    if (parallelPlan && !overlapPlan) {
        ZoneScopedN("BS.EmitPlan.Prepare");
        TaskSystem::Get().ParallelFor(0, planLanes, preparePlanLane);
    }
    uint32_t planVerifiedBatches = 0, planCachedSlots = 0;

    const int n = static_cast<int>(candidates.size());
    if (ctx.planScope && !prefixPlan) {
        ZoneScopedN("BS.EmitPlan.Join");
        ctx.planScope->Join();
    }
#if WITH_HARNESS
    if (verifyOverlapPlan && !prefixPlan) {
        // Independent serial recomputation before any slot allocation mutates
        // the lookup. Check every row, including skipped/truncated batches.
        for (uint32_t k = 0; parallelPlan && k < planCount; ++k) {
            const auto& c = candidates[sortedCandidates[k]];
            const bool eligible = retainInst && IsStaticLaneInstance(c);
            const uint32_t entity = static_cast<uint32_t>(c.entity);
            const uint32_t slot = eligible && entity < m_instSlotOfEntity.size()
                ? m_instSlotOfEntity[entity] : ~0u;
            const bool dissolve = !m_dissolveParams.empty() && m_dissolveParams.find(c.entity) != m_dissolveParams.end();
            const bool starts = k == 0 || !SameEmitBatch(candidates[sortedCandidates[k - 1]], c);
            const auto& row = emitPlan[k];
            ENGINE_CHECK(row.staticEligible == eligible && row.staticSlot == slot &&
                         row.hasDissolve == dissolve && row.startsBatch == starts,
                         "overlapped emit plan oracle mismatch");
        }
        LOG_INFO("[PLAN_OVERLAP_ORACLE] count=%u queued=%u overlap=%u match=1",
                 parallelPlan ? planCount : 0u, ctx.planScope ? planLanes : 0u, overlapPlan ? 1u : 0u);
    }
#endif

    // ---- ResolveBatchPlan ----
    // The TDR guard's bounds (kMaxDrawVerts / kMaxDrawInstances) are shared in
    // RendererSceneShared.h: the plan predicts the skip, the nest applies it.
    // Two passes over the sorted array:
    //   pass A  walks the batch boundaries and records begin/end/instBegin. It
    //           touches no allocator (AcquirePersistentMatSlot, AcquirePage,
    //           MapEntity), and its rows are then validated (contiguous from 0,
    //           instBegin tracking begin, inside the truncation bound).
    //   pass B  performs the two stateful acquisitions, once per row.
    // Do not merge them: with one pass the first allocator call precedes any proof
    // that the plan covers every batch, and the only recovery left is a per-batch
    // fallback that re-calls AcquirePersistentMatSlot for a key the plan already
    // stamped - it reports firstSight=false / needsBaseWrite=false, which silences
    // StageMaterialSlot and PackCustomMaterialBindings and changes the material
    // bytes. A plan that cannot be trusted is refused for the whole frame before any
    // allocator is touched (planRefused -> planActive false), and the frame runs the
    // in-nest allocators end to end. -batchplanrefuse forces that path
    // (tests/scenarios/surfacepages_refuse).
    // Both passes read only DrawCandidate fields and emitPlan rows - no World, no
    // pool pointer, no mapped upload memory.
    // `idx` mirrors the nest's `instIdx` (same m_maxInstances truncation) and is
    // recorded per row as instBegin, so the nest can check its own counter against it.
    uint32_t acquiresBeforeNest = m_surfaceCache.GetAcquireCallCount();
    uint32_t mapsBeforeNest     = m_surfaceCache.GetMapEntityCallCount();
    uint32_t planGatedBatches   = 0, planMappedEntities = 0;
    uint32_t matSlotStamped     = 0;
    const uint32_t planEpochBefore = m_surfaceCache.GetPublishEpoch();
    double planMs = 0.0;
    bool   planRefused = false;
#if WITH_HARNESS
    // -batchplanrefuse=<N> refuses plan frame N (the mid-run transition); the bare
    // flag refuses every frame, the arm a golden is captured under: a run that never
    // takes the hoist must produce the same picture as the hoist being off.
    static const int  s_planRefuseFrame = []{
        const auto& cl = Sys::CommandLine::Get();
        return cl.Has("-batchplanrefuse") ? cl.GetInt("-batchplanrefuse", 0) : -1;
    }();
    // -batchplandesync=<N>: corrupts a published row after pass B on plan frame N, so
    // the nest's cross-walk tripwire (rowValid) is under test rather than pass A's
    // validator.
    static const int  s_planDesyncFrame = []{
        const auto& cl = Sys::CommandLine::Get();
        return cl.Has("-batchplandesync") ? cl.GetInt("-batchplandesync", 0) : -1;
    }();
    // -matslotstampoff: the plan keeps both allocators but does not stamp
    // m_entityMatSlot, so the emit nest stays the producer while the hoist runs. The
    // escape hatch and positive control for the plan-side stamp: with -brslane it
    // reproduces the defect that stamp prevents (DDGISceneAS::Gather runs before the
    // nest, so every Toon/Unlit RT proxy loses its knobs), which
    // tests/scenarios/ddgi_proxy_slot_stampoff pins.
    static const bool s_matSlotStampOff = Sys::CommandLine::Get().Has("-matslotstampoff");
    static uint64_t   s_planFrame = 0;
    const uint64_t    planSeqFrame = ++s_planFrame;
#else
    constexpr int     s_planRefuseFrame = -1;
    constexpr int     s_planDesyncFrame = -1;
    constexpr bool    s_matSlotStampOff = false;
    constexpr uint64_t planSeqFrame = 0;
#endif
    // Who owns m_entityMatSlot this frame. A refused plan leaves m_batchPlan empty,
    // so pass B stamps nothing and planStampedMatSlots is false: the nest is the
    // producer, as without the hoist.
    const bool stampMatSlots = !s_matSlotStampOff;
    if (earlyPlan)
    {
        ZoneScopedN("BS.ResolveSurfacePages");
        const auto planT0 = std::chrono::steady_clock::now();
        m_batchPlan.clear();
        if (!planOnLane) ++m_batchPlanGen;   // lane plan: reserved by PrepareLanePlan

        // Pass A: boundaries only. No allocator is touched here.
        {
            uint32_t idx = 0;
            for (int a = 0; a < n && idx < m_maxInstances; )
            {
                EnsurePlanPrefix(static_cast<uint32_t>(a));
                const DrawCandidate& first = candidates[indices[a]];
                int b = a + 1;
                if (parallelPlan) {
                    while (b < static_cast<int>(planCount)) {
                        EnsurePlanPrefix(static_cast<uint32_t>(b));
                        if (emitPlan[b].startsBatch) break;
                        ++b;
                    }
                } else {
                    while (b < n && (idx + static_cast<uint32_t>(b - a)) < m_maxInstances &&
                           SameEmitBatch(first, candidates[indices[b]]))
                        ++b;
                }
                BatchPlanRow row;
                row.begin     = static_cast<uint32_t>(a);
                row.end       = static_cast<uint32_t>(b);
                row.instBegin = idx;
                m_batchPlan.push_back(row);
                idx += static_cast<uint32_t>(b - a);
                a = b;
            }
        }

        // Pass A's verdict: can the nest consume this plan? Each condition is what the
        // nest's own walk would produce; a plan that meets them all makes the nest's
        // `rowValid` true for every batch by construction, because under planActive the
        // nest takes its `j` from row.end and advances i/instIdx by the row's span.
        const char* refuseReason = nullptr;
        for (size_t r = 0; r < m_batchPlan.size() && !refuseReason; ++r)
        {
            const BatchPlanRow& row = m_batchPlan[r];
            const uint32_t expectBegin = (r == 0) ? 0u : m_batchPlan[r - 1].end;
            if (row.begin != expectBegin)            refuseReason = "rows are not contiguous";
            else if (row.end <= row.begin)           refuseReason = "row spans no candidate";
            else if (row.end > static_cast<uint32_t>(n)) refuseReason = "row runs past the candidate array";
            else if (row.instBegin != row.begin)     refuseReason = "instance ordinal left the candidate ordinal";
            else if (row.instBegin >= m_maxInstances) refuseReason = "row starts past the instance cap";
        }
        if (s_planRefuseFrame == 0 ||
            (s_planRefuseFrame > 0 && planSeqFrame == static_cast<uint64_t>(s_planRefuseFrame)))
        {
            planRefused = true;
            // Log once in the every-frame arm: a line per frame would bury the rest of the
            // golden run's log.
            static bool s_refuseLogged = false;
            const bool  logThis = (s_planRefuseFrame > 0) || !s_refuseLogged;
            s_refuseLogged = true;
            // LOG_INFO, not LOG_WARNING: this refusal is what the command line asked for,
            // and a warning would count against every pack's engineWarnings budget. A
            // refusal the validator decides on stays an ENGINE_CHECK error below.
            if (logThis)
                LOG_INFO("[SURFACEPAGES] plan refused for frame %llu "
                         "(harness -batchplanrefuse); this frame runs the legacy "
                         "in-nest allocators end to end",
                         static_cast<unsigned long long>(planSeqFrame));
        }
        else if (refuseReason)
        {
            planRefused = true;
            ENGINE_CHECK_ONCE(false,
                "[SURFACEPAGES] plan refused for frame %llu: %s (rows=%zu, "
                "candidates=%d) - falling back to the legacy nest for the WHOLE "
                "frame, before any allocator was touched",
                static_cast<unsigned long long>(planSeqFrame), refuseReason,
                m_batchPlan.size(), n);
        }
        if (planRefused)
        {
            // Nothing acquired anything yet, so dropping the rows is the whole rollback.
            // planActive below turns the nest back to its own allocators in one place,
            // material half and surface-cache half together.
            m_batchPlan.clear();
        }

        // Pass B: the two stateful acquisitions, once per row.
        MaterialBindingKey planLastKey{};
        uint32_t planLastSlot = 0;
        bool     planLastValid = false;
        for (BatchPlanRow& row : m_batchPlan)
        {
            const int a = static_cast<int>(row.begin);
            const int b = static_cast<int>(row.end);
            const DrawCandidate& first = candidates[indices[a]];
            const uint32_t batchCount = static_cast<uint32_t>(b - a);

            // Half one of the page key: the persistent material slot.
            const MaterialBindingKey bindingKey = BindingKeyOf(first);
            bool fs = false, nbw = false, sv = true;
            uint32_t matIdx;
            if (planLastValid && bindingKey == planLastKey)
            {
                matIdx = planLastSlot;
            }
            else
            {
                matIdx = AcquirePersistentMatSlot(bindingKey, frameSlot, fs, nbw, sv);
                planLastKey = bindingKey; planLastSlot = matIdx; planLastValid = sv;
            }
            row.matIdx         = matIdx;
            row.firstSight     = fs  ? 1u : 0u;
            row.needsBaseWrite = nbw ? 1u : 0u;
            row.slotValid      = sv  ? 1u : 0u;

            // The dissolve tally, per instance and not part of the key.
            bool dissolve = false;
            for (int k = a; k < b; ++k)
            {
                const bool hit = parallelPlan ? emitPlan[k].hasDissolve
                    : (!m_dissolveParams.empty() &&
                       m_dissolveParams.find(candidates[indices[k]].entity) != m_dissolveParams.end());
                if (hit) { dissolve = true; break; }
            }
            row.hasDissolve = dissolve ? 1u : 0u;

            // The TDR-guard prediction.
            const uint32_t vtxCount = first.indexCount;
            row.tdrSkip = (vtxCount == 0 || vtxCount > kMaxDrawVerts ||
                           batchCount == 0 || batchCount > kMaxDrawInstances) ? 1u : 0u;

            // Entity -> material slot, stamped here on main. DDGISceneAS::Gather consumes
            // this map later in the same BeginFrame, and under -brslane the nest has not run
            // yet, so a nest-only stamp would leave every Toon and Unlit RT/DDGI proxy with
            // "no slot" (shader defaults: no toon knobs, no exposure protection). The nest's
            // stamp is gated on !planStampedMatSlots, so there is one writer per frame (see
            // StampEntityMaterialSlot).
            // tdrSkip is not honoured here, for parity: the nest's TDR guard drops the packet
            // after every instance was allocated and stamped, so the nest stamps TDR-skipped
            // batches too. This also keeps `stamped` and `nestInst` equal on a frame that
            // hits the guard (ddgi_proxy_slot_pages/_lane compare them). The surface-cache
            // page gate below keeps its `!row.tdrSkip`: the nest takes that one after the
            // guard. Rows are walked in order, so duplicates of one entity (base +
            // outline/picking copy) resolve last-wins, as in the nest.
            if (stampMatSlots)
            {
                for (int k = a; k < b; ++k)
                    StampEntityMaterialSlot(candidates[indices[k]].entity,
                                            row.matIdx, row.slotValid != 0);
                matSlotStamped += batchCount;
            }

            // Half two: the surface-cache page. The gate is the nest's surface-cache gate in
            // BuildScene_EmitBatches (its rationale is there); keep the two identical.
            if (!row.tdrSkip &&
                m_surfaceCache.IsEnabled() && (first.matBits & MB_CUSTOM_SHADER) &&
                first.customPSID != 0 &&
                !(first.matBits & MB_ALPHA_TEST) &&
                !(first.matBits & MB_FRAME_PARAMS) &&
                !(first.matBits & MB_AUTHORED_RT) &&
                first.filter == DrawFilter::Opaque && first.renderInMainPass && !dissolve &&
                (first.viewMask & ViewBit::MainCamera) != 0 &&
                (first.matBits & MB_IDENTITY_UV))
            {
                row.pageGate = 1;
                ++planGatedBatches;
                row.contentHash = first.matHash ^
                    (static_cast<uint64_t>(vtxCount) * 0x9E3779B97F4A7C15ull);
                const uint32_t page = m_surfaceCache.AcquirePage(
                    m_gfx, first.meshDescSlot, matIdx, row.contentHash);
                row.page = page;
                if (page != MaterialSurfaceCache::kInvalidPage)
                {
                    for (int k = a; k < b; ++k)
                        m_surfaceCache.MapEntity(
                            static_cast<uint32_t>(candidates[indices[k]].entity), page);
                    planMappedEntities += batchCount;
                }
            }
        }
#if WITH_HARNESS
        // The nest-side cross-walk tripwire's positive control: corrupt a
        // PUBLISHED row (pass A already blessed it, pass B already acquired
        // against it) so `rowValid` is what has to notice.
        if (s_planDesyncFrame >= 0 && planSeqFrame == static_cast<uint64_t>(s_planDesyncFrame) &&
            m_batchPlan.size() > 1)
        {
            BatchPlanRow& victim = m_batchPlan[m_batchPlan.size() / 2];
            LOG_INFO("[SURFACEPAGES] harness -batchplandesync: row %zu of %zu "
                     "shifted from begin=%u instBegin=%u - the nest's cross-walk "
                     "check must refuse the rest of this frame",
                     m_batchPlan.size() / 2, m_batchPlan.size(),
                     victim.begin, victim.instBegin);
            victim.begin += 1;
        }
#endif
        planMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - planT0).count();
        acquiresBeforeNest = m_surfaceCache.GetAcquireCallCount();
        mapsBeforeNest     = m_surfaceCache.GetMapEntityCallCount();
    }
    // One predicate for the rest of the function: is the nest consuming rows? Both
    // halves of the nest (material slot, surface-cache page) key off this and nothing
    // else, so "the plan ran" and "the plan is being used" cannot disagree.
    const bool planActive = earlyPlan && !planRefused;
#if WITH_HARNESS
    verifySurfacePages = verifySurfacePages && planActive;
#endif
    const uint32_t planEpochAfter = m_surfaceCache.GetPublishEpoch();

    // -brslane + a refused plan: this frame's emit half stays on main.
    // FramePipeline::Configure's -brslane ladder refuses -surfacepagesearlyoff at
    // boot, because without the hoist the nest owns AcquirePersistentMatSlot,
    // AcquirePage and MapEntity while main reads the surface cache's publish epoch
    // (DDGISceneAS::Gather) after the hand-off. ResolveBatchPlan's per-frame refusal
    // reaches the same state for one frame, with the nest on the lane, so the
    // ladder's answer is taken per frame: m_emitDeferred is cleared here, before
    // anything downstream reads it, so
    //   * BuildScene_SortAndEmit runs the emit half on main, in the default path's
    //     position (right after PlanEmit);
    //   * BeginFrame takes the default publish, so m_laneEmitOwed is never set and
    //     the lane runs RenderAsync alone;
    //   * both allocators and the publish epoch are main's by construction.
    // The fallback frame is the default path byte for byte, so it needs no oracle of
    // its own. framePreparation is revisited with it: it was latched true off
    // m_emitDeferred on the promise that the lane would hold a FrameTaskContext, and
    // main holds none, so the material tail takes the immediate CpuTaskScope path
    // (the same writes, order and bytes as -framepreparelegacy).
#if WITH_HARNESS
    // -brslanefallbackoff: the positive control and escape hatch for the fallback.
    // With the fallback the state it prevents is unreachable, so the
    // ENGINE_CHECK_ONCE at the head of BuildScene_EmitBatches could never be seen to
    // fire. This flag keeps the frame deferred: the in-nest allocators run on the
    // lane and the check fires (tests/scenarios/brslane_planrefuse_nofallback).
    static const bool s_laneFallbackOff =
        Sys::CommandLine::Get().Has("-brslanefallbackoff");
#else
    constexpr bool s_laneFallbackOff = false;
#endif
    // A plan that runs on the lane needs no fallback: the refused plan's in-nest
    // allocators run on the lane, and nothing on main reads them after the hand-off
    // (the DDGI binding is in BuildOrRefit, after the nest).
    const bool laneFallback = m_emitDeferred && !planActive && !s_laneFallbackOff && !planOnLane;
    if (s_laneFallbackOff && m_emitDeferred && !planActive && !planOnLane)
    {
        // Once: the arm has to DECLARE itself, or a pack asserting the check
        // fires cannot tell "the fallback was suppressed" from "the fallback
        // was never reached".
        static bool s_offLogged = false;
        if (!s_offLogged)
        {
            s_offLogged = true;
            LOG_INFO("[BRSLANE] frame=%llu fallback=off reason=planRefused "
                     "(harness -brslanefallbackoff); the refused plan's legacy "
                     "in-nest allocators are about to run on the lane",
                     static_cast<unsigned long long>(FrameViewEmit().currentFrame));
        }
    }
    if (laneFallback)
    {
        m_emitDeferred = false;
        ++m_emitLaneFallbackFrames;
        // LOG_INFO and rate limited, for the refusal line's reasons (the bare
        // -batchplanrefuse arm refuses every frame): the first four, then every 60th,
        // with the running count in the line.
        if (m_emitLaneFallbackFrames <= 4 || m_emitLaneFallbackFrames % 60 == 0)
            LOG_INFO("[BRSLANE] frame=%llu fallback=main reason=planRefused count=%llu",
                     static_cast<unsigned long long>(FrameViewEmit().currentFrame),
                     static_cast<unsigned long long>(m_emitLaneFallbackFrames));
    }

    // ---- The seam: everything EmitBatches may read ----
    // One block, so what crosses is a list. Every field is assigned here on every
    // path, so no stale value survives from last frame. The three CpuTaskScopes and
    // the plan-prefix cursor are not copied: they are written through ctx where they
    // are created, because they have identity (a scope that is joined, a cursor two
    // loops share), not a value.
    ctx.earlyPlan            = earlyPlan;
    ctx.planActive           = planActive;
    ctx.planStampedMatSlots  = planActive && stampMatSlots;
    ctx.matSlotStamped       = matSlotStamped;
#if WITH_HARNESS
    ctx.verifySurfacePages   = verifySurfacePages;
#else
    ctx.verifySurfacePages   = false;
#endif
    ctx.parallelPlan         = parallelPlan;
    ctx.verifyPlan           = verifyPlan;
    ctx.overlapPlan          = overlapPlan;
    // ctx.prefixPlan is not here: EnsurePlanPrefix reads it in pass A, on this side
    // of the seam, so it is armed with the plan-prefix group above. A second
    // assignment here would hide that ordering requirement.
    ctx.verifyPrefixPlan     = verifyPrefixPlan;
    ctx.widePrefix           = widePrefix;
    ctx.deferMaterial        = deferMaterial;
    ctx.verifyMaterialWrites = verifyMaterialWrites;
    // ...and not `framePreparation` itself: on a lane fallback main runs the nest and
    // holds no FrameTaskContext.
    ctx.framePreparation     = framePreparation && !laneFallback;
    ctx.laneFallback         = laneFallback;
    ctx.earlyMaterial        = earlyMaterial;
    ctx.overlapMaterialTail  = overlapMaterialTail;
    ctx.parallelEmit         = parallelEmit;
    ctx.reuseEmitMemory      = reuseEmitMemory;
    ctx.retainInst           = retainInst;
    ctx.persistentCustom     = persistentCustom;
    // The lease: all four derive from the one upload slot latched at the head of
    // BeginFrame, so they can never be half a frame apart.
    ctx.frameSlot            = frameSlot;
    ctx.instances            = instances;
    ctx.matBuf               = matBuf;
    ctx.remapMapped          = remapMapped;
    ctx.sortedCandidates     = sortedCandidates;
    ctx.emitPlan             = emitPlan;
    ctx.planCount            = planCount;
    // Accumulators: initialised here, accumulated by the nest, read by the tail.
    ctx.dynCursor                 = dynCursor;
    ctx.dynOverflowWarned         = dynOverflowWarned;
    ctx.materialSlotOverflowWarned = materialSlotOverflowWarned;
    ctx.materialBatchCount        = materialBatchCount;
    ctx.secondaryPackets          = secondaryPackets;
    ctx.argsWritten               = argsWritten;
    ctx.secondaryArgsSkipped      = secondaryArgsSkipped;
    ctx.firstSkippedArg           = firstSkippedArg;
    ctx.snapDrivenWrites          = snapDrivenWrites;
    ctx.snapVerifiedWrites        = snapVerifiedWrites;
    ctx.reusedMaterialBindings    = reusedMaterialBindings;
    ctx.planVerifiedBatches       = planVerifiedBatches;
    ctx.planCachedSlots           = planCachedSlots;
    // ResolveBatchPlan's own output, consumed by the nest's side-effect-free
    // assertion and by the [SURFACEPAGES]/[SURFACEPAGES_ORACLE] lines.
    ctx.acquiresBeforeNest   = acquiresBeforeNest;
    ctx.mapsBeforeNest       = mapsBeforeNest;
    ctx.planGatedBatches     = planGatedBatches;
    ctx.planMappedEntities   = planMappedEntities;
    ctx.planEpochBefore      = planEpochBefore;
    ctx.planEpochAfter       = planEpochAfter;
    ctx.planMs               = planMs;
    // The plan-prefix group was armed before pass A; its fields must still be what
    // this block would write. This catches a later edit that moves the arming point
    // or reassigns one of the three after it. No positive control: forcing it would
    // change a field the nest reads, and -emitctxstale must stay a byte-for-byte
    // no-op; the two arming stamps are the exercised part.
    ENGINE_CHECK_ONCE(ctx.prefixPlan == prefixPlan &&
                      ctx.activePlanLanes == activePlanLanes &&
                      ctx.planBounds == planBounds,
        "[EMITCTX] the plan-prefix group the waiter consumed is not the one the "
        "seam would publish (prefixPlan %u/%u, lanes %u/%u) - something between "
        "the arming point and here changed it",
        unsigned(ctx.prefixPlan), unsigned(prefixPlan),
        ctx.activePlanLanes, activePlanLanes);
    // Last statement of the seam. EmitBatches compares it, so "PlanEmit ran for this
    // frame" and "PlanEmit ran for the last one" are told apart once the two halves
    // run on different threads.
    if (!ctxStale) ctx.armedEpoch = m_materialFrameEpoch;

    EmitZoneMark(8);   // preamble: ResolveBatchPlan + everything else between sort and nest
}
