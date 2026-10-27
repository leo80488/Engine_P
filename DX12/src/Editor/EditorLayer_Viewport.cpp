// EditorLayer_Viewport.cpp - the main viewport panel: its toolbar (the debug
// target Buffer menu, Play / Pause and the pre-Play autosave),
// RenderViewportPanel, focus and spawn placement, snap to floor, picking, and
// the transform gizmo (its draw list, RenderViewportGizmo, ApplyGizmoToEntity).

#include "Editor/EditorLayer.h"
#include "Editor/EditorOrthoViews.h"   // the axis views' fwd/up
#include "Editor/EditorViewCube.h"     // the view cube over the picture's corner
#include "Editor/FocusTracker.h"       // the viewport reports what it showed (the Game View route reads it)
#include "Editor/GameView.h"           // in Play the Scene may show the editor camera's auxiliary view
#include "Editor/AnimGraphEditorWindow.h"
#include "Editor/EditorLayerShared.h"  // kWindow* dock titles, s_faIconsLoaded, SaveFileDialog (shared with the split EditorLayer_* TUs)
#include "Editor/MaterialPreviewWidget.h"
#include "Editor/CameraPreviewWindow.h"
#include "Editor/AssetPreviewWindow.h"
#include "Editor/MeshEditorWindow.h"
#include "Editor/SkeletonEditorWindow.h"
#include "Editor/TextureViewerWindow.h"
#include "Editor/UIEditorWindow.h"         // a double-click on Canvas UI opens it there
#include "Editor/MaterialPropertyPanel.h"
#include "Editor/MaterialEditorWindow.h"
#include "Editor/NodeEditorSmoke.h"
#include "Editor/SplitViewportPanel.h"
#include "Editor/ChainPhysicsPreviewWindow.h"
#include "System/CommandLine.h"            // -editor_gizmo_foreground / -unattended (ViewportGizmoDrawList)
#include "Editor/EntityRefPicker.h"
#include "Editor/FontAwesomeIcons.h"
#include "ECS/PlayModeController.h"   // authoritative Edit/Play state machine
#include "ECS/GuidRegistry.h"
#include "AI/BTNode.h"
#include "UI/UICanvas.h"
#include "UI/UICanvasLayout.h"   // UI::IsUINode (no 3D gizmo / F / End on Canvas UI)
#include "Graphics/TracerSystem.h"    // Renderer::GetTracerSystem() for the T-key tracer test
#include "Scene/MeshSpawner.h"
#include "Editor/InputBindingsWindow.h"
#include "System/TaskSystem.h"
#include "System/PathUtil.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/Ray.h"
#include "Scene/SceneInstanceLoader.h"
#include "Editor/HumanoidMapping.h"
#include "Renderer/Renderer.h"
#include <chrono>
#include "Graphics/GraphicsDX12.h"
#include "RenderGraph/RenderPass/DDGIProbeDebugPass.h"
#include "Resource/ComponentSerializers.h"
#include "Resource/AnimationClipSystem.h"
#include "Editor/DebugDrawSystem.h"
#include "Graphics/GraphicsDX12.h"
#include "imgui/imgui_impl_win32.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/ImGuizmo/ImGuizmo.h"
#include "Editor/ChainRestPoseMath.h"
#include <cassert>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <optional>
#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>

using namespace DirectX;


