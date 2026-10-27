// The Editor Preferences window (Edit > Editor Preferences...): per-user editor
// behaviour, persisted in editor_prefs.ini. Project settings (settings.json /
// engine.json) are the separate Project Settings window.
//
// It gathers preferences that also live on their panels (viewport snap and grid,
// log filters, Hierarchy "Internal", Content Browser view) and ones with no other
// UI (recent scenes, camera bookmarks, the material preview switch, font size,
// autosave). Widgets write the same members the panels use, so a change shows
// everywhere at once; SaveEditorPrefs writes them on exit, on a recent-scene
// change, or from the Save Now button. Under -unattended the file is read-only.
#include "Editor/EditorLayer.h"
#include "Editor/EditorTheme.h"
#include "Editor/FontAwesomeIcons.h"
#include "Editor/GameView.h"   // the Game View's Esc preference
#include "Editor/MaterialPreviewWidget.h"   // preview.enabled kill switch
#include "System/CommandLine.h"

#include "imgui/imgui.h"
#include "imgui/ImGuizmo/ImGuizmo.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <functional>

namespace
{
    constexpr float kLabelW = 190.0f;

    bool ContainsNoCase(const char* hay, const char* needle)
    {
        if (!needle || !*needle) return true;
        for (const char* h = hay; *h; ++h)
        {
            const char* a = h;
            const char* b = needle;
            while (*a && *b && std::tolower(static_cast<unsigned char>(*a)) ==
                               std::tolower(static_cast<unsigned char>(*b)))
                ++a, ++b;
            if (!*b) return true;
        }
        return false;
    }

    // A labelled row: the label on the left, the widget in a fixed column on the
    // right (settings-page layout, not ImGui's control-then-label).
    void RowLabel(const char* label, const char* tooltip = nullptr)
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        if (tooltip && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tooltip);
        ImGui::SameLine(kLabelW);
        ImGui::SetNextItemWidth(std::min(260.0f, ImGui::GetContentRegionAvail().x));
    }
}

