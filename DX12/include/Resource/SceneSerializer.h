#pragma once

// SceneSerializer - save / load the whole ECS World to / from a .iscene binary file.
// The disk format is a scene asset, the runtime container is the World: SaveScene(World&, path)
// saves the live world into a scene asset, LoadScene(path, World&) loads one into it (the argument
// order is the direction of the copy).
//
// .iscene blob layout:
//   [AssetHeader (24 B)] [world metadata (16 B)] [line-based text payload (null-terminated)]
//
// Text payload format (generic component serialization via ComponentSerializerRegistry):
//   W name=<scene-name> [postProcessConfig=<path>] [navMesh=<path>] [sceneScript=<path>]
//     [systems=<name>,<name>...]
//   N idx=<i> parent=<j|-1> tx=.. ty=.. tz=.. qx=.. qy=.. qz=.. qw=.. sx=.. sy=.. sz=..
//     name=<percent-encoded-name>
//   <2-space indented component lines, same as .ipfb>
//     Tag: key=val key=val ...
//
// Entities a SceneRef generates from its .iscn are not stored; the load regenerates them.
// Every root entity and its subtree is saved; a load clears the world and rebuilds it.

#include "ECS/ECS.h"
#include <string>
#include <memory>
#include <vector>

class Renderer;

namespace Resource
{
    class AssetManager;
    class AnimationClipSystem;

    // Serialize the whole World to a .iscene file. @p sceneName is stored as metadata (editor title
    // bar). @p postProcessConfigPath, @p navMeshPath and @p sceneScript (each optional) go on the
    // W line; LoadScene loads and applies the .ippc after the world is restored. @p sceneScript is a
    // Lua scene script: a singleton bound to the scene (not an entity) whose OnSceneEnter /
    // OnSceneUpdate(dt) / OnSceneExit hooks drive the scene flow; LoadScene returns it in
    // outSceneScript so SceneManager can activate it. @p sceneSystems names the scene-scoped Lua
    // Systems the scene runs (W-line systems=). @p errorCode (optional) names a failure:
    // "empty_world", "open_failed" or "write_failed" (the stream failed after the open, e.g. a full
    // disk; the file on disk is then incomplete).
    bool SaveScene(World& world, const std::string& path,
                   const std::string& sceneName = "Untitled",
                   const std::string& postProcessConfigPath = "",
                   const std::string& navMeshPath = "",
                   const std::string& sceneScript = "",
                   std::string* errorCode = nullptr,
                   const std::vector<std::string>& sceneSystems = {});

    // Deserialize a .iscene file, CLEAR the world, then rebuild every entity. When the scene names a
    // .ippc and @p renderer is given, that post-process config is loaded and applied in the same call.
    // Returns true on success. @p outPostProcessConfigPath (optional) receives the stored .ippc path,
    // applied or not; @p outNavMeshPath receives the stored .inav path - the caller feeds it to
    // NavMeshSystem::Load. @p outSceneSystems receives the W-line systems= list (empty when absent).
    bool LoadScene(const std::string& path, World& world, AssetManager& assetMgr,
                   Renderer* renderer = nullptr,
                   AnimationClipSystem* animClipSys = nullptr,
                   std::string* outSceneName = nullptr,
                   std::string* outPostProcessConfigPath = nullptr,
                   std::string* outNavMeshPath = nullptr,
                   std::string* outSceneScript = nullptr,
                   std::vector<std::string>* outSceneSystems = nullptr);

    // Cross-frame asset prewarm (BeginScenePreload / PollScenePreload): the blocking LoadScene cannot
    // report progress, so a scene transition first runs this read-only prewarm across frames to drive a
    // real loading bar, then calls LoadScene, whose internal wait then returns at once.
    struct ScenePreloadState;                       // opaque; defined in the .cpp
    using ScenePreload = std::shared_ptr<ScenePreloadState>;

