#pragma once

// SceneManager: the data-driven scene registry and load orchestrator.
// A scene is data: content (a .iscene file), behaviour (an optional Lua scene
// script with OnSceneEnter/Update/Exit) and identity (a name registered in the
// project manifest, game.json). Lua calls Scene.Load("name"); the manager
// resolves the name to a .iscene path, wraps the swap in the fade transition and
// tells ScriptSystem which scene script is active.
// App owns the World; SceneManager is a service wired with the subsystems a load
// needs. Threading: main thread only (the App loop, App's scene-swap drain, Lua
// bindings inside ScriptSystem::Update). ActivateScene loads blocking.

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <memory>

#include "Scene/ISceneTransition.h"   // Scene::TransitionParams (effect selection)
#include "Scene/SceneEvents.h"        // Scene::SceneLoaded/LoadFailedEvent (published here)

class IGraphicsDevice;
class Renderer;
class ScriptSystem;
class World;

namespace Resource    { class AssetManager; class AnimationClipSystem; struct ScenePreloadState; }
namespace DX12Physics { class PhysicsSystem; }
namespace Audio       { class AudioSystem; }

class SceneManager
{
public:
    // One registered scene: where its content + behaviour live.
    struct SceneEntry
    {
        std::string path;    // .iscene file (content)
        std::string script;  // Lua scene script: fallback when the .iscene declares no sceneScript
        // Scene-scoped Lua Systems: fallback when the .iscene declares no systems=.
        std::vector<std::string> systems;
        // Optional identity for `path` (32-char hex, Resource::AssetGuidIndex). The
        // path stays authoritative; the guid only rescues an entry whose scene moved.
        // game.json is hand-edited, so a stale guid must never win over a typed path.
        std::string guid;
    };

    // Scene-swap request: takes a scene id (a registry name OR a raw .iscene
    // path), exactly the string ActivateScene accepts.
    using SceneSwapFn = std::function<void(const std::string&)>;

    SceneManager() = default;

    // Wire the engine subsystems SceneManager needs to perform a load. Called
    // once by App during startup, after the systems exist.
    void Configure(IGraphicsDevice* gfx, Renderer* renderer, World* world,
                   Resource::AssetManager* assetMgr,
                   Resource::AnimationClipSystem* animClipSys,
                   ScriptSystem* scriptSys,
                   DX12Physics::PhysicsSystem* physics,
                   Audio::AudioSystem* audio);

    // Transition hooks (App owns the pending-swap slot and the fade manager):
    // begin wraps the swap in fade-out -> loading -> fade-in, replace swaps
    // instantly. Both take the scene id to switch to.
    void SetTransitionHooks(SceneSwapFn begin, SceneSwapFn replace);

    // ---- Registry (project manifest) ----
    // Parse game.json: "startup_scene" (name or path), the optional "scenes" map
    // { "name": "path.iscene" } or
    // { "name": { "path":..., "script":..., "systems": [...], "guid":... } },
    // "startup_state" (see StartupState), "name" and "engine_version". A manifest
    // with only "startup_scene" boots that path with an empty registry.
    // A missing manifest or a blank "startup_scene" is not an error: m_startupScene
    // falls back to Scene::kBaseScenePath so a fresh project boots the engine's base
    // world. Returns false when the manifest is missing or does not parse.
    bool LoadManifest(const std::string& gameJsonPath);

    // Declared engine version from the project manifest, for a mismatch
    // warning. Empty when the project does not state one.
    const std::string& EngineVersion() const { return m_engineVersion; }

    // The GameFlow state the project boots into ("startup_state"). Empty by
    // default, meaning "boot the startup scene". It is project identity like
    // startup_scene (sealed into game.ipak, never player-writable), so it lives in
    // game.json, not settings.json or engine.json. SceneManager only carries the
    // string to App, which owns boot order.
    // No fallback default: a guessed state name the project does not define would
    // leave an empty world for the rest of the run.
    const std::string& StartupState() const { return m_startupState; }

    // Resolve a registry name or a raw .iscene path to @p outPath and the manifest
    // script fallback @p outScript; a path equal to a registered entry's path
    // resolves to that entry. @p outName is the canonical scene name (the path stem
    // for an unregistered path). Returns false when @p nameOrPath is neither a
    // registered name nor an existing file.
    bool ResolveScene(const std::string& nameOrPath,
                      std::string& outPath, std::string& outScript,
                      std::string& outName) const;