// The viewport toolbar's Buffer menu (Renderer/DebugTargets.h): the main view
// shows ONE render target instead of the lit image - in Edit or Play, the same
// view Lua's Render.target sets - and the sliders button beside it edits how that
// target is mapped. Availability is the snapshot the render half refreshes while
// the menu is open (a frame old); an unavailable target can still be picked - the
// view then paints a grey checkerboard and the tooltip says why.
static void DrawDebugTargetMenu(Renderer& r, float comboW, float buttonW, const char* optionsLabel)
{
    using DebugTargets::Mode;
    const DebugTargets::View view = r.GetDebugTargetView();
    const bool active = !view.name.empty();
    const std::string preview = "Buffer: " + (active ? view.name : std::string("Lit"));
    ImGui::SetNextItemWidth(comboW);
    // A picture that is not the lit image must not pass for one.
    if (active) ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.55f, 0.30f, 0.05f, 1.00f));
    const bool open = ImGui::BeginCombo("##debugtarget", preview.c_str(), ImGuiComboFlags_HeightLarge);
    if (active) ImGui::PopStyleColor();
    if (!open && ImGui::IsItemHovered())
        ImGui::SetTooltip("Buffer: show one render target instead of the lit image (Edit or Play).\n"
                          "Lua: Render.target(\"normal\"), Render.target(false); flag -debugtarget=<name>.\n"
                          "Grey checkerboard = the target is not there this frame (its feature is off).\n"
                          "Magenta = NaN / Inf. The sliders button maps it: mode, channel, range.");
    if (open)
    {
        std::string error;
        if (ImGui::Selectable("Lit (no buffer)", !active)) r.SetDebugTargetView(DebugTargets::View{}, error);
        std::string group;
        for (const DebugTargets::Info& i : r.ListDebugTargets())
        {
            if (i.group != group)
            {
                group = i.group;
                ImGui::SeparatorText(group.c_str());
            }
            const std::string label = (i.label == i.name ? i.name : i.label + "  (" + i.name + ")") + "##" + i.name;
            const bool usable = i.available && i.viewable;
            if (!usable) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            if (ImGui::Selectable(label.c_str(), view.name == i.name,
                                  i.viewable ? ImGuiSelectableFlags_None : ImGuiSelectableFlags_Disabled))
            {
                DebugTargets::View v;
                v.name = i.name;
                if (!r.SetDebugTargetView(v, error)) LOG_WARNING("Buffer menu: %s", error.c_str());
            }
            if (!usable) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
                ImGui::TextUnformatted(i.meaning.c_str());
                ImGui::TextDisabled("%s  %ux%u  channels: %s  default: %s", i.format.empty() ? "?" : i.format.c_str(),
                                    i.width, i.height, i.channels.c_str(), DebugTargets::ModeName(i.defaultMode));
                if (!i.why.empty()) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", i.why.c_str());
                if (!i.viewWhy.empty())
                    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "not viewable: %s", i.viewWhy.c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    if (!active) ImGui::BeginDisabled();
    if (ImGui::Button(optionsLabel, ImVec2(buttonW, buttonW))) ImGui::OpenPopup("##debugtargetopts");
    if (!active) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("How the buffer is mapped: mode, channel, value range, exposure");
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 4.0f));
    if (ImGui::BeginPopup("##debugtargetopts"))
    {
        DebugTargets::View v = r.GetDebugTargetView();
        std::string channels;
        for (const DebugTargets::Info& i : r.ListDebugTargets())
            if (i.name == v.name) channels = i.channels;
        bool changed = false;
        ImGui::TextUnformatted(v.name.c_str());
        ImGui::Separator();
        static const char* kModes[] = { "Auto (the target's own)", "Raw", "Channel", "HDR", "Depth", "Velocity",
                                        "Signed", "Heat", "Categorical", "Bits", "EV (stops from grey)" };
        static_assert(IM_ARRAYSIZE(kModes) == (int)Mode::Count, "kModes mirrors DebugTargets::Mode");
        int mode = (int)v.mode;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
        if (ImGui::Combo("Mode", &mode, kModes, IM_ARRAYSIZE(kModes))) { v.mode = (Mode)mode; changed = true; }

        // Channel: "default" + the target's own channel names.
        std::vector<std::string> names;
        for (size_t s = 0; s <= channels.size();)
        {
            const size_t comma = channels.find(',', s);
            names.push_back(channels.substr(s, comma == std::string::npos ? std::string::npos : comma - s));
            if (comma == std::string::npos) break;
            s = comma + 1;
        }
        const std::string chPreview = v.channel < 0 ? std::string("default")
                                    : (v.channel < (int)names.size() ? names[(size_t)v.channel]
                                                                      : std::to_string(v.channel));
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
        if (ImGui::BeginCombo("Channel", chPreview.c_str()))
        {
            if (ImGui::Selectable("default", v.channel < 0)) { v.channel = -1; changed = true; }
            for (size_t c = 0; c < names.size(); ++c)
                if (ImGui::Selectable(names[c].c_str(), v.channel == (int)c)) { v.channel = (int)c; changed = true; }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A channel with mode Auto shows that channel alone, as grey");
        // What a field the view does not set really maps to: the target's own
        // default, as the last painted frame resolved it.
        const DebugTargets::Shown painted = r.GetDebugTargetShown();
        const bool usePainted = painted.active && painted.view.name == v.name;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
        float range[2] = { (v.minGiven || !usePainted) ? v.rangeMin : painted.view.rangeMin,
                           (v.maxGiven || !usePainted) ? v.rangeMax : painted.view.rangeMax };
        if (ImGui::DragFloat2("Range", range, 0.005f, -1000.0f, 1000.0f, "%.3f") && range[0] != range[1])
        {
            v.rangeMin = range[0];
            v.rangeMax = range[1];
            v.minGiven = v.maxGiven = true;
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Raw / Channel / Heat: the value mapped to black .. white (Heat: to the ramp's ends)");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
        if (ImGui::DragFloat("Exposure", &v.exposure, 0.01f, 0.001f, 1000.0f, "%.3f",
                             ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp))
            changed = true;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
        if (!v.scaleGiven && usePainted) v.scale = painted.view.scale;
        if (ImGui::DragFloat("Scale", &v.scale, 1.0f, 1.0f, 100000.0f, "%.0f",
                             ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp))
        {
            v.scaleGiven = true;
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Velocity: NDC distance -> brightness; Categorical / Bits on a float target: value -> integer");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
        if (!v.maskGiven && usePainted) v.mask = painted.view.mask;
        if (ImGui::InputScalar("Mask", ImGuiDataType_U32, &v.mask, nullptr, nullptr, "%08X",
                               ImGuiInputTextFlags_CharsHexadecimal))
        {
            v.maskGiven = true;
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Categorical / Bits: the integer's bits that count");
        if (ImGui::Button("Reset mapping"))
        {
            const std::string name = v.name;
            v = DebugTargets::View{};
            v.name = name;
            changed = true;
        }
        std::string error;
        if (changed && !r.SetDebugTargetView(v, error)) LOG_WARNING("Buffer options: %s", error.c_str());

        const DebugTargets::Shown shown = r.GetDebugTargetShown();
        ImGui::Separator();
        if (shown.active)
            ImGui::TextDisabled("Frame %llu: %s, channel %d%s%s", static_cast<unsigned long long>(shown.frame),
                                DebugTargets::ModeName(shown.view.mode), shown.view.channel,
                                shown.available ? "" : " - not there: ", shown.available ? "" : shown.why.c_str());
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

void EditorLayer::RenderViewportToolbar()
{
    // Sizes follow the editor font size (Editor Preferences > Appearance) and are
    // exactly 25 px / scale 1.0 at the default 16 px, the only size the harness runs
    // at: the toolbar height feeds the Viewport rectangle the packs measure.
    const float uiScale     = ImGui::GetFontSize() / 16.0f;
    const float padding     = 5.0f;
    const float buttonSize  = std::round(ImGui::GetFontSize() + 9.0f);
    const float itemSpacing = 2.0f;

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padding, padding));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,  ImVec2(itemSpacing, 0.0f));

    auto iconOrLabel = [this](const char* icon, const char* label) { return m_hasFontAwesomeIcons ? icon : label; };

    // Toggle button: the theme accent + white glyph when ON, a dimmed glyph when
    // OFF. The accent is read from the style (SliderGrab carries it) so the toolbar
    // follows EditorTheme instead of hard-coding its own blue.
    auto toggleBtn = [&](const char* label, bool on, const ImVec2& size) -> bool
    {
        if (on)
        {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImGui::GetStyleColorVec4(ImGuiCol_SliderGrab));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_SliderGrabActive));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1.00f, 1.00f, 1.00f, 1.00f));
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.52f, 0.52f, 0.52f, 1.00f));
        }
        const bool clicked = ImGui::Button(label, size);
        ImGui::PopStyleColor(on ? 4 : 1);
        return clicked;
    };

    // Row origin - used to center the play cluster on the FULL toolbar width.
    const float contentMinX = ImGui::GetCursorPosX();

    // ---- Gizmo operation buttons (left-aligned) ----
    // Icons (move / rotate / scale) with the W/E/R letters as the fallback when
    // Font Awesome is missing. The "##gz?" suffix keeps the IDs stable across the
    // two spellings.
    auto gizmoBtn = [&](const char* icon, const char* letter, const char* id,
                        ImGuizmo::OPERATION op, const char* tooltip)
    {
        char label[48];
        std::snprintf(label, sizeof(label), "%s%s", iconOrLabel(icon, letter), id);
        if (toggleBtn(label, m_gizmoOperation == op, ImVec2(buttonSize, buttonSize)))
            m_gizmoOperation = op;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", tooltip);
    };

    gizmoBtn(FontAwesome::kMove,   "T", "##gzT", ImGuizmo::TRANSLATE, "Move  [W]");
    ImGui::SameLine();
    gizmoBtn(FontAwesome::kRotate, "R", "##gzR", ImGuizmo::ROTATE,    "Rotate  [E]");
    ImGui::SameLine();
    gizmoBtn(FontAwesome::kScale,  "S", "##gzS", ImGuizmo::SCALE,     "Scale  [R]");
    ImGui::SameLine();

    // ---- Gizmo space toggle ----
    // Local: axes follow the object's rotation; World: axes stay world-aligned.
    // Scale is always local (ImGuizmo forces LOCAL for SCALE to avoid skewing the matrix).
    {
        const bool isLocal = (m_gizmoMode == ImGuizmo::LOCAL);
        if (ImGui::Button(isLocal ? "Local##gzmode" : "World##gzmode",
                          ImVec2(buttonSize * 2.0f, buttonSize)))
            m_gizmoMode = isLocal ? ImGuizmo::WORLD : ImGuizmo::LOCAL;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(isLocal
                ? "Gizmo space: Local — axes follow the object's rotation (click for World)"
                : "Gizmo space: World — axes stay aligned to world XYZ (click for Local)");
    }
    ImGui::SameLine();

    // ---- Edit Collider ----
    // The gizmo drives the selection's Collider instead of its transform. Shown for
    // an entity that has one.
    if (m_world && m_selectedEntity != NullEntity && m_world->IsAlive(m_selectedEntity) &&
        m_world->HasComponent<ColliderComponent>(m_selectedEntity))
    {
        if (toggleBtn("Collider##gzcol", m_editCollider, ImVec2(buttonSize * 2.6f, buttonSize)))
            SetEditCollider(!m_editCollider);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Edit Collider: Move / Rotate / Scale the selection's collider (its offset, rotation\n"
                              "and size - a box by its faces) instead of the entity. One drag = one undo step.");
        ImGui::SameLine();
    }

    // ---- Play / Pause / Stop, centered on the FULL toolbar width ----
    const int   playCount = 3;
    const float playTotal = playCount * buttonSize + (playCount - 1) * itemSpacing;
    const float rowRight  = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    const float centerX   = contentMinX + (rowRight - contentMinX - playTotal) * 0.5f;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), centerX));

    // Buttons express INTENT to the PlayModeController (the authoritative EngineCore
    // state machine); App applies the transition next frame. Without a controller the
    // local flags stand in so the toolbar still reflects a state.
    const bool isPlaying = (GetPlayState() == ViewportPlayState::Playing);
    if (ImGui::Button(iconOrLabel(isPlaying ? FontAwesome::kPause : FontAwesome::kPlay,
                                  isPlaying ? "Pause" : "Play"),
                      ImVec2(buttonSize, buttonSize)))
        TogglePlayPause();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(isPlaying ? "Pause  [Ctrl+P]" : "Play  [Ctrl+P]");

    ImGui::SameLine();

    if (ImGui::Button(iconOrLabel(FontAwesome::kStop, "Stop"), ImVec2(buttonSize, buttonSize)))
    {
        if (m_playModeController) m_playModeController->RequestStop();
        else                      m_playState = ViewportPlayState::Stopped;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Stop (restore scene)");

    ImGui::SameLine();

    if (ImGui::Button(iconOrLabel(FontAwesome::kStepForward, "Step"), ImVec2(buttonSize, buttonSize)))
    {
        if (m_playModeController) m_playModeController->RequestStep();
        else                      m_wantsStepFrame = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Update 1 frame (pause + step)");

    // ---- Right cluster: camera view + Show + grid + snap (view mode trails) ----
    const float camViewW = buttonSize * 2.6f;
    const float showW    = buttonSize * 2.8f;
    {
        const float unitW    = buttonSize * 2.4f;
        const float snapW    = buttonSize * 2.6f;
        const float viewW    = m_renderer ? (110.0f * uiScale + itemSpacing) : 0.0f;
        const float clusterW = camViewW + itemSpacing + showW + itemSpacing
                             + buttonSize + itemSpacing + unitW
                             + itemSpacing + buttonSize + itemSpacing + snapW + viewW;
        ImGui::SameLine();
        const float rightX = ImGui::GetCursorPosX()
                           + ImGui::GetContentRegionAvail().x - clusterW;
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), rightX));
    }

    // ---- Show: everything the viewport draws over the scene ----
    // (grid, canvas bounds, gizmo icons, probe viz, debug wireframes), as a popup
    // anchored under the button like the combo boxes beside it.
    {
        char showLabel[48];
        std::snprintf(showLabel, sizeof(showLabel), "%s%sShow##vpshow",
                      m_hasFontAwesomeIcons ? FontAwesome::kEye : "",
                      m_hasFontAwesomeIcons ? " " : "");
        const ImVec2 btnMin = ImGui::GetCursorScreenPos();
        if (ImGui::Button(showLabel, ImVec2(showW, buttonSize))
            || (HarnessMenuHeldOpen("Show") && !ImGui::IsPopupOpen("##vpShowMenu")))
            ImGui::OpenPopup("##vpShowMenu");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Show — grid, gizmo icons, debug wireframes");
        ImGui::SetNextWindowPos(ImVec2(btnMin.x, btnMin.y + buttonSize + 2.0f));
        // The popup would inherit the toolbar's squeezed spacing; give it the
        // theme's normal menu metrics back (EditorTheme::ApplyStyle).
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,  ImVec2(8.0f, 5.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f));
        if (ImGui::BeginPopup("##vpShowMenu"))
        {
            DrawViewportShowMenuItems();
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar(2);
        ImGui::SameLine();
    }

    // ---- Viewport camera view: perspective free-fly or axis-locked ortho ----
    {
        static const char* kViews[] = { "Persp", "Top", "Bottom", "Front", "Back", "Left", "Right" };
        const int cur = GetEditorViewMode();
        const bool inPlay = (GetPlayState() != ViewportPlayState::Stopped);
        if (inPlay) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(camViewW);
        if (ImGui::BeginCombo("##camview", kViews[cur], ImGuiComboFlags_None))
        {
            for (int v = 0; v < 7; ++v)
                if (ImGui::Selectable(kViews[v], v == cur))
                    m_editorViewMode = static_cast<EditorViewMode>(v);
            ImGui::EndCombo();
        }
        if (inPlay) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(inPlay
                ? "Camera view (Edit mode only — Play uses the scene camera)"
                : "Camera view — Persp = free-fly; others = axis-locked orthographic (wheel = zoom, RMB-drag = pan)");
        ImGui::SameLine();
    }

    // ---- Reference grid: visibility toggle + unit (metres per cell) ----
    {
        if (toggleBtn(iconOrLabel(FontAwesome::kTh, "#"), m_showEditorGrid,
                      ImVec2(buttonSize, buttonSize)))
            m_showEditorGrid = !m_showEditorGrid;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Show reference grid (XZ plane at origin)");

        ImGui::SameLine();

        static const float kUnits[]  = { 0.1f, 0.5f, 1.0f, 5.0f, 10.0f };
        static const char* kLabels[] = { "0.1m", "0.5m", "1m", "5m", "10m" };
        int cur = 2;
        for (int u = 0; u < 5; ++u)
            if (std::fabs(m_gridCellSize - kUnits[u]) < 1e-4f) { cur = u; break; }
        ImGui::SetNextItemWidth(buttonSize * 2.4f);
        if (ImGui::BeginCombo("##gridunit", kLabels[cur], ImGuiComboFlags_None))
        {
            for (int u = 0; u < 5; ++u)
                if (ImGui::Selectable(kLabels[u], u == cur))
                    m_gridCellSize = kUnits[u];
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Grid unit — metres per cell (major line every 10 cells)");
        // Renderer push happens once per frame in OnUIRender (not here) so the
        // state stays live in F11 fullscreen, where this toolbar isn't drawn.
    }
    ImGui::SameLine();

    // ---- Drag snap: toggle + increment for the current gizmo operation ----
    {
        if (toggleBtn(iconOrLabel(FontAwesome::kMagnet, "Sn"), m_snapEnabled,
                      ImVec2(buttonSize, buttonSize)))
            m_snapEnabled = !m_snapEnabled;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Snap dragging — off = free drag; hold Ctrl while dragging to invert temporarily");

        ImGui::SameLine();

        // The value edits the increment of the CURRENT gizmo operation.
        float*       val;
        const char*  fmt;
        const char*  tip;
        float        speed, minV, maxV;
        switch (m_gizmoOperation)
        {
        case ImGuizmo::ROTATE:
            val = &m_snapRotate;    fmt = "%.1f deg"; tip = "Rotate snap (degrees per step)";
            speed = 0.5f;  minV = 0.1f;   maxV = 90.0f;  break;
        case ImGuizmo::SCALE:
            val = &m_snapScale;     fmt = "%.2f x";   tip = "Scale snap (factor per step)";
            speed = 0.01f; minV = 0.01f;  maxV = 10.0f;  break;
        default: // TRANSLATE
            val = &m_snapTranslate; fmt = "%.3g m";   tip = "Move snap (metres per step)";
            speed = 0.05f; minV = 0.001f; maxV = 100.0f; break;
        }
        ImGui::SetNextItemWidth(buttonSize * 2.6f);
        ImGui::DragFloat("##snapval", val, speed, minV, maxV, fmt,
                         ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", tip);
    }

    // ---- Global View Mode dropdown (right-aligned on the toolbar row) ----
    // Applies to ALL objects. Wireframe and every lighting-debug view also suppress
    // sky/clouds/fog/TAA in the Renderer so the result reads against a clean dark
    // background (see Renderer::IsLightingDebugView).
    if (m_renderer)
    {
        // ORDER IS LOAD-BEARING: this array is indexed by the raw enum value,
        // which is uploaded straight into LightCB.viewMode and compared against
        // the VIEW_MODE_* defines in shaders/view_mode_common.hlsli. Insert only
        // at the END, and only alongside matching entries in both mirrors.
        static const char* kViewModes[] = {
            "Lit", "Unlit", "Wireframe",
            "Direct Diffuse", "Direct Specular", "Direct Transmission",
            "Indirect Diffuse", "Indirect Specular",
            "Direct Only", "Indirect Only",
            "Physical Visibility", "Form Shadow",
            "Base Color", "World Normal", "Roughness", "Metallic",
            "AO", "Depth",
            "Meshlets", "Meshlet LOD",
            "Sky Visibility", "IBL Only",
        };
        static_assert(IM_ARRAYSIZE(kViewModes) == (int)Renderer::ViewMode::Count,
                      "kViewModes must mirror Renderer::ViewMode 1:1 — and both must "
                      "mirror VIEW_MODE_* in shaders/view_mode_common.hlsli");
        const float comboW  = 150.0f * uiScale;
        // The Buffer menu (DrawDebugTargetMenu) + its options button sit
        // left of the View Mode combo; the three are right-aligned together.
        const float bufferW = 170.0f * uiScale;
        const float groupW  = bufferW + itemSpacing + buttonSize + 3.0f * itemSpacing + comboW;
        ImGui::SameLine();
        const float rightX = ImGui::GetCursorPosX()
                           + ImGui::GetContentRegionAvail().x - groupW;
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), rightX));
        DrawDebugTargetMenu(*m_renderer, bufferW, buttonSize,
                            (std::string(iconOrLabel(FontAwesome::kSliders, "...")) + "##dtopts").c_str());
        ImGui::SameLine(0.0f, 3.0f * itemSpacing);
        ImGui::SetNextItemWidth(comboW);
        int vm = (int)m_renderer->GetViewMode();
        if (ImGui::Combo("##viewmode", &vm, kViewModes, IM_ARRAYSIZE(kViewModes)))
            m_renderer->SetViewMode((Renderer::ViewMode)vm);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "View Mode.\n\n"
                "Lit / Unlit / Wireframe.\n\n"
                "The rest isolate one bucket of the lighting taxonomy\n"
                "(shaders/lighting_types.hlsli):\n"
                "  Direct*   = proportional to one light's radiance, shadow-gated\n"
                "  Indirect* = IBL / DDGI / probe / traced, never shadow-gated\n"
                "  Direct Only + Indirect Only == Lit, exactly\n"
                "  (emissive excepted - it bypasses this pass entirely)\n\n"
                "Physical Visibility = what an occlusion query returned.\n"
                "Form Shadow         = authored surface shading (SDF, half-lambert).\n"
                "Confusing those two is the bug class this exists to catch.\n\n"
                "RED-SUN LEAK TEST: set the sun to pure red and the ambient to\n"
                "pure blue. Any RED in a fully CSM-shadowed pixel is a direct-\n"
                "energy leak; any BLUE that changes when the sun is occluded is a\n"
                "shadow term wrongly gating indirect.\n\n"
                "Base Color .. Depth show RAW GBuffer channels (no BRDF).\n"
                "Depth: white = near, black = far/sky (reversed-Z, 4th-root ramp).\n"
                "AO = baked AO x SSAO. No velocity view - LightingPass does not\n"
                "bind the velocity target.\n\n"
                "Sky Visibility = what the sky light is multiplied by:\n"
                "  R diffuse, G specular, B the GI-owned share of the hemisphere.\n"
                "  Without GI nothing occludes the sky: R and G stay bright indoors.\n"
                "IBL Only = the light the sky delivers directly (not via GI,\n"
                "  probes or traced reflections).\n\n"
                "Any render target exactly as stored (velocity, SSAO, RT masks,\n"
                "every graph texture): the Buffer menu to the left.");
        }
    }

    ImGui::PopStyleVar(2);
}

