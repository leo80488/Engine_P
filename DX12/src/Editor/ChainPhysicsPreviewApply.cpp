// ChainPhysicsPreviewApply.cpp - the Chain Physics Preview panel's COMMIT half:
// Apply to a scene character / the subject's .ipfb / every scene instance,
// Revert, the dirty dot and close confirmation, presets, and the sync read-out.
// Member functions of ChainPhysicsPreviewWindow, split from
// ChainPhysicsPreviewWindow.cpp for size; the header's side-effect discipline
// applies: Draw* functions touch panel state only, and everything that writes
// the World, a file or the undo stack runs in ProcessPendingActions (PPA).

#include "Editor/ChainPhysicsPreviewWindow.h"
#include "Editor/ImGuiFlow.h"   // flowing rows / wrapped text

#include "ECS/AnimationComponents.h"        // SkeletonComponent
#include "ECS/ECS.h"
#include "ECS/HierarchyComponents.h"        // Parent / Children / PrefabSourcePath / stage tags
#include "Editor/ChainApply.h"
#include "Editor/ChainPresetLibrary.h"
#include "Editor/ComponentTransferWindow.h" // PrefabDoc (Apply -> .ipfb)
#include "Editor/Undo/ComponentBlob.h"      // Serialize/DeserializeComponentBlob
#include "Editor/Undo/EditHistory.h"        // EditMacro
#include "Editor/Undo/ValueEditAction.h"
#include "Physics/ChainPhysicsSystem.h"
#include "Renderer/Renderer.h"
#include "Resource/ComponentSerializers.h"  // GetComponentRegistry
#include "Resource/SceneSerializer.h"      // -chainpreview_scene_save (Resource::SaveScene)
#include "Resource/SkeletonAsset.h"
#include "Scene/SceneInstanceLoader.h"      // LoadSkeletonFile (the target .ipfb's rig)
#include "System/Check.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include "imgui/imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <typeindex>

namespace Editor
{
namespace
{
    namespace CT = Editor::ComponentTransfer;

    const std::type_index kChainTi = std::type_index(typeid(ChainPhysicsComponent));

