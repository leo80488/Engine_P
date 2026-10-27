// The Project Settings window (display / present / HDR output / upscaling /
// quality / ray tracing / render features / post-process defaults / input /
// developer / pass tuning, plus settings.json persistence) and Lua's
// Editor.ProjectSetting. EditorLayer member definitions only, no state of their
// own; the widgets act directly on the live renderer / graphics device / window.
//
// Layout: a category list with a keyword search on the left, the selected page
// (one lambda each) on the right, and the settings.json buttons in a footer that
// never scrolls away.

#include "Editor/EditorLayer.h"
#include "Editor/ImGuiWidgets.h"                      // EditorUI::DragFloat / DragInt
#include "Editor/EditorTheme.h"                       // Heading / PushBold
#include "Editor/ExposureReadout.h"                   // the exposure number and the metering histogram
#include "Editor/InputBindingsWindow.h"               // Input page -> the bindings window
#include "Graphics/IGraphicsDevice.h"                 // PresentSettings / DisplayHDRInfo plumbing
#include "Graphics/HDROutput.h"                       // HDROutputMode(Name) / IsHDR / nit clamps
#include "Graphics/StreamlineBackend.h"               // Reflex / DLSS feature availability
#include "Graphics/MaterialSurfaceCache.h"            // MaterialSurfaceCache::kPageCount
#include "PostProcess/ProfileSystem.h"                // global default profile toggles
#include "PostProcess/PostProcessProfile.h"
#include "Renderer/Renderer.h"
#include "RenderGraph/RenderPass/AutoExposurePass.h"  // live exposure readout
#include "RenderGraph/RenderPass/DLSSPass.h"          // DLSS model (preset) override
#include "RenderGraph/RenderPass/PTRtxdiCore.h"       // PT direct light
#include "RenderGraph/RenderPass/FSR2Pass.h"          // FSR sharpening controls
#include "RenderGraph/RenderPass/TAAPass.h"           // TAA tuning (Upscaling & AA)
#include "RenderGraph/RenderPass/FXAAPass.h"          // FXAA tuning (Upscaling & AA)
#include "RenderGraph/RenderPass/VolumetricFogPass.h"
#include "Resource/EngineSettings.h"                  // settings.json Capture/Save + Load/Apply
#include "Settings/SettingsStore.h"                   // Load & Apply / Reset record their writes
#include "Settings/LightingTier.h"                    // the lighting tier preset
#include "Settings/Scalability.h"                     // the quality levels
#include "Resource/EngineDevSettings.h"               // engine.json (developer config)
#include "ECS/IKSystem.h"                             // the MMD IK switch (Developer)
#include "ECS/IKRigSystem.h"
#include "Graphics/GraphicsDX12.h"                    // display refresh / effective cap readout
#include "Graphics/GPUProfiler.h"                     // live ReSTIR cost
#include "RenderGraph/RenderPass/ReSTIRBackend.h"     // backend names
#include "Platform/Window.h"                          // fullscreen toggle (RequestFullscreen)
#include "Editor/SettingControls.h"
#include "Editor/PropertyRow.h"   // the search-results page (IPropertySource over every settings row)                   // a row's control and its recorded write
#include "Scripting/LuaDoc.h"                         // Editor.ProjectSetting

#include "imgui/imgui.h"
#include <sol/sol.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>
#include <string>


namespace
{
    // Case-insensitive substring test for the category search.
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

    // Paper white: the row at 0 follows Windows' SDR white level, a value overrides it. The checkbox goes back
    // to following; @p overrideRow draws the row's own control while the player overrides.
    // @p writeRow records and applies one row.
    void PaperWhiteRows(ISurface& surface, const std::function<void(const char*, const Settings::Value&)>& writeRow,
                        const std::function<void()>& overrideRow)
    {
        const bool following = surface.GetPresentSettings().paperWhiteNits <= 0.0f;
        bool follow = following;
        if (ImGui::Checkbox("Paper White follows Windows##hdrPaperWhiteFollow", &follow))
            writeRow("display.hdrPaperWhiteNits", Settings::Value{ .f = follow ? 0.0f : surface.GetPaperWhiteNits() });
        if (following)
            ImGui::TextDisabled("  %.0f nits: the SDR content brightness set in Windows", surface.GetPaperWhiteNits());
        else
            overrideRow();
    }

