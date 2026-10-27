// EditorLayer_Documents.cpp - the documents other editor windows hold: what the exit guard lists,
// and its Save. Each window keeps its own document model and save path; this file only asks them.

#include "Editor/EditorLayer.h"
#include "Editor/AnimGraphEditorWindow.h"
#include "Editor/AnimSetEditorWindow.h"
#include "Editor/AttackEditor.h"
#include "Editor/ChainPhysicsPreviewWindow.h"
#include "Editor/MaterialEditorWindow.h"
#include "Editor/MeshEditorWindow.h"
#include "Editor/SkeletonEditorWindow.h"
#include "Editor/UIEditorWindow.h"
#include "System/Log.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace
{
// The free-function editors answer a Control request with a JSON state that carries "dirty" and
// "path"; "save" writes the file.
struct ControlledDocument
{
    const char* owner;
    std::string (*control)(const std::string& request);
};

const ControlledDocument kControlledDocuments[] = {
    { "Animation Graph Editor", &Editor::AnimGraphEditor::Control },
    { "Animation Set Editor",   &Editor::AnimSetEditor::Control },
    { "Attack Editor",          &::AttackEditor::Control },
};

bool ControlledDirty(const ControlledDocument& d, std::string& path)
{
    const nlohmann::json state = nlohmann::json::parse(d.control(R"({"action":"state"})"), nullptr, false);
    if (state.is_discarded() || !state.value("ok", false) || !state.value("dirty", false)) return false;
    path = state.value("path", std::string());
    return true;
}
} // namespace

std::vector<EditorLayer::UnsavedDocument> EditorLayer::CollectUnsavedDocuments()
{
    std::vector<UnsavedDocument> out;
    auto add = [&out](const char* owner, const std::string& name, bool saveable = true) {
        out.push_back({ owner, name.empty() ? std::string("(untitled)") : name, saveable });
    };

    for (const std::string& p : Editor::UIEditorWindow::Get().UnsavedAssets()) add("UI Editor", p);
    for (const std::string& p : Editor::MaterialEditorWindow::Get().UnsavedFiles()) add("Material Editor", p);

    const auto& mesh = Editor::MeshEditorWindow::Get();
    if (mesh.HasUnsavedEdits()) add("Mesh Editor", mesh.DocumentPath());
    const auto& skel = Editor::SkeletonEditorWindow::Get();
    if (skel.Dirty()) add("Skeleton Editor", skel.DocumentPath());
    if (m_sequencerWindow.Dirty()) add("Sequencer", m_sequencerWindow.Path());
    if (const std::string vfx = UnsavedVfxPath(); !vfx.empty()) add("VFX Editor", vfx);

    for (const ControlledDocument& d : kControlledDocuments)
    {
        std::string path;
        if (ControlledDirty(d, path)) add(d.owner, path);
    }

    // Staged chain values are committed by Apply to a destination the user picks (scene, prefab,
    // instances): nothing here can choose it for them.
    if (Editor::ChainPhysicsPreviewWindow::Get().IsStagedDirty())
        add("Chain Physics Preview", "unapplied chain edits", false);
    return out;
}

bool EditorLayer::SaveUnsavedDocuments(std::string& why)
{
    bool ok = true;
    auto fail = [&ok, &why](const std::string& what) {
        LOG_ERROR("EditorLayer: %s", what.c_str());
        if (ok) why = what;
        ok = false;
    };

    std::string err;
    auto& ui = Editor::UIEditorWindow::Get();
    if (!ui.UnsavedAssets().empty() && !ui.SaveAllUnsaved(UIEditorCtx(), err))
        fail("the UI assets were not all saved: " + err);

    auto& mat = Editor::MaterialEditorWindow::Get();
    if (!mat.UnsavedFiles().empty() && !mat.SaveAllNow(MaterialEditorCtx(), err))
        fail("the Material Editor " + err);

    auto& mesh = Editor::MeshEditorWindow::Get();
    if (mesh.HasUnsavedEdits() && !mesh.SaveNow(MeshEditorCtx(), err))
        fail("the Mesh Editor could not save '" + mesh.DocumentPath() + "': " + err);

    auto& skel = Editor::SkeletonEditorWindow::Get();
    if (skel.Dirty() && !skel.SaveNow(SkeletonEditorCtx(), err))
        fail("the Skeleton Editor could not save '" + skel.DocumentPath() + "': " + err);

    if (m_sequencerWindow.Dirty() && !m_sequencerWindow.Save())
        fail("the Sequencer could not save '" + m_sequencerWindow.Path() + "'");

    if (!UnsavedVfxPath().empty() && !SaveVfxNow(err))
        fail("the VFX Editor " + err);

    for (const ControlledDocument& d : kControlledDocuments)
    {
        std::string path;
        if (!ControlledDirty(d, path)) continue;
        const nlohmann::json reply = nlohmann::json::parse(d.control(R"({"action":"save"})"), nullptr, false);
        // AttackEditor answers ok even when its Save threw into its own error: dirty is the truth.
        if (reply.is_discarded() || !reply.value("ok", false) || ControlledDirty(d, path))
        {
            const std::string error = reply.is_discarded() ? std::string() : reply.value("error", std::string());
            fail(std::string("the ") + d.owner + " could not save '" + path + "'" +
                 (error.empty() ? std::string() : ": " + error));
        }
    }

    if (Editor::ChainPhysicsPreviewWindow::Get().IsStagedDirty())
        fail("the Chain Physics Preview has unapplied edits: Apply or Discard them in its panel");
    return ok;
}

void EditorLayer::ProcessPendingSaveDocuments()
{
    if (!m_pendingSaveDocuments) return;
    m_pendingSaveDocuments = false;
    std::string why;
    if (SaveUnsavedDocuments(why)) return;
    // Same contract as ProcessPendingSaveScene: the exit was queued optimistically, and a document
    // that is still unsaved must not be thrown away by it.
    m_exitApproved = false;
    LOG_ERROR("EditorLayer: not every document was saved (%s) - the exit was CANCELLED.", why.c_str());
}