    // One spelling for comparing prefab paths (the panel's NormAssetPath rule):
    // project-relative, forward slashes, lower case.
    std::string NormPath(const std::string& p)
    {
        std::string r = Sys::RelativizeAssetPath(p);
        if (r.empty()) r = p;
        for (char& c : r)
        {
            if (c == '\\') c = '/';
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return r;
    }

    // A bare / relative harness path lands in the run's artifact directory
    // (the -chainpreview_dump rule).
    std::string HarnessPath(const std::string& p)
    {
        const bool absolute = p.size() > 1 && (p[1] == ':' || p[0] == '/' || p[0] == '\\');
        return absolute ? p : Sys::DiagnosticsDir() + "/" + p;
    }

#if defined(WITH_HARNESS) && WITH_HARNESS
    // THE WRITE-SITE rule for a harness run's .ipfb writes: inside this run's
    // -artifacts directory, and never the subject. The boot check
    // (ChainPreviewHarness::ApplyBootFlags) sees only -chainpreview_apply=prefab...;
    // a confirmation answered "apply" (and Apply-and-continue) reaches the same
    // write with an EMPTY arg, i.e. the subject's own tracked .ipfb.
    bool HarnessPrefabTargetOk(const std::string& path, const std::string& subject,
                               const std::string& harnessSubject, std::string* why)
    {
        namespace fs = std::filesystem;
        std::error_code ec1, ec2, ec3, ec4;
        const fs::path art = fs::weakly_canonical(Sys::U8Path(Sys::DiagnosticsDir()), ec1);
        const fs::path tgt = fs::weakly_canonical(Sys::U8Path(path), ec2);
        const fs::path sub = fs::weakly_canonical(Sys::U8Path(subject), ec3);
        const fs::path hsb = fs::weakly_canonical(Sys::U8Path(harnessSubject), ec4);
        const auto rel = tgt.lexically_relative(art);
        const bool inside = Sys::HasArtifactDir() && !path.empty() && !ec1 && !ec2 &&
                            !rel.empty() && *rel.begin() != fs::path("..");
        const bool isSubject = (!ec3 && !subject.empty() && tgt == sub) ||
                               (!ec4 && !harnessSubject.empty() && tgt == hsb);
        if (inside && !isSubject) return true;
        if (why)
            *why = isSubject ? "harness run: the target IS the subject's own .ipfb - refused "
                               "(a harness write never lands on a tracked asset)"
                             : "harness run: the target is outside this run's -artifacts "
                               "directory - refused (a harness write never lands on a tracked asset)";
        return false;
    }
#endif

    // The instance ROOT of a scene entity: PrefabInstanceMember names it for
    // every entity LoadPrefab created; otherwise walk Parent to the top (Use
    // Selected's rule).
    Entity RootOf(World& w, Entity e)
    {
        Entity root = e;
        if (const PrefabInstanceMember* pim = w.GetComponent<PrefabInstanceMember>(e))
            if (pim->root != NullEntity && w.IsAlive(pim->root)) root = pim->root;
        for (int guard = 0; guard < 256; ++guard)
        {
            const Parent* par = w.GetComponent<Parent>(root);
            if (!par || par->entity == NullEntity || !w.IsAlive(par->entity)) break;
            root = par->entity;
        }
        return root;
    }

    // The chain carrier under `root`: the entity with SkeletonComponent AND
    // ChainPhysicsComponent (ResolveChainTarget's hop), else the first
    // skeleton (Apply ADDS the component there). Stage entities never qualify.
    Entity CarrierOf(World& w, Entity root)
    {
        Entity firstSkel = NullEntity;
        std::vector<Entity> stack{ root };
        for (int guard = 0; !stack.empty() && guard < 4096; ++guard)
        {
            const Entity e = stack.back();
            stack.pop_back();
            if (!w.IsAlive(e) || w.HasComponent<PreviewStageTag>(e)) continue;
            if (w.HasComponent<SkeletonComponent>(e))
            {
                if (w.HasComponent<ChainPhysicsComponent>(e)) return e;
                if (firstSkel == NullEntity) firstSkel = e;
            }
            if (const Children* ch = w.GetComponent<Children>(e))
                for (auto it = ch->entities.rbegin(); it != ch->entities.rend(); ++it)
                    stack.push_back(*it);
        }
        return firstSkel;
    }

    // Serialize / deserialize ONE ChainPhysicsComponent through the registry's
    // own ChainPhysics serializer, on a scratch World (no listeners, no renderer,
    // never the main World).
    std::string BlobOf(const ChainPhysicsComponent& value)
    {
        World scratch;
        const Entity e = scratch.CreateEntity();
        ChainPhysicsComponent c;
        value.CopyAuthoredInto(c);
        scratch.AddComponent<ChainPhysicsComponent>(e, std::move(c));
        return Undo::SerializeComponentBlob(GetComponentRegistry(), scratch, e, kChainTi);
    }
    bool ValueOf(const std::string& blob, ChainPhysicsComponent& out)
    {
        World scratch;
        const Entity e = scratch.CreateEntity();
        if (!Undo::DeserializeComponentBlob(GetComponentRegistry(), scratch, e, kChainTi, blob, nullptr))
            return false;
        const ChainPhysicsComponent* c = scratch.GetComponent<ChainPhysicsComponent>(e);
        if (!c) return false;
        c->CopyAuthoredInto(out);
        return true;
    }

    std::uint32_t RigOfEntity(Renderer& renderer, World& w, Entity carrier, std::uint32_t* bones)
    {
        *bones = 0;
        const SkeletonComponent* skc = w.GetComponent<SkeletonComponent>(carrier);
        SkeletonRegistry& reg = renderer.GetSkeletonRegistry();
        if (!skc || skc->assetIndex >= reg.Count()) return 0;
        const SkeletonAsset& s = reg.Get(skc->assetIndex);
        *bones = s.boneCount;
        return ChainApply::BoneNameHash(s);
    }

    std::uint32_t CapsulesPastRig(World& w, Entity carrier, std::uint32_t bones)
    {
        const CapsuleColliderComponent* cc = w.GetComponent<CapsuleColliderComponent>(carrier);
        if (!cc || bones == 0) return 0;
        std::uint32_t n = 0;
        for (int i = 0; i < cc->count && i < CapsuleColliderComponent::MAX_CAPSULES; ++i)
            if (cc->capsules[i].enabled &&
                (cc->capsules[i].boneA >= bones || cc->capsules[i].boneB >= bones)) ++n;
        return n;
    }
}

const char* ChainPhysicsPreviewWindow::ApplyDestName(ApplyDest d)
{
    return d == ApplyDest::Scene ? "scene" : d == ApplyDest::Instances ? "instances" : "prefab";
}

bool ChainPhysicsPreviewWindow::IsStagedDirty() const
{
    // Unapplied work = the staged value differs from what loading the subject
    // gives now (an auto-detected component, never loaded, is dirty).
    return m_stagedValid && (!m_loadedValid || !m_staged.AuthoredEquals(m_loadedValue));
}

// ==== Harness setters (ChainPreviewHarness::ApplyBootFlags parses; false = ENGINE_CHECK + ignored) ====
bool ChainPhysicsPreviewWindow::HarnessAddApply(ApplyDest dest, const std::string& arg,
                                                std::uint64_t at)
{
    HarnessApply a;
    a.dest = dest;
    a.arg  = arg;
    a.at   = at;
    m_harnessApply.push_back(std::move(a));
    return true;
}

bool ChainPhysicsPreviewWindow::HarnessSetApplySeed(const std::string& src)
{
    std::error_code ec;
    if (src.empty() || !std::filesystem::exists(Sys::U8Path(src), ec)) return false;
    m_harnessApplySeed = src;
    return true;
}

bool ChainPhysicsPreviewWindow::HarnessAddPreset(const std::string& nameOrPath, int group,
                                                 std::uint64_t at)
{
    ChainPresets::Preset p;
    std::string err;
    if (!ChainPresets::Resolve(nameOrPath, p, &err)) return false;   // resolvable NOW
    HarnessPreset hp;
    hp.name  = nameOrPath;
    hp.group = group;
    hp.at    = at;
    m_harnessPresets.push_back(std::move(hp));
    return true;
}

bool ChainPhysicsPreviewWindow::HarnessSetRevertAt(const std::vector<std::uint64_t>& frames)
{
    for (std::uint64_t f : frames) if (f == 0) return false;
    m_harnessRevertAt = frames;
    std::sort(m_harnessRevertAt.begin(), m_harnessRevertAt.end());
    m_harnessRevertNext = 0;
    return !m_harnessRevertAt.empty();
}

// ==== PPA: presets + Revert (into m_staged, before TickStagedParams) ====
void ChainPhysicsPreviewWindow::TickPresets(const ChainPreviewContext& ctx)
{
    (void)ctx;
    if (m_pendingPresetRescan || (!m_userPresetsScanned && m_open))
    {
        // File I/O stays on the game half (Draw only asks for it).
        m_pendingPresetRescan = false;
        m_userPresetsScanned  = true;
        m_userPresets = ChainPresets::ListUserPresets();
    }
    if (m_stageRoot == NullEntity || !m_stagedValid) return;

    auto load = [&](const std::string& nameOrPath, int group, const char* why)
    {
        ChainPresets::Preset p;
        std::string err;
        if (!ChainPresets::Resolve(nameOrPath, p, &err))
        {
            m_applyStatus = "Preset: " + err;
            m_applyStatusError = true;
            LOG_WARNING("[CHAINPREVIEW] preset '%s' refused: %s", nameOrPath.c_str(), err.c_str());
            return;
        }
        // CLAMPED field by field on every load: an out-of-range value never reaches
        // m_staged (the reverse arm -chainpreview_preset_clamp_off lets it through).
        const ChainPresets::Report r =
            ChainPresets::Apply(p, m_staged, group, !m_harnessPresetClampOff);
        m_presetCovered.clear();
        for (const auto& kv : p.fields) m_presetCovered.push_back(kv.first);
        m_presetActive = p.name;
        ++m_presetLoads;
        const ChainGroupDef* g0 = nullptr;
        for (const ChainGroupDef& g : m_staged.groups)
            if (g.ovrEnabled && g.type == (p.target == ChainPresets::Target::Cloth
                                               ? ChainGroupType::Cloth : ChainGroupType::Chain))
            { g0 = &g; break; }
        m_applyStatus = "Preset '" + p.name + "' loaded";
        m_applyStatusError = false;
        LOG_INFO("[CHAINPREVIEW] preset: name='%s' target=%s group=%d fields=%u clamped=%u "
                 "unknown=%u nonFinite=%u groups=%u clampOff=%d builtin=%d "
                 "stagedStructural=%.4f stagedRing=%.4f stagedShear=%.4f "
                 "typeOvrStructural=%.4f typeOvrDamping=%.4f loads=%llu frame=%llu why=%s "
                 "clampedKeys=%s",
                 p.name.c_str(), ChainPresets::TargetName(p.target), group, r.fields, r.clamped,
                 r.unknown, r.nonFinite, r.groups, m_harnessPresetClampOff ? 1 : 0,
                 p.builtin ? 1 : 0, m_staged.structuralStiffness, m_staged.ringStiffness,
                 m_staged.shearStiffness, g0 ? g0->ovrStructuralStiffness : -1.f,
                 g0 ? g0->ovrDamping : -1.f, static_cast<unsigned long long>(m_presetLoads),
                 static_cast<unsigned long long>(m_tick), why,
                 r.clampedKeys.empty() ? "-" : r.clampedKeys.c_str());
    };
    for (HarnessPreset& hp : m_harnessPresets)
    {
        if (hp.done || (hp.at != 0 && m_tick < hp.at)) continue;
        hp.done = true;
        load(hp.name, hp.group, "harness");
    }
    if (m_pendingPresetSet)
    {
        m_pendingPresetSet = false;
        load(m_pendingPreset, m_presetGroup, "ui");
    }
    if (m_pendingPresetSave)
    {
        m_pendingPresetSave = false;
        const ChainPresets::Target t =
            m_presetSaveTarget == 1 ? ChainPresets::Target::Cloth : ChainPresets::Target::Chain;
        std::string source;
        ChainPresets::Preset p = ChainPresets::Capture(m_presetSaveName, t, m_staged,
                                                       m_presetGroup, &source);
        const std::string path = ChainPresets::UserPresetDir() + "/" +
                                 ChainPresets::SanitizeName(m_presetSaveName) + ".json";
        std::string err;
        const bool ok = ChainPresets::SavePresetFile(p, path, &err);
        m_applyStatus = ok ? "Saved preset '" + p.name + "' (" + source + ")" : "Save failed: " + err;
        m_applyStatusError = !ok;
        LOG_INFO("[CHAINPREVIEW] preset saved: name='%s' target=%s fields=%zu source=%s file='%s' ok=%d",
                 p.name.c_str(), ChainPresets::TargetName(t), p.fields.size(), source.c_str(),
                 path.c_str(), ok ? 1 : 0);
        m_pendingPresetRescan = true;
    }
    if (!m_pendingPresetDelete.empty())
    {
        // Only a USER preset file under UserPresetDir() - never a built-in,
        // never a path outside that folder.
        std::string name = std::move(m_pendingPresetDelete);
        m_pendingPresetDelete.clear();
        for (const ChainPresets::Preset& p : m_userPresets)
        {
            if (p.name != name || p.path.empty()) continue;
            std::error_code ec;
            const std::filesystem::path fp = Sys::U8Path(p.path);
            if (NormPath(Sys::PathToUtf8(fp.parent_path())) != NormPath(ChainPresets::UserPresetDir()))
                continue;
            std::filesystem::remove(fp, ec);
            LOG_INFO("[CHAINPREVIEW] preset deleted: '%s' (%s)", p.path.c_str(),
                     ec ? ec.message().c_str() : "ok");
        }
        m_pendingPresetRescan = true;
    }

#if defined(WITH_HARNESS) && WITH_HARNESS
    while (m_harnessRevertNext < m_harnessRevertAt.size() &&
           m_tick >= m_harnessRevertAt[m_harnessRevertNext])
    {
        ++m_harnessRevertNext;
        m_pendingRevert = true;
    }
#endif
    if (m_pendingRevert)
    {
        m_pendingRevert = false;
        DoRevert("revert");
    }
}

void ChainPhysicsPreviewWindow::DoRevert(const char* why)
{
    // Revert: m_staged back to the subject's load-time value. An edit like any
    // other: TickStagedParams records it as ONE panel-undo step (so Revert is
    // undoable) and syncs it - structurally (Invalidate) when it moves the
    // topology.
    if (!m_stagedValid || !m_loadedValid)
    {
        LOG_INFO("[CHAINPREVIEW] revert: nothing to revert to (loaded=%d) why=%s",
                 m_loadedValid ? 1 : 0, why);
        return;
    }
    const bool dirtyBefore = IsStagedDirty();
    m_loadedValue.CopyAuthoredInto(m_staged);
    ++m_reverts;
    m_presetActive.clear();
    m_presetCovered.clear();
    m_applyStatus = "Reverted to the load-time values";
    m_applyStatusError = false;
    LOG_INFO("[CHAINPREVIEW] revert: dirtyBefore=%d dirtyAfter=%d damping=%.4f "
             "g0ovrDamping=%.4f reverts=%llu frame=%llu why=%s", dirtyBefore ? 1 : 0,
             IsStagedDirty() ? 1 : 0, m_staged.damping,
             m_staged.groups.empty() ? 0.f : m_staged.groups[0].ovrDamping,
             static_cast<unsigned long long>(m_reverts),
             static_cast<unsigned long long>(m_tick), why);
}

// ==== PPA: Apply (after TickStagedParams - the live component equals m_staged) ====
void ChainPhysicsPreviewWindow::TickApply(const ChainPreviewContext& ctx)
{
    if (m_stageRoot == NullEntity || !m_stagedValid || !ctx.world || !ctx.renderer) return;
    for (HarnessApply& ha : m_harnessApply)
    {
        if (ha.done || (ha.at != 0 && m_tick < ha.at)) continue;
        ha.done = true;
        RunApply(ctx, ha.dest, ha.arg, /*confirmed=*/false, "harness");
    }
    if (m_pendingApply.valid)
    {
        const ApplyRequest r = m_pendingApply;
        m_pendingApply = ApplyRequest{};
        const std::uint64_t before = m_applies;
        RunApply(ctx, r.dest, r.arg, r.confirmed, "ui");
        if (m_closeAfterApply)
        {
            m_closeAfterApply = false;
            if (m_applies > before) m_open = false;   // applied: now close (not dirty any more)
        }
        // "Apply and continue": the guarded subject change goes ahead only if the
        // apply really happened.
        if (m_continueAfterApply.kind != GuardKind::None)
        {
            const GuardedRequest next = m_continueAfterApply;
            m_continueAfterApply = GuardedRequest{};
            if (m_applies > before)
                CommitGuarded(next);
            else
                LOG_WARNING("[CHAINPREVIEW] confirm: the apply was refused - %s NOT carried out "
                            "(the staged edits are kept)", GuardKindName(next.kind));
        }
    }
}

bool ChainPhysicsPreviewWindow::SerializeStaged(std::string& blob, bool* textRoundTrip) const
{
    blob = BlobOf(m_staged);
    if (blob.empty()) return false;
    if (textRoundTrip)
    {
        ChainPhysicsComponent back;
        *textRoundTrip = ValueOf(blob, back) && BlobOf(back) == blob;
    }
    return true;
}

std::uint32_t ChainPhysicsPreviewWindow::StageRigHash(Renderer& renderer, std::uint32_t* bones) const
{
    *bones = 0;
    SkeletonRegistry& reg = renderer.GetSkeletonRegistry();
    if (m_stageSkelAsset >= reg.Count()) return 0;
    const SkeletonAsset& s = reg.Get(m_stageSkelAsset);
    *bones = s.boneCount;
    return ChainApply::BoneNameHash(s);
}

std::uint32_t ChainPhysicsPreviewWindow::PrefabRigHash(const std::string& iskel, std::uint32_t* bones)
{
    *bones = 0;
    if (iskel.empty()) return 0;
    const std::string key = NormPath(iskel);
    if (auto it = m_rigCache.find(key); it != m_rigCache.end())
    {
        *bones = it->second.second;
        return it->second.first;
    }
    // One read per .iskel per session (the clip SOA comes along - an Apply
    // click, not a per-frame cost). Heap: SkeletonAsset is large.
    auto sk = std::make_unique<SkeletonAsset>();
    std::vector<ClipAsset> clips;
    std::uint32_t h = 0;
    if (SceneInstanceLoader::LoadSkeletonFile(iskel, *sk, clips))
    {
        h = ChainApply::BoneNameHash(*sk);
        *bones = sk->boneCount;
    }
    m_rigCache[key] = { h, *bones };
    return h;
}

// The Apply rules, shared with EditorLayer's Play LIVE target resolution and
// the Inspector's "Edit Chains...".
Entity ChainPhysicsPreviewWindow::SceneRootOf(World& world, Entity e)
{
    return RootOf(world, e);
}

// The carrier under `root` - the one the Inspector's "Edit Chains..." (or Use
// Selected on a carrier) named when it belongs to this root, else the first
// carrier of the root's DFS (a character parented under another entity, or a
// root with two carriers, must not Apply to a different carrier than the one
// the user clicked).
Entity ChainPhysicsPreviewWindow::SceneCarrierOf(World& world, Entity root) const
{
    if (world.IsHandleValid(m_sceneCarrierH))
    {
        const Entity c = m_sceneCarrierH.entity;
        if (!world.HasComponent<PreviewStageTag>(c) &&
            world.HasComponent<ChainPhysicsComponent>(c) && world.HasComponent<SkeletonComponent>(c) &&
            RootOf(world, c) == root)
            return c;
    }
    // No click: the stage carrier's counterpart (CarrierOf's first-in-DFS before
    // any spawn, and whenever the root has a single carrier).
    return CounterpartCarrier(world, root);
}

void ChainPhysicsPreviewWindow::ChainCarriersUnder(World& world, Entity root, bool stageOk,
                                                   std::vector<Entity>& out)
{
    out.clear();
    std::vector<Entity> stack{ root };
    for (int guard = 0; !stack.empty() && guard < 4096; ++guard)
    {
        const Entity e = stack.back();
        stack.pop_back();
        if (!world.IsAlive(e)) continue;
        if (!stageOk && world.HasComponent<PreviewStageTag>(e)) continue;
        if (world.HasComponent<SkeletonComponent>(e) && world.HasComponent<ChainPhysicsComponent>(e))
            out.push_back(e);
        if (const Children* ch = world.GetComponent<Children>(e))
            for (auto it = ch->entities.rbegin(); it != ch->entities.rend(); ++it)
                stack.push_back(*it);
    }
}

std::string ChainPhysicsPreviewWindow::CarrierNamePath(World& world, Entity root, Entity e)
{
    std::vector<std::string> names;
    Entity cur = e;
    for (int guard = 0; guard < 256 && cur != root; ++guard)
    {
        if (cur == NullEntity || !world.IsAlive(cur)) return std::string();
        names.push_back(world.GetName(cur));
        const Parent* par = world.GetComponent<Parent>(cur);
        cur = par ? par->entity : NullEntity;
    }
    if (cur != root) return std::string();
    std::string path;
    for (auto it = names.rbegin(); it != names.rend(); ++it)
    {
        if (!path.empty()) path += '/';
        path += *it;
    }
    return path;
}

Entity ChainPhysicsPreviewWindow::CounterpartCarrier(World& world, Entity root) const
{
    if (m_stageCarrierOrdinal < 0) return CarrierOf(world, root);
    std::vector<Entity> cs;
    ChainCarriersUnder(world, root, false, cs);
    if (cs.empty()) return CarrierOf(world, root);   // Apply ADDS the component there
    if (!m_stageCarrierPath.empty())
        for (Entity c : cs)
            if (CarrierNamePath(world, root, c) == m_stageCarrierPath) return c;
    if (static_cast<std::size_t>(m_stageCarrierOrdinal) < cs.size())
        return cs[static_cast<std::size_t>(m_stageCarrierOrdinal)];
    return cs.front();
}

Entity ChainPhysicsPreviewWindow::ResolveSceneTarget(World& world, const std::string& name,
                                                     std::string* why) const
{
    Entity pick = NullEntity;
    if (!name.empty())
    {
        for (Entity e : world.GetEntities())
            if (world.IsAlive(e) && !m_stageSet.count(e) &&
                !world.HasComponent<PreviewStageTag>(e) && world.GetName(e) == name)
            { pick = e; break; }
        if (pick == NullEntity) { if (why) *why = "no scene entity named '" + name + "'"; return NullEntity; }
    }
    else if (m_sceneTargetSet || m_subjectInPlace)
    {
        // A session that HAS a Use Selected target (an in-place subject always has)
        // writes that target or nothing - never whatever the editor has selected,
        // which can be a same-rig duplicate of the original.
        if (!world.IsHandleValid(m_sceneTargetH))
        {
            if (why) *why = "scene target lost - Use Selected again";
            return NullEntity;
        }
        pick = m_sceneTargetH.entity;
    }
    else if (m_uiSelection != NullEntity && world.IsAlive(m_uiSelection) &&
             !world.HasComponent<PreviewStageTag>(m_uiSelection))
        pick = m_uiSelection;
    if (pick == NullEntity)
    {
        if (why) *why = "no scene character - select one (or Use Selected)";
        return NullEntity;
    }
    const Entity carrier = SceneCarrierOf(world, RootOf(world, pick));   // the clicked carrier
    if (carrier == NullEntity && why) *why = "'" + world.GetName(pick) + "' has no skeleton";
    return carrier;
}

void ChainPhysicsPreviewWindow::RefreshApplyTargets(const ChainPreviewContext& ctx)
{
    m_uiDirty = IsStagedDirty();
    // Default destination: a prefab-sourced subject -> the .ipfb; in-place -> the
    // scene entity.
    if (!m_applyDestUser) m_applyDest = m_subjectInPlace ? ApplyDest::Scene : ApplyDest::Prefab;
    if (!ctx.world || m_stageRoot == NullEntity) { m_uiSceneTargetValid = false; return; }
    World& world = *ctx.world;
    const Entity t = ResolveSceneTarget(world, std::string(), nullptr);
    m_uiSceneTargetValid = t != NullEntity;
    // The Apply button names its target, or says it is lost.
    m_uiSceneTargetLost = (m_sceneTargetSet || m_subjectInPlace) && !world.IsHandleValid(m_sceneTargetH);
    if (m_uiSceneTargetValid)
    {
        const Entity root = RootOf(world, t);
        m_uiSceneTargetName = world.GetName(root);
        m_uiSceneTargetPrefabRef = world.HasComponent<PrefabSourcePath>(root);
    }
    if ((m_tick % 30u) == 0u || m_uiInstanceCount == 0)
    {
        m_uiInstanceCount = 0;
        const std::string want = NormPath(m_subjectPath);
        if (auto* pp = world.GetPool<PrefabSourcePath>())
        {
            auto& ents = pp->Entities();
            auto& data = pp->Data();
            for (std::size_t i = 0; i < ents.size(); ++i)
                if (!m_stageSet.count(ents[i]) && !data[i].path.empty() &&
                    NormPath(data[i].path) == want) ++m_uiInstanceCount;
        }
    }
}

void ChainPhysicsPreviewWindow::RunApply(const ChainPreviewContext& ctx, ApplyDest dest,
                                         const std::string& arg, bool confirmed, const char* why)
{
    World&    world    = *ctx.world;
    Renderer& renderer = *ctx.renderer;
    ChainPhysicsSystem* cps = renderer.GetChainPhysicsSystem();

    // Everything the one "[CHAINPREVIEW] apply:" line reports.
    std::uint32_t written = 0, targets = 0, rigMismatch = 0, capsuleOob = 0, prefabRef = 0;
    std::uint32_t invalidated = 0, subjBones = 0, targetBones = 0;
    // Material-only writes / targets whose rest pose was left as it was; whether
    // the scene target came from Use Selected (1) or the live selection / a name
    // (0).
    std::uint32_t materialOnlyWrites = 0, restPoseKept = 0, fromUseSelected = 0;
    std::uint32_t subjRig = StageRigHash(renderer, &subjBones), targetRig = 0;
    bool rigKnown = true, persists = false, undoPushed = false, bak = false, sizeOk = false;
    bool roundTrip = false, textRoundTrip = false, liveEquals = false, refused = false;
    std::uint32_t textLength = 0, dataSize = 0;
    int node = -1;
    float reloadDamping = -1.f;
    std::string target, reason;
    int carrierOrdinal = -1;   // of the first written carrier

    auto emit = [&]()
    {
        if (refused) ++m_applyRefusals; else ++m_applies;
        m_applyStatus = refused ? "Apply (" + std::string(ApplyDestName(dest)) + ") refused: " + reason
                                : "Applied to " + std::string(ApplyDestName(dest)) + ": " + target +
                                      (persists ? "" : " (session only - not saved)");
        m_applyStatusError = refused;
        const std::size_t depth = ctx.undoCount ? ctx.undoCount() : 0;
        char line[1400];
        std::snprintf(line, sizeof(line),
            "[CHAINPREVIEW] apply: dest=%s written=%u targets=%u refused=%d rigMismatch=%u "
            "rigKnown=%d subjectRig=%08x targetRig=%08x subjectBones=%u targetBones=%u "
            "capsuleOob=%u prefabRef=%u persists=%d undoPushed=%d undoDepth=%zu "
            "invalidated=%u bak=%d sizeOk=%d roundTrip=%d textRoundTrip=%d textLength=%u "
            "dataSize=%u node=%d stagedDamping=%.4f reloadDamping=%.4f liveEquals=%d "
            "confirmed=%d applies=%llu frame=%llu materialOnly=%u restPoseKept=%u "
            "fromUseSelected=%u targetKeeps=%u targetReresolves=%u targetLost=%d "
            "carrierOrdinal=%d stageCarrierOrdinal=%d carrierMismatches=%u "
            "why=%s target='%s' reason='%s'",
            ApplyDestName(dest), written, targets, refused ? 1 : 0, rigMismatch,
            rigKnown ? 1 : 0, subjRig, targetRig, subjBones, targetBones, capsuleOob,
            prefabRef, persists ? 1 : 0, undoPushed ? 1 : 0, depth, invalidated, bak ? 1 : 0,
            sizeOk ? 1 : 0, roundTrip ? 1 : 0, textRoundTrip ? 1 : 0, textLength, dataSize,
            node, m_staged.damping, reloadDamping, liveEquals ? 1 : 0, confirmed ? 1 : 0,
            static_cast<unsigned long long>(m_applies), static_cast<unsigned long long>(m_tick),
            materialOnlyWrites, restPoseKept, fromUseSelected, m_sceneTargetKeeps,
            m_sceneTargetReresolves, m_sceneTargetLost ? 1 : 0,
            carrierOrdinal, m_stageCarrierOrdinal, m_applyCarrierMismatch,
            why, target.c_str(), reason.c_str());
        // A refusal is a WARNING (a harness run that asked for this write and
        // did not get it is not silent); an apply is INFO.
        if (refused) LOG_WARNING("%s", line);
        else         LOG_INFO("%s", line);
    };
    auto refuse = [&](const std::string& r) { refused = true; reason = r; emit(); };

    // The stage exists only in Edit mode, and a scene write in Play would be
    // reverted by Stop's AdoptState anyway.
    if (!ctx.editing) return refuse("Play mode - apply after Stop");
    std::string blob;
    if (!SerializeStaged(blob, &textRoundTrip)) return refuse("the ChainPhysics serializer produced nothing");
    if (subjRig == 0) rigKnown = false;

    // A rig mismatch needs a confirmation: ChainPhysics names bones, and a
    // different skeleton silently drops or re-targets them.
    auto needConfirm = [&](const std::string& what)
    {
        if (confirmed) return false;
        m_confirmApply = ApplyRequest{ dest, arg, true, true };
        if (std::strcmp(why, "ui") == 0)
        {
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                          "%s\n\nThe subject's skeleton (%u bones, rig %08x) is not the "
                          "target's (%u bones, rig %08x%s).\nChain groups name bones; "
                          "names the target lacks are dropped at load.\n\nApply anyway?",
                          what.c_str(), subjBones, subjRig, targetBones, targetRig,
                          rigKnown ? "" : " - UNKNOWN");
            m_confirmApplyText = buf;
            m_confirmApplyOpen = true;
        }
        refuse("rig mismatch - " + what + " (needs confirmation)");
        return true;
    };