    // The Lighting Tier combo (Settings/LightingTier.h): a preset of the rows below it. It shows the tier
    // this frame's plan derives from those rows, marked custom when they match no tier exactly. A tier
    // that needs inline ray tracing is greyed out on a device without it, with the reason as its tooltip.
    // @p writeRow records and applies one row (a tier writes several).
    void LightingTierCombo(const Renderer& renderer, bool inlineRT,
                           const std::function<void(const char*, const Settings::Value&)>& writeRow)
    {
        const FramePlan::Decisions& plan = renderer.GamePlan();
        char preview[64];
        std::snprintf(preview, sizeof(preview), "%s%s", FramePlan::TierName(plan),
                      plan.tierPartial ? "  (custom)" : "");
        if (ImGui::BeginCombo("Lighting Tier", preview))
        {
            for (FramePlan::Tier t : Settings::kLightingTiers)
            {
                char label[128];
                std::snprintf(label, sizeof(label), "%-4s %s", Settings::LightingTierKey(t),
                              Settings::LightingTierSummary(t));
                const char* unavailable = Settings::LightingTierUnavailableReason(t, inlineRT);
                ImGui::BeginDisabled(unavailable != nullptr);
                if (ImGui::Selectable(label, plan.tier == t && !plan.tierPartial))
                {
                    for (const Settings::TierWrite& w : Settings::LightingTierWrites(t))
                        if (const Settings::Row* row = Settings::FindPlayerRow(w.path))
                            writeRow(w.path, row->kind == Settings::Kind::Bool ? Settings::Value{ .b = w.value != 0 }
                                                                              : Settings::Value{ .i = w.value });
                }
                ImGui::EndDisabled();
                if (unavailable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("%s", unavailable);
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A preset: choosing a tier sets the render path and the ray-traced and GI "
                              "switches below. Changing one of them afterwards shows the tier as custom. "
                              "The DDGI switch allows DDGI; a scene whose Indirect Lighting leaves it off "
                              "(or that has no DDGI volume) runs without it, and the tier shows what runs. "
                              "T4 and Ref choose the path tracer only: Ray Reconstruction comes with the "
                              "DLSS mode and its RR box.");
    }

    // The live GPU cost of the ReSTIR / PT sections: the p50 of the last
    // kSamples profiler results of each region, one sample per resolved
    // profiler frame.
    struct RestirCost
    {
        static constexpr uint32_t kSamples = 120;
        static constexpr const char* kNames[] = { "RTDI", "RTDI.RELAX", "RTDI.REBLUR",
                                                  "RTGI", "RTGI.Trace", "RTGI.RELAX", "RTGI.REBLUR", "RTGI.Sharc.Update",
                                                  "PT.Trace" };
        static constexpr size_t kCount = std::size(kNames);
        float    values[kCount][kSamples] = {};
        uint32_t count[kCount] = {}, head[kCount] = {};
        uint64_t sequence = 0;

        void Sample(const GPUProfiler* p)
        {
            if (!p || !p->enabled || !p->hasResults || p->resultsSequence == sequence) return;
            sequence = p->resultsSequence;
            for (uint32_t r = 0; r < p->resultCount; ++r)
            {
                const auto& res = p->results[r];
                if (!res.name) continue;
                for (size_t i = 0; i < kCount; ++i)
                    if (std::strcmp(res.name, kNames[i]) == 0)
                    {
                        values[i][head[i]] = res.gpuMs;
                        head[i] = (head[i] + 1) % kSamples;
                        count[i] = std::min(count[i] + 1, kSamples);
                        break;
                    }
            }
        }
        // -1 = no sample yet.
        float P50(const char* name) const
        {
            for (size_t i = 0; i < kCount; ++i)
                if (std::strcmp(name, kNames[i]) == 0)
                {
                    if (!count[i]) return -1.f;
                    float tmp[kSamples];
                    std::copy(values[i], values[i] + count[i], tmp);
                    std::nth_element(tmp, tmp + count[i] / 2, tmp + count[i]);
                    return tmp[count[i] / 2];
                }
            return -1.f;
        }
    };
    RestirCost s_restirCost;

    // "GPU p50: RTDI 1.23 ms (RTDI.RELAX 0.40 ms, ...)". Parts without a sample
    // are left out; the total without one reads "measuring".
    void RestirCostLine(const char* total, std::initializer_list<const char*> parts)
    {
        const float t = s_restirCost.P50(total);
        if (t < 0.f) { ImGui::TextDisabled("GPU p50: measuring %s...", total); return; }
        char text[256];
        int n = std::snprintf(text, sizeof(text), "GPU p50: %s %.2f ms", total, t);
        bool open = false;
        for (const char* part : parts)
        {
            const float v = s_restirCost.P50(part);
            if (v < 0.f || n <= 0 || n >= static_cast<int>(sizeof(text))) continue;
            n += std::snprintf(text + n, sizeof(text) - n, "%s%s %.2f", open ? ", " : " (", part, v);
            open = true;
        }
        if (open && n > 0 && n < static_cast<int>(sizeof(text))) std::snprintf(text + n, sizeof(text) - n, ")");
        ImGui::TextDisabled("%s", text);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Median GPU time over the last %u profiled frames (the Profiler's\n"
                              "region names). Change a setting and watch it settle.", RestirCost::kSamples);
    }

    // One core per stage: the hook of raytracing.restirBackend<stage>. Cores
    // this build cannot run are listed, disabled, with why; a request for one
    // (engine.json, the CLI, Lua) is shown with what really runs.
    bool RestirBackendCombo(Renderer& r, ReSTIR::Stage stage, const Settings::DevRow& row, Settings::Value& v)
    {
        ReSTIR::Backend requested;
        if (!ReSTIR::ParseBackend(stage, v.s, requested)) requested = ReSTIR::DefaultRequest(stage);
        bool changed = false;
        if (ImGui::BeginCombo(EditorUI::SettingLabel(row).c_str(), ReSTIR::BackendName(requested)))
        {
            for (uint8_t i = 0; i < static_cast<uint8_t>(ReSTIR::Backend::Count); ++i)
            {
                const auto b = static_cast<ReSTIR::Backend>(i);
                if (!ReSTIR::IsLegal(stage, b)) continue;
                const char* unavailable = r.UnavailableRestirBackendReason(stage, b);
                // A core that lost its stage's A/B stays selectable, labelled, until
                // ReSTIR::RetirementDate.
                // TODO(ReSTIR_Dual_Backend_Plan S8): the RTXDI DI / GI cores leave this menu on 2026-11-15
                // (then command line only for four weeks, then deleted).
                const char* retiring = ReSTIR::RetirementDate(stage, b);
                char item[48];
                std::snprintf(item, sizeof(item), "%s%s", ReSTIR::BackendName(b), retiring ? " (retiring)" : "");
                ImGui::BeginDisabled(unavailable != nullptr);
                if (ImGui::Selectable(item, b == requested) && b != requested)
                {
                    v.s = ReSTIR::BackendName(b);
                    changed = true;
                }
                ImGui::EndDisabled();
                if (unavailable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Not available here: %s\n(ReSTIR_Dual_Backend_Plan.md: DI S4, PT S5, GI S7).", unavailable);
                else if (retiring && ImGui::IsItemHovered())
                    ImGui::SetTooltip("The engine core won this stage's A/B (ReSTIR_Dual_Backend_Plan.md sec.4).\n"
                                      "Selectable here until %s while the weekly A/B runs; after that only\n"
                                      "the command line and the nightly reference packs select it.", retiring);
                else if (b == ReSTIR::Backend::Auto && ImGui::IsItemHovered())
                    ImGui::SetTooltip("By render path (sec.9.35): RTXDI's ReSTIR PT on Path Trace (Real-time),\n"
                                      "this pass's own path tracer on Path Trace (Reference) - and on the\n"
                                      "real-time path when a command-line pin asks for it (-pt_bounces,\n"
                                      "-pt_reference, -pt_area_*, -pt_lens_radius).");
            }
            ImGui::EndCombo();
        }
        EditorUI::SettingNotes(row);
        const ReSTIR::BackendStatus status = r.GetRestirBackendStatus(stage);
        if (std::strcmp(status.requestedName, status.effectiveName) != 0)
            ImGui::TextDisabled("  running %s: %s", status.effectiveName, status.reason);
        return changed;
    }

    // The DI / GI shell's denoiser: the hook of
    // raytracing.restir<Di|Gi>Denoiser. Under DLSS Ray Reconstruction it is the
    // denoiser before RR; what really runs (after the -di_* / -gi_* NRD flags
    // and RR) is shown beside it.
    bool RestirDenoiserCombo(Renderer& r, ReSTIR::Stage stage, const Settings::DevRow& row, Settings::Value& v)
    {
        const bool di = stage == ReSTIR::Stage::DI;
        static constexpr const char* kItems[] = { "Auto", "RELAX", "REBLUR", "None (raw)" };
        static_assert(std::size(kItems) == static_cast<size_t>(ReSTIR::Denoiser::Count));
        ReSTIR::Denoiser current;
        if (!ReSTIR::ParseDenoiser(v.s, current)) current = ReSTIR::Denoiser::Auto;
        int value = static_cast<int>(current);
        bool changed = false;
        if (ImGui::Combo(EditorUI::SettingLabel(row).c_str(), &value, kItems, IM_ARRAYSIZE(kItems)))
        {
            v.s = ReSTIR::DenoiserName(static_cast<ReSTIR::Denoiser>(value));
            changed = true;
        }
        EditorUI::SettingNotes(row);
        ImGui::SameLine();
        ImGui::TextDisabled("(running: %s%s)", di ? r.GetRestirDIEffectiveDenoiser() : r.GetRestirGIEffectiveDenoiser(),
            r.IsRayReconstructionActive() ? " + RR" : "");
        return changed;
    }

    // Preset rows: a button per preset, the one the live values match shown
    // as selected.
    template <typename Preset, size_t N, typename Match, typename Apply>
    void RestirPresetRow(const Preset (&presets)[N], Match matches, Apply apply)
    {
        ImGui::TextUnformatted("Preset");
        for (size_t i = 0; i < N; ++i)
        {
            ImGui::SameLine();
            const bool active = matches(presets[i]);
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::SmallButton(presets[i].name)) apply(presets[i]);
            if (active) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", presets[i].tip);
        }
    }

    struct DIPreset { const char* name; const char* tip; uint32_t candidates, spatialPasses, spatialK; bool fullRes; };
    constexpr DIPreset kDIPresets[] = {
        { "Performance", "4 candidates, 1 spatial pass with 2 neighbours, half-resolution trace.", 4, 1, 2, false },
        { "Balanced",    "The defaults: 8 candidates, 1 spatial pass with 3 neighbours, half resolution.", 8, 1, 3, false },
        { "Quality",     "16 candidates, 1 spatial pass with 5 neighbours, full-resolution trace\n"
                         "(no 2x2 reconstruction; about 4x the trace cost).", 16, 1, 5, true },
    };
    struct GIPreset { const char* name; const char* tip; uint32_t initial, emitter, exact; };
    constexpr GIPreset kGIPresets[] = {
        { "Performance", "1 cosine ray + 4 emitter candidates, 4 exact lights at a hit.", 1, 4, 4 },
        { "Balanced",    "The defaults: 2 cosine rays + 8 emitter candidates, 8 exact lights at a hit.", 2, 8, 8 },
        { "Quality",     "4 cosine rays + 16 emitter candidates, 16 exact lights at a hit.\n"
                         "Full resolution stays a separate choice (about 3.5x the GI cost).", 4, 16, 16 },
    };
}

namespace
{
    [[noreturn]] void ProjectSettingFail(const char* code, const std::string& what)
    {
        throw sol::error(std::string("Editor.ProjectSetting: ") + code + ": " + what);
    }
}

void EditorLayer::RegisterProjectSettingsLua(lua_State* L)
{
    if (!L) return;
    sol::state_view lua(L);
    sol::table ed = lua["Editor"].get_or_create<sol::table>();
    LuaDoc::Fn(ed, "Editor", "ProjectSetting",
        [this](sol::this_state ts, const std::string& path, sol::object value) -> sol::table
        {
            if (!m_renderer || !m_gfx) ProjectSettingFail("no_renderer", "the editor has no renderer");
            Settings::Request q;
            if (value.is<bool>())             { q.type = Settings::Request::Type::Bool;   q.b      = value.as<bool>(); }
            else if (value.is<double>())      { q.type = Settings::Request::Type::Number; q.number = value.as<double>(); }
            else if (value.is<std::string>()) { q.type = Settings::Request::Type::Text;   q.text   = value.as<std::string>(); }
            const auto refused = [&](Settings::Refusal why)
            {
                ProjectSettingFail("refused", path + " refuses the value (" + Settings::RefusalName(why) + ")");
            };
            sol::table out = sol::state_view(ts).create_table();
            out["path"] = path;
            Settings::Value v;
            if (const Settings::Row* r = Settings::FindPlayerRow(path))
            {
                if (const Settings::Refusal why = Settings::Coerce(*r, q, v); why != Settings::Refusal::None) refused(why);
                Settings::ApplyCtx c{ *m_renderer, *m_gfx, m_window };
                out["applied"] = Settings::EditPlayerRow(*r, v, EditorUI::kProjectSettingsWriter, c);
                out["domain"]  = "player";
                out["layer"]   = "player";
                return out;
            }
            if (const Settings::DevRow* r = Settings::FindDevRow(path))
            {
                if (const Settings::Refusal why = Settings::Coerce(*r, q, v); why != Settings::Refusal::None) refused(why);
                Settings::DevApplyCtx c{ *m_renderer };
                out["applied"] = Settings::EditDevRow(*r, v, EditorUI::kProjectSettingsWriter, c);
                out["domain"]  = "developer";
                out["layer"]   = "runtime";
                return out;
            }
            if (const Settings::PassRow* r = Settings::FindPassRow(path))
            {
                if (const Settings::Refusal why = Settings::Coerce(*r, q, v); why != Settings::Refusal::None) refused(why);
                out["applied"] = Settings::EditPassRow(*r, v, EditorUI::kProjectSettingsWriter);
                out["domain"]  = "pass";
                out["layer"]   = "runtime";
                return out;
            }
            ProjectSettingFail("unknown_path", "there is no setting '" + path +
                                               "' (python tools/gen_catalog.py --setting all names them)");
        },
        { .doc = "The write a Project Settings control makes: the value checked as Settings.set checks it, recorded as "
                 "'Project Settings' - a player setting in the Player layer, an engine.json one in the Runtime layer "
                 "(never saved; Save to engine.json is), a pass one in the Runtime layer (never saved) - and applied "
                 "for that setting alone (Settings.where shows it). For scripts and tests: a harness run has no mouse.",
          .params = { { .name = "path", .type = "SettingKey|DevSettingKey|PassSettingKey" }, { .name = "value" } },
          .returns = "table", .effect = LuaDoc::Effect::Settings, .context = LuaDoc::Context::Editor,
          .errors = { "no_renderer", "unknown_path", "refused" } });
}

void EditorLayer::SyncRestirCostWant(bool want)
{
    if (want == m_restirCostWant || !m_gfx) return;
    if (want) m_gfx->AddGPUProfilerWant();
    else m_gfx->ReleaseGPUProfilerWant();
    m_restirCostWant = want;
}

namespace
{
    // Every row of the three settings stores as one list, each drawn by its Project Settings control
    // (the search results page).
    struct SettingsRows : EditorUI::IPropertySource
    {
        EditorLayer& layer;
        explicit SettingsRows(EditorLayer& l) : layer(l) {}
        std::span<const Settings::Row>     player = Settings::PlayerRows();
        std::span<const Settings::DevRow>  dev    = Settings::DevRows();
        std::span<const Settings::PassRow> pass   = Settings::PassRows();
        template <class Fn> auto Visit(std::size_t i, Fn fn) const
        {
            if (i < player.size()) return fn(player[i]);
            i -= player.size();
            if (i < dev.size()) return fn(dev[i]);
            return fn(pass[i - dev.size()]);
        }
        std::size_t RowCount() const override { return player.size() + dev.size() + pass.size(); }
        const char* Label(std::size_t i) const override { return Visit(i, [](const auto& r) { return EditorUI::SettingTitle(r); }); }
        const char* Path(std::size_t i) const override  { return Visit(i, [](const auto& r) { return r.path; }); }
        const char* Tooltip(std::size_t i) const override { return Visit(i, [](const auto& r) { return r.spec.tip; }); }
        // The filter's Modified test is the row control's own (SettingControls), drawn below.
        bool Modified(std::size_t) const override { return true; }
        bool DrawRow(std::size_t i) override { return layer.DrawSettingsRow(Path(i)); }
    };
}

bool EditorLayer::DrawSettingsRow(const char* path)
{
    if (!m_renderer || !m_gfx) return false;
    if (Settings::FindPlayerRow(path))
    {
        Settings::CaptureCtx read{ *m_renderer, *m_gfx, m_window };
        Settings::ApplyCtx   write{ *m_renderer, *m_gfx, m_window };
        return EditorUI::PlayerSetting(path, read, write);
    }
    if (Settings::FindDevRow(path)) return EditorUI::DevSetting(path, *m_renderer);
    for (const Settings::PassRow& r : Settings::PassRows())
        if (std::strcmp(r.path, path) == 0) return EditorUI::PassSetting(r);
    return false;
}

void EditorLayer::RenderGraphicsSettingsWindow()
{
    // Player rows (settings.json) and developer rows (engine.json). Widgets act
    // directly on the live renderer / graphics / window; the footer's buttons
    // save / load settings.json. "###Graphics Settings" is the window's ImGui ID
    // (its former title): keep it, or a saved imgui.ini loses the window's place.
    ImGui::SetNextWindowSize(ImVec2(860.0f, 640.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Project Settings###Graphics Settings", &m_showGraphicsSettings))
    {
        SyncRestirCostWant(false);
        ImGui::End();
        return;
    }
    // Set by the Ray Tracing page while a ReSTIR / PT section (and its live
    // cost line) is drawn; synced after the page.
    bool restirCostOnScreen = false;

    // engine.json, not settings.json: a different document with a different
    // lifecycle (sealed into game.ipak, never written by a shipped build), so it
    // has its own buttons (Developer page, and the Ray Tracing page's ReSTIR / PT
    // sections). Do not share the settings.json button: one click would silently
    // write two files, one of them shipped content.
    auto saveEngineJson = [&]()
    {
        // Seeded from the file before capturing (read-modify-write): CaptureFrom
        // refreshes only the knobs the Renderer owns, so without the seed an
        // authored-only key (gameFlow.quitConfirmState) would be written back as
        // its C++ default, i.e. deleted. Recorded in the developer settings store.
        Resource::EditEngineDevSettingsFile([&](Resource::EngineDevSettings& d) { d.CaptureFrom(*m_renderer); },
                                            "Graphics Settings: Save to engine.json");
    };
    auto reloadEngineJson = [&]()
    {
        // An absent or unreadable file reapplies the C++ defaults.
        Resource::ReloadEngineDevSettings(*m_renderer, "Graphics Settings: Reload engine.json", /*requireFile*/false);
    };
    if (!m_renderer || !m_gfx)
    {
        ImGui::TextDisabled("(renderer / graphics device not available)");
        ImGui::End();
        return;
    }

    // A player row's control on a page: the value is what runs; a change is
    // recorded as "Project Settings" in the Player layer and applied for that
    // row alone (a present row changes its field of the live present settings
    // and pushes them back: Settings::ApplyPlayerRow).
    auto player = [&](const char* path, const EditorUI::PlayerDraw& draw = {})
    {
        Settings::CaptureCtx read{ *m_renderer, *m_gfx, m_window };
        Settings::ApplyCtx   write{ *m_renderer, *m_gfx, m_window };
        return EditorUI::PlayerSetting(path, read, write, draw);
    };
    // An engine.json row's control: the same cycle, in the Runtime layer.
    auto dev = [&](const char* path, const EditorUI::DevDraw& draw = {})
    {
        return EditorUI::DevSetting(path, *m_renderer, draw);
    };
    // A write that is not a control's own (a preset button, a shortcut),
    // recorded and applied the same way.
    auto playerSet = [&](const char* path, const Settings::Value& v)
    {
        if (const Settings::Row* row = Settings::FindPlayerRow(path))
        {
            Settings::ApplyCtx c{ *m_renderer, *m_gfx, m_window };
            Settings::EditPlayerRow(*row, v, EditorUI::kProjectSettingsWriter, c);
        }
    };
    auto devSet = [&](const char* path, const Settings::Value& v)
    {
        if (const Settings::DevRow* row = Settings::FindDevRow(path))
        {
            Settings::DevApplyCtx c{ *m_renderer };
            Settings::EditDevRow(*row, v, EditorUI::kProjectSettingsWriter, c);
        }
    };

    auto pageDisplay = [&]()
    {
        // ---- Display / present ----
        // Each control is a settings.json row: EngineSettingsSchema.inl holds its
        // title, tooltip and choices.
        player("display.frameRateCap");
        // Detected refresh + the cap actually enforced once the G-Sync ceiling
        // applies. The refresh is re-queried when the window appears (the monitor
        // under it can change between openings), not every frame.
        if (auto* dx12 = dynamic_cast<GraphicsDX12*>(m_gfx))
        {
            if (ImGui::IsWindowAppearing())
                dx12->Surface().RefreshDisplayInfo();
            const RHI::PresentSettings live = m_gfx->MainSurface().GetPresentSettings();
            const int hz  = dx12->Surface().GetDisplayRefreshHz();
            const int eff = dx12->Surface().EffectiveFrameCap();
            const bool gsyncBinding = live.gsync && eff > 0
                                   && (live.frameRateCap == 0 || eff < live.frameRateCap);
            if (hz > 0)
                ImGui::TextDisabled("  Display %d Hz  ·  effective cap %s%s", hz,
                                    eff > 0 ? (std::to_string(eff) + " FPS").c_str() : "unlimited",
                                    gsyncBinding ? " (G-Sync ceiling)" : "");
        }

        player("display.vsync");
        player("display.gsync");   // the G-Sync ceiling depends on the live refresh

        // Fullscreen is a request, never applied here: the row's apply is
        // Window::RequestFullscreen. Do not call Window::SetFullscreen from ImGui: it
        // reaches SurfaceDX12::SetFullscreen, which destroys and recreates every back
        // buffer (WaitForPreviousFrame, render targets released, ResizeSwapChainAndRtv)
        // while the open primary command list has already recorded a
        // PRESENT->RENDER_TARGET transition and OMSetRenderTargets against them. A
        // command list holds no refcount, so the driver then reads freed memory (AV in
        // nvwgf2umx.dll). Window::ServicePendingFullscreen (App::Run, after
        // ProcessMessage and before BeginFrame) is the only safe swap-chain rebuild
        // point.
        ImGui::BeginDisabled(m_window == nullptr);
        player("display.fullscreen");
        ImGui::EndDisabled();

        // Do not add a render-scale control here: the one control
        // (antialiasing.fsrRenderScale or display.renderScale, by whether an
        // upscaler runs) is on the Upscaling & AA page, and two live controls over
        // one render ratio silently multiply under FSR.

        // ---- HDR Output ----
        // Its rows are present rows like the knobs above: each changes its field of
        // the live present settings. hdrMode needs a swap-chain rebuild, but the
        // backend latches it to the next frame boundary, so this is still an ordinary
        // mid-frame ImGui write (see RHI::PresentSettings::hdrMode).
        //
        // This is the only panel that owns these controls. Do not mirror them into
        // the post-process panel: paper white and peak nits change the display
        // signal, not the look, and two live copies of a value the backend can
        // override make "the slider does nothing" bugs.
        EditorTheme::Heading("HDR Output");
        {
            // Capability line. HDR is a display mode owned by Windows: the engine only
            // chooses the signal format it hands the compositor and cannot turn the
            // display's HDR on. Read-only; it shows a user whose HDR10 selection looks
            // washed out why.
            const RHI::DisplayHDRInfo hdrInfo = m_gfx->MainSurface().GetDisplayHDRInfo();
            if (hdrInfo.hdrCapable)
            {
                ImGui::TextDisabled("  Display: HDR is ON in Windows.");
                ImGui::TextDisabled("  Peak %.0f nits, sustained full-frame %.0f nits, black level %.4f nits.",
                                    hdrInfo.maxLuminanceNits,
                                    hdrInfo.maxFullFrameNits,
                                    hdrInfo.minLuminanceNits);
            }
            else
            {
                ImGui::TextDisabled("  Display: the OS does not report this monitor as HDR.");
                ImGui::TextDisabled("  Enable HDR for it in Windows Settings > System > Display > Use HDR first —");
                ImGui::TextDisabled("  the engine cannot switch the desktop into HDR on your behalf.");
            }

            // The modes by their descriptive names (HDROutputModeName - the
            // enum's own, so the list cannot drift); the row's labels are the
            // short names Settings.set and settings.json take.
            player("display.hdrMode", [&](const Settings::Row& row, Settings::Value& v)
            {
                bool changed = false;
                if (ImGui::BeginCombo(EditorUI::SettingLabel(row).c_str(),
                                      HDROutputModeName(static_cast<HDROutputMode>(v.i))))
                {
                    for (int i = 0; i < static_cast<int>(row.labels.size()); ++i)
                        if (ImGui::Selectable(HDROutputModeName(static_cast<HDROutputMode>(i)), i == v.i) && i != v.i)
                        {
                            v.i = i;
                            changed = true;
                        }
                    ImGui::EndCombo();
                }
                EditorUI::SettingNotes(row);
                return changed;
            });
            const RHI::PresentSettings live = m_gfx->MainSurface().GetPresentSettings();

            // The capability query is advisory, not a gate: it is wrong on some drivers
            // and in remote / virtual sessions, and the backend falls back to SDR by
            // itself when the swap chain refuses the colour space. Hard-disabling the
            // combo would lock out displays that work; warn and let the user try.
            if (!hdrInfo.hdrCapable && IsHDR(live.hdrMode))
                ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f),
                                   "  HDR selected on a display the OS calls SDR — expect a washed-out\n"
                                   "  image, or a silent fallback to SDR. Try it anyway if you know better.");

            // What is actually running, which can differ from the request: a mode
            // change is latched to the next frame boundary (one frame of lag), and the
            // swap chain can refuse the colour space (the backend falls back to SDR).
            // Neither raises an error, so this line is the user's only signal.
            const HDROutputMode liveMode = m_gfx->MainSurface().GetHDROutputMode();
            if (liveMode != live.hdrMode)
                ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f),
                                   "  Active: %s — either the change has not landed yet (it applies at the\n"
                                   "  next frame boundary) or the swap chain refused it and fell back.",
                                   HDROutputModeName(liveMode));

            // Nit knobs mean nothing in SDR (the present tail copies the already
            // gamma-encoded composition buffer through). Their rows' controls are drags
            // clamped to the rows' ranges (AlwaysClamp), so text entry cannot hand the
            // PQ encode in the present shader a NaN, which would blank the screen.
            if (IsHDR(live.hdrMode))
            {
                ImGui::Indent();
                PaperWhiteRows(m_gfx->MainSurface(), playerSet, [&] { player("display.hdrPaperWhiteNits"); });
                player("display.hdrPeakNits");
                ImGui::Unindent();
            }
        }
    };