// The toolbar's Play/Pause button and Ctrl+P. Buttons and keys express INTENT to
// the PlayModeController (the authoritative EngineCore state machine); App applies
// the transition next frame. Without a controller the local flag stands in so the
// toolbar still reflects a state.
void EditorLayer::TogglePlayPause()
{
    const bool isPlaying = (GetPlayState() == ViewportPlayState::Playing);
    WritePrePlayAutosave();
    if (m_playModeController)
        m_playModeController->RequestTogglePlay();
    else
        m_playState = isPlaying ? ViewportPlayState::Paused : ViewportPlayState::Playing;
}

// Pre-Play snapshot: entering Play is the most common step before a crash takes
// unsaved edits with it. Cheap (dirty-gated), synchronous, and never touches the
// authoring .iscene. Called by the toolbar, Ctrl+P and Editor.Play.
void EditorLayer::WritePrePlayAutosave()
{
    if (m_autosaveEnabled && m_sceneDirty && GetPlayState() == ViewportPlayState::Stopped)
        WriteAutosave("pre-play");
}

void EditorLayer::FocusSelectedEntity()
{
    if (!m_world || m_selectedEntity == NullEntity || !m_world->IsAlive(m_selectedEntity))
        return;
    // Canvas UI lives in screen space: there is no 3D place to frame (a loaded
    // canvas root sits at the origin).
    if (UI::IsUINode(*m_world, m_selectedEntity)) return;
    const GlobalTransform* tgt = m_world->GetComponent<GlobalTransform>(m_selectedEntity);
    if (!tgt) return;
    const DirectX::XMFLOAT3 targetPos{ tgt->matrix._41, tgt->matrix._42, tgt->matrix._43 };

    // Fit distance from the entity's local AABB (world-scaled), else a default.
    float fitDist = 5.0f;
    if (const LocalAabb* ab = m_world->GetComponent<LocalAabb>(m_selectedEntity))
    {
        const float dx = ab->max.x - ab->min.x;
        const float dy = ab->max.y - ab->min.y;
        const float dz = ab->max.z - ab->min.z;
        const float sx = std::sqrt(tgt->matrix._11 * tgt->matrix._11 +
                                   tgt->matrix._12 * tgt->matrix._12 +
                                   tgt->matrix._13 * tgt->matrix._13);   // X-basis length ~ scale
        const float r  = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz) * (sx > 1e-4f ? sx : 1.f);
        fitDist = std::max(2.0f, r * 2.5f);
    }

    // Ortho views: focusing recenters/zooms the view instead of moving a camera.
    if (m_editorViewMode != EditorViewMode::Perspective
        && GetPlayState() == ViewportPlayState::Stopped)
    {
        m_orthoFocus      = targetPos;
        m_orthoHalfHeight = std::clamp(fitDist * 0.75f, 0.05f, 5000.f);
        return;
    }

    // In Edit mode the editor's transient free camera (EditorCameraTag) owns the
    // viewport: focus must move IT, never the scene's camera entity.
    Entity editorCam = NullEntity;
    if (GetPlayState() == ViewportPlayState::Stopped)
        if (auto* tagPool = m_world->GetPool<EditorCameraTag>())
            for (Entity e : tagPool->Entities())
                if (m_world->IsAlive(e)) { editorCam = e; break; }

    auto* pool = m_world->GetPool<CameraControllerComponent>();
    if (!pool) return;
    const auto& ents = pool->Entities();
    auto&       data = pool->Data();
    for (std::size_t i = 0; i < ents.size(); ++i)
    {
        const Entity cam = ents[i];
        if (!m_world->IsAlive(cam)) continue;
        if (editorCam != NullEntity && cam != editorCam) continue;
        LocalTransform* lt = m_world->GetComponent<LocalTransform>(cam);
        if (!lt) continue;
        const CameraControllerComponent& cc = data[i];
        // Forward from yaw/pitch (CameraSystem FPS LH convention). Reposition so the
        // target sits `fitDist` ahead, keeping the current look direction.
        const float cp = std::cos(cc.pitch), sp = std::sin(cc.pitch);
        const float cy = std::cos(cc.yaw),   sy = std::sin(cc.yaw);
        const DirectX::XMFLOAT3 fwd{ sy * cp, -sp, cy * cp };
        lt->translation = { targetPos.x - fwd.x * fitDist,
                            targetPos.y - fwd.y * fitDist,
                            targetPos.z - fwd.z * fitDist };
        // The camera is a root entity: patch GlobalTransform so the move shows this
        // frame (the next TransformSystem::Propagate reaffirms it).
        if (GlobalTransform* gt = m_world->GetComponent<GlobalTransform>(cam))
        {
            gt->matrix._41 = lt->translation.x;
            gt->matrix._42 = lt->translation.y;
            gt->matrix._43 = lt->translation.z;
        }
        break;
    }
}

