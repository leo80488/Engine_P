// ChainPhysicsPreviewCompare.cpp - the Chain Physics Preview panel's COMPARE
// tools: the A/B ghost, the per-group solo and the poke. Member functions of
// ChainPhysicsPreviewWindow, split from ChainPhysicsPreviewWindow.cpp for size;
// the header's side-effect discipline applies: Draw* functions touch panel
// state only, and everything that reads or writes the World or the solver runs
// in ProcessPendingActions (the game half's serial window).
//   * A/B: B is a bare ChainPhysicsComponent entity in the preview lane's set,
//     fed A's animated input pose every frame (ChainPhysicsSystem::
//     SetPreviewPoseFeed). No second character is animated, pinned, skinned or
//     drawn; B costs one extra chain solve. The ghost is B's FillDebugSnapshot
//     drawn through the overlay, mapped with A's skeleton -> world transform. B
//     has its own staged copy (m_abStaged) and ONE writer (SyncAbToLive,
//     field-wise). Swap / Copy / Promote re-sync through the same writers and
//     restart both chains from the same pose.
//   * Solo: ChainPhysicsSystem::SetSoloGroup - the other groups FROZEN AT REST.
//   * Poke: the panel's own camera-basis ray (not Ray::FromViewport) through the
//     pressed pixel, mapped into the stage's SKELETON space (where the solver
//     simulates), the nearest particle of the solver snapshot (CPU, no GPU
//     readback) and ChainPhysicsSystem::ApplyExternalImpulse (an impulse with a
//     TTL).
// TODO(ChainPhysics_Preview_Viewport_Plan Q8): the FK <-> sim blend (deferred,
// not implemented).

#include "Editor/ChainPhysicsPreviewWindow.h"
#include "Editor/ImGuiFlow.h"   // flowing rows / wrapped text

#include "ECS/AnimationComponents.h"        // SkeletonComponent / MeshSkinnedComponent
#include "ECS/ECS.h"
#include "ECS/HierarchyComponents.h"        // SceneTransientTag / PreviewStageTag
#include "ECS/HierarchyOps.h"               // Hierarchy::DestroySubtree
#include "Editor/Undo/ComponentBlob.h"      // SerializeComponentBlob (the A/B field diff)
#include "Graphics/AnimProfile.h"           // previewChain* (the A/B budget)
#include "Physics/ChainPhysicsSystem.h"
#include "Renderer/Renderer.h"
#include "Resource/ComponentSerializers.h"  // GetComponentRegistry
#include "Resource/SkeletonAsset.h"
#include "System/Check.h"
#include "System/Log.h"

