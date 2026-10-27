// The editor's main menu bar (EditorLayer member definitions only, no state of
// its own).
//
//   File    New / Open / Open Recent / Save / Save As / Import / Exit
//   Edit    Undo / Redo, Copy / Paste / Duplicate / Delete / Rename,
//           Select All / Deselect, Focus / Snap to Floor, settings windows
//   Create  EditorLayer_CreateMenu.cpp (shared with the Hierarchy)
//   Window  every dockable panel, grouped General / World / Animation / Content /
//           Scripting & Gameplay / Analysis - labels are the window titles
//   Tools   Command Palette / AI Control Channel / Bake / Retarget / Component
//           Transfer / Asset References / Lua tools / migrations
//   Build   startup scene + packaging
//   Debug   renderer diagnostics, tracers
//   Help    shortcuts / about / folders
//
// Two rules:
//  * A menu item that tears down GPU state only queues (m_pending*); the work
//    runs in ProcessPendingActions at the frame boundary.
//  * Never open a modal inside BeginMenuBar: the ID-stack mismatch makes
//    BeginPopupModal a silent no-op. Items set a request flag and
//    SetupDockSpace opens the popup after EndMenuBar.
#include "Editor/EditorLayer.h"
#include "Editor/EditorPanels.h"
#include "Editor/EditorLayerShared.h"      // OpenFileDialog, WriteStartupSceneToManifest
#include "Editor/FontAwesomeIcons.h"
#include "Editor/EditorTheme.h"             // Icon() column, MenuSection()
#include "Editor/ImGuiWidgets.h"           // EditorUI::DragFloat
#include "Editor/AttackEditor.h"
#include "Editor/ChainPhysicsPreviewWindow.h"
#include "Editor/MeshEditorWindow.h"          // Window > Content > Mesh Editor
#include "Editor/SkeletonEditorWindow.h"
#include "Editor/TextureViewerWindow.h"      // Window > Content > Texture Viewer
#include "Editor/UIEditorWindow.h"           // Window > Content > UI Editor
#include "Editor/MaterialEditorWindow.h"     // Window > Content > Material Editor
#include "Editor/RenderGraphWindow.h"        // Window > Analysis > Render Graph
#include "Editor/EditorModule.h"             // the enabled modules' rows
#include "Editor/InputBindingsWindow.h"
#include "Editor/DebugDrawSystem.h"
#include "Editor/DebugCategory.h"
#include "ECS/PlayModeController.h"
#include "Graphics/GraphicsDX12.h"         // HWND for File > Exit
#include "Graphics/GPUProfiler.h"
#include "Graphics/TracerSystem.h"
#include "Harness/ControlChannel.h"        // Tools > AI Control Channel
#include "Renderer/Renderer.h"
#include "RenderGraph/RenderPass/DDGIProbeDebugPass.h"
#include "RenderGraph/RenderPass/DecalPass.h"
#include "Scene/SceneDefaults.h"           // Scene::kBaseScenePath
#include "System/PathUtil.h"               // Sys::RelativizeAssetPath
#include "System/CommandLine.h"            // -ui_open_menu (harness)
#include "System/Log.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"          // GImGui->NavWindow (SelectionPanelFocused)

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>   // ShellExecuteA: packager console, folder openers

namespace
{
    // Open a folder (or file) in Explorer. Paths are relative to the working
    // directory (DX12/).
    void OpenInExplorer(const char* path)
    {
        ShellExecuteA(nullptr, "open", path, nullptr, nullptr, SW_SHOWNORMAL);
    }

    constexpr const char* kSceneFilter = "Scene (*.iscene)\0*.iscene\0All Files\0*.*\0\0";
}

// What the engine modules' editor halves are handed (Editor/EditorModule.h).
Editor::ModuleHost EditorLayer::ModuleHostFor()
{
    Editor::ModuleHost host;
    host.layer    = this;
    host.gfx      = m_gfx;
    host.world    = m_world;
    host.renderer = m_renderer;
    host.physics  = m_physicsSys;
    host.selected = GetSelectedEntity();
    host.menuHeld = [this](const char* n) { return HarnessMenuHeldOpen(n); };
    host.addComponentWithUndo = [this](Entity e, std::type_index type, std::function<void(World&, Entity)> add) {
        AddComponentWithUndo(e, type, std::move(add));
    };
    return host;
}