    const ChainPhysicsComponent& staged = m_staged;
    if (dest == ApplyDest::Prefab)
    {
        // ---- (b) the .ipfb ----
        if (m_subjectInPlace && arg.empty())
            return refuse("an in-place subject has no .ipfb - Apply to the scene entity");
        const std::string path = arg.empty() ? m_subjectPath : HarnessPath(arg);
        target = path;
#if defined(WITH_HARNESS) && WITH_HARNESS
        // Guarded HERE, whatever route asked - a harness apply entry, a confirmation
        // answered "apply", or Apply and continue - and BEFORE the seed copy (which
        // would otherwise overwrite the target first and leave the seed in its .bak).
        if (!m_harnessSubject.empty())
        {
            std::string why2;
            if (!HarnessPrefabTargetOk(path, m_subjectPath, m_harnessSubject, &why2))
                return refuse(why2);
        }
        // The seed goes only onto an EXPLICIT, validated prefab:<arg> target.
        if (!m_harnessApplySeed.empty() && !m_harnessSeedDone && !arg.empty() &&
            !m_harnessSubject.empty())
        {
            // The pack's "copy of a fixture": the tracked asset is never the target.
            m_harnessSeedDone = true;
            std::error_code ec;
            std::filesystem::create_directories(Sys::U8Path(path).parent_path(), ec);
            ec.clear();
            std::filesystem::copy_file(Sys::U8Path(m_harnessApplySeed), Sys::U8Path(path),
                                       std::filesystem::copy_options::overwrite_existing, ec);
            LOG_INFO("[CHAINPREVIEW] apply seed: from='%s' to='%s' ok=%d%s%s",
                     m_harnessApplySeed.c_str(), path.c_str(), ec ? 0 : 1,
                     ec ? " err=" : "", ec ? ec.message().c_str() : "");
        }
#endif
        CT::PrefabDoc doc;
        std::string err;
        if (!CT::LoadPrefabDoc(path, doc, &err)) return refuse(err);
        const std::string carrierName = world.GetName(m_stageChain);
        node = ChainApply::FindChainNode(doc, carrierName, &err);
        if (node < 0) return refuse(err);
        targets = 1;
        // The target's rig: SceneRef -> .iscn "K file=" -> .iskel (UNKNOWN
        // when any link is missing - which also needs the confirmation).
        const std::string iskel = ChainApply::SkeletonPathOfNode(doc, node);
        targetRig = PrefabRigHash(iskel, &targetBones);
        if (targetRig == 0) rigKnown = false;
        if (!rigKnown || targetRig != subjRig) rigMismatch = 1;
        for (const auto& ab : ChainApply::CapsuleBonesOfNode(doc, node))
            if (targetBones != 0 && (ab.first >= targetBones || ab.second >= targetBones)) ++capsuleOob;
        if (capsuleOob)
            LOG_WARNING("[CHAINPREVIEW] apply: %u capsule(s) on '%s' node %d point past its "
                        "%u-bone skeleton", capsuleOob, path.c_str(), node, targetBones);
        if (rigMismatch && needConfirm("'" + path + "'")) return;

        const ChainApply::PrefabWrite w = ChainApply::WriteChainBlob(path, carrierName, blob);
        if (!w.ok) return refuse(w.err.empty() ? "the write failed" : w.err);
        written    = 1;
        bak        = w.bak;
        sizeOk     = w.sizeOk;
        roundTrip  = w.roundTrip;
        textLength = w.textLength;
        dataSize   = w.dataSize;
        persists   = true;
        // Read the field back from what is ON DISK (not the blob we meant).
        {
            CT::PrefabDoc again;
            ChainPhysicsComponent back;
            if (CT::LoadPrefabDoc(path, again, &err) &&
                ValueOf(CT::GetNodeComponentBlob(again, static_cast<std::size_t>(node), "ChainPhysics"), back))
                reloadDamping = back.damping;
        }
        // The subject's OWN source moved: loading it now yields the staged
        // value, so that is the new load-time value (dirty clears).
        if (!m_subjectInPlace && NormPath(path) == NormPath(m_subjectPath))
        {
            staged.CopyAuthoredInto(m_loadedValue);
            m_loadedValid = true;
            m_loadedMoved = true;
            m_loadedBlob  = blob;
        }
        return emit();
    }

