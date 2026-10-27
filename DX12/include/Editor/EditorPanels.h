#pragma once

// EditorPanels - the editor's panel registry. A panel registers once - a stable id, its title and
// Begin() name, its Window-menu section, how it draws, its lifecycle hooks - and the Window menu, the
// command palette, the open-state preferences, the workspaces and the frame loop read the registry:
// adding a tool panel touches no central list. The core registers its tool panels at startup
// (EditorLayer::RegisterCorePanels), an engine module its own (IEditorModule::AddPanels, forwarded
// here). UI thread.

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace Editor
{
    struct PanelDesc
    {
        std::string id;         // stable: "core.profiler" (the panel.<id> preference, workspaces)
        std::string title;      // the Window-menu row: the window's visible title
        std::string windowId;   // the exact Begin() name, "###" form included (focus after opening)
        std::string section;    // the Window-menu section it is listed under
        std::string tooltip;

        // The open flag. A panel that keeps its own (a window object, an m_show flag) binds both;
        // otherwise the registry holds it.
        std::function<bool()>     isOpen;
        std::function<void(bool)> setOpen;
        bool                      defaultOpen = false;
        bool                      persistOpen = true;   // its open state is an editor preference

        // Draws the window, its own Begin / End, while open; null for a panel drawn elsewhere (a
        // module's DrawPanels).
        std::function<void()> draw;
        // Every frame at the frame boundary, open or not.
        std::function<void()> onFrameBoundary;
        std::function<void()> onOpened;
        std::function<void()> onClosed;
        std::function<void()> onWorldChanged;
        std::function<void()> onPlayModeChanged;
        std::function<void()> onSelectionChanged;
        std::function<void()> onShutdown;

        // Harness: a -ui_open_menu hold on any of these names opens it (focused, with focusOnHold).
        std::vector<std::string> holdNames;
        bool                     focusOnHold = false;
    };

    // A saved arrangement: the panels it had open and the dock layout (imgui.ini text).
    struct Workspace
    {
        std::string              name;
        std::vector<std::string> open;
        std::string              layout;
    };

    class PanelRegistry
    {
    public:
        static PanelRegistry& Get();

        // A desc with the same id replaces the earlier one (its open state kept).
        void Register(PanelDesc desc);
        const std::vector<PanelDesc>& All() const { return m_panels; }
        const PanelDesc* Find(const std::string& id) const;
        PanelDesc*       Find(const std::string& id);

        bool IsOpen(const PanelDesc& p) const;
        bool IsOpen(const std::string& id) const;
        // Runs onOpened / onClosed when the state changes.
        void SetOpen(const std::string& id, bool open);

        // The frame loop: DrawOpen in the UI pass, FrameBoundary at the frame boundary; the events
        // at the boundary after the editor saw them change; Shutdown once, before the editor goes.
        // @p held answers a harness -ui_open_menu hold by name (null = none).
        void DrawOpen(const std::function<bool(const char*)>& held = {});
        void FrameBoundary();
        void WorldChanged();
        void PlayModeChanged();
        void SelectionChanged();
        void Shutdown();

        // Open states as editor preferences: "panel.<id>" = "1" / "0", written only when they differ
        // from defaultOpen (editor_prefs.ini is tracked).
        void ApplyOpenStates(const std::map<std::string, std::string>& prefs);
        void WriteOpenStates(std::map<std::string, std::string>& prefs) const;

        // Workspaces: kept in the editor preferences, loaded by name.
        std::vector<Workspace>&       Workspaces()       { return m_workspaces; }
        const std::vector<Workspace>& Workspaces() const { return m_workspaces; }
        // The current arrangement under @p name (replacing one of that name); @p layout is the
        // dock layout text (ImGui::SaveIniSettingsToMemory).
        void SaveWorkspace(const std::string& name, std::string layout);
        // Opens exactly the workspace's panels; its layout is returned for the caller to load before
        // the next ImGui frame. False when there is no such workspace.
        bool OpenWorkspace(const std::string& name, std::string& layout);
        void DeleteWorkspace(const std::string& name);
        // workspace.<name>.open = id,id ; workspace.<name>.layout = the escaped layout.
        void ReadWorkspaces(const std::map<std::string, std::string>& prefs);
        void WriteWorkspaces(std::map<std::string, std::string>& prefs) const;

        // Test seam: forget every panel and workspace.
        void Clear();

    private:
        std::size_t IndexOf(const std::string& id) const;

        std::vector<PanelDesc> m_panels;
        std::vector<char>      m_ownOpen;   // the registry's flag, for a panel without isOpen / setOpen
        std::vector<Workspace> m_workspaces;
    };
}