// Harness: -ui_open_menu=<Menu>@<imgui frame>[,...]. True while `name` is the
// entry whose frame most recently passed. Menu names are the top-level menu
// labels, plus "Show" for the viewport toolbar's Show dropdown.
bool EditorLayer::HarnessMenuHeldOpen(const char* name) const
{
#if defined(WITH_HARNESS) && WITH_HARNESS
    struct Entry { std::string name; int frame; };
    static const std::vector<Entry> s_entries = []
    {
        std::vector<Entry> out;
        const std::string spec = Sys::CommandLine::Get().Get("-ui_open_menu");
        size_t pos = 0;
        while (pos < spec.size())
        {
            size_t end = spec.find(',', pos);
            if (end == std::string::npos) end = spec.size();
            const std::string item = spec.substr(pos, end - pos);
            const size_t at = item.rfind('@');
            out.push_back({ item.substr(0, at),
                            at == std::string::npos ? 0 : std::atoi(item.c_str() + at + 1) });
            pos = end + 1;
        }
        return out;
    }();
    if (s_entries.empty()) return false;
    const int now = ImGui::GetFrameCount();
    const Entry* active = nullptr;
    for (const Entry& e : s_entries)
        if (e.frame <= now && (!active || e.frame >= active->frame)) active = &e;
    return active && active->name == name;
#else
    (void)name;
    return false;
#endif
}

bool EditorLayer::SelectionPanelFocused() const
{
    const ImGuiWindow* nav = GImGui ? GImGui->NavWindow : nullptr;
    if (!nav || !nav->RootWindow) return false;
    const char* name = nav->RootWindow->Name;
    // Docked windows are children of the dock host; their RootWindow is the
    // window itself (docked windows are root windows in the dock tree), so the
    // name comparison is against our own titles.
    return std::strcmp(name, kWindowHierarchy) == 0
        || std::strcmp(name, kWindowViewport)  == 0;
}

void EditorLayer::OpenSceneWithDialog()
{
    std::string loadPath = OpenFileDialog(kSceneFilter, "");
    // RequestLoadScene raises the unsaved-changes modal on a dirty world, else
    // queues the load. Either way the teardown is deferred to
    // ProcessPendingActions, outside the in-flight ImGui frame: inline it would
    // free descriptor slots that ImGui::Image calls already baked into this
    // frame's draw list.
    if (!loadPath.empty() && m_world)
        RequestLoadScene(std::move(loadPath));
}

void EditorLayer::RequestAppExit()
{
    // WM_CLOSE, not PostQuitMessage: App's WM_CLOSE hook routes through
    // RequestExit(), which raises the unsaved-changes modal on a dirty scene.
    if (!m_gfx) return;
    if (HWND hwnd = static_cast<GraphicsDX12&>(*m_gfx).Surface().GetHwnd())
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
}

// ==== Menu bar ====
void EditorLayer::RenderMenuBar()
{
    // Harness: -ui_open_menu=<Menu>@<imgui frame>[,...] holds one top-level menu
    // open from that frame until the next entry's frame, so a -ui_screenshot can
    // show a menu's contents (the harness has no mouse). A menu-bar menu is a
    // popup keyed by its label in the menu bar's ID stack, which is where
    // OpenPopup(label) runs here. No-op without the flag; compiled out of Retail
    // with the rest of the harness.
    auto holdOpen = [this](const char* label)
    {
        if (HarnessMenuHeldOpen(label) && !ImGui::IsPopupOpen(label))
            ImGui::OpenPopup(label);
    };

    holdOpen("File");   RenderFileMenu();
    holdOpen("Edit");   RenderEditMenu();
    holdOpen("Create");
    if (ImGui::BeginMenu("Create"))
    {
        DrawCreateMenuItems();
        ImGui::EndMenu();
    }
    holdOpen("Window"); RenderWindowMenu();
    holdOpen("Tools");  RenderToolsMenu();
    holdOpen("Build");  RenderBuildMenu();
    holdOpen("Debug");  RenderDebugMenu();
    holdOpen("Help");   RenderHelpMenu();

    // ---- Unsaved-changes marker ----
    // With the status bar on, the marker sits next to the scene name
    // (EditorLayer_StatusBar.cpp). With it off (a preference, and always under the
    // harness layout) it trails these menus, so a dirty scene is visible without
    // opening File.
    if (m_sceneDirty && !m_showStatusBar)
    {
        ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f), "  *  (unsaved)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s has unsaved changes  —  Ctrl+S to save",
                              m_currentScenePath.empty()
                                  ? "Untitled scene"
                                  : m_currentScenePath.c_str());
    }
}