    // ---- (a) scene entity / (c) all instances ----
    if (!ctx.undoEnabled) return refuse("undo is Edit-mode only");
    std::vector<Entity> dsts;
    if (dest == ApplyDest::Scene)
    {
        std::string err;
        const Entity c = ResolveSceneTarget(world, arg, &err);
        if (c == NullEntity) return refuse(err);
        // The staged values were read from the stage carrier: writing them onto a
        // DIFFERENT carrier of the same root (a two-carrier character, the other one
        // clicked) is a silent cross-write the rig-hash confirmation cannot see on a
        // same-rig pair.
        if (world.HasComponent<ChainPhysicsComponent>(c))
        {
            const Entity want = CounterpartCarrier(world, RootOf(world, c));
            if (want != NullEntity && want != c)
            {
                ++m_applyCarrierMismatch;
                return refuse("carrier '" + world.GetName(c) + "' is not the one the stage was "
                              "spawned from ('" + m_stageCarrierPath + "') - Use Selected on it first");
            }
        }
        dsts.push_back(c);
        target = world.GetName(RootOf(world, c));
        fromUseSelected = (arg.empty() && world.IsHandleValid(m_sceneTargetH) &&
                           SceneCarrierOf(world, RootOf(world, m_sceneTargetH.entity)) == c) ? 1u : 0u;
    }
    else
    {
        const std::string want = NormPath(arg.empty() ? m_subjectPath : arg);
        target = arg.empty() ? m_subjectPath : arg;
        if (auto* pp = world.GetPool<PrefabSourcePath>())
        {
            std::vector<Entity> roots;
            auto& ents = pp->Entities();
            auto& data = pp->Data();
            for (std::size_t i = 0; i < ents.size(); ++i)
                if (!m_stageSet.count(ents[i]) && !data[i].path.empty() &&
                    NormPath(data[i].path) == want)
                    roots.push_back(ents[i]);
            for (Entity r : roots)   // the stage carrier's counterpart
                if (const Entity c = CounterpartCarrier(world, r); c != NullEntity) dsts.push_back(c);
        }
        if (dsts.empty()) return refuse("no scene instance of '" + target + "'");
    }
    targets = static_cast<std::uint32_t>(dsts.size());
    {
        std::vector<Entity> cs;
        ChainCarriersUnder(world, RootOf(world, dsts.front()), false, cs);
        for (std::size_t i = 0; i < cs.size(); ++i)
            if (cs[i] == dsts.front()) carrierOrdinal = static_cast<int>(i);
    }
    for (Entity d : dsts)
    {
        std::uint32_t tb = 0;
        const std::uint32_t tr = RigOfEntity(renderer, world, d, &tb);
        if (targetRig == 0) { targetRig = tr; targetBones = tb; }
        if (tr == 0) rigKnown = false;
        if (tr == 0 || tr != subjRig) ++rigMismatch;
        capsuleOob += CapsulesPastRig(world, d, tb);
        if (world.HasComponent<PrefabSourcePath>(RootOf(world, d))) ++prefabRef;
    }
    if (capsuleOob)
        LOG_WARNING("[CHAINPREVIEW] apply: %u capsule(s) on the %s target(s) point past their "
                    "skeleton", capsuleOob, ApplyDestName(dest));
    if (rigMismatch && needConfirm(std::to_string(rigMismatch) + " of " +
                                   std::to_string(targets) + " target(s)")) return;