    // The startup scene name or path from the manifest; Scene::kBaseScenePath when
    // the manifest names none (empty only before LoadManifest).
    const std::string& StartupScene() const { return m_startupScene; }

    // All registered scene names (for editor UI / Lua introspection).
    std::vector<std::string> SceneNames() const;

    // ---- Loading ----
    // BLOCKING: clear the World, load @p nameOrPath into it and mark its scene script
    // active, in the editor's safe world-reload order (WaitIdle ->
    // Renderer::OnWorldClear -> LoadScene -> navmesh). Called by App's swap drain.
    // A failed load is replaced by the base scene (still returns true) or, if that
    // fails too, by a code-built default world (returns false), so the viewport is
    // never black; SceneLoadFailedEvent reports each failure.
    bool ActivateScene(const std::string& nameOrPath);

    // Request a scene change from gameplay code (Lua Scene.Load): resolves the id,
    // starts the asset prewarm when fading, and routes the id through the transition
    // hooks; @p fade false cuts instantly.
    // Returns DISPATCH, not load success: the load runs frames later in ActivateScene
    // behind the cover. False when the id resolved to nothing (the literal path is
    // still dispatched and lands in the base-scene fallback if the file is gone), or
    // when no hooks are wired (nothing dispatched; SceneLoadFailedEvent published).
    // The real outcome is Scene::SceneLoadedEvent / Scene::SceneLoadFailedEvent
    // (SceneEvents.h); the bool only lets a caller fail fast on a mistyped name.
    bool RequestLoad(const std::string& nameOrPath, bool fade = true);

    // Overload carrying a transition-effect selection (Lua Scene.Load(name, opts)).
    // The transition manager takes the params at its next begin, so they apply only
    // when @p fade is true.
    bool RequestLoad(const std::string& nameOrPath, bool fade,
                     const Scene::TransitionParams& params);

    // ---- Explicit preload -> commit (game-driven loading screens) ----
    // The built-in transition's cross-frame asset prewarm, exposed so a game owns the
    // timing (mechanism here, policy in Lua):
    //   BeginPreload("level2")  -- start the async asset warm
    //   PreloadReady(pct)       -- poll each frame; draw your own loading art
    //   CommitPreloaded()       -- perform the instant swap
    // Only the asset warm is asynchronous: Resource::LoadScene still blocks, and
    // CommitPreloaded pays it in one frame; the prewarm only makes that frame short.
    // Progress is a real ready-handle count, but it covers only the RM handles named
    // by the scene's own top-level components. A scene assembled from SceneRef
    // instances names none and reports ready on the first poll while the commit still
    // stalls (~1 s for AIBench); covering them needs CollectPreloadPaths to recurse
    // into .iscn refs (a SceneSerializer change). The built-in transition bar has the
    // same gap.
    // A preload is only a warm cache: dropping it without committing is legal and
    // costs only the streamed bytes.
    void BeginPreload(const std::string& nameOrPath);

    // Poll the explicit preload: fills @p outProgress with the real 0..1 warm ratio
    // and returns true once it is safe to commit. Returns false with progress 0 when
    // no preload is in flight, including after a BeginPreload whose id did not
    // resolve (that one published SceneLoadFailedEvent).
    bool PreloadReady(float& outProgress) const;

    // Swap to the preloaded scene now, as an instant cut: the caller has been
    // drawing its own loading screen, so a built-in fade on top would be wrong.
    // Returns RequestLoad's dispatch result; false also when nothing was preloaded.
    bool CommitPreloaded();

    // The id handed to the last BeginPreload (empty when nothing is in flight).
    const std::string& PreloadTarget() const { return m_preloadRequest; }

    // ---- Transition / loading-bar seams (pulled by SceneTransitionManager) ----
    // Poll the in-flight scene's async asset prewarm: fills @p outProgress (0..1)
    // and returns true once warm enough to commit the blocking swap. Wired to
    // SceneTransitionManager::SetLoadPoll in App.
    bool PollActivePreload(float& outProgress);

    // Hand the pending transition params to the manager, one-shot: the next load
    // defaults to the plain fade. Wired to SceneTransitionManager::SetParamsProvider.
    Scene::TransitionParams TakePendingTransitionParams();