// ==== File ====
void EditorLayer::RenderFileMenu()
{
    if (!ImGui::BeginMenu("File")) return;

    // What is open, and whether it matches disk.
    if (m_currentScenePath.empty())
        ImGui::TextDisabled("Untitled scene (never saved)");
    else
        ImGui::TextDisabled("%s", m_currentScenePath.c_str());
    if (m_sceneDirty)
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f), "(unsaved)");
    }
    ImGui::Separator();

    EnsureCommandsRegistered();
    CommandMenuItem("file.new_scene");
    CommandMenuItem("file.open_scene");

    // Most sessions reopen one of a handful of scenes: spare the file dialog.
    // RequestLoadScene keeps every protection (unsaved-changes modal, autosave
    // recovery offer, deferred teardown).
    if (ImGui::BeginMenu("Open Recent", !m_recentScenes.empty()))
    {
        std::string picked;
        for (const std::string& r : m_recentScenes)
            if (ImGui::MenuItem(r.c_str()))
                picked = r;
        ImGui::Separator();
        if (ImGui::MenuItem("Clear list"))
        {
            m_recentScenes.clear();
            SaveEditorPrefs();
        }
        ImGui::EndMenu();
        if (!picked.empty() && m_world)
            RequestLoadScene(std::move(picked));
    }
    ImGui::Separator();

    // The active document first when it is not the scene (Ctrl+S is its Save then). The scene's
    // items are greyed out in Play: the played world is runtime state and Stop restores the
    // pre-Play snapshot over it.
    const bool sceneActive = m_documents.Active().kind == "scene";
    if (!sceneActive)
    {
        CommandMenuItem("doc.save");
        if (m_documents.Active().saveAs) CommandMenuItem("doc.save_as");
        ImGui::Separator();
    }
    // "Save Scene" writes in place once there is a path; only the very first save of an untitled
    // scene prompts (hence the "..." then).
    char saveLabel[96];
    std::snprintf(saveLabel, sizeof(saveLabel), "%s%s", m_currentScenePath.empty() ? "Save Scene..." : "Save Scene",
                  m_sceneDirty ? " *" : "");
    CommandMenuItem("scene.save", saveLabel, sceneActive ? "doc.save" : nullptr);
    CommandMenuItem("scene.save_as", nullptr, sceneActive ? "doc.save_as" : nullptr);
    CommandMenuItem("file.save_all");
    ImGui::Separator();

    // The same list the Content Browser's Import button shows. Every entry
    // only stages files / opens a dialog; the heavy work is deferred.
    if (ImGui::BeginMenu("Import"))
    {
        RenderAssetImportMenuItems();
        ImGui::EndMenu();
    }
    ImGui::Separator();

    CommandMenuItem("file.exit");

    ImGui::EndMenu();
}

// ==== Edit ====
void EditorLayer::RenderEditMenu()
{
    if (!ImGui::BeginMenu("Edit")) return;

    EnsureCommandsRegistered();
    // Undo / Redo act on the active document: the asset tab or panel that had focus, else the
    // scene. Its own name for the entry is in the label ("Undo AI: Edit Collider").
    CommandMenuItem("doc.undo");
    CommandMenuItem("doc.redo");
    ImGui::Separator();
    // The same gates as the shortcuts: copy / paste / duplicate record undo and so are Edit-mode
    // only; delete also works in Play (DeleteEntities has a no-undo branch for it).
    CommandMenuItem("edit.copy");
    CommandMenuItem("edit.paste");
    CommandMenuItem("edit.duplicate");
    CommandMenuItem("edit.delete");
    CommandMenuItem("edit.rename");
    ImGui::Separator();
    CommandMenuItem("edit.select_all");
    CommandMenuItem("edit.deselect");
    ImGui::Separator();
    CommandMenuItem("edit.focus");
    CommandMenuItem("edit.snap_floor");
    ImGui::Separator();
    CommandMenuItem("edit.project_settings");
    CommandMenuItem("edit.preferences");
    ImGui::EndMenu();
}