    // FIELD BY FIELD into the target (its own rest-pose name cache survives), ONE
    // global undo entry for the gesture, and Invalidate: the solver keeps
    // per-entity data built from the PREVIOUS config ("copy did nothing"
    // otherwise). The restore closures re-Invalidate on undo / redo for the same
    // reason.
    Renderer* rp = ctx.renderer;
    auto restoreFn = [rp](std::shared_ptr<void> holder) -> Undo::RestoreFn
    {
        return [holder, rp](World& w, Entity e)
        {
            if (holder) w.RestoreComponentByType(e, kChainTi, holder);
            else        w.RemoveComponentByType(e, kChainTi);
            if (rp) if (ChainPhysicsSystem* c = rp->GetChainPhysicsSystem()) c->Invalidate(e);
        };
    };
    auto macro = std::make_unique<Undo::EditMacro>(
        dest == ApplyDest::Scene ? "Chain params from preview"
                                 : "Chain params from preview (all instances)");
    std::uint32_t equal = 0;
    for (Entity d : dsts)
    {
        // A prefab-REF target gets the PHYSICS MATERIAL only; its restPose /
        // restPoseEnabled / restPoseWeight are left as they were. SceneSerializer
        // saves an instance's rest pose per instance (a CPR line) and replays it on
        // load, pinning the instance against later .ipfb pose edits, while the
        // material is saved only as the reference - so writing the staged rest pose
        // would half-persist an Apply the panel reports as session-only. An in-place
        // target (no PrefabSourcePath) is saved whole with the scene and gets the
        // whole authored block. HarnessSetApplyRestPose is the reverse arm.
        const bool materialOnly = world.HasComponent<PrefabSourcePath>(RootOf(world, d)) &&
                                  !m_harnessApplyRestPose;
        std::shared_ptr<void> before = world.CloneComponentByType(d, kChainTi);
        if (ChainPhysicsComponent* live = world.GetComponent<ChainPhysicsComponent>(d))
        {
            ChainPhysicsComponent restBefore;
            live->CopyAuthoredInto(restBefore);
            if (materialOnly) staged.CopyMaterialInto(*live);
            else              staged.CopyAuthoredInto(*live);
            // "kept" = the target's rest-pose block is what it was before.
            ChainPhysicsComponent restAfter;
            live->CopyAuthoredInto(restAfter);
            restAfter.CopyMaterialInto(restBefore);   // equal materials -> compares the rest blocks
            if (restBefore.AuthoredEquals(restAfter)) ++restPoseKept;
        }
        else
        {
            ChainPhysicsComponent fresh;
            if (materialOnly) staged.CopyMaterialInto(fresh);
            else              staged.CopyAuthoredInto(fresh);
            world.AddComponent<ChainPhysicsComponent>(d, std::move(fresh));
        }
        if (materialOnly) ++materialOnlyWrites;
        std::shared_ptr<void> after = world.CloneComponentByType(d, kChainTi);
        macro->Add(std::make_unique<Undo::ValueEditAction>(
            world.MakeHandle(d), restoreFn(std::move(before)), restoreFn(std::move(after)),
            "Chain params from preview"));
        if (cps) { cps->Invalidate(d); ++invalidated; }
        if (const ChainPhysicsComponent* c = world.GetComponent<ChainPhysicsComponent>(d))
            if (materialOnly ? c->MaterialEquals(staged) : c->AuthoredEquals(staged)) ++equal;
        ++written;
    }
    liveEquals = equal == written;
    if (ctx.pushEdit && !macro->Empty())
    {
        ctx.pushEdit(std::move(macro));
        undoPushed = true;
    }
    // Persistence: a prefab-REF instance is saved as a reference only, so a
    // per-instance write survives only this session; "All instances" is
    // session-only by definition. An in-place character (no PrefabSourcePath) is
    // saved with the scene.
    persists = dest == ApplyDest::Scene && prefabRef == 0;
    if (persists && m_subjectInPlace && world.IsHandleValid(m_sceneTargetH) &&
        SceneCarrierOf(world, RootOf(world, m_sceneTargetH.entity)) == dsts.front())
    {
        // The in-place subject's ORIGINAL now holds the staged value.
        staged.CopyAuthoredInto(m_loadedValue);
        m_loadedValid = true;
        m_loadedMoved = true;
        m_loadedBlob  = blob;
    }
    reloadDamping = world.GetComponent<ChainPhysicsComponent>(dsts.front())
                        ? world.GetComponent<ChainPhysicsComponent>(dsts.front())->damping : -1.f;
    emit();
}

// ==== -chainpreview_scene_save ====
// The observable for "what does a Ctrl+S persist after an Apply to prefab-ref
// instances": the World is written through Resource::SaveScene (the editor's
// own writer) into -artifacts (ChainPreviewHarness::ApplyBootFlags refuses an
// absolute or '..' name at boot), then the file's CPR lines - SceneSerializer's
// per-instance rest-pose overrides - are counted, with how many carry the
// STAGED rest-pose weight.
bool ChainPhysicsPreviewWindow::HarnessAddSceneSave(const std::string& path, std::uint64_t at)
{
    if (path.empty() || at == 0) return false;
    m_harnessSceneSaves.push_back(SceneSave{ path, at, false });
    return true;
}

void ChainPhysicsPreviewWindow::TickSceneSave(const ChainPreviewContext& ctx)
{
#if defined(WITH_HARNESS) && WITH_HARNESS
    if (!ctx.world || !ctx.editing) return;
    for (SceneSave& s : m_harnessSceneSaves)
    {
        if (s.done || m_tick < s.at) continue;
        s.done = true;
        const std::string path = HarnessPath(s.path);
        const bool ok = Resource::SaveScene(*ctx.world, path, "Scene");
        std::uint32_t cpr = 0, cprStaged = 0, cprDefaultW = 0;
        std::ostringstream wt;
        wt << m_staged.restPoseWeight;   // the CPR writer's own stream formatting
        const std::string stagedTok = " rpW=" + wt.str() + " ";
        std::size_t bytes = 0;
        if (ok)
        {
            std::ifstream f(Sys::U8Path(path), std::ios::binary);
            const std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            bytes = all.size();
            std::size_t p = 0;
            while ((p = all.find("\nCPR idx=", p)) != std::string::npos)
            {
                ++p;
                const std::size_t e = all.find('\n', p);
                const std::string line = all.substr(p, e == std::string::npos ? std::string::npos : e - p) + " ";
                ++cpr;
                if (line.find(stagedTok) != std::string::npos) ++cprStaged;
                if (line.find(" rpW=1 ") != std::string::npos) ++cprDefaultW;
            }
        }
        LOG_INFO("[CHAINPREVIEW] scene save: ok=%d bytes=%zu cprLines=%u cprStagedWeight=%u "
                 "cprWeight1=%u stagedRpW=%.4f applies=%llu frame=%llu file='%s'", ok ? 1 : 0,
                 bytes, cpr, cprStaged, cprDefaultW, m_staged.restPoseWeight,
                 static_cast<unsigned long long>(m_applies),
                 static_cast<unsigned long long>(m_tick), path.c_str());
    }
#else
    (void)ctx;
#endif
}

// ==== The sync read-out (boneNameResolves stays 0 after settle) ====
void ChainPhysicsPreviewWindow::TickSyncProbe(const ChainPreviewContext& ctx)
{
    if (m_syncProbeSpawn != m_spawnCount)
    {
        m_syncProbeSpawn  = m_spawnCount;
        m_boneNameResolves = m_syncProbeFrames = 0;
        m_syncProbeEntries = 0;
    }
    if (m_stageRoot == NullEntity || !ctx.world || m_tick < m_spawnTick + kSyncSettleFrames) return;
    World& world = *ctx.world;
    if (!world.IsHandleValid(m_stageChainH)) return;
    const ChainPhysicsComponent* live = world.GetComponent<ChainPhysicsComponent>(m_stageChain);
    const SkeletonComponent*     skc  = world.GetComponent<SkeletonComponent>(m_stageChain);
    if (!live || !skc) return;
    ++m_syncProbeFrames;
    m_syncProbeEntries = static_cast<std::uint32_t>(live->restPose.size());
    // ApplyRestPose re-resolves an entry's bone name exactly when this cache test
    // is stale (its early-outs are mirrored). Taken at the end of PPA, after every
    // panel write and before this frame's ApplyRestPose, so each stale entry IS
    // one re-resolve. The one writer's field-wise copy keeps the cache; a
    // whole-struct copy (`*live = m_staged`) makes every entry stale every frame.
    if (!live->restPoseEnabled || live->restPose.empty() || live->restPoseWeight <= 0.f) return;
    for (const BoneRestPose& r : live->restPose)
    {
        if (!r.enabled || r.boneName[0] == '\0') continue;
        if (r.resolvedSkel != skc->assetIndex || std::strcmp(r.resolvedName, r.boneName) != 0)
            ++m_boneNameResolves;
    }
}

void ChainPhysicsPreviewWindow::LogSyncProbe(const char* when) const
{
    LOG_INFO("[CHAINPREVIEW] sync: boneNameResolves=%llu frames=%llu restPoseEntries=%u "
             "syncs=%llu wholesale=%d settle=%llu when=%s",
             static_cast<unsigned long long>(m_boneNameResolves),
             static_cast<unsigned long long>(m_syncProbeFrames), m_syncProbeEntries,
             static_cast<unsigned long long>(m_syncs), m_harnessSyncWholesale ? 1 : 0,
             static_cast<unsigned long long>(kSyncSettleFrames), when ? when : "");
}

// ==== UI half (panel state only) ====
std::string ChainPhysicsPreviewWindow::ApplyButtonLabel() const
{
    const bool sceneLost = m_applyDest == ApplyDest::Scene && m_uiSceneTargetLost;
    char lbl[160];
    if (m_applyDest == ApplyDest::Scene)
        std::snprintf(lbl, sizeof(lbl), "Apply (scene: %s)",
                      sceneLost ? "target lost" : m_uiSceneTargetValid ? m_uiSceneTargetName.c_str()
                                                                        : "select a character");
    else if (m_applyDest == ApplyDest::Prefab)
    {
        // The .ipfb destination names the file.
        const std::size_t cut = m_subjectPath.find_last_of("/\\");
        std::snprintf(lbl, sizeof(lbl), "Apply (prefab: %s)",
                      cut == std::string::npos ? m_subjectPath.c_str()
                                               : m_subjectPath.c_str() + cut + 1);
    }
    else
        std::snprintf(lbl, sizeof(lbl), "Apply (all %u instances)", m_uiInstanceCount);
    return lbl;
}

// Apply, prominent at the top of Parameters (the same request as the
// toolbar's button): there is no continuous auto-apply, so the one commit sits
// where the edits are.
void ChainPhysicsPreviewWindow::DrawParamsApplyRow()
{
    const bool haveStage = m_stageRoot != NullEntity && m_stagedValid;
    const bool sceneLost = m_applyDest == ApplyDest::Scene && m_uiSceneTargetLost;
    ImGui::BeginDisabled(!haveStage || m_suspendedForPlay || sceneLost || !m_uiDirty);
    if (m_uiDirty)
    {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.70f, 0.42f, 0.10f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.82f, 0.52f, 0.16f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.90f, 0.60f, 0.20f, 1.f));
    }
    const std::string lbl = ApplyButtonLabel() + "##cpv_params_apply";
    if (ImGui::Button(lbl.c_str()))
        m_pendingApply = ApplyRequest{ m_applyDest, std::string(), false, true };
    if (m_uiDirty) ImGui::PopStyleColor(3);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(m_uiDirty
            ? "Write the staged parameters to this destination (the toolbar's Apply;\n"
              "its arrow picks another destination). Edits here are STAGED until then."
            : "Nothing to apply: the staged parameters equal what loading the subject gives.");
    // The status flows under the button when the column is narrow.
    const char* state = m_uiDirty ? "* unapplied edits" : "applied / unchanged";
    EditorUI::Flow::SameLineIfFits(EditorUI::Flow::TextW(state));
    ImGui::TextDisabled("%s", state);
}

