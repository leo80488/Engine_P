// EditorLayer_Panels.cpp - the core's tool panels in Editor::PanelRegistry, the modules' rows forwarded
// to it, their open states and the workspaces as preferences, the registry's lifecycle events at the
// frame boundary, and the Window menu's Workspaces submenu.

#include "Editor/EditorLayer.h"

#include "Editor/EditorModule.h"
#include "Editor/EditorPanels.h"
#include "Editor/GameView.h"
#include "Editor/RenderGraphWindow.h"
#include "Scripting/LuaDoc.h"
#include "System/CommandLine.h"
#include "System/Log.h"

#include "imgui/imgui.h"

#include <sol/sol.hpp>

#include <cstring>
#include <string>
#include <vector>

void EditorLayer::EnsurePanelsRegistered()
{
    if (m_panelsRegistered) return;
    m_panelsRegistered = true;
    Editor::PanelRegistry& reg = Editor::PanelRegistry::Get();
    InstallServices();

    // A core tool panel: its flag stays EditorLayer's, its draw is the Render* function.
    auto tool = [&](const char* id, const char* section, const char* title, bool* flag, std::function<void()> draw,
                    const char* tip = "", std::vector<std::string> holds = {}, bool focusOnHold = false)
    {
        Editor::PanelDesc p;
        p.id = id;
        p.title = title;
        p.windowId = title;
        p.section = section;
        p.tooltip = tip;
        p.isOpen = [flag] { return *flag; };
        p.setOpen = [flag](bool v) { *flag = v; };
        p.draw = std::move(draw);
        p.holdNames = std::move(holds);
        p.focusOnHold = focusOnHold;
        reg.Register(std::move(p));
    };
    tool("core.cameraSwitcher", "World", "Camera Switcher", &m_showCameraSwitcher, [this] { RenderCameraSwitcherWindow(); });
    tool("core.decalMaterials", "Content", "Decal Materials", &m_showDecalMaterials, [this] { RenderDecalMaterialsWindow(); });
    tool("core.fontLibrary", "Content", "Font Library", &m_showFontEditor, [this] { RenderFontEditorWindow(); });
    tool("core.scriptRegistry", "Scripting & Gameplay", "Script Registry", &m_showScriptRegistry,
         [this] { RenderScriptRegistryWindow(); });
    tool("core.gameFlow", "Scripting & Gameplay", "Game Flow", &m_showGameFlow, [this] { RenderGameFlowWindow(); });
    // The GPU timestamp profiler runs while the window is open, through a want held by
    // EditorLayer::SyncProfilerHold (called every frame before the registry draws). Do not write
    // GPUProfiler::enabled here: it switches the HUD's GPU row off underneath it.
    tool("core.profiler", "Analysis", "Profiler", &m_showProfiler, [this] { RenderProfilerPanel(); });
    tool("core.poolWatermarks", "Analysis", "Pool Watermarks", &m_showPoolWatermarks, [this] { RenderPoolWatermarkPanel(); },
         "Fixed-capacity pool usage vs its kMax* constant.\n"
         "Every pool here truncates SILENTLY on the GPU side: past the cap\n"
         "the objects simply stop being drawn, which on screen is\n"
         "indistinguishable from a bad mesh load or an over-aggressive cull.");
    tool("core.loadedGeometry", "Analysis", "Loaded Geometry", &m_showLoadedGeometry, [this] { RenderLoadedGeometryWindow(); },
         "Every loaded .meshlib: who holds it (world, asset preview,\n"
         "Play's snapshot pin), its vertex + index buffer size, how many\n"
         "entities draw from it and whether its identity is its own.",
         { "Loaded Geometry" });
    tool("core.byteView", "Analysis", "Byte View", &m_showByteView, [this] { RenderByteViewWindow(); },
         "A component's or an asset file's bytes, read-only, with the reflected fields named.\n"
         "Opened from a component header's or an asset's right-click View Bytes.");
    if (Editor::PanelDesc* bytes = reg.Find("core.byteView"))
        bytes->onFrameBoundary = [this] { TickByteView(); };
    tool("core.jobs", "Analysis", "Jobs", &m_showJobs, [this] { RenderJobsPanel(); },
         "Every job the editor tracks - imports, saves, UI test runs, bakes - with its state,\n"
         "progress and error. A job that finishes posts a notice.");
    if (Editor::PanelDesc* jobs = reg.Find("core.jobs"))
        jobs->onFrameBoundary = [this] { TickJobNotices(); };
    tool("core.loadedTextures", "Analysis", "Loaded Textures", &m_showLoadedTextures, [this] { RenderLoadedTexturesWindow(); },
         "Every texture the TextureSystem holds: size, format, mips, VRAM,\n"
         "references, the entities that sample it, its state. Filter for\n"
         "the heavy, the uncompressed, the mip-less and the failed ones.",
         { "Loaded Textures" }, /*focusOnHold*/ true);
    for (int i = 0; i < Editor::GameView::kMaxPanels; ++i)
    {
        // The game as the player sees it, beside the Scene viewport (Editor/GameView.h): up to three, each with
        // its own framing and camera.
        Editor::GameView& gv = Editor::GameView::Get();
        Editor::PanelDesc p;
        p.id = Editor::GameView::kPanelIds[i];
        p.title = Editor::GameView::kWindowIds[i];
        p.windowId = Editor::GameView::kWindowIds[i];
        p.section = "General";
        p.tooltip = "A camera and the HUD at a chosen aspect or resolution, beside the Scene viewport.\n"
                    "In Play the game gets input only while the focused Game View that shows the game\n"
                    "camera has focus; the Scene viewport keeps the editor camera.";
        p.isOpen = [&gv, i] { return gv.IsOpen(i); };
        p.setOpen = [&gv, i](bool v) { gv.SetOpen(v, i); };
        p.draw = [this, i] { DrawGameViewPanel(i); };
        reg.Register(std::move(p));
    }
    {
        // Drawn every frame by its own call (a harness hold opens it, closing releases): listed, not drawn.
        Editor::PanelDesc p;
        p.id = "core.renderGraph";
        p.title = "Render Graph";
        p.windowId = "Render Graph";
        p.section = "Analysis";
        p.tooltip = "The main render graph's last frame, read-only: its passes by stage, what each\n"
                    "declared and the barriers it got, the textures and their states, the frame's\n"
                    "command lists. A capture is taken only while this window is open.";
        p.isOpen = [] { return Editor::RenderGraphWindow::Get().IsOpen(); };
        p.setOpen = [](bool v) { Editor::RenderGraphWindow::Get().SetOpen(v); };
        reg.Register(std::move(p));
    }

    // The enabled modules' rows: drawn by their module (DrawPanels), listed by the registry.
    std::vector<Editor::ModulePanel> modulePanels;
    Editor::ModuleRegistry::Get().AddPanels(modulePanels);
    for (Editor::ModulePanel& mp : modulePanels)
    {
        Editor::PanelDesc p;
        p.id = std::string("module.") + mp.label;
        p.title = mp.label;
        p.windowId = mp.windowId ? mp.windowId : mp.label;
        p.section = mp.section;
        p.tooltip = mp.tooltip ? mp.tooltip : "";
        p.isOpen = std::move(mp.isOpen);
        p.setOpen = std::move(mp.setOpen);
        reg.Register(std::move(p));
    }

    // The saved open states and workspaces. A harness run's layout is the default one (packs measure
    // it): the preferences are not applied.
    if (!Sys::CommandLine::Get().Has("-unattended"))
    {
        reg.ApplyOpenStates(m_panelPrefs);
        reg.ReadWorkspaces(m_panelPrefs);
    }
}