// ==== Window ====
// The panel table. Labels are the windows' visible titles; windowId is the
// exact Begin() name, "###" form included, so the palette / menu can focus it
// after opening.
std::vector<EditorLayer::PanelEntry> EditorLayer::CollectPanelEntries()
{
    auto flag = [](bool* f) {
        return std::pair<std::function<bool()>, std::function<void(bool)>>(
            [f] { return *f; }, [f](bool v) { *f = v; });
    };
    std::vector<PanelEntry> out;
    out.reserve(32);
    auto add = [&](const char* section, const char* label, const char* id, bool* f,
                   const char* tip = nullptr)
    {
        auto [get, set] = flag(f);
        out.push_back({ section, label, id, std::move(get), std::move(set), tip });
    };
    // A registry panel's row at this place in its section (EditorLayer_Panels.cpp registers them).
    EnsurePanelsRegistered();
    std::vector<std::string> listed;
    auto registryEntry = [](const Editor::PanelDesc& p) {
        const std::string id = p.id;
        return PanelEntry{ p.section.c_str(), p.title.c_str(), p.windowId.c_str(),
                           [id] { return Editor::PanelRegistry::Get().IsOpen(id); },
                           [id](bool v) { Editor::PanelRegistry::Get().SetOpen(id, v); },
                           p.tooltip.empty() ? nullptr : p.tooltip.c_str() };
    };
    auto registered = [&](const char* id) {
        if (const Editor::PanelDesc* p = Editor::PanelRegistry::Get().Find(id))
        {
            out.push_back(registryEntry(*p));
            listed.push_back(id);
        }
    };

    // Core panels. The Viewport is not listed: the engine's render size
    // follows it, so it is never closed.
    add("General", "Hierarchy",       kWindowHierarchy,      &m_showHierarchy);
    add("General", "Inspector",       kWindowInspector,      &m_showInspector);
    add("General", "Content Browser", kWindowContentBrowser, &m_showContentBrowser);
    add("General", "Output Log",      kWindowOutputLog,      &m_showOutputLog);
    registered("core.gameView");
    registered("core.gameView2");
    registered("core.gameView3");

    add("World", "World Settings",  "World Settings",  &m_showWorldSettings,
        "Per-scene settings: scene script, Render Config (.ippc),\n"
        "ambient occlusion and outline tuning.");
    add("World", "Environment",     "Environment",     &m_showEnvironmentWindow);
    add("World", "Post Processing", "Post Processing", &m_showPostProcess);
    registered("core.cameraSwitcher");
    add("World", "Camera Preview",  "Camera Preview",  &m_showCameraPreview);
    add("World", "Split Viewports", "Split Viewports", &m_showSplitViewport);

    add("Animation", "Animation Timeline",   "Animation Timeline",   &m_showTimeline);
    add("Animation", "Sequencer",            "Sequencer",            &m_showSequencer);
    add("Animation", "Animation Set Editor", "Animation Set Editor", &m_showAnimSetEditor);
    out.push_back({ "Animation", "Attack Editor", "Attack Editor", nullptr,
                    [](bool) { AttackEditor::Open(); } });
    // A close from the menu goes through the panel's dirty confirmation, like its
    // title-bar X - hence RequestOpen.
    out.push_back({ "Animation", "Chain Physics Preview", "Chain Physics Preview",
                    [] { return Editor::ChainPhysicsPreviewWindow::Get().IsOpen(); },
                    [](bool v) { Editor::ChainPhysicsPreviewWindow::Get().RequestOpen(v); } });

    add("Content", "VFX Editor",      "VFX Editor###VFX Editor (.ivfx)", &m_showVFXEditor);
    registered("core.decalMaterials");
    registered("core.fontLibrary");
    add("Content", "Asset Preview",   "Asset Preview",   &m_showAssetPreview);
    // A close from the menu asks about unsaved slot edits, like the panel's own X
    // - hence RequestClose.
    out.push_back({ "Content", "Mesh Editor", "###MeshEditor",
                    [] { return Editor::MeshEditorWindow::Get().IsOpen(); },
                    [](bool v)
                    {
                        auto& me = Editor::MeshEditorWindow::Get();
                        if (v) *me.OpenFlag() = true; else me.RequestClose();
                    } });
    // A plain close: unlike the panel's own X, a close from here does not ask
    // about unsaved edits.
    out.push_back({ "Content", "Skeleton Editor", "###SkeletonEditor",
                    [] { return Editor::SkeletonEditorWindow::Get().IsOpen(); },
                    [](bool v) { *Editor::SkeletonEditorWindow::Get().OpenFlag() = v; } });
    // A viewer: nothing to save, a plain close.
    out.push_back({ "Content", "Texture Viewer", "###TextureViewer",
                    [] { return Editor::TextureViewerWindow::Get().IsOpen(); },
                    [](bool v) { *Editor::TextureViewerWindow::Get().OpenFlag() = v; } });
    // Scene tabs edit the scene: nothing of its own to save, a plain close.
    out.push_back({ "Content", "UI Editor", "###UIEditor",
                    [] { return Editor::UIEditorWindow::Get().IsOpen(); },
                    [](bool v) { *Editor::UIEditorWindow::Get().OpenFlag() = v; } });
    // Closing asks when a tab is unsaved.
    out.push_back({ "Content", "Material Editor", "###MaterialEditor",
                    [] { return Editor::MaterialEditorWindow::Get().IsOpen(); },
                    [](bool v)
                    {
                        auto& mw = Editor::MaterialEditorWindow::Get();
                        if (v) *mw.OpenFlag() = true; else mw.RequestClose();
                    } });

    // (No "Lua Console" row: the command line is the Output Log's bottom row -
    // Tools > Lua Script Tools > "Open the Output Log's command line".)
    registered("core.scriptRegistry");
    registered("core.gameFlow");
    add("Scripting & Gameplay", "Input Bindings",  "Input Bindings",
        Editor::InputBindingsWindow::Get().OpenFlag());

    // The GPU timestamp profiler runs while the window is open, through a want
    // held by EditorLayer::SyncProfilerHold. Do not write GPUProfiler::enabled
    // here: it switches the HUD's GPU row off underneath it.
    registered("core.profiler");
    registered("core.poolWatermarks");
    registered("core.loadedGeometry");
    registered("core.loadedTextures");
    registered("core.renderGraph");
    add("Analysis", "Animation Debug", "Animation Debug", &m_showAnimDebug);
    add("Analysis", "Phase Debug",     "Phase Debug",     &m_showPhaseDebug);

    // Every other registered panel - the enabled modules' and any tool registered later - after its
    // section's last row (the menu heads a section once per run of rows), else at the end.
    for (const Editor::PanelDesc& p : Editor::PanelRegistry::Get().All())
    {
        if (std::find(listed.begin(), listed.end(), p.id) != listed.end()) continue;
        auto at = out.end();
        for (auto it = out.begin(); it != out.end(); ++it)
            if (p.section == it->section) at = it + 1;
        out.insert(at, registryEntry(p));
    }
    return out;
}

