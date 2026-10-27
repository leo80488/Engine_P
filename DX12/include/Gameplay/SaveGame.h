#pragma once

// SaveGame - save-game persistence for Gameplay::GameState.
//
// Serializes the GameState maps (numbers / strings / items), playtime and the active scene's
// identity to a versioned JSON slot file, save<slot>.json under Sys::ProjectDataDir()
// (SaveSlotPath). JSON deliberately: saves stay readable and hand-editable.
// Writes are atomic: save<slot>.json.tmp is renamed over the target (MSVC fs::rename ->
// MoveFileEx(REPLACE_EXISTING)), so a crash mid-save never leaves a truncated save.
// Versions: the file carries "version"; the reader lifts an older document to the current one through
// a chain of steps (one per version, SaveGame.cpp), and refuses a newer one without touching
// GameState. Lua: Engine.ListSaves(), Engine.DeleteSave([slot]).
// Lua (LuaSceneBindings.cpp, which has the SceneManager for the scene identity):
// Engine.SaveGame([slot]), Engine.LoadGame([slot][, alsoLoadScene=true]),
// Engine.HasSaveGame([slot]), Engine.GetPlaytime().
//
// Entity snapshots: entities carrying PersistentTag are captured keyed by their Guid -
// LocalTransform plus a curated WHITELIST of registry components (kEntitySnapshotTags: among them the
// entity's Attributes, so Health comes back as it was). The
// whitelist matters: ref-spawning loaders (SceneRef / PrefabRef) would re-instantiate whole
// subtrees onto a LIVE entity, so they are never applied. RigidBody is not on it either: a save
// carries no physics state, and its loader resets bodyId, so PhysicsSystem would rebuild the live
// body.
// On load, snapshots PATCH matching entities in the current world (Guid first, unique-name
// fallback) through ComponentWrite, so a patched component keeps its runtime state (a light its
// gather order); entities missing from the world are skipped with a log.
//
// A PersistentTag entity should also carry a scene-serialized GuidComponent (re-save the scene
// after tagging): a Guid stamped only at save time cannot be matched after an app restart, and
// the name fallback carries it only while the name stays unique.

// Spawn / destroy tracking: PersistentTag entities destroyed at runtime land in GameState's
// graveyard (the InstallPersistenceHooks destroy listener) and are RE-DESTROYED after load;
// runtime-created entities marked with Engine.MakePersistent (PersistentTag + RuntimeSpawnedTag +
// Guid) are saved with their PrefabSourcePath and RE-INSTANTIATED on load when their Guid is not
// live. Runtime spawns without a PrefabSourcePath cannot be recreated and are skipped with a
// warning at save time.

#include <cstdint>
#include <string>
#include <vector>

class World;
class Renderer;
namespace Resource { class AssetManager; class AnimationClipSystem; }

namespace Gameplay
{
    class GameFlowStateMachine;

    // Installs the World destroy listener that records destroyed PersistentTag entities into
    // GameState's graveyard. Idempotent per world; call once at App init next to
    // GuidRegistry::RegisterWithWorld (the Guid must still be readable, and listeners fire BEFORE
    // components are erased).
    void InstallPersistenceHooks(World& world);
    // "save<slot>.json"; slot clamps at 0.
    std::string SaveSlotPath(int slot);

    bool SaveGameExists(int slot);

    // One save<N>.json in the save folder, as a slot picker shows it.
    struct SaveInfo
    {
        int           slot            = 0;
        int           version         = 0;       // the version the file was written at; 0 = not a save document
        std::string   sceneName;
        double        playtimeSeconds = 0.0;
        std::uintmax_t bytes          = 0;
        bool          loadable        = false;   // LoadGame would accept it (a known version, valid JSON)
    };
    // Every save<N>.json, ascending by slot. A file this build cannot read is listed too, with
    // loadable = false, so a picker can say so instead of hiding it.
    std::vector<SaveInfo> ListSaves();
    // Removes slot's file (and a half-written .tmp). False when there was none or slot < 0.
    bool DeleteSave(int slot);

    // Serializes the CURRENT GameState, playtime and scene identity, plus - when `world` is non-null -
    // a snapshot of every PersistentTag entity. Returns false (with a log) on I/O failure; the
    // previous save survives. `flow`, when given, adds a "flow" record: the current state path and the
    // params it was entered with. Without it a load restores the world and the scene but drops the
    // player wherever the boot state puts them (save while Paused over InGame, quit, load, and the
    // pause menu is gone along with every entity that state owned).
    bool SaveGame(int slot, const std::string& sceneName, const std::string& scenePath,
                  World* world = nullptr,
                  const GameFlowStateMachine* flow = nullptr);

    // Parses the slot file and REPLACES GameState's contents and playtime; GameState is untouched
    // when parsing fails. outSceneName / outScenePath (optional) receive the stored scene identity
    // ("" when absent). Entity snapshots are NOT applied here: call ApplySavedEntities once the right
    // world is live (SceneManager does this through its pending-apply latch when LoadGame jumps
    // scenes). `flow`, when given, is handed the saved leaf via QueueRestore - NOT switched here: the
    // states being restored spawn entities into a world that does not exist yet; App applies the
    // queued restore when the scene load completes.
    bool LoadGame(int slot, std::string* outSceneName, std::string* outScenePath,
                  GameFlowStateMachine* flow = nullptr);

    // Applies the slot file's entity record onto the current world, in order:
    //   1. patch scene-authored PersistentTag entities,
    //   2. destroy scene entities whose Guid is in the save's graveyard,
    //   3. re-instantiate runtime-spawned entities (spawned with the saved Guid as seed, the Guid
    //      re-stamped, the snapshot patched) whose Guid is not live.
    // renderer / animClipSys feed the prefab spawn (pass what the scene loader has; null degrades
    // prefab visuals the same way it does for Engine.SpawnPrefab).
    // Returns the number of entities touched (patched + destroyed + spawned).
    int ApplySavedEntities(int slot, World& world, Resource::AssetManager* assetMgr,
                           Renderer* renderer, Resource::AnimationClipSystem* animClipSys);

} // namespace Gameplay
