// ChainPhysicsPreviewSession.cpp - the Chain Physics Preview panel's SESSION
// rules and its solver-state read-out. Member functions of
// ChainPhysicsPreviewWindow, split from ChainPhysicsPreviewWindow.cpp for size;
// the header's side-effect discipline applies (Draw-side helpers touch panel
// state only; everything that reads the World or the solver runs in
// ProcessPendingActions).
//   * The Scene Apply target: kept across AdoptState, re-resolved through its
//     Guid, or LOST - never replaced by the live selection.
//   * Lost work: every subject-changing request and the close go through ONE
//     dirty confirmation (Apply / Discard / Keep editing); a world load stashes
//     the staged edits by subject and offers them back.
//   * The harness's -chainpreview_request / _confirm / _reapply drive exactly
//     those UI paths.
//   * ChainPhysicsSystem::FillDebugSnapshot for the overlay, the stats box and
//     the "[CHAINPREVIEW] stats:" line.
//   * The Inspector's "Edit Chains..." (TickEditRequest) and the Play "Keep
//     these values" slot (TickPlayKeep) - both enter the session through the
//     same guarded Use Selected.

#include "Editor/ChainPhysicsPreviewWindow.h"

#include "ECS/ECS.h"
#include "ECS/GuidComponent.h"
#include "ECS/HierarchyComponents.h"        // Parent / stage tags
#include "ECS/AnimationComponents.h"        // SkeletonComponent (carriers)
#include "Graphics/AnimProfile.h"           // previewChain* (the lane's own counters)
#include "Physics/ChainPhysicsSystem.h"
#include "Renderer/Renderer.h"
#include "System/Check.h"
#include "System/CommandLine.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include "imgui/imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace Editor
{
// ==== Small helpers ====
std::string ChainPhysicsPreviewWindow::StashKey(const std::string& subject)
{
    std::string r = Sys::RelativizeAssetPath(subject);
    if (r.empty()) r = subject;
    for (char& c : r)
    {
        if (c == '\\') c = '/';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return r;
}

// An in-place subject's stash is keyed by the SCENE TARGET (scene +
// name#ordinal + Guid), never by the clone's path: every in-place character is
// cloned to the same cache/chain_preview_subject.ipfb.
std::string ChainPhysicsPreviewWindow::SessionStashKey() const
{
    if (!m_subjectInPlace || m_harnessStashPathKey) return StashKey(m_subjectPath);
    char guid[40];
    if (m_sceneTargetGuidValid)
        std::snprintf(guid, sizeof(guid), "%016llx%016llx",
                      static_cast<unsigned long long>(m_sceneTargetGuidHi),
                      static_cast<unsigned long long>(m_sceneTargetGuidLo));
    else
        std::snprintf(guid, sizeof(guid), "-");
    return "inplace:" + StashKey(m_sceneTargetScene) + "|" + m_sceneTargetName + "#" +
           std::to_string(m_sceneTargetOrdinal) + "|" + guid;
}

std::string ChainPhysicsPreviewWindow::SessionStashLabel() const
{
    if (!m_subjectInPlace) return m_subjectPath;
    return "'" + m_sceneTargetName + "#" + std::to_string(m_sceneTargetOrdinal) + "' in '" +
           m_sceneTargetScene + "'";
}

// Play tears the stage down, and the unapplied edits then live only in the
// carry (m_stagedCarry) - still work a subject change would lose.
bool ChainPhysicsPreviewWindow::HasUnappliedWork() const
{
    // A "Keep these values" slot not yet written into m_staged is unapplied work
    // too; the Stop-time confirmation covers it.
    return HasUnappliedStagedWork() || m_playKeep.valid;
}

bool ChainPhysicsPreviewWindow::HasUnappliedStagedWork() const
{
    if (m_stageRoot != NullEntity) return IsStagedDirty();
    return m_stagedCarry && m_stagedDirty;
}

const char* ChainPhysicsPreviewWindow::GuardKindName(GuardKind k)
{
    switch (k)
    {
    case GuardKind::Load:        return "load";
    case GuardKind::Drop:        return "drop";
    case GuardKind::UseSelected: return "useselected";
    case GuardKind::Reload:      return "reload";
    case GuardKind::Close:       return "close";
    default:                     return "none";
    }
}

bool ChainPhysicsPreviewWindow::EditNaiveArm()
{
#if defined(WITH_HARNESS) && WITH_HARNESS
    static const bool s_on = Sys::CommandLine::Get().Has("-chainpreview_edit_naive");
    return s_on;
#else
    return false;
#endif
}

// The panel-tree probe's reverse arm (-chainpreview_tree_live): the tree on
// the stage's LIVE component, with a direct Invalidate.
bool ChainPhysicsPreviewWindow::TreeLiveArm()
{
#if defined(WITH_HARNESS) && WITH_HARNESS
    static const bool s_on = Sys::CommandLine::Get().Has("-chainpreview_tree_live");
    return s_on;
#else
    return false;
#endif
}

// "<name>[#k]": the k-th (0-based) PARENTLESS scene entity with that name -
// scene characters are roots; the .iscn spawn under them may reuse names.
// Stage entities never qualify.
Entity ChainPhysicsPreviewWindow::FindNamed(World& world, const std::string& nameIdx) const
{
    std::string name = nameIdx;
    int k = 0;
    if (const std::size_t h = nameIdx.rfind('#'); h != std::string::npos && h + 1 < nameIdx.size() &&
        nameIdx.find_first_not_of("0123456789", h + 1) == std::string::npos)
    {
        name = nameIdx.substr(0, h);
        k    = std::atoi(nameIdx.c_str() + h + 1);
    }
    int seen = 0;
    for (Entity e : world.GetEntities())
    {
        if (!world.IsAlive(e) || m_stageSet.count(e) || world.HasComponent<PreviewStageTag>(e))
            continue;
        if (const Parent* p = world.GetComponent<Parent>(e))
            if (p->entity != NullEntity && world.IsAlive(p->entity)) continue;
        if (world.GetName(e) != name) continue;
        if (seen++ == k) return e;
    }
    return NullEntity;
}

// ==== The Scene Apply target across a World epoch ====
void ChainPhysicsPreviewWindow::RecordSceneTarget(World& world, Entity root)
{
    m_sceneTargetName = world.GetName(root);
    // The rest of the in-place stash identity: FindNamed's ordinal among
    // same-named parentless roots, and the scene.
    m_sceneTargetScene   = m_scenePathNow;
    m_sceneTargetOrdinal = 0;
    {
        int seen = 0;
        for (Entity e : world.GetEntities())
        {
            if (!world.IsAlive(e) || m_stageSet.count(e) || world.HasComponent<PreviewStageTag>(e))
                continue;
            if (const Parent* p = world.GetComponent<Parent>(e))
                if (p->entity != NullEntity && world.IsAlive(p->entity)) continue;
            if (world.GetName(e) != m_sceneTargetName) continue;
            if (e == root) { m_sceneTargetOrdinal = seen; break; }
            ++seen;
        }
    }
    m_sceneTargetGuidValid = false;
    m_sceneTargetGuidHi = m_sceneTargetGuidLo = 0;
    if (const GuidComponent* g = world.GetComponent<GuidComponent>(root))
        if (g->guid.IsValid())
        {
            m_sceneTargetGuidValid = true;
            m_sceneTargetGuidHi = g->guid.hi;
            m_sceneTargetGuidLo = g->guid.lo;
        }
    // Also logs the carrier Apply will write (the clicked one when it belongs to
    // this root, else the root's first carrier).
    LOG_INFO("[CHAINPREVIEW] scene target: name='%s' entity=%u guid=%d carrier=%u preferred=%d "
             "(Use Selected)", m_sceneTargetName.c_str(), static_cast<unsigned>(root),
             m_sceneTargetGuidValid ? 1 : 0, static_cast<unsigned>(SceneCarrierOf(world, root)),
             (world.IsHandleValid(m_sceneCarrierH) && SceneCarrierOf(world, root) == m_sceneCarrierH.entity)
                 ? 1 : 0);
}

void ChainPhysicsPreviewWindow::KeepSceneTargetAcrossEpoch(World& world)
{
    if (!m_sceneTargetSet)
    {
        m_sceneTargetH = NullEntityHandle;
        return;
    }
    if (m_harnessSceneTargetDrop)
    {
        // The reverse arm: drop the target on every epoch change.
        m_sceneTargetH    = NullEntityHandle;
        m_sceneTargetLost = true;
        LOG_WARNING("[CHAINPREVIEW] scene target DROPPED on the world epoch change "
                    "(-chainpreview_scene_target_drop)");
        return;
    }
    // AdoptState restores ids AND generations: a handle that still validates
    // AND still names an entity with the recorded name (and Guid) is the same
    // character. A different World (a scene load) may recycle the pair, which
    // is what the identity test is for.
    auto guidOf = [&](Entity e, std::uint64_t& hi, std::uint64_t& lo)
    {
        const GuidComponent* g = world.GetComponent<GuidComponent>(e);
        if (!g || !g->guid.IsValid()) return false;
        hi = g->guid.hi; lo = g->guid.lo;
        return true;
    };
    if (world.IsHandleValid(m_sceneTargetH) && world.GetName(m_sceneTargetH.entity) == m_sceneTargetName)
    {
        std::uint64_t hi = 0, lo = 0;
        const bool same = !m_sceneTargetGuidValid ||
                          (guidOf(m_sceneTargetH.entity, hi, lo) &&
                           hi == m_sceneTargetGuidHi && lo == m_sceneTargetGuidLo);
        if (same)
        {
            ++m_sceneTargetKeeps;
            m_sceneTargetLost = false;
            LOG_INFO("[CHAINPREVIEW] scene target kept across the world epoch: name='%s' "
                     "entity=%u keeps=%u", m_sceneTargetName.c_str(),
                     static_cast<unsigned>(m_sceneTargetH.entity), m_sceneTargetKeeps);
            return;
        }
    }
    if (m_sceneTargetGuidValid)
        if (auto* gp = world.GetPool<GuidComponent>())
        {
            auto& ents = gp->Entities();
            auto& data = gp->Data();
            for (std::size_t i = 0; i < ents.size(); ++i)
            {
                if (data[i].guid.hi != m_sceneTargetGuidHi || data[i].guid.lo != m_sceneTargetGuidLo)
                    continue;
                if (!world.IsAlive(ents[i]) || world.HasComponent<PreviewStageTag>(ents[i])) continue;
                m_sceneTargetH = world.MakeHandle(ents[i]);
                m_sceneTargetLost = false;
                ++m_sceneTargetReresolves;
                LOG_INFO("[CHAINPREVIEW] scene target re-resolved through its Guid: name='%s' "
                         "entity=%u reresolves=%u", m_sceneTargetName.c_str(),
                         static_cast<unsigned>(ents[i]), m_sceneTargetReresolves);
                return;
            }
        }
    m_sceneTargetH    = NullEntityHandle;
    m_sceneTargetLost = true;
    LOG_WARNING("[CHAINPREVIEW] scene target LOST on the world epoch change: name='%s' - "
                "Apply to the scene refuses until Use Selected is pressed again",
                m_sceneTargetName.c_str());
}

// ==== One dirty confirmation for every way staged work could be lost ====
void ChainPhysicsPreviewWindow::RequestOpen(bool open)
{
    if (open) { m_open = true; return; }
    if (!m_open) return;
    RequestGuarded(GuardedRequest{ GuardKind::Close, std::string(), NullEntity });
}

void ChainPhysicsPreviewWindow::RequestGuarded(const GuardedRequest& r)
{
    // Carried edits count too: during Play the stage is gone and they live only
    // in the carry. The confirmation is then QUEUED: TickGuard runs in Edit mode
    // only, so it is answered after Stop (and a "keep" leaves the subject, and the
    // carry, as they were). The keep slot's OWN subject switch does not count the
    // slot it serves.
    const bool dirty = r.fromKeep ? HasUnappliedStagedWork() : HasUnappliedWork();
    if (dirty && !m_harnessConfirmOff)
    {
        if (m_guarded.kind != GuardKind::None)
        {
            LOG_INFO("[CHAINPREVIEW] confirm: a %s request is already waiting - %s ignored",
                     GuardKindName(m_guarded.kind), GuardKindName(r.kind));
            return;
        }
        m_guarded = r;
        m_confirmGuardOpen = true;       // Draw opens the modal
        LOG_INFO("[CHAINPREVIEW] confirm asked: kind=%s dirty=1 stage=%d carried=%d subject='%s' "
                 "request='%s'", GuardKindName(r.kind), m_stageRoot != NullEntity ? 1 : 0,
                 m_stagedCarry ? 1 : 0, m_subjectPath.c_str(), r.path.c_str());
        return;
    }
    if (dirty)   // -chainpreview_confirm_off: no confirmation, said out loud
        LOG_WARNING("[CHAINPREVIEW] %s with unapplied staged edits and NO confirmation "
                    "(-chainpreview_confirm_off)", GuardKindName(r.kind));
    CommitGuarded(r);
}

void ChainPhysicsPreviewWindow::CommitGuarded(const GuardedRequest& r)
{
    switch (r.kind)
    {
    case GuardKind::Load:
    case GuardKind::Drop:
        m_pendingSubject        = r.path;
        m_pendingSubjectInPlace = false;
        // A Load / drop never inherits a Use Selected scale.
        m_pendingScaleValid     = false;
        m_pendingForceRespawn   = false;
        break;
    case GuardKind::UseSelected:
        m_pendingUseSelected = true;
        m_pendingSelected    = r.selected;
        break;
    case GuardKind::Reload:
        m_pendingReload = true;
        break;
    case GuardKind::Close:
        m_open = false;
        break;
    default:
        break;
    }
}

void ChainPhysicsPreviewWindow::TickGuard(const ChainPreviewContext& ctx)
{
    (void)ctx;
    if (m_guarded.kind == GuardKind::None) { m_pendingGuardAnswer = -1; return; }
#if defined(WITH_HARNESS) && WITH_HARNESS
    // -chainpreview_confirm: the harness answers the modal nobody can click
    // under -hidden - one answer per confirmation, in order.
    if (m_pendingGuardAnswer < 0 && m_harnessConfirmNext < m_harnessConfirm.size())
        m_pendingGuardAnswer = m_harnessConfirm[m_harnessConfirmNext++];
#endif
    if (m_pendingGuardAnswer < 0) return;
    const int answer = m_pendingGuardAnswer;
    m_pendingGuardAnswer = -1;
    const GuardedRequest r = m_guarded;
    m_guarded = GuardedRequest{};
    ++m_confirms;
    static const char* kAnswer[3] = { "keep", "discard", "apply" };
    LOG_INFO("[CHAINPREVIEW] confirm %u: kind=%s answer=%s answerCode=%d dirty=%d "
             "subject='%s' request='%s' frame=%llu", m_confirms, GuardKindName(r.kind),
             kAnswer[std::clamp(answer, 0, 2)], answer, HasUnappliedWork() ? 1 : 0,
             m_subjectPath.c_str(), r.path.c_str(), static_cast<unsigned long long>(m_tick));
    if (answer == 0) return;                 // Keep editing: nothing happens
    if (answer == 1)
    {
        // Discard: the staged work of THIS subject goes - no carry, no stash.
        m_stagedCarry = false;
        m_stagedStash.erase(SessionStashKey());
        // ... and a pending "Keep these values" (unless this request IS its subject
        // switch).
        if (!r.fromKeep && m_playKeep.valid)
        {
            ++m_playKeepDropped;
            LOG_INFO("[CHAINPREVIEW] play keep dropped: target='%s' why=discarded",
                     m_playKeep.name.c_str());
            m_playKeep = PlayKeep{};
        }
        if (r.kind == GuardKind::Close)
        {
            m_pendingDiscard = true;         // the close path reverts + clears the panel-undo ring
            m_open = false;
        }
        else
            CommitGuarded(r);                // a new subject session starts anyway
        return;
    }
    // Apply and continue / close: TickApply carries the request out only if
    // the apply really happened (a refused apply keeps the edits and the subject).
    m_pendingApply = ApplyRequest{ m_applyDest, std::string(), false, true };
    if (r.kind == GuardKind::Close) m_closeAfterApply = true;
    else                            m_continueAfterApply = r;
}

// Picking the same subject again offers to re-apply the staged params kept in
// panel memory (m_stagedStash).
void ChainPhysicsPreviewWindow::TickReapply(const ChainPreviewContext& ctx)
{
    (void)ctx;
    if (!m_reapplyOffer) { m_pendingReapply = -1; return; }
#if defined(WITH_HARNESS) && WITH_HARNESS
    if (m_pendingReapply < 0 && m_harnessReapply >= 0) m_pendingReapply = m_harnessReapply;
#endif
    if (m_pendingReapply < 0 || !m_stagedValid || m_stageRoot == NullEntity) return;
    const int answer = m_pendingReapply;
    m_pendingReapply = -1;
    m_reapplyOffer   = false;
    auto it = m_stagedStash.find(m_reapplyKey);
    if (it == m_stagedStash.end()) return;
    if (answer == 1)
    {
        // An edit like a preset load: one panel-undo step, synced (and
        // re-initialised if structural) by TickStagedParams in this window.
        it->second.value.CopyAuthoredInto(m_staged);
        ++m_reapplies;
    }
    LOG_INFO("[CHAINPREVIEW] staged re-applied: subject='%s' answer=%d reapplied=%d "
             "damping=%.4f dirty=%d reapplies=%u frame=%llu source=%s", m_subjectPath.c_str(),
             answer, answer == 1 ? 1 : 0, m_staged.damping, IsStagedDirty() ? 1 : 0, m_reapplies,
             static_cast<unsigned long long>(m_tick), it->second.label.c_str());
    m_stagedStash.erase(it);
}

// ==== Harness requests (-chainpreview_request / _confirm, subject "selected:") ====
bool ChainPhysicsPreviewWindow::HarnessAddRequest(const std::string& kind, const std::string& arg,
                                                  std::uint64_t at)
{
    // edit:<name>[#k][/<child>] = the Inspector's "Edit Chains..." on that
    // character's carrier (<child>: a named carrier under the root instead of the
    // first one).
    static const char* kKinds[] = { "load", "reload", "close", "open", "useselected", "select",
                                    "worldload", "edit", "group" };
    bool known = false;
    for (const char* k : kKinds) known = known || kind == k;
    if (!known || at == 0) return false;
    const bool needArg = kind == "load" || kind == "useselected" || kind == "select" ||
                         kind == "edit" || kind == "group";
    if (needArg == arg.empty()) return false;
    // group:<new>=<a>+<b>[+...]
    if (kind == "group")
    {
        const std::size_t eq = arg.find('=');
        if (eq == std::string::npos || eq == 0 || arg.find('+', eq) == std::string::npos) return false;
    }
    m_harnessRequests.push_back(HarnessRequest{ kind, arg, at, false });
    return true;
}

bool ChainPhysicsPreviewWindow::HarnessSetConfirm(const std::vector<int>& answers)
{
    if (answers.empty()) return false;
    for (int a : answers) if (a < 0 || a > 2) return false;
    m_harnessConfirm = answers;
    m_harnessConfirmNext = 0;
    return true;
}

void ChainPhysicsPreviewWindow::TickHarnessRequests(const ChainPreviewContext& ctx)
{
#if defined(WITH_HARNESS) && WITH_HARNESS
    World& world = *ctx.world;
    // -chainpreview_subject=selected:<name>[#k] - the in-place subject, taken
    // exactly as the Use Selected button takes it (nothing is staged yet, so
    // there is nothing to confirm).
    if (!m_harnessUseSelected.empty() && !m_harnessUseSelectedDone)
    {
        const Entity e = FindNamed(world, m_harnessUseSelected);
        if (e != NullEntity)
        {
            m_harnessUseSelectedDone = true;
            m_pendingUseSelected = true;
            m_pendingSelected    = e;
            LOG_INFO("[CHAINPREVIEW] harness: Use Selected on '%s' (entity %u) at editor frame %llu",
                     m_harnessUseSelected.c_str(), static_cast<unsigned>(e),
                     static_cast<unsigned long long>(m_tick));
        }
    }
    for (HarnessRequest& r : m_harnessRequests)
    {
        if (r.done || m_tick < r.at) continue;
        r.done = true;
        const bool dirty = HasUnappliedWork();
        Entity e = NullEntity;
        if (r.kind == "load")
            RequestGuarded(GuardedRequest{ GuardKind::Load, r.arg, NullEntity });
        else if (r.kind == "reload")
            RequestGuarded(GuardedRequest{ GuardKind::Reload, std::string(), NullEntity });
        else if (r.kind == "close")
        {
            // A HARNESS close (the -panelvisibleverify oracle must not read it
            // as "the panel stopped drawing"), through the Window menu's path.
            m_harnessClosed = true;
            RequestOpen(false);
        }
        else if (r.kind == "open")
            RequestOpen(true);
        else if (r.kind == "worldload")
            m_harnessWorldReload = true;
        else if (r.kind == "group")
        {
            // The TWO-CARRIER fixture no shipped content has: a new in-place root
            // <new> with the named scene roots re-parented under it (an identity
            // transform, so their world poses do not move). Use Selected on one of the
            // carriers clones the whole group, and the stage has two carriers.
            const std::size_t eq = r.arg.find('=');
            const std::string name = r.arg.substr(0, eq);
            std::vector<Entity> members;
            std::size_t pos = eq + 1;
            bool ok = true;
            while (pos <= r.arg.size())
            {
                std::size_t plus = r.arg.find('+', pos);
                if (plus == std::string::npos) plus = r.arg.size();
                const Entity m = FindNamed(world, r.arg.substr(pos, plus - pos));
                const Parent* par = m != NullEntity ? world.GetComponent<Parent>(m) : nullptr;
                if (m == NullEntity || (par && par->entity != NullEntity) ||
                    world.HasComponent<PreviewStageTag>(m))
                    ok = false;
                else
                    members.push_back(m);
                pos = plus + 1;
            }
            ENGINE_CHECK(ok && members.size() >= 2, "-chainpreview_request group:%s - every member "
                         "must be a live, parentless scene root; nothing is grouped.", r.arg.c_str());
            if (ok && members.size() >= 2)
            {
                e = world.CreateEntity();
                world.SetName(e, name);
                world.AddComponent<LocalTransform>(e, LocalTransform{});
                world.AddComponent<GlobalTransform>(e, GlobalTransform{});
                Children ch;
                for (Entity m : members)
                {
                    world.AddComponent<Parent>(m, Parent{ e });
                    ch.entities.push_back(m);
                }
                world.AddComponent<Children>(e, std::move(ch));
                std::vector<Entity> cs;
                ChainCarriersUnder(world, e, false, cs);
                LOG_INFO("[CHAINPREVIEW] harness group: name='%s' entity=%u members=%zu carriers=%zu "
                         "frame=%llu", name.c_str(), static_cast<unsigned>(e), members.size(),
                         cs.size(), static_cast<unsigned long long>(m_tick));
            }
        }
        else if (r.kind == "edit")
        {
            // The Inspector row is drawn for the entity that CARRIES the
            // component: resolve the character, then its carrier (a named one
            // when the arg says <root>/<child>).
            const std::size_t slash = r.arg.find('/');
            const Entity root = FindNamed(world, r.arg.substr(0, slash));
            Entity carrier = NullEntity;
            if (root != NullEntity && slash != std::string::npos &&
                r.arg.find('/', slash + 1) != std::string::npos)
            {
                // <root>/<a>/<b>/<carrier>: the carrier by its NAME PATH below the
                // root (two carriers may share a name, as in the group fixture).
                const std::string path = r.arg.substr(slash + 1);
                std::vector<Entity> cs;
                ChainCarriersUnder(world, root, false, cs);
                for (Entity c : cs)
                    if (carrier == NullEntity && CarrierNamePath(world, root, c) == path) carrier = c;
            }
            else if (root != NullEntity && slash != std::string::npos)
            {
                const std::string child = r.arg.substr(slash + 1);
                std::vector<Entity> stack{ root };
                for (int guard = 0; !stack.empty() && guard < 4096 && carrier == NullEntity; ++guard)
                {
                    const Entity c = stack.back();
                    stack.pop_back();
                    if (!world.IsAlive(c)) continue;
                    if (world.GetName(c) == child && world.HasComponent<ChainPhysicsComponent>(c) &&
                        world.HasComponent<SkeletonComponent>(c))
                        carrier = c;
                    if (const Children* ch = world.GetComponent<Children>(c))
                        for (Entity k : ch->entities) stack.push_back(k);
                }
            }
            else if (root != NullEntity)
                carrier = SceneCarrierOf(world, root);
            ENGINE_CHECK(carrier != NullEntity && world.HasComponent<ChainPhysicsComponent>(carrier),
                         "-chainpreview_request edit:%s - no chain carrier by that name; nothing is "
                         "done.", r.arg.c_str());
            if (carrier != NullEntity && world.HasComponent<ChainPhysicsComponent>(carrier))
            {
                e = carrier;
                RequestEditEntity(world.MakeHandle(carrier), !ctx.editing);
            }
        }
        else
        {
            e = FindNamed(world, r.arg);
            ENGINE_CHECK(e != NullEntity, "-chainpreview_request %s:%s - no scene entity by that "
                         "name; nothing is done.", r.kind.c_str(), r.arg.c_str());
            if (e != NullEntity && r.kind == "useselected")
                RequestGuarded(GuardedRequest{ GuardKind::UseSelected, std::string(), e });
            else if (e != NullEntity)
                m_harnessSelectOut = e;      // EditorLayer selects it (the EDITOR's selection)
        }
        LOG_INFO("[CHAINPREVIEW] request: kind=%s arg='%s' entity=%u dirty=%d guarded=%d frame=%llu",
                 r.kind.c_str(), r.arg.c_str(), static_cast<unsigned>(e), dirty ? 1 : 0,
                 m_guarded.kind != GuardKind::None ? 1 : 0, static_cast<unsigned long long>(m_tick));
    }
#else
    (void)ctx;
#endif
}

// ==== The Inspector's "Edit Chains..." in Edit mode ====
void ChainPhysicsPreviewWindow::TickEditRequest(const ChainPreviewContext& ctx)
{
    if (!m_pendingEdit) return;
    m_pendingEdit = false;
    const EntityHandle h = m_pendingEditH;
    m_pendingEditH = NullEntityHandle;
    World& world = *ctx.world;
    ++m_editRequests;
    if (!world.IsHandleValid(h) || world.HasComponent<PreviewStageTag>(h.entity))
    {
        m_status = "Edit Chains: that entity is gone (or is the preview stage).";
        LOG_INFO("[CHAINPREVIEW] inspector edit: entity=%u refused=1 requests=%u",
                 static_cast<unsigned>(h.entity), m_editRequests);
        return;
    }
    const Entity carrier = h.entity;
    const Entity root    = SceneRootOf(world, carrier);
    const PrefabSourcePath* psp = world.GetComponent<PrefabSourcePath>(root);
    const bool inPlace = !psp || psp->path.empty();
    // "Already the subject": its root IS the session's scene target, or it is an
    // instance of the prefab the stage was spawned from. Use Selected would
    // otherwise force-respawn and put staged work behind a confirmation for a
    // no-op. Decided from the SESSION (subject path, scene target, in-place flag),
    // not from a live stage: a hidden or collapsed panel has torn its stage down
    // (the carry holds the edits), and a click on its own subject must not become
    // a dirty-edits modal and a forced respawn.
    const char* why = "";
    bool same = false;
    bool otherCarrier = false;
    if (!m_subjectPath.empty() && !m_spawnFailed && !EditNaiveArm())
    {
        if (world.IsHandleValid(m_sceneTargetH) && m_sceneTargetH.entity == root)
        { same = true; why = "scene target"; }
        else if (!m_subjectInPlace && !inPlace && StashKey(psp->path) == StashKey(m_subjectPath))
        { same = true; why = "same prefab"; }
        // The same character but ANOTHER of its carriers: the staged values belong
        // to the stage's carrier, so this is a new subject session (the guarded Use
        // Selected respawns with the clicked carrier), not a focus.
        if (same && m_stageCarrierOrdinal >= 0 && world.HasComponent<ChainPhysicsComponent>(carrier) &&
            CounterpartCarrier(world, root) != carrier)
        { same = false; otherCarrier = true; why = "other carrier"; }
    }
    bool retarget = false;
    if (same)
    {
        ++m_editSameTarget;
        // The clicked character becomes Apply -> Scene's target (an explicit
        // request, never the live selection), and its clicked carrier the one Apply
        // writes. Nothing respawns.
        m_sceneCarrierH = h;
        if (m_stageCarrierOrdinal < 0)
        {
            // No spawn yet (the panel was never visible): the coming spawn takes
            // the clicked carrier.
            std::vector<Entity> cs;
            ChainCarriersUnder(world, root, false, cs);
            m_carrierPref = CarrierPref{};
            for (std::size_t i = 0; i < cs.size(); ++i)
                if (cs[i] == carrier) m_carrierPref.ordinal = static_cast<int>(i);
            if (m_carrierPref.ordinal >= 0) m_carrierPref.path = CarrierNamePath(world, root, carrier);
        }
        if (!(world.IsHandleValid(m_sceneTargetH) && m_sceneTargetH.entity == root))
        {
            retarget = true;
            m_sceneTargetH    = world.MakeHandle(root);
            m_sceneTargetSet  = true;
            m_sceneTargetLost = false;
            RecordSceneTarget(world, root);
        }
        m_status.clear();
    }
    else
    {
        const bool waiting = m_guarded.kind != GuardKind::None;
        RequestGuarded(GuardedRequest{ GuardKind::UseSelected, std::string(), carrier });
        if (!waiting && m_guarded.kind != GuardKind::None) ++m_editGuardAsks;
    }
    LOG_INFO("[CHAINPREVIEW] inspector edit: entity=%u root=%u carrier=%u carrierIsRoot=%d "
             "inPlace=%d sameTarget=%d retarget=%d otherCarrier=%d stageLive=%d guarded=%d why='%s' "
             "requests=%u sameTargets=%u "
             "guardAsks=%u naive=%d frame=%llu", static_cast<unsigned>(carrier),
             static_cast<unsigned>(root), static_cast<unsigned>(carrier), carrier == root ? 1 : 0,
             inPlace ? 1 : 0, same ? 1 : 0, retarget ? 1 : 0, otherCarrier ? 1 : 0,
             m_stageRoot != NullEntity ? 1 : 0,
             m_guarded.kind != GuardKind::None ? 1 : 0, why, m_editRequests, m_editSameTarget,
             m_editGuardAsks, EditNaiveArm() ? 1 : 0, static_cast<unsigned long long>(m_tick));
}

// ==== "Keep these values" after Stop ====
void ChainPhysicsPreviewWindow::TickPlayKeep(const ChainPreviewContext& ctx)
{
    if (!m_playKeep.valid) return;
    World& world = *ctx.world;
    auto drop = [&](const char* why)
    {
        ++m_playKeepDropped;
        LOG_INFO("[CHAINPREVIEW] play keep dropped: target='%s' why=%s dropped=%u",
                 m_playKeep.name.c_str(), why, m_playKeepDropped);
        m_playKeep = PlayKeep{};
    };
    if (m_harnessPlayKeepOff) { drop("-chainpreview_play_keep_off"); return; }
    // AdoptState restores ids AND generations, so a scene character that existed
    // before Play still validates; one spawned IN Play does not.
    if (!world.IsHandleValid(m_playKeep.root)) { drop("target lost at Stop"); return; }
    const Entity root = m_playKeep.root.entity;
    bool isSubject = false;
    if (!m_subjectPath.empty() && !m_spawnFailed)
    {
        if (world.IsHandleValid(m_sceneTargetH) && m_sceneTargetH.entity == root)
            isSubject = true;
        else if (!m_subjectInPlace && !m_playKeep.prefabPath.empty() &&
                 StashKey(m_playKeep.prefabPath) == StashKey(m_subjectPath))
            isSubject = true;
    }
    if (!isSubject)
    {
        if (!m_playKeep.switchRequested)
        {
            // The kept character becomes the subject first - through the SAME
            // guarded Use Selected the toolbar and "Edit Chains..." take (a
            // dirty current subject gets its confirmation).
            m_playKeep.switchRequested = true;
            const Entity pick = world.IsHandleValid(m_playKeep.carrier) ? m_playKeep.carrier.entity : root;
            RequestGuarded(GuardedRequest{ GuardKind::UseSelected, std::string(), pick, true });
            LOG_INFO("[CHAINPREVIEW] play keep: switching the subject to '%s' first guarded=%d",
                     m_playKeep.name.c_str(), m_guarded.kind != GuardKind::None ? 1 : 0);
            return;
        }
        // Still on its way (the confirmation, or the Use Selected / spawn).
        if (m_guarded.kind != GuardKind::None || m_pendingUseSelected || !m_pendingSubject.empty() ||
            m_pendingApply.valid || m_continueAfterApply.kind != GuardKind::None)
            return;
        drop("subject switch declined");
        return;
    }
    if (m_stageRoot == NullEntity || !m_stagedValid) return;   // wait for the spawn
    // ONE panel-undo step, like a preset load / a re-apply: TickStagedParams
    // commits it and its sync writes the stage (re-initialising if the kept
    // value is structural). Material only for a prefab-REF root (the Apply rule:
    // its rest pose is saved per instance, never pushed into the .ipfb).
    const std::uint64_t commitsBefore = m_commits;
    if (m_playKeep.materialOnly) m_playKeep.value.CopyMaterialInto(m_staged);
    else                         m_playKeep.value.CopyAuthoredInto(m_staged);
    ++m_playKeepApplied;
    LOG_INFO("[CHAINPREVIEW] play keep staged: target='%s' materialOnly=%d subjectSwitched=%d "
             "damping=%.4f dirty=%d commitsBefore=%llu applied=%u frame=%llu",
             m_playKeep.name.c_str(), m_playKeep.materialOnly ? 1 : 0,
             m_playKeep.switchRequested ? 1 : 0, m_staged.damping, IsStagedDirty() ? 1 : 0,
             static_cast<unsigned long long>(commitsBefore), m_playKeepApplied,
             static_cast<unsigned long long>(m_tick));
    m_playKeep = PlayKeep{};
}

// ==== The solver snapshot + the stats box ====
bool ChainPhysicsPreviewWindow::HarnessSetStats(
    const std::vector<std::pair<bool, std::uint64_t>>& schedule)
{
    if (schedule.empty()) return false;
    m_harnessStats = schedule;
    std::stable_sort(m_harnessStats.begin(), m_harnessStats.end(),
                     [](const auto& a, const auto& b) { return a.second < b.second; });
    m_harnessStatsNext = 0;
    m_harnessStatsSet  = true;
    m_showStats = m_harnessStats.front().second == 0 ? m_harnessStats.front().first : false;
    return true;
}

void ChainPhysicsPreviewWindow::TickStats(const ChainPreviewContext& ctx)
{
#if defined(WITH_HARNESS) && WITH_HARNESS
    while (m_harnessStatsNext < m_harnessStats.size() &&
           m_tick >= m_harnessStats[m_harnessStatsNext].second)
    {
        m_showStats = m_harnessStats[m_harnessStatsNext].first;
        ++m_harnessStatsNext;
        LOG_INFO("[CHAINPREVIEW] stats overlay %s from editor frame %llu (-chainpreview_stats)",
                 m_showStats ? "ON" : "OFF", static_cast<unsigned long long>(m_tick));
    }
#endif
    m_dbgValid = false;
    m_statsBox.show = false;
    m_stageStats = StageStats{};
    if (m_stageRoot == NullEntity || m_stageChain == NullEntity || !ctx.world || !ctx.renderer ||
        !ctx.world->IsHandleValid(m_stageChainH))
        return;
    World& world = *ctx.world;
    ChainPhysicsSystem* cps = ctx.renderer->GetChainPhysicsSystem();
    const ChainPhysicsComponent* live = world.GetComponent<ChainPhysicsComponent>(m_stageChain);
    if (!cps || !live) return;
    // SERIAL (this is the game half's window; the chain Update has not run
    // yet this frame): the state the LAST pass left - the pose the overlay's
    // bones (FK of the post-chain LocalPose) show too.
    m_dbgValid = cps->FillDebugSnapshot(m_stageChain, m_dbgSnap, live);

    StageStats& s = m_stageStats;
    for (const ChainGroupDef& g : live->groups) s.chains += g.enabled ? 1u : 0u;
    const ChainPhysicsSystem::PreviewLaneStats& ls = cps->GetPreviewLaneStats();
    const Anim::AnimProfile& ap = Anim::AnimProfile::Get();
    s.valid      = m_dbgValid ? 1u : 0u;
    s.strands    = m_dbgValid ? m_dbgSnap.strands : 0u;
    s.particles  = m_dbgValid ? static_cast<std::uint32_t>(m_dbgSnap.particlePos.size()) : 0u;
    s.links      = m_dbgValid ? static_cast<std::uint32_t>(m_dbgSnap.links.size()) : 0u;
    s.islands    = m_dbgValid ? m_dbgSnap.islands : 0u;
    s.springs    = m_dbgValid ? m_dbgSnap.springBones : 0u;
    s.capsules   = m_dbgValid ? m_dbgSnap.ownCapsules : 0u;
    s.interpenetrating = m_dbgValid ? m_dbgSnap.interpenetratingCapsules : 0u;
    s.interPairs = m_dbgValid ? m_dbgSnap.interpenetratingPairs : 0u;
    s.limitOnCone = m_dbgValid ? m_dbgSnap.limitOnCone : 0u;
    s.limitBeyond = m_dbgValid ? m_dbgSnap.limitBeyond : 0u;
    s.skippedCollideGroups = m_dbgValid ? m_dbgSnap.skippedCollideGroups : 0u;
    s.lodTier    = m_dbgValid ? static_cast<std::uint32_t>(m_dbgSnap.lodTier) : 0u;
    // The lane's own counters: last frame's preview passes.
    s.substeps   = ap.previewChainSubsteps;
    s.tier0      = ap.previewChainTier0;
    s.tier1      = ap.previewChainTier1;
    s.tier2      = ap.previewChainTier2;
    s.steps      = ls.steps;
    s.iterations = ls.minIterations == ~0u ? 0u : ls.minIterations;
    s.serialMs   = ap.previewChainSerialMs;
    s.parallelMs = ap.previewChainParallelMs;
    s.cpuMs      = ap.previewChainCpuMs;
    s.laneEntities = ap.previewChainEntities;
    // A/B on = two populations in the lane counters. What CAN be A's own is made
    // A's own; the rest is labelled.
    s.abOn = (m_abEnt != NullEntity) ? 1u : 0u;
    if (s.abOn)
    {
        s.tier0 = s.lodTier == 0 ? 1u : 0u;
        s.tier1 = s.lodTier == 1 ? 1u : 0u;
        s.tier2 = s.lodTier >= 2 ? 1u : 0u;
        s.iterations = static_cast<std::uint32_t>(std::max(1, live->iterations));
    }
    m_statsMaxTier2    = std::max(m_statsMaxTier2, s.tier2);
    m_statsMaxInterpen = std::max(m_statsMaxInterpen, s.interpenetrating);

    // "solve ms" is the prominent line: substeps and iterations MULTIPLY the
    // capsule-collision cost (nSub x (1 + iterations) + 1 passes per island), and
    // nothing else on screen says what a setting costs.
    m_statsBox.show = m_showStats;
    m_statsBox.lines.clear();
    if (m_showStats)
    {
        char b[160];
        std::snprintf(b, sizeof(b), "solve %.3f ms cpu   (serial %.3f  wall %.3f)%s",
                      s.cpuMs, s.serialMs, s.parallelMs, s.abOn ? "  lane A+B" : "");
        m_statsBox.lines.emplace_back(b);
        std::snprintf(b, sizeof(b), "chains %u  strands %u  particles %u  capsules %u",
                      s.chains, s.strands, s.particles, s.capsules);
        m_statsBox.lines.emplace_back(b);
        std::snprintf(b, sizeof(b), "substeps %u%s  steps %u  iterations %u  tier %u/%u/%u%s",
                      s.substeps, s.abOn ? " (A+B)" : "", s.steps, s.iterations, s.tier0,
                      s.tier1, s.tier2, s.abOn ? " (A)" : "");
        m_statsBox.lines.emplace_back(b);
        std::snprintf(b, sizeof(b), "limits %u on cone / %u beyond   interpenetrating %u   LOD-skipped %u",
                      s.limitOnCone, s.limitBeyond, s.interpenetrating, s.skippedCollideGroups);
        m_statsBox.lines.emplace_back(b);
    }
#if defined(WITH_HARNESS) && WITH_HARNESS
    if (!m_harnessSubject.empty() && (m_tick % 60u) == 0u) LogStats("periodic");
#endif
}

void ChainPhysicsPreviewWindow::LogStats(const char* when) const
{
    const StageStats& s = m_stageStats;
    LOG_INFO("[CHAINPREVIEW] stats: chains=%u strands=%u particles=%u capsules=%u links=%u "
             "islands=%u springs=%u substeps=%u steps=%u iterations=%u tier0=%u tier1=%u tier2=%u "
             "maxTier2=%u interpenetrating=%u interPairs=%u maxInterpenetrating=%u "
             "limitOnCone=%u limitBeyond=%u skippedCollideGroups=%u lodTier=%u serialMs=%.4f "
             "parallelMs=%.4f cpuMs=%.4f valid=%u show=%d lines=%u abOn=%u laneEntities=%u when=%s",
             s.chains, s.strands, s.particles, s.capsules, s.links, s.islands, s.springs,
             s.substeps, s.steps, s.iterations, s.tier0, s.tier1, s.tier2, m_statsMaxTier2,
             s.interpenetrating, s.interPairs, m_statsMaxInterpen, s.limitOnCone, s.limitBeyond,
             s.skippedCollideGroups, s.lodTier, s.serialMs, s.parallelMs, s.cpuMs, s.valid,
             m_showStats ? 1 : 0, static_cast<unsigned>(m_statsBox.lines.size()), s.abOn,
             s.laneEntities, when ? when : "");
}

} // namespace Editor