void EditorLayer::RenderWindowMenu()
{
    if (!ImGui::BeginMenu("Window")) return;

    const char* section = nullptr;
    for (const PanelEntry& e : CollectPanelEntries())
    {
        if (!section || std::strcmp(section, e.section) != 0)
        {
            section = e.section;
            EditorTheme::MenuSection(section);
        }
        if (e.isOpen)
        {
            bool open = e.isOpen();
            const bool was = open;
            if (ImGui::MenuItem(e.label, nullptr, &open))
            {
                e.setOpen(open);
                if (open && !was && e.windowId)   // reopening focuses it
                    ImGui::SetWindowFocus(e.windowId);
            }
        }
        else if (ImGui::MenuItem(e.label))
            e.setOpen(true);
        if (e.tooltip && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", e.tooltip);
    }

    ImGui::Separator();
    RenderWorkspacesMenu();
    CommandMenuItem("layout.reset");

    ImGui::EndMenu();
}

// ==== Tools ====
void EditorLayer::RenderToolsMenu()
{
    if (!ImGui::BeginMenu("Tools")) return;

    EnsureCommandsRegistered();
    CommandMenuItem("tools.palette", "Command Palette...");
    // The control channel, opened in this running editor (not only by -control at launch;
    // Harness/ControlChannel.h). The status bar shows an "AI" chip while it is open.
    if constexpr (Harness::ControlChannel::kCompiled) CommandMenuItem("tools.control_channel");
    ImGui::Separator();

    // Popup bodies live outside the menu-bar context (see the bake popups in
    // SetupDockSpace, after EndMenuBar): modals can't be opened from inside
    // BeginMenuBar because the ID stack hash would not match.
    if (ImGui::BeginMenuEx("Bake", EditorTheme::Icon(FontAwesome::kHammer)))
    {
        if (ImGui::MenuItem("Collision Meshes..."))
            m_bakeCollisionOpenRequest = true;
        // The enabled modules' bake rows (the Nav module's NavMesh...).
        std::vector<Editor::ModuleMenuItem> moduleItems;
        Editor::ModuleRegistry::Get().AddMenuItems(moduleItems);
        for (const Editor::ModuleMenuItem& item : moduleItems)
            if (std::string_view(item.menu) == "Tools/Bake" && ImGui::MenuItem(item.label))
                item.onClick();
        ImGui::Separator();
        if (ImGui::MenuItem("All Reflection Probes", nullptr, false, m_renderer != nullptr))
            m_renderer->BakeAllProbes();
        ImGui::EndMenu();
    }
    ImGui::Separator();

    ImGui::MenuItem("Animation Retarget", nullptr, &m_showRetargetWindow);
    ImGui::MenuItem("Component Transfer", nullptr, &m_showComponentTransfer);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Diff two entities / .ipfb prefabs component by\n"
                          "component and copy the ones you want across,\n"
                          "in either direction. One undo entry per press.");
    if (ImGui::MenuItem("Asset References", nullptr, &m_showReferencesWindow))
    {
        // Opening from the menu (no asset picked yet) still builds the index so
        // the window's stats line and manual query work.
        if (m_showReferencesWindow && !m_refScanner.Built())
            m_refScanner.Rebuild(m_assetDir);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Who uses this asset? Reverse index over every\n"
                          ".iscene / .ipfb / .imat under asset/. Also on\n"
                          "the Content Browser's right-click menu.");
    // asset/scripts/editor/*.lua, run as one console command each (the echo,
    // the result and what it logged land in the Output Log).
    RenderLuaToolsMenu();
    ImGui::Separator();

    // ---- WaterComponent migration ----
    // Converts every WaterComponent in the open world into the mesh-driven form
    // (Plane + Water-domain material + optional ripple component). The dry run
    // only logs what it would do.
    if (ImGui::BeginMenu("Convert Legacy Water"))
    {
        if (ImGui::MenuItem("Dry Run (log only)"))  ConvertLegacyWater(true);
        if (ImGui::MenuItem("Convert"))             ConvertLegacyWater(false);
        ImGui::EndMenu();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "WaterComponent -> Plane mesh + Domain=Water material.\n"
            "Run the dry run first and read the Output Log; then Convert,\n"
            "check the result, and save the scene.");
    ImGui::EndMenu();
}