void ChainPhysicsPreviewWindow::DrawApplyUi()
{
    const bool haveStage = m_stageRoot != NullEntity && m_stagedValid;
    const bool play      = m_suspendedForPlay;
    // ---- The dirty dot ----
    if (m_uiDirty)
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float  r = ImGui::GetTextLineHeight() * 0.3f;
        ImGui::GetWindowDrawList()->AddCircleFilled(
            ImVec2(p.x + r, p.y + ImGui::GetFrameHeight() * 0.5f), r, IM_COL32(255, 170, 60, 255));
        ImGui::Dummy(ImVec2(2.f * r + 2.f, ImGui::GetFrameHeight()));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Unapplied edits: the staged parameters differ from what "
                              "loading the subject gives.");
        ImGui::SameLine();
    }
    // The Scene destination NAMES its target on the button, and a lost one
    // disables it rather than falling back to the selection.
    const bool sceneLost = m_applyDest == ApplyDest::Scene && m_uiSceneTargetLost;
    ImGui::BeginDisabled(!haveStage || play || sceneLost);
    if (ImGui::Button(ApplyButtonLabel().c_str()))
        m_pendingApply = ApplyRequest{ m_applyDest, std::string(), false, true };
    if (sceneLost && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("The scene character this subject was taken from is gone (deleted, or\n"
                          "a different scene was loaded). Select it and press Use Selected again.");
    ImGui::SameLine(0.f, 2.f);
    if (ImGui::ArrowButton("##cpv_apply_menu", ImGuiDir_Down)) ImGui::OpenPopup("##cpv_apply");
    ImGui::EndDisabled();
    if (play && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Suspended during Play: the stage is rebuilt after Stop.\n"
                          "Scene edits made in Play are reverted by Stop anyway.");
    if (ImGui::BeginPopup("##cpv_apply"))
    {
        // (a) scene entity
        std::string sceneLbl = m_uiSceneTargetValid
            ? "Scene entity: " + m_uiSceneTargetName +
                  (m_uiSceneTargetPrefabRef ? "  (prefab ref - material only, NOT saved)" : "")
            : m_uiSceneTargetLost ? std::string("Scene entity (target lost - Use Selected again)")
                                  : std::string("Scene entity (select a character)");
        ImGui::BeginDisabled(!m_uiSceneTargetValid || play);
        if (ImGui::MenuItem(sceneLbl.c_str()))
            m_pendingApply = ApplyRequest{ ApplyDest::Scene, std::string(), false, true };
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(play ? "Disabled during Play: undo is Edit-only and Stop restores\n"
                                     "the pre-Play scene. Apply to the .ipfb instead."
                                   : m_uiSceneTargetPrefabRef
                                     ? "This character is a prefab INSTANCE: the scene saves only a\n"
                                       "reference to its .ipfb, so the PHYSICS MATERIAL written here\n"
                                       "lasts until the scene is reloaded (Apply to the .ipfb to make it\n"
                                       "stick). Its rest pose is left alone: the scene saves that per\n"
                                       "instance, and a staged rest pose would pin it there."
                                     : "Write the staged values into this character (one undo entry).");
        // (b) the .ipfb
        ImGui::BeginDisabled(m_subjectInPlace || play);
        if (ImGui::MenuItem(".ipfb (the subject's prefab)"))
            m_pendingApply = ApplyRequest{ ApplyDest::Prefab, std::string(), false, true };
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(m_subjectInPlace
                ? "The subject is an in-place character - there is no .ipfb to write back."
                : "Rewrite the ChainPhysics block of %s (a .bak is kept). Every\n"
                  "instance picks it up at its next load.", m_subjectPath.c_str());
        // (c) all instances - session only
        char inst[128];
        std::snprintf(inst, sizeof(inst), "All instances (%u) - session only, not saved",
                      m_uiInstanceCount);
        ImGui::BeginDisabled(m_uiInstanceCount == 0 || play);
        if (ImGui::MenuItem(inst))
            m_pendingApply = ApplyRequest{ ApplyDest::Instances, std::string(), false, true };
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("session only - not saved; use .ipfb to make it stick.\n"
                              "Scene instances are saved as references to the prefab.");
        ImGui::Separator();
        ImGui::TextDisabled("Default:");
        int d = static_cast<int>(m_applyDest);
        const char* names[] = { "scene", "prefab", "instances" };
        for (int i = 0; i < 3; ++i)
        {
            ImGui::SameLine();
            if (ImGui::RadioButton(names[i], d == i))
            { m_applyDest = static_cast<ApplyDest>(i); m_applyDestUser = true; }
        }
        ImGui::EndPopup();
    }
    // Revert and the (unbounded) status flow; the status wraps in whatever the
    // line has left, or on a line of its own.
    EditorUI::Flow::SameLineIfFits(EditorUI::Flow::ButtonW("Revert"));
    ImGui::BeginDisabled(!haveStage || !m_loadedValid || !m_uiDirty);
    if (ImGui::Button("Revert")) m_pendingRevert = true;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Put the staged parameters back to the subject's load-time values\n"
                          "(one panel-undo step - Ctrl+Z brings the edits back).");
    if (!m_applyStatus.empty())
    {
        EditorUI::Flow::SameLineIfFits(std::min(EditorUI::Flow::TextW(m_applyStatus.c_str()), 220.f));
        EditorUI::Flow::ColoredWrapped(m_applyStatusError ? ImVec4(1.f, 0.45f, 0.35f, 1.f)
                                                          : ImVec4(0.6f, 0.85f, 0.6f, 1.f),
                                       "%s", m_applyStatus.c_str());
    }
}

