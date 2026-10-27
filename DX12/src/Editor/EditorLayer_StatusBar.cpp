// The editor's bottom status bar and the two Help windows (Keyboard Shortcuts, About).
//
// Status bar: one row pinned to the bottom of the main viewport with ImGui::BeginViewportSideBar,
// which shrinks the viewport's work area so the dock space above never overlaps it:
//
//   [EDIT | PLAYING | PAUSED]  scene.iscene *  unsaved   N selected   [AI: ...]
//                              [E errors] [W warnings]   1005x783   60 fps
//
// The error / warning counters are the Output Log's badge tallies; clicking them opens and focuses
// the log. While the AI control channel is open (Tools > AI Control Channel) an "AI" chip follows
// the selection: amber while it waits for a client, green with the client's executable once one is
// connected. Everything a client does is in the Output Log (source control); the chip opens it.
#include "Editor/EditorLayer.h"
#include "Editor/EditorLayerShared.h"      // kWindowOutputLog
#include "Editor/EditorTheme.h"
#include "Editor/FontAwesomeIcons.h"
#include "Harness/ControlChannel.h"
#include "System/Log.h"                    // Logger badge tallies
#include "System/PathUtil.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"          // BeginViewportSideBar

#include <cstdio>
#include <filesystem>
#include <string>

namespace
{
    // A small coloured chip (filled rounded rect + text); returns true when clicked.
    bool Chip(const char* id, const char* text, ImU32 bg, ImU32 fg, const char* tooltip)
    {
        // Menu-bar rows lay items out at the text line: the chip is that line plus horizontal padding,
        // with a rounded fill that bleeds 1 px above and below.
        const float  padX = 7.0f;
        const ImVec2 ts   = ImGui::CalcTextSize(text);
        const ImVec2 pos  = ImGui::GetCursorScreenPos();
        ImGui::PushID(id);
        const bool clicked = ImGui::InvisibleButton("##chip", ImVec2(ts.x + padX * 2.0f, ts.y));
        ImGui::PopID();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if ((bg >> IM_COL32_A_SHIFT) & 0xFF)
            dl->AddRectFilled(ImVec2(pos.x, pos.y - 1.0f),
                              ImVec2(pos.x + ts.x + padX * 2.0f, pos.y + ts.y + 1.0f), bg, 3.0f);
        dl->AddText(ImVec2(pos.x + padX, pos.y), fg, text);
        if (tooltip && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tooltip);
        return clicked;
    }
}

void EditorLayer::ToggleControlChannel()
{
    Harness::ControlChannel& channel = Harness::ControlChannel::Get();
    if (channel.Enabled())
    {
        channel.RequestStop();
        return;
    }
    if (!channel.Start("menu"))
        LOG_ERROR("AI control channel: the pipe could not be opened - the ControlChannel lines above say why");
}