#include "imgui/imgui.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Editor
{
namespace
{
    using namespace DirectX;

    const std::type_index kChainTi = std::type_index(typeid(ChainPhysicsComponent));

    struct CamBasis { XMVECTOR eye, fwd, up, right; float tanY; bool ok; };

    // ONE ChainPhysicsComponent through the registry's own serializer, on a
    // scratch World (never the main World) - the diff lists exactly the keys an
    // Apply would write.
    std::string BlobOf(const ChainPhysicsComponent& value)
    {
        World scratch;
        const Entity e = scratch.CreateEntity();
        ChainPhysicsComponent c;
        value.CopyAuthoredInto(c);
        scratch.AddComponent<ChainPhysicsComponent>(e, std::move(c));
        return Undo::SerializeComponentBlob(GetComponentRegistry(), scratch, e, kChainTi);
    }

    // "key=value" tokens of a serializer line, in line order.
    std::vector<std::pair<std::string, std::string>> Tokens(const std::string& blob)
    {
        std::vector<std::pair<std::string, std::string>> out;
        std::size_t i = 0;
        while (i < blob.size())
        {
            while (i < blob.size() && std::isspace(static_cast<unsigned char>(blob[i]))) ++i;
            const std::size_t s = i;
            while (i < blob.size() && !std::isspace(static_cast<unsigned char>(blob[i]))) ++i;
            const std::string tok = blob.substr(s, i - s);
            const std::size_t eq = tok.find('=');
            if (eq == std::string::npos || eq == 0) continue;
            out.emplace_back(tok.substr(0, eq), tok.substr(eq + 1));
        }
        return out;
    }

    const char* AbPressName(ChainPhysicsPreviewWindow::AbPress p)
    {
        switch (p)
        {
        case ChainPhysicsPreviewWindow::AbPress::Swap:    return "swap";
        case ChainPhysicsPreviewWindow::AbPress::Copy:    return "copy";
        case ChainPhysicsPreviewWindow::AbPress::Promote: return "promote";
        case ChainPhysicsPreviewWindow::AbPress::Off:     return "off";
        }
        return "?";
    }

    float Dist3(const XMFLOAT3& a, const XMFLOAT3& b)
    {
        const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // Per-bone particle position of a solver snapshot: the WRITER copy of a bone
    // (a bone above a fork is duplicated into every leaf strand - the writer is
    // the one whose direction becomes the bone's rotation), else its first copy.
    void BonePositions(const ChainPhysicsSystem::DebugSnapshot& s,
                       std::unordered_map<std::uint32_t, XMFLOAT3>& out)
    {
        out.clear();
        const std::size_t n = std::min(s.particlePos.size(), s.particleBone.size());
        for (std::size_t i = 0; i < n; ++i)
        {
            const bool writer = i < s.particleWritesBone.size() && s.particleWritesBone[i] != ~0u;
            auto it = out.find(s.particleBone[i]);
            if (it == out.end()) out.emplace(s.particleBone[i], s.particlePos[i]);
            else if (writer)     it->second = s.particlePos[i];
        }
    }

    // The panel's own camera basis: forward, orthonormal up and right exactly
    // as XMMatrixLookToLH builds them (the overlay's ViewProj), so a ray and a
    // projection agree to the last bit of arithmetic that matters.
    CamBasis Basis(const ChainOverlay::Camera& cam)
    {
        CamBasis b{};
        b.eye = XMLoadFloat3(&cam.eye);
        b.fwd = XMVector3Normalize(XMLoadFloat3(&cam.forward));
        XMVECTOR up = XMVector3Normalize(XMLoadFloat3(&cam.up));
        if (XMVector3IsNaN(up) || std::fabs(XMVectorGetX(XMVector3Dot(up, b.fwd))) > 0.999f)
            up = XMVectorSet(0.f, 1.f, 0.f, 0.f);
        b.right = XMVector3Normalize(XMVector3Cross(up, b.fwd));   // LH: up x forward
        b.up    = XMVector3Cross(b.fwd, b.right);
        b.tanY  = std::tan(0.5f * std::max(cam.fovY, 1e-3f));
        b.ok    = !XMVector3IsNaN(b.fwd) && !XMVector3IsNaN(b.right) && cam.valid;
        return b;
    }
}

// ==== Harness setters (-chainpreview_abparam / _ab_press / _solo / _poke) ====
bool ChainPhysicsPreviewWindow::HarnessAddAbParam(const std::string& key, const std::string& value,
                                                  std::uint64_t editorFrame)
{
    if (!ParamKeyValid(key, value)) return false;
    HarnessAbParam hp;
    hp.key   = key;
    hp.value = value;
    hp.at    = editorFrame;
    m_harnessAbParams.push_back(std::move(hp));
    return true;
}

bool ChainPhysicsPreviewWindow::HarnessAddAbPress(AbPress what, std::uint64_t editorFrame)
{
    if (editorFrame == 0) return false;
    HarnessAbPress p;
    p.what = what;
    p.at   = editorFrame;
    m_harnessAbPresses.push_back(p);
    std::stable_sort(m_harnessAbPresses.begin(), m_harnessAbPresses.end(),
                     [](const HarnessAbPress& a, const HarnessAbPress& b) { return a.at < b.at; });
    return true;
}

bool ChainPhysicsPreviewWindow::HarnessAddSolo(const std::string& group, std::uint64_t editorFrame)
{
    if (group.empty()) return false;
    HarnessSolo s;
    s.group = group;
    s.at    = editorFrame;
    m_harnessSolo.push_back(std::move(s));
    std::stable_sort(m_harnessSolo.begin(), m_harnessSolo.end(),
                     [](const HarnessSolo& a, const HarnessSolo& b) { return a.at < b.at; });
    return true;
}

bool ChainPhysicsPreviewWindow::HarnessAddPoke(std::uint64_t editorFrame, const std::string& target,
                                               float strength, float blendSeconds)
{
    if (editorFrame == 0 || target.empty() || !std::isfinite(strength) || strength <= 0.f ||
        strength > 1e5f || !std::isfinite(blendSeconds) || blendSeconds <= 0.f ||
        blendSeconds > ChainPhysicsSystem::kImpulseMaxTtl)
        return false;
    HarnessPoke p;
    p.at       = editorFrame;
    p.target   = target;
    p.strength = strength;
    p.blend    = blendSeconds;
    m_harnessPokes.push_back(std::move(p));
    return true;
}

bool ChainPhysicsPreviewWindow::HarnessSetPokeProbe(std::uint32_t afterFrames, std::uint32_t settleFrames)
{
    if (afterFrames == 0 || settleFrames <= afterFrames || settleFrames > 5000) return false;
    m_pokeProbeAfter  = afterFrames;
    m_pokeProbeSettle = settleFrames;
    return true;
}

// ==== A/B ====
ChainPhysicsPreviewWindow::AbBudget
ChainPhysicsPreviewWindow::SampleAbBudget(const ChainPreviewContext& ctx) const
{
    // The World's pool sums (skin jobs, skinned verts, pose bones, entities: the
    // same census chain_preview_open's budget line uses), plus the renderer's
    // instance rows of the last frame and the preview lane's own counters.
    AbBudget b;
    if (!ctx.renderer || !ctx.world) return b;
    World& world = *ctx.world;
    const Renderer::FrameStats& fs = ctx.renderer->GetFrameStats();
    b.instances = fs.staticInstSlots + fs.dynamicInstances;
    if (auto* kp = world.GetPool<MeshSkinnedComponent>())
    {
        b.skinJobs = static_cast<std::uint32_t>(kp->Size());
        for (const MeshSkinnedComponent& m : kp->Data()) b.verts += m.vertexCount;
    }
    if (auto* sp = world.GetPool<SkeletonComponent>())
        for (const SkeletonComponent& s : sp->Data()) b.poseBones += s.boneCount;
    for (Entity e : world.GetEntities()) b.entities += world.IsAlive(e) ? 1u : 0u;
    const Anim::AnimProfile& ap = Anim::AnimProfile::Get();
    b.previewChainEntities = ap.previewChainEntities;
    b.solveMs = ap.previewChainCpuMs;
    b.particles = (m_dbgValid ? static_cast<std::uint32_t>(m_dbgSnap.particlePos.size()) : 0u) +
                  (m_abDbgValid ? static_cast<std::uint32_t>(m_abDbg.particlePos.size()) : 0u);
    b.valid = true;
    return b;
}

void ChainPhysicsPreviewWindow::CreateAbStage(const ChainPreviewContext& ctx)
{
    World& world = *ctx.world;
    Renderer& renderer = *ctx.renderer;
    ChainPhysicsSystem* cps = renderer.GetChainPhysicsSystem();
    if (!cps) return;
    // The budget's OFF side: the frame before B exists.
    m_abBudgetOff = SampleAbBudget(ctx);

    // B: ONE bare entity - a ChainPhysicsComponent (field-wise from B's staged
    // copy; default-constructed, so BoneRestPose's runtime caches start clean) and
    // the stage tags (kept out of save, the outliner, picking and save-as-prefab).
    // No skeleton, mesh, transform or animation: it is never animated, pinned,
    // skinned or drawn. Not recorded for undo.
    const Entity b = world.CreateEntity();
    world.SetName(b, "__ChainPreviewB");
    ChainPhysicsComponent c{};
    m_abStaged.CopyAuthoredInto(c);
    world.AddComponent<ChainPhysicsComponent>(b, std::move(c));
    world.AddComponent<SceneTransientTag>(b, SceneTransientTag{});
    world.AddComponent<PreviewStageTag>(b, PreviewStageTag{ PreviewStageTag::kChainPreview });
    m_abEnt  = b;
    m_abEntH = world.MakeHandle(b);
    m_abStaged.CopyAuthoredInto(m_abSynced);
    m_abStructural = false;
    // THE PREVIEW LANE'S set (never m_stageSet / m_stagePin: B is solved, not
    // animated or drawn) + the pose feed: B is solved on A's skeleton from A's
    // input pose of each frame, with A's transform / capsules.
    m_previewSet.insert(b);
    cps->SetPreviewPoseFeed(b, m_stageChain);
    if (m_soloGroup >= 0 && !m_harnessSoloOff) cps->SetSoloGroup(b, m_soloGroup);
    ++m_abCreates;
    m_abDbgValid = false;
    m_abDiv = AbDivergence{};
    m_abBudgetOnAt   = m_tick + 30;
    m_abBudgetLogged = false;
    // A and B restart TOGETHER from the same pose: equal parameters then give
    // bit-identical chains (the -chainpreview_ab_same arm's claim).
    ResetSimulation(renderer, world, "ab on");
    LOG_INFO("[CHAINPREVIEW] ab on: entity=%u source=%u creates=%u sameParams=%d frame=%llu",
             static_cast<unsigned>(b), static_cast<unsigned>(m_stageChain), m_abCreates,
             m_abStaged.AuthoredEquals(m_staged) ? 1 : 0, static_cast<unsigned long long>(m_tick));
}

void ChainPhysicsPreviewWindow::DestroyAbStage(Renderer* renderer, World* world, const char* why)
{
    if (m_abEnt == NullEntity) return;
    const Entity b = m_abEnt;
    if (renderer)
        if (ChainPhysicsSystem* cps = renderer->GetChainPhysicsSystem())
            cps->SetPreviewPoseFeed(b, NullEntity);
    m_previewSet.erase(b);
    // Through the handle: a recycled id is somebody else's entity.
    if (world && world->IsHandleValid(m_abEntH)) Hierarchy::DestroySubtree(*world, b);
    m_abEnt  = NullEntity;
    m_abEntH = NullEntityHandle;
    m_abDbgValid = false;
    m_abDiv = AbDivergence{};
    LOG_INFO("[CHAINPREVIEW] ab off: entity=%u why=%s frame=%llu", static_cast<unsigned>(b),
             why ? why : "", static_cast<unsigned long long>(m_tick));
}

void ChainPhysicsPreviewWindow::SyncAbToLive(const ChainPreviewContext& ctx, const char* why)
{
    // THE ONE WRITER of B's live component (the single-writer rule, applied to
    // B): field-wise, only when B's staged value moved, re-initialised on a
    // structural difference (NeedsReinit) - SyncStagedToLive's shape.
    if (m_abEnt == NullEntity || !ctx.world || !ctx.world->IsHandleValid(m_abEntH)) return;
    ChainPhysicsComponent* live = ctx.world->GetComponent<ChainPhysicsComponent>(m_abEnt);
    if (!live) return;
    if (m_abStaged.AuthoredEquals(m_abSynced)) { m_abStructural = false; return; }
    const bool structural = m_abStructural || ChainPhysicsSystem::NeedsReinit(m_abSynced, m_abStaged);
    m_abStaged.CopyAuthoredInto(*live);      // never *live = m_abStaged: field-wise keeps the caches
    m_abStaged.CopyAuthoredInto(m_abSynced);
    m_abStructural = false;
    ++m_abSyncs;
    if (!structural) return;
    // Not a bare Invalidate(B): B's input is the feed's copy of A's LocalPose,
    // which on a no-clip rig holds A's last SIMULATED rotations - InitEntity (it
    // runs before resetRot) would bake B's rest data from A's swing, and B's
    // UNCHANGED groups would then diverge from A's. A and B restart together from
    // a clean rest-seeded pose instead, as the Swap / Copy / Promote presses do
    // (and SetSoloGroup's reason for ResetPreviewEntity).
    if (m_harnessReinitBare)
    {
        // reverse arm (-chainpreview_reinit_bare): a bare Invalidate(B)
        if (ChainPhysicsSystem* cps = ctx.renderer->GetChainPhysicsSystem()) cps->Invalidate(m_abEnt);
    }
    else if (ctx.renderer && ctx.world)
        ResetSimulation(*ctx.renderer, *ctx.world, "ab structural");
    ++m_abRebuilds;
    LOG_INFO("[CHAINPREVIEW] ab B re-initialised: why=%s rebuilds=%u frame=%llu", why, m_abRebuilds,
             static_cast<unsigned long long>(m_tick));
}

void ChainPhysicsPreviewWindow::RebuildAbDiff()
{
    if (!m_abStagedValid || !m_stagedValid)
    {
        m_abDiff.clear();
        m_abDiffValid = false;
        return;
    }
    if (m_abDiffValid && m_staged.AuthoredEquals(m_abDiffA) && m_abStaged.AuthoredEquals(m_abDiffB))
        return;
    m_staged.CopyAuthoredInto(m_abDiffA);
    m_abStaged.CopyAuthoredInto(m_abDiffB);
    m_abDiffValid = true;
    m_abDiff.clear();
    // Every wire key of the serializer (the fields an Apply would write), in
    // line order: "key: A -> B".
    const auto ta = Tokens(BlobOf(m_staged));
    const auto tb = Tokens(BlobOf(m_abStaged));
    std::unordered_map<std::string, std::string> ma(ta.begin(), ta.end()), mb(tb.begin(), tb.end());
    for (const auto& [k, va] : ta)
    {
        const auto it = mb.find(k);
        const std::string vb = it == mb.end() ? std::string("(absent)") : it->second;
        if (va != vb) m_abDiff.push_back(k + ": " + va + " -> " + vb);
    }
    for (const auto& [k, vb] : tb)
        if (!ma.count(k)) m_abDiff.push_back(k + ": (absent) -> " + vb);
}

void ChainPhysicsPreviewWindow::TickAb(const ChainPreviewContext& ctx)
{
    Renderer* renderer = ctx.renderer;
    World*    world    = ctx.world;
    if (!renderer || !world) return;
    const bool haveStage = m_stageRoot != NullEntity && m_stageChain != NullEntity &&
                           m_stagedValid && !m_noChain && world->IsHandleValid(m_stageChainH);
    // B's parameters belong to the subject they were made for (its groups, its rig).
    if (m_abStagedValid && m_abSubject != m_subjectPath)
    {
        m_abStagedValid = false;
        m_abDiffValid = false;
        m_abDiff.clear();
    }
    auto ensureStaged = [&]()
    {
        if (m_abStagedValid) return;
        m_staged.CopyAuthoredInto(m_abStaged);   // B starts as a copy of A
        m_abStagedValid = true;
        m_abSubject = m_subjectPath;
    };
#if defined(WITH_HARNESS) && WITH_HARNESS
    // -chainpreview_abparam: A/B on at the earliest due entry; each edit lands
    // in B's staged copy (and reaches B's live component through its one writer
    // below). -chainpreview_ab_same ignores the values (B stays A).
    if (haveStage)
        for (HarnessAbParam& hp : m_harnessAbParams)
        {
            if (hp.applied || (hp.at != 0 && m_tick < hp.at)) continue;
            hp.applied = true;
            if (!m_abOn)
            {
                m_abOn = true;
                LOG_INFO("[CHAINPREVIEW] ab: switched on by -chainpreview_abparam at editor frame %llu",
                         static_cast<unsigned long long>(m_tick));
            }
            ensureStaged();
            const bool ok = m_harnessAbSame || ApplyParamKey(m_abStaged, hp.key, hp.value);
            ENGINE_CHECK(ok, "-chainpreview_abparam %s=%s could not be applied to B (group index "
                         "past the subject's groups); B keeps A's value.", hp.key.c_str(),
                         hp.value.c_str());
            LOG_INFO("[CHAINPREVIEW] abparam apply: key=%s value=%s applied=%d same=%d frame=%llu",
                     hp.key.c_str(), hp.value.c_str(), (ok && !m_harnessAbSame) ? 1 : 0,
                     m_harnessAbSame ? 1 : 0, static_cast<unsigned long long>(m_tick));
        }
    for (HarnessAbPress& hp : m_harnessAbPresses)
    {
        if (hp.done || m_tick < hp.at || !haveStage) continue;
        hp.done = true;
        m_pendingAbPress.push_back(hp.what);
        LOG_INFO("[CHAINPREVIEW] -chainpreview_ab_press: %s at editor frame %llu",
                 AbPressName(hp.what), static_cast<unsigned long long>(m_tick));
    }
#endif
    if (m_pendingAbToggle >= 0)
    {
        m_abOn = m_pendingAbToggle == 1;
        m_pendingAbToggle = -1;
    }
    for (AbPress p : m_pendingAbPress)
        if (p == AbPress::Off) m_abOn = false;
    if (!haveStage)
    {
        // B goes with the stage (DestroyStage); nothing to press without one.
        m_pendingAbPress.clear();
        return;
    }
    // B destroyed outside the panel (a Lua console, an Internal-toggle
    // delete): its id may be somebody else's - out of the lane now, respawned
    // below if A/B is still on.
    if (m_abEnt != NullEntity && !world->IsHandleValid(m_abEntH))
    {
        LOG_WARNING("[CHAINPREVIEW] ab: B (entity %u) was destroyed outside the panel",
                    static_cast<unsigned>(m_abEnt));
        if (ChainPhysicsSystem* cps = renderer->GetChainPhysicsSystem())
            cps->SetPreviewPoseFeed(m_abEnt, NullEntity);
        m_previewSet.erase(m_abEnt);
        m_abEnt  = NullEntity;
        m_abEntH = NullEntityHandle;
        m_abDbgValid = false;
    }
    if (!m_abOn)
    {
        if (m_abEnt != NullEntity) DestroyAbStage(renderer, world, "off");
        m_pendingAbPress.clear();
        return;
    }
    ensureStaged();
    if (m_harnessAbSame && !m_abStaged.AuthoredEquals(m_staged)) m_staged.CopyAuthoredInto(m_abStaged);
    if (m_abEnt == NullEntity) CreateAbStage(ctx);
    if (m_abEnt == NullEntity) { m_pendingAbPress.clear(); return; }

    // ---- The buttons: Swap A<->B / Copy A->B / Promote B->A ----
    // A is m_staged (the panel's authority): Swap and Promote write it, the next
    // TickStagedParams commits the change as ONE panel-undo step (so no work is
    // lost - Ctrl+Z brings the old A back; no confirmation needed) and its one
    // writer syncs it HERE, now. Both chains then restart from the same pose so
    // the comparison begins again from equal state.
    for (AbPress p : m_pendingAbPress)
    {
        if (p == AbPress::Off) continue;
        const AbDivergence before = m_abDiv;
        const int aItBefore = m_staged.iterations, bItBefore = m_abStaged.iterations;
        std::uint32_t n = 0;
        ChainPhysicsComponent tmp;
        switch (p)
        {
        case AbPress::Swap:
            m_staged.CopyAuthoredInto(tmp);
            m_abStaged.CopyAuthoredInto(m_staged);
            tmp.CopyAuthoredInto(m_abStaged);
            n = ++m_abSwaps;
            break;
        case AbPress::Copy:
            m_staged.CopyAuthoredInto(m_abStaged);
            n = ++m_abCopies;
            break;
        case AbPress::Promote:
            m_abStaged.CopyAuthoredInto(m_staged);
            n = ++m_abPromotes;
            break;
        case AbPress::Off:
            break;
        }
        if (m_harnessAbSame) m_staged.CopyAuthoredInto(m_abStaged);   // the reverse arm: B stays A
        SyncStagedToLive(ctx, AbPressName(p));
        SyncAbToLive(ctx, AbPressName(p));
        ResetSimulation(*renderer, *world, AbPressName(p));
        LOG_INFO("[CHAINPREVIEW] ab %s %u: divergedBefore=%u comparedBefore=%u maxDeltaBefore=%.6f "
                 "aIterationsBefore=%d bIterationsBefore=%d aIterationsAfter=%d bIterationsAfter=%d "
                 "sameParams=%d frame=%llu", AbPressName(p), n, before.diverged, before.compared,
                 before.maxDelta, aItBefore, bItBefore, m_staged.iterations, m_abStaged.iterations,
                 m_abStaged.AuthoredEquals(m_staged) ? 1 : 0, static_cast<unsigned long long>(m_tick));
        m_abDiv = AbDivergence{};
    }
    m_pendingAbPress.clear();

    // ---- B's one writer, then the field diff ----
    // The diff is rebuilt only when either side moved, and not while a slider is
    // held: it serializes both sides through a scratch World, which a per-frame
    // drag need not pay - it updates on release.
    SyncAbToLive(ctx, "edit");
    if (!ctx.uiEditActive) RebuildAbDiff();
}

void ChainPhysicsPreviewWindow::TickAbReadout(const ChainPreviewContext& ctx)
{
    m_abDbgValid = false;
    if (m_abEnt == NullEntity || !ctx.world || !ctx.renderer ||
        !ctx.world->IsHandleValid(m_abEntH))
        return;
    World& world = *ctx.world;
    ChainPhysicsSystem* cps = ctx.renderer->GetChainPhysicsSystem();
    const ChainPhysicsComponent* bLive = world.GetComponent<ChainPhysicsComponent>(m_abEnt);
    if (!cps || !bLive) return;
    // SERIAL (the game half's window; this frame's chain Update has not run):
    // the state B's last pass left - in A's skeleton space (the pose feed).
    m_abDbgValid = cps->FillDebugSnapshot(m_abEnt, m_abDbg, bLive);

    // ---- Divergence: per BONE, A's position vs B's (skeleton units) ----
    AbDivergence d;
    if (m_abDbgValid && m_dbgValid)
    {
        std::unordered_map<std::uint32_t, XMFLOAT3> pa, pb;
        BonePositions(m_dbgSnap, pa);
        BonePositions(m_abDbg, pb);
        double sum = 0.0;
        for (const auto& [bone, a] : pa)
        {
            const auto it = pb.find(bone);
            if (it == pb.end()) continue;
            const float dist = Dist3(a, it->second);
            ++d.compared;
            sum += dist;
            d.maxDelta = std::max(d.maxDelta, dist);
            if (dist > kAbDivergeEps) ++d.diverged;
        }
        d.meanDelta = d.compared ? static_cast<float>(sum / d.compared) : 0.f;
        d.valid = true;
    }
    m_abDiv = d;
    if (d.valid)
    {
        ++m_abFrames;
        m_abMaxDiverged = std::max(m_abMaxDiverged, d.diverged);
        if (m_abStagedValid && m_abStaged.AuthoredEquals(m_staged))
        {
            ++m_abSameFrames;
            if (d.diverged != 0 || d.maxDelta != 0.f) ++m_abSameDivergedFrames;
        }
    }
    // ---- The budget's ON side, once B has run for a while ----
    if (!m_abBudgetLogged && m_abBudgetOnAt != 0 && m_tick >= m_abBudgetOnAt)
    {
        m_abBudgetLogged = true;
        m_abBudgetOn = SampleAbBudget(ctx);
        const AbBudget& o = m_abBudgetOff;
        const AbBudget& n = m_abBudgetOn;
        auto dl = [](std::uint32_t on, std::uint32_t off)
        { return static_cast<int>(on) - static_cast<int>(off); };
        LOG_INFO("[CHAINPREVIEW] ab budget: path=feed instancesOff=%u instancesOn=%u "
                 "instancesDelta=%d skinJobsOff=%u skinJobsDelta=%d poseBonesOff=%u "
                 "poseBonesDelta=%d vertsOff=%u vertsDelta=%d entitiesDelta=%d "
                 "previewChainEntitiesOff=%u previewChainEntitiesOn=%u chainEntitiesDelta=%d "
                 "particlesOff=%u particlesOn=%u particlesDelta=%d solveMsOff=%.4f "
                 "solveMsOn=%.4f frame=%llu",
                 o.instances, n.instances, dl(n.instances, o.instances), o.skinJobs,
                 dl(n.skinJobs, o.skinJobs), o.poseBones, dl(n.poseBones, o.poseBones),
                 o.verts, dl(n.verts, o.verts), dl(n.entities, o.entities),
                 o.previewChainEntities, n.previewChainEntities,
                 dl(n.previewChainEntities, o.previewChainEntities), o.particles, n.particles,
                 dl(n.particles, o.particles), o.solveMs, n.solveMs,
                 static_cast<unsigned long long>(m_tick));
    }
#if defined(WITH_HARNESS) && WITH_HARNESS
    if (!m_harnessSubject.empty() && (m_tick % 60u) == 0u) LogAb("periodic");
#endif
}

void ChainPhysicsPreviewWindow::LogAb(const char* when) const
{
    // name=number pairs (logLineFields; the LAST line is the session's).
    // ghostParticles = B's particles mapped into the overlay snapshot (PPA);
    // ghostDrawn = the ghost dots Draw projected on its last frame.
    LOG_INFO("[CHAINPREVIEW] ab: stages=%u on=%d ghostParticles=%u ghostLinks=%u ghostDrawn=%u "
             "divergedBones=%u comparedBones=%u maxBoneDelta=%.6f meanBoneDelta=%.6f eps=%.6f "
             "maxDiverged=%u diffFields=%u sameParams=%d frames=%llu sameParamFrames=%llu "
             "sameParamsDivergedFrames=%llu creates=%u swaps=%u "
             "copies=%u promotes=%u syncs=%u rebuilds=%u aParticles=%u bParticles=%u "
             "aIterations=%d bIterations=%d when=%s",
             m_abEnt != NullEntity ? 2u : 1u, m_abOn ? 1 : 0,
             static_cast<unsigned>(m_overlaySnap.ghost.pos.size()),
             static_cast<unsigned>(m_overlaySnap.ghost.links.size()), m_overlayStats.ghostParticles,
             m_abDiv.diverged, m_abDiv.compared, m_abDiv.maxDelta, m_abDiv.meanDelta,
             kAbDivergeEps, m_abMaxDiverged, static_cast<unsigned>(m_abDiff.size()),
             (m_abStagedValid && m_stagedValid && m_abStaged.AuthoredEquals(m_staged)) ? 1 : 0,
             static_cast<unsigned long long>(m_abFrames),
             static_cast<unsigned long long>(m_abSameFrames),
             static_cast<unsigned long long>(m_abSameDivergedFrames), m_abCreates, m_abSwaps, m_abCopies,
             m_abPromotes, m_abSyncs, m_abRebuilds,
             m_dbgValid ? static_cast<unsigned>(m_dbgSnap.particlePos.size()) : 0u,
             m_abDbgValid ? static_cast<unsigned>(m_abDbg.particlePos.size()) : 0u,
             m_stagedValid ? m_staged.iterations : 0, m_abStagedValid ? m_abStaged.iterations : 0,
             when ? when : "");
}

void ChainPhysicsPreviewWindow::DrawAbUi(Renderer* renderer, const ChainParamTreeFn& drawParamTree)
{
    if (m_stageRoot == NullEntity || !m_stagedValid)
    {
        ImGui::TextDisabled("(no subject)");
        return;
    }
    bool on = m_abOn;
    if (ImGui::Checkbox("A/B ghost", &on)) m_pendingAbToggle = on ? 1 : 0;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Compare a second parameter set B with the staged one (A).\n"
                          "B is solved on A's own animated pose - no second character is\n"
                          "animated, skinned or drawn, it costs one extra chain solve - and\n"
                          "shown as a grey ghost of its particles and links over A.");
    if (!m_abOn) return;
    // The three buttons and the read-outs flow.
    namespace Flow = EditorUI::Flow;
    if (ImGui::SmallButton("Swap A<->B")) m_pendingAbPress.push_back(AbPress::Swap);
    Flow::SameLineIfFits(Flow::ButtonW("Copy A->B"));
    if (ImGui::SmallButton("Copy A->B")) m_pendingAbPress.push_back(AbPress::Copy);
    Flow::SameLineIfFits(Flow::ButtonW("Promote B->A"));
    if (ImGui::SmallButton("Promote B->A")) m_pendingAbPress.push_back(AbPress::Promote);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("B's values become the staged parameters (A): one panel-undo\n"
                          "step (Ctrl+Z brings A back). Nothing reaches the scene or the\n"
                          "asset until Apply.");
    if (m_abDiv.valid)
        Flow::Wrapped("diverged bones %u / %u   max %.4f", m_abDiv.diverged, m_abDiv.compared,
                      m_abDiv.maxDelta);
    else
        Flow::DisabledWrapped("diverged bones - (B has no state yet)");
    Flow::DisabledWrapped("ghost: %u particles, %u links drawn", m_overlayStats.ghostParticles,
                          m_overlayStats.ghostLinks);
    ImGui::Text("%u field(s) differ", static_cast<unsigned>(m_abDiff.size()));
    if (!m_abDiff.empty())
    {
        const float h = std::min(140.f, ImGui::GetTextLineHeightWithSpacing() *
                                             static_cast<float>(m_abDiff.size() + 1));
        ImGui::BeginChild("##cpv_ab_diff", ImVec2(0.f, h), ImGuiChildFlags_Borders);
        for (const std::string& l : m_abDiff) ImGui::TextUnformatted(l.c_str());
        ImGui::EndChild();
    }
    if (ImGui::TreeNode("B parameters"))
    {
        if (drawParamTree && renderer && m_abStagedValid &&
            m_stageSkelAsset < renderer->GetSkeletonRegistry().Count())
        {
            // The same tree, on B's staged copy (B's one writer syncs it; a
            // structural control only raises B's flag). B's edits are an
            // experiment and have no undo of their own - Promote is the commit.
            ChainParamHooks h;
            h.entity         = m_stageChain;   // B runs on A's skeleton and pose
            h.invalidate     = [this]() { m_abStructural = true; };
            h.markStructural = true;
            h.referenceBands = true;
            // B indexes A's skeleton, so it shares the stage's pose-tool state (never
            // the main viewport's).
            h.pose           = &m_stagePose;
            h.labelColumn    = TreeLabelColumn();   // width-aware column
            ImGui::PushID("cpv_ab_b");
            drawParamTree(m_abStaged, renderer->GetSkeletonRegistry().Get(m_stageSkelAsset), h);
            ImGui::PopID();
        }
        else
            ImGui::TextDisabled("(parameter tree unavailable)");
        ImGui::TreePop();
    }
}

// ==== Per-group solo (PhAT "Selected Simulation") ====
int ChainPhysicsPreviewWindow::ResolveSoloGroup(const std::string& name) const
{
    if (name == "none" || name == "-1") return -1;
    if (!m_stagedValid) return -2;
    const int count = static_cast<int>(m_staged.groups.size());
    if (!name.empty() && (name[0] == '#' || (name[0] == 'g' && name.size() > 1 &&
                                             std::isdigit(static_cast<unsigned char>(name[1])))))
    {
        char* end = nullptr;
        const long v = std::strtol(name.c_str() + 1, &end, 10);
        if (end && *end == '\0' && v >= 0 && v < count) return static_cast<int>(v);
    }
    for (int gi = 0; gi < count; ++gi)
        if (name == m_staged.groups[static_cast<std::size_t>(gi)].name) return gi;
    for (int gi = 0; gi < count; ++gi)
        if (name == m_staged.groups[static_cast<std::size_t>(gi)].rootBone) return gi;
    return -2;
}

void ChainPhysicsPreviewWindow::ApplySolo(const ChainPreviewContext& ctx, int groupIdx, const char* why)
{
    ChainPhysicsSystem* cps = ctx.renderer ? ctx.renderer->GetChainPhysicsSystem() : nullptr;
    const int was = m_soloGroup;
    m_soloGroup     = groupIdx;
    m_soloAppliedTo = m_stageChain;
    ++m_soloSets;
    // THE SOLVER applies it (SetSoloGroup re-initialises the entity: the freeze
    // lives in InitEntity's state) - on A and on the A/B ghost alike.
    // -chainpreview_solo_off is the reverse arm: logged, never handed over.
    if (cps && !m_harnessSoloOff)
    {
        cps->SetSoloGroup(m_stageChain, groupIdx);
        if (m_abEnt != NullEntity) cps->SetSoloGroup(m_abEnt, groupIdx);
    }
    // A re-init, like ResetSimulation: the hold check must not compare across it.
    ++m_resetSerial;
    m_laneHasState = false;
    m_simStale     = false;
    const char* name = (groupIdx >= 0 && m_stagedValid &&
                        groupIdx < static_cast<int>(m_staged.groups.size()))
                     ? m_staged.groups[static_cast<std::size_t>(groupIdx)].name : "";
    LOG_INFO("[CHAINPREVIEW] solo set: group='%s' groupIdx=%d was=%d sets=%u off=%d why=%s frame=%llu",
             name, groupIdx, was, m_soloSets, m_harnessSoloOff ? 1 : 0, why,
             static_cast<unsigned long long>(m_tick));
}

void ChainPhysicsPreviewWindow::TickSolo(const ChainPreviewContext& ctx)
{
    if (!ctx.world) return;
    // A solo names a group of ITS subject.
    if (m_soloSubject != m_subjectPath)
    {
        m_soloSubject   = m_subjectPath;
        m_soloGroup     = -1;
        m_pendingSolo   = -2;
        m_soloAppliedTo = NullEntity;
    }
    const bool haveStage = m_stageRoot != NullEntity && m_stageChain != NullEntity &&
                           m_stagedValid && !m_noChain && ctx.world->IsHandleValid(m_stageChainH);
#if defined(WITH_HARNESS) && WITH_HARNESS
    if (haveStage)
        for (HarnessSolo& hs : m_harnessSolo)
        {
            if (hs.done || (hs.at != 0 && m_tick < hs.at)) continue;
            hs.done = true;
            const int g = ResolveSoloGroup(hs.group);
            if (g == -2)
            {
                LOG_ERROR("[CHAINPREVIEW] -chainpreview_solo='%s' names no group of the subject "
                          "(%u groups; a name, a root bone, #<idx> or none) - not applied.",
                          hs.group.c_str(), static_cast<unsigned>(m_staged.groups.size()));
                continue;
            }
            m_pendingSolo = g;
            LOG_INFO("[CHAINPREVIEW] -chainpreview_solo: '%s' -> group %d at editor frame %llu",
                     hs.group.c_str(), g, static_cast<unsigned long long>(m_tick));
        }
#endif
    if (!haveStage) return;
    if (m_pendingSolo != -2)
    {
        const int g = m_pendingSolo;
        m_pendingSolo = -2;
        if (g != m_soloGroup || m_soloAppliedTo != m_stageChain) ApplySolo(ctx, g, "set");
    }
    else if (m_soloGroup >= 0 && m_soloAppliedTo != m_stageChain)
        ApplySolo(ctx, m_soloGroup, "respawn");   // a new stage of the same session
    // A structural edit removed the soloed group: nothing to solo any more.
    if (m_soloGroup >= static_cast<int>(m_staged.groups.size()))
        ApplySolo(ctx, -1, "group removed");
#if defined(WITH_HARNESS) && WITH_HARNESS
    if (!m_harnessSubject.empty() && m_soloSets != 0 && (m_tick % 60u) == 0u) LogSolo("periodic");
#endif
}

void ChainPhysicsPreviewWindow::LogSolo(const char* when) const
{
    // From the stage carrier's solver snapshot (TickStats): what the solver
    // actually froze, and how far each population sits from its animated rest.
    const ChainPhysicsSystem::DebugSnapshot& s = m_dbgSnap;
    const bool v = m_dbgValid;
    const char* name = (m_soloGroup >= 0 && m_stagedValid &&
                        m_soloGroup < static_cast<int>(m_staged.groups.size()))
                     ? m_staged.groups[static_cast<std::size_t>(m_soloGroup)].name : "";
    LOG_INFO("[CHAINPREVIEW] solo: group='%s' groupIdx=%d solverGroup=%d frozenGroups=%u "
             "frozenStrands=%u frozenParticles=%u frozenSprings=%u frozenMaxDev=%.6f "
             "activeMaxDev=%.6f strands=%u valid=%d sets=%u off=%d when=%s",
             name, m_soloGroup, v ? s.soloGroup : -1, v ? s.frozenGroups : 0u,
             v ? s.frozenStrands : 0u, v ? s.frozenParticles : 0u, v ? s.frozenSprings : 0u,
             v ? s.frozenMaxDeviation : 0.f, v ? s.activeMaxDeviation : 0.f, v ? s.strands : 0u,
             v ? 1 : 0, m_soloSets, m_harnessSoloOff ? 1 : 0, when ? when : "");
}

void ChainPhysicsPreviewWindow::DrawSoloUi()
{
    if (m_stageRoot == NullEntity || !m_stagedValid || m_noChain) return;
    const int shown = m_pendingSolo != -2 ? m_pendingSolo : m_soloGroup;
    std::string preview = "None (every group)";
    if (shown >= 0 && shown < static_cast<int>(m_staged.groups.size()))
    {
        const ChainGroupDef& g = m_staged.groups[static_cast<std::size_t>(shown)];
        preview = g.name[0] ? g.name : g.rootBone;
    }
    namespace Flow = EditorUI::Flow;
    ImGui::SetNextItemWidth(Flow::FitWidth(170.f, "Solo"));
    if (ImGui::BeginCombo("Solo", preview.c_str()))
    {
        if (ImGui::Selectable("None (every group)", shown < 0)) m_pendingSolo = -1;
        for (int gi = 0; gi < static_cast<int>(m_staged.groups.size()); ++gi)
        {
            const ChainGroupDef& g = m_staged.groups[static_cast<std::size_t>(gi)];
            char lbl[160];
            std::snprintf(lbl, sizeof(lbl), "%s##solo%d", g.name[0] ? g.name : g.rootBone, gi);
            if (ImGui::Selectable(lbl, shown == gi)) m_pendingSolo = gi;
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Simulate ONE group; every other group is FROZEN AT REST - its\n"
                          "bones follow the clip exactly (not stopped: a stopped chain would\n"
                          "pop back when un-soloed). Changing it re-initialises the chains.");
    if (m_soloGroup >= 0 && m_dbgValid)
        Flow::DisabledWrapped("frozen: %u group(s), %u strand(s)", m_dbgSnap.frozenGroups,
                              m_dbgSnap.frozenStrands);
    Flow::DisabledWrapped("Poke: Ctrl + right-drag a chain in the viewport");
    if (!m_pokeStatus.empty()) Flow::DisabledWrapped("%s", m_pokeStatus.c_str());
}

// ==== Poke: CPU ray -> nearest particle -> ApplyExternalImpulse ====
void ChainPhysicsPreviewWindow::TickPoke(const ChainPreviewContext& ctx)
{
    if (!ctx.renderer || !ctx.world) return;
    World&    world    = *ctx.world;
    Renderer& renderer = *ctx.renderer;
    const bool haveStage = m_stageRoot != NullEntity && m_stageChain != NullEntity &&
                           world.IsHandleValid(m_stageChainH);
    ChainPhysicsSystem* cps = renderer.GetChainPhysicsSystem();
    const float ow = static_cast<float>(m_overlayOutW ? m_overlayOutW : m_renderW);
    const float oh = static_cast<float>(m_overlayOutH ? m_overlayOutH : m_renderH);

#if defined(WITH_HARNESS) && WITH_HARNESS
    // ---- The probe (-chainpreview_poke): the state the previous frames left ----
    auto displacement = [&](const std::vector<XMFLOAT3>& from, const std::vector<XMFLOAT3>& to,
                            std::uint32_t poked, float& dPoked, float& dMax, float& dMean)
    {
        dPoked = dMax = dMean = 0.f;
        const std::size_t n = std::min(from.size(), to.size());
        double sum = 0.0;
        for (std::size_t i = 0; i < n; ++i)
        {
            const float d = Dist3(from[i], to[i]);
            dMax = std::max(dMax, d);
            sum += d;
            if (i == poked) dPoked = d;
        }
        dMean = n ? static_cast<float>(sum / n) : 0.f;
        if (from.size() != to.size()) dPoked = dMax = dMean = -1.f;   // a re-init moved the layout
    };
    if (m_pokeProbe.active && m_dbgValid && cps)
    {
        PokeProbe& pr = m_pokeProbe;
        const std::uint32_t pending = cps->GetPendingImpulseCount(m_stageChain);
        if (!pr.afterDone && m_tick >= pr.at + m_pokeProbeAfter)
        {
            pr.afterDone = true;
            float dp = 0.f, dm = 0.f, dn = 0.f;
            displacement(pr.before, m_dbgSnap.particlePos, pr.particle, dp, dm, dn);
            LOG_INFO("[CHAINPREVIEW] poke after: n=%u particle=%u frames=%u dispPoked=%.6f "
                     "maxDisp=%.6f meanDisp=%.6f pending=%u frame=%llu", pr.n, pr.particle,
                     m_pokeProbeAfter, dp, dm, dn, pending, static_cast<unsigned long long>(m_tick));
        }
        if (!pr.settleDone && m_tick >= pr.at + m_pokeProbeSettle)
        {
            pr.settleDone = true;
            pr.active = false;
            float dp = 0.f, dm = 0.f, dn = 0.f;
            displacement(pr.before, m_dbgSnap.particlePos, pr.particle, dp, dm, dn);
            // The CONTROL: the same number of frames with NO poke, right before
            // it - a static equilibrium reads ~0 here, which is what makes the
            // "after" displacement the poke's and the settle one a return.
            float cp = -1.f, cm = -1.f, cn = 0.f;
            if (!pr.pre.empty()) displacement(pr.pre, pr.before, pr.particle, cp, cm, cn);
            LOG_INFO("[CHAINPREVIEW] poke settle: n=%u particle=%u frames=%u dispPoked=%.6f "
                     "maxDisp=%.6f meanDisp=%.6f pending=%u preDrift=%.6f preMaxDrift=%.6f "
                     "frame=%llu", pr.n, pr.particle, m_pokeProbeSettle, dp, dm, dn, pending, cp, cm,
                     static_cast<unsigned long long>(m_tick));
        }
    }
    // ---- -chainpreview_poke: the UI's own path, driven from a particle ----
    for (HarnessPoke& hp : m_harnessPokes)
    {
        if (hp.done) continue;
        if (!hp.preDone && m_dbgValid && hp.at > m_pokeProbeAfter &&
            m_tick >= hp.at - m_pokeProbeAfter)
        {
            hp.preDone = true;
            m_pokeProbe.pre = m_dbgSnap.particlePos;
        }
        if (m_tick < hp.at || !haveStage) continue;
        hp.done = true;
        const auto refuse = [&](const char* why)
        {
            ++m_pokeRefusals;
            LOG_ERROR("[CHAINPREVIEW] -chainpreview_poke at editor frame %llu target '%s': %s - "
                      "nothing poked.", static_cast<unsigned long long>(m_tick), hp.target.c_str(), why);
        };
        if (!m_dbgValid || !m_overlayCam.valid || !ctx.buildBones || !(ow > 0.f) || !(oh > 0.f))
        { refuse("the stage has no solver state / preview camera yet"); continue; }
        // the target: a particle index, or the first free particle on a named bone
        std::uint32_t target = ~0u;
        {
            char* end = nullptr;
            const unsigned long v = std::strtoul(hp.target.c_str(), &end, 10);
            if (end && *end == '\0' && !hp.target.empty()) target = static_cast<std::uint32_t>(v);
        }
        if (target == ~0u)
        {
            SkeletonRegistry& reg = renderer.GetSkeletonRegistry();
            if (m_stageSkelAsset < reg.Count())
            {
                const SkeletonAsset& sk = reg.Get(m_stageSkelAsset);
                std::uint32_t bone = ~0u;
                for (std::uint32_t b = 0; b < sk.boneCount; ++b)
                    if (hp.target == sk.boneNames[b]) { bone = b; break; }
                for (std::size_t i = 0; bone != ~0u && i < m_dbgSnap.particleBone.size(); ++i)
                    if (m_dbgSnap.particleBone[i] == bone && !m_dbgSnap.particlePinned[i])
                    { target = static_cast<std::uint32_t>(i); break; }
            }
        }
        if (target >= m_dbgSnap.particlePos.size() || m_dbgSnap.particlePinned[target])
        { refuse("no free (unpinned, unfrozen) particle by that index / bone name"); continue; }
        // Its projection in the preview IS the "mouse" - the overlay's own
        // ViewProj (the reverse-Z projection) - so the ray below is tested
        // against the picture the user would click.
        XMFLOAT4X4 entF;
        if (!ChainOverlay::SkeletonToWorld(renderer, world, m_stageChain, ctx.buildBones, entF))
        { refuse("no skeleton -> world transform"); continue; }
        const XMMATRIX ent = XMLoadFloat4x4(&entF);
        const XMFLOAT3& ps = m_dbgSnap.particlePos[target];
        const XMVECTOR pw = XMVector3TransformCoord(XMVectorSet(ps.x, ps.y, ps.z, 1.f), ent);
        const XMVECTOR c  = XMVector4Transform(XMVectorSetW(pw, 1.f),
                                               ChainOverlay::ViewProj(m_overlayCam, ow / oh, false));
        const float w = XMVectorGetW(c);
        const float nx = XMVectorGetX(c) / w, ny = XMVectorGetY(c) / w;
        if (!(w > 1e-6f) || !(std::fabs(nx) <= 1.f) || !(std::fabs(ny) <= 1.f))
        { refuse("the target particle is not inside the preview picture"); continue; }
        m_pendingPoke = PendingPoke{};
        m_pendingPoke.valid    = true;
        m_pendingPoke.u        = 0.5f * (nx + 1.f);
        m_pendingPoke.v        = 0.5f * (1.f - ny);
        m_pendingPoke.harness  = true;
        m_pendingPoke.target   = target;
        m_pendingPoke.strength = hp.strength;
        m_pendingPoke.blend    = hp.blend;
        break;   // one poke per editor frame
    }
#endif
    if (!m_pendingPoke.valid) return;
    const PendingPoke pk = m_pendingPoke;
    m_pendingPoke = PendingPoke{};
    auto refuseUi = [&](const char* why)
    {
        ++m_pokeRefusals;
        m_pokeStatus = std::string("Poke: ") + why;
        LOG_INFO("[CHAINPREVIEW] poke refused: %s (frame %llu)", why,
                 static_cast<unsigned long long>(m_tick));
    };
    if (!haveStage || !cps || !m_dbgValid || !m_overlayCam.valid || !ctx.buildBones ||
        !(ow > 0.f) || !(oh > 0.f))
    { refuseUi("the chains have no simulated state to poke (simulate first)"); return; }

    // ---- The ray: the panel's OWN, from the camera it authored ----
    // Not Ray::FromViewport: a camera-basis construction needs no matrix inverse
    // and no reverse-Z unprojection. NDC x -> right * tan(fovY/2) * aspect, NDC y
    // -> up * tan(fovY/2): XMMatrixPerspectiveFovLH's own scales.
    const CamBasis cb = Basis(m_overlayCam);
    if (!cb.ok) { refuseUi("no preview camera"); return; }
    const float aspect = ow / oh;
    const float ndcX = 2.f * pk.u - 1.f, ndcY = 1.f - 2.f * pk.v;
    const XMVECTOR dirW = XMVector3Normalize(XMVectorAdd(cb.fwd,
        XMVectorAdd(XMVectorScale(cb.right, ndcX * cb.tanY * aspect),
                    XMVectorScale(cb.up, ndcY * cb.tanY))));
    // Into SKELETON space (where the solver simulates) through the overlay's own
    // skeleton -> world transform, so the pick matches what the overlay draws.
    XMFLOAT4X4 entF;
    if (!ChainOverlay::SkeletonToWorld(renderer, world, m_stageChain, ctx.buildBones, entF))
    { refuseUi("no skeleton -> world transform"); return; }
    const XMMATRIX ent = XMLoadFloat4x4(&entF);
    XMVECTOR det;
    const XMMATRIX inv = XMMatrixInverse(&det, ent);
    if (!(std::fabs(XMVectorGetX(det)) > 1e-20f)) { refuseUi("degenerate stage transform"); return; }
    const XMVECTOR oS = XMVector3TransformCoord(cb.eye, inv);
    const XMVECTOR dS = XMVector3Normalize(XMVector3TransformNormal(dirW, inv));

    // ---- The nearest free particle to the ray (angular distance = pixels) ----
    std::uint32_t best = ~0u;
    float bestAng = 1e30f;
    const std::size_t np = m_dbgSnap.particlePos.size();
    for (std::size_t i = 0; i < np; ++i)
    {
        if (m_dbgSnap.particlePinned[i]) continue;   // roots and frozen strands
        const XMVECTOR rel = XMVectorSubtract(XMLoadFloat3(&m_dbgSnap.particlePos[i]), oS);
        const float t = XMVectorGetX(XMVector3Dot(rel, dS));
        if (!(t > 1e-6f)) continue;
        const float perp = XMVectorGetX(XMVector3Length(XMVectorSubtract(rel, XMVectorScale(dS, t))));
        const float ang = perp / t;
        if (ang < bestAng) { bestAng = ang; best = static_cast<std::uint32_t>(i); }
    }
    constexpr float kPickPixels = 14.f;
    const float pxPerRad = oh / (2.f * cb.tanY);   // pixels per unit of tan-angle
    if (best == ~0u || bestAng * pxPerRad > kPickPixels)
    { refuseUi("no free particle under the cursor"); return; }
    const float pixelDist = bestAng * pxPerRad;
    float rayMiss = -1.f;
    if (pk.harness && pk.target < np)
    {
        const XMVECTOR rel = XMVectorSubtract(XMLoadFloat3(&m_dbgSnap.particlePos[pk.target]), oS);
        const float t = XMVectorGetX(XMVector3Dot(rel, dS));
        rayMiss = XMVectorGetX(XMVector3Length(XMVectorSubtract(rel, XMVectorScale(dS, t))));
    }

    // ---- The impulse, in SKELETON units / s ----
    XMVECTOR impS;
    const char* how = "";
    if (pk.harness)
    {
        // the camera's screen-right, mapped into skeleton space, x strength
        impS = XMVectorScale(XMVector3Normalize(XMVector3TransformNormal(cb.right, inv)), pk.strength);
        how = "harness";
    }
    else
    {
        const float dxPx = pk.du * ow, dyPx = pk.dv * oh;
        const XMVECTOR pw = XMVector3TransformCoord(XMLoadFloat3(&m_dbgSnap.particlePos[best]), ent);
        if (dxPx * dxPx + dyPx * dyPx >= 9.f)
        {
            // A DRAG: the dragged screen vector as a world displacement on the
            // camera-facing plane through the particle, x the gain (1/s).
            const float depth = XMVectorGetX(XMVector3Dot(XMVectorSubtract(pw, cb.eye), cb.fwd));
            const float perPx = 2.f * std::max(depth, 1e-4f) * cb.tanY / oh;
            const XMVECTOR dragW = XMVectorScale(XMVectorSubtract(XMVectorScale(cb.right, dxPx),
                                                                  XMVectorScale(cb.up, dyPx)), perPx);
            impS = XMVector3TransformNormal(XMVectorScale(dragW, m_pokeGain), inv);
            how = "drag";
        }
        else
        {
            // A CLICK: along the ray, half a subject height per second.
            impS = XMVector3TransformNormal(XMVectorScale(dirW, 0.5f * SubjectHeight()), inv);
            how = "click";
        }
    }
    XMFLOAT3 imp;
    XMStoreFloat3(&imp, impS);
    const float blend = pk.harness ? pk.blend : m_pokeBlend;
    const bool accepted = cps->ApplyExternalImpulse(m_stageChain, best, imp, blend);
    // The A/B ghost gets the SAME poke when it has the same particle layout -
    // "how do A and B answer the same push" is a comparison worth having.
    bool acceptedB = false;
    if (accepted && m_abEnt != NullEntity && m_abDbgValid && m_abDbg.particlePos.size() == np)
        acceptedB = cps->ApplyExternalImpulse(m_abEnt, best, imp, blend);
    ++m_pokes;
    const std::uint32_t bone = best < m_dbgSnap.particleBone.size() ? m_dbgSnap.particleBone[best] : ~0u;
    char st[160];
    std::snprintf(st, sizeof(st), "Poke %u: particle %u (bone %u) %s, |impulse| %.3f%s", m_pokes, best,
                  bone, how, XMVectorGetX(XMVector3Length(impS)), accepted ? "" : " - REFUSED");
    m_pokeStatus = st;
    LOG_INFO("[CHAINPREVIEW] poke: n=%u particle=%u bone=%u target=%d hitMatches=%d pixelDist=%.4f "
             "rayMiss=%.6f impulseX=%.4f impulseY=%.4f impulseZ=%.4f impulse=%.4f blend=%.4f "
             "accepted=%d acceptedB=%d pending=%u harness=%d frame=%llu how=%s",
             m_pokes, best, bone, pk.harness ? static_cast<int>(pk.target) : -1,
             (pk.harness && pk.target == best) ? 1 : 0, pixelDist, rayMiss, imp.x, imp.y, imp.z,
             XMVectorGetX(XMVector3Length(impS)), blend, accepted ? 1 : 0, acceptedB ? 1 : 0,
             cps->GetPendingImpulseCount(m_stageChain), pk.harness ? 1 : 0,
             static_cast<unsigned long long>(m_tick), how);
#if defined(WITH_HARNESS) && WITH_HARNESS
    if (pk.harness && accepted)
    {
        // The probe's reference: the state at the poke (this frame's snapshot,
        // before this frame's passes consume the impulse).
        std::vector<XMFLOAT3> pre = std::move(m_pokeProbe.pre);
        m_pokeProbe = PokeProbe{};
        m_pokeProbe.active   = true;
        m_pokeProbe.at       = m_tick;
        m_pokeProbe.particle = best;
        m_pokeProbe.n        = m_pokes;
        m_pokeProbe.before   = m_dbgSnap.particlePos;
        m_pokeProbe.pre      = std::move(pre);
    }
#endif
}

} // namespace Editor