void ChainPhysicsPreviewWindow::DrawPresetUi()
{
    if (!m_userPresetsScanned) m_pendingPresetRescan = true;   // PPA reads the folder
    // The combo and its three buttons flow.
    namespace Flow = EditorUI::Flow;
    std::string preview = m_presetPick.empty() ? std::string("(pick a preset)") : m_presetPick;
    ImGui::SetNextItemWidth(std::max(80.f, std::min(150.f, ImGui::GetContentRegionAvail().x)));
    if (ImGui::BeginCombo("##cpv_preset", preview.c_str()))
    {
        ImGui::TextDisabled("built-in");
        for (const ChainPresets::Preset& p : ChainPresets::Builtins())
        {
            char l[96];
            std::snprintf(l, sizeof(l), "%s  [%s]", p.name.c_str(), ChainPresets::TargetName(p.target));
            if (ImGui::Selectable(l, m_presetPick == p.name)) m_presetPick = p.name;
        }
        if (!m_userPresets.empty())
        {
            ImGui::Separator();
            ImGui::TextDisabled("user");
            for (const ChainPresets::Preset& p : m_userPresets)
            {
                char l[160];
                std::snprintf(l, sizeof(l), "%s  [%s]##u", p.name.c_str(),
                              ChainPresets::TargetName(p.target));
                if (ImGui::Selectable(l, m_presetPick == p.name)) m_presetPick = p.name;
            }
        }
        if (ImGui::Selectable("(rescan the user folder)")) m_pendingPresetRescan = true;
        ImGui::EndCombo();
    }
    Flow::SameLineIfFits(Flow::ButtonW("Load"));
    ImGui::BeginDisabled(m_presetPick.empty());
    if (ImGui::SmallButton("Load")) { m_pendingPreset = m_presetPick; m_pendingPresetSet = true; }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Writes the preset's fields (and only those) into the staged copy:\n"
                          "the type globals + every group of that type that overrides them.\n"
                          "Every value is clamped (PBD saturation, solver limits). One undo step.");
    Flow::SameLineIfFits(Flow::ButtonW("Save as..."));
    if (ImGui::SmallButton("Save as...")) ImGui::OpenPopup("##cpv_preset_save");
    Flow::SameLineIfFits(Flow::ButtonW("Delete"));
    bool isUser = false;
    for (const ChainPresets::Preset& p : m_userPresets) isUser |= (p.name == m_presetPick);
    ImGui::BeginDisabled(!isUser);
    if (ImGui::SmallButton("Delete")) m_pendingPresetDelete = m_presetPick;
    ImGui::EndDisabled();
    // scope: every group of the preset's type, or one group (a Cloth group = its piece)
    std::string scope = m_presetGroup < 0 ? std::string("all groups of its type")
                                          : "group " + std::to_string(m_presetGroup);
    if (m_presetGroup >= 0 && m_presetGroup < static_cast<int>(m_staged.groups.size()))
        scope += std::string(" (") + m_staged.groups[static_cast<std::size_t>(m_presetGroup)].name + ")";
    ImGui::SetNextItemWidth(Flow::FitWidth(200.f, "Preset scope"));
    if (ImGui::BeginCombo("Preset scope", scope.c_str()))
    {
        if (ImGui::Selectable("all groups of its type", m_presetGroup < 0)) m_presetGroup = -1;
        for (int gi = 0; gi < static_cast<int>(m_staged.groups.size()); ++gi)
        {
            char l[96];
            std::snprintf(l, sizeof(l), "%d: %s##ps%d", gi,
                          m_staged.groups[static_cast<std::size_t>(gi)].name, gi);
            if (ImGui::Selectable(l, m_presetGroup == gi)) m_presetGroup = gi;
        }
        ImGui::EndCombo();
    }
    if (ImGui::BeginPopup("##cpv_preset_save"))
    {
        ImGui::InputText("Name", m_presetSaveName, sizeof(m_presetSaveName));
        ImGui::RadioButton("Chain", &m_presetSaveTarget, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Cloth", &m_presetSaveTarget, 1);
        ImGui::TextDisabled("-> %s/", ChainPresets::UserPresetDir().c_str());
        ImGui::BeginDisabled(m_presetSaveName[0] == '\0');
        if (ImGui::Button("Save")) { m_pendingPresetSave = true; ImGui::CloseCurrentPopup(); }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
    if (!m_presetActive.empty())
        Flow::DisabledWrapped("active preset: %s (%zu fields - blue chips)", m_presetActive.c_str(),
                              m_presetCovered.size());
}

void ChainPhysicsPreviewWindow::DrawM5Modals()
{
    if (m_confirmApplyOpen)
    {
        m_confirmApplyOpen = false;
        ImGui::OpenPopup("Apply to a different rig?##cpv");
    }
    if (ImGui::BeginPopupModal("Apply to a different rig?##cpv", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted(m_confirmApplyText.c_str());
        if (ImGui::Button("Apply anyway"))
        {
            m_pendingApply = m_confirmApply;
            m_pendingApply.confirmed = true;
            m_pendingApply.valid = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    // ONE confirmation for every way the staged edits could be lost: the close
    // (title-bar X, Window menu) and every subject change (Load, drop, Use
    // Selected, Reload). The answer is posted to PPA (TickGuard), which also takes
    // the harness's answers.
    if (m_confirmGuardOpen)
    {
        m_confirmGuardOpen = false;
        ImGui::OpenPopup("Unapplied chain edits##cpv");
    }
    if (ImGui::BeginPopupModal("Unapplied chain edits##cpv", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        const bool close = m_guarded.kind == GuardKind::Close;
        ImGui::TextUnformatted("The staged chain parameters differ from the subject's "
                               "load-time values\nand have not been applied.");
        if (!close)
            ImGui::TextDisabled("%s would replace the subject%s%s.", GuardKindName(m_guarded.kind),
                                m_guarded.path.empty() ? "" : " with ", m_guarded.path.c_str());
        const char* verb = close ? "close" : "continue";
        char l[64];
        std::snprintf(l, sizeof(l), "Apply (%s) and %s", ApplyDestName(m_applyDest), verb);
        if (ImGui::Button(l)) { m_pendingGuardAnswer = 2; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        std::snprintf(l, sizeof(l), "Discard and %s", verb);
        if (ImGui::Button(l)) { m_pendingGuardAnswer = 1; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Keep editing")) { m_pendingGuardAnswer = 0; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

} // namespace Editor