    auto pageUpscaling = [&]()
    {
        // ---- Upscaling & Frame Generation ----
        // AA mode, render scale, DLSS model, Reflex and Frame Generation live
        // together because they are one decision chain: DLSS-G requires Reflex (the
        // engine forces it on), the render-scale enum only means something under an
        // upscaler, and the DLSS model decides whether a render-scale change is only
        // a resolution change. Split across the panel, FG ends up requested under an
        // AA mode that cannot drive it.
        //
        // Each control is a settings.json row; the hooks below replace the widget
        // where the row's plain one does not fit.

        // ---- Anti-aliasing ----
        EditorTheme::Heading("Anti-Aliasing");
        {
            // The one AA combo. Ray Reconstruction is not in it: RR is an option of
            // DLSS, the box below the combo while DLSS is selected. Underneath it stays
            // one enum value, AAMode::DLSS_RR (antialiasing.mode's seventh label, which
            // settings.json, Lua and the packs use); the hook folds it into the box. The
            // combo index is the enum value, so the folded mode must be the last one: a
            // mode appended after it needs a real index map here (the static_asserts say
            // so).
            static const char* kAA[] = { "None", "FXAA", "TAA", "FXAA + TAA", "FSR", "DLSS" };
            static_assert(static_cast<uint32_t>(Renderer::AAMode::DLSS_RR) == Renderer::kMaxAAMode,
                          "a mode after DLSS_RR: map combo indices to AAMode explicitly");
            static_assert(IM_ARRAYSIZE(kAA) == Renderer::kMaxAAMode,
                          "AA mode combo is missing an entry — see Renderer::AAMode (DLSS_RR is DLSS + the RR box)");
            constexpr int kDLSSItem = static_cast<int>(Renderer::AAMode::DLSS);
            constexpr int kRRMode   = static_cast<int>(Renderer::AAMode::DLSS_RR);
            player("antialiasing.mode", [&](const Settings::Row& row, Settings::Value& v)
            {
                // A DLSS_RR request shows as DLSS + the box even while RR cannot
                // run (the effective mode is DLSS SR, TAA or PT accumulation then),
                // so the box that turns it off is always on screen; any other
                // request shows the mode that runs.
                int aa = v.i == kRRMode ? kDLSSItem : static_cast<int>(m_renderer->GetAAMode());
                bool changed = false;
                if (ImGui::Combo(EditorUI::SettingLabel(row).c_str(), &aa, kAA, IM_ARRAYSIZE(kAA)))
                {
                    v.i = aa;
                    changed = true;
                }
                EditorUI::SettingNotes(row);
                if (!SL::StreamlineBackend::Get().IsFeatureSupported(SL::Feature::DLSS))
                    ImGui::TextDisabled("DLSS unavailable: %s",
                        SL::StreamlineBackend::Get().GetFeatureStatusText(SL::Feature::DLSS));

                // ---- Ray Reconstruction (an option of DLSS) ----
                if (aa == kDLSSItem)
                {
                    bool rr = v.i == kRRMode;
                    if (ImGui::Checkbox("Ray Reconstruction (DLSS-RR)", &rr))
                    {
                        v.i = rr ? kRRMode : kDLSSItem;
                        changed = true;
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("DLSS's denoiser for the ray-traced paths: it replaces NRD on ReSTIR DI / GI and the RT\n"
                                          "reflections, and denoises the path tracer (GBuffer primary). Render Scale: Native = DLAA,\n"
                                          "else RR upscales. On the Raster path there is nothing to denoise: DLSS Super Resolution runs.");
                }
                return changed;
            });
            if (m_renderer->GetAAModeRequested() == Renderer::AAMode::DLSS_RR)
            {
                ImGui::Indent();
                const bool rrLoaded = SL::StreamlineBackend::Get().IsFeatureSupported(SL::Feature::DLSS_RR);
                if (const char* why = m_renderer->GetRayReconstructionInertReason())
                {
                    if (!rrLoaded)
                        ImGui::TextDisabled("RR inactive: %s. The plugin loads at startup (engine.json raytracing.rayReconstructionLoad, or -dlss_rr).", why);
                    else
                        ImGui::TextDisabled("RR inactive: %s; %s.", why,
                            Renderer::IsPathTrace(m_renderer->GetRenderPath()) ? "progressive PT accumulation runs"
                            : m_renderer->IsRRFallbackToDLSS() ? "DLSS Super Resolution runs" : "TAA runs");
                }
                else if (Renderer::IsPathTrace(m_renderer->GetRenderPath()))
                    ImGui::TextDisabled("RR denoises real-time PT (RTXDI's ReSTIR PT, GBuffer primary).");
                else
                    ImGui::TextDisabled("RR denoises ReSTIR DI / GI and RT reflections (NRD off, raster guides,\nDI at full resolution).");
                player("antialiasing.dlssRRModel");
                if (m_renderer->GetDLSSRRModel() != m_renderer->GetDLSSRRModelSetting())
                    ImGui::TextDisabled("(-dlss_rr_preset runs %s)", SL::DLSSRRModelName(m_renderer->GetDLSSRRModel()));
                ImGui::Unindent();
            }

            // ---- Render Scale (shared by FSR and DLSS) ----
            // One enum drives both: Native is 1:1 (FSR native AA / DLAA, pure
            // anti-aliasing); the rest render smaller and the upscaler resolves to
            // display resolution.
            const bool fsrMode      = (m_renderer->GetAAMode() == Renderer::AAMode::FSR);
            const bool dlssMode     = (m_renderer->GetAAMode() == Renderer::AAMode::DLSS);
            const bool rrMode       = m_renderer->IsRayReconstructionActive();
            const bool upscalerMode = fsrMode || dlssMode || rrMode;

            // One render-scale control, chosen by whether an upscaler runs - never two.
            // The enum (antialiasing.fsrRenderScale) is authoritative under FSR / DLSS;
            // the free scalar (display.renderScale) otherwise, because
            // Renderer::GetEffectiveUpscaleMode() forces Native whenever the AA mode is
            // not FSR or DLSS, so under None / FXAA / TAA the scalar is the only
            // resolution lever. The default AA is TAA, and a machine without DLSS needs
            // that lever to trade resolution for frames.
            //
            // Never show both: they compose under FSR (the upscaler's render size is
            // multiplied by renderScale), so "Quality + 0.75" would render at a ratio
            // neither control shows.
            if (upscalerMode)
            {
                // The presets with their ratios (the row's labels are the short
                // names Settings.set and the in-game menu take).
                static const char* kUpscaleModes[] = {
                    "Native (1:1 AA)",
                    "Quality (1.5x)",
                    "Balanced (1.7x)",
                    "Performance (2.0x)",
                    "Ultra Performance (3.0x)",
                };
                player("antialiasing.fsrRenderScale", [&](const Settings::Row& row, Settings::Value& v)
                {
                    const bool changed = ImGui::Combo(EditorUI::SettingLabel(row).c_str(), &v.i,
                                                      kUpscaleModes, IM_ARRAYSIZE(kUpscaleModes));
                    EditorUI::SettingNotes(row);
                    return changed;
                });
                // Under DLSS the render size comes from the SDK's own table, not from
                // the label's ratio: print the real numbers.
                ImGui::TextDisabled("Render %ux%u -> Display %ux%u",
                    m_renderer->GetRenderWidth(),  m_renderer->GetRenderHeight(),
                    m_renderer->GetDisplayWidth(), m_renderer->GetDisplayHeight());
            }
            else
            {
                player("display.renderScale");
                ImGui::TextDisabled("  <1 trades resolution for frames, >1 supersamples.\n"
                                    "  3D only — UI stays at display resolution.\n"
                                    "  Switches to the upscaler preset list under FSR / DLSS.");
            }

            // Not settings: FSR's live sharpening knobs.
            if (FSR2Pass* fsr = m_renderer->GetFSR2Pass())
            {
                ImGui::BeginDisabled(!fsrMode);
                ImGui::Checkbox("FSR Sharpening", &fsr->sharpeningEnabled);
                EditorUI::DragFloat("FSR Sharpness", &fsr->sharpness, 0.0f, 1.0f, "%.2f");
                ImGui::EndDisabled();
            }

            // Which DL model DLSS runs (antialiasing.dlssPreset). Exposed because
            // the SDK's per-mode defaults make the quality slider change the model as
            // well as the resolution (K for DLAA / Quality / Balanced, M / L for
            // Performance / Ultra Performance), and the models differ visibly on thin
            // dark features such as the character outline.
            if (DLSSPass* dlss = m_renderer->GetDLSSPass())
            {
                ImGui::BeginDisabled(!dlssMode);
                player("antialiasing.dlssPreset");
                ImGui::EndDisabled();
                if (dlssMode)
                    ImGui::TextDisabled("%s", SL::DLSSPresetName(dlss->preset));
            }
        }

        // ---- NVIDIA Reflex ----
        EditorTheme::Heading("NVIDIA Reflex");
        {
            const bool reflexOk =
                SL::StreamlineBackend::Get().IsFeatureSupported(SL::Feature::Reflex);
            ImGui::BeginDisabled(!reflexOk);
            player("display.reflex");
            ImGui::EndDisabled();
            const RHI::PresentSettings live = m_gfx->MainSurface().GetPresentSettings();
            if (!reflexOk)
            {
                ImGui::TextDisabled("Reflex unavailable: %s",
                    SL::StreamlineBackend::Get().GetFeatureStatusText(SL::Feature::Reflex));
            }
            else if (live.reflexMode != 0)
            {
                // The frame cap above still chooses the number; Reflex enforces it.
                // Say so: the engine's own pacing wait drops to zero, which looks in
                // the profiler like the cap stopped working.
                ImGui::SameLine();
                ImGui::TextDisabled("(Reflex paces frames; engine limiter stands down)");
            }
            else if (live.frameGenFrames > 0 && reflexOk)
            {
                // The Reflex combo reads "Off" while Reflex runs, because FG forces
                // the effective mode up without rewriting the saved preference. Say
                // so, or the two controls look inconsistent.
                ImGui::SameLine();
                ImGui::TextDisabled("(forced on by Frame Generation)");
            }
        }


        // ---- DLSS Frame Generation ----
        EditorTheme::Heading("Frame Generation");
        {
            auto& sl = SL::StreamlineBackend::Get();
            // Read the snapshot, never slDLSSGGetState: querying it resets the SDK's
            // presents-since-last-query counter, and this panel redraws every frame,
            // which would drain the window GraphicsDX12 measures over and make Frame
            // Generation read as dead. GraphicsDX12 is the single owner of that query.
            SL::FrameGenState fgState{};
            SL::FrameGenState fgCaps{};
            float fgRatio = 0.0f;
            if (auto* dx12fg = dynamic_cast<GraphicsDX12*>(m_gfx))
            {
                fgState = dx12fg->Surface().GetFrameGenSnapshot();
                fgCaps  = dx12fg->Surface().GetFrameGenCaps();
                fgRatio = dx12fg->Surface().GetFrameGenPresentRatio();
            }

            const bool fgSupported = sl.IsFeatureSupported(SL::Feature::DLSS_G);

            // Build the multiplier list from what the hardware reports, not a fixed
            // 2x/3x/4x: numFramesToGenerateMax is a hard SDK limit, and exceeding it
            // makes slDLSSGSetOptions fail, which turns Frame Generation off rather
            // than degrading to a lower multiplier. Default to 2x-capable so the
            // control works before the driver answers; the engine's clamp covers a
            // smaller answer.
            const uint32_t maxGen = fgCaps.numFramesToGenerateMax
                                      ? fgCaps.numFramesToGenerateMax : 1u;
            static const char* kFGFixed[] = { "Off", "2x", "3x", "4x" };
            const char* fgItems[5]        = {};
            int fgValues[5]               = {};   // maps combo index -> frameGenFrames
            int fgCount                   = 0;
            for (uint32_t i = 0; i <= maxGen && i < 4; ++i)
            {
                fgItems[fgCount]  = kFGFixed[i];
                fgValues[fgCount] = static_cast<int>(i);
                ++fgCount;
            }
            if (fgCaps.dynamicMFGSupported)
            {
                fgItems[fgCount]  = "Dynamic";
                fgValues[fgCount] = RHI::PresentSettings::kFrameGenDynamic;
                ++fgCount;
            }

            ImGui::BeginDisabled(!fgSupported);
            player("display.frameGen", [&](const Settings::Row& row, Settings::Value& v)
            {
                // A saved request can be unrepresentable (settings.json carrying
                // 4x, or Dynamic, on a GPU that offers neither). Index 0 would then
                // read "Off" while FG runs at the clamped multiplier; show the
                // highest fixed entry instead, which is what ApplyFrameGeneration
                // falls back to.
                int fgIndex = (v.i > 0) ? fgCount - 1 : 0;
                if (fgIndex > 0 && fgValues[fgIndex] == RHI::PresentSettings::kFrameGenDynamic)
                    --fgIndex;
                for (int i = 0; i < fgCount; ++i)
                    if (fgValues[i] == v.i) { fgIndex = i; break; }
                bool changed = false;
                if (ImGui::Combo(EditorUI::SettingLabel(row).c_str(), &fgIndex, fgItems, fgCount))
                {
                    v.i = fgValues[fgIndex];
                    changed = true;
                }
                EditorUI::SettingNotes(row);
                return changed;
            });
            ImGui::EndDisabled();

            const RHI::PresentSettings live = m_gfx->MainSurface().GetPresentSettings();
            if (!fgSupported)
            {
                ImGui::TextDisabled("Frame Generation unavailable: %s",
                    sl.GetFeatureStatusText(SL::Feature::DLSS_G));
            }
            else
            {
                // Docked mode composites the scene into a panel, so FG has no
                // coherent full-screen image to interpolate. F11 fixes that and is
                // the practical way to try FG without building the game.
                ImGui::TextDisabled("Needs F11 fullscreen viewport (or a game build)");
                if (live.frameGenFrames == RHI::PresentSettings::kFrameGenDynamic)
                    ImGui::TextDisabled("Dynamic: the driver picks the multiplier to fit the display refresh");
                // The request / effective split made visible: a silently refused FG
                // is the failure this panel exists to prevent.
                if (auto* dx12 = dynamic_cast<GraphicsDX12*>(m_gfx))
                {
                    const int active = dx12->Surface().GetFrameGenActive();
                    if (live.frameGenFrames > 0 && active == 0)
                    {
                        // Say why here, not only in the log: otherwise the combo looks
                        // accepted and nothing on screen says a gate refused it.
                        const char* why = dx12->Surface().GetFrameGenRefusal();
                        ImGui::TextDisabled("Requested but NOT running%s%s",
                                            why ? " - " : " - see DX12Log.txt", why ? why : "");
                    }
                    else if (active > 0 && !fgState.statusOk)
                        ImGui::TextDisabled("Running but reporting: %s", fgState.statusText);
                    else if (active > 0)
                        ImGui::TextDisabled("Presenting %.2fx rendered frames", fgRatio);
                }
            }
        }


        // ---- Streamline status (read-only) ----
        ImGui::Spacing();
        // Streamline refuses features silently (slInit succeeds, the options call
        // succeeds, nothing happens), so the support state is the first thing to
        // check when a toggle appears to do nothing.
        if (ImGui::TreeNode("Streamline status"))
        {
            auto& sl = SL::StreamlineBackend::Get();
            if (!sl.IsInitialized())
            {
                ImGui::TextDisabled("Not initialised - sl.interposer.dll missing or slInit failed");
            }
            else
            {
                struct { const char* label; SL::Feature f; } kFeatures[] = {
                    { "DLSS Super Resolution", SL::Feature::DLSS   },
                    { "DLSS Frame Generation", SL::Feature::DLSS_G },
                    { "DLSS Ray Reconstruction", SL::Feature::DLSS_RR },
                    { "Reflex",                SL::Feature::Reflex },
                    { "PCL",                   SL::Feature::PCL    },
                };
                for (const auto& e : kFeatures)
                {
                    const bool ok = sl.IsFeatureSupported(e.f);
                    ImGui::TextColored(ok ? ImVec4(0.55f, 0.85f, 0.55f, 1.0f)
                                          : ImVec4(0.65f, 0.65f, 0.65f, 1.0f),
                                       "%-24s %s", e.label, ok ? "supported" : "unavailable");
                    if (!ok && ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", sl.GetFeatureStatusText(e.f));
                }

                if (auto* dx12 = dynamic_cast<GraphicsDX12*>(m_gfx))
                {
                    const SL::FrameGenState& caps = dx12->Surface().GetFrameGenCaps();
                    ImGui::Separator();
                    if (caps.numFramesToGenerateMax > 0)
                    {
                        ImGui::TextDisabled("Max multiplier      %ux", caps.numFramesToGenerateMax + 1);
                        ImGui::TextDisabled("Dynamic multiplier  %s",
                                            caps.dynamicMFGSupported ? "yes" : "no (needs 50-series + a recent driver)");
                        ImGui::TextDisabled("VSync with FG       %s", caps.vsyncSupported ? "yes" : "no");
                        // The VRAM estimate comes from the live snapshot, not the
                        // caps: DLSS-G computes it only once configured, and the caps
                        // are probed at swap-chain creation (before any
                        // SetFrameGenOptions), so the cached copy is always 0. Shown
                        // only while FG runs, rather than printing 0 as if FG were free.
                        const uint64_t vram = dx12->Surface().GetFrameGenSnapshot().estimatedVRAMBytes;
                        if (vram > 0)
                            ImGui::TextDisabled("Estimated VRAM      %.0f MB", vram / (1024.0 * 1024.0));
                        else
                            ImGui::TextDisabled("Estimated VRAM      (reported once FG is running)");
                    }
                    else
                    {
                        // Distinguish "not asked yet" from "answered zero". The
                        // caps query needs the proxy swap chain, so it can be
                        // genuinely too early rather than genuinely unsupported.
                        ImGui::TextDisabled("Frame Generation capabilities not reported yet");
                    }
                }
            }
            ImGui::TreePop();
        }

        // ---- Advanced tuning ----
        // Live pass knobs, not player options: not written to settings.json. Every
        // anti-aliasing control lives on this page.
        EditorTheme::Heading("Anti-Aliasing Tuning");
        ImGui::TextDisabled("Live pass values for A/B work - not saved to settings.json.");
        // ---- TAA ----
        if (TAAPass* taa = m_renderer ? m_renderer->GetTAAPass() : nullptr)
        {
            if (ImGui::CollapsingHeader("Temporal AA (TAA)", 0))
            {

                // Direct history blend weight: alpha_diffuse = 1 - w, alpha_spec =
                // alpha_diffuse x 0.125. 0.0 ~ TAA off (each frame fully replaced), 0.9
                // default (UE-class integration), 0.95 cinematic. Drop toward 0.5 for
                // ghost-prone surfaces (foliage / curtains). Rationale: the historyWeight
                // comment in TAA_Common.hlsli.
                EditorUI::DragFloat("History weight", &taa->historyWeight, 0.0f, 0.99f, "%.3f");
                ImGui::SameLine();
                ImGui::TextDisabled("(higher = more history, more ghost-prone)");

                // Variance-clip box widths (YCoCg AABB). Lower = tighter clamp; Falcor's
                // default 1.0, 1.5 reduces distant shimmer.
                EditorUI::DragFloat("Box σ (base)",       &taa->colorBoxSigma,         0.5f, 4.0f, "%.2f");
                EditorUI::DragFloat("Box σ (specular)",   &taa->colorBoxSigmaSpecular, 0.5f, 4.0f, "%.2f");
                EditorUI::DragFloat("Specular roughness", &taa->specularRoughnessMax,  0.0f, 1.0f, "%.2f");

                // Distance-to-clamp anti-flicker (Karis 2014): leans on history when the
                // pre-clip colour is near the AABB boundary.
                ImGui::Checkbox("Anti-flicker (distance-to-clamp)", &taa->antiFlicker);

                // Motion-proportional AABB widening: 0 = tight clamp (Karis-dim peaks),
                // 1 = AABB doubles at full motion. Higher for HDR scenes.
                EditorUI::DragFloat("Velocity widen", &taa->velocityWiden, 0.0f, 2.0f, "%.2f");

                // Karis 5-tap unsharp on the de-jittered current frame: 0 = off, 0.1
                // default, > 0.2 rings on high-contrast edges.
                EditorUI::DragFloat("Sharpen strength", &taa->sharpenStrength, 0.0f, 0.3f, "%.2f");

                ImGui::SeparatorText("Responsive Motion");
                EditorUI::DragFloat("Motion start (px)", &taa->motionVelocityStartPx, 0.0f, 1.0f, "%.3f");
                EditorUI::DragFloat("Motion full (px)", &taa->motionVelocityEndPx, 0.1f, 4.0f, "%.2f");
                EditorUI::DragFloat("Motion alpha floor", &taa->motionAlphaFloor, 0.0f, 1.0f, "%.2f");
                EditorUI::DragFloat("Motion raw current", &taa->motionRawBlend, 0.0f, 1.0f, "%.2f");

                // Outline-aware history weakening: OutlinePass tags rim pixels with a
                // stencil bit and TAA floors alpha to this value there, so the outline
                // tracks the silhouette instead of ghosting. 0 disables; 0.5 ~ 3-frame
                // convergence; 1.0 = no history (slightly aliased rim).
                EditorUI::DragFloat("Outline α floor", &taa->outlineMinAlpha, 0.0f, 1.0f, "%.2f");
                ImGui::SetItemTooltip(
                    "Denies history to pixels the rim covers THIS frame. Defaults to 0:\n"
                    "the rim is a thin line jittering +-0.5 px, so denying it history stops\n"
                    "the jitter sequence from averaging and the outline SHIMMERS. The trail\n"
                    "is handled at its source by 'Outline vacated a' instead. Raise only if\n"
                    "you deliberately want a hard, aliased rim.");

                EditorUI::DragFloat("Outline vacated α", &taa->outlineVacatedAlpha,
                                    0.0f, 1.0f, "%.2f");
                ImGui::SetItemTooltip(
                    "History rejection on the band the outline JUST LEFT -- the\n"
                    "'character leaves outline-shaped edges where it passed' trail.\n"
                    "The hull writes no depth, so depth disocclusion never fires there;\n"
                    "no velocity, so the texel reprojects to itself; and the 3x3 clamp\n"
                    "already contains the rim's new position. TAA keeps its own one-frame\n"
                    "outline coverage mask to catch it. This one WORKS -- it removed the\n"
                    "ghosting where three history-side knobs before it did nothing.\n"
                    "1 = discard history there. 0 disables.\n"
                    "Inert while Outline > 'Draw as geometry' is ON -- the geometry\n"
                    "path drops the stencil tag this is gated on.");

                ImGui::Checkbox("DEBUG: show outline stencil", &taa->outlineStencilDebug);
                ImGui::SetItemTooltip(
                    "Paints every pixel OutlinePass tagged with its stencil bit magenta.\n"
                    "All outline handling in TAA is gated on that bit -- if no magenta\n"
                    "ring appears around an outlined object, none of it is running and\n"
                    "the outline is not coming from OutlinePass at all.");
            }
        }

        // ---- FXAA ----
        if (FXAAPass* fxaa = m_renderer ? m_renderer->GetFXAAPass() : nullptr)
        {
            if (ImGui::CollapsingHeader("Fast Approximate AA (FXAA)", 0))
            {
                ImGui::TextDisabled("Active when AA Mode is FXAA or FXAA + TAA");
                // Quality knobs from NVIDIA FXAA 3.11 reference.
                // qualitySubpix: subpixel softening. 0 hard edges, 0.75 default, 1 max blur.
                EditorUI::DragFloat("Subpixel quality",     &fxaa->qualitySubpix,           0.0f, 1.0f,  "%.2f");
                // Edge threshold: relative contrast minimum. 0.063 high quality, 0.166 default.
                EditorUI::DragFloat("Edge threshold",       &fxaa->qualityEdgeThreshold,    0.04f, 0.4f, "%.3f");
                // Edge threshold min: absolute floor. 0.0312 high, 0.0833 default.
                EditorUI::DragFloat("Edge threshold (min)", &fxaa->qualityEdgeThresholdMin, 0.02f, 0.2f, "%.4f");
            }
        }

    };

    auto pageQuality = [&]()
    {
        namespace Sc = Settings::Scalability;
        // ---- Quality levels (Settings/Scalability.h) ----
        // Each group's level is a player row: choosing one (its combo, or Overall
        // for every group) lets go of the group's overrides and rewrites the rows'
        // Scalability layer (EditPlayerRow -> RecordPlayerChoice); a row changed
        // below afterwards is an override, and the group reads custom.
        // engine/scalability.json says which rows each group sets.
        EditorTheme::Heading("Quality Levels");
        {
            const int overall = Sc::Overall();
            bool anyCustom = false;
            for (size_t g = 0; g < Sc::kGroupCount; ++g) anyCustom = anyCustom || Sc::Custom(g);
            char preview[48];
            std::snprintf(preview, sizeof(preview), "%s%s", overall < 0 ? "Custom" : Sc::LevelName(overall),
                          (overall >= 0 && anyCustom) ? "  (custom)" : "");
            if (ImGui::BeginCombo("Overall Quality", preview))
            {
                for (int l = 0; l < Sc::kLevelCount; ++l)
                    if (ImGui::Selectable(Sc::LevelName(l), overall == l && !anyCustom))
                        for (size_t g = 0; g < Sc::kGroupCount; ++g)
                            if (const Settings::Row* lr = Sc::LevelRow(g))
                                playerSet(lr->path, Settings::Value{ .i = l });
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Every group at one level: their rows follow it and the rows changed on their own "
                                  "go back to it. High is the defaults; Low / Medium are cheaper, Ultra / Cinematic "
                                  "richer. The lighting tier (Ray Tracing) is separate.");
        }
        if (!Sc::Loaded())
            ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f), "%s was refused (%s): the levels set no rows.",
                               Sc::Source().c_str(), Sc::RefusalReason().c_str());
        const Settings::SettingsStore& st = Settings::SettingsStore::Get();
        for (size_t g = 0; g < Sc::kGroupCount; ++g)
        {
            const Settings::Row* lr = Sc::LevelRow(g);
            if (!lr || (Sc::Loaded() && Sc::Rows(g).empty())) continue;
            player(lr->path);
            if (!Sc::Custom(g)) continue;
            // Which rows do not run their level's value, and who holds them.
            ImGui::SameLine();
            ImGui::TextDisabled("(custom)");
            if (ImGui::IsItemHovered())
            {
                std::string tip = "Rows off this level's values:";
                const int level = Sc::Level(g);
                for (const Sc::Entry& e : Sc::Rows(g))
                {
                    const Settings::Value& v = st.OwnerValue(*e.row);
                    if (Settings::SameValue(*e.row, v, e.values[level])) continue;
                    tip += std::string("\n  ") + e.row->path + " = " + Settings::ValueText(*e.row, v) + "  (" +
                           Settings::LayerName(st.Source(*e.row)) + "; the level gives " +
                           Settings::ValueText(*e.row, e.values[level]) + ")";
                }
                ImGui::SetTooltip("%s", tip.c_str());
            }
        }

        // ---- Texture filtering ----
        // The restart rows take effect at the next launch (kSettingRestart):
        // material samplers and the shadow atlases are created at startup.
        EditorTheme::Heading("Texture Filtering");
        player("textures.anisotropy");

        // ---- Shadows ----
        EditorTheme::Heading("Shadows");
        player("shadows.csmResolution");
        player("shadows.distance");
        player("shadows.spotMaxResolution");
        player("shadows.pointResolution");

        // ---- Ambient occlusion and geometry ----
        EditorTheme::Heading("Ambient Occlusion");
        player("features.aoQuality");
        EditorTheme::Heading("Geometry");
        player("geometry.lodScale");
    };

