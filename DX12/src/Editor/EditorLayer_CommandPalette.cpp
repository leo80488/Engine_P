// Tools > Command Palette (Ctrl+Shift+P): one search box over every panel and
// every editor command. Commands come from the registry (Editor/EditorCommands.h)
// the menus and the shortcuts read, panels from CollectPanelEntries (the Window
// menu's table), so none of them can disagree. Ranked by Editor::Fuzzy over
// "Category: Name" and the command's keywords.
//
// Commands run inside the popup exactly like their menu items, so the same rules
// apply: GPU teardown only queues (m_pending*), and a modal (the bake dialogs) is
// requested through the same flag the menu sets.
#include "Editor/EditorLayer.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorModule.h"           // the enabled modules' Tools rows
#include "Editor/FuzzyMatch.h"

#include "imgui/imgui.h"

#include <algorithm>
#include <cfloat>
#include <functional>
#include <string>
#include <vector>

namespace
{
    struct PaletteEntry
    {
        std::string           label;      // "Category: Name" - what the search ranks
        std::string           keywords;   // more words it matches
        std::string           shortcut;   // display only
        bool                  enabled = true;
        std::function<void()> run;
        int                   score = 0;
    };
}

void EditorLayer::RenderCommandPalette()
{
    static char s_query[128] = {};
    static int  s_selected   = 0;

    // Harness: -ui_open_menu=Command Palette@<frame> opens it for a screenshot.
    if (HarnessMenuHeldOpen("Command Palette") && !ImGui::IsPopupOpen("##CommandPalette"))
        m_commandPaletteRequest = true;
    if (m_commandPaletteRequest)
    {
        m_commandPaletteRequest = false;
        s_query[0] = '\0';
        s_selected = 0;
        ImGui::OpenPopup("##CommandPalette");
    }
    if (!ImGui::IsPopupOpen("##CommandPalette"))
        return;

    // ---- The list (rebuilt only while the palette is open) ----
    EnsureCommandsRegistered();
    std::vector<PaletteEntry> cmds;
    cmds.reserve(128);
    auto add = [&](std::string label, std::string keywords, std::string shortcut, bool enabled,
                   std::function<void()> fn) {
        cmds.push_back({ std::move(label), std::move(keywords), std::move(shortcut), enabled, std::move(fn) });
    };

    // Panels: "Window: X" opens and focuses (never closes - the palette is for getting somewhere).
    for (PanelEntry& e : CollectPanelEntries())
        add(std::string("Window: ") + e.label, "panel", std::string(), true, [set = e.setOpen, id = e.windowId] {
            set(true);
            if (id) ImGui::SetWindowFocus(id);
        });

    // Every command the menus and the shortcuts read.
    auto& reg = Editor::CommandRegistry::Get();
    for (const Editor::Command& c : reg.All())
    {
        std::string label = c.category + ": " + reg.LabelOf(c);
        if (c.checked) label += c.checked() ? "  (on)" : "  (off)";
        add(std::move(label), c.keywords, Editor::FormatChord(reg.ChordOf(c)), reg.IsEnabled(c),
            [id = c.id] { Editor::CommandRegistry::Get().Execute(id); });
    }

    for (const std::string& r : m_recentScenes)
        add("File: Open Recent: " + r, "scene", std::string(), true, [this, r] { RequestLoadScene(r); });
    if (!m_luaToolsScanned) ScanLuaTools();
    for (const std::string& t : m_luaTools)
        add("Lua Script Tool: " + t, "script", std::string(), true,
            [this, t] { RunLuaScriptTool(std::string("asset/scripts/editor/") + t); });
    std::vector<Editor::ModuleMenuItem> moduleItems;
    Editor::ModuleRegistry::Get().AddMenuItems(moduleItems);
    for (const Editor::ModuleMenuItem& item : moduleItems)
        add(std::string("Tools: ") + item.paletteLabel, std::string(), std::string(), true, item.onClick);

    // ---- Popup ----
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float width = std::min(640.0f, vp->WorkSize.x * 0.7f);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + (vp->WorkSize.x - width) * 0.5f,
                                   vp->WorkPos.y + 48.0f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f));
    if (!ImGui::BeginPopup("##CommandPalette", ImGuiWindowFlags_NoMove))
        return;

    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool enter = ImGui::InputTextWithHint("##paletteQuery",
        "Type a panel or command  (e.g. \"profiler\", \"save as\", \"grid\")",
        s_query, sizeof(s_query), ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemEdited()) s_selected = 0;

    // Ranked: the label, then the keywords at a discount; ties keep the registry's order.
    std::vector<int> visible;
    for (int i = 0; i < static_cast<int>(cmds.size()); ++i)
    {
        const Editor::Fuzzy::Match m = Editor::Fuzzy::Score(cmds[i].label, s_query);
        const Editor::Fuzzy::Match k = Editor::Fuzzy::Score(cmds[i].label + " " + cmds[i].keywords, s_query);
        if (!m.matched && !k.matched) continue;
        cmds[i].score = m.matched ? m.score : k.score - 4;
        visible.push_back(i);
    }
    if (s_query[0])
        std::stable_sort(visible.begin(), visible.end(),
                         [&](int a, int b) { return cmds[a].score > cmds[b].score; });

    const int n = static_cast<int>(visible.size());
    bool moved = false;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) { s_selected = std::min(s_selected + 1, n - 1); moved = true; }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))   { s_selected = std::max(s_selected - 1, 0);     moved = true; }
    s_selected = std::clamp(s_selected, 0, std::max(n - 1, 0));

    int runIndex = -1;
    const float rowH = ImGui::GetTextLineHeightWithSpacing();
    const float listH = rowH * static_cast<float>(std::clamp(n, 1, 14)) + ImGui::GetStyle().WindowPadding.y;
    if (ImGui::BeginChild("##paletteList", ImVec2(0.0f, listH)))
    {
        if (n == 0)
            ImGui::TextDisabled("No command matches \"%s\"", s_query);
        for (int row = 0; row < n; ++row)
        {
            const PaletteEntry& c = cmds[visible[row]];
            ImGui::PushID(row);
            const float rowRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
            ImGui::BeginDisabled(!c.enabled);
            if (ImGui::Selectable(c.label.c_str(), row == s_selected,
                                  ImGuiSelectableFlags_AllowDoubleClick))
                runIndex = visible[row];
            ImGui::EndDisabled();
            if (row == s_selected && moved)
                ImGui::SetScrollHereY();
            if (!c.shortcut.empty())
            {
                const float sw = ImGui::CalcTextSize(c.shortcut.c_str()).x;
                ImGui::SameLine(rowRight - sw);
                ImGui::TextDisabled("%s", c.shortcut.c_str());
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::TextDisabled("Up / Down to move  ·  Enter to run  ·  Esc to close");

    if (enter && n > 0)
        runIndex = visible[s_selected];
    if (runIndex >= 0 && cmds[runIndex].enabled)
    {
        ImGui::CloseCurrentPopup();
        cmds[runIndex].run();
    }
    else if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