void EditorLayer::TickPanelLifecycle()
{
    if (!m_panelsRegistered) return;
    Editor::PanelRegistry& reg = Editor::PanelRegistry::Get();
    reg.FrameBoundary();

    const std::uint64_t epoch = m_world ? m_world->Epoch() : 0;
    if (m_world != m_panelSeenWorld || epoch != m_panelSeenEpoch)
    {
        m_panelSeenWorld = m_world;
        m_panelSeenEpoch = epoch;
        reg.WorldChanged();
    }
    const int play = static_cast<int>(GetPlayState());
    if (play != m_panelSeenPlayState)
    {
        m_panelSeenPlayState = play;
        reg.PlayModeChanged();
    }
    if (m_selectedEntities != m_panelSeenSelection)
    {
        m_panelSeenSelection = m_selectedEntities;
        ++m_selectionEpoch;
        reg.SelectionChanged();
    }
}

void EditorLayer::RenderWorkspacesMenu()
{
    Editor::PanelRegistry& reg = Editor::PanelRegistry::Get();
    if (!ImGui::BeginMenu("Workspaces")) return;
    std::string load, remove;
    for (const Editor::Workspace& w : reg.Workspaces())
    {
        if (ImGui::MenuItem(w.name.c_str())) load = w.name;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%zu panel(s) open; the dock layout as it was saved", w.open.size());
    }
    if (reg.Workspaces().empty()) ImGui::TextDisabled("(none saved)");
    ImGui::Separator();
    ImGui::SetNextItemWidth(160.f);
    ImGui::InputTextWithHint("##workspaceName", "Name", m_workspaceName, sizeof(m_workspaceName));
    ImGui::SameLine();
    const bool named = m_workspaceName[0] != '\0';
    if (ImGui::MenuItem("Save Current", nullptr, false, named))
    {
        std::size_t size = 0;
        const char* ini = ImGui::SaveIniSettingsToMemory(&size);
        reg.SaveWorkspace(m_workspaceName, std::string(ini ? ini : "", size));
        LOG_INFO("Editor: saved workspace '%s'", m_workspaceName);
        SaveEditorPrefs();
    }
    if (!reg.Workspaces().empty() && ImGui::BeginMenu("Delete"))
    {
        for (const Editor::Workspace& w : reg.Workspaces())
            if (ImGui::MenuItem(w.name.c_str())) remove = w.name;
        ImGui::EndMenu();
    }
    ImGui::EndMenu();

    if (!load.empty())
    {
        std::string layout;
        if (reg.OpenWorkspace(load, layout))
        {
            // The layout is loaded between frames (BeginImGuiFrame), never mid-frame.
            m_pendingWorkspaceLayout = std::move(layout);
            LOG_INFO("Editor: opened workspace '%s'", load.c_str());
        }
    }
    if (!remove.empty())
    {
        reg.DeleteWorkspace(remove);
        SaveEditorPrefs();
    }
}

