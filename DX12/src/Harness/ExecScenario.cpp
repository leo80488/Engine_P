#include "Harness/ExecScenario.h"

#ifdef WITH_HARNESS

#include "Harness/ExitCodes.h"
#include "Harness/RunLedger.h"
#include "Scripting/ScriptSystem.h"
#include "System/CommandLine.h"
#include "System/HostPolicy.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include <string>

void Harness::StartExecScenario(ScriptSystem& scriptSystem, bool bootScriptsRefused)
{
    // Compiled out of Retail (Engine.Cpp.props): a shipping binary does not carry the
    // loader at all.
    //
    // LoadSystem, NOT AddGlobalScript: AddGlobalScript runs the chunk once and
    // DISCARDS its return value (it only records the path for hot reload), so the
    // driver would get no OnInit and no per-frame OnUpdate - it could not wait for a
    // frame, drive anything over time or assert at a chosen moment. LoadSystem
    // consumes the returned table and registers a singleton that ScriptLogicSystem
    // ticks every Play frame. The driver's contract is in ExecScenario.h.
    //
    // The -artifacts gate is load-bearing: -exec runs an arbitrary .lua from a path
    // the caller chooses, never cooked into game.ipak, so ungated a Game.exe would
    // execute any script anyone points it at. It costs a scenario runner nothing:
    // every run passes -artifacts (without it there is no run.json, frames.jsonl or
    // verdict).
    // !bootScriptsRefused: a refused manifest boot must not execute ANY chunk's
    // top-level side effects - the driver included.
    if (!bootScriptsRefused && Sys::CommandLine::Get().Has("-exec"))
    {
        // Refuse with PostQuitMessage, not a bare return: App::Run then leaves through
        // its normal exit path (ShutdownAllSystems, SavePSOLibrary), keeping the PSO
        // cache.
        if (!Sys::HasArtifactDir())
        {
            LOG_ERROR("-exec is refused without -artifacts=<dir>. Running an "
                      "arbitrary external script is a harness capability, and a "
                      "harness run writes its verdict somewhere by definition.");
            PostQuitMessage(Harness::PreconditionFail);
        }
        else
        {
        const std::string execPath = Sys::CommandLine::Get().Get("-exec");

        // LoadSystem keys on the FILE STEM and tears down any existing system with the
        // same name - a driver called QuestSystem.lua would silently unload
        // asset/scripts/systems/QuestSystem.lua, firing its OnShutdown and dropping its
        // event subscriptions. Log the resolved name so that collision is visible.
        LOG_INFO("App: -exec='%s' (system name '%s')", execPath.c_str(),
                 Sys::PathToUtf8(Sys::U8Path(execPath).stem()).c_str());

        if (!scriptSystem.LoadSystem(execPath))
        {
            // Covers both "file missing / unreadable" and "loaded but did not
            // return a table". Also the ONLY place a Lua SYNTAX error in the
            // driver is observable: sol's safe_script returns a load-status
            // result before the message handler is ever installed, so the
            // traceback handler never sees it.
            LOG_ERROR("App: -exec='%s' failed to load. The file must exist and "
                      "must `return <table>` at top level.", execPath.c_str());
            Harness::RunLedger::Get().NoteScriptFault(execPath.c_str(),
                                                      "driver failed to load");
            PostQuitMessage(Harness::PreconditionFail);
        }
        else if (Sys::IsEditorHost() && !Sys::CommandLine::Get().Has("-play"))
        {
            // Editor.exe boots in Edit mode, and ScriptLogicSystem is RunMask::Play - so
            // without -play the driver loads and then never ticks. Warn instead of a silent
            // no-op.
            LOG_WARNING("App: -exec= was given to an editor host still in Edit "
                        "mode; the scenario will NOT tick. Add -play.");
        }
        }   // else (-artifacts present)
    }
}

#endif // WITH_HARNESS