void EditorLayer::RenderViewportPanel()
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool viewportVisible = ImGui::Begin(kWindowViewport);
    ImGui::PopStyleVar();
    Editor::FocusTracker::Get().Note(Editor::FocusTracker::kSceneView, viewportVisible);

    // The toolbar row (RenderViewportToolbar), with its own padding for this section
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(50.0f, 4.0f));
    RenderViewportToolbar();
    ImGui::PopStyleVar();
    ImGui::Separator();

    ImVec2 size = ImGui::GetContentRegionAvail();
    CacheRenderableViewportSize(size);
    if (size.x > 0 && size.y > 0)
    {
        const float contentW = size.x;
        const float contentH = size.y;

        // Pre-compute viewport rect (vpMin/vpMax used by both draw list and ImGuizmo).
        const ImVec2 vpMin = ImGui::GetCursorScreenPos();
        const ImVec2 vpMax = ImVec2(vpMin.x + contentW, vpMin.y + contentH);
        m_viewportMin = vpMin;  // cached for material drag-drop pick coordinate

        // ---- Draw scene texture / background (draw list, no item created) ----
        ImDrawList* vpDL = ImGui::GetWindowDrawList();
        // In Play with the Game View showing the game, the main pipeline's picture is the game's: the Scene
        // keeps the editor camera through its own secondary view (EditorLayer_GameView.cpp).
        // A hidden Scene tab asks for no picture: the view goes at the next frame boundary.
        const bool sceneAux = Editor::GameView::Get().Simulating()
                              && Editor::GameView::Get().Route() == Editor::MainRoute::Game;
        if (sceneAux)
        {
            if (viewportVisible)
                DrawSceneAuxPicture(vpDL, vpMin, vpMax);
        }
        else if (m_sceneTextureId != 0)
        {
            vpDL->AddImage(
                ImTextureRef(static_cast<ImTextureID>(m_sceneTextureId)),
                vpMin, vpMax);
        }
        else
        {
            vpDL->AddRectFilled(vpMin, vpMax, IM_COL32(40, 44, 52, 255));
            const char* hint = "Drag .iscn or .ipfb here to spawn";
            ImVec2 tsz = ImGui::CalcTextSize(hint);
            vpDL->AddText(
                ImVec2(vpMin.x + (size.x - tsz.x) * 0.5f,
                       vpMin.y + (size.y - tsz.y) * 0.5f),
                IM_COL32(120, 120, 140, 200), hint);
        }

        // ---- Play / Paused frame ----
        // Drawn before the gizmo + overlays so it tints the scene, not the UI. Without a
        // loud marker it is easy to keep authoring inside a Play world and only find out
        // at Stop, when the pre-Play snapshot is restored over it (File > Save is greyed
        // out there for the same reason).
        {
            const ViewportPlayState play = GetPlayState();
            if (play != ViewportPlayState::Stopped)
            {
                const bool  paused = (play == ViewportPlayState::Paused);
                const ImU32 tint   = paused ? IM_COL32(255, 185,  40, 14)
                                            : IM_COL32(255,  85,  70, 14);
                const ImU32 edge   = paused ? IM_COL32(255, 185,  40, 220)
                                            : IM_COL32(255,  85,  70, 220);
                vpDL->AddRectFilled(vpMin, vpMax, tint);
                vpDL->AddRect(vpMin, vpMax, edge, 0.f, 0, 3.f);

                const char*  tag  = paused ? "PAUSED" : "PLAYING";
                const ImVec2 tsz  = ImGui::CalcTextSize(tag);
                const ImVec2 bMin(vpMin.x + 10.f, vpMin.y + 10.f);
                const ImVec2 bMax(bMin.x + tsz.x + 12.f, bMin.y + tsz.y + 6.f);
                vpDL->AddRectFilled(bMin, bMax, IM_COL32(0, 0, 0, 150), 3.f);
                vpDL->AddText(ImVec2(bMin.x + 6.f, bMin.y + 3.f), edge, tag);
            }
        }

        // ---- A requested DLSS-RR that is not running ----
        // SetAAMode keeps the request and falls back; say so ON the picture. Not on the
        // Raster path with the plugin loaded: RR is an option of DLSS that has nothing to
        // denoise there, and DLSS Super Resolution runs as asked (IsRRFallbackToDLSS).
        const bool rrIdleOnRaster = m_renderer
            && m_renderer->GetRenderPath() == Renderer::RenderPath::Raster
            && m_renderer->IsRRFallbackToDLSS();
        if (m_renderer && m_renderer->GetAAModeRequested() == Renderer::AAMode::DLSS_RR && !rrIdleOnRaster)
        {
            if (const char* why = m_renderer->GetRayReconstructionInertReason())
            {
                char text[256];
                std::snprintf(text, sizeof(text), "DLSS-RR inactive: %s", why);
                const ImVec2 tsz = ImGui::CalcTextSize(text);
                const ImVec2 bMin(vpMin.x + 10.f, vpMax.y - tsz.y - 16.f);
                const ImVec2 bMax(bMin.x + tsz.x + 12.f, bMin.y + tsz.y + 6.f);
                vpDL->AddRectFilled(bMin, bMax, IM_COL32(0, 0, 0, 160), 3.f);
                vpDL->AddText(ImVec2(bMin.x + 6.f, bMin.y + 3.f), IM_COL32(255, 200, 80, 255), text);
            }
        }

        // ---- Bone picking, BEFORE the gizmo submit ----
        // Runs here rather than in HandleViewportPicking (called after the gizmo) so a
        // bone selected by this click gets its handle in the SAME frame, not "click to
        // select, click again to drag". Hover is tested by hand because the "##viewport"
        // InvisibleButton that would answer it is submitted after the gizmo.
        m_bonePickConsumed = false;
        if (GetPlayState() == ViewportPlayState::Stopped && m_chainMainPose.poseMode && m_world && m_renderer
            && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)
            && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
            && !ImGui::IsMouseDown(ImGuiMouseButton_Right)
            && !GizmoIsOver() && !GizmoIsUsingAny())
        {
            const ImVec2 mp = ImGui::GetIO().MousePos;
            if (mp.x >= vpMin.x && mp.x <= vpMax.x && mp.y >= vpMin.y && mp.y <= vpMax.y)
            {
                const Entity poseTarget = ChainOverlayCandidate(*m_world);
                if (poseTarget != NullEntity && m_world->IsAlive(poseTarget))
                {
                    const uint32_t hit = PickChainBone(
                        *m_renderer, *m_world, poseTarget,
                        mp.x - vpMin.x, mp.y - vpMin.y,
                        vpMax.x - vpMin.x, vpMax.y - vpMin.y);
                    if (hit != ~0u)
                    {
                        m_chainMainPose.poseBone    = hit;
                        m_bonePickConsumed = true;   // swallow it (see HandleViewportPicking)
                    }
                }
            }
        }

        ImGui::SetCursorScreenPos(vpMin);
        // The bone gizmo owns the viewport while a bone is selected: two ImGuizmo
        // instances would fight for the same click. Every gizmo is submitted before the
        // "##viewport" InvisibleButton: ImGuizmo's CanActivate() needs no item hovered/active.
        if (m_chainMainPose.poseMode && m_chainMainPose.poseBone != ~0u)
            RenderBonePoseGizmo(vpMin, vpMax);
        else if(m_ikPreviewSlot>=0 && m_ikPreviewEntity==m_selectedEntity && GetPlayState()==ViewportPlayState::Stopped)
            RenderIKTargetGizmo(vpMin,vpMax);
        else
            RenderViewportGizmo(vpMin, vpMax);

        ImGui::SetCursorScreenPos(vpMin);
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##viewport", size,
            ImGuiButtonFlags_MouseButtonLeft |
            ImGuiButtonFlags_MouseButtonRight |
            ImGuiButtonFlags_MouseButtonMiddle);

        const bool vpClicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        // "Some OTHER widget owns the mouse": the viewport's own InvisibleButton does not
        // count. It takes ImGui's ActiveId on the mouse-DOWN edge (default
        // PressedOnClickRelease), so a plain IsAnyItemActive() would read true for the
        // whole press and lock out drags over the scene image. IsItemActive() still
        // refers to that button here, which exempts it.
        const bool otherItemActive =
            ImGui::IsAnyItemActive() && !ImGui::IsItemActive();
        m_viewportOtherItemActive = otherItemActive;   // App reads it for Canvas input

        // ---- The view cube, over the picture's top-right corner ----
        // Edit mode only (Play's camera is the scene's), and not under the harness layout, whose
        // panel pictures are pinned. A press on the cube is never a pick.
        bool cubeOwnsMouse = false;
        m_viewCubeRect[2] = 0.f;
        if (m_showViewCube && (!m_harnessImGuiLayout || m_viewCubeForTests)
            && GetPlayState() == ViewportPlayState::Stopped)
        {
            const float side = std::min(110.f, std::min(contentW, contentH) * 0.3f);
            m_viewCubeRect[0] = vpMax.x - side - 8.f;
            m_viewCubeRect[1] = vpMin.y + 8.f;
            m_viewCubeRect[2] = side;
            const Editor::ViewCube::Basis basis = Editor::ViewCube::BasisOfView(m_renderView.viewMatrix);
            const int activeView = m_editorViewMode == EditorViewMode::Perspective
                                       ? -1 : static_cast<int>(m_editorViewMode) - 1;
            const Editor::ViewCube::Input cube =
                Editor::ViewCube::Draw(vpDL, vpMax.x - side - 8.f, vpMin.y + 8.f, side, basis, activeView);
            cubeOwnsMouse = cube.hovered;
            if (cube.clickedView >= 0) m_editorViewMode = static_cast<EditorViewMode>(cube.clickedView + 1);
            if (cube.dragging) TurnEditorCamera(cube.dragX, cube.dragY, basis.forward);
        }

        // F - frame the selected entity (editor free-fly camera). Gated on viewport
        // hover + not typing so it does not fire from the Inspector or a rename, and off
        // while the Game View has the game's keyboard (the key is the game's then).
        const bool gameKeys = Editor::GameView::Get().GameHasKeyboard();
        if (hovered && !ImGui::GetIO().WantTextInput && !gameKeys
            && ImGui::IsKeyPressed(ImGuiKey_F, false))
            FocusSelectedEntity();

        // End - drop the selected entity onto the floor (as in Unreal). Undo-recorded;
        // see SnapSelectedToFloor for the ray details.
        if (hovered && !ImGui::GetIO().WantTextInput && !gameKeys
            && ImGui::IsKeyPressed(ImGuiKey_End, false))
            SnapSelectedToFloor();

        // Camera bookmarks: Ctrl+Shift+1..4 store, Ctrl+1..4 recall. Same hover/typing
        // gate as F; free-cam pose only, so Edit + Perspective.
        if (hovered && !ImGui::GetIO().WantTextInput && ImGui::GetIO().KeyCtrl
            && m_editorViewMode == EditorViewMode::Perspective
            && GetPlayState() == ViewportPlayState::Stopped)
        {
            for (int b = 0; b < kCameraBookmarks; ++b)
            {
                if (!ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_1 + b), false))
                    continue;
                if (ImGui::GetIO().KeyShift) StoreCameraBookmark(b);
                else                         RestoreCameraBookmark(b);
            }
        }

        // Highlight drop zone when a drag is in flight

        if (ImGui::IsDragDropActive())
        {
            vpDL->AddRectFilled(vpMin, vpMax, IM_COL32(60, 120, 255, 25));
            vpDL->AddRect(vpMin, vpMax, IM_COL32(80, 160, 255, 180), 0.f, 0, 2.f);
        }

        // ---- Drag-drop target: .iscn / Mesh / .ipfb / .imat / .ianim drops ----
        if (ImGui::BeginDragDropTarget())
        {
            // Cursor position in viewport-relative pixels: projects the drop point into
            // the world so dropped assets land under the cursor.
            const ImVec2 dropMouse = ImGui::GetMousePos();
            const float  dropX     = dropMouse.x - vpMin.x;
            const float  dropY     = dropMouse.y - vpMin.y;

            if (const ImGuiPayload* payload =
                    ImGui::AcceptDragDropPayload("ISCN_PATH"))
            {
                const char* path = static_cast<const char*>(payload->Data);
                if (!m_world || !m_assetMgr)
                    LOG_ERROR("EditorLayer: world/assetMgr not wired — cannot spawn scene");
                else
                {
                    Resource::MeshLibrary* meshLib = m_renderer
                        ? m_renderer->GetMeshLibrary() : nullptr;
                    if (!meshLib)
                    {
                        LOG_ERROR("EditorLayer: MeshLibrary not wired — cannot spawn scene");
                    }
                    else
                    {
                        bool ok = false; Entity root = NullEntity;
                        uint32_t nodeCount = 0, meshCount = 0;
                        RecordSpawn("Spawn Scene", [&]{
                            auto res = SceneInstanceLoader::Load(path, *m_world, *m_assetMgr, *meshLib, m_renderer);
                            ok = res.success; root = res.rootEntity;
                            nodeCount = res.nodeCount; meshCount = res.meshEntityCount;
                            // Place the instance root where the user dropped it.
                            if (ok)
                                if (auto* lt = m_world->GetComponent<LocalTransform>(root))
                                    lt->translation = ComputeSpawnPosition(dropX, dropY);
                        });
                        if (ok)
                        {
                            m_selectedEntity = root;
                            LOG_SUCCESS("EditorLayer: spawned scene '%s' — %u nodes, %u meshes",
                                        path, nodeCount, meshCount);
                        }
                        else
                            LOG_ERROR("EditorLayer: failed to spawn scene '%s'", path);
                    }
                }
            }

            // One Mesh of a model (a Content Browser Mesh item) -> a StaticMesh owner
            // under the cursor.
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("STATICMESH_REF"))
            {
                std::string model, name;
                uint64_t    key = 0;
                if (m_world && ParseStaticMeshRef(static_cast<const char*>(payload->Data), model, key, name))
                {
                    Entity e = NullEntity;
                    RecordSpawn("Place Mesh", [&] {
                        e = CreateStaticMeshEntity(model, key, name, ComputeSpawnPosition(dropX, dropY));
                    });
                    if (e != NullEntity)
                    {
                        m_selectedEntity = e;
                        MarkHierarchyDirty();
                        LOG_SUCCESS("EditorLayer: placed mesh '%s' of '%s'", name.c_str(), model.c_str());
                    }
                }
            }

            // Prefab drop into the 3D view -> spawn under the cursor.
            if (const ImGuiPayload* payload =
                    ImGui::AcceptDragDropPayload("IPFB_PATH"))
            {
                const char* pfbPath = static_cast<const char*>(payload->Data);
                if (!m_world || !m_assetMgr)
                    LOG_ERROR("EditorLayer: world/assetMgr not wired — cannot spawn prefab");
                else
                {
                    Entity spawned = NullEntity;
                    RecordSpawn("Spawn Prefab", [&]{
                        spawned = Resource::LoadPrefab(pfbPath, *m_world, *m_assetMgr,
                                                       m_renderer, m_animClipSys);
                        // A UI prefab: a detached copy on a canvas, not a 3D placement.
                        if (spawned != NullEntity && !AdoptUIPrefabRoot(spawned, NullEntity))
                            if (auto* lt = m_world->GetComponent<LocalTransform>(spawned))
                                lt->translation = ComputeSpawnPosition(dropX, dropY);
                    });
                    if (spawned != NullEntity)
                    {
                        m_selectedEntity = spawned;
                        LOG_SUCCESS("EditorLayer: spawned prefab '%s' — root entity %u", pfbPath, spawned);
                    }
                    else
                        LOG_ERROR("EditorLayer: failed to spawn prefab '%s'", pfbPath);
                }
            }

            // Material drop: request a pick at the drop location, apply when resolved.
            if (const ImGuiPayload* payload =
                    ImGui::AcceptDragDropPayload("IMAT_PATH"))
            {
                const char* path = static_cast<const char*>(payload->Data);
                m_pendingMatDropPath = path;
                // Pick at the viewport-relative mouse position; m_viewportMin is reliable
                // inside a drag-drop.
                const ImVec2 mousePos = ImGui::GetMousePos();
                if (m_pickCallback)
                    m_pickCallback(mousePos.x - m_viewportMin.x,
                                   mousePos.y - m_viewportMin.y);
                m_hasPendingDrag = true;
            }

            // Animation drop: begin async load, resolve when clip is ready.
            if (const ImGuiPayload* payload =
                    ImGui::AcceptDragDropPayload("IANIM_PATH"))
            {
                const char* path = static_cast<const char*>(payload->Data);
                if (!m_world || !m_animClipSys)
                    LOG_ERROR("EditorLayer: world/animClipSys not wired — cannot assign animation");
                else if (m_selectedEntity == NullEntity)
                    LOG_WARNING("EditorLayer: no entity selected — select an entity first");
                else
                {
                    AssignAnimationAsset(m_selectedEntity, path);
                }
            }

            ImGui::EndDragDropTarget();
        }

        // Right-mouse drag -> camera rotation (perspective) / pan (ortho views).
        m_viewportRightDragging = hovered && ImGui::IsMouseDown(ImGuiMouseButton_Right);
        m_viewportHovered       = hovered;   // gamepad look gate (App.cpp)
        // In Play, while the Scene viewport shows the editor camera, its right drag flies that camera with the
        // editor's own keys: the game's input never moves it, and it never moves the game (GetGameMouseLook).
        m_sceneFly = {};
        if (Editor::GameView::Get().SceneObservesPlay() && m_viewportRightDragging)
        {
            const ImGuiIO& io = ImGui::GetIO();
            m_sceneFly.active  = true;
            m_sceneFly.lookDX  = io.MouseDelta.x;
            m_sceneFly.lookDY  = io.MouseDelta.y;
            m_sceneFly.forward = ImGui::IsKeyDown(ImGuiKey_W);
            m_sceneFly.back    = ImGui::IsKeyDown(ImGuiKey_S);
            m_sceneFly.left    = ImGui::IsKeyDown(ImGuiKey_A);
            m_sceneFly.right   = ImGui::IsKeyDown(ImGuiKey_D);
            m_sceneFly.up      = ImGui::IsKeyDown(ImGuiKey_E);
            m_sceneFly.down    = ImGui::IsKeyDown(ImGuiKey_Q);
            m_sceneFly.fast    = io.KeyShift;
        }
        const bool orthoView = (m_editorViewMode != EditorViewMode::Perspective)
                            && (GetPlayState() == ViewportPlayState::Stopped);
        if (orthoView)
        {
            // Screen right/up per axis view: Editor/EditorOrthoViews.h, the one
            // table the ortho camera is built from too.
            const int a = static_cast<int>(m_editorViewMode) - 1;
            if (m_viewportRightDragging)
            {
                const ImVec2 d     = ImGui::GetIO().MouseDelta;
                const float  scale = (contentH > 0.f)
                    ? (2.0f * m_orthoHalfHeight) / contentH : 0.f;
                const DirectX::XMFLOAT3  right = Editor::OrthoViews::Right(a);
                const DirectX::XMFLOAT3& up    = Editor::OrthoViews::kUp[a];
                m_orthoFocus.x -= (right.x * d.x - up.x * d.y) * scale;
                m_orthoFocus.y -= (right.y * d.x - up.y * d.y) * scale;
                m_orthoFocus.z -= (right.z * d.x - up.z * d.y) * scale;
            }
            if (hovered)
            {
                const float wheel = ImGui::GetIO().MouseWheel;
                if (wheel != 0.f)
                    m_orthoHalfHeight = std::clamp(
                        m_orthoHalfHeight * std::pow(0.85f, wheel), 0.05f, 5000.f);
            }
            // Don't feed the perspective fly-cam while an ortho view is active.
            m_vpMouseDX = 0.f;
            m_vpMouseDY = 0.f;
        }
        else if (m_viewportRightDragging)
        {
            const ImVec2 delta = ImGui::GetIO().MouseDelta;
            m_vpMouseDX = delta.x;
            m_vpMouseDY = delta.y;
        }
        else
        {
            m_vpMouseDX = 0.f;
            m_vpMouseDY = 0.f;
        }

        // Before picking: a click that lands on a rect handle must not also fire
        // a GPU pick, whose result arrives a frame later and would retarget the
        // selection out from under the drag.
        RenderUIRectTool(vpMin, otherItemActive);

        // Left-click picking and body drag (skipped over gizmo handles, see GizmoIsOver).
        HandleViewportPicking(vpMin.x, vpMin.y, contentW, contentH, hovered && !cubeOwnsMouse,
                              /*allowBonePick=*/true);

        // Keyboard shortcuts for the gizmo operation (W/E/R, viewport hovered).
        // Blocked mid-drag: switching the operation while ImGuizmo is using would strand
        // its internal mbUsing (every handler early-outs on the new op and the
        // mouse-release deactivation becomes unreachable).
        if (GetPlayState() == ViewportPlayState::Stopped && hovered && !ImGuizmo::IsUsing())
        {
            if (ImGui::IsKeyPressed(ImGuiKey_W)) m_gizmoOperation = ImGuizmo::TRANSLATE;
            if (ImGui::IsKeyPressed(ImGuiKey_E)) m_gizmoOperation = ImGuizmo::ROTATE;
            if (ImGui::IsKeyPressed(ImGuiKey_R)) m_gizmoOperation = ImGuizmo::SCALE;
        }

        // Tracer test: T fires along view ray. Gated on m_tracerTestModeEnabled + hover.
        if (hovered
            && m_tracerTestModeEnabled
            && m_renderer
            && ImGui::IsKeyPressed(ImGuiKey_T, /*repeat=*/false))
        {
            if (TracerSystem* ts = m_renderer->GetTracerSystem())
            {
                using DirectX::XMFLOAT3;
                using DirectX::XMFLOAT4;
                const XMFLOAT3& cp = m_renderView.cameraPosition;
                const XMFLOAT3& cf = m_renderView.cameraForward;
                XMFLOAT3 start{ cp.x + cf.x * 0.5f,
                                cp.y + cf.y * 0.5f,
                                cp.z + cf.z * 0.5f };
                XMFLOAT3 end  { cp.x + cf.x * 30.0f,
                                cp.y + cf.y * 30.0f,
                                cp.z + cf.z * 30.0f };
                ts->Spawn(start, end,
                          XMFLOAT4{ 1.0f, 0.85f, 0.3f, 6.0f },
                          /*width=*/0.5f,
                          /*lifetime=*/0.8f);
            }
        }
    }

    ImGui::End();
}