void EditorLayer::RenderEditorPreferencesWindow()
{
    ImGui::SetNextWindowSize(ImVec2(760.0f, 560.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Editor Preferences", &m_showEditorPreferences))
    {
        ImGui::End();
        return;
    }

    // ---- Pages ----
    auto pageAppearance = [&]()
    {
        EditorTheme::Heading("Text");
        RowLabel("Font size", "Editor text size in pixels. Applies immediately.\n"
                              "Menus, toolbars and panels grow with it.");
        float size = m_uiFontSize;
        if (ImGui::SliderFloat("##fontSize", &size, EditorTheme::kMinFontSize,
                               EditorTheme::kMaxFontSize, "%.0f px",
                               ImGuiSliderFlags_AlwaysClamp))
            m_uiFontSize = EditorTheme::SetFontSize(std::round(size));
        ImGui::SameLine();
        if (ImGui::SmallButton("Default##font"))
            m_uiFontSize = EditorTheme::SetFontSize(EditorTheme::kDefaultFontSize);

        const EditorTheme::Fonts& f = EditorTheme::GetFonts();
        RowLabel("Typeface");
        ImGui::TextDisabled("%s", f.noto ? "Noto Sans  +  Noto Sans TC / JP" : "fallback (Noto files missing)");
        RowLabel("Icons");
        ImGui::TextDisabled("%s", f.icons ? "Font Awesome 7 Solid" : "not loaded");
        ImGui::Spacing();
        ImGui::TextDisabled("Sample:  English  ·  繁體中文  ·  日本語のテキスト");
        if (Sys::CommandLine::Get().Has("-unattended"))
            ImGui::TextDisabled("(-unattended: the harness pins the font to %.0f px)",
                                EditorTheme::kDefaultFontSize);
    };

    auto pageViewport = [&]()
    {
        EditorTheme::Heading("Grid");
        RowLabel("Show grid");
        ImGui::Checkbox("##grid", &m_showEditorGrid);
        RowLabel("Cell size", "Metres per cell; a major line every 10 cells.");
        ImGui::DragFloat("##cell", &m_gridCellSize, 0.05f, 0.01f, 100.0f, "%.2f m",
                         ImGuiSliderFlags_AlwaysClamp);

        EditorTheme::Heading("Transform Gizmo");
        RowLabel("Space", "Local: axes follow the object. World: axes stay world-aligned.\n"
                          "Scale is always local.");
        int space = (m_gizmoMode == ImGuizmo::WORLD) ? 1 : 0;
        if (ImGui::Combo("##space", &space, "Local\0World\0"))
            m_gizmoMode = space ? ImGuizmo::WORLD : ImGuizmo::LOCAL;

        EditorTheme::Heading("Snapping");
        RowLabel("Snap while dragging", "Hold Ctrl while dragging to invert it temporarily.");
        ImGui::Checkbox("##snap", &m_snapEnabled);
        RowLabel("Move step");
        ImGui::DragFloat("##snapT", &m_snapTranslate, 0.05f, 0.001f, 100.0f, "%.3g m",
                         ImGuiSliderFlags_AlwaysClamp);
        RowLabel("Rotate step");
        ImGui::DragFloat("##snapR", &m_snapRotate, 0.5f, 0.1f, 90.0f, "%.1f deg",
                         ImGuiSliderFlags_AlwaysClamp);
        RowLabel("Scale step");
        ImGui::DragFloat("##snapS", &m_snapScale, 0.01f, 0.01f, 10.0f, "%.2f x",
                         ImGuiSliderFlags_AlwaysClamp);

        EditorTheme::Heading("Game View");
        RowLabel("Esc releases the mouse",
                 "While the game holds the mouse in the Game View, Esc gives it back to the editor\n"
                 "first and the game never sees that press; once released, Esc is the game's\n"
                 "(its pause menu). Off: Esc is always the game's and Shift+F1 releases the mouse.");
        {
            bool escReleases = Editor::GameView::Get().EscReleasesCapture();
            if (ImGui::Checkbox("##gameViewEsc", &escReleases))
                Editor::GameView::Get().SetEscReleasesCapture(escReleases);
        }

        EditorTheme::Heading("Gizmos");
        RowLabel("Reflection probe viz", "Master switch for the probes' mirror-ball viz.\n"
                                         "Also in the viewport's Show menu.");
        ImGui::Checkbox("##probeViz", &m_showProbeVizSpheres);

        EditorTheme::Heading("Camera Bookmarks");
        ImGui::TextDisabled("Ctrl+Shift+1..4 stores the free-camera pose, Ctrl+1..4 recalls it.");
        for (int i = 0; i < kCameraBookmarks; ++i)
        {
            ImGui::PushID(i);
            char label[24];
            std::snprintf(label, sizeof(label), "Bookmark %d", i + 1);
            RowLabel(label);
            const CameraBookmark& bm = m_camBookmarks[i];
            if (bm.valid)
            {
                ImGui::TextDisabled("(%.1f, %.1f, %.1f)", bm.position.x, bm.position.y, bm.position.z);
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear")) m_camBookmarks[i] = CameraBookmark{};
            }
            else
                ImGui::TextDisabled("empty");
            ImGui::PopID();
        }
    };

    auto pagePanels = [&]()
    {
        EditorTheme::Heading("Layout");
        RowLabel("Status bar", "Play state, scene / unsaved marker, selection, error and\n"
                               "warning counters, viewport size and fps along the bottom.");
        ImGui::Checkbox("##statusBar", &m_showStatusBar);

        EditorTheme::Heading("Hierarchy");
        RowLabel("Show internal entities", "Editor camera, camera-stack channel, the Chain\n"
                                           "Physics Preview stage.");
        ImGui::Checkbox("##internal", &m_showInternalEntities);

        EditorTheme::Heading("Content Browser");
        RowLabel("View");
        int view = static_cast<int>(m_assetViewMode);
        if (ImGui::Combo("##view", &view, "Tiles\0List\0Details\0"))
            m_assetViewMode = static_cast<AssetViewMode>(view);
        RowLabel("Folder tree");
        ImGui::Checkbox("##tree", &m_showFolderTree);
        {
            auto& w = Editor::MaterialPreviewWidget::Get();
            RowLabel("Live material preview", "Inspector / thumbnail preview renders (preview.enabled).\n"
                                              "The DX12_PREVIEW=0 environment variable also turns it off.");
            bool on = w.PrefEnabled();
            if (ImGui::Checkbox("##preview", &on))
                w.SetPrefEnabled(on);
        }

        EditorTheme::Heading("Output Log");
        RowLabel("Show errors");   ImGui::Checkbox("##le", &m_logShowError);
        RowLabel("Show warnings"); ImGui::Checkbox("##lw", &m_logShowWarning);
        RowLabel("Show info");     ImGui::Checkbox("##li", &m_logShowInfo);
        RowLabel("Show success");  ImGui::Checkbox("##ls", &m_logShowSuccess);
        RowLabel("Auto-scroll");   ImGui::Checkbox("##la", &m_logAutoScroll);
    };

    auto pageSaving = [&]()
    {
        EditorTheme::Heading("Autosave");
        ImGui::TextWrapped("Snapshots of the open scene into autosave/, never over the "
                           "authoring .iscene. Written only while there are unsaved changes, "
                           "and before entering Play. Opening a scene offers a newer autosave.");
        ImGui::Spacing();
        RowLabel("Enabled");
        ImGui::Checkbox("##autosave", &m_autosaveEnabled);
        ImGui::BeginDisabled(!m_autosaveEnabled);
        RowLabel("Interval");
        int minutes = static_cast<int>(std::lround(m_autosaveIntervalSec / 60.0));
        if (ImGui::SliderInt("##autosaveMin", &minutes, 1, 60, "%d min", ImGuiSliderFlags_AlwaysClamp))
            m_autosaveIntervalSec = static_cast<double>(minutes) * 60.0;
        ImGui::EndDisabled();

        EditorTheme::Heading("Recent Scenes");
        if (m_recentScenes.empty())
            ImGui::TextDisabled("(none)");
        for (const std::string& r : m_recentScenes)
            ImGui::BulletText("%s", r.c_str());
        if (!m_recentScenes.empty() && ImGui::SmallButton("Clear list"))
        {
            m_recentScenes.clear();
            SaveEditorPrefs();
        }
    };

    struct Page
    {
        const char*           title;
        const char*           keywords;
        std::function<void()> draw;
    };
    const Page pages[] = {
        { "Appearance", "font size text typeface noto cjk chinese japanese icons scale", pageAppearance },
        { "Viewport",   "grid cell snap snapping move rotate scale step gizmo local world space "
                        "probe viz camera bookmark", pageViewport },
        { "Panels",     "hierarchy internal content browser tiles list details folder tree "
                        "material preview thumbnail output log errors warnings auto-scroll "
                        "inspector legacy full material inspector", pagePanels },
        { "Saving",     "autosave interval backup recovery recent scenes", pageSaving },
    };
    constexpr int kPageCount = static_cast<int>(sizeof(pages) / sizeof(pages[0]));
    static int  s_page = 0;
    static char s_search[64] = {};

    const float footerH = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y + 1.0f;
    ImGui::BeginChild("##prefsNav", ImVec2(180.0f, -footerH), ImGuiChildFlags_Borders);
    {
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##prefsSearch", "Search preferences", s_search, sizeof(s_search));
        ImGui::Spacing();
        int firstMatch = -1;
        int clicked = -1;
        bool currentMatches = false;
        for (int i = 0; i < kPageCount; ++i)
        {
            if (!ContainsNoCase(pages[i].title, s_search) && !ContainsNoCase(pages[i].keywords, s_search))
                continue;
            if (firstMatch < 0) firstMatch = i;
            if (i == s_page) currentMatches = true;
            if (ImGui::Selectable(pages[i].title, s_page == i))
                clicked = i;
        }
        // Apply the click after the loop: changed inside it, clicking a page above the
        // open one leaves currentMatches false and jumps to the first match.
        if (clicked >= 0) s_page = clicked;
        else if (!currentMatches && firstMatch >= 0) s_page = firstMatch;
        if (firstMatch < 0) ImGui::TextDisabled("No preference matches");
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##prefsPage", ImVec2(0.0f, -footerH), ImGuiChildFlags_Borders);
    {
        EditorTheme::PushBold();
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.25f);
        ImGui::TextUnformatted(pages[s_page].title);
        ImGui::PopFont();
        EditorTheme::PopBold();
        ImGui::Separator();
        pages[s_page].draw();
    }
    ImGui::EndChild();

    // ---- Footer ----
    if (ImGui::Button("Save Now"))
        SaveEditorPrefs();
    ImGui::SameLine();
    ImGui::TextDisabled("Saved to editor_prefs.ini on exit%s.",
                        Sys::CommandLine::Get().Has("-unattended")
                            ? " (read-only under -unattended)" : "");
    ImGui::End();
}