    auto pageRayTracing = [&]()
    {
        // ---- Ray Tracing ----
        // Every DXR-backed feature in one place: they share a hardware gate, the TLAS
        // and a GPU budget. The RT budget watchdog is deliberately not here: it is a
        // developer diagnostic (Developer page), not a quality setting.
        //
        // The render path and the RT features are settings.json rows; the ReSTIR /
        // PT sections and the RT internals are engine.json's.
        const bool inlineRT = m_gfx->GetCapabilities().inlineRaytracing;

        // ---- Lighting tier (Settings/LightingTier.h) ----
        // Outside the RT-only block: T0 needs no ray tracing, the tiers above it are greyed out without.
        LightingTierCombo(*m_renderer, inlineRT, playerSet);

        ImGui::BeginDisabled(!inlineRT);
        player("features.renderPath");
        // The selected path's ReSTIR options open here: the backend each stage
        // resamples with, its live GPU cost, presets, the temporal / spatial split
        // and the cost knobs. They are developer options (engine.json, the buttons
        // at the end of the section); the Developer page keeps the budget guard and
        // compare.
        const int renderPath = static_cast<int>(m_renderer->GetRenderPathRequested());
        const bool diPath = renderPath == static_cast<int>(Renderer::RenderPath::ReSTIR_DI)
                         || renderPath == static_cast<int>(Renderer::RenderPath::ReSTIR_DI_GI);
        const bool giPath = renderPath == static_cast<int>(Renderer::RenderPath::ReSTIR_GI)
                         || renderPath == static_cast<int>(Renderer::RenderPath::ReSTIR_DI_GI);
        const bool ptPath = Renderer::IsPathTrace(static_cast<Renderer::RenderPath>(renderPath));
        restirCostOnScreen = diPath || giPath || ptPath;
        if (restirCostOnScreen) s_restirCost.Sample(m_gpuProfiler);
        Renderer& r = *m_renderer;
        if (diPath)
        {
            EditorTheme::Heading("ReSTIR DI");
            dev("raytracing.restirBackendDI", [&](const Settings::DevRow& row, Settings::Value& v)
                { return RestirBackendCombo(r, ReSTIR::Stage::DI, row, v); });
            RestirCostLine("RTDI", { "RTDI.RELAX", "RTDI.REBLUR" });
            RestirPresetRow(kDIPresets,
                [&](const DIPreset& p) { return r.GetRestirDIReuse() && r.GetRestirDICandidates() == p.candidates
                    && r.GetRestirDISpatialPasses() == p.spatialPasses && r.GetRestirDIK() == p.spatialK
                    && r.GetRestirDIFullRes() == p.fullRes; },
                [&](const DIPreset& p) {
                    devSet("raytracing.restirDiReuse",         Settings::Value{ .b = true });
                    devSet("raytracing.restirDiCandidates",    Settings::Value{ .i = static_cast<int>(p.candidates) });
                    devSet("raytracing.restirDiSpatialPasses", Settings::Value{ .i = static_cast<int>(p.spatialPasses) });
                    devSet("raytracing.restirDiK",             Settings::Value{ .i = static_cast<int>(p.spatialK) });
                    devSet("raytracing.restirDiFullRes",       Settings::Value{ .b = p.fullRes }); });
            dev("raytracing.restirDiReuse");
            const bool temporal = r.GetRestirDIReuse();
            ImGui::SameLine();
            ImGui::BeginDisabled(!temporal);
            // Spatial reuse is "spatial passes > 0": the box writes that row,
            // restoring the last count it had.
            static uint32_t s_diPasses = 1;
            if (r.GetRestirDISpatialPasses() > 0) s_diPasses = r.GetRestirDISpatialPasses();
            dev("raytracing.restirDiSpatialPasses", [&](const Settings::DevRow& row, Settings::Value& v)
            {
                bool spatial = v.i > 0;
                const bool changed = ImGui::Checkbox(("DI Spatial Reuse##" + std::string(row.path)).c_str(), &spatial);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Neighbour reuse after the temporal merge (Spatial Passes > 0).\nNeeds temporal reuse.");
                if (changed) v.i = spatial ? static_cast<int>(s_diPasses) : 0;
                return changed;
            });
            ImGui::EndDisabled();
            dev("raytracing.restirDiCandidates");
            dev("raytracing.restirDiFullRes");
            if (r.IsRayReconstructionActive() && r.GetRestirDIDenoiser() == ReSTIR::Denoiser::Auto)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("(always on under Ray Reconstruction)");
            }
            dev("raytracing.restirDiDenoiser", [&](const Settings::DevRow& row, Settings::Value& v)
                { return RestirDenoiserCombo(r, ReSTIR::Stage::DI, row, v); });
            dev("raytracing.restirDiSpecAntiFirefly");
            if (ImGui::TreeNode("DI Advanced"))
            {
                ImGui::BeginDisabled(!temporal);
                dev("raytracing.restirDiSpatialPasses");
                dev("raytracing.restirDiSpatialRadiusPx");
                dev("raytracing.restirDiK");
                dev("raytracing.restirDiMCap");
                dev("raytracing.restirDiAgeCap");
                ImGui::EndDisabled();
                dev("raytracing.diRayTMax");
                ImGui::TreePop();
            }
        }
        if (giPath)
        {
            EditorTheme::Heading("ReSTIR GI");
            dev("raytracing.restirBackendGI", [&](const Settings::DevRow& row, Settings::Value& v)
                { return RestirBackendCombo(r, ReSTIR::Stage::GI, row, v); });
            RestirCostLine("RTGI", { "RTGI.Trace", "RTGI.RELAX", "RTGI.REBLUR", "RTGI.Sharc.Update" });
            RestirPresetRow(kGIPresets,
                [&](const GIPreset& p) { return r.GetRestirGIReuse() && r.GetRestirGISpatial()
                    && r.GetRestirGIInitialCandidates() == p.initial && r.GetRestirGIEmitterCandidates() == p.emitter
                    && r.GetRestirGIHitExactLights() == p.exact; },
                [&](const GIPreset& p) {
                    devSet("raytracing.restirGiReuse",             Settings::Value{ .b = true });
                    devSet("raytracing.restirGiSpatial",           Settings::Value{ .b = true });
                    devSet("raytracing.restirGiInitialCandidates", Settings::Value{ .i = static_cast<int>(p.initial) });
                    devSet("raytracing.restirGiEmitterCandidates", Settings::Value{ .i = static_cast<int>(p.emitter) });
                    devSet("raytracing.restirGiHitExactLights",    Settings::Value{ .i = static_cast<int>(p.exact) }); });
            dev("raytracing.restirGiReuse");
            const bool temporal = r.GetRestirGIReuse();
            ImGui::SameLine();
            ImGui::BeginDisabled(!temporal);
            dev("raytracing.restirGiSpatial");
            ImGui::EndDisabled();
            dev("raytracing.restirGiInitialCandidates");
            dev("raytracing.restirGiEmitterCandidates");
            dev("raytracing.restirGiHitExactLights");
            dev("raytracing.restirGiFullRes");
            dev("raytracing.restirGiDenoiser", [&](const Settings::DevRow& row, Settings::Value& v)
                { return RestirDenoiserCombo(r, ReSTIR::Stage::GI, row, v); });
            // SHaRC radiance cache at the GI hits.
            const Renderer::GISharcStatus sharcStatus = r.GetRestirGISharcStatus();
            dev("raytracing.restirGiSharc");
            const bool sharc = r.GetRestirGISharc();
            if (sharc && !sharcStatus.effective)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("(not running: %s)", sharcStatus.reason ? sharcStatus.reason : "?");
            }
            // ReSTIR GI is the diffuse bounce (albedo x (1 - metal)); the traced
            // indirect specular is RT Reflections'. Without RT Reflections, metal and
            // glossy surfaces keep SSR / probes only: the main gap to the path tracer
            // (SchoolNight, two views: -17% / -24% with them off, -7% / -10% on).
            if (!r.IsRTReflectionsEnabled())
            {
                ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f), "RT Reflections are off:");
                ImGui::SameLine();
                if (ImGui::SmallButton("Enable##giRtr")) playerSet("features.rtReflections", Settings::Value{ .b = true });
                ImGui::TextDisabled("  metal / glossy surfaces get no traced indirect specular\n"
                                    "  (the path tracer has it): the main gap to Path Trace.");
            }
            if (ImGui::TreeNode("GI Advanced"))
            {
                ImGui::BeginDisabled(!sharc);
                dev("raytracing.restirGiSharcMode", [&](const Settings::DevRow& row, Settings::Value& v)
                {
                    int mode = v.s == "multi" ? 1 : 0;
                    const bool changed = ImGui::Combo(EditorUI::SettingLabel(row).c_str(), &mode,
                                                      "Parity (Tier 1 expectation)\0Multi-bounce\0");
                    EditorUI::SettingNotes(row);
                    if (changed) v.s = mode == 1 ? "multi" : "parity";
                    return changed;
                });
                ImGui::EndDisabled();
                ImGui::BeginDisabled(!temporal);
                dev("raytracing.restirGiSpatialRadiusPx");
                dev("raytracing.restirGiAgeCap");
                ImGui::EndDisabled();
                dev("raytracing.restirGiMaxDistance");
                // The radiance cache holds Tier 1 shading: while it is requested the GI tier
                // is Tier 1, so the combo is disabled.
                ImGui::BeginDisabled(sharc);
                dev("raytracing.restirGiTier", [&](const Settings::DevRow& row, Settings::Value& v)
                {
                    const bool changed = ImGui::Combo(EditorUI::SettingLabel(row).c_str(), &v.i,
                                                      "Tier 0: coarse diffuse\0Tier 1: local shadows\0");
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip(sharc ? "Tier 1 while the radiance cache is on (it holds Tier 1 shading)."
                                                : "Tier 0 leaks light through walls: hit lights are unshadowed.");
                    return changed;
                });
                ImGui::EndDisabled();
                ImGui::TreePop();
            }
        }
        // Real-time and reference path tracing are two render paths, each with
        // its own options. The core each runs is the path's (PT backend 'auto');
        // an explicit backend stays a developer override.
        if (renderPath == static_cast<int>(Renderer::RenderPath::PathTraceRealtime))
        {
            EditorTheme::Heading("Path Trace (Real-time)");
            RestirCostLine("PT.Trace", {});
            const ReSTIR::BackendStatus status = r.GetRestirBackendStatus(ReSTIR::Stage::PT);
            ImGui::TextDisabled("Core: %s (%s)", status.effectiveName, status.reason);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("RTXDI's ReSTIR PT (ReSTIR_Dual_Backend_Plan.md S9): the SHaRC radiance cache ends paths\n"
                                  "at rough surfaces, 5 vertices, x1's lights from its own next-event sample; a fresh\n"
                                  "estimate every frame. Glass reflections and toon (NPR) characters are path traced\n"
                                  "too. A command-line pin (-pt_bounces, -pt_area_*, -pt_lens_radius) sends it to the\n"
                                  "engine path tracer, and the reason says so.");
            if (!r.IsRayReconstructionActive())
            {
                ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f), "Ray Reconstruction is off:");
                ImGui::TextDisabled("  real-time PT is made for it - AA DLSS with Ray Reconstruction ticked\n"
                                    "  (Render Scale Quality, RR Model F). Without it the frames accumulate\n"
                                    "  while the camera is still, and noise shows while it moves.");
            }
            // Where x1's direct light comes from.
            dev("raytracing.ptDirectLight", [&](const Settings::DevRow& row, Settings::Value& v)
            {
                static constexpr const char* kDirectLights[] = { "Auto (ReSTIR DI)", "Path NEE", "ReSTIR DI", "Unified reservoir" };
                static_assert(std::size(kDirectLights) == static_cast<size_t>(PTRtxdi::DirectLight::Count));
                PTRtxdi::DirectLight d;
                if (!PTRtxdi::ParseDirectLight(v.s.c_str(), d)) d = PTRtxdi::DirectLight::Auto;
                int direct = static_cast<int>(d);
                const bool changed = ImGui::Combo(EditorUI::SettingLabel(row).c_str(), &direct,
                                                  kDirectLights, IM_ARRAYSIZE(kDirectLights));
                EditorUI::SettingNotes(row);
                if (changed) v.s = PTRtxdi::DirectLightName(static_cast<PTRtxdi::DirectLight>(direct));
                return changed;
            });
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", PTRtxdi::DirectLightName(PTRtxdi::EffectiveDirectLight()));
            if (ImGui::TreeNode("PT Developer##realtime"))
            {
                dev("raytracing.restirBackendPT", [&](const Settings::DevRow& row, Settings::Value& v)
                    { return RestirBackendCombo(r, ReSTIR::Stage::PT, row, v); });
                ImGui::TreePop();
            }
        }
        if (renderPath == static_cast<int>(Renderer::RenderPath::PathTrace))
        {
            EditorTheme::Heading("Path Trace (Reference)");
            RestirCostLine("PT.Trace", {});
            const ReSTIR::BackendStatus status = r.GetRestirBackendStatus(ReSTIR::Stage::PT);
            ImGui::TextDisabled("Core: %s (%s), %u spp", status.effectiveName, status.reason, r.GetPathTraceSamples());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The engine path tracer, the ground truth: it accumulates while the camera and the\n"
                                  "scene are still (samples per pixel above) and restarts on any change. No Ray\n"
                                  "Reconstruction (it converges instead): Path Trace (Real-time) is the RR path.");
            // Time-driven content (a playing animation, wind, clouds, time of day) restarts the
            // image every frame: the freeze stops it so the image converges.
            dev("raytracing.ptFreezeTime");
            if (r.IsWorldClockFrozen())
            {
                ImGui::SameLine();
                ImGui::TextDisabled("(frozen, %u steps)", r.GetWorldStepsDone());
            }
            dev("raytracing.ptDenoise");
            ImGui::BeginDisabled(!r.GetPTDenoise());
            dev("raytracing.ptDenoiseFadeSpp");
            ImGui::EndDisabled();
            dev("raytracing.ptReferenceArm");
            ImGui::BeginDisabled(r.GetPTReferenceArm());
            dev("raytracing.ptMaxBounces");
            ImGui::EndDisabled();
            // Renderer::PTCameraModesHeld: these are live on this path only.
            dev("raytracing.ptAreaMode", [&](const Settings::DevRow& row, Settings::Value& v)
            {
                static const char* kAreaModes[] = { "Off", "Temporal", "Temporal + spatial" };
                const bool changed = ImGui::Combo(EditorUI::SettingLabel(row).c_str(), &v.i, kAreaModes, IM_ARRAYSIZE(kAreaModes));
                EditorUI::SettingNotes(row);
                return changed;
            });
            const int areaMode = static_cast<int>(r.GetPTAreaMode());
            if (areaMode > 0)
            {
                ImGui::TextDisabled("Area enables camera rays and film jitter.");
                dev("raytracing.ptAreaHistory");
                dev("raytracing.ptAreaReconnect");
                if (areaMode == 2)
                {
                    dev("raytracing.ptAreaSpatialK");
                    dev("raytracing.ptAreaSpatialRadius");
                    ImGui::TextDisabled("Each neighbour costs two path traces per pixel.");
                }
            }
            dev("raytracing.ptCameraPrimary");
            dev("raytracing.ptLensRadius");
            ImGui::BeginDisabled(!r.GetPTCameraPrimary() && r.GetPTLensRadius() <= 0 && areaMode == 0);
            dev("raytracing.ptFocusDistance");
            dev("raytracing.ptFilmJitter");
            dev("raytracing.ptPrimaryRayCone");
            ImGui::EndDisabled();
            if (ImGui::TreeNode("PT Developer##reference"))
            {
                dev("raytracing.restirBackendPT", [&](const Settings::DevRow& row, Settings::Value& v)
                    { return RestirBackendCombo(r, ReSTIR::Stage::PT, row, v); });
                ImGui::TreePop();
            }
        }
        if (restirCostOnScreen)
        {
            // The sections above are engine.json (developer) settings, not
            // settings.json: the footer's Save does not write them.
            if (ImGui::SmallButton("Save to engine.json##restir")) saveEngineJson();
            ImGui::SameLine();
            if (ImGui::SmallButton("Reload engine.json##restir")) reloadEngineJson();
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("ReSTIR and Path Trace options are developer settings in engine.json\n"
                                  "(the same buttons as the Developer page), not player options.");
            ImGui::Separator();
        }

        player("features.rtReflections");
        if (m_renderer->IsRTReflectionsEnabled())
        {
            dev("raytracing.reflectionsMaxRoughness");
            player("features.rtReflectionsRayTMax");
            dev("raytracing.reflectionsUseNRD");
        }
        player("features.rtShadows");
        if (m_renderer->IsRTShadowsEnabled())
        {
            ImGui::Indent();
            dev("raytracing.shadowBlendOccluders");
            ImGui::Unindent();
        }

        // RTAO is a SUB-MODE of the AO feature rather than a standalone one: it
        // swaps XeGTAO's occlusion source for TLAS rays and leaves the denoise
        // chain alone, so it is meaningless with AO off. The gate is kept and
        // stated rather than hidden, so an unavailable checkbox explains itself.
        if (m_renderer->IsSSAOEnabled())
        {
            ImGui::Indent();
            player("features.rtao");
            ImGui::Unindent();
        }
        else
            ImGui::TextDisabled("Ray-Traced AO needs Ambient Occlusion enabled (Render Features)");

        ImGui::EndDisabled();
        if (!inlineRT) ImGui::TextDisabled("Requires DXR 1.1 inline ray tracing");
    };

    auto pageFeatures = [&]()
    {
        // ---- Render features ----
        // AO, fog, DDGI and SSGI show the player's allowance: what their rows
        // capture and "Save to settings.json" writes. What runs is that AND the
        // scene's intent (Settings.where shows both layers); a line under each says
        // when the current scene leaves it off.
        player("features.ssao");
        if (m_renderer->GetSSAOAuthored() && !m_renderer->GetSSAOSceneIntent())
            ImGui::TextDisabled("  the current scene's .ippc leaves AO off");
        player("features.ssr");
        player("features.volumetricFog");
        if (m_renderer->GetVolumetricFogAuthored() && !m_renderer->GetSceneFogIntent())
            ImGui::TextDisabled("  the current scene has no fog (no Volumetric Fog component,\n"
                                "  local fog volume or .ippc fog)");
        player("features.ddgi");
        if (m_renderer->GetDDGIAuthored() && !m_renderer->GetSceneDDGIIntent())
            ImGui::TextDisabled("  the current scene's Indirect Lighting leaves DDGI off");
        player("features.ssgi");
        if (m_renderer->GetSSGIAuthored() && !m_renderer->GetSceneSSGIIntent())
            ImGui::TextDisabled("  the current scene does not use SSGI (its Indirect Lighting)");
    };

    auto pagePostProcess = [&]()
    {
        // ---- Post-process (global default profile; scenes / volumes override) ----
        // Each toggle is a postProcess.* row: it shows the authored mirror (what the
        // settings save reads) and writes the live base layer and the mirror
        // together (the row's apply); a scene .ippc can replace the live layer
        // wholesale. Do not copy the live layer into the mirror here: a scene's
        // choice would leak into the save.
        player("postProcess.cas");
        player("postProcess.dof");
        player("postProcess.lensFlare");
        player("postProcess.autoExposure");   // the scene author's: the in-game menu has no switch

        // Live exposure readout, so metering is tuned against numbers and not only
        // the final image. The GPU readback is enabled only while this panel draws
        // it.
        if (AutoExposurePass* ae = m_renderer->GetAutoExposurePass())
        {
            EditorUI::DrawExposureReadout(*ae, PostProcess::ProfileSystem::Get().EngineDefault().autoExposure.enabled.value);
        }

        player("postProcess.colorGrading");
        ImGui::TextDisabled("  Base look only — a scene's .ippc profile or a\n"
                            "  PostProcessVolume still overrides these on top.");
    };

    auto pageInput = [&]()
    {
        ImGui::TextWrapped("Action / axis bindings for keyboard, mouse and gamepad live in "
                           "their own window: it rebinds by capture, shows live pad "
                           "diagnostics and saves input_profiles.json.");
        ImGui::Spacing();
        if (ImGui::Button("Open Input Bindings..."))
            *Editor::InputBindingsWindow::Get().OpenFlag() = true;
    };

    auto pageDeveloper = [&]()
    {
        // ---- Developer (engine tuning, not player options) ----
        // Everything below changes how the renderer works rather than how it looks;
        // none of it belongs in a shipped options menu (a player cannot judge GPU
        // culling or meshlet LOD error, and a wrong answer costs frames or
        // correctness with no visible cause).
        //
        // Each control is an engine.json row (Settings/Rows/Developer.inl holds its
        // title, tooltip and range). A change is live at once and recorded in the
        // Runtime layer; "Save to engine.json" below writes what runs.
        EditorTheme::Heading("Geometry / Culling");
        dev("culling.gpuCulling");
        dev("culling.indirectDraw");
        dev("culling.gpuCompaction");
        dev("culling.hiZOcclusion");
        dev("culling.animCulling");
        // Not settings: the IK systems' live switches.
        if (IKSystem* ik = m_renderer->GetIKSystem())
        {
            bool b = ik->IsEnabled();
            if (ImGui::Checkbox("MMD IK (evaluation)", &b)) ik->SetEnabled(b);
        }
        if (IKRigSystem* ik = m_renderer->GetIKRigSystem())
        {
            bool b = ik->IsEnabled();
            if (ImGui::Checkbox("Procedural IK", &b)) ik->SetEnabled(b);
        }

        EditorTheme::Heading("Meshlets (mesh shaders)");
        dev("culling.meshShading");
        dev("culling.meshletLodError");
        dev("culling.meshletShadowLodError");

        EditorTheme::Heading("Performance Experiments");
        dev("perf.depthPrepass");
        if (m_renderer->IsDepthPrepassEnabled())
            ImGui::TextDisabled("   covered %u draws last frame", m_renderer->GetDepthPrepassDrawCount());
        dev("perf.perObjectShadow");

        EditorTheme::Heading("Ray Tracing Diagnostics");
        dev("raytracing.budgetGuard");
        if (m_renderer->IsRTBudgetGuardEnabled())
            dev("raytracing.budgetMs");
        dev("raytracing.restirBudgetMs");
        ImGui::TextDisabled("ReSTIR budget stage: %u / 4", m_renderer->GetRestirBudgetStage());
        // Compare: the other core of a stage as a shadow pass in the same frame.
        // A compare this build cannot run (no second core) is listed, disabled,
        // with why.
        dev("raytracing.restirCompare", [&](const Settings::DevRow& row, Settings::Value& v)
        {
            bool changed = false;
            if (ImGui::BeginCombo(EditorUI::SettingLabel(row).c_str(), v.s.c_str()))
            {
                for (uint8_t i = 0; i < static_cast<uint8_t>(ReSTIR::Compare::Count); ++i)
                {
                    const auto c = static_cast<ReSTIR::Compare>(i);
                    const ReSTIR::Stage stage = c == ReSTIR::Compare::DI ? ReSTIR::Stage::DI : ReSTIR::Stage::GI;
                    const char* why = c == ReSTIR::Compare::Off ? nullptr
                        : m_renderer->UnavailableRestirBackendReason(stage, ReSTIR::Backend::Rtxdi);
                    const char* name = ReSTIR::CompareName(c);
                    ImGui::BeginDisabled(why != nullptr);
                    if (ImGui::Selectable(name, v.s == name) && v.s != name)
                    {
                        v.s = name;
                        changed = true;
                    }
                    ImGui::EndDisabled();
                    if (why && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("Needs the stage's second core: %s.", why);
                }
                ImGui::EndCombo();
            }
            EditorUI::SettingNotes(row);
            return changed;
        });
        if (m_renderer->GetRestirCompare() != m_renderer->GetRestirCompareEffective())
            ImGui::TextDisabled("  compare off: %s", m_renderer->GetRestirCompareReason());
        ImGui::TextDisabled("ReSTIR DI / GI options: Ray Tracing page (with a ReSTIR render path).");
        dev("materialSurfaceCache.enabled");
        if (m_renderer->IsMaterialSurfaceCacheEnabled())
        {
            ImGui::Indent();
            dev("materialSurfaceCache.pagesPerFrame");
            if (const auto* sc = m_renderer->GetMaterialSurfaceCache())
                ImGui::TextDisabled("  %u/%u pages used, %u baked, %u pending",
                                    sc->GetPagesInUse(), MaterialSurfaceCache::kPageCount,
                                    sc->GetPagesBaked(), sc->GetPendingBakes());
            ImGui::Unindent();
        }

        // engine.json, not settings.json: its own buttons (see saveEngineJson).
        EditorTheme::Heading("engine.json");
        if (ImGui::Button("Save to engine.json")) saveEngineJson();
        ImGui::SameLine();
        if (ImGui::Button("Reload engine.json")) reloadEngineJson();
        ImGui::TextDisabled("  Developer config — committed and sealed into game.ipak.\n"
                            "  Not player options; a shipped build reads it and never writes it.");
    };

    auto pagePasses = [&]()
    {
        // ---- Pass tuning ----
        // The render passes' tuning parameters: the pass rows
        // (Settings/Rows/Passes.inl), one tree per pass. A change is recorded in the
        // Runtime layer and is what the pass reads next; nothing saves them - a run
        // sets them with -set:pass.<...>=<v> or the pass's own flag.
        ImGui::TextDisabled("Engine tuning for one session: never saved. Per run: -set:pass.<pass>.<field>=<v>.");
        std::string open;
        bool        shown = false;
        for (const Settings::PassRow& r : Settings::PassRows())
        {
            const std::string pass = r.key.substr(0, r.key.find('.'));
            if (pass != open)
            {
                if (shown) ImGui::TreePop();
                open  = pass;
                shown = ImGui::TreeNodeEx(pass.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
            }
            if (shown) EditorUI::PassSetting(r);
        }
        if (shown) ImGui::TreePop();
    };

    // ---- Category list ----
    // Keywords are what the search box matches besides the title, so typing a
    // setting's name ("vsync", "rtao", "anisotropic") finds its page.
    struct Page
    {
        const char*           title;
        const char*           keywords;
        std::function<void()> draw;
    };
    const Page pages[] = {
        { "Display",               "vsync gsync g-sync vrr frame rate fps cap limit fullscreen refresh hz "
                                   "hdr hdr10 scrgb sdr output paper white peak nits monitor", pageDisplay },
        { "Upscaling & AA",        "anti-aliasing aa fxaa taa fsr dlss dlaa render scale resolution "
                                   "sharpen sharpness model preset reflex latency frame generation fg "
                                   "mfg streamline history ghosting jitter subpixel edge threshold "
                                   "outline stencil", pageUpscaling },
        { "Quality",               "quality level scalability overall preset low medium high ultra cinematic custom "
                                   "texture filtering anisotropic trilinear shadow csm cascade resolution distance spot "
                                   "point cube ambient occlusion xegtao geometry lod", pageQuality },
        { "Ray Tracing",           "rt rtr reflections roughness ray length nrd reblur denoiser sun "
                                   "shadows translucent rtao ambient occlusion dxr render path restir "
                                   "di gi reuse temporal spatial candidates backend rtxdi preset "
                                   "path trace pt area lens focus", pageRayTracing },
        { "Rendering Features",    "ssao ao xegtao ambient occlusion ssr screen-space reflections "
                                   "volumetric fog froxel", pageFeatures },
        { "Post-Process Defaults", "cas sharpening dof depth of field lens flare auto exposure "
                                   "eye adaptation color grading profile", pagePostProcess },
        { "Input",                 "bindings keyboard mouse gamepad controller xbox rebind actions", pageInput },
        { "Developer",             "gpu culling frustum indirect draw compaction hi-z occlusion animation "
                                   "culling ik solver depth prepass per-object player shadow rt budget "
                                   "watchdog restir budget compare material surface cache engine.json", pageDeveloper },
        { "Pass Tuning",           "pass passes tuning knobs flags command line spot shadow mixed cache dlss rr "
                                   "ray reconstruction exposure normal tag", pagePasses },
    };
    constexpr int kPageCount = static_cast<int>(sizeof(pages) / sizeof(pages[0]));
    static int  s_page = 0;
    static char s_search[64] = {};
    static bool s_modifiedOnly = false;
    // The search results page: every row of the three stores whose title, path or tooltip matches.
    constexpr int kResultsPage = -1;
    // Harness: -ui_open_menu=Project Settings@N,Project Settings/<Page title>@N+1
    // selects a page for a screenshot (the window stays open once opened).
    for (int i = 0; i < kPageCount; ++i)
        if (HarnessMenuHeldOpen((std::string("Project Settings/") + pages[i].title).c_str()))
            s_page = i;

    // Footer = one row of buttons; the two panes take everything above it.
    const float footerH = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y + 1.0f;
    const float navW    = 200.0f;

    ImGui::BeginChild("##settingsNav", ImVec2(navW, -footerH), ImGuiChildFlags_Borders);
    {
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##settingsSearch", "Search settings", s_search, sizeof(s_search));
        ImGui::Checkbox("Modified only", &s_modifiedOnly);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("Lists the settings that differ from their default.");
        ImGui::Spacing();
        // The rows themselves: a search or Modified only lists every matching row on one page.
        const bool rowSearch = s_search[0] != '\0' || s_modifiedOnly;
        if (rowSearch && ImGui::Selectable("Matching settings", s_page == kResultsPage)) s_page = kResultsPage;
        if (!rowSearch && s_page == kResultsPage) s_page = 0;
        int firstMatch = -1;
        int clicked = -1;
        bool currentMatches = s_page == kResultsPage;
        for (int i = 0; i < kPageCount; ++i)
        {
            const bool match = ContainsNoCase(pages[i].title, s_search)
                            || ContainsNoCase(pages[i].keywords, s_search);
            if (!match) continue;
            if (firstMatch < 0) firstMatch = i;
            if (i == s_page) currentMatches = true;
            if (ImGui::Selectable(pages[i].title, s_page == i))
                clicked = i;
        }
        // Apply the click after the loop: assigned inside it, clicking a page
        // above the open one fails the "open page still listed" test
        // (currentMatches) and the fallback below jumps to the first page.
        if (clicked >= 0) s_page = clicked;
        // A search that hides the open page moves to the first page it kept.
        else if (!currentMatches && firstMatch >= 0) s_page = firstMatch;
        if (firstMatch < 0) ImGui::TextDisabled("No setting matches");
    }
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("##settingsPage", ImVec2(0.0f, -footerH), ImGuiChildFlags_Borders);
    {
        EditorTheme::PushBold();
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.25f);
        ImGui::TextUnformatted(s_page == kResultsPage ? "Matching settings" : pages[s_page].title);
        ImGui::PopFont();
        EditorTheme::PopBold();
        ImGui::Separator();
        if (s_page == kResultsPage)
        {
            EditorUI::RowFilter filter;
            filter.text = s_search;
            filter.modifiedOnly = s_modifiedOnly;
            EditorUI::SetActiveRowFilter(&filter);
            SettingsRows rows(*this);
            // The source answers the text; each row's control also applies Modified only (its test
            // needs the live value).
            const std::size_t drawn = EditorUI::DrawPropertySource(rows, &filter);
            EditorUI::SetActiveRowFilter(nullptr);
            if (drawn == 0) ImGui::TextDisabled("No setting row matches.");
        }
        else
            pages[s_page].draw();
    }
    ImGui::EndChild();
    SyncRestirCostWant(restirCostOnScreen);

    // ---- Persistence footer (settings.json) ----
    if (ImGui::Button("Save to settings.json"))
        Resource::SaveCapturedEngineSettings(*m_renderer, *m_gfx, m_window);
    ImGui::SameLine();
    // Both replace the player's own values in the settings store
    // (Settings/SettingsStore.h) - Load & Apply with what the file holds, Reset
    // with nothing - and push every row as the store then has it. Do not write
    // every row into the Player layer: a value from another layer could never
    // show through again.
    if (ImGui::Button("Load & Apply"))
    {
        Resource::EngineSettings s;
        Resource::SettingsLoadInfo info;
        Resource::LoadEngineSettings(Resource::SettingsFilePath(), s, &info);
        Resource::RecordLoadedPlayerRows(s, info, "Editor: Load & Apply", /*replace*/true);
        Resource::ProjectStoreValues(s);
        s.ApplyTo(*m_renderer, *m_gfx, m_window);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset to Defaults"))
    {
        Settings::SettingsStore& st = Settings::SettingsStore::Get();
        for (const auto& r : Settings::PlayerRows())
            st.Clear(r, Settings::Layer::Player, "Editor: Reset to Defaults");
        Resource::EngineSettings s;
        Resource::ProjectStoreValues(s);
        s.ApplyTo(*m_renderer, *m_gfx, m_window);
    }

    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("settings.json holds the PLAYER options on every page except\n"
                          "Developer and the Ray Tracing page's ReSTIR / Path Trace sections\n"
                          "(engine.json, their own buttons). Widgets act on the live renderer\n"
                          "immediately; Save writes them, Load & Apply reads the file back,\n"
                          "Reset applies the compiled-in defaults.");

    ImGui::End();
}