void EditorLayer::RenderStatusBar()
{
    // The control channel's `status` names what a person sees here, so a client facing several
    // engines can tell this editor from a hidden harness run. Runs before the visibility early-out:
    // the status bar may be switched off.
    if (Harness::ControlChannel::Get().Enabled())
    {
        const ViewportPlayState ps = GetPlayState();
        Harness::ControlChannel::Get().PublishContext(
            m_currentScenePath,
            ps == ViewportPlayState::Playing ? "play" : ps == ViewportPlayState::Paused ? "paused" : "edit");
    }

    if (!m_showStatusBar) return;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings
                                 | ImGuiWindowFlags_MenuBar;
    // Styled like the menu bar (a MenuBar-flagged side bar draws its row in MenuBarBg), so the
    // editor reads as framed top and bottom.
    if (ImGui::BeginViewportSideBar("##EditorStatusBar", vp, ImGuiDir_Down,
                                    ImGui::GetFrameHeight(), flags))
    {
        if (ImGui::BeginMenuBar())
        {
            const bool icons = m_hasFontAwesomeIcons;

            // ---- Play state ----
            const ViewportPlayState play = GetPlayState();
            char playText[48];
            ImU32 playBg, playFg = IM_COL32(255, 255, 255, 255);
            if (play == ViewportPlayState::Playing)
            {
                std::snprintf(playText, sizeof(playText), "%s%sPLAYING",
                              icons ? FontAwesome::kPlay : "", icons ? " " : "");
                playBg = IM_COL32(200, 64, 52, 255);
            }
            else if (play == ViewportPlayState::Paused)
            {
                std::snprintf(playText, sizeof(playText), "%s%sPAUSED",
                              icons ? FontAwesome::kPause : "", icons ? " " : "");
                playBg = IM_COL32(214, 150, 30, 255);
                playFg = IM_COL32(20, 20, 20, 255);
            }
            else
            {
                std::snprintf(playText, sizeof(playText), "EDIT");
                playBg = IM_COL32(58, 58, 64, 255);
                playFg = IM_COL32(210, 210, 216, 255);
            }
            Chip("play", playText, playBg, playFg,
                 play == ViewportPlayState::Stopped
                     ? "Edit mode - Ctrl+P or the toolbar's Play button starts Play"
                     : "Play mode - changes are discarded by Stop (the pre-Play scene is restored)");
            ImGui::SameLine(0.0f, 12.0f);

            // ---- Scene + unsaved marker ----
            {
                const std::string name = m_currentScenePath.empty()
                    ? std::string("Untitled")
                    : Sys::PathToUtf8(Sys::U8Path(m_currentScenePath).filename());
                if (icons)
                {
                    ImGui::TextDisabled("%s", FontAwesome::kCubes);
                    ImGui::SameLine(0.0f, 6.0f);
                }
                ImGui::TextUnformatted(name.c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", m_currentScenePath.empty()
                        ? "Untitled scene (never saved)" : m_currentScenePath.c_str());
                if (m_sceneDirty)
                {
                    ImGui::SameLine(0.0f, 4.0f);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_UnsavedMarker));
                    ImGui::TextUnformatted("*  unsaved");
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Unsaved changes - Ctrl+S to save");
                }
            }
            ImGui::SameLine(0.0f, 18.0f);

            // ---- Selection ----
            if (m_world && m_selectedEntity != NullEntity && m_world->IsAlive(m_selectedEntity))
            {
                if (m_selectedEntities.size() > 1)
                    ImGui::TextDisabled("%zu selected", m_selectedEntities.size());
                else
                {
                    const std::string& n = m_world->GetName(m_selectedEntity);
                    ImGui::TextDisabled("Selected: %s", n.empty() ? "(unnamed)" : n.c_str());
                }
            }
            else
                ImGui::TextDisabled("Nothing selected");

            // ---- AI control channel (only while it is open) ----
            if constexpr (Harness::ControlChannel::kCompiled)
            {
                const Harness::ControlChannelState cs = Harness::ControlChannel::Get().Snapshot();
                if (cs.enabled)
                {
                    ImGui::SameLine(0.0f, 18.0f);
                    char aiText[96];
                    ImU32 aiBg;
                    if (cs.stopPending)
                    {
                        std::snprintf(aiText, sizeof(aiText), "AI: closing");
                        aiBg = IM_COL32(90, 90, 96, 255);
                    }
                    else if (cs.connected)
                    {
                        std::snprintf(aiText, sizeof(aiText), "AI: %s",
                                      cs.clientImage.empty() ? "connected" : cs.clientImage.c_str());
                        aiBg = IM_COL32(46, 140, 80, 255);
                    }
                    else
                    {
                        std::snprintf(aiText, sizeof(aiText), "AI: listening");
                        aiBg = IM_COL32(170, 125, 20, 255);
                    }
                    char last[96] = "none yet";
                    if (cs.lastAgeMs >= 0.0)
                        std::snprintf(last, sizeof(last), "%s, %.1f s ago", cs.lastVerb.c_str(), cs.lastAgeMs / 1000.0);
                    char tip[640];
                    std::snprintf(tip, sizeof(tip),
                        "AI control channel - open (opened by %s)\n"
                        "Pipe: %s\n"
                        "Client: %s\n"
                        "Commands answered: %llu   last: %s\n\n"
                        "A local pipe an agent attaches to (python tools/engine_ctl.py list).\n"
                        "Same Windows user only; NOT read-only. Every request is logged\n"
                        "in the Output Log (source control) - click to open it.\n"
                        "Tools > AI Control Channel closes it.",
                        cs.startedBy.c_str(), cs.pipe.c_str(),
                        cs.connected ? (cs.clientImage.empty() ? "connected" : cs.clientImage.c_str()) : "none",
                        static_cast<unsigned long long>(cs.served), last);
                    if (Chip("ai", aiText, aiBg, IM_COL32(255, 255, 255, 255), tip))
                    {
                        m_showOutputLog = true;
                        ImGui::SetWindowFocus(kWindowOutputLog);
                    }
                }
            }

            // ---- Right cluster: log counters, viewport size, fps ----
            const uint32_t errN = Logger::Get().ErrorCount();
            const uint32_t wrnN = Logger::Get().WarningCount();
            char errText[32], wrnText[32], sizeText[32], fpsText[32];
            std::snprintf(errText,  sizeof(errText),  "%s%s%u", icons ? FontAwesome::kError : "E", " ", errN);
            std::snprintf(wrnText,  sizeof(wrnText),  "%s%s%u", icons ? FontAwesome::kWarning : "W", " ", wrnN);
            std::snprintf(sizeText, sizeof(sizeText), "%ux%u", m_viewportSizeX, m_viewportSizeY);
            const float fps = ImGui::GetIO().Framerate;
            std::snprintf(fpsText,  sizeof(fpsText),  "%.0f fps  %.2f ms", fps, fps > 0.0f ? 1000.0f / fps : 0.0f);

            // Right-align: two chips padded 7 px each side + 6 px between them, then two gap-separated text
            // items, measured from the cursor the menu-bar layout left after the selection text.
            const float gap = 16.0f;
            const float rightW = ImGui::CalcTextSize(errText).x + ImGui::CalcTextSize(wrnText).x
                               + ImGui::CalcTextSize(sizeText).x + ImGui::CalcTextSize(fpsText).x
                               + 4.0f * 7.0f + 6.0f + gap * 2.0f + 8.0f;
            const float avail = ImGui::GetContentRegionAvail().x;
            ImGui::SameLine(0.0f, avail > rightW + gap ? avail - rightW : gap);

            auto counter = [&](const char* id, const char* text, uint32_t n, ImU32 onColor)
            {
                const ImU32 bg = n ? onColor : IM_COL32(0, 0, 0, 0);
                const ImU32 fg = n ? IM_COL32(255, 255, 255, 255)
                                   : ImGui::GetColorU32(ImGuiCol_TextDisabled);
                if (Chip(id, text, bg, fg, "Open the Output Log"))
                {
                    m_showOutputLog = true;
                    ImGui::SetWindowFocus(kWindowOutputLog);
                }
            };
            counter("err", errText, errN, IM_COL32(190, 50, 50, 255));
            ImGui::SameLine(0.0f, 6.0f);
            counter("wrn", wrnText, wrnN, IM_COL32(170, 125, 20, 255));
            ImGui::SameLine(0.0f, gap);
            ImGui::TextDisabled("%s", sizeText);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Viewport render size");
            ImGui::SameLine(0.0f, gap);
            ImGui::TextDisabled("%s", fpsText);

            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
}

