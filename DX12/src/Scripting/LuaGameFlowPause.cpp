// LuaGameFlowPause.cpp - GameFlow.Pause / Resume / IsPaused / PauseReasons / ConfigurePause: the
// Lua face of the game flow's pause layer (Gameplay/GameFlowPause.cpp).

#include "Scripting/LuaGameFlowBindings.h"
#include "Scripting/LuaDoc.h"

#include "Gameplay/GameFlowState.h"
#include "Scripting/ScriptSystem.h"
#include "System/Log.h"

#include <sol/sol.hpp>

#include <string>
#include <vector>

namespace Gameplay
{

void RegisterLuaGameFlowPause(sol::table& table, GameFlowStateMachine& flow, ScriptSystem& scripts)
{
    // A pause belongs to the script that asked for it and ends with it (a scene's exit, a System's
    // shutdown, an entity's destroy), like Time.RequestScale.
    scripts.AddOwnerReleaseHook([&flow](std::uint64_t owner, bool everything)
    {
        if (everything) flow.ReleaseScriptPauses();
        else            flow.ReleasePausesOwnedBy(owner);
    });

    LuaDoc::Fn(table, "GameFlow", "Pause",
        [&flow, &scripts](sol::optional<std::string> reason) -> std::uint64_t
        {
            return flow.RequestPause(reason ? *reason : std::string("script"), scripts.CurrentOwner());
        },
        { .doc = "Pause the game until released: from the next frame the game clock stops (Time.dt 0, animation, attack clocks, hit windows, physics, AI) and the pause template's SystemSets are suspended (by default GameplaySimulation, GameplayFX, Cinematics). Scripts, UI, audio and the skinning chain keep running; Time.realDt and real-time timers too. Pauses stack: the game runs again when every pause is released. Released with the script that made it.",
          .params = { { .name = "reason", .doc = "shown in [PAUSE] log lines and GameFlow.PauseReasons", .optional = true } },
          .returnsDoc = "handle for GameFlow.Resume", .effect = LuaDoc::Effect::World });

    LuaDoc::Fn(table, "GameFlow", "Resume",
        [&flow](std::uint64_t handle) -> bool { return flow.ReleasePause(handle); },
        { .doc = "Release a GameFlow.Pause; false when it was already released. The game runs again from the next frame once no pause is left.",
          .params = { { .name = "handle" } }, .effect = LuaDoc::Effect::World });

    LuaDoc::Fn(table, "GameFlow", "IsPaused",
        [&flow]() -> bool { return flow.IsPaused(); },
        { .doc = "True while a pause is in effect (applied at the start of the frame, like a state switch)." });

    LuaDoc::Fn(table, "GameFlow", "PauseReasons",
        [&flow](sol::this_state ts) -> sol::table
        {
            sol::state_view lua(ts);
            sol::table out = lua.create_table();
            const std::vector<std::string> reasons = flow.PauseReasons();
            for (size_t i = 0; i < reasons.size(); ++i) out[i + 1] = reasons[i];
            return out;
        },
        { .doc = "The reason of every live pause request, oldest first." });

    LuaDoc::Fn(table, "GameFlow", "ConfigurePause",
        [&flow](sol::table config) -> bool
        {
            GameFlowStateMachine::PauseTemplate t = flow.GetPauseTemplate();
            if (sol::optional<sol::table> sets = config["suspends"])
            {
                t.suspends.clear();
                for (size_t i = 1; i <= sets->size(); ++i)
                {
                    const sol::optional<std::string> s = (*sets)[i];
                    if (!s)
                    {
                        LOG_ERROR("GameFlow.ConfigurePause: suspends[%zu] must be a string", i);
                        return false;
                    }
                    t.suspends.push_back(*s);
                }
            }
            if (const sol::optional<float> v = config["timeScale"]) t.timeScale = *v < 0.f ? 0.f : *v;
            if (const sol::optional<bool> v = config["pauseAudio"]) t.pauseAudio = *v;
            if (const sol::optional<std::string> v = config["inputContext"]) t.inputContext = *v;
            if (const sol::optional<bool> v = config["menuKey"]) t.menuKey = *v;
            if (const sol::optional<std::string> v = config["cursor"])
            {
                if (*v == "free")          t.cursor = CursorMode::Free;
                else if (*v == "captured") t.cursor = CursorMode::Captured;
                else if (*v == "inherit")  t.cursor = CursorMode::Inherit;
                else
                {
                    LOG_ERROR("GameFlow.ConfigurePause: cursor must be inherit, free or captured");
                    return false;
                }
            }
            flow.ConfigurePause(std::move(t));
            return true;
        },
        { .doc = "Set what a pause does (the pause template); fields left out keep their value. suspends = array of SystemSet names (default GameplaySimulation, GameplayFX, Cinematics), timeScale (default 0), pauseAudio (default false), inputContext (pushed while paused), cursor (inherit|free|captured), menuKey (default true: the menu key - Escape / pad Start - pauses and resumes when no state handles the 'pause' trigger).",
          .params = { { .name = "config", .type = "table" } },
          .returnsDoc = "false when a field is malformed", .effect = LuaDoc::Effect::World });
}

} // namespace Gameplay