// ==== Build ====
void EditorLayer::RenderBuildMenu()
{
    if (!ImGui::BeginMenu("Build")) return;

    EditorTheme::MenuSection("Startup Scene");
    if (ImGui::MenuItem("Set Startup Scene..."))
    {
        std::string path = OpenFileDialog(kSceneFilter, "asset/");
        if (!path.empty())
        {
            // game.json's startup_scene must be project-root-relative.
            WriteStartupSceneToManifest(Sys::RelativizeAssetPath(path));
        }
    }
    if (ImGui::MenuItem("Reset Startup Scene (Base Scene)"))
        WriteStartupSceneToManifest(Scene::kBaseScenePath);

    // Configurations offered here. DX12Engine.sln is the source of truth:
    // tools/package_game.py reads it via tools/buildconfigs.py. Update this list by
    // hand when a configuration is added (the editor has no .sln parser). Retail is
    // what a player receives: no harness, no test bindings, no -exec; Release
    // carries the agent/CI harness. The suffix is on the menu because both produce
    // a Game.exe (in different directories) and picking the wrong one is silent.
    struct PackageConfig { const char* cfg; const char* note; };
    static constexpr PackageConfig kPackageConfigs[] = {
        { "Retail",  " - shipping, no harness" },
        { "Release", " - with test harness" },
        { "Debug",   "" },
    };
    EditorTheme::MenuSection("Package Game");
    for (const PackageConfig& pc : kPackageConfigs)
    {
        char label[80];
        std::snprintf(label, sizeof(label), "%s%s", pc.cfg, pc.note);
        if (ImGui::MenuItemEx(label, EditorTheme::Icon(FontAwesome::kBoxArchive)))
        {
            // A separate console keeps the editor responsive; /k keeps it open
            // after exit so build errors stay visible.
            char cmd[512];
            std::snprintf(cmd, sizeof(cmd),
                "/k cd /d \"%%CD%%\" && python tools\\package_game.py --config %s", pc.cfg);
            ShellExecuteA(nullptr, "open", "cmd.exe", cmd, nullptr, SW_SHOWNORMAL);
        }
    }
    ImGui::Separator();
    // The packager's --out default: DX12/build/package/<config>/ (cwd is DX12/).
    // build/ itself also holds the compiler trees (ide/, pkg/, ci/), which is
    // not what "output" means to the person clicking this.
    if (ImGui::MenuItemEx("Open Output Folder", EditorTheme::Icon(FontAwesome::kExternal)))
        OpenInExplorer("build\\package");
    ImGui::EndMenu();
}

