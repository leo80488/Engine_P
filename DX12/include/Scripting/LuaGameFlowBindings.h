#pragma once

namespace sol { class state; }

class World;
class SceneManager;
class SystemRegistry;
class ScriptSystem;

namespace Gameplay
{
class GameFlowStateMachine;

// Creates the narrow GameFlow.* Lua surface and the lightweight `world` proxy
// passed to state OnEnter/OnExit callbacks.
void RegisterLuaGameFlowBindings(sol::state& lua,
                                 GameFlowStateMachine& flow,
                                 World& world,
                                 SceneManager& scenes,
                                 SystemRegistry& systems,
                                 ScriptSystem& scripts);
}