void EditorLayer::RegisterPanelLua(lua_State* L)
{
    sol::state_view lua(L);
    sol::table ed = lua["Editor"].get_or_create<sol::table>();
    LuaDoc::Fn(ed, "Editor", "Panels",
        [this](sol::this_state ts) -> sol::table
        {
            EnsurePanelsRegistered();
            sol::state_view view(ts);
            sol::table out = view.create_table();
            int i = 1;
            for (const Editor::PanelDesc& p : Editor::PanelRegistry::Get().All())
            {
                sol::table row = view.create_table();
                row["id"] = p.id;
                row["title"] = p.title;
                row["section"] = p.section;
                row["open"] = Editor::PanelRegistry::Get().IsOpen(p);
                out[i++] = row;
            }
            return out;
        },
        { .doc = "The registered panels (Editor::PanelRegistry): the core's tool panels and the enabled modules' - "
                 "what the Window menu lists after the fixed rows.",
          .returns = "table", .returnsDoc = "{{id, title, section, open}, ...}",
          .effect = LuaDoc::Effect::Read, .context = LuaDoc::Context::Editor });
    LuaDoc::Fn(ed, "Editor", "SetPanelOpen",
        [this](const std::string& id, bool open) -> bool
        {
            EnsurePanelsRegistered();
            if (!Editor::PanelRegistry::Get().Find(id))
                throw sol::error("Editor.SetPanelOpen: not_found: no panel '" + id + "' (Editor.Panels lists them)");
            Editor::PanelRegistry::Get().SetOpen(id, open);
            return Editor::PanelRegistry::Get().IsOpen(id);
        },
        { .doc = "Opens or closes a registered panel as its Window-menu row does.",
          .params = { { "id", "a panel id from Editor.Panels, e.g. \"core.jobs\"", "string" },
                      { "open", "true to open", "boolean" } },
          .returns = "boolean", .returnsDoc = "whether it is open now",
          .effect = LuaDoc::Effect::World, .context = LuaDoc::Context::Editor, .errors = { "not_found" } });
}