    // Re-load the current scene from disk (Scene.Reload()). Same return
    // contract as RequestLoad; false also when there is no current scene.
    bool Reload(bool fade = true);

    const std::string& CurrentScene() const { return m_currentName; }
    const std::string& CurrentScenePath() const { return m_currentPath; }

    // Companion assets the last ActivateScene resolved off the .iscene header:
    // the post-process config (.ippc) and the bound Lua scene script. The editor
    // adopts these after the boot load so File>Save Scene round-trips them
    // instead of silently stripping the bindings.
    const std::string& CurrentPostProcessPath() const { return m_currentPPCPath; }
    const std::string& CurrentSceneScript()     const { return m_currentScript; }
    const std::vector<std::string>& CurrentSceneSystems() const { return m_currentSystems; }

    // ---- Save-game plumbing (Gameplay/SaveGame) ----
    // World/asset access for PersistentTag entity capture and apply, and a one-shot
    // "apply slot after the next ActivateScene" latch for an Engine.LoadGame that
    // jumps scenes: saved entities can only be patched once the target world exists.
    // Consumed and reset in ActivateScene, before the scene script's OnSceneEnter.
    World*                         GetWorld()          const { return m_world; }
    Resource::AssetManager*        GetAssetManager()   const { return m_assetMgr; }
    Renderer*                      GetRenderer()       const { return m_renderer; }
    Resource::AnimationClipSystem* GetAnimClipSystem() const { return m_animClipSys; }
    void SetPendingSaveApply(int slot) { m_pendingSaveApplySlot = slot; }

private:
    // Wired subsystems (App-owned; valid for the whole session).
    IGraphicsDevice*               m_gfx         = nullptr;
    Renderer*                      m_renderer    = nullptr;
    World*                         m_world       = nullptr;
    Resource::AssetManager*        m_assetMgr    = nullptr;
    Resource::AnimationClipSystem* m_animClipSys = nullptr;
    ScriptSystem*                  m_scriptSys   = nullptr;
    DX12Physics::PhysicsSystem*    m_physics     = nullptr;
    Audio::AudioSystem*            m_audio       = nullptr;

    SceneSwapFn m_beginTransition;   // faded swap
    SceneSwapFn m_requestReplace;    // instant swap

    std::unordered_map<std::string, SceneEntry> m_registry;  // name -> entry
    std::string m_startupScene;
    std::string m_startupState;         // GameFlow state to boot into ("" = none)
    std::string m_engineVersion;        // manifest "engine_version" (empty = not stated)

    std::string m_currentName;         // active scene name (or path stem if unnamed)
    std::string m_currentPath;         // active .iscene path
    std::string m_currentPPCPath;      // active scene's .ippc (empty = none)
    std::string m_currentScript;       // active scene's Lua scene script (empty = none)
    std::vector<std::string> m_currentSystems;   // active scene's scene-scoped Lua Systems
    int         m_pendingSaveApplySlot = -1;   // save slot to apply post-activate (-1 = none)
    // The exact id last passed to ActivateScene (a registry name or a raw path).
    // Reload() reuses it: m_currentName is a lossy stem for path-loaded scenes and
    // would not re-resolve.
    std::string m_currentRequest;

    // Transition-effect selection for the next begin: set by RequestLoad, taken by
    // the manager's params provider, reset to the default fade after each take.
    Scene::TransitionParams m_pendingParams;

    // The in-flight async asset prewarm (set by RequestLoad's faded path, polled
    // by PollActivePreload each Loading frame). shared_ptr so the RM handles it
    // kicked stay referenced until LoadScene re-acquires them.
    std::shared_ptr<Resource::ScenePreloadState> m_activePreload;

    // The explicit (BeginPreload) prewarm, a separate slot from m_activePreload so
    // a game holding a preload for the next level does not lose it when another code
    // path starts a faded load.
    std::shared_ptr<Resource::ScenePreloadState> m_preload;
    std::string m_preloadRequest;             // id as passed to BeginPreload
    std::string m_preloadPath;                // resolved .iscene (logs + events)
    bool        m_preloadActive    = false;   // distinguishes "nothing to warm" from "no preload"
    // Kept alive across the commit: the handles are the only thing holding the
    // warmed assets in the ResourceManager until LoadScene re-acquires them.
    // Released at the end of the ActivateScene the commit produced.
    bool        m_preloadCommitted = false;
};
