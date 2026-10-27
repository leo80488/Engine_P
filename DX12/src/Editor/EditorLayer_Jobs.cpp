// EditorLayer_Jobs.cpp - the Jobs panel (Window > Analysis > Jobs): every job the editor tracks
// through Editor::EditorServices().jobs - imports, saves, UI test runs, hot reloads, bakes - with its
// state, progress, age and error; and the notice a job posts when it finishes (TickJobNotices, at the
// frame boundary, open or not).

#include "Editor/EditorLayer.h"

#include "Editor/EditorNotify.h"
#include "Editor/EditorServices.h"
#include "Harness/ControlChannelJobs.h"

#include "imgui/imgui.h"

#include <algorithm>
#include <cstdio>
#include <string>

void EditorLayer::TickJobNotices()
{
    Editor::IJobService* jobs = Editor::EditorServices().jobs;
    if (!jobs) return;
#if defined(WITH_HARNESS) && WITH_HARNESS
    // Re-read the table only when its summary moved (a job created or one fewer active).
    const Harness::Jobs::Summary sum = Harness::Jobs::GetSummary();
    if (sum.active == m_jobsSeenActive && sum.lastId == m_jobsSeenLastId) return;
    m_jobsSeenActive = sum.active;
    m_jobsSeenLastId = sum.lastId;
#endif
    for (const Editor::JobInfo& j : jobs->Jobs())
    {
        const bool terminal = j.state == "succeeded" || j.state == "failed" || j.state == "cancelled";
        auto it = m_jobsSeenState.find(j.id);
        const bool known = it != m_jobsSeenState.end();
        if (known && it->second == j.state) continue;
        m_jobsSeenState[j.id] = j.state;
        // A job already finished when it was first seen (before the panel's watch began) posts nothing,
        // and neither do the hot-reload receipts every saved shader files.
        if (!terminal || !known || j.kind.rfind("hotreload.", 0) == 0) continue;
        std::string text = j.kind + (j.detail.empty() ? "" : ": " + j.detail);
        if (j.state == "succeeded")
            Editor::Notices::Get().Push(Editor::NoticeLevel::Success, text + " - done",
                                        [this] { m_showJobs = true; });
        else
            Editor::Notices::Get().Push(Editor::NoticeLevel::Error, text + " - " + j.state + (j.error.empty() ? "" : " (" + j.error + ")"),
                                        [this] { m_showJobs = true; });
    }
    // Forget receipts the table no longer holds.
    if (m_jobsSeenState.size() > 512) m_jobsSeenState.clear();
}

void EditorLayer::RenderJobsPanel()
{
    ImGui::SetNextWindowSize(ImVec2(720.f, 360.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Jobs", &m_showJobs))
    {
        ImGui::End();
        return;
    }
    Editor::IJobService* service = Editor::EditorServices().jobs;
    std::vector<Editor::JobInfo> jobs = service ? service->Jobs() : std::vector<Editor::JobInfo>();
    static char s_filter[64] = {};
    static bool s_activeOnly = false;
    ImGui::SetNextItemWidth(220.f);
    ImGui::InputTextWithHint("##jobsFilter", "Filter kind / detail", s_filter, sizeof(s_filter));
    ImGui::SameLine();
    ImGui::Checkbox("Active only", &s_activeOnly);
    ImGui::SameLine();
    ImGui::TextDisabled("%zu job(s); newest first", jobs.size());
    if (!service) ImGui::TextDisabled("This build tracks no jobs (no control channel).");

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##jobs", 5, flags))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Id", ImGuiTableColumnFlags_WidthFixed, 48.f);
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthStretch, 0.18f);
        ImGui::TableSetupColumn("Detail", ImGuiTableColumnFlags_WidthStretch, 0.46f);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch, 0.16f);
        ImGui::TableSetupColumn("Result", ImGuiTableColumnFlags_WidthStretch, 0.2f);
        ImGui::TableHeadersRow();
        const std::string filter = s_filter;
        auto contains = [&](const std::string& hay) {
            if (filter.empty()) return true;
            auto lower = [](std::string v) {
                std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return v;
            };
            return lower(hay).find(lower(filter)) != std::string::npos;
        };
        for (const Editor::JobInfo& j : jobs)
        {
            const bool active = j.state == "queued" || j.state == "running";
            if (s_activeOnly && !active) continue;
            if (!contains(j.kind) && !contains(j.detail)) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(j.id));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(j.kind.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(j.detail.c_str());
            if (!j.detail.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", j.detail.c_str());
            ImGui::TableNextColumn();
            if (active && j.progress >= 0.f)
                ImGui::ProgressBar(j.progress, ImVec2(-1.f, 0.f), j.state.c_str());
            else
            {
                const ImVec4 col = j.state == "failed" ? ImVec4(1.f, 0.45f, 0.4f, 1.f)
                                 : j.state == "succeeded" ? ImVec4(0.5f, 0.85f, 0.5f, 1.f)
                                                          : ImVec4(0.8f, 0.8f, 0.8f, 1.f);
                ImGui::TextColored(col, "%s", j.state.c_str());
            }
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(j.id));
            if (!j.error.empty())
            {
                ImGui::TextUnformatted(j.error.c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n(right-click to copy)", j.error.c_str());
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::SetClipboardText(j.error.c_str());
            }
            if (j.cancellable && active && service && ImGui::SmallButton("Cancel")) service->Cancel(j.id);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}