DirectX::XMFLOAT3 EditorLayer::ComputeSpawnPosition(float vpX, float vpY) const
{
    using namespace DirectX;

    // Fallback distance from the eye when the ray hits no collision: a spawn into
    // the void stays comfortably in view whatever the camera angle.
    constexpr float kSpawnDistance = 8.0f;

    const float vpW = (m_viewportSizeX > 0) ? static_cast<float>(m_viewportSizeX) : 1.f;
    const float vpH = (m_viewportSizeY > 0) ? static_cast<float>(m_viewportSizeY) : 1.f;

    // Negative coords -> aim at the viewport center (NDC origin, resolution-independent).
    const float px = (vpX >= 0.f) ? vpX : vpW * 0.5f;
    const float py = (vpY >= 0.f) ? vpY : vpH * 0.5f;

    // FromViewport is reverse-Z robust: origin = near point, direction = into the
    // scene, so a fixed offset along the ray lands that many units in front of the
    // camera (under the cursor for drops).
    const Ray ray = Ray::FromViewport(px, py, vpW, vpH, m_renderView);

    // Surface projection: a drop / spawn lands on whatever the cursor points at (as
    // in UE / Unity). Jolt closest-hit on the default layer mask (hurt / attack /
    // sensor volumes excluded); a miss (sky, or a scene with no collision) falls back
    // to kSpawnDistance. Lifted 5 cm along the hit normal so the pivot is not buried.
    if (m_physicsSys)
    {
        const auto hit = m_physicsSys->CastRayClosest(
            ray.origin, ray.direction, /*maxDistance*/ 500.f);
        if (hit.hit)
            return { hit.point.x + hit.normal.x * 0.05f,
                     hit.point.y + hit.normal.y * 0.05f,
                     hit.point.z + hit.normal.z * 0.05f };
    }
    return ray.At(kSpawnDistance);
}

