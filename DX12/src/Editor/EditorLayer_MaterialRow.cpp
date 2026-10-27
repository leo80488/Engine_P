// The Inspector's material row. The Material section and each Static Mesh slot
// show one row (Editor::DrawMaterialRow): the asset, what it is, the entity's own
// edits, Render.explain's warnings and [Edit], which opens the asset in the
// Material Editor; Edit here opens the entity's material as a Material Editor tab.
//
// Every action - the row's click and Editor.InspectorMaterialAction alike - goes
// through RunMaterialRowAction.
#include "Editor/EditorLayer.h"
#include "Editor/EditorLayerShared.h"      // OpenFileDialog, SaveFileDialog
#include "Editor/MaterialEditorWindow.h"
#include "ECS/HierarchyComponents.h"      // MaterialSourcePath, MaterialInstanceInfo, MaterialOverride
#include "ECS/StaticMeshComponents.h"     // StaticMeshComponent, MeshSectionOf
#include "Renderer/RenderExplain.h"       // RenderExplain::MaterialHints
#include "Renderer/Renderer.h"
#include "Resource/MaterialSerializer.h"
#include "Scene/MeshAssetRegistry.h"
#include "System/CommandLine.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include "imgui/imgui.h"

#include <cstdio>
#include <string>

namespace
{
    const char* kImatFilter     = "Material (*.imat)\0*.imat\0All Files\0*.*\0\0";
    const char* kImatInstFilter = "Material Instance (*.imatinst)\0*.imatinst\0All Files\0*.*\0\0";
    const char* kImatLoadFilter = "Material (*.imat;*.imatinst)\0*.imat;*.imatinst\0All Files\0*.*\0\0";

    // A row tile's key is its GUID's (ResolveAssetPreviewIdentity) - the same key
    // the Content Browser's own entry for the file gets, and the thumbnail lane
    // routes results by key. Salted, so each answers its own request.
    constexpr uint64_t kRowTileSalt = 0xD6E8FEB86659FD93ull;

    // The derived section entity of `owner` for `sectionKey` (NullEntity before
    // MeshInstanceSystem has derived it).
    Entity SectionEntity(World& w, Entity owner, uint64_t sectionKey)
    {
        if (const Children* ch = w.GetComponent<Children>(owner))
            for (Entity c : ch->entities)
                if (const MeshSectionOf* so = w.GetComponent<MeshSectionOf>(c);
                    so && so->owner == owner && so->sectionKey == sectionKey)
                    return c;
        return NullEntity;
    }
}

uint64_t EditorLayer::MaterialRowThumb(const std::string& path)
{
    if (path.empty()) return 0;
    auto [it, added] = m_materialRowTiles.try_emplace(path);
    AssetEntry& tile = it->second;
    if (added)
    {
        tile.path = path;
        tile.stem = tile.name = Sys::PathToUtf8(Sys::U8Path(path).stem());
        tile.type = AssetType::Material;
        ResolveAssetPreviewIdentity(tile);
        tile.previewKey ^= kRowTileSalt;
        if (tile.previewKey == 0) tile.previewKey = 1;
    }
    StreamEntryThumbnail(tile, /*visible=*/true, static_cast<float>(ImGui::GetTime()));
    return GetEntryThumbnailSrv(tile);
}

Editor::MaterialRowInfo EditorLayer::MaterialRowFor(Entity e)
{
    Editor::MaterialRowInfo info;
    if (!m_world || e == NullEntity || !m_world->IsAlive(e)) return info;
    const MaterialComponent* mat = m_world->GetComponent<MaterialComponent>(e);
    if (!mat) return info;
    const MaterialSourcePath*   src = m_world->GetComponent<MaterialSourcePath>(e);
    const MaterialInstanceInfo* mi  = m_world->GetComponent<MaterialInstanceInfo>(e);
    const MaterialOverride*     ovr = m_world->GetComponent<MaterialOverride>(e);
    info.path = src ? src->path : std::string();
    const bool instance = mi && !mi->parentPath.empty();
    Editor::DescribeMaterial(*mat, instance, info.tag, info.summary);
    if (info.path.empty()) info.tag = "Inline";
    if (instance)
    {
        info.parent         = mi->parentPath;
        info.parentResolved = mi->parentResolved;
    }
    info.entityOverrides = ovr ? static_cast<int>(ovr->props.size()) : 0;
    info.unsaved         = mat->IsAssetDirty();
    if (auto it = m_matRowHints.find(e); it != m_matRowHints.end()) info.warnings = it->second;
    return info;
}

