// Authoring window for an entity's AnimationSetComponent (clip aliases -> .ianim paths), plus the
// runtime Animator strip and the raw clip-index debug tools. Part of EditorLayer, in its own file to
// keep EditorLayer.cpp manageable.
// The target entity is latched (EntityHandle) when the Inspector's "Edit Animation Set..." button
// is clicked and deliberately does not follow the hierarchy selection, so other entities can be
// selected while editing; [Use Selected] re-latches when the selection qualifies.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Editor/EditorLayer.h"
#include "Editor/AnimSetEditorWindow.h"
#include "Editor/AnimSetDocument.h"
#include "Editor/AttackEditor.h"
#include "Editor/ImGuiWidgets.h"
#include "ECS/AnimationComponents.h"
#include "ECS/AnimGraphSystem.h"
#include "ECS/HierarchyComponents.h"
#include "Resource/AnimationClipSystem.h"
#include "Resource/AssetFS.h"
#include "Resource/SkeletonAsset.h"
#include "Renderer/Renderer.h"
#include "System/PathUtil.h"
#include "System/Log.h"
#include "imgui/imgui.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <typeindex>
#include <vector>

namespace Editor::AnimSetEditor {
namespace {
using Json = nlohmann::json;
struct AssetWindow {
    AnimSetDocument document;
    std::string selected, error, savedBytes, pendingReload;
    bool open = false, focus = false, focused = false;
    uint32_t drawnFrames = 0;
} assetWindow;

bool LoadFile(const std::string& path) {
    std::string bytes;
    if (!Resource::AssetFS::Get().ReadFileText(path, bytes)) {
        assetWindow.error = "Could not read Animation Set: " + path; return false;
    }
    if (!assetWindow.document.Load(bytes, assetWindow.error)) return false;
    assetWindow.document.path = path; assetWindow.savedBytes = std::move(bytes);
    const auto& slots = assetWindow.document.Definition()["slots"];
    assetWindow.selected = slots.empty() ? "" : slots.begin().key();
    LOG_INFO("[ANIMSET_EDITOR] open %s slots=%zu", path.c_str(), slots.size());
    return true;
}
bool Save() {
    auto& document = assetWindow.document;
    if (!AnimSetDocument::Validate(document.Definition(), assetWindow.error)) return false;
    if (document.path.empty()) { assetWindow.error = "Choose an Animation Set asset path first."; return false; }
    std::string disk;
    const bool exists = Resource::AssetFS::Get().ReadFileText(document.path, disk);
    if ((exists && disk != assetWindow.savedBytes) || (!exists && !assetWindow.savedBytes.empty())) {
        assetWindow.error = "Animation Set changed on disk. Reload it before saving."; return false;
    }
    const auto bytes = document.Text();
    if (!Sys::WriteFileBytesAtomic(document.path, bytes.data(), bytes.size())) {
        assetWindow.error = "Could not save Animation Set: " + document.path; return false;
    }
    assetWindow.savedBytes = bytes; document.Saved(); assetWindow.error.clear();
    assetWindow.pendingReload = document.path;
    LOG_INFO("[ANIMSET_EDITOR] saved %s slots=%zu", document.path.c_str(), document.Definition()["slots"].size());
    return true;
}
void SetField(const char* field, Json value) {
    auto slot = assetWindow.document.Definition()["slots"].at(assetWindow.selected);
    if (slot.is_string()) slot = {{"path", slot.get<std::string>()}};
    slot[field] = std::move(value);
    if (std::string_view(field) == "path") slot["sourceClip"] = "";
    assetWindow.document.Set(assetWindow.selected, std::move(slot)); assetWindow.error.clear();
}
}
bool Open(const std::string& path) {
    assetWindow.open = true; assetWindow.focus = true;
    const std::string relative = Sys::RelativizeAssetPath(path);
    if (assetWindow.document.Dirty()) {
        if (assetWindow.document.path == relative) return true;
        assetWindow.error = "Save or reload the current Animation Set before opening another."; return false;
    }
    return LoadFile(relative);
}
std::string Control(const std::string& request) {
    try {
        const auto command = Json::parse(request);
        const auto action = command.value("action", "state");
        auto& document = assetWindow.document;
        if (action == "open" && !Open(command.at("path"))) throw std::runtime_error(assetWindow.error);
        if (action == "new") {
            assetWindow.open = true; assetWindow.focus = true;
            if (document.Dirty()) throw std::runtime_error("Save or reload the current Animation Set before creating another.");
            document.New(); document.path = Sys::RelativizeAssetPath(command.at("path"));
            assetWindow.savedBytes.clear(); assetWindow.selected.clear(); assetWindow.error.clear();
        }
        if (action == "save" && !Save()) throw std::runtime_error(assetWindow.error);
        if (action == "reload" && !LoadFile(document.path)) throw std::runtime_error(assetWindow.error);
        if (action == "set") {
            const std::string name = command.at("name");
            auto next = document.Definition(); next["slots"][name] = command.at("slot");
            if (!AnimSetDocument::Validate(next, assetWindow.error)) throw std::runtime_error(assetWindow.error);
            document.Commit(std::move(next)); assetWindow.selected = name;
        }
        if (action == "rename") {
            const std::string name = command.at("name"), replacement = command.at("replacement");
            if (!document.Rename(name, replacement, assetWindow.error)) throw std::runtime_error(assetWindow.error);
            assetWindow.selected = replacement;
        }
        if (action == "remove") document.Remove(command.at("name"));
        if (action == "undo") document.Undo();
        if (action == "redo") document.Redo();
        if (action == "close") assetWindow.open = false;
        return Json{{"ok", true}, {"open", assetWindow.open}, {"path", document.path},
            {"dirty", document.Dirty()}, {"definition", document.Definition()}, {"drawnFrames", assetWindow.drawnFrames}}.dump();
    } catch (const std::exception& e) {
        assetWindow.error = e.what(); return Json{{"ok", false}, {"error", assetWindow.error}}.dump();
    }
}
void ProcessPendingActions(AnimGraphSystem* system) {
    if (system && !assetWindow.pendingReload.empty()) {
        system->Reload(assetWindow.pendingReload); assetWindow.pendingReload.clear();
    }
}
bool ConsumeFocusedLastDraw() {
    const bool focused = assetWindow.open && assetWindow.focused; assetWindow.focused = false; return focused;
}
void Draw(Resource::AnimationClipSystem* clips) {
    if (!assetWindow.open) return;
    ImGui::SetNextWindowSize(ImVec2(800, 580), ImGuiCond_FirstUseEver);
    if (assetWindow.focus) { ImGui::SetNextWindowFocus(); assetWindow.focus = false; }
    const bool visible = ImGui::Begin("Animation Set Asset", &assetWindow.open, ImGuiWindowFlags_MenuBar);
    assetWindow.focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (!visible) { ImGui::End(); return; }
    auto& document = assetWindow.document;
    bool saveRequested = false, reloadRequested = false, addRequested = false;
    int historyAction = 0;
    std::string nextSelection, droppedAsset;
    if (ImGui::BeginMenuBar()) {
        if (ImGui::MenuItem("Save", "Ctrl+S")) saveRequested = true;
        if (ImGui::MenuItem("Undo", "Ctrl+Z")) historyAction = -1;
        if (ImGui::MenuItem("Redo", "Ctrl+Y")) historyAction = 1;
        if (ImGui::MenuItem("Reload", nullptr, false, !document.path.empty())) reloadRequested = true;
        ImGui::EndMenuBar();
    }
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::GetIO().KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_S)) saveRequested = true;
        if (!ImGui::GetIO().WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_Z)) historyAction = ImGui::GetIO().KeyShift ? 1 : -1;
            if (ImGui::IsKeyPressed(ImGuiKey_Y)) historyAction = 1;
        }
    }
    ImGui::TextWrapped("%s%s", document.path.c_str(), document.Dirty() ? " *" : "");
    ImGui::TextDisabled("Slots map graph clip names to animation assets. %zu slot(s).", document.Definition()["slots"].size());
    if (!assetWindow.error.empty()) ImGui::TextWrapped("%s", assetWindow.error.c_str());
    addRequested = ImGui::Button("+ Add Slot");
    ImGui::Separator();
    const float listWidth = 220.f;
    ImGui::BeginChild("Slots", ImVec2(listWidth, 0), ImGuiChildFlags_Borders);
    for (auto it = document.Definition()["slots"].begin(); it != document.Definition()["slots"].end(); ++it) {
        if (ImGui::Selectable(it.key().c_str(), assetWindow.selected == it.key())) nextSelection = it.key();
        if (ImGui::IsItemHovered()) {
            const auto path = it.value().is_string() ? it.value().get<std::string>() : it.value().value("path", "");
            ImGui::SetTooltip("%s", path.c_str());
        }
    }
    if (document.Definition()["slots"].empty()) ImGui::TextWrapped("Add a slot, or drop an animation asset here.");
    ImGui::EndChild();
    if (ImGui::BeginDragDropTarget()) {
        if (const auto* payload = ImGui::AcceptDragDropPayload("IANIM_PATH")) {
            droppedAsset = Sys::RelativizeAssetPath(static_cast<const char*>(payload->Data));
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::SameLine(); ImGui::BeginChild("Slot Definition", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (document.Definition()["slots"].contains(assetWindow.selected)) {
        ImGui::PushID(assetWindow.selected.c_str());
        auto slot = document.Definition()["slots"][assetWindow.selected];
        if (slot.is_string()) slot = {{"path", slot.get<std::string>()}};
        char name[256]{}; std::snprintf(name, sizeof(name), "%s", assetWindow.selected.c_str());
        ImGui::InputText("Slot Name", name, sizeof(name));
        if ((ImGui::IsItemDeactivatedAfterEdit() || (saveRequested && ImGui::IsItemActive()))
            && document.Rename(assetWindow.selected, name, assetWindow.error)) assetWindow.selected = name;
        char path[1024]{}; std::snprintf(path, sizeof(path), "%s", slot.value("path", "").c_str());
        ImGui::InputText("Animation Asset", path, sizeof(path));
        if (ImGui::IsItemDeactivatedAfterEdit() || (saveRequested && ImGui::IsItemActive())) {
            const auto relative = Sys::RelativizeAssetPath(path);
            if (slot.value("path", "") != relative) SetField("path", relative);
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const auto* payload = ImGui::AcceptDragDropPayload("IANIM_PATH")) {
                SetField("path", Sys::RelativizeAssetPath(static_cast<const char*>(payload->Data)));
            }
            ImGui::EndDragDropTarget();
        }
        char source[256]{}; std::snprintf(source, sizeof(source), "%s", slot.value("sourceClip", "").c_str());
        ImGui::InputText("Source Clip", source, sizeof(source));
        if (ImGui::IsItemDeactivatedAfterEdit() || (saveRequested && ImGui::IsItemActive())) SetField("sourceClip", source);
        ImGui::TextDisabled("Leave Source Clip empty to use the first clip in the asset.");
        const auto names = clips && !slot.value("path", "").empty() ? clips->GetBoneClipNames(slot.value("path", "")) : std::vector<std::string>{};
        if (!names.empty() && ImGui::BeginCombo("Choose Source", source[0] ? source : "(first clip)")) {
            if (ImGui::Selectable("(first clip)", !source[0])) SetField("sourceClip", "");
            for (const auto& clip : names) if (ImGui::Selectable(clip.c_str(), clip == source)) SetField("sourceClip", clip);
            ImGui::EndCombo();
        }
        bool loop = slot.value("loop", true); if (ImGui::Checkbox("Loop", &loop)) SetField("loop", loop);
        float rate = slot.value("rate", 1.f);
        const bool changed = ImGui::DragFloat("Playback Rate", &rate, .05f, -8.f, 8.f, "%.2fx");
        if (ImGui::IsItemActivated()) document.BeginGesture();
        if (changed) SetField("rate", rate);
        if (ImGui::IsItemDeactivated()) document.EndGesture();
        ImGui::Separator();
        if (ImGui::Button("Remove Slot")) { document.Remove(assetWindow.selected); assetWindow.selected.clear(); }
        ImGui::PopID();
    } else ImGui::TextWrapped("Select a slot on the left to inspect its definition.");
    ImGui::EndChild();
    if (!nextSelection.empty()) assetWindow.selected = nextSelection;
    if (addRequested || !droppedAsset.empty()) {
        std::string name = "Clip";
        for (unsigned suffix = 1; document.Definition()["slots"].contains(name); ++suffix) name = "Clip" + std::to_string(suffix);
        document.Set(name, Json{{"path", droppedAsset}, {"sourceClip", ""}, {"loop", true}, {"rate", 1.f}});
        assetWindow.selected = name;
    }
    if (historyAction) { if (historyAction < 0) document.Undo(); else document.Redo(); assetWindow.error.clear(); }
    if (reloadRequested) LoadFile(document.path);
    // Save after text fields commit, including the active field on Ctrl+S.
    if (saveRequested) Save();
    ++assetWindow.drawnFrames; ImGui::End();
}
}