// End key: rest the selected entity's AABB bottom on the first collision surface
// below it. Undo-recorded (TransformEditAction, same path as a gizmo drag). Needs
// a physics-backed floor: purely visual scenes miss and no-op.
void EditorLayer::SnapSelectedToFloor()
{
    if (!m_world || !m_physicsSys) return;
    if (m_selectedEntity == NullEntity || !m_world->IsAlive(m_selectedEntity)) return;
    if (UI::IsUINode(*m_world, m_selectedEntity)) return;   // Canvas UI has no floor to drop to
    if (GetPlayState() != ViewportPlayState::Stopped) return;   // authoring edit only

    const GlobalTransform* gt = m_world->GetComponent<GlobalTransform>(m_selectedEntity);
    LocalTransform*        lt = m_world->GetComponent<LocalTransform>(m_selectedEntity);
    if (!gt || !lt) return;

    const DirectX::XMFLOAT3 wpos{ gt->matrix._41, gt->matrix._42, gt->matrix._43 };
    // Pivot height above the AABB bottom, world-scaled (Y-basis length ~ vertical
    // scale, the heuristic FocusSelectedEntity uses).
    float bottomOffset = 0.f;
    if (const LocalAabb* ab = m_world->GetComponent<LocalAabb>(m_selectedEntity))
    {
        const float sy = std::sqrt(gt->matrix._21 * gt->matrix._21 +
                                   gt->matrix._22 * gt->matrix._22 +
                                   gt->matrix._23 * gt->matrix._23);
        bottomOffset = -ab->min.y * (sy > 1e-4f ? sy : 1.f);
    }

    // Cast from just BELOW the entity's own bottom so its own collider (if any)
    // cannot be the first hit. Already resting on the floor -> the ray starts inside
    // the floor body and misses -> clean no-op.
    const DirectX::XMFLOAT3 from{ wpos.x, wpos.y - bottomOffset - 0.02f, wpos.z };
    const auto hit = m_physicsSys->CastRayClosest(from, { 0.f, -1.f, 0.f }, 2000.f);
    if (!hit.hit) return;

    const float dy = (hit.point.y + bottomOffset) - wpos.y;
    if (std::fabs(dy) < 1e-4f) return;

    // World dy applied to local Y: exact for roots and unrotated parents (the
    // normal case for placed props); a rotated parent gets an approximation rather
    // than a full parent-space solve.
    const LocalTransform before = *lt;
    lt->translation.y += dy;
    PushEdit(std::make_unique<Editor::Undo::TransformEditAction>(
        m_world->MakeHandle(m_selectedEntity), before, *lt));
    // Show the drop this frame; Propagate reaffirms it next frame (the same
    // root-entity patch FocusSelectedEntity does).
    if (GlobalTransform* g2 = m_world->GetComponent<GlobalTransform>(m_selectedEntity))
        g2->matrix._42 += dy;
}