Editor::MaterialRowInfo EditorLayer::MaterialSlotRowFor(Entity owner, uint64_t sectionKey, const std::string& path,
                                                        bool overridden)
{
    Editor::MaterialRowInfo info;
    info.path      = path;
    info.slotState = overridden ? "override" : "default";
    if (!m_world || path.empty()) { info.tag = "None"; return info; }
    // What the slot draws with: the derived section's own material when it is
    // there (live, and the entity Render.explain answers for), else the file.
    const Entity sec = SectionEntity(*m_world, owner, sectionKey);
    const MaterialComponent* mat = sec != NullEntity ? m_world->GetComponent<MaterialComponent>(sec) : nullptr;
    if (mat)
    {
        const MaterialInstanceInfo* mi = m_world->GetComponent<MaterialInstanceInfo>(sec);
        const bool instance = mi && !mi->parentPath.empty();
        Editor::DescribeMaterial(*mat, instance, info.tag, info.summary);
        if (instance) { info.parent = mi->parentPath; info.parentResolved = mi->parentResolved; }
        if (auto it = m_matRowHints.find(sec); it != m_matRowHints.end()) info.warnings = it->second;
        return info;
    }
    MaterialComponent m{};
    Resource::MaterialLoadInfo li{};
    if (!Resource::LoadMaterial(path, m, &li)) { info.tag = "Missing"; return info; }
    Editor::DescribeMaterial(m, !li.parentPath.empty(), info.tag, info.summary);
    if (!li.parentPath.empty()) { info.parent = li.parentPath; info.parentResolved = li.parentResolved; }
    return info;
}

bool EditorLayer::StaticMeshSlotMaterial(Entity owner, uint64_t sectionKey, std::string& path, bool& overridden,
                                         std::string* defaultPath)
{
    if (!m_world || !m_world->IsAlive(owner)) return false;
    const StaticMeshComponent* sm = m_world->GetComponent<StaticMeshComponent>(owner);
    MeshAssetRegistry* reg = m_renderer ? m_renderer->GetMeshAssets() : nullptr;
    if (!sm || !reg || sm->model.empty()) return false;
    const MeshAssetRegistry::Model& model = reg->GetModel(sm->model, m_world->Epoch());
    const MeshAssetRegistry::Mesh*  mesh  = MeshAssetRegistry::Find(model, sm->meshKey, sm->meshName);
    if (!mesh) return false;
    for (const MeshAssetRegistry::Section& s : mesh->sections)
        if (s.key == sectionKey)
        {
            const StaticMeshComponent::SlotOverride* o = sm->FindOverride(sectionKey);
            overridden = o != nullptr;
            path       = o ? o->material : s.defaultMaterial;
            if (defaultPath) *defaultPath = s.defaultMaterial;
            return true;
        }
    return false;
}