namespace
{
// Candidate entity for [Use Selected]: follows the SkeletonRef hop (mesh child -> character root)
// and requires the animation components.
Entity ResolveAnimSetTarget(World& world, Entity e)
{
    if (e == NullEntity || !world.IsAlive(e)) return NullEntity;
    if (const SkeletonRef* ref = world.GetComponent<SkeletonRef>(e))
        if (ref->entity != NullEntity && world.IsAlive(ref->entity))
            e = ref->entity;
    if (!world.HasComponent<AnimationSetComponent>(e) ||
        !world.HasComponent<SkeletonComponent>(e))
        return NullEntity;
    return e;
}

void DrawSourceSelector(const char* label, const char* automaticLabel,
                        std::string& selected,
                        const std::vector<std::string>& names)
{
    const char* preview = selected.empty() ? automaticLabel : selected.c_str();
    if (!ImGui::BeginCombo(label, preview)) return;
    const bool automatic = selected.empty();
    if (ImGui::Selectable(automaticLabel, automatic))
        selected.clear();
    if (automatic) ImGui::SetItemDefaultFocus();
    for (const std::string& name : names)
    {
        const bool isSelected = selected == name;
        if (ImGui::Selectable(name.c_str(), isSelected))
            selected = name;
        if (isSelected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
}
} // namespace

void EditorLayer::RenderAnimationSetEditorWindow()
{
    if (!m_showAnimSetEditor) return;

    ImGui::SetNextWindowSize(ImVec2(560, 680), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Animation Set Editor", &m_showAnimSetEditor))
    {
        ImGui::End();
        return;
    }

    World* world = m_world;
    if(ImGui::Button("Edit Attacks..."))AttackEditor::Open();

    // ---- Target latch (generation-checked, survives ID recycling) ----
    const bool targetValid =
        world && world->IsHandleValid(m_animSetEditTarget) &&
        world->HasComponent<AnimationSetComponent>(m_animSetEditTarget.entity) &&
        world->HasComponent<SkeletonComponent>(m_animSetEditTarget.entity);

    const Entity selCandidate =
        world ? ResolveAnimSetTarget(*world, m_selectedEntity) : NullEntity;

    if (targetValid)
    {
        const Entity target = m_animSetEditTarget.entity;
        ImGui::Text("Target: %s  (#%u)", world->GetName(target).c_str(), target);
    }
    else
    {
        ImGui::TextDisabled("Target lost - click \"Edit Animation Set...\" on an "
                            "entity's Animation Set component, or use [Use Selected].");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(selCandidate == NullEntity);
    if (ImGui::Button("Use Selected"))
    {
        m_animSetEditTarget = world->MakeHandle(selCandidate);
        m_animSetSelClip    = -1;
    }
    ImGui::EndDisabled();

    if (!targetValid) { ImGui::End(); return; }

    if (!UndoEnabled())
        ImGui::TextDisabled("Play mode: edits are transient (reverted on Stop).");

    const Entity target = m_animSetEditTarget.entity;

    // ---- Undo bracketing (same machinery as the Inspector dispatch) ----
    Editor::Undo::CaptureFn                    capture;
    std::function<std::string(World&, Entity)> fingerprint;
    {
        const auto it = m_componentEditors.find(
            std::type_index(typeid(AnimationSetComponent)));
        if (it != m_componentEditors.end())
        {
            capture     = it->second.captureValue;
            fingerprint = it->second.fingerprint;
        }
    }

    DrawComponentWithUndo(target, std::type_index(typeid(AnimationSetComponent)),
        capture, fingerprint, [&]()
    {
        auto& set = *world->GetComponent<AnimationSetComponent>(target);

        ImGui::Checkbox("Play Default On Load", &set.playDefaultOnLoad);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180.f);
        if (ImGui::BeginCombo("Default",
                              set.defaultClip.empty() ? "(none)" : set.defaultClip.c_str()))
        {
            if (ImGui::Selectable("(none)", set.defaultClip.empty()))
                set.defaultClip.clear();
            for (const auto& ref : set.clips)
            {
                if (ref.name.empty()) continue;
                const bool selected = (set.defaultClip == ref.name);
                if (ImGui::Selectable(ref.name.c_str(), selected))
                    set.defaultClip = ref.name;
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::Separator();

        // Clamp the list selection every frame (removals, target switches).
        if (m_animSetSelClip >= static_cast<int>(set.clips.size()))
            m_animSetSelClip = static_cast<int>(set.clips.size()) - 1;

        int removeIndex = -1;

        // ---- Left: clip list ----
        const float listHeight = 340.f;
        ImGui::BeginChild("cliplist", ImVec2(190.f, listHeight), true);
        for (int i = 0; i < static_cast<int>(set.clips.size()); ++i)
        {
            const auto& ref = set.clips[static_cast<size_t>(i)];
            char row[160];
            snprintf(row, sizeof(row), "%s##clip%d",
                     ref.name.empty() ? "(unnamed)" : ref.name.c_str(), i);
            if (ImGui::Selectable(row, m_animSetSelClip == i))
                m_animSetSelClip = i;
            if (ImGui::IsItemHovered() && !ref.path.empty())
                ImGui::SetTooltip("%s\n%s | %.2fx", ref.path.c_str(),
                                  ref.looping ? "loop" : "once", ref.speed);
        }
        if (set.clips.empty())
            ImGui::TextDisabled("(empty)\nDrop a .ianim here\nor click + Add.");
        ImGui::EndChild();
        // Dropping a .ianim on the list appends a new alias.
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("IANIM_PATH"))
            {
                std::string name = "Clip";
                for (uint32_t suffix = 1; set.Find(name); ++suffix)
                    name = "Clip" + std::to_string(suffix);
                AnimationClipReference ref{ name };
                ref.path = Sys::RelativizeAssetPath(static_cast<const char*>(payload->Data));
                set.clips.push_back(std::move(ref));
                m_animSetSelClip = static_cast<int>(set.clips.size()) - 1;
            }
            ImGui::EndDragDropTarget();
        }

        ImGui::SameLine();

        // ---- Right: selected-clip detail ----
        ImGui::BeginChild("clipdetail", ImVec2(0.f, listHeight), true);
        if (m_animSetSelClip >= 0 &&
            m_animSetSelClip < static_cast<int>(set.clips.size()))
        {
            auto& ref = set.clips[static_cast<size_t>(m_animSetSelClip)];

            // Name / Asset commit on deactivate: committing per keystroke makes AnimationClipSystem's reconcile
            // see every intermediate string as remove-alias + add-alias (ReleaseClip / AcquireClip churn).
            char nameBuf[96]{};
            strncpy_s(nameBuf, sizeof(nameBuf), ref.name.c_str(), _TRUNCATE);
            ImGui::InputText("Name", nameBuf, sizeof(nameBuf));
            if (ImGui::IsItemDeactivatedAfterEdit() && ref.name != nameBuf)
            {
                const std::string oldName = ref.name;
                ref.name = nameBuf;
                if (set.defaultClip == oldName) set.defaultClip = ref.name;
            }

            char pathBuf[512]{};
            strncpy_s(pathBuf, sizeof(pathBuf), ref.path.c_str(), _TRUNCATE);
            ImGui::InputText("Asset", pathBuf, sizeof(pathBuf));
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                const std::string rel = Sys::RelativizeAssetPath(pathBuf);
                if (ref.path != rel)
                {
                    ref.path = rel;
                    ref.sourceClip.clear();
                    ref.sourceMorphClip.clear();
                }
            }
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("IANIM_PATH"))
                {
                    ref.path = Sys::RelativizeAssetPath(
                        static_cast<const char*>(payload->Data));
                    ref.sourceClip.clear();
                    ref.sourceMorphClip.clear();
                }
                ImGui::EndDragDropTarget();
            }

            const std::vector<std::string> boneClips =
                m_animClipSys && !ref.path.empty()
                ? m_animClipSys->GetBoneClipNames(ref.path)
                : std::vector<std::string>{};
            const std::vector<std::string> morphClips =
                m_animClipSys && !ref.path.empty()
                ? m_animClipSys->GetMorphClipNames(ref.path)
                : std::vector<std::string>{};
            DrawSourceSelector("Bone Clip", "(first bone clip)",
                               ref.sourceClip, boneClips);
            DrawSourceSelector("Morph Clip", "(auto match / first)",
                               ref.sourceMorphClip, morphClips);
            if (!ref.path.empty() && boneClips.empty())
                ImGui::TextDisabled("Clip list loading or asset contains no bone clips.");

            ImGui::Checkbox("Loop", &ref.looping);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100.f);
            ImGui::DragFloat("Speed", &ref.speed, 0.05f, -8.f, 8.f, "%.2fx");

            bool duplicate = ref.name.empty();
            for (int j = 0; j < static_cast<int>(set.clips.size()); ++j)
                duplicate |= (j != m_animSetSelClip &&
                              set.clips[static_cast<size_t>(j)].name == ref.name);
            if (duplicate)
                ImGui::TextColored(ImVec4(1.f, .45f, .25f, 1.f),
                                   "Name must be non-empty and unique.");

            ImGui::Separator();
            ImGui::BeginDisabled(duplicate || ref.path.empty() || !m_animClipSys);
            if (ImGui::Button("Play (0.2s blend)"))
                m_animClipSys->QueueAnimatorPlay(*world, target, ref.name, 0.2f, true);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Remove")) removeIndex = m_animSetSelClip;
        }
        else
        {
            ImGui::TextDisabled("Select a clip on the left, or add one.");
        }
        ImGui::EndChild();

        if (removeIndex >= 0)
        {
            const std::string removed = set.clips[static_cast<size_t>(removeIndex)].name;
            set.clips.erase(set.clips.begin() + removeIndex);
            if (set.defaultClip == removed) set.defaultClip.clear();
            if (m_animSetSelClip >= static_cast<int>(set.clips.size()))
                m_animSetSelClip = static_cast<int>(set.clips.size()) - 1;
        }

        if (ImGui::Button("+ Add Clip"))
        {
            std::string name = "Clip";
            for (uint32_t suffix = 1; set.Find(name); ++suffix)
                name = "Clip" + std::to_string(suffix);
            set.clips.push_back(AnimationClipReference{ name });
            m_animSetSelClip = static_cast<int>(set.clips.size()) - 1;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!m_animClipSys);
        if (ImGui::Button("Unassign / Clear All"))
        {
            m_animClipSys->UnassignAnimator(*world, target, true);
            m_animSetSelClip = -1;
        }
        ImGui::EndDisabled();
    });

    // ---- Runtime Animator strip (transient state, not in the undo set) ----
    ImGui::Separator();
    if (auto* anim = world->GetComponent<AnimationComponent>(target))
    {
        if (ImGui::Button(anim->paused ? "Resume" : "Pause"))
            anim->paused = !anim->paused;
        ImGui::SameLine();
        ImGui::BeginDisabled(!m_animClipSys);
        if (ImGui::Button("Stop"))
            m_animClipSys->StopAnimator(*world, target);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (const auto* transition =
                world->GetComponent<AnimatorTransitionComponent>(target))
        {
            if (transition->active)
                ImGui::TextDisabled("Current: %s  ->  %s (%.0f%%)",
                    transition->currentClipName.empty() ? "(unnamed)"
                                                        : transition->currentClipName.c_str(),
                    transition->targetClipName.c_str(), anim->blendWeight * 100.f);
            else
                ImGui::TextDisabled("Current: %s",
                    transition->currentClipName.empty() ? "(unnamed)"
                                                        : transition->currentClipName.c_str());
        }

        // ---- Debug: raw ClipLibrary indices ----
        if (m_renderer && ImGui::CollapsingHeader("Debug (raw clip indices)"))
        {
            const ClipLibrary& clipLib   = m_renderer->GetClipLibrary();
            const uint32_t     clipCount = clipLib.Count();
            if (clipCount == 0)
            {
                ImGui::TextDisabled("No clips loaded.");
            }
            else
            {
                uint32_t boneFilter = 0;
                if (const SkeletonComponent* sc =
                        world->GetComponent<SkeletonComponent>(target))
                    if (sc->assetIndex != kInvalidSkeletonIndex)
                        boneFilter = sc->boneCount;

                auto clipLabel = [&](uint32_t idx) -> std::string {
                    if (idx == kInvalidClipIndex) return "(none)";
                    if (idx >= clipCount)         return "(invalid)";
                    const ClipAsset& c = clipLib.Get(idx);
                    char buf[64];
                    snprintf(buf, sizeof(buf), "Clip #%u  |  %.2f s  |  %u bones",
                             idx, c.duration, c.boneCount);
                    return buf;
                };
                auto clipCombo = [&](const char* tag, uint32_t& clipIdx, float& timeFld)
                {
                    ImGui::TextDisabled("%s", tag);
                    ImGui::PushID(tag);
                    const std::string cur = clipLabel(clipIdx);
                    if (ImGui::BeginCombo("##c", cur.c_str()))
                    {
                        if (ImGui::Selectable("(none)", clipIdx == kInvalidClipIndex))
                            clipIdx = kInvalidClipIndex;
                        for (uint32_t i = 0; i < clipCount; ++i)
                        {
                            const ClipAsset& c = clipLib.Get(i);
                            if (boneFilter && c.boneCount != boneFilter) continue;
                            const bool sel = (clipIdx == i);
                            if (ImGui::Selectable(clipLabel(i).c_str(), sel))
                            { clipIdx = i; timeFld = 0.f; }
                            if (sel) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::PopID();
                    if (clipIdx != kInvalidClipIndex && clipIdx < clipCount)
                    {
                        const float dur = clipLib.Get(clipIdx).duration;
                        char id[16]; snprintf(id, sizeof(id), "Time##%s", tag);
                        EditorUI::DragFloat(id, &timeFld, 0.f,
                                           dur > 0.f ? dur : 1.f, "%.3f s");
                    }
                };
                clipCombo("Primary Clip",           anim->primaryClip,   anim->primaryTime);
                clipCombo("Cross-fade (Secondary)", anim->secondaryClip, anim->secondaryTime);
                if (anim->secondaryClip != kInvalidClipIndex)
                    EditorUI::DragFloat("Blend##blend", &anim->blendWeight, 0.f, 1.f,
                                       "%.2f  (0=primary, 1=secondary)");
            }
        }
    }
    else
    {
        ImGui::TextDisabled("No Animator (runtime) component yet - Play a clip to create one.");
    }

    ImGui::End();
}
