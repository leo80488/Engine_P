#include "Scene/LuaSceneBindings.h"
#include "Scene/SceneManager.h"
#include "Gameplay/SaveGame.h"
#include "Gameplay/GameState.h"
#include "Scripting/LuaDoc.h"

#include <sol/sol.hpp>

#include <algorithm>
#include <string>
#include <tuple>
#include <vector>

namespace Scene
{

void RegisterLuaSceneBindings(sol::state& lua, ::SceneManager& mgr,
                              Gameplay::GameFlowStateMachine* flow)
{
    sol::table tbl = lua.create_table();

    // Scene.Load("name" [, opts]) -> bool: faded transition (cover -> loading -> reveal) to a
    // registered scene name or a raw .iscene path. The optional opts table picks the transition
    // effect and its knobs (ISceneTransition.h):
    //   Scene.Load("game", {
    //       transition  = "wipe",   -- effect registry name (default "fade")
    //       duration    = 1.0,      -- durationScale, multiplies cover/reveal seconds
    //       r = 0, g = 0, b = 0,    -- cover colour, 0..255 (fade/wipe use it)
    //       progressBar = true })   -- draw the shared loading bar (default true)
    // Returns false when the id is neither a registered name nor an existing file, so a script can
    // branch to an error state on a typo; the load is still attempted. The real outcome arrives
    // later as Scene::SceneLoadedEvent (Scene/SceneEvents.h).
    tbl.set_function("Load",
        [&mgr](const std::string& name, sol::optional<sol::table> opts) -> bool
        {
            if (!opts) return mgr.RequestLoad(name, /*fade=*/true);

            sol::table t = *opts;
            TransitionParams p;
            p.type          = t.get_or("transition", std::string("fade"));
            p.durationScale = t.get_or("duration", 1.0f);
            p.r             = static_cast<std::uint8_t>(t.get_or("r", 0));
            p.g             = static_cast<std::uint8_t>(t.get_or("g", 0));
            p.b             = static_cast<std::uint8_t>(t.get_or("b", 0));
            p.showProgressBar = t.get_or("progressBar", true);
            return mgr.RequestLoad(name, /*fade=*/true, p);
        });

    // Scene.LoadInstant("name") -> bool: immediate cut, no fade (quick restarts).
    tbl.set_function("LoadInstant",
        [&mgr](const std::string& name) { return mgr.RequestLoad(name, /*fade=*/false); });

    // Scene.Reload() -> bool: reload the current scene from disk.
    tbl.set_function("Reload",
        [&mgr]() { return mgr.Reload(/*fade=*/true); });

    // Explicit preload -> commit, for a game that owns its loading screen: the mechanism is in C++
    // (SceneManager::BeginPreload / PreloadReady / CommitPreloaded), the decision when to commit
    // is in script:
    //
    //   Scene.Preload("level2")
    //   ...
    //   local ready, pct = Scene.PreloadStatus()
    //   myLoadingBar:SetFill(pct)
    //   if ready and player:PressedAny() then Scene.Commit() end
    //
    // Only the asset warm is asynchronous: Commit still pays one blocking frame to deserialise the
    // .iscene. `pct` is a real ready-handle ratio, but it counts only the assets the scene names
    // directly, so a scene of SceneRef instances can report 1.0 at once and still stall on commit
    // (SceneManager::BeginPreload).
    tbl.set_function("Preload",
        [&mgr](const std::string& name) { mgr.BeginPreload(name); });

    // Scene.PreloadStatus() -> ready, pct. ready=false with pct=0 also means no preload in flight
    // (nothing begun, or the target did not resolve and a SceneLoadFailedEvent was published).
    tbl.set_function("PreloadStatus",
        [&mgr]() -> std::tuple<bool, float>
        {
            float pct = 0.f;
            const bool ready = mgr.PreloadReady(pct);
            return { ready, pct };
        });

    // Scene.Commit() -> bool: swap to the preloaded scene now (instant cut; the caller's own
    // loading screen covers the screen). False when nothing was preloaded or the request could not
    // be dispatched.
    tbl.set_function("Commit",
        [&mgr]() { return mgr.CommitPreloaded(); });

    // Scene.Current(): the active scene's name.
    tbl.set_function("Current",
        [&mgr]() -> std::string { return mgr.CurrentScene(); });

    // Scene.List(): 1-based array table of registered scene names.
    tbl.set_function("List",
        [&mgr, &lua]() -> sol::table
        {
            sol::table out = lua.create_table();
            const std::vector<std::string> names = mgr.SceneNames();
            for (size_t i = 0; i < names.size(); ++i)
                out[i + 1] = names[i];
            return out;
        });

    lua["Scene"] = tbl;

    // Save games (Gameplay/SaveGame.h). Registered here, not in ScriptSystem, because the save
    // carries the active scene's identity and LoadGame can jump back to it: both need the
    // SceneManager. ScriptSystem has normally created the Engine table already; create it if not.
    sol::object engObj = lua["Engine"];
    sol::table engine = engObj.is<sol::table>() ? engObj.as<sol::table>()
                                                : lua.create_named_table("Engine");

    // Engine.SaveGame([slot=0]) -> bool: GameState, playtime, scene identity and PersistentTag
    // entity snapshots to save<slot>.json (atomic write, next to the exe).
    engine.set_function("SaveGame",
        [&mgr, flow](sol::optional<int> slot) -> bool
        {
            return Gameplay::SaveGame(slot.value_or(0),
                                      mgr.CurrentScene(), mgr.CurrentScenePath(),
                                      mgr.GetWorld(), flow);
        });

    // Engine.LoadGame([slot=0][, alsoLoadScene=true]) -> bool: restores GameState and playtime;
    // when alsoLoadScene and the save names a different scene, jumps there (faded). Pass false
    // when already in the right scene (e.g. restoring from a scene script's OnSceneEnter: jumping
    // would re-enter and loop).
    engine.set_function("LoadGame",
        [&mgr, flow](sol::optional<int> slot, sol::optional<bool> alsoLoadScene) -> bool
        {
            const int slotN = slot.value_or(0);
            std::string name, path;
            // The flow restore is QUEUED here, not applied: the scene jump
            // below is what brings up the world those states spawn into.
            if (!Gameplay::LoadGame(slotN, &name, &path, flow)) return false;

            bool jumping = false;
            if (alsoLoadScene.value_or(true))
            {
                // Prefer the registry name (keeps the manifest script binding
                // + readable logs); a raw path round-trips regardless.
                std::string target = path;
                const std::vector<std::string> names = mgr.SceneNames();
                if (!name.empty()
                    && std::find(names.begin(), names.end(), name) != names.end())
                    target = name;
                if (!target.empty() && target != mgr.CurrentScene()
                    && target != mgr.CurrentScenePath())
                {
                    // Entity snapshots can patch only the target scene's world: latch the slot; ActivateScene
                    // applies it after the load, before the scene script's OnSceneEnter.
                    mgr.SetPendingSaveApply(slotN);
                    mgr.RequestLoad(target, /*fade=*/true);
                    jumping = true;
                }
            }
            // Same-scene restore: patch the live world right now.
            if (!jumping && mgr.GetWorld())
                Gameplay::ApplySavedEntities(slotN, *mgr.GetWorld(), mgr.GetAssetManager(),
                                             mgr.GetRenderer(), mgr.GetAnimClipSystem());
            return true;
        });

    // Engine.HasSaveGame([slot=0]) -> bool
    engine.set_function("HasSaveGame",
        [](sol::optional<int> slot) -> bool
        { return Gameplay::SaveGameExists(slot.value_or(0)); });

    // Engine.ListSaves() / Engine.DeleteSave([slot]): the slots a picker shows, and removing one.
    LuaDoc::Fn(engine, "Engine", "ListSaves",
        [](sol::this_state ts) -> sol::table
        {
            sol::state_view view(ts);
            sol::table list = view.create_table();
            int n = 0;
            for (const Gameplay::SaveInfo& s : Gameplay::ListSaves())
            {
                sol::table row = view.create_table();
                row["slot"]            = s.slot;
                row["version"]         = s.version;
                row["sceneName"]       = s.sceneName;
                row["playtimeSeconds"] = s.playtimeSeconds;
                row["bytes"]           = static_cast<double>(s.bytes);
                row["loadable"]        = s.loadable;
                list[++n] = row;
            }
            return list;
        },
        { .doc = "Every save<N>.json in the save folder, ascending by slot: {slot, version, sceneName, playtimeSeconds, "
                 "bytes, loadable}. loadable is false for a file this build cannot read (a newer version, damaged JSON).",
          .returnsDoc = "a list of {slot, version, sceneName, playtimeSeconds, bytes, loadable}" });
    LuaDoc::Fn(engine, "Engine", "DeleteSave",
        [](sol::optional<int> slot) -> bool { return Gameplay::DeleteSave(slot.value_or(0)); },
        { .doc = "Removes the slot's save file; false when there was none (or the slot is negative).",
          .params = { { .name = "slot", .doc = "slot number, default 0" } },
          .effect = LuaDoc::Effect::Disk });

    // Engine.GetPlaytime() -> seconds (wall-clock while Playing; persisted)
    engine.set_function("GetPlaytime",
        []() -> double { return Gameplay::GameState::Get().GetPlaytime(); });
}

} // namespace Scene