    // ==== Section keys in a scene or prefab file ====
    // StampSceneKeys keys the lines that name a model part or a collider mesh. A part override line
    // (MC / FS / MO / MR / MRI / MOI: src=<library> srcMesh=<entry>) gets srcSec=<SectionKey> right
    // after srcMesh=; a collider mesh (an MC line's, a Collider: line's, a Mesh / ConvexHull shape)
    // gets meshSec= right after meshId=. Keys come from the library file (MeshLibKeyTableAt); a keyed
    // line whose entry hint is stale gets the hint refreshed from the key. In a prefab, each N line
    // that is a part of a SceneRef'd model gets msec= before name= (as SavePrefab writes it): the key
    // of the model's placement at that node's DFS ordinal, the mapping LoadPrefab applies. An N line
    // already keyed is left alone (a different key means the model was reordered; the key follows the
    // part). Text edits only - nothing else moves; dry run unless @p apply; idempotent. LoadScene
    // resolves the keys first; SaveScene writes them.
    struct SceneKeyReport
    {
        struct Counts
        {
            uint32_t lines      = 0;   // lines naming a part / a collider mesh
            uint32_t added      = 0;   // got a key
            uint32_t kept       = 0;   // keyed, hint agrees
            uint32_t hintFixed  = 0;   // keyed; the entry hint was stale and now names the key's
            uint32_t dangling   = 0;   // keyed with a key the library does not have (left alone)
            uint32_t unresolved = 0;   // the library does not read / the entry is out of range
        };
        Counts part;       // srcSec=
        Counts collider;   // meshSec=
        Counts prefabPart; // msec= (.ipfb): kept = keyed already, dangling = keyed differently
        bool   changed = false;
        bool   written = false;
    };
    // @p path: an .iscene (MAGIC_WORLD) or .ipfb (MAGIC_PREFAB).
    bool StampSceneKeys(const std::string& path, bool apply, SceneKeyReport& report,
                        std::string* outError = nullptr);

    // True when @p e's Collider + RigidBody are what its model gave it at spawn (ColliderSpawnDefault;
    // within an MC line's %.4f precision), i.e. not an override. @p outFromAsset: that came from the
    // model's asset collision. False when it has none of them, or was given nothing.
    bool ColliderIsSpawnDefault(World& world, Entity e, bool* outFromAsset = nullptr);

    // StripColliderEchoes drops the MC lines of @p path (.iscene / .ipfb) that only restate what the
    // models' C records already give every part they reach. An MC line reaches every part, of every
    // model the file places (its SceneRef lines and, recursively, its PrefabRef'd prefabs'), whose
    // (library, entry) it names. It is an echo when every such part's F-line has a C record, the first
    // record the spawn accepts is the line's collider (within %.4f) and the line's body is the spawn's
    // (RigidBodyComponent, motion Static). Kept: a line reaching a part without a C record (an authored
    // collider, e.g. Bistro's "Use baked as collision" lines), one that differs, one no placed model
    // reaches, one on a library a StaticMesh of the file draws from (its derived sections carry the same
    // (library, entry)), one that does not parse. Run it before the models' C records are migrated
    // (SceneInstanceLoader::MigrateModelCollision): after that the line would override the asset
    // collision. Text edits only; dry run unless @p apply; idempotent.
    struct ColliderEchoReport
    {
        uint32_t mcLines    = 0;   // MC lines in the file
        uint32_t echoes     = 0;   // dropped
        uint32_t authored   = 0;   // kept: a part it reaches has no C record
        uint32_t differs    = 0;   // kept: not what the C record gives
        uint32_t unplaced   = 0;   // kept: no model of the file places its part
        uint32_t staticMesh = 0;   // kept: a StaticMesh of the file draws from its library
        uint32_t unparsed   = 0;   // kept: the line does not parse
        std::vector<std::string> models;   // the .iscn models the file places
        bool     changed = false;
        bool     written = false;
    };
    bool StripColliderEchoes(const std::string& path, bool apply, ColliderEchoReport& report,
                             std::string* outError = nullptr);

    // Parse @p path's asset references and kick their async loads (RM textures /
    // anims + .iscn OS page-cache warmups). Does NOT touch the World. Returns an
    // opaque handle to poll, or null if the file can't be read (caller then just
    // proceeds straight to LoadScene).
    ScenePreload BeginScenePreload(const std::string& path, AssetManager& assetMgr);

    // Pump one frame of a prewarm: run the main-thread GPU-upload pump and recount
    // ready handles. Sets @p outProgress to [0,1] (1 when there is nothing to
    // preload). Returns true once every preload is ready (or @p pre is null/empty).
    bool PollScenePreload(const ScenePreload& pre, AssetManager& assetMgr, float& outProgress);
}
