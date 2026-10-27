#include "Editor/FocusTracker.h"

#include "imgui/imgui.h"

Editor::FocusTracker& Editor::FocusTracker::Get()
{
    static FocusTracker s_tracker;
    return s_tracker;
}

void Editor::FocusTracker::Note(const char* id, bool visible)
{
    if (!id) return;
    WindowFocus& w = m_windows[id];
    w.visible = visible;
    // RootAndChildWindows: the viewport and Game View draw their pictures in the window itself, but a
    // toolbar combo or a child region must not read as "focus moved elsewhere".
    w.focused = visible && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    w.hovered = visible && ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
    w.frame   = static_cast<std::uint64_t>(ImGui::GetFrameCount());
}

Editor::WindowFocus Editor::FocusTracker::State(const char* id) const
{
    if (!id) return {};
    const auto it = m_windows.find(id);
    if (it == m_windows.end()) return {};
    // Read either during the frame that drew it or at the top of the next one (App's input gate runs
    // before that frame's UI): anything older belongs to a window that stopped drawing.
    const std::uint64_t now = static_cast<std::uint64_t>(ImGui::GetFrameCount());
    if (it->second.frame + 1 < now) return {};
    return it->second;
}

void Editor::FocusTracker::Clear()
{
    m_windows.clear();
    m_capture.clear();
}