// ==== Viewport > Show ====
// What the viewport draws on top of the scene; the viewport toolbar's Show
// button draws these items. State: DebugDrawSystem's category mask (the single
// source of truth App pushes onto the renderer each frame) plus the editor's
// grid / canvas-bounds / probe-viz flags.
void EditorLayer::DrawViewportShowMenuItems()
{
    EditorTheme::MenuSection("Viewport");
    ImGui::MenuItem("Grid", nullptr, &m_showEditorGrid);
    ImGui::MenuItem("View Cube", nullptr, &m_showViewCube);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The cube in the viewport's corner: click a face for that ortho view,\n"
                          "drag it to turn the camera (Edit mode).");
    // The selection's skeleton (UE's Show > Bones).
    ImGui::MenuItem("Skeleton (selection)", nullptr, &m_showSelectionSkeleton);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The selected character's skeleton in its live pose (virtual bones violet)\n"
                          "and its sockets as small axis tripods - a Static Mesh's mesh sockets too.\n"
                          "Faded where the scene is in front of it.");
    ImGui::MenuItem("Screen UI", nullptr, &m_showScreenUI);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The game's Canvas UI over the scene (Edit mode; Play and F11 always show it).\n"
                          "Edit it in Window > Content > UI Editor.");
    ImGui::MenuItem("UI Rect Tool", nullptr, &m_uiRectToolInViewport);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Drag the selected Canvas UI node's rect in this viewport.\n"
                          "Off by default: the UI Editor is the full tool (handles, snapping, anchors).");
    ImGui::MenuItem("UI Canvas Bounds", nullptr, &m_showUICanvasWireframe);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Cyan: layout bounds\n"
            "Green: raycast target\n"
            "Purple: Canvas root\n"
            "Red: hidden / clipped\n"
            "Orange: effective clip\n"
            "Yellow: selected bounds + pivot");

    DebugDrawSystem* dd = m_debugDraw;
    if (!dd) return;

    EditorTheme::MenuSection("Gizmos & Icons");
    // Master switch for every reflection probe's viz ball. ANDed with the
    // per-probe ReflectionProbeComponent::showVizSphere and pushed onto
    // VisibilityComponent in OnUIRender; baking is unaffected.
    ImGui::MenuItem("Reflection Probe Viz", nullptr, &m_showProbeVizSpheres);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Master switch for the reflection-probe debug balls.\n"
                          "Per-probe override: Inspector > Reflection Probe >\n"
                          "Show Viz Sphere. Probe capture keeps running either way.");
    // Debug icon gizmos: one toggle per registered icon kind (from
    // DebugDrawSystem's kIconKinds[], so a new kind appears here automatically).
    ImGui::MenuItem("Icons", nullptr, &dd->iconsEnabled);
    ImGui::BeginDisabled(!dd->iconsEnabled);
    ImGui::DragFloat("Icon distance", &dd->iconMaxDistance, 1.f, 1.f, 10000.f,
                     "%.0f m", ImGuiSliderFlags_AlwaysClamp);
    ImGui::DragFloat("Icon size", &dd->iconSizeScale, 0.01f, 0.1f, 3.f,
                     "%.2f x", ImGuiSliderFlags_AlwaysClamp);
    for (int i = 0; i < DebugDrawSystem::IconKindCount(); ++i)
    {
        const DebugDrawSystem::IconKindInfo info = DebugDrawSystem::GetIconKindInfo(i);
        bool on = DebugCatEnabled(dd->categoryMask, info.category);
        char label[96];
        std::snprintf(label, sizeof(label), "    %s", info.name);
        if (ImGui::MenuItem(label, nullptr, &on))
            DebugCatSet(dd->categoryMask, info.category, on);
    }
    ImGui::EndDisabled();

    EditorTheme::MenuSection("Debug Wireframes");
    ImGui::MenuItem("Wireframes", nullptr, &dd->masterEnabled);
    ImGui::BeginDisabled(!dd->masterEnabled);
    auto catItem = [&](const char* label, DebugCategory cat)
    {
        bool on = DebugCatEnabled(dd->categoryMask, cat);
        if (ImGui::MenuItem(label, nullptr, &on))
            DebugCatSet(dd->categoryMask, cat, on);
    };
    catItem("    AABBs",             DebugCategory::AABB);
    catItem("    Frustum",           DebugCategory::Frustum);
    catItem("    Light Ranges",      DebugCategory::LightRanges);
    catItem("    Capsules",          DebugCategory::Capsules);
    catItem("    Reflection Probes", DebugCategory::ReflectionProbes);
    catItem("    DDGI Volumes",      DebugCategory::DDGIVolumes);
    catItem("    Decals",            DebugCategory::Decals);
    catItem("    Post-Process Volumes", DebugCategory::PostProcessVolumes);
    catItem("    Fog Volumes",       DebugCategory::FogVolumes);
    catItem("    Collision",         DebugCategory::Collision);
#ifdef JPH_DEBUG_RENDERER
    catItem("    Physics Bodies",    DebugCategory::Physics);
#endif
    catItem("    NavMesh",           DebugCategory::NavMesh);
    catItem("    Nav Soup",          DebugCategory::NavSoup);
    if (DebugCatEnabled(dd->categoryMask, DebugCategory::Collision) ||
        DebugCatEnabled(dd->categoryMask, DebugCategory::NavSoup))
    {
        // 0 = unlimited (every Mesh collider in the world). Dial down when the
        // wire buffer fills (AddLine bails silently past kMaxVertices). The nav
        // soup shares it.
        ImGui::SetNextItemWidth(160);
        EditorUI::DragFloat("Collision / soup distance (0 = all)",
                            &dd->collisionMaxDistance, 0.0f, 200.0f, "%.0f m");
    }
    if (DebugCatEnabled(dd->categoryMask, DebugCategory::Collision))
    {
        // Tools::CollisionMesh::EmitDebugWireframe's colours: what the BODY is.
        ImGui::TextDisabled("Collision: tan = static, blue = moving, yellow dashed = trigger,");
        ImGui::TextDisabled("  red = HurtBox, orange = AttackBox, purple = other layer, white = selected");
    }
    if (DebugCatEnabled(dd->categoryMask, DebugCategory::NavSoup))
    {
        // The soup is Recast's INPUT: green = walkable under the bake's slope
        // limit, red = too steep / facing down. Only static ground goes in
        // (no triggers, moving bodies or other layers: BuildSoupFromWorldColliders).
        ImGui::TextDisabled("Nav soup: green = walkable, red = too steep");
        ImGui::TextDisabled("(the last bake's input: Tools > Bake > NavMesh... > Build)");
    }
    ImGui::EndDisabled();
}

