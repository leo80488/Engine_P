// ScriptRegistryWindow - read-only view of everything the ScriptSystem has
// registered, by category: boot scripts (global / service / system / ui,
// manifest or folder scan), the scene-script binding, and Logic templates
// with live instance counts. Owned by EditorLayer (View -> Panels -> Script
// Registry); compiled separately to keep EditorLayer.cpp manageable.
//
// Deliberately read-only: registration is boot configuration
// (asset/scripts/script_manifest.json). The panel shows what booted, in what
// order, and whether it ticks, without letting a stray click change any of it.

#include "Editor/EditorLayer.h"
#include "Scripting/ScriptSystem.h"

#include "imgui/imgui.h"

namespace
{
    const ImVec4 kOk    { 0.45f, 0.85f, 0.45f, 1.f };
    const ImVec4 kBad   { 0.95f, 0.35f, 0.35f, 1.f };
    const ImVec4 kGated { 0.95f, 0.80f, 0.35f, 1.f };

    constexpr ImGuiTableFlags kTableFlags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings;

    void YesCell(bool v)
    {
        if (v) ImGui::TextUnformatted("yes");
        else   ImGui::TextDisabled("-");
    }

    // Global scripts are stored as full paths; show the stem up front like
    // the other categories do.
    const char* StemOf(const std::string& path)
    {
        const size_t slash = path.rfind('/');
        return path.c_str() + (slash == std::string::npos ? 0 : slash + 1);
    }
}

