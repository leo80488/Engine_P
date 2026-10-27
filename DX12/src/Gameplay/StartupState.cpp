#include "Gameplay/StartupState.h"

#include "Gameplay/GameFlowState.h"
#include "System/CommandLine.h"
#include "System/HostPolicy.h"   // Sys::IsEditorHost (never #ifdef WITH_EDITOR)
#include "System/Log.h"

// A menu-first project's Boot/MainMenu state CHOOSES the first world: pushing startup_scene first
// would pay a blocking .iscene load plus a fade for a world it is about to replace. Empty, pinned
// by -scene=, an editor host in Edit mode, undefined or refused -> false: the caller pushes
// startup_scene as usual.
bool Gameplay::RequestStartupState(GameFlowStateMachine& gameFlow, const std::string& bootState)
{
    bool flowOwnsBootScene = false;
    if (!bootState.empty())
    {
        // -scene= outranks BOTH manifest keys. The whole scenario corpus pins
        // the world it is about to assert on that way, and a boot state whose
        // on_enter loads something else would silently un-pin every one of them.
        const bool cliScenePinned = !Sys::CommandLine::Get().Get("-scene").empty();

        // GameFlowResolveSystem is RunMask::Play, so an editor host still in Edit mode never resolves
        // anything and never runs an on_enter: honouring startup_state there boots the editor into an
        // empty world with no current-scene binding for Ctrl+S and no way to get one. Runtime query, not
        // #ifdef.
        const bool flowWillResolve = !Sys::IsEditorHost()
                                  || Sys::CommandLine::Get().Has("-play");

        if (cliScenePinned)
        {
            LOG_INFO("App: -scene= pins the boot world — game.json startup_state "
                     "'%s' is ignored for this run.", bootState.c_str());
        }
        else if (!flowWillResolve)
        {
            LOG_INFO("App: game.json startup_state '%s' ignored — this host boots "
                     "into Edit, where GameFlowResolveSystem (RunMask::Play) never "
                     "runs. Pass -play to boot through the flow.", bootState.c_str());
        }
        else if (!gameFlow.IsDefined(bootState))
        {
            // Not fatal: the graph is authored in Lua and a project may have its flow scripts disabled.
            // Say it out loud though - the symptom otherwise is a boot state that silently never runs.
            LOG_WARNING("App: game.json startup_state '%s' is not a defined GameFlow "
                        "state (a Lua System script declares it with "
                        "GameFlow.DefineState, at top level) — booting "
                        "startup_scene instead.", bootState.c_str());
        }
        else if (const Gameplay::SwitchResult flowBoot =
                     gameFlow.TryRequestSwitch(bootState);
                 flowBoot == Gameplay::SwitchResult::Accepted)
        {
            // REQUESTED, not resolved: GameFlowResolveSystem is ResolvePending's only caller, once at the
            // fixed Input-phase transition point. Resolving here would make App a second resolver holding the
            // scheduler's CommandBuffer outside any phase, and would gain no visible frame: frame 1 resolves
            // in TickPhase::Input, and an on_enter's Scene.Load(name, false) is picked up by that same frame's
            // mode drain, so the world exists before the frame renders.
            flowOwnsBootScene = true;
            LOG_INFO("App: boot state = '%s' — startup_scene is NOT pushed; the "
                     "state's on_enter owns the first Scene.Load.", bootState.c_str());
        }
        else
        {
            // Defined but refused (abstract target, a can_enter guard, or
            // validation quarantined it). Fall through rather than boot nowhere.
            LOG_WARNING("App: game.json startup_state '%s' was not accepted (%s) — "
                        "booting startup_scene instead.",
                        bootState.c_str(), Gameplay::ToString(flowBoot));
        }
    }
    return flowOwnsBootScene;
}
