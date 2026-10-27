#include "Editor/EditorNotify.h"

#include "System/CommandLine.h"
#include "System/Log.h"

#include "imgui/imgui.h"

#include <cstdio>

namespace Editor
{
namespace
{
    ImVec4 LevelColor(NoticeLevel level)
    {
        switch (level)
        {
        case NoticeLevel::Success: return ImVec4(0.45f, 0.85f, 0.45f, 1.f);
        case NoticeLevel::Warning: return ImVec4(1.f, 0.78f, 0.3f, 1.f);
        case NoticeLevel::Error:   return ImVec4(1.f, 0.42f, 0.38f, 1.f);
        default:                   return ImVec4(0.6f, 0.8f, 1.f, 1.f);
        }
    }

    const char* LevelName(NoticeLevel level)
    {
        switch (level)
        {
        case NoticeLevel::Success: return "done";
        case NoticeLevel::Warning: return "warning";
        case NoticeLevel::Error:   return "error";
        default:                   return "info";
        }
    }
}

Notices& Notices::Get()
{
    static Notices s_notices;
    return s_notices;
}

void Notices::Push(NoticeLevel level, std::string text, std::function<void()> action)
{
    if (level == NoticeLevel::Error) LOG_ERROR("[NOTICE] %s: %s", LevelName(level), text.c_str());
    else if (level == NoticeLevel::Warning) LOG_WARNING("[NOTICE] %s: %s", LevelName(level), text.c_str());
    else LOG_INFO("[NOTICE] %s: %s", LevelName(level), text.c_str());
    Notice n;
    n.id = m_nextId++;
    n.level = level;
    n.text = std::move(text);
    n.action = std::move(action);
    n.postedAt = ImGui::GetCurrentContext() ? ImGui::GetTime() : 0.0;
    m_recent.push_back(std::move(n));
    while (m_recent.size() > kKept) m_recent.pop_front();
}

void Notices::Draw()
{
    static const bool s_unattended = Sys::CommandLine::Get().Has("-unattended");
    if (s_unattended || !ImGui::GetCurrentContext()) return;
    const double now = ImGui::GetTime();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float pad = 12.f;
    float y = vp->WorkPos.y + vp->WorkSize.y - pad;
    int shown = 0;
    for (auto it = m_recent.rbegin(); it != m_recent.rend() && shown < 5; ++it)
    {
        Notice& n = *it;
        const double age = now - n.postedAt;
        if (n.dismissed || age > kShowSeconds) continue;
        const float alpha = age > kShowSeconds - 0.6 ? static_cast<float>((kShowSeconds - age) / 0.6) : 1.f;
        // One window per screen slot, not per notice: ImGui keeps every window it has seen.
        char name[32];
        std::snprintf(name, sizeof(name), "##noticeSlot%d", shown);
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - pad, y), ImGuiCond_Always, ImVec2(1.f, 1.f));
        ImGui::SetNextWindowBgAlpha(0.92f * alpha);
        ImGui::SetNextWindowViewport(vp->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking;
        if (ImGui::Begin(name, nullptr, flags))
        {
            ImGui::TextColored(LevelColor(n.level), "%s", LevelName(n.level));
            ImGui::SameLine();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.f);
            ImGui::TextUnformatted(n.text.c_str());
            ImGui::PopTextWrapPos();
            if (n.action) ImGui::TextDisabled("Click to show");
            if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                if (n.action) n.action();
                n.dismissed = true;
            }
            y -= ImGui::GetWindowHeight() + 6.f;
        }
        ImGui::End();
        ImGui::PopStyleVar();
        ++shown;
    }
}
}