void EditorLayer::RenderScriptRegistryWindow()
{
    ImGui::SetNextWindowSize(ImVec2(780.f, 560.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Script Registry", &m_showScriptRegistry))
    {
        ImGui::End();
        return;
    }
    if (!m_scriptSys)
    {
        ImGui::TextDisabled("No ScriptSystem attached.");
        ImGui::End();
        return;
    }

    ScriptSystem::ScriptRegistrySnapshot snap;
    m_scriptSys->FillRegistrySnapshot(snap);

    ImGui::Text("Boot source: %s",
                snap.bootedFromManifest ? "asset/scripts/script_manifest.json"
                                        : "legacy folder scan (directory implies kind)");
    if (GetPlayState() == ViewportPlayState::Stopped)
        ImGui::TextDisabled("Edit mode — Systems, Logic instances and the scene script tick in Play only.");
    ImGui::Separator();

    // ---- Systems (tick order) ----
    char header[96];
    snprintf(header, sizeof(header), "Systems (%zu)###systems", snap.systems.size());
    if (ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (snap.systems.empty())
            ImGui::TextDisabled("none");
        else if (ImGui::BeginTable("systems", 9, kTableFlags))
        {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Order", ImGuiTableColumnFlags_WidthFixed, 44.f);
            ImGui::TableSetupColumn("Scope");
            ImGui::TableSetupColumn("Set");
            ImGui::TableSetupColumn("Init");
            ImGui::TableSetupColumn("OnUpdate");
            ImGui::TableSetupColumn("OnShutdown");
            ImGui::TableSetupColumn("State");
            ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            for (const auto& s : snap.systems)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(s.name.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%d", s.order);
                ImGui::TableNextColumn();
                if (s.sceneScoped) ImGui::TextUnformatted("scene");
                else               ImGui::TextDisabled("engine");
                ImGui::TableNextColumn();
                if (s.systemSet.empty()) ImGui::TextDisabled("-");
                else                     ImGui::TextUnformatted(s.systemSet.c_str());
                ImGui::TableNextColumn(); YesCell(s.initCalled);
                ImGui::TableNextColumn(); YesCell(s.hasUpdate);
                ImGui::TableNextColumn(); YesCell(s.hasShutdown);
                ImGui::TableNextColumn();
                if (!s.loaded)
                    ImGui::TextColored(kBad, "LOAD FAILED");
                else if (!s.active)
                    ImGui::TextDisabled("not in this scene");   // scene scope, unlisted
                else if (!s.setActive)
                    ImGui::TextColored(kGated, "gated");   // set inactive: silently skipped
                else
                    ImGui::TextColored(kOk, "active");
                ImGui::TableNextColumn(); ImGui::TextUnformatted(s.path.c_str());
                if (!s.lastError.empty() && ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", s.lastError.c_str());
            }
            ImGui::EndTable();
        }
    }

    // ---- Services / UI scripts (name order) ----
    auto namedTable = [](const char* id,
                         const std::vector<ScriptSystem::ScriptRegistrySnapshot::NamedRow>& rows)
    {
        if (rows.empty()) { ImGui::TextDisabled("none"); return; }
        if (!ImGui::BeginTable(id, 3, kTableFlags)) return;
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("State");
        ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto& r : rows)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.name.c_str());
            ImGui::TableNextColumn();
            if (r.loaded) ImGui::TextColored(kOk, "loaded");
            else          ImGui::TextColored(kBad, "LOAD FAILED");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.path.c_str());
        }
        ImGui::EndTable();
    };
    snprintf(header, sizeof(header), "Services (%zu)###services", snap.services.size());
    if (ImGui::CollapsingHeader(header))
        namedTable("services", snap.services);
    snprintf(header, sizeof(header), "UI Scripts (%zu)###uiscripts", snap.uiScripts.size());
    if (ImGui::CollapsingHeader(header))
        namedTable("uiscripts", snap.uiScripts);

    // ---- Globals (load order matters: later files may override) ----
    snprintf(header, sizeof(header), "Global Scripts (%zu)###globals", snap.globals.size());
    if (ImGui::CollapsingHeader(header))
    {
        if (snap.globals.empty())
            ImGui::TextDisabled("none");
        else if (ImGui::BeginTable("globals", 3, kTableFlags))
        {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 24.f);
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            int idx = 0;
            for (const std::string& g : snap.globals)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%d", ++idx);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(StemOf(g));
                ImGui::TableNextColumn(); ImGui::TextUnformatted(g.c_str());
            }
            ImGui::EndTable();
        }
    }

    // ---- Disabled manifest entries ----
    if (!snap.disabled.empty())
    {
        snprintf(header, sizeof(header), "Disabled (%zu)###disabled", snap.disabled.size());
        if (ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen))
            for (const std::string& d : snap.disabled)
                ImGui::TextDisabled("%s  (enabled: false in the manifest)", d.c_str());
    }

    // ---- Scene script ----
    if (ImGui::CollapsingHeader("Scene Script###scenescript", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const auto& ss = snap.sceneScript;
        if (ss.path.empty() && !ss.pending)
            ImGui::TextDisabled("none bound (scene has no script)");
        if (!ss.path.empty())
        {
            ImGui::Text("%s", ss.path.c_str());
            ImGui::SameLine();
            if (ss.entered) ImGui::TextColored(kOk, "(entered '%s')", ss.sceneName.c_str());
            else            ImGui::TextDisabled("(enter pending — fires on next Play update)");
            ImGui::TextDisabled("OnSceneUpdate: %s   OnSceneExit: %s",
                                ss.hasUpdate ? "yes" : "-", ss.hasExit ? "yes" : "-");
        }
        if (ss.pending)
            ImGui::TextColored(kGated, "pending swap -> %s ('%s')",
                               ss.pendingPath.empty() ? "(none)" : ss.pendingPath.c_str(),
                               ss.pendingSceneName.c_str());
        auto joined = [](const std::vector<std::string>& names)
        {
            std::string out;
            for (const std::string& n : names) out += (out.empty() ? "" : ", ") + n;
            return out;
        };
        if (!ss.systems.empty())
            ImGui::Text("scene systems: %s", joined(ss.systems).c_str());
        if (ss.pending && !ss.pendingSystems.empty())
            ImGui::TextColored(kGated, "pending scene systems: %s", joined(ss.pendingSystems).c_str());
    }

    // ---- What scripts hold (released with their owner) ----
    snprintf(header, sizeof(header), "Owned Resources (%zu owners)###owners", snap.owners.size());
    if (ImGui::CollapsingHeader(header))
    {
        if (snap.owners.empty())
            ImGui::TextDisabled("nothing held");
        else if (ImGui::BeginTable("owners", 5, kTableFlags))
        {
            ImGui::TableSetupColumn("Owner", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Subs");
            ImGui::TableSetupColumn("Timers");
            ImGui::TableSetupColumn("Time");
            ImGui::TableSetupColumn("Contexts");
            ImGui::TableHeadersRow();
            for (const auto& o : snap.owners)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(o.label.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%d", o.subs);
                ImGui::TableNextColumn(); ImGui::Text("%d", o.timers);
                ImGui::TableNextColumn(); ImGui::Text("%d", o.timeRequests);
                ImGui::TableNextColumn(); ImGui::Text("%d", o.contexts);
            }
            ImGui::EndTable();
        }
    }

    // ---- Logic templates (lazy-loaded, never boot-scanned) ----
    snprintf(header, sizeof(header), "Logic Templates (%zu)###logictemplates",
             snap.logicTemplates.size());
    if (ImGui::CollapsingHeader(header))
    {
        if (snap.logicTemplates.empty())
            ImGui::TextDisabled("none loaded yet (templates load on first ScriptComponent sight)");
        else if (ImGui::BeginTable("logictemplates", 7, kTableFlags))
        {
            ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Instances");
            ImGui::TableSetupColumn("Faulted");
            ImGui::TableSetupColumn("OnSpawn");
            ImGui::TableSetupColumn("OnUpdate");
            ImGui::TableSetupColumn("OnDestroy");
            ImGui::TableSetupColumn("State");
            ImGui::TableHeadersRow();
            for (const auto& t : snap.logicTemplates)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(t.path.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%d / %d", t.liveCount, t.instanceCount);
                ImGui::TableNextColumn();
                if (t.faultedCount > 0) ImGui::TextColored(kBad, "%d", t.faultedCount);
                else                    ImGui::TextDisabled("-");
                ImGui::TableNextColumn(); YesCell(t.hasSpawn);
                ImGui::TableNextColumn(); YesCell(t.hasUpdate);
                ImGui::TableNextColumn(); YesCell(t.hasDestroy);
                ImGui::TableNextColumn();
                if (t.loadFailed)
                {
                    // The entry may still hold the last-good prototype - say so.
                    ImGui::TextColored(kBad, "FAILED (last good kept)");
                    if (!t.lastError.empty() && ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", t.lastError.c_str());
                }
                else
                    ImGui::TextColored(kOk, "ok");
            }
            ImGui::EndTable();
        }
    }

    ImGui::End();
}