bool EditorLayer::RunMaterialRowAction(Entity e, uint64_t sectionKey, const Editor::MaterialRowResult& a,
                                       std::string* why, bool recordUndo)
{
    using A = Editor::MaterialRowAction;
    auto fail = [why](std::string w) { if (why) *why = std::move(w); return false; };
    if (!m_world || e == NullEntity || !m_world->IsAlive(e)) return fail("stale_handle: the entity is not alive");

    // What the row shows: a slot's material, or the entity's own.
    const bool  slot = sectionKey != 0;
    std::string path, slotDefault;
    bool        overridden = false;
    if (slot)
    {
        if (!StaticMeshSlotMaterial(e, sectionKey, path, overridden, &slotDefault))
            return fail("no_slot: the entity has no Static Mesh slot with that section key");
    }
    else
    {
        if (!m_world->GetComponent<MaterialComponent>(e))
            return fail("no_material: the entity has no MaterialComponent");
        if (const MaterialSourcePath* src = m_world->GetComponent<MaterialSourcePath>(e)) path = src->path;
    }
    const bool unattended = Sys::CommandLine::Get().Has("-unattended");
    auto assign = [&](const std::string& p) -> bool
    {
        std::string w;
        // A slot given its model's own default is the default again, not an override of it.
        const bool toDefault = slot && !p.empty() && Sys::RelativizeAssetPath(p) == slotDefault;
        const bool ok = slot ? AssignStaticMeshSlot(e, sectionKey, toDefault ? std::string() : p, &w, recordUndo)
                             : AssignMaterial(e, p, &w);
        return ok ? true : fail(w);
    };
    // A path from the caller, else the dialog (never under -unattended: a script passes the path).
    auto pickPath = [&](const char* filter, const char* ext, bool save) -> std::string
    {
        if (!a.path.empty()) return a.path;
        if (unattended) return {};
        return save ? SaveFileDialog(filter, ext, "asset/") : OpenFileDialog(filter, "asset/");
    };

    bool ok = true;
    std::string result = path;
    switch (a.action)
    {
    case A::None:
        return fail("bad_action: no action");
    case A::Assign:
        if (a.path.empty()) return fail("needs_path: assign takes a material path");
        ok = assign(a.path);
        result = Sys::RelativizeAssetPath(a.path);
        break;
    case A::Browse:
    {
        const std::string p = pickPath(kImatLoadFilter, "imat", /*save=*/false);
        if (p.empty()) return fail(unattended ? "needs_path: browse opens a dialog; pass the path" : "cancelled");
        ok = assign(p);
        result = Sys::RelativizeAssetPath(p);
        break;
    }
    case A::Clear:
    {
        if (slot)
        {
            if (!overridden) return fail("nothing_to_clear: the slot already uses the model's default");
            ok = assign(std::string());
            StaticMeshSlotMaterial(e, sectionKey, result, overridden);
        }
        else
        {
            std::string w;
            ok = ClearMaterial(e, &w) ? true : fail(w);
            result.clear();
        }
        break;
    }
    case A::Edit:
        if (path.empty()) return fail("no_asset: an inline material has no file to open (Edit here, or Save as Asset)");
        Editor::MaterialEditorWindow::Get().RequestOpen(path);
        break;
    case A::EditHere:
    {
        if (slot) return fail("bad_action: a Static Mesh slot has no entity material of its own");
        // The entity's own material opens as a Material Editor tab (its edits are this
        // entity's MaterialOverride).
        std::string w;
        if (!Editor::MaterialEditorWindow::Get().OpenEntityNow(*m_world, e, w)) return fail("refused: " + w);
        result = "entity:" + m_world->GetName(e);
        break;
    }
    case A::CreateInstance:
    {
        if (path.empty()) return fail("no_asset: an inline material has no file to make an instance of");
        const std::string made = CreateMaterialInstance(path);
        if (made.empty()) return fail("create_failed: see the log");
        ok = assign(made);
        if (ok) Editor::MaterialEditorWindow::Get().RequestOpen(made);
        result = Sys::RelativizeAssetPath(made);
        break;
    }
    case A::Find:
        if (path.empty()) return fail("no_asset: an inline material has no file");
        m_revealAssetPath = path;
        SetCurrentAssetDir(Sys::PathToUtf8(Sys::U8Path(path).parent_path()));
        m_needsRescan        = true;
        m_showContentBrowser = true;
        break;
    case A::CopyPath:
        if (path.empty()) return fail("no_asset: an inline material has no file");
        ImGui::SetClipboardText(path.c_str());
        break;
    case A::SaveAsAsset:
    case A::PromoteOverrides:
    {
        if (slot) return fail("bad_action: a Static Mesh slot has no entity material of its own");
        const bool promote = a.action == A::PromoteOverrides;
        if (promote && path.empty()) return fail("no_asset: an inline material has no asset to make an instance of");
        const std::string p = promote ? pickPath(kImatInstFilter, "imatinst", /*save=*/true)
                                      : pickPath(kImatFilter, "imat", /*save=*/true);
        if (p.empty()) return fail(unattended ? "needs_path: pass the file to write" : "cancelled");
        MaterialComponent* mat = m_world->GetComponent<MaterialComponent>(e);
        // Promote: an instance of the entity's asset holding what differs from
        // it - the entity's overrides and its unsaved edits (a value diff).
        const bool written = promote ? Resource::SaveMaterialInstance(p, path, *mat)
                                     : Resource::SaveMaterial(*mat, p);
        if (!written) return fail("save_failed: '" + p + "' was not written (see the log)");
        ok = assign(p);
        result = Sys::RelativizeAssetPath(p);
        if (ok) InvalidateMaterialThumbnails(result);
        break;
    }
    case A::ClearOverrides:
        if (path.empty()) return fail("no_asset: an inline material has no asset to go back to");
        ok = assign(path);   // AssignMaterial reloads the file and drops MaterialOverride
        break;
    }
    LOG_INFO("[INSPECTOR] material row action %s on %s%s: ok=%d path=%s", Editor::MaterialRowActionName(a.action),
             m_world->GetName(e).c_str(), slot ? " (slot)" : "", ok ? 1 : 0, result.empty() ? "-" : result.c_str());
    return ok;
}

