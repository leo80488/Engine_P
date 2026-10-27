#pragma once

// Lua bindings for the data-driven scene system: a global `Scene` table forwarding to the C++
// SceneManager:
//   Scene.Load(name[, opts]) -> faded transition to a registered scene name or raw .iscene path
//   Scene.LoadInstant(name)  -> same, as an instant cut
//   Scene.Reload()           -> reload the current scene
//   Scene.Preload / PreloadStatus / Commit -> explicit preload for a script-owned loading screen
//   Scene.Current()          -> current scene name
//   Scene.List()             -> array table of registered scene names
// plus Engine.SaveGame / LoadGame / HasSaveGame / GetPlaytime.
//
// Registered once at startup, after ScriptSystem::Initialize() (which creates the Lua VM) and
// before any user script runs.

namespace sol { class state; }
class SceneManager;
namespace Gameplay { class GameFlowStateMachine; }

namespace Scene
{
    // @p flow is optional and used only by Engine.SaveGame / Engine.LoadGame, so a save records
    // the player's GameFlow state and a load restores it. Null = no GameFlow state is saved or
    // restored.
    void RegisterLuaSceneBindings(sol::state& lua, ::SceneManager& mgr,
                                  Gameplay::GameFlowStateMachine* flow = nullptr);
}