// ==== Help > About ====
void EditorLayer::RenderAboutWindow()
{
    ImGui::SetNextWindowSize(ImVec2(520.0f, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::Begin("About", &m_showAbout,
                      ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End();
        return;
    }
    EditorTheme::PushBold();
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.4f);
    ImGui::TextUnformatted("DX12 Engine Editor");
    ImGui::PopFont();
    EditorTheme::PopBold();
    ImGui::TextDisabled("Direct3D 12  ·  C++20  ·  Dear ImGui %s", IMGUI_VERSION);
#if defined(_DEBUG)
    ImGui::TextDisabled("Build: Debug");
#else
    ImGui::TextDisabled("Build: optimised");
#endif
    EditorTheme::Heading("Fonts");
    ImGui::TextWrapped("Noto Sans, Noto Sans TC and Noto Sans JP by Google - SIL Open Font "
                       "License 1.1 (engine/editor/fonts/*/OFL.txt). Font Awesome 7 Free "
                       "Solid - SIL OFL 1.1 (icons).");
    EditorTheme::Heading("Folders");
    {
        std::error_code ec;
        const std::string root = Sys::PathToUtf8(std::filesystem::absolute(Sys::U8Path("."), ec));
        ImGui::TextDisabled("Project   %s", ec ? "." : root.c_str());
    }
    ImGui::TextDisabled("Logs      log/   ·   Docs  docs/   ·   Packages  build/package/");
    ImGui::End();
}
