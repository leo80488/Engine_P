#include "Renderer/RenderPathFlags.h"

#include "Renderer/Renderer.h"
#include "RenderGraph/RenderPass/ReSTIRBackend.h"
#include "Settings/SettingsStore.h"
#include "System/CommandLine.h"
#include "System/Check.h"
#include "System/Log.h"
#include "Harness/ExitCodes.h"

#include <ctime>
#include <string>

namespace
{
    // Set the path, and record the write in the settings store's CommandLine layer
    // (row features.renderPath) under the flag's name.
    void SetPathFromFlag(Renderer& renderer, Renderer::RenderPath path, const char* flag)
    {
        renderer.SetRenderPath(path);
        if (const Settings::Row* row = Settings::FindPlayerRow("features.renderPath"))
        {
            Settings::Value v;
            v.i = static_cast<int>(path);
            Settings::SettingsStore::Get().Write(*row, Settings::Layer::CommandLine, v, flag);
        }
    }
}

// Apply CLI render-path overrides before Lua captures the settings snapshot.
void ApplyRenderPathFlags(Renderer& renderer)
{
    if (Sys::CommandLine::Get().Has("-pathtrace")) SetPathFromFlag(renderer, Renderer::RenderPath::PathTrace, "-pathtrace");
    if (Sys::CommandLine::Get().Has("-gipath"))
    {
        const auto token = Sys::CommandLine::Get().Get("-gipath", "");
        Renderer::RenderPath path;
        if (!Renderer::ParseRenderPath(token, path))
        {
            LOG_ERROR("Invalid -gipath=%s; expected raster|restir_di|restir_gi|restir|pathtrace|pathtrace_realtime", token.c_str());
            PostQuitMessage(Harness::PreconditionFail);
        }
        else SetPathFromFlag(renderer, path, "-gipath");
    }
    // -pt_realtime=0|1 picks between the two path-tracing paths when one is selected -
    // by settings.json, -pathtrace or -gipath. Read unconditionally (unreadFlags).
    if (Sys::CommandLine::Get().Has("-pt_realtime"))
    {
        const bool realtime = Sys::CommandLine::Get().GetInt("-pt_realtime", 0) != 0;
        if (Renderer::IsPathTrace(renderer.GetRenderPathRequested()))
            SetPathFromFlag(renderer, realtime ? Renderer::RenderPath::PathTraceRealtime : Renderer::RenderPath::PathTrace,
                            "-pt_realtime");
        else
            LOG_WARNING("-pt_realtime=%d: the render path is not a path-tracing one; ignored", realtime ? 1 : 0);
    }
}

// A core past its ReSTIR::RetirementDate that the editor menu still offers is a missed step of the retirement
// plan: one WARNING at boot. An ERROR would fail every pack that holds the run to zero errors on the day; the
// pack restir_retirement_deadline runs on the real date and is the one that turns red. -restir_today=YYYY-MM-DD
// (harness) stands in for today, for the packs.
static void CheckRetirementDeadlines()
{
    int today = 0;
#if WITH_HARNESS
    if (Sys::CommandLine::Get().Has("-restir_today"))
        today = ReSTIR::DateNumber(Sys::CommandLine::Get().Get("-restir_today", "").c_str());
#endif
    if (today == 0)
    {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_s(&local, &now);
        today = (local.tm_year + 1900) * 10000 + (local.tm_mon + 1) * 100 + local.tm_mday;
    }
    const std::string overdue = ReSTIR::OverdueRetirements(today);
    static bool s_reported = false;
    if (!overdue.empty() && !s_reported)
    {
        s_reported = true;
        LOG_WARNING("ReSTIR cores past their retirement date are still offered by the editor menu: %s - the 'CLI only' "
                    "step of ReSTIR_Dual_Backend_Plan S8 is due", overdue.c_str());
    }
}

// ReSTIR backends: the command line wins over engine.json for this run; an unknown
// name refuses the run like -gipath. -di_backend_initial= / -di_backend_reuse= (dev)
// pick one half of the DI composition; RTDIPass reads them, this only validates.
void ApplyRestirBackendFlags(Renderer& renderer)
{
    CheckRetirementDeadlines();
    const auto& cmd = Sys::CommandLine::Get();
    static constexpr struct { const char* flag; ReSTIR::Stage stage; } kBackendFlags[] = {
        { "-di_backend", ReSTIR::Stage::DI }, { "-gi_backend", ReSTIR::Stage::GI }, { "-pt_backend", ReSTIR::Stage::PT } };
    for (const auto& f : kBackendFlags)
    {
        if (!cmd.Has(f.flag)) continue;
        const auto token = cmd.Get(f.flag, "");
        ReSTIR::Backend backend;
        if (!ReSTIR::ParseBackend(f.stage, token, backend))
        {
            LOG_ERROR("Invalid %s=%s; expected %s", f.flag, token.c_str(), ReSTIR::LegalBackends(f.stage).c_str());
            PostQuitMessage(Harness::PreconditionFail);
        }
        else renderer.SetRestirBackend(f.stage, backend);
    }
    for (const char* flag : { "-di_backend_initial", "-di_backend_reuse" })
    {
        if (!cmd.Has(flag)) continue;
        const auto token = cmd.Get(flag, "");
        ReSTIR::Backend half;
        if (!ReSTIR::ParseHalf(token, half))
        {
            LOG_ERROR("Invalid %s=%s; expected engine|rtxdi", flag, token.c_str());
            PostQuitMessage(Harness::PreconditionFail);
        }
    }
    if (cmd.Has("-restir_compare"))
    {
        const auto token = cmd.Get("-restir_compare", "");
        ReSTIR::Compare compare;
        if (!ReSTIR::ParseCompare(token, compare))
        {
            LOG_ERROR("Invalid -restir_compare=%s; expected off|di|gi", token.c_str());
            PostQuitMessage(Harness::PreconditionFail);
        }
        else renderer.SetRestirCompare(compare);
    }
    // -gi_sharc_mode (SHaRC on ReSTIR GI) is not checked here: it is the flag alias of the
    // row raytracing.restirGiSharcMode, and the boot load refuses a mode the row does not
    // offer, or none (Settings/LegacyFlags.h).
}