void EditorLayer::HandleViewportPicking(float imageMinX, float imageMinY,
                                        float contentW,  float contentH,
                                        bool  hovered,
                                        bool  allowBonePick)
{
    // Play owns viewport mouse input, including paused/stepped simulation.
    if (GetPlayState() != ViewportPlayState::Stopped)
    {
        m_hasPendingDrag = m_isDragging = false;
        m_bonePickConsumed = m_uiRectToolClaimsMouse = false;
        m_uiRectDrag.active = false;
        m_pendingMatDropPath.clear();
        return;
    }
    if (contentW <= 0.f || contentH <= 0.f) return;

    // A click consumed by bone picking must not also reach the GPU pick: its result
    // arrives a frame later and would retarget m_selectedEntity out from under the
    // pose session. Only the PICK branch is suppressed; the drag/release bookkeeping
    // below still runs. The F11 fullscreen path passes allowBonePick=false: it never
    // submits the bone gizmo, so swallowing a click there would be a dead click.
    const bool boneAte = allowBonePick && m_bonePickConsumed;
    m_bonePickConsumed = false;

    // Same rule for the Canvas rect tool, which runs just before this and has
    // flagged the click it took: otherwise the GPU pick fires on the same press,
    // resolves a frame later to the 3D geometry behind the UI, and swaps
    // m_selectedEntity out from under the live drag.
    //
    // Break a latched drag rather than masking it. The flag is otherwise cleared only
    // inside the tool, so if the viewport stops being submitted mid-drag (panel
    // collapsed to zero content) it would stay set - killing viewport picking and the
    // undo shortcuts, which defer to it - and the tool would later resume from a
    // stale press position.
    if (m_uiRectDrag.active && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
        CommitUIRectDrag();
    const bool uiRectAte = m_uiRectDrag.active;

    const ImGuiIO& io      = ImGui::GetIO();
    const float    px      = io.MousePos.x - imageMinX;
    const float    py      = io.MousePos.y - imageMinY;
    const bool     rightHeld = ImGui::IsMouseDown(ImGuiMouseButton_Right);

    // ---- Double-click on Canvas UI -> the UI Editor ----
    // An editor-side hit test of the game's own UIComputedRect (front to back by
    // sortKey) - the GPU pick never sees Canvas UI - and that press's pick is skipped
    // (the uiRectAte rule): the node opens in its canvas's tab, selected. Only while
    // the viewport shows the screen UI, and not under F11 (no panels).
    bool uiOpened = false;
    if (hovered && !rightHeld && !boneAte && !uiRectAte && m_showScreenUI && !m_viewportFullscreen
        && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !GizmoIsOver())
        if (const Entity hit = ScreenUIAt(px, py); hit != NullEntity)
        {
            Editor::UIEditorWindow::Get().RequestOpen(hit);
            uiOpened = true;
            LOG_INFO("[UIEDITOR] viewport double-click at %.0f,%.0f -> '%s'", px, py, m_world->GetName(hit).c_str());
        }

    // ---- Left-click -> GPU pick callback (skipped over gizmo handles) ----
    // GizmoIsOver(), not ImGuizmo::IsOver(): the raw call answers from cached geometry
    // when no gizmo was submitted this frame, which swallows clicks in the
    // last-selected entity's gizmo footprint.
    if (hovered && !rightHeld && !boneAte && !uiRectAte && !uiOpened
        && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
        && !GizmoIsOver() && contentW > 0.f && contentH > 0.f)
    {
        // Latch the modifier NOW - see m_pickSelectMode.
        const ImGuiIO& pickIO = ImGui::GetIO();
        m_pickSelectMode = pickIO.KeyCtrl  ? SelectMode::Toggle
                         : pickIO.KeyShift ? SelectMode::Add
                                           : SelectMode::Replace;
        if (m_pickCallback)
            m_pickCallback(px, py);
        m_hasPendingDrag = true;
        m_isDragging = false;  // drag starts when pick result arrives
    }

    // ---- Left-hold: move entity on drag plane (off while the gizmo is manipulating) ----
    if (m_isDragging
        && ImGui::IsMouseDown(ImGuiMouseButton_Left)
        && !ImGuizmo::IsUsing()
        && m_dragEntity != NullEntity
        && m_world)
    {
        const Ray ray = Ray::FromViewport(px, py, contentW, contentH, m_renderView);

        float t;
        if (ray.IntersectPlane(m_dragPlanePoint, m_dragPlaneNormal, t))
        {
            const XMFLOAT3 newHit = ray.At(t);
            if (LocalTransform* lt = m_world->GetComponent<LocalTransform>(m_dragEntity))
            {
                lt->translation.x = newHit.x + m_dragEntityOffset.x;
                lt->translation.y = newHit.y + m_dragEntityOffset.y;
                lt->translation.z = newHit.z + m_dragEntityOffset.z;

                // Body-drag snaps to ABSOLUTE grid positions (aligns with the
                // reference grid), unlike the gizmo's relative-delta snap.
                if (m_snapEnabled != ImGui::GetIO().KeyCtrl && m_snapTranslate > 0.f)
                {
                    const float s = m_snapTranslate;
                    lt->translation.x = std::round(lt->translation.x / s) * s;
                    lt->translation.y = std::round(lt->translation.y / s) * s;
                    lt->translation.z = std::round(lt->translation.z / s) * s;
                }
            }
        }
    }

    // ---- Release: end drag ----
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        m_isDragging = false;
}

// The gizmo layer rule: both main-viewport gizmos draw into the CURRENT window's
// list, not ImGui::GetForegroundDrawList(), which is rendered after every window and
// would paint the arrows over any floating panel, popup or menu covering the
// viewport. RenderViewportPanel calls both at the Viewport window's level, so the
// list is the Viewport window's: clipped to its content and covered by anything
// above it (callers also clip to the scene image, vpMin..vpMax, sparing the
// toolbar row). Hover under a covering window is blocked either way:
// ImGuizmo::IsHoveringWindow accepts SetAlternativeWindow's window (the Viewport)
// and answers false while any other window is hovered. Unlike the foreground list
// ("##Foreground" names no window), a window's list is not matched by a NULL
// hovered window.
// The self-report (harness + -unattended only) logs the owner and the TOP window
// at the gizmo's centre, once per change - never per frame.
ImDrawList* EditorLayer::ViewportGizmoDrawList(const char* which, const ImVec2& center)
{
    if (!m_gizmoForegroundRead)
    {
        m_gizmoForegroundRead = true;
#if defined(WITH_HARNESS) && WITH_HARNESS
        // -editor_gizmo_foreground: draw on the foreground list (above every window),
        // the A/B arm of the layer rule.
        m_gizmoForeground = Sys::CommandLine::Get().Has("-editor_gizmo_foreground");
        if (m_gizmoForeground)
            LOG_INFO("[EDITOR] gizmo layer: -editor_gizmo_foreground - the main viewport's "
                     "gizmos draw into the FOREGROUND list (above every window)");
        // The self-report is a harness run's evidence only: an interactive
        // session returns the list and nothing else (no per-frame hover lookup,
        // no log line each time a floating window crosses the gizmo).
        m_gizmoLayerReportOn = Sys::CommandLine::Get().Has("-unattended");
#endif
    }
    ImDrawList* dl = m_gizmoForeground ? ImGui::GetForegroundDrawList()
                                       : ImGui::GetWindowDrawList();
#if defined(WITH_HARNESS) && WITH_HARNESS
    if (m_gizmoLayerReportOn)
    {
        const int slot = (which && std::strcmp(which, "restpose") == 0) ? 1 : 0;
        GizmoLayerReport& rep = m_gizmoLayer[slot];
        ++rep.submits;
        // Which window a click at the gizmo's centre would reach (the one ImGui
        // itself would pick: back-to-front, NoMouseInputs windows skipped).
        ImGuiWindow* top = nullptr;
        ImGuiWindow* underMoving = nullptr;
        ImGui::FindHoveredWindowEx(center, true, &top, &underMoving);
        const char* covered = (top && top->RootWindow) ? top->RootWindow->Name : "(none)";
        const char* owner   = dl->_OwnerName ? dl->_OwnerName : "(null)";
        const int   fg      = m_gizmoForeground ? 1 : 0;
        if (rep.foreground != fg || rep.owner != owner || rep.coveredBy != covered)
        {
            rep.foreground = fg;
            rep.owner      = owner;
            rep.coveredBy  = covered;
            // Judged against the Viewport WINDOW BY NAME, independently of the
            // window this is called from: the list must BE that window's own
            // list (a child window's or another window's list reads 0).
            ImGuiWindow* vpWin = ImGui::FindWindowByName(kWindowViewport);
            const bool ownerIsViewport = vpWin && vpWin->DrawList == dl;
            const bool coveredByOther  = top && vpWin && top->RootWindow != vpWin->RootWindow;
            LOG_INFO("[EDITOR] gizmo layer: which=%s owner='%s' ownerIsViewport=%d foreground=%d "
                     "coveredBy='%s' coveredByOther=%d cx=%.0f cy=%.0f submits=%llu",
                     slot == 1 ? "restpose" : "transform", owner, ownerIsViewport ? 1 : 0, fg,
                     covered, coveredByOther ? 1 : 0, center.x, center.y,
                     static_cast<unsigned long long>(rep.submits));
        }
    }
#else
    (void)which; (void)center;
#endif
    return dl;
}

void EditorLayer::RenderViewportGizmo(ImVec2 vpMin, ImVec2 vpMax)
{
    // Edit Collider takes the gizmo (and commits a drag whose target went away).
    if (RenderColliderGizmo(vpMin, vpMax)) return;
    if (GetPlayState() != ViewportPlayState::Stopped) return;
    if (m_selectedEntity == NullEntity || !m_world || !m_world->IsAlive(m_selectedEntity))
        return;

    // GlobalTransform is the single source of truth (DX row-major matches ImGuizmo).
    const GlobalTransform* gt = m_world->GetComponent<GlobalTransform>(m_selectedEntity);
    if (!gt) return;

    // A Canvas node belongs to RenderUIRectTool, not to ImGuizmo. "Has no transform"
    // is not the test: SceneSerializer stamps LocalTransform + GlobalTransform onto
    // every non-skinned node it loads, so a reloaded UI node would get a spurious
    // gizmo at the world origin, and one click could drive the gizmo AND a rect drag
    // (two undo entries for one gesture). A canvas root is excluded too: it has no
    // UIRect, but a loaded one carries LocalTransform.
    if (UI::IsUINode(*m_world, m_selectedEntity)) return;
    const DirectX::XMFLOAT4X4 worldMatrix = gt->matrix;

    // Frustum-cull: ImGuizmo otherwise pins handles to the screen edge for
    // off-screen entities (the window draw list clips them, but a handle pinned
    // inside the rect would still be drawn and hit-tested).
    ImVec2 gizmoCenter(0.f, 0.f);
    {
        using namespace DirectX;
        const XMMATRIX view = XMLoadFloat4x4(&m_renderView.viewMatrix);
        const XMMATRIX proj = XMLoadFloat4x4(&m_renderView.projMatrixNoJitter);
        const XMMATRIX vp   = XMMatrixMultiply(view, proj);
        const XMVECTOR worldPos = XMVectorSet(worldMatrix._41, worldMatrix._42,
                                              worldMatrix._43, 1.0f);
        const XMVECTOR clipPos  = XMVector4Transform(worldPos, vp);
        const float w = XMVectorGetW(clipPos);
        if (w <= 0.0f) return;                           // behind camera
        const float ndcX = XMVectorGetX(clipPos) / w;
        const float ndcY = XMVectorGetY(clipPos) / w;
        // 1.5x margin keeps gizmo arms visible for near-edge entities so handles don't snap off mid-drag.
        const float kMargin = 1.5f;
        if (ndcX < -kMargin || ndcX > kMargin ||
            ndcY < -kMargin || ndcY > kMargin) return;
        gizmoCenter = ImVec2(vpMin.x + (ndcX * 0.5f + 0.5f) * (vpMax.x - vpMin.x),
                             vpMin.y + (0.5f - ndcY * 0.5f) * (vpMax.y - vpMin.y));
    }

    // Freeze ImGuizmo input while hovered/dragged: float drift from TransformSystem
    // flickers hit-tests.
    if (m_selectedEntity != m_gizmoCachedEntity)
    {
        m_gizmoCachedEntity = m_selectedEntity;
        m_gizmoWorldMatrix  = worldMatrix;
    }
    else if (!ImGuizmo::IsUsing() && !ImGuizmo::IsOver())
    {
        m_gizmoWorldMatrix = worldMatrix;
    }

    float worldMut[16];
    memcpy(worldMut, &m_gizmoWorldMatrix, sizeof(worldMut));

    // Local = axes follow the object's rotation; World = axes stay world-aligned.
    // Toggled from the viewport toolbar. ImGuizmo forces LOCAL for SCALE itself.
    const ImGuizmo::MODE mode = m_gizmoMode;

    ImGuizmo::SetOrthographic(m_editorViewMode != EditorViewMode::Perspective
                              && GetPlayState() == ViewportPlayState::Stopped);
    ImGuizmo::SetRect(vpMin.x, vpMin.y, vpMax.x - vpMin.x, vpMax.y - vpMin.y);
    // THE VIEWPORT WINDOW'S OWN draw list (the gizmo layer rule, see
    // ViewportGizmoDrawList) + SetAlternativeWindow for hover resolution in
    // docked layouts.
    ImDrawList* gizmoDL = ViewportGizmoDrawList("transform", gizmoCenter);
    ImGuizmo::SetDrawlist(gizmoDL);
    ImGuizmo::SetAlternativeWindow(ImGui::GetCurrentWindow());

    // Drag snap increments (toolbar toggle; Ctrl inverts temporarily).
    // ImGuizmo snaps the cumulative delta from drag start: translate wants a
    // per-axis float3 (metres), rotate/scale read a single float (deg/factor).
    float snapVals[3] = { 0.f, 0.f, 0.f };
    const float* snap = nullptr;
    if (m_snapEnabled != ImGui::GetIO().KeyCtrl)
    {
        switch (m_gizmoOperation)
        {
        case ImGuizmo::ROTATE: snapVals[0] = m_snapRotate; break;
        case ImGuizmo::SCALE:  snapVals[0] = m_snapScale;  break;
        default:               snapVals[0] = snapVals[1] = snapVals[2] = m_snapTranslate; break;
        }
        snap = snapVals;
    }

    // ImGuizmo exposes a matrix API, but LocalTransform stores rotation as a
    // quaternion.  Keep the per-frame delta so ROTATE can be accumulated in
    // quaternion space instead of repeatedly accepting an absolute matrix
    // decomposition (whose q / -q choice is not temporally stable).
    float deltaMut[16];
    m_gizmoSubmittedThisFrame = true;   // ImGuizmo's hit-test state is now current
    // Clipped to the SCENE IMAGE, not just the Viewport window: the window's
    // clip rect also holds the viewport's toolbar row above the image.
    gizmoDL->PushClipRect(vpMin, vpMax, true);
    const bool manipulated = ImGuizmo::Manipulate(
        reinterpret_cast<const float*>(&m_renderView.viewMatrix),
        reinterpret_cast<const float*>(&m_renderView.projMatrixNoJitter),
        m_gizmoOperation, mode, worldMut, deltaMut, snap);
    gizmoDL->PopClipRect();
    if (!manipulated)
        return;

    // Only apply if the left mouse is actually held: prevents hover-only activation.
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        return;

    // worldMut is now the modified world matrix.
    XMFLOAT4X4 newWorld;
    memcpy(&newWorld, worldMut, sizeof(newWorld));
    XMFLOAT4X4 deltaWorldStore;
    memcpy(&deltaWorldStore, deltaMut, sizeof(deltaWorldStore));
    const XMMATRIX deltaWorld = XMLoadFloat4x4(&deltaWorldStore);

    // ROOTS only. A selected child of a selected parent is already carried by
    // that parent, so applying the delta to both moves it twice.
    const std::vector<Entity> roots =
        (m_selectedEntities.size() > 1) ? SelectionRoots()
                                        : std::vector<Entity>{ m_selectedEntity };
    bool primaryIsRoot = false;

    for (Entity target : roots)
    {
        if (target == m_selectedEntity)
        {
            // The primary takes ImGuizmo's ABSOLUTE result, which is the one
            // snapping was applied to.
            primaryIsRoot = true;
            ApplyGizmoToEntity(target, newWorld, deltaWorld, /*isPrimary=*/true);
            continue;
        }
        const GlobalTransform* gt = m_world->GetComponent<GlobalTransform>(target);
        if (!gt) continue;
        XMFLOAT4X4 targetWorld;
        XMStoreFloat4x4(&targetWorld,
                        XMMatrixMultiply(XMLoadFloat4x4(&gt->matrix), deltaWorld));
        ApplyGizmoToEntity(target, targetWorld, deltaWorld, /*isPrimary=*/false);
    }

    if (!primaryIsRoot)
    {
        // The primary is carried by a selected ancestor, so its own LocalTransform must
        // not change - but the gizmo still has to track it, or the handle detaches from
        // the object under the cursor. The primary moves rigidly with the ancestor, so
        // the delta is exact.
        XMStoreFloat4x4(&m_gizmoWorldMatrix,
                        XMMatrixMultiply(XMLoadFloat4x4(&m_gizmoWorldMatrix), deltaWorld));
    }
}

void EditorLayer::ApplyGizmoToEntity(Entity target,
                                     const XMFLOAT4X4& newWorld,
                                     const XMMATRIX&   deltaWorld,
                                     bool              isPrimary)
{
    if (!m_world || !m_world->IsAlive(target)) return;

    if (LocalTransform* lt = m_world->GetComponent<LocalTransform>(target))
    {
        // The space LocalTransform lives in. A socket/entity attachment resolves first
        // because FollowSystem is the last writer of GlobalTransform for those entities:
        // if the gizmo baked a Parent-relative local here, the next FollowSystem::Update
        // would compose it against the socket instead and the object would jump.
        XMMATRIX attachWorld;
        bool hasAttachSpace = Follow::ResolveAttachParentWorld(
            *m_world, static_cast<Entity>(target), attachWorld);

        if (!hasAttachSpace)
        {
            const Parent* par = m_world->GetComponent<Parent>(target);
            const GlobalTransform* pgt = (par && par->entity != NullEntity)
                ? m_world->GetComponent<GlobalTransform>(par->entity) : nullptr;
            if (pgt)
            {
                attachWorld    = XMLoadFloat4x4(&pgt->matrix);
                hasAttachSpace = true;
            }
        }

        const XMMATRIX attachInverse = hasAttachSpace
            ? XMMatrixInverse(nullptr, attachWorld)
            : XMMatrixIdentity();
        const XMMATRIX newLocal = hasAttachSpace
            ? XMMatrixMultiply(XMLoadFloat4x4(&newWorld), attachInverse)
            : XMLoadFloat4x4(&newWorld);

        XMVECTOR sVec, qVec, tVec;
        if (XMMatrixDecompose(&sVec, &qVec, &tVec, newLocal))
        {
            if (m_gizmoOperation == ImGuizmo::ROTATE)
            {
                // newWorld = oldWorld * deltaWorld.  Convert that incremental
                // rotation into LocalTransform's parent/attachment space:
                //   newLocal = oldLocal * attach * deltaWorld * inverse(attach)
                const XMMATRIX deltaLocal = hasAttachSpace
                    ? XMMatrixMultiply(
                        XMMatrixMultiply(attachWorld, deltaWorld),
                        attachInverse)
                    : deltaWorld;

                XMVECTOR deltaScale, deltaRotation, deltaTranslation;
                const bool deltaDecomposed = XMMatrixDecompose(
                    &deltaScale, &deltaRotation, &deltaTranslation, deltaLocal);
                XMFLOAT3 deltaScale3{};
                if (deltaDecomposed)
                    XMStoreFloat3(&deltaScale3, deltaScale);

                const bool deltaIsRigid =
                    deltaDecomposed &&
                    std::fabs(deltaScale3.x - 1.0f) < 1e-3f &&
                    std::fabs(deltaScale3.y - 1.0f) < 1e-3f &&
                    std::fabs(deltaScale3.z - 1.0f) < 1e-3f;

                const XMVECTOR oldRotation =
                    XMQuaternionNormalize(XMLoadFloat4(&lt->rotation));
                XMVECTOR newRotation;
                if (deltaIsRigid)
                {
                    // Compose rotation matrices only to make the row-vector
                    // multiplication order explicit: oldLocal * deltaLocal.
                    const XMMATRIX composedRotation =
                        XMMatrixMultiply(
                            XMMatrixRotationQuaternion(oldRotation),
                            XMMatrixRotationQuaternion(
                                XMQuaternionNormalize(deltaRotation)));
                    newRotation = XMQuaternionRotationMatrix(composedRotation);
                }
                else
                {
                    // A non-uniformly scaled parent can turn a world-space
                    // rotation delta into shear, which a TRS LocalTransform
                    // cannot represent exactly.  Preserve the existing
                    // best-fit absolute decomposition for that case.
                    newRotation = qVec;
                }

                newRotation = XMQuaternionNormalize(newRotation);
                // q and -q encode the same orientation.  Keep the closest
                // hemisphere so downstream interpolation/editor displays do
                // not observe a 360-degree sign flip during a continuous drag.
                if (XMVectorGetX(XMVector4Dot(oldRotation, newRotation)) < 0.0f)
                    newRotation = XMVectorNegate(newRotation);
                XMStoreFloat4(&lt->rotation, newRotation);
            }
            else if (m_gizmoOperation == ImGuizmo::TRANSLATE)
            {
                XMStoreFloat3(&lt->translation, tVec);
            }
            else if (m_gizmoOperation == ImGuizmo::SCALE)
            {
                XMStoreFloat3(&lt->scale, sVec);
            }
            else
            {
                // Any other (combined) operation takes the full TRS, still with a normalized,
                // hemisphere-continuous quaternion.
                const XMVECTOR oldRotation =
                    XMQuaternionNormalize(XMLoadFloat4(&lt->rotation));
                qVec = XMQuaternionNormalize(qVec);
                if (XMVectorGetX(XMVector4Dot(oldRotation, qVec)) < 0.0f)
                    qVec = XMVectorNegate(qVec);
                XMStoreFloat3(&lt->translation, tVec);
                XMStoreFloat4(&lt->rotation, qVec);
                XMStoreFloat3(&lt->scale, sVec);
            }
        }

        // Recompute matching GlobalTransform so m_gizmoWorldMatrix stays in sync (IsOver() won't oscillate).
        const XMMATRIX ltMat = lt->ToMatrix();
        const XMMATRIX expectedGT = hasAttachSpace
            ? XMMatrixMultiply(ltMat, attachWorld)
            : ltMat;

        // Only the primary anchors the gizmo; the others just need their own
        // GlobalTransform kept coherent until the transform system runs.
        if (isPrimary) XMStoreFloat4x4(&m_gizmoWorldMatrix, expectedGT);
        if (GlobalTransform* gt = m_world->GetComponent<GlobalTransform>(target))
            XMStoreFloat4x4(&gt->matrix, expectedGT);
    }
}
