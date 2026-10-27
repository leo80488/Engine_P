// GameFlowWindow - read-only view of the top-level GameFlow state machine: the
// current state, the hierarchy, which SystemSets that makes active, and every
// transition (including the REJECTED ones) that led here. Owned by EditorLayer
// (toggle: View > Panels > Game Flow); its own TU.
//
// Everything except the Force Switch row is a pure pull off
// GameFlowStateMachine::FillSnapshot / Diagnostics / History - the same plain value
// types the harness dump and the control channel read, so the panel can never
// disagree with a log. Force Switch is the one write, and it is fenced hard (see
// the gate below): a request made outside Play would sit in the pending slot
// indefinitely, because GameFlowResolveSystem is RunMask::Play and only a Play
// frame drains it.

#include "Editor/EditorLayer.h"
#include "ECS/ISystem.h"
#include "ECS/SystemRegistry.h"
#include "Gameplay/GameFlowState.h"
#include "Scripting/ScriptSystem.h"

#include "imgui/imgui.h"

#include <cstdio>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    const ImVec4 kOk    { 0.45f, 0.85f, 0.45f, 1.f };
    const ImVec4 kBad   { 0.95f, 0.35f, 0.35f, 1.f };
    const ImVec4 kGated { 0.95f, 0.80f, 0.35f, 1.f };

    constexpr ImGuiTableFlags kTableFlags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings;

    using Flow     = Gameplay::GameFlowStateMachine;
    using StateRow = Flow::FlowStateRow;
    using Severity = Flow::FlowDiagnostic::Severity;

    // A whole-panel condition (pass-through, invalid definitions) must not read
    // as one row's status, so it gets a full-width tinted band above the tables
    // rather than another coloured line of text.
    void WarningBar(const ImVec4& tint, const char* text)
    {
        const ImVec2 pos    = ImGui::GetCursorScreenPos();
        const float  width  = ImGui::GetContentRegionAvail().x;
        const float  height = ImGui::GetTextLineHeight() + 8.f;
        ImGui::GetWindowDrawList()->AddRectFilled(
            pos, ImVec2(pos.x + width, pos.y + height),
            ImGui::GetColorU32(ImVec4(tint.x, tint.y, tint.z, 0.18f)), 3.f);
        ImGui::Dummy(ImVec2(0.f, 4.f));
        ImGui::Indent(6.f);
        ImGui::TextColored(tint, "%s", text);
        ImGui::Unindent(6.f);
        ImGui::Dummy(ImVec2(0.f, 4.f));
    }

    std::string Join(const std::vector<std::string>& items)
    {
        std::string out;
        for (const std::string& item : items)
        {
            if (!out.empty()) out += ", ";
            out += item;
        }
        return out;
    }

    // Rows arrive sorted by name, which reads as scrambled once depth indentation is
    // applied: a child can precede its parent. Re-thread them parent-first so the
    // indent draws the tree. A row whose parent is not a defined state is treated as a
    // root: it is already flagged invalid, and dropping it would hide the very
    // definition the user came here to fix.
    std::vector<int> ThreadHierarchy(const std::vector<StateRow>& rows)
    {
        const int count = static_cast<int>(rows.size());
        std::unordered_map<std::string, int> byName;
        byName.reserve(rows.size());
        for (int i = 0; i < count; ++i) byName[rows[i].name] = i;

        std::vector<std::vector<int>> children(rows.size());
        std::vector<int> roots;
        for (int i = 0; i < count; ++i)
        {
            const auto parent = rows[i].parent.empty() ? byName.end()
                                                       : byName.find(rows[i].parent);
            if (parent == byName.end()) roots.push_back(i);
            else                        children[parent->second].push_back(i);
        }

        // `visited` is not belt-and-braces. Validation QUARANTINES a definition cycle, it
        // does not delete it, so a cycle reaches this panel intact - and an unguarded walk
        // would hang the editor instead of showing it.
        std::vector<int> order;
        std::vector<char> visited(rows.size(), 0);
        std::vector<int> stack(roots.rbegin(), roots.rend());
        order.reserve(rows.size());
        while (!stack.empty())
        {
            const int index = stack.back();
            stack.pop_back();
            if (visited[index]) continue;
            visited[index] = 1;
            order.push_back(index);
            for (auto it = children[index].rbegin(); it != children[index].rend(); ++it)
                stack.push_back(*it);
        }
        for (int i = 0; i < count; ++i)
            if (!visited[i]) order.push_back(i);   // unreachable: inside a cycle
        return order;
    }
}