// ==== Debug ====
void EditorLayer::RenderDebugMenu()
{
    if (!ImGui::BeginMenu("Debug")) return;

    // ---- Renderer diagnostics ----
    if (m_renderer && ImGui::BeginMenu("Renderer"))
    {
        if (auto* pdbg = m_renderer->GetDDGIProbeDebugPass())
        {
            ImGui::MenuItem("DDGI Probe Spheres", nullptr, &pdbg->enabled);
            if (pdbg->enabled)
            {
                static const char* kModes[] = {
                    "0: SH irradiance (default)",
                    "1: grid-coord color",
                    "2: SH L0 magnitude (verify SH has data)",
                    "3: SH L1 direction (verify SH evolves)" };
                int mode = static_cast<int>(pdbg->debugMode);
                if (ImGui::Combo("  Debug Mode", &mode, kModes, IM_ARRAYSIZE(kModes)))
                    pdbg->debugMode = static_cast<uint32_t>(mode);
                ImGui::DragFloat("  Sphere Radius", &pdbg->sphereRadius, 0.005f, 0.01f, 5.0f, "%.3f");
            }
        }
        // Decal cluster heatmap: green / yellow / red overlay for overdraw and
        // bounds-sphere sanity.
        if (DecalPass* dp = m_renderer->GetDecalPass())
        {
            bool heat = dp->GetDebugHeatmap();
            if (ImGui::MenuItem("Decal Cluster Heatmap", nullptr, &heat))
                dp->SetDebugHeatmap(heat);
        }
        ImGui::EndMenu();
    }

    // ---- Tracer test mode ----
    // T fires one along the view ray; the burst entry stress-tests the ring cursor.
    if (m_renderer && ImGui::BeginMenu("Tracers"))
    {
        if (TracerSystem* ts = m_renderer->GetTracerSystem())
        {
            using DirectX::XMFLOAT3;
            using DirectX::XMFLOAT4;
            ImGui::MenuItem("Tracer Test Mode (T to fire)", nullptr, &m_tracerTestModeEnabled);
            const XMFLOAT3& cp = m_renderView.cameraPosition;
            const XMFLOAT3& cf = m_renderView.cameraForward;
            const XMFLOAT3 start{ cp.x + cf.x * 0.5f, cp.y + cf.y * 0.5f, cp.z + cf.z * 0.5f };
            if (ImGui::MenuItem("Test Spawn Tracer (one-shot)"))
            {
                const XMFLOAT3 end{ cp.x + cf.x * 30.0f, cp.y + cf.y * 30.0f, cp.z + cf.z * 30.0f };
                ts->Spawn(start, end, XMFLOAT4{ 1.0f, 0.85f, 0.3f, 6.0f },
                          /*width=*/0.5f, /*lifetime=*/0.8f);
            }
            if (ImGui::MenuItem("Test Spawn Tracer Burst (32)"))
            {
                // 32-shot fan around the view ray: stresses the ring cursor and the
                // depth-fade math. Rainbow colour by index for identification.
                for (int k = 0; k < 32; ++k)
                {
                    const float ang    = static_cast<float>(k) * 0.196f;  // ~11.25 deg steps
                    const float spread = 4.0f;
                    const XMFLOAT3 end{ cp.x + cf.x * 25.0f + std::cos(ang) * spread,
                                        cp.y + cf.y * 25.0f + std::sin(ang) * spread,
                                        cp.z + cf.z * 25.0f };
                    const float h = static_cast<float>(k) / 32.0f;
                    const XMFLOAT4 col{ 0.5f + 0.5f * std::cos(6.28f * h),
                                        0.5f + 0.5f * std::cos(6.28f * (h + 0.33f)),
                                        0.5f + 0.5f * std::cos(6.28f * (h + 0.66f)),
                                        5.0f };
                    ts->Spawn(start, end, col, 0.04f, 0.6f);
                }
            }
        }
        ImGui::EndMenu();
    }
    ImGui::EndMenu();
}

// ==== Help ====
void EditorLayer::RenderHelpMenu()
{
    if (!ImGui::BeginMenu("Help")) return;
    EnsureCommandsRegistered();
    CommandMenuItem("help.shortcuts");
    CommandMenuItem("help.about");
    ImGui::Separator();
    // Paths are relative to DX12/, the editor's working directory.
    if (ImGui::MenuItemEx("Open Log Folder", EditorTheme::Icon(FontAwesome::kTerminal)))
        OpenInExplorer("log");
    if (ImGui::MenuItemEx("Open Docs Folder", EditorTheme::Icon(FontAwesome::kExternal)))
        OpenInExplorer("docs");
    if (ImGui::MenuItemEx("Open Project Folder", EditorTheme::Icon(FontAwesome::kFolderOpen)))
        OpenInExplorer(".");
    ImGui::EndMenu();
}
