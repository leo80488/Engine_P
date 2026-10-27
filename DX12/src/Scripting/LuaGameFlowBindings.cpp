#include "Scripting/LuaGameFlowBindings.h"
#include "Scripting/LuaDoc.h"   // bindings described as they are bound

#include "Gameplay/GameFlowState.h"
#include "ECS/SystemRegistry.h"
#include "Scene/SceneManager.h"
#include "Scripting/ScriptSystem.h"
#include "System/Log.h"

#include <sol/sol.hpp>

#include "Scripting/LuaEntityId.h"     // LuaEntity + LuaId::* (needs sol first)
#include "Scripting/LuaPropertyBag.h"  // transition params (needs sol first)

#include <algorithm>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace Gameplay
{
// GameFlow.Pause and friends (LuaGameFlowPause.cpp).
void RegisterLuaGameFlowPause(sol::table& table, GameFlowStateMachine& flow, ScriptSystem& scripts);

namespace
{
    bool ReadStringArray(sol::table definition, const char* field,
                         std::vector<std::string>& out)
    {
        out.clear();
        sol::object value = definition[field];
        if (!value.valid() || value == sol::lua_nil) return true;
        if (!value.is<sol::table>())
        {
            LOG_ERROR("GameFlow.DefineState: '%s' must be an array of strings", field);
            return false;
        }

        sol::table array = value.as<sol::table>();
        const size_t count = array.size();
        // Reject map-style tables ({ MenuUI = true }): size() only counts the
        // sequence part, so map keys would silently activate nothing.
        size_t pairCount = 0;
        for (const auto& kv : array) { (void)kv; ++pairCount; }
        if (pairCount != count)
        {
            LOG_ERROR("GameFlow.DefineState: '%s' must be a sequential array "
                      "of strings, e.g. { \"MenuUI\" }", field);
            return false;
        }
        out.reserve(count);
        for (size_t i = 1; i <= count; ++i)
        {
            sol::object item = array[i];
            if (!item.is<std::string>())
            {
                LOG_ERROR("GameFlow.DefineState: '%s[%zu]' must be a string", field, i);
                return false;
            }
            out.push_back(item.as<std::string>());
        }
        return true;
    }

    // The edge, as a Lua table. "Leaving InGame for MainMenu fades out, leaving
    // it for Paused does not" is otherwise inexpressible without every state
    // enumerating every state it can possibly reach.
    sol::table EdgeToLua(sol::state& lua, const GameFlowTransition& edge)
    {
        sol::table t = lua.create_table();
        t["from"]    = edge.fromName;
        t["to"]      = edge.toName;
        t["trigger"] = edge.trigger;
        t["reentry"] = edge.isReentry;
        return t;
    }

    // Hooks take (world, edge, params); a Lua function that declares fewer ignores the rest,
    // so function(world) hooks work.
    // -> false when the hook errored. A missing hook is success: the state had nothing to do.
    bool InvokeLuaHook(sol::state* lua, const std::string& stateName,
                       const char* hookName,
                       const GameFlowTransition& edge,
                       const PropertyBag& params)
    {
        if (!lua) return true;
        sol::table states = (*lua)["__gameflow_states"];
        if (!states.valid()) return true;
        sol::object definitionObject = states[stateName];
        if (!definitionObject.is<sol::table>()) return true;

        sol::table definition = definitionObject.as<sol::table>();
        sol::protected_function hook = definition[hookName];
        if (!hook.valid()) return true;

        sol::table worldApi = (*lua)["__gameflow_world"];
        sol::protected_function_result result =
            hook(worldApi, EdgeToLua(*lua, edge),
                 Scripting::PropertyBagToLua(*lua, params));
        if (!result.valid())
        {
            sol::error error = result;
            LOG_ERROR("GameFlow Lua %s error [%s]: %s",
                      hookName, stateName.c_str(), error.what());
            return false;
        }
        return true;
    }

    void InvokeLuaUpdate(sol::state* lua, const std::string& stateName,
                         float dt, float timeInState)
    {
        if (!lua) return;
        sol::table states = (*lua)["__gameflow_states"];
        if (!states.valid()) return;
        sol::object definitionObject = states[stateName];
        if (!definitionObject.is<sol::table>()) return;

        sol::protected_function hook = definitionObject.as<sol::table>()["on_update"];
        if (!hook.valid()) return;

        sol::table worldApi = (*lua)["__gameflow_world"];
        sol::protected_function_result result = hook(worldApi, dt, timeInState);
        if (!result.valid())
        {
            sol::error error = result;
            LOG_ERROR("GameFlow Lua on_update error [%s]: %s",
                      stateName.c_str(), error.what());
        }
    }

    // Guards run outside the transition, at request time (so Switch reports its refusal
    // synchronously), so unlike the hooks they do not rebind the world. A guard is a pure
    // predicate; one that errors counts as a refusal.
    bool InvokeLuaGuard(sol::state* lua, const std::string& stateName,
                        const char* guardName, const GameFlowTransition& edge)
    {
        if (!lua) return true;
        sol::table states = (*lua)["__gameflow_states"];
        if (!states.valid()) return true;
        sol::object definitionObject = states[stateName];
        if (!definitionObject.is<sol::table>()) return true;

        sol::protected_function guard = definitionObject.as<sol::table>()[guardName];
        if (!guard.valid()) return true;

        sol::protected_function_result result = guard(EdgeToLua(*lua, edge));
        if (!result.valid())
        {
            sol::error error = result;
            LOG_ERROR("GameFlow Lua %s error [%s]: %s -- treating it as a refusal",
                      guardName, stateName.c_str(), error.what());
            return false;
        }
        return result.get<bool>();
    }

    std::string BuildDebugDump(const GameFlowStateMachine& flow,
                               const SystemRegistry& systems,
                               const ScriptSystem& scripts)
    {
        std::ostringstream out;
        out << flow.DebugDump();

        std::vector<std::pair<std::string, SystemSetId>> sets;
        sets.reserve(flow.KnownSystemSets().size());
        for (const auto& [id, name] : flow.KnownSystemSets())
            sets.emplace_back(name, id);
        std::sort(sets.begin(), sets.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

        const auto luaAssignments = scripts.GetSystemSetAssignments();
        out << "  SystemSets:\n";
        if (sets.empty()) out << "    <none>\n";
        for (const auto& [setName, setId] : sets)
        {
            out << "    " << setName << ": "
                << (flow.IsSystemSetActive(setId) ? "active" : "inactive") << '\n';

            const auto cppSystems = systems.GetSystemsInSet(setId);
            for (const ISystem* system : cppSystems)
                out << "      C++ " << system->GetName() << '\n';
            for (const auto& [scriptName, scriptSet] : luaAssignments)
                if (scriptSet == setName)
                    out << "      Lua " << scriptName << '\n';
            if (cppSystems.empty()
                && std::none_of(luaAssignments.begin(), luaAssignments.end(),
                    [&](const auto& assignment) { return assignment.second == setName; }))
            {
                out << "      <no systems>\n";
            }
        }
        return out.str();
    }
}

void RegisterLuaGameFlowBindings(sol::state& lua,
                                 GameFlowStateMachine& flow,
                                 World& world,
                                 SceneManager& scenes,
                                 SystemRegistry& systems,
                                 ScriptSystem& scripts)
{
    lua["__gameflow_states"] = lua.create_table();

    // The callback argument intentionally exposes only flow-relevant seams.
    // Existing global Engine/Scene APIs remain available to the callback too.
    sol::table worldApi = lua.create_table();
    // -> bool: dispatch, not load success (the load runs frames later, behind the cover).
    // False means the id resolved to nothing; the attempt still goes through, but an on_enter
    // can switch to an error state on a typo'd name. The real outcome arrives as
    // Scene::SceneLoadedEvent / SceneLoadFailedEvent (Scene/SceneEvents.h).
    worldApi.set_function("LoadScene",
        [&scenes](sol::table, const std::string& name)
        {
            return scenes.RequestLoad(name, /*fade=*/true);
        });
    worldApi.set_function("LoadSceneInstant",
        [&scenes](sol::table, const std::string& name)
        {
            return scenes.RequestLoad(name, /*fade=*/false);
        });

    // The flow's async seam. What is asynchronous is the asset warm, not the world build:
    // BeginScenePreload starts it and performs no swap, so a Loading state keeps the screen and
    // its own art; CommitScenePreload pays the (blocking) world build when the state chooses.
    worldApi.set_function("BeginScenePreload",
        [&scenes](sol::table, const std::string& name)
        {
            scenes.BeginPreload(name);
        });
    // -> ready, pct. ready=false with pct=0 also means "nothing in flight",
    // including a preload whose target did not resolve -- that publishes a
    // Scene::SceneLoadFailedEvent instead of pretending it will become ready.
    worldApi.set_function("ScenePreloadStatus",
        [&scenes](sol::table) -> std::tuple<bool, float>
        {
            float pct = 0.f;
            const bool ready = scenes.PreloadReady(pct);
            return { ready, pct };
        });
    // -> bool. Instant cut on purpose: the caller's loading screen is already
    // covering the screen, so the built-in fade would be fading out art that is
    // already up.
    worldApi.set_function("CommitScenePreload",
        [&scenes](sol::table) { return scenes.CommitPreloaded(); });
    worldApi.set_function("Scope",
        [&flow, &world](sol::table, LuaEntity entity) -> bool
        {
            const Entity e = LuaId::ResolveLive(world, entity);
            if (e == NullEntity) return false;
            return flow.ScopeEntity(world, e);
        });
    lua["__gameflow_world"] = worldApi;

    sol::table table = lua.create_table();

    auto define = [&lua, &flow, &scripts](const std::string& name,
                                          sol::table config,
                                          bool subState) -> bool
    {
        GameFlowStateDefinition definition;
        definition.name = name;
        if (!ReadStringArray(config, "sets", definition.sets)
            || !ReadStringArray(config, "suspends", definition.suspends))
        {
            return false;
        }

        // A misspelled hook value ("on_enter = 'foo'") must fail loudly at
        // define time, not as a silent no-op on the first transition.
        for (const char* hookField : { "on_enter", "on_exit" })
        {
            sol::object hook = config[hookField];
            if (hook.valid() && hook != sol::lua_nil
                && hook.get_type() != sol::type::function)
            {
                LOG_ERROR("GameFlow.%s('%s'): '%s' must be a function",
                          subState ? "DefineSubState" : "DefineState",
                          name.c_str(), hookField);
                return false;
            }
        }

        // ---- Declarative resources ----
        // Applied by reading the resolved value every frame, not by pairing an apply in on_enter
        // with an undo in on_exit, so a state cannot leak its cursor grab or input context
        // whatever happens to the transition meant to clean up after it.
        if (sol::object ts = config["time_scale"]; ts.valid() && ts != sol::lua_nil)
        {
            if (!ts.is<double>())
            {
                LOG_ERROR("GameFlow.%s('%s'): 'time_scale' must be a number",
                          subState ? "DefineSubState" : "DefineState", name.c_str());
                return false;
            }
            const double value = ts.as<double>();
            if (value < 0.0)
            {
                LOG_ERROR("GameFlow.%s('%s'): 'time_scale' must be >= 0 (0 freezes)",
                          subState ? "DefineSubState" : "DefineState", name.c_str());
                return false;
            }
            definition.timeScale = static_cast<float>(value);
        }

        definition.inputContext = config.get_or("input_context", std::string{});
        definition.pauseAudio   = config.get_or("pause_audio", false);

        if (const std::string cursor = config.get_or("cursor", std::string{});
            !cursor.empty())
        {
            if (cursor == "captured")   definition.cursor = CursorMode::Captured;
            else if (cursor == "free")  definition.cursor = CursorMode::Free;
            else if (cursor == "inherit") definition.cursor = CursorMode::Inherit;
            else
            {
                LOG_ERROR("GameFlow.%s('%s'): 'cursor' must be "
                          "\"captured\", \"free\" or \"inherit\" (got '%s')",
                          subState ? "DefineSubState" : "DefineState",
                          name.c_str(), cursor.c_str());
                return false;
            }
        }

        // bus_duck is a MAP ({ Music = 0.25 }), unlike sets/suspends which are
        // arrays -- so the array-shape rejection in ReadStringArray must not be
        // applied to it.
        if (sol::object duck = config["bus_duck"]; duck.valid() && duck != sol::lua_nil)
        {
            if (!duck.is<sol::table>())
            {
                LOG_ERROR("GameFlow.%s('%s'): 'bus_duck' must be a table, "
                          "e.g. { Music = 0.25 }",
                          subState ? "DefineSubState" : "DefineState", name.c_str());
                return false;
            }
            for (const auto& [key, value] : duck.as<sol::table>())
            {
                if (!key.is<std::string>() || !value.is<double>())
                {
                    LOG_ERROR("GameFlow.%s('%s'): 'bus_duck' entries must be "
                              "bus-name = number",
                              subState ? "DefineSubState" : "DefineState",
                              name.c_str());
                    return false;
                }
                definition.busDuck.emplace_back(
                    key.as<std::string>(), static_cast<float>(value.as<double>()));
            }
        }

        // ---- Behaviour ----
        // The trigger table is a map ({ pause = "Paused" }), so it must not go through the
        // array-shape check the sets/suspends fields use.
        if (sol::object on = config["on"]; on.valid() && on != sol::lua_nil)
        {
            if (!on.is<sol::table>())
            {
                LOG_ERROR("GameFlow.%s('%s'): 'on' must be a table of "
                          "trigger = \"TargetState\"",
                          subState ? "DefineSubState" : "DefineState", name.c_str());
                return false;
            }
            for (const auto& [key, value] : on.as<sol::table>())
            {
                if (!key.is<std::string>() || !value.is<std::string>())
                {
                    LOG_ERROR("GameFlow.%s('%s'): 'on' entries must be "
                              "trigger = \"TargetState\"",
                              subState ? "DefineSubState" : "DefineState",
                              name.c_str());
                    return false;
                }
                definition.on.push_back(
                    GameFlowEdge{ key.as<std::string>(), value.as<std::string>() });
            }
        }

        definition.timeout       = config.get_or("timeout", 0.f);
        definition.timeoutTarget = config.get_or("timeout_to", std::string{});
        definition.rebuildOnSceneLoad =
            config.get_or("rebuild_on_scene_load", false);
        definition.final = config.get_or("final", false);
        if (definition.timeout > 0.f && definition.timeoutTarget.empty())
        {
            LOG_ERROR("GameFlow.%s('%s'): 'timeout' needs a 'timeout_to' target",
                      subState ? "DefineSubState" : "DefineState", name.c_str());
            return false;
        }

        for (const char* fnField : { "on_update", "can_enter", "can_exit" })
        {
            sol::object fn = config[fnField];
            if (fn.valid() && fn != sol::lua_nil
                && fn.get_type() != sol::type::function)
            {
                LOG_ERROR("GameFlow.%s('%s'): '%s' must be a function",
                          subState ? "DefineSubState" : "DefineState",
                          name.c_str(), fnField);
                return false;
            }
        }

        // ---- Hierarchy ----
        definition.initial  = config.get_or("initial", std::string{});
        definition.abstract = config.get_or("abstract", false);
        definition.priority = config.get_or("priority", 0);
        if (definition.abstract && definition.initial.empty())
        {
            LOG_ERROR("GameFlow.%s('%s'): an abstract state must name an "
                      "'initial' child -- otherwise nothing can ever enter it",
                      subState ? "DefineSubState" : "DefineState", name.c_str());
            return false;
        }

        definition.parent = config.get_or("parent", std::string{});
        if (subState && definition.parent.empty())
        {
            LOG_ERROR("GameFlow.DefineSubState('%s'): missing parent", name.c_str());
            return false;
        }
        if (!subState && !definition.parent.empty())
        {
            LOG_ERROR("GameFlow.DefineState('%s'): parent is only valid on DefineSubState",
                      name.c_str());
            return false;
        }

        // Hooks fire from the Input-phase resolver, outside ScriptSystem::Update: bind the world
        // for the hook's duration so Engine.* entity APIs (CreateEntity, DestroyEntity, ...) work
        // inside on_enter / on_exit.
        sol::state* luaPtr = &lua;
        ScriptSystem* scriptsPtr = &scripts;
        definition.onEnter = [luaPtr, scriptsPtr, name](
            World& world, const GameFlowTransition& edge,
            const PropertyBag& params) -> bool
        {
            struct BindGuard
            {
                ScriptSystem* scripts;
                World* saved;
                ~BindGuard() { scripts->ExchangeBoundWorld(saved); }
            } guard{ scriptsPtr, scriptsPtr->ExchangeBoundWorld(&world) };
            return InvokeLuaHook(luaPtr, name, "on_enter", edge, params);
        };
        definition.onExit = [luaPtr, scriptsPtr, name](
            World& world, const GameFlowTransition& edge,
            const PropertyBag& params) -> bool
        {
            struct BindGuard
            {
                ScriptSystem* scripts;
                World* saved;
                ~BindGuard() { scripts->ExchangeBoundWorld(saved); }
            } guard{ scriptsPtr, scriptsPtr->ExchangeBoundWorld(&world) };
            return InvokeLuaHook(luaPtr, name, "on_exit", edge, params);
        };
        definition.onUpdate = [luaPtr, scriptsPtr, name](
            World& world, float dt, float timeInState)
        {
            struct BindGuard
            {
                ScriptSystem* scripts;
                World* saved;
                ~BindGuard() { scripts->ExchangeBoundWorld(saved); }
            } guard{ scriptsPtr, scriptsPtr->ExchangeBoundWorld(&world) };
            InvokeLuaUpdate(luaPtr, name, dt, timeInState);
        };

        // Only installed when the table actually declares one: an always-present
        // std::function would make every state pay a Lua round trip per request.
        if (config["can_enter"].valid())
        {
            definition.canEnter = [luaPtr, name](const GameFlowTransition& edge)
            {
                return InvokeLuaGuard(luaPtr, name, "can_enter", edge);
            };
        }
        if (config["can_exit"].valid())
        {
            definition.canExit = [luaPtr, name](const GameFlowTransition& edge)
            {
                return InvokeLuaGuard(luaPtr, name, "can_exit", edge);
            };
        }

        if (!flow.DefineState(std::move(definition))) return false;
        sol::table registry = lua["__gameflow_states"];
        registry[name] = config; // hot reload replaces callbacks in place
        return true;
    };

    table.set_function("DefineState",
        [define](const std::string& name, sol::table config) mutable -> bool
        {
            return define(name, config, /*subState=*/false);
        });
    table.set_function("DefineSubState",
        [define](const std::string& name, sol::table config) mutable -> bool
        {
            return define(name, config, /*subState=*/true);
        });
    // Two return values: `if GameFlow.Switch(x) then` works (Lua drops the extra), and a
    // controller can tell "someone else won this frame, retry" from "that state does not
    // exist".
    LuaDoc::Fn(table, "GameFlow", "Switch",
        [&flow](const std::string& name,
                sol::optional<sol::table> params) -> std::tuple<bool, std::string>
        {
            PropertyBag bag;
            if (params) bag = Scripting::PropertyBagFromLua(*params);
            const SwitchResult result = flow.TryRequestSwitch(name, std::move(bag));
            return { result == SwitchResult::Accepted, ToString(result) };
        },
            { .doc = "Request a switch to a named state, with optional parameters; answers whether it was accepted and, when not, why (someone else won this frame, or no such state).", .params = { { .name = "name" }, { .name = "params" } }, .returnsDoc = "accepted, reason", .effect = LuaDoc::Effect::World });

    // Params ride with the transition. Do not stash them in the global GameState KV instead:
    // it has no transition lifetime, so two callers racing to start a Loading state overwrite
    // each other and the second one's scene loads.
    table.set_function("Params",
        [&flow, &lua]() -> sol::table
        {
            return Scripting::PropertyBagToLua(lua, flow.CurrentParams());
        });
    table.set_function("TimeInState",
        [&flow]() -> float { return flow.TimeInState(); });

    // Synchronous, unlike a fire-and-forget request channel: the caller learns whether any
    // state took the trigger.
    table.set_function("Dispatch",
        [&flow](const std::string& trigger) -> bool
        {
            return flow.Dispatch(trigger);
        });
    LuaDoc::Fn(table, "GameFlow", "Current",
        [&flow]() -> std::string { return flow.CurrentName(); },
            { .doc = "The name of the current (leaf) game-flow state." });

    // Restart the current leaf. Ancestors survive, so the HUD a parent state
    // owns is NOT rebuilt. The alternative everyone reaches for first --
    // Scene.Reload() from inside the state -- goes through World::Clear, which
    // wipes the state's own scoped entities without running OnExit and without
    // re-entering, silently.
    table.set_function("Reenter",
        [&flow]() -> std::tuple<bool, std::string>
        {
            const SwitchResult result = flow.TryRequestReenter();
            return { result == SwitchResult::Accepted, ToString(result) };
        });

    // Overlay + Back: the ONE-DEEP return slot. Not a stack -- an ordinary
    // Switch clears it -- so "where does Back go" always has exactly one
    // answer. This is what lets a single Options state be opened from both
    // MainMenu and Paused, which a sub-state cannot express (one parent).
    table.set_function("SwitchOverlay",
        [&flow](const std::string& name) -> std::tuple<bool, std::string>
        {
            const SwitchResult result = flow.TryRequestSwitchOverlay(name);
            return { result == SwitchResult::Accepted, ToString(result) };
        });
    table.set_function("Back",
        [&flow]() -> std::tuple<bool, std::string>
        {
            const SwitchResult result = flow.TryRequestReturn();
            return { result == SwitchResult::Accepted, ToString(result) };
        });
    table.set_function("ReturnTo",
        [&flow]() -> std::string { return flow.ReturnToName(); });
    // "If I switch to X, where do I actually land?" -- the answer differs from
    // X the moment X names an initial child.
    table.set_function("EntryLeaf",
        [&flow](const std::string& name) -> std::string
        {
            return flow.EntryLeafName(name);
        });

    // Current() names the LEAF only, so `Current() == "InGame"` goes false the
    // instant a sub-state is entered. Ask IsInState instead -- it is true for
    // the state itself and every ancestor on the active path.
    table.set_function("IsInState",
        [&flow](const std::string& name) -> bool
        {
            return flow.IsInState(name);
        });
    table.set_function("Path",
        [&flow, &lua]() -> sol::table
        {
            sol::table result = lua.create_table();
            const auto names = flow.CurrentPathNames();
            for (size_t i = 0; i < names.size(); ++i) result[i + 1] = names[i];
            return result;
        });
    table.set_function("Depth",
        [&flow]() -> int { return flow.CurrentDepth(); });
    table.set_function("Pending",
        [&flow]() -> std::string { return flow.PendingName(); });
    table.set_function("InTransition",
        [&flow]() -> bool { return flow.InTransition(); });
    table.set_function("IsDefined",
        [&flow](const std::string& name) -> bool { return flow.IsDefined(name); });

    // false == every SystemSet gate answers "active" regardless of membership,
    // because no flow has started. A HUD that prints set state must say so.
    table.set_function("IsGatingActive",
        [&flow]() -> bool { return flow.GatingActive(); });
    RegisterLuaGameFlowPause(table, flow, scripts);

    // Resolved declarative resources, for HUDs and scenario assertions.
    table.set_function("EffectiveTimeScale",
        [&flow]() -> float { return flow.EffectiveTimeScale(); });
    table.set_function("EffectiveCursor",
        [&flow]() -> std::string
        {
            switch (flow.EffectiveCursorMode())
            {
            case CursorMode::Captured: return "captured";
            case CursorMode::Free:     return "free";
            default:                   return "inherit";
            }
        });
    table.set_function("EffectiveAudioPaused",
        [&flow]() -> bool { return flow.EffectiveAudioPaused(); });
    table.set_function("EffectiveInputContexts",
        [&flow, &lua]() -> sol::table
        {
            sol::table result = lua.create_table();
            const auto& contexts = flow.EffectiveInputContexts();
            for (size_t i = 0; i < contexts.size(); ++i) result[i + 1] = contexts[i];
            return result;
        });

    table.set_function("Snapshot",
        [&flow, &lua]() -> sol::table
        {
            GameFlowStateMachine::FlowSnapshot snap;
            flow.FillSnapshot(snap);

            sol::table result = lua.create_table();
            result["current"]          = snap.current;
            result["pending"]          = snap.pending;
            result["inTransition"]     = snap.inTransition;
            result["passThrough"]      = snap.passThrough;
            result["definitionsValid"] = snap.definitionsValid;

            sol::table path = lua.create_table();
            for (size_t i = 0; i < snap.currentPath.size(); ++i)
                path[i + 1] = snap.currentPath[i];
            result["path"] = path;

            sol::table states = lua.create_table();
            for (size_t i = 0; i < snap.states.size(); ++i)
            {
                const auto& row = snap.states[i];
                sol::table entry = lua.create_table();
                entry["name"]          = row.name;
                entry["parent"]        = row.parent;
                entry["valid"]         = row.valid;
                entry["onCurrentPath"] = row.onCurrentPath;
                entry["isCurrent"]     = row.isCurrent;
                entry["depth"]         = row.depth;
                sol::table sets = lua.create_table();
                for (size_t s = 0; s < row.sets.size(); ++s) sets[s + 1] = row.sets[s];
                entry["sets"] = sets;
                sol::table suspends = lua.create_table();
                for (size_t s = 0; s < row.suspends.size(); ++s)
                    suspends[s + 1] = row.suspends[s];
                entry["suspends"] = suspends;
                states[i + 1] = entry;
            }
            result["states"] = states;

            sol::table sets = lua.create_table();
            for (size_t i = 0; i < snap.sets.size(); ++i)
            {
                sol::table entry = lua.create_table();
                entry["name"]       = snap.sets[i].name;
                entry["active"]     = snap.sets[i].active;
                entry["referenced"] = snap.sets[i].referenced;
                sets[i + 1] = entry;
            }
            result["sets"] = sets;
            return result;
        });

    // Oldest -> newest, rejections included. The requests that were DROPPED are
    // the half a log makes hardest to reconstruct after the fact.
    table.set_function("History",
        [&flow, &lua]() -> sol::table
        {
            sol::table result = lua.create_table();
            const auto records = flow.History();
            for (size_t i = 0; i < records.size(); ++i)
            {
                sol::table entry = lua.create_table();
                entry["frame"]   = static_cast<double>(records[i].frame);
                entry["from"]    = records[i].from;
                entry["to"]      = records[i].to;
                entry["ms"]      = records[i].elapsedMs;
                entry["kind"]    = ToString(records[i].kind);
                entry["repeats"] = static_cast<int>(records[i].repeats);
                result[i + 1] = entry;
            }
            return result;
        });

    table.set_function("Diagnostics",
        [&flow, &lua]() -> sol::table
        {
            sol::table result = lua.create_table();
            const auto& findings = flow.Diagnostics();
            for (size_t i = 0; i < findings.size(); ++i)
            {
                sol::table entry = lua.create_table();
                entry["severity"] =
                    findings[i].severity
                        == GameFlowStateMachine::FlowDiagnostic::Severity::Error
                    ? "error" : "warning";
                entry["state"]   = findings[i].state;
                entry["message"] = findings[i].message;
                result[i + 1] = entry;
            }
            return result;
        });

    // State-scoped entity ownership. Omitted owner means the current leaf.
    table.set_function("Scope",
        [&flow, &world](LuaEntity entity,
                        sol::optional<std::string> ownerName) -> bool
        {
            std::optional<GameStateId> owner;
            if (ownerName && !ownerName->empty()) owner = GameStateId(*ownerName);
            const Entity e = LuaId::ResolveLive(world, entity);
            if (e == NullEntity) return false;
            return flow.ScopeEntity(world, e, owner);
        });

    // Declares a bare SystemSet with no member systems (e.g. a set that only
    // labels state-scoped UI content). Without a declaration, a state
    // referencing the name is quarantined at FinalizeDefinitions.
    table.set_function("DeclareSet",
        [&flow](const std::string& setName) -> bool
        {
            return flow.RegisterSystemSet(setName);
        });

    table.set_function("AssignSystemSet",
        [&flow, &scripts](const std::string& systemName,
                          const std::string& setName) -> bool
        {
            if (!flow.RegisterSystemSet(setName)) return false;
            return scripts.AssignSystemToSet(systemName, setName);
        });
    table.set_function("IsSetActive",
        [&flow](const std::string& setName) -> bool
        {
            return flow.IsSystemSetActive(setName);
        });
    table.set_function("ActiveSets",
        [&flow, &lua]() -> sol::table
        {
            sol::table result = lua.create_table();
            const auto names = flow.ActiveSystemSetNames();
            for (size_t i = 0; i < names.size(); ++i) result[i + 1] = names[i];
            return result;
        });
    table.set_function("SetTraceTransitions",
        [&flow](bool enabled) { flow.SetTraceTransitions(enabled); });

    // ---- Fault landing, run boundary, hot reload ----
    // Where the flow goes when an on_enter throws. Without one, a typo in a hook leaves the
    // world half-entered (the sets are live, the entities the hook was spawning are not) with
    // no state reachable to fix it.
    table.set_function("SetFaultState",
        [&flow](const std::string& name) { flow.SetFaultState(name); });
    table.set_function("FaultState",
        [&flow]() -> std::string { return flow.FaultStateName(); });

    // New Game / level select / back to title are the SAME action: drop the
    // return slot and the params, optionally wipe GameState, then switch.
    table.set_function("BeginRun",
        [&flow](const std::string& name,
                sol::optional<sol::table> opts) -> bool
        {
            bool clearState = false;
            if (opts) clearState = opts->get_or("clear_state", false);
            return flow.BeginRun(name, clearState);
        });

    // Hot reload: remove a state. If it is current, the flow retreats to the
    // nearest surviving ancestor rather than pointing at nothing.
    table.set_function("UndefineState",
        [&flow, &lua](const std::string& name) -> bool
        {
            if (!flow.UndefineState(name)) return false;
            sol::table registry = lua["__gameflow_states"];
            if (registry.valid()) registry[name] = sol::lua_nil;
            return true;
        });
    table.set_function("Dump",
        [&flow, &systems, &scripts]() -> std::string
        {
            std::string dump = BuildDebugDump(flow, systems, scripts);
            LOG_INFO("%s", dump.c_str());
            return dump;
        });

    lua["GameFlow"] = table;
}

} // namespace Gameplay