void EditorLayer::RenderGameFlowWindow()
{
    ImGui::SetNextWindowSize(ImVec2(820.f, 620.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Game Flow", &m_showGameFlow))
    {
        ImGui::End();
        return;
    }
    if (!m_gameFlow)
    {
        ImGui::TextDisabled("No GameFlowStateMachine attached.");
        ImGui::End();
        return;
    }

    Flow::FlowSnapshot snap;
    m_gameFlow->FillSnapshot(snap);
    const std::vector<Flow::FlowDiagnostic>& diagnostics = m_gameFlow->Diagnostics();

    // ---- Header ----
    ImGui::TextUnformatted("Current:");
    ImGui::SameLine();
    if (snap.current.empty()) ImGui::TextDisabled("(none)");
    else                      ImGui::TextColored(kOk, "%s", snap.current.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("->");
    ImGui::SameLine();
    if (snap.pending.empty()) ImGui::TextDisabled("(no pending)");
    else                      ImGui::TextColored(kGated, "%s", snap.pending.c_str());
    if (snap.inTransition)
    {
        ImGui::SameLine();
        ImGui::TextColored(kGated, "[IN TRANSITION]");
    }

    if (snap.passThrough)
        WarningBar(kGated, "no flow started: every SystemSet gate is open "
                           "(IsSetActive answers 'active' even for a name nothing declared)");
    if (!snap.definitionsValid)
        WarningBar(kBad, "definitions INVALID - quarantined states are red below; "
                         "switching to one is rejected as 'invalid'");
    ImGui::Separator();

    // ---- Current path ----
    ImGui::TextUnformatted("Path:");
    ImGui::SameLine();
    if (snap.currentPath.empty())
    {
        ImGui::TextDisabled("(empty)");
    }
    else
    {
        for (size_t i = 0; i < snap.currentPath.size(); ++i)
        {
            if (i > 0) { ImGui::SameLine(); ImGui::TextDisabled("/"); ImGui::SameLine(); }
            // Only the leaf is Current(); the ancestors are what IsInState()
            // answers for, which is the whole reason this breadcrumb exists.
            const bool leaf = (i + 1 == snap.currentPath.size());
            if (leaf) ImGui::TextColored(kOk, "%s", snap.currentPath[i].c_str());
            else      ImGui::TextUnformatted(snap.currentPath[i].c_str());
        }
    }

    // ---- Force switch ----
    // Fenced to "Playing, with an empty slot" on purpose. TryRequestSwitch is
    // first-write-wins, and only GameFlowResolveSystem (TickPhase::Input,
    // RunMask::Play) drains the slot - so a click while Stopped or Paused parks a
    // request nothing consumes, and the NEXT Play's boot switch then loses
    // first-write-wins to it, permanently. Clicking mid-transition or on top of an
    // existing pending request is the same trap.
    static std::string s_target;
    static std::string s_lastResult;
    static bool        s_lastResultOk = true;

    const bool playing   = (GetPlayState() == ViewportPlayState::Playing);
    const bool slotFree  = snap.pending.empty() && !snap.inTransition;
    const bool canSwitch = playing && slotFree;
    const char* blockedWhy =
        !playing  ? "Play is not running. GameFlowResolveSystem is RunMask::Play, so a request\n"
                    "made now would occupy the pending slot until the next Play frame - and the\n"
                    "boot switch would lose first-write-wins to it, permanently."
      : !slotFree ? "A request already owns this frame (first-write-wins). Wait one frame."
                  : "";

    ImGui::BeginDisabled(!canSwitch);
    ImGui::SetNextItemWidth(240.f);
    if (ImGui::BeginCombo("##flowtarget",
                          s_target.empty() ? "(pick a state)" : s_target.c_str()))
    {
        for (const StateRow& row : snap.states)
        {
            if (!row.valid) ImGui::PushStyleColor(ImGuiCol_Text, kBad);
            if (ImGui::Selectable(row.name.c_str(), row.name == s_target))
                s_target = row.name;
            if (!row.valid) ImGui::PopStyleColor();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(s_target.empty());
    if (ImGui::Button("Switch"))
    {
        const Gameplay::SwitchResult result = m_gameFlow->TryRequestSwitch(s_target);
        s_lastResultOk = (result == Gameplay::SwitchResult::Accepted);
        s_lastResult   = s_target + ": " + Gameplay::ToString(result);
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    // AllowWhenDisabled: explaining the fence IS the point of this tooltip.
    if (!canSwitch && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", blockedWhy);
    if (!s_lastResult.empty())
    {
        ImGui::SameLine();
        ImGui::TextColored(s_lastResultOk ? kOk : kBad, "%s", s_lastResult.c_str());
    }
    ImGui::Separator();

    char header[96];

    // ---- State graph ----
    snprintf(header, sizeof(header), "State Graph (%zu)###stategraph", snap.states.size());
    if (ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (snap.states.empty())
        {
            ImGui::TextDisabled("none defined (no DefineState call ran)");
        }
        else if (ImGui::BeginTable("stategraph", 4, kTableFlags))
        {
            ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Parent");
            ImGui::TableSetupColumn("Sets");
            ImGui::TableSetupColumn("Suspends");
            ImGui::TableHeadersRow();
            for (const int index : ThreadHierarchy(snap.states))
            {
                const StateRow& row = snap.states[static_cast<size_t>(index)];
                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                const float indent = static_cast<float>(row.depth) * 14.f;
                if (indent > 0.f) ImGui::Indent(indent);
                if (!row.valid)
                    ImGui::TextColored(kBad, "%s  (INVALID)", row.name.c_str());
                else if (row.isCurrent)
                    ImGui::TextColored(kOk, "%s  <- current", row.name.c_str());
                else if (row.onCurrentPath)
                    ImGui::TextColored(kOk, "%s", row.name.c_str());
                else
                    ImGui::TextUnformatted(row.name.c_str());
                // Hung off the name cell rather than the row: the diagnostic
                // explains why THIS definition was quarantined.
                if (!row.valid && ImGui::IsItemHovered())
                {
                    std::string why;
                    for (const Flow::FlowDiagnostic& d : diagnostics)
                    {
                        if (d.state != row.name) continue;
                        if (!why.empty()) why += '\n';
                        why += d.message;
                    }
                    ImGui::SetTooltip("%s", why.empty() ? "quarantined by validation"
                                                        : why.c_str());
                }
                if (indent > 0.f) ImGui::Unindent(indent);

                ImGui::TableNextColumn();
                if (row.parent.empty()) ImGui::TextDisabled("-");
                else                    ImGui::TextUnformatted(row.parent.c_str());

                ImGui::TableNextColumn();
                const std::string sets = Join(row.sets);
                if (sets.empty()) ImGui::TextDisabled("-");
                else              ImGui::TextUnformatted(sets.c_str());

                ImGui::TableNextColumn();
                const std::string suspends = Join(row.suspends);
                if (suspends.empty()) ImGui::TextDisabled("-");
                else                  ImGui::TextColored(kGated, "%s", suspends.c_str());
            }
            ImGui::EndTable();
        }
    }

    // ---- System sets ----
    snprintf(header, sizeof(header), "System Sets (%zu)###systemsets", snap.sets.size());
    if (ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (snap.sets.empty())
        {
            ImGui::TextDisabled("none declared");
        }
        else
        {
            // Membership comes from two registries that never see each other: C++ systems
            // via the scheduler, Lua Systems via ScriptSystem. Both halves are listed, or a
            // set looks empty when only the other one filled it. Names -> ids come from the
            // flow's own table rather than re-hashing, so an alias resolves the same way it
            // does at gate time.
            std::unordered_map<std::string, SystemSetId> idByName;
            idByName.reserve(m_gameFlow->KnownSystemSets().size());
            for (const auto& [id, name] : m_gameFlow->KnownSystemSets())
                idByName[name] = id;

            std::vector<std::pair<std::string, std::string>> luaAssignments;
            if (m_scriptSys) luaAssignments = m_scriptSys->GetSystemSetAssignments();

            if (ImGui::BeginTable("systemsets", 3, kTableFlags))
            {
                ImGui::TableSetupColumn("Set");
                ImGui::TableSetupColumn("State");
                ImGui::TableSetupColumn("Members", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                for (const Flow::FlowSetRow& setRow : snap.sets)
                {
                    std::vector<const ISystem*> cppSystems;
                    const auto id = idByName.find(setRow.name);
                    if (m_systemRegistry && id != idByName.end())
                        cppSystems = m_systemRegistry->GetSystemsInSet(id->second);
                    int luaCount = 0;
                    for (const auto& assignment : luaAssignments)
                        if (assignment.second == setRow.name) ++luaCount;

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    if (setRow.referenced)
                    {
                        ImGui::TextUnformatted(setRow.name.c_str());
                    }
                    else
                    {
                        // Declared, but no state's sets/suspends names it, so its gate can never flip:
                        // nearly always a typo on one side.
                        ImGui::TextColored(kGated, "%s  (unreferenced)", setRow.name.c_str());
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("declared but named by no state - "
                                              "this gate never changes");
                    }

                    ImGui::TableNextColumn();
                    if (snap.passThrough)
                        ImGui::TextColored(kGated, "open (pass-through)");
                    else if (setRow.active)
                        ImGui::TextColored(kOk, "active");
                    else
                        ImGui::TextColored(kGated, "inactive");

                    ImGui::TableNextColumn();
                    if (cppSystems.empty() && luaCount == 0)
                    {
                        ImGui::TextColored(kGated, "no members (this set gates nothing)");
                    }
                    else
                    {
                        for (const ISystem* system : cppSystems)
                            ImGui::Text("C++  %s", system->GetName());
                        for (const auto& assignment : luaAssignments)
                            if (assignment.second == setRow.name)
                                ImGui::Text("Lua  %s", assignment.first.c_str());
                    }
                }
                ImGui::EndTable();
            }
            if (!m_systemRegistry)
                ImGui::TextDisabled("No SystemRegistry attached - C++ members are not listed.");
        }
    }

    // ---- Diagnostics ----
    snprintf(header, sizeof(header), "Diagnostics (%zu)###flowdiagnostics",
             diagnostics.size());
    if (ImGui::CollapsingHeader(header,
                                diagnostics.empty() ? 0 : ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (diagnostics.empty())
            ImGui::TextDisabled("none");
        for (const Flow::FlowDiagnostic& d : diagnostics)
        {
            const bool error = (d.severity == Severity::Error);
            ImGui::TextColored(error ? kBad : kGated, "%s", error ? "ERROR" : "warn");
            ImGui::SameLine();
            if (d.state.empty()) ImGui::Text("%s", d.message.c_str());
            else                 ImGui::Text("[%s] %s", d.state.c_str(), d.message.c_str());
        }
    }

    // ---- Transition history ----
    const std::vector<Flow::FlowTransitionRecord> history = m_gameFlow->History();
    snprintf(header, sizeof(header), "History (%zu/%zu)###flowhistory",
             history.size(), static_cast<size_t>(Flow::kHistoryCapacity));
    if (ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (history.empty())
        {
            ImGui::TextDisabled("no transitions yet");
        }
        else if (ImGui::BeginTable("flowhistory", 5, kTableFlags))
        {
            ImGui::TableSetupColumn("Frame", ImGuiTableColumnFlags_WidthFixed, 64.f);
            ImGui::TableSetupColumn("From");
            ImGui::TableSetupColumn("To");
            ImGui::TableSetupColumn("Result", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, 56.f);
            ImGui::TableHeadersRow();
            // History() hands back oldest -> newest (ring order). Walked
            // backwards: the record being debugged is the one that just landed.
            for (auto it = history.rbegin(); it != history.rend(); ++it)
            {
                const Flow::FlowTransitionRecord& record = *it;
                const bool accepted = (record.kind == Gameplay::SwitchResult::Accepted);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%llu", static_cast<unsigned long long>(record.frame));
                ImGui::TableNextColumn();
                if (record.from.empty()) ImGui::TextDisabled("-");
                else                     ImGui::TextUnformatted(record.from.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(record.to.c_str());
                ImGui::TableNextColumn();
                if (record.repeats > 0)
                {
                    // Folded consecutive identical rejections: the count is the
                    // signal (a per-frame retry loop), not the single entry.
                    ImGui::TextColored(accepted ? kOk : kBad, "%s  x%u",
                                       Gameplay::ToString(record.kind),
                                       record.repeats + 1u);
                }
                else
                {
                    ImGui::TextColored(accepted ? kOk : kBad, "%s",
                                       Gameplay::ToString(record.kind));
                }
                ImGui::TableNextColumn();
                if (accepted) ImGui::Text("%.2f", record.elapsedMs);
                else          ImGui::TextDisabled("-");   // a rejection did no work
            }
            ImGui::EndTable();
        }
    }

    ImGui::End();
}