void EditorLayer::DrawInspectorMaterialRow(Entity e)
{
    Editor::MaterialRowInfo info = MaterialRowFor(e);
    info.thumb = MaterialRowThumb(info.path);
    const Editor::MaterialRowResult r = Editor::DrawMaterialRow("##matrow", info);
    if (r.action != Editor::MaterialRowAction::None)
    {
        std::string why;
        if (!RunMaterialRowAction(e, 0, r, &why) && !why.empty() && why != "cancelled")
            LOG_WARNING("Inspector: material %s - %s", Editor::MaterialRowActionName(r.action), why.c_str());
    }

    // One line per change of what the row shows: the harness's reading of it.
    if (Sys::CommandLine::Get().Has("-unattended") && m_world)
    {
        std::string warn;
        for (const auto& [code, text] : info.warnings) warn += (warn.empty() ? "" : ",") + code;
        char line[1024];
        std::snprintf(line, sizeof(line), "path=%s tag=%s parent=%s overrides=%d unsaved=%d warnings=%s thumb=%d",
                      info.path.empty() ? "-" : info.path.c_str(), info.tag.c_str(),
                      info.parent.empty() ? "-" : info.parent.c_str(), info.entityOverrides, info.unsaved ? 1 : 0,
                      warn.empty() ? "-" : warn.c_str(), info.thumb ? 1 : 0);
        const std::string name = m_world->GetName(e);
        std::string& last = m_matRowLogged[name];
        if (last != line)
        {
            last = line;
            LOG_INFO("[INSPECTOR] material row %s: %s", name.c_str(), line);
        }
    }
}

void EditorLayer::RefreshMaterialRowHints()
{
    if (!m_world || !m_renderer) return;
    const Entity sel = (m_selectedEntity != NullEntity && m_world->IsAlive(m_selectedEntity))
                     ? m_selectedEntity : NullEntity;
    if (sel == m_matRowHintsFor && m_editorFrame - m_matRowHintsFrame < 30) return;
    m_matRowHintsFor   = sel;
    m_matRowHintsFrame = m_editorFrame;
    m_matRowHints.clear();
    if (sel == NullEntity) return;

    // The causes only: a note (texture_loading, material_not_bound, unlit) is a
    // state, not a problem with the material.
    auto collect = [this](Entity e)
    {
        std::vector<std::pair<std::string, std::string>> out;
        for (const RenderExplain::Hint& h : RenderExplain::MaterialHints(*m_world, *m_renderer, e))
            if (h.kind == "cause") out.emplace_back(h.code, h.text);
        if (!out.empty()) m_matRowHints[e] = std::move(out);
    };
    collect(sel);
    // A Static Mesh's slots: its derived sections.
    if (m_world->HasComponent<StaticMeshComponent>(sel))
        if (const Children* ch = m_world->GetComponent<Children>(sel))
            for (Entity c : ch->entities)
                if (const MeshSectionOf* so = m_world->GetComponent<MeshSectionOf>(c); so && so->owner == sel)
                    collect(c);
}
