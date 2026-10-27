#include "Scene/SceneManager.h"
#include "System/PathUtil.h"
#include "Resource/AssetGuid.h"   // optional guid rescue for a moved scene
#include "Scene/SceneDefaults.h"

#include "Resource/SceneSerializer.h"
#include "Resource/AssetManager.h"
#include "Resource/AssetFS.h"
#include "Renderer/Renderer.h"
#include "Graphics/IGraphicsDevice.h"
#include "Scene/SceneNavMesh.h"
#include "Scripting/ScriptSystem.h"
#include "Physics/PhysicsSystem.h"
#include "Gameplay/SaveGame.h"      // pending entity-snapshot apply after a scene jump
#include "Audio/AudioSystem.h"
#include "System/CommandLine.h"   // -worldclear_skip_sync (harness)
#include "System/EventBus.h"   // scene-load edges (Scene/SceneEvents.h)
#include "System/Log.h"
#include "System/ProcessMemory.h"   // the [MEM] line of each load
#include "System/PathUtil.h"

#include <nlohmann/json.hpp>
#include <fstream>

namespace
{
    // A scene id is a registry name or a raw .iscene path; this checks the path
    // case through AssetFS (pak and loose files).
    bool SceneFileExists(const std::string& path)
    {
        if (path.empty()) return false;
        // AssetFS::Exists, not HasInPak + Sys::FileExists: only it walks the ordered
        // search roots and the relocation shim, so a -scene= naming a pre-move path still
        // resolves against a pak that holds the scene under its new name.
        return ::Resource::AssetFS::Get().Exists(path);
    }

    // Separator-insensitive path comparison ('/' vs '\\'). Scene ids arrive from
    // game.json, Lua and file dialogs, so the same file can spell itself either
    // way; a raw == would let the base-scene fallback recurse into itself.
    bool SamePath(const std::string& a, const std::string& b)
    {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
        {
            const char ca = (a[i] == '\\') ? '/' : a[i];
            const char cb = (b[i] == '\\') ? '/' : b[i];
            if (ca != cb) return false;
        }
        return true;
    }

    // Basename without directory or extension: names an unregistered scene loaded
    // straight from a path.
    std::string StemOf(const std::string& path)
    {
        const size_t slash = path.find_last_of("/\\");
        const size_t start = (slash == std::string::npos) ? 0 : slash + 1;
        const size_t dot   = path.find_last_of('.');
        const size_t end   = (dot == std::string::npos || dot < start) ? path.size() : dot;
        return path.substr(start, end - start);
    }
}

void SceneManager::Configure(IGraphicsDevice* gfx, Renderer* renderer, World* world,
                             Resource::AssetManager* assetMgr,
                             Resource::AnimationClipSystem* animClipSys,
                             ScriptSystem* scriptSys,
                             DX12Physics::PhysicsSystem* physics,
                             Audio::AudioSystem* audio)
{
    m_gfx         = gfx;
    m_renderer    = renderer;
    m_world       = world;
    m_assetMgr    = assetMgr;
    m_animClipSys = animClipSys;
    m_scriptSys   = scriptSys;
    m_physics     = physics;
    m_audio       = audio;
}

void SceneManager::SetTransitionHooks(SceneSwapFn begin, SceneSwapFn replace)
{
    m_beginTransition = std::move(begin);
    m_requestReplace  = std::move(replace);
}

bool SceneManager::LoadManifest(const std::string& gameJsonPath)
{
    m_registry.clear();
    m_startupScene.clear();
    // Cleared with startup_scene (unlike m_engineVersion): both startup_* keys
    // decide what the process does first, so a re-parse that drops the key must not
    // leave the previous manifest driving boot.
    m_startupState.clear();

    std::string src;
    if (!::Resource::AssetFS::Get().ReadFileText(gameJsonPath, src))
    {
        // No manifest: still boot the engine's base scene.
        m_startupScene = Scene::kBaseScenePath;
        LOG_WARNING("SceneManager: no manifest '%s' — scene registry empty, "
                    "startup falls back to '%s'",
                    gameJsonPath.c_str(), m_startupScene.c_str());
        return false;
    }

    try
    {
        nlohmann::json j = nlohmann::json::parse(src);

        // game.json is the project file: it carries the project name and engine
        // version too, so there is no second manifest to keep in sync.
        if (j.contains("name") && j["name"].is_string())
            Sys::SetProjectName(j["name"].get<std::string>());
        if (j.contains("engine_version") && j["engine_version"].is_string())
            m_engineVersion = j["engine_version"].get<std::string>();

        if (j.contains("startup_scene") && j["startup_scene"].is_string())
            m_startupScene = j["startup_scene"].get<std::string>();
        // Optional identity for startup_scene (see SceneEntry::guid): a typed path
        // always wins, the guid only rescues a moved scene.
        if (j.contains("startup_scene_guid") && j["startup_scene_guid"].is_string())
            m_startupScene = ::Resource::ResolveStoredPath(
                m_startupScene, j["startup_scene_guid"].get<std::string>(), "startup scene");

        // A GameFlow state name: states are declared in Lua, not stored on disk, so no
        // guid twin and no path resolution. Absent or non-string leaves it empty, which
        // App reads as "boot startup_scene". Whether the state exists is App's check: the
        // flow graph is not finalized when this runs.
        if (j.contains("startup_state") && j["startup_state"].is_string())
            m_startupState = j["startup_state"].get<std::string>();

        // "scenes" : { "name" : "path.iscene" }                                      (path-only form)
        //          or { "name" : { "path":.., "script":.., "systems":[..], "guid":.. } } (full form)
        if (j.contains("scenes") && j["scenes"].is_object())
        {
            for (auto& kv : j["scenes"].items())
            {
                const std::string& name = kv.key();
                const nlohmann::json& val = kv.value();
                SceneEntry e;
                if (val.is_string())
                {
                    e.path = val.get<std::string>();
                }
                else if (val.is_object())
                {
                    if (val.contains("path")   && val["path"].is_string())
                        e.path = val["path"].get<std::string>();
                    if (val.contains("script") && val["script"].is_string())
                        e.script = val["script"].get<std::string>();
                    if (val.contains("systems"))
                    {
                        if (val["systems"].is_array())
                        {
                            for (const auto& s : val["systems"])
                                if (s.is_string()) e.systems.push_back(s.get<std::string>());
                        }
                        else
                            LOG_WARNING("SceneManager: scene '%s' `systems` must be an array of "
                                        "System names — ignored", name.c_str());
                    }
                    if (val.contains("guid")   && val["guid"].is_string())
                        e.guid = val["guid"].get<std::string>();
                }
                if (!e.guid.empty())
                    e.path = ::Resource::ResolveStoredPath(e.path, e.guid, "registered scene");
                if (!e.path.empty())
                    m_registry[name] = std::move(e);
                else
                    LOG_WARNING("SceneManager: scene '%s' has no path — skipped", name.c_str());
            }
        }
    }
    catch (const std::exception& ex)
    {
        LOG_ERROR("SceneManager: failed to parse '%s': %s", gameJsonPath.c_str(), ex.what());
        m_startupScene = Scene::kBaseScenePath;
        return false;
    }

    // A manifest without a usable startup_scene boots the engine base scene rather
    // than an empty world; the default lives here (and in App's boot), never in a
    // C++ scene class.
    if (m_startupScene.empty())
    {
        m_startupScene = Scene::kBaseScenePath;
        LOG_INFO("SceneManager: manifest declares no startup_scene — using base scene '%s'",
                 m_startupScene.c_str());
    }

    LOG_INFO("SceneManager: manifest loaded — %zu registered scene(s), startup='%s'",
             m_registry.size(), m_startupScene.c_str());
    // Logged separately and only when set: on a flow-driven boot the line above
    // names a scene that is never pushed, which without this line reads as "the
    // startup scene failed to load".
    if (!m_startupState.empty())
        LOG_INFO("SceneManager: manifest declares startup_state='%s' — App boots "
                 "through the GameFlow and the startup scene above is only the "
                 "fallback", m_startupState.c_str());
    return true;
}

bool SceneManager::ResolveScene(const std::string& nameOrPath,
                                std::string& outPath, std::string& outScript,
                                std::string& outName) const
{
    auto it = m_registry.find(nameOrPath);
    if (it != m_registry.end())
    {
        outPath   = it->second.path;
        outScript = it->second.script;
        outName   = it->first;
        return true;
    }
    // Not a registered name: if the string is a registered entry's path, resolve to
    // that entry so its manifest script still binds (the startup_scene case:
    // game.json names a raw .iscene path).
    auto normalize = [](std::string s) {
        for (char& c : s) if (c == '\\') c = '/';
        return s;
    };
    const std::string wanted = normalize(nameOrPath);
    for (const auto& kv : m_registry)
    {
        if (normalize(kv.second.path) == wanted)
        {
            outPath   = kv.second.path;
            outScript = kv.second.script;
            outName   = kv.first;
            return true;
        }
    }

    // Accept it as a literal .iscene path if it exists.
    if (SceneFileExists(nameOrPath))
    {
        outPath = nameOrPath;
        outScript.clear();
        outName = StemOf(nameOrPath);
        return true;
    }
    return false;
}

std::vector<std::string> SceneManager::SceneNames() const
{
    std::vector<std::string> names;
    names.reserve(m_registry.size());
    for (auto& kv : m_registry) names.push_back(kv.first);
    return names;
}

bool SceneManager::ActivateScene(const std::string& nameOrPath)
{
    if (!m_world)
    {
        LOG_ERROR("SceneManager: ActivateScene('%s') with no World wired", nameOrPath.c_str());
        // Reachable in a headless or mis-wired host. Publish the failure: no
        // SceneLoadedEvent follows on this path, and a waiter that only hears about
        // successes would wait forever.
        EventBus::Get().Publish(Scene::SceneLoadFailedEvent{
            nameOrPath, nameOrPath, "no World wired (SceneManager::Configure not called)" });
        return false;
    }

    // Remember the exact request for Reload(): m_currentName is only a lossy stem
    // for path-loaded scenes.
    m_currentRequest = nameOrPath;

    std::string path, scriptFallback, name;
    if (!ResolveScene(nameOrPath, path, scriptFallback, name))
    {
        // Unresolved: try the id as a literal path; a missing file fails into the
        // fallback below.
        path = nameOrPath;
        name = StemOf(nameOrPath);
        scriptFallback.clear();
    }

    LOG_INFO("SceneManager: activating scene '%s' (path='%s')", name.c_str(), path.c_str());

    // What was asked for: the base-scene fallback rewrites `name`/`path`, and a
    // failure event naming the substitute would be useless to whoever waits on this
    // load.
    const std::string requestedName = name;
    const std::string requestedPath = path;

    // Safe world-reload order (as in EditorLayer::ProcessPendingActions):
    //   1. hard-sync every queue and drain deferred releases
    //   2. let the Renderer stash its caches for deferred release
    //   3. LoadScene (clears the World itself)
    // Do not swap 1 and 2: bindless SRV slots could reach just-freed memory and cause
    // a delayed GPU TDR.
    // The outgoing scene ends while its world still exists: OnSceneExit and its scene-scoped
    // Systems' OnShutdown see the scene they belong to, and what its scripts own is released
    // before a timer of theirs can fire into the next one.
    if (m_scriptSys) m_scriptSys->ExitScene(/*worldSwap=*/true);

#if defined(WITH_HARNESS) && WITH_HARNESS
    // -worldclear_skip_sync: the reload idles only the graphics queue (FlushAndWait) instead of the hard
    // sync, the caller mistake Renderer::OnWorldClear's check catches (the world_clear_sync pack).
    static const bool s_skipSync = Sys::CommandLine::Get().Has("-worldclear_skip_sync");
#else
    constexpr bool s_skipSync = false;
#endif
    if (m_gfx && s_skipSync)
    {
        m_gfx->FlushAndWait();
        LOG_WARNING("[WORLDCLEAR_PROBE] -worldclear_skip_sync: '%s' is loaded after FlushAndWait only",
                    name.c_str());
    }
    else if (m_gfx)
    {
        m_gfx->WaitIdleAndReleaseDeferred();
    }
    if (m_renderer) m_renderer->OnWorldClear();
    // Destroy the outgoing scene's Jolt bodies before the World is cleared:
    // World::Clear recycles entity ids without firing destroy listeners, so the
    // per-entity reaper misses bodies on ids reused with a RigidBody next scene; they
    // would leak as ghost colliders and body-pool slots.
    if (m_physics)  m_physics->OnWorldClear();
    // Stop and release the outgoing scene's audio voices before the World is
    // cleared, for the same reason: AudioSystem::OnEntityDestroyed never runs on
    // World::Clear, so a looping BGM (or any playing AudioSourceComponent) would keep
    // sounding into the next scene.
    if (m_audio)    m_audio->OnWorldClear();

    std::string loadedName, ppcPath, navPath, sceneScript;
    std::vector<std::string> sceneSystems;
    bool ok = m_assetMgr
        ? ::Resource::LoadScene(path, *m_world, *m_assetMgr, m_renderer, m_animClipSys,
                                &loadedName, &ppcPath, &navPath, &sceneScript, &sceneSystems)
        : false;

    // Two-step fallback: a missing or broken scene retries the engine's base scene
    // (real data: camera, light, sky, a cube); only if that also fails is a minimal
    // world built in code. The SamePath guard keeps a broken base scene from being
    // retried as its own fallback.
    if (!ok && m_assetMgr && !SamePath(path, Scene::kBaseScenePath))
    {
        LOG_ERROR("SceneManager: LoadScene('%s') failed — falling back to base scene '%s'",
                  path.c_str(), Scene::kBaseScenePath);
        EventBus::Get().Publish(Scene::SceneLoadFailedEvent{
            requestedName, requestedPath, "LoadScene failed — substituting the base scene" });
        m_world->Clear();
        sceneSystems.clear();
        ok = ::Resource::LoadScene(Scene::kBaseScenePath, *m_world, *m_assetMgr,
                                   m_renderer, m_animClipSys,
                                   &loadedName, &ppcPath, &navPath, &sceneScript, &sceneSystems);
        if (ok)
        {
            path = Scene::kBaseScenePath;
            name = StemOf(path);
            // Do not bind the requested scene's manifest script to the substituted
            // base scene: its OnSceneEnter would run against a world that has none of
            // the entities it expects.
            scriptFallback.clear();
        }
    }

    if (!ok)
    {
        LOG_ERROR("SceneManager: LoadScene('%s') failed — spawning default world", path.c_str());
        // Still the requested scene: `path` is rewritten only when the base-scene
        // fallback succeeds, and here it failed or was never tried (no AssetManager, or
        // the request was the base scene). If the branch above also published, these
        // are two distinct failures, told apart by their reason strings.
        EventBus::Get().Publish(Scene::SceneLoadFailedEvent{
            requestedName, requestedPath,
            "no scene could be loaded — synthesised default world" });
        m_world->Clear();
        Scene::SpawnDefaultWorld(*m_world);
        ppcPath.clear();
        sceneScript.clear();
        sceneSystems.clear();
    }
    else if (!navPath.empty())
    {
        // Companion navmesh: load it so runtime FindPath / NavAgent work (the Nav module's;
        // without it only the path is kept).
        if (!SceneNavMesh::LoadForScene(navPath))
            LOG_ERROR("SceneManager: navmesh '%s' failed to load — AI path-following off",
                      navPath.c_str());
    }

    // A loaded scene with no WorldEnvironmentComponent gets one synthesised from its
    // AtmosphereComponent / SkyboxComponent so it renders unchanged; otherwise a
    // no-op. The !ok path was already seeded by SpawnDefaultWorld.
    if (ok)
        Scene::MigrateEnvironment(*m_world);

    // A LoadGame that jumped to this scene queued its entity snapshots: apply them
    // now that the world exists, and before the scene script binds so OnSceneEnter
    // sees the restored state.
    if (m_pendingSaveApplySlot >= 0)
    {
        if (ok)
            Gameplay::ApplySavedEntities(m_pendingSaveApplySlot, *m_world, m_assetMgr,
                                         m_renderer, m_animClipSys);
        m_pendingSaveApplySlot = -1;
    }

    // The .iscene's own sceneScript and systems= win over the manifest's for this scene
    // (each on its own). The manifest systems apply only to the file their entry names, so a
    // substituted base scene does not inherit the requested scene's. An empty path binds no
    // script.
    const std::string finalScript = !sceneScript.empty() ? sceneScript : scriptFallback;
    std::vector<std::string> finalSystems = sceneSystems;
    if (finalSystems.empty() && ok)
        if (auto it = m_registry.find(name); it != m_registry.end() && SamePath(it->second.path, path))
            finalSystems = it->second.systems;
    if (m_scriptSys)
        m_scriptSys->SetActiveSceneScript(finalScript, name, finalSystems);

    m_currentName    = name;
    m_currentPath    = path;
    m_currentPPCPath = ppcPath;
    m_currentScript  = finalScript;
    m_currentSystems = finalSystems;

    // A committed preload (this activation came from CommitPreloaded) is done:
    // LoadScene re-acquired every warmed asset, so the RM holds its own references
    // and these handles only keep a second ref count alive.
    if (m_preloadCommitted)
    {
        m_preload.reset();
        m_preloadRequest.clear();
        m_preloadPath.clear();
        m_preloadCommitted = false;
    }

    // The load edge, published last on purpose: Publish is queued, so subscribers
    // run at the next DispatchAll, when the World, the current name/path and the
    // scene script are all the new scene's and no handler can see a half-swapped
    // state. Also published on failure (ok=false): a Loading state needs exactly one
    // guaranteed edge to leave on (see SceneEvents.h).
    EventBus::Get().Publish(Scene::SceneLoadedEvent{ name, path, ok });
    const std::string tag = "scene loaded: " + name;
    Sys::LogProcessMemory(tag.c_str());
    Resource::AssetFS::Get().LogStats(tag.c_str());
    return ok;
}

bool SceneManager::RequestLoad(const std::string& nameOrPath, bool fade)
{
    return RequestLoad(nameOrPath, fade, Scene::TransitionParams{});
}

bool SceneManager::RequestLoad(const std::string& nameOrPath, bool fade,
                               const Scene::TransitionParams& params)
{
    // Resolve for a clearer log and a concrete path for the prewarm (the load itself
    // still falls back if the file is missing).
    std::string path, script, name;
    bool resolved = true;
    if (!ResolveScene(nameOrPath, path, script, name))
    {
        LOG_WARNING("SceneManager: Scene.Load('%s') — not a registered name or existing "
                    "path; attempting as a literal path", nameOrPath.c_str());
        path = nameOrPath; // literal-path attempt (mirrors ActivateScene's fallback)
        // Reported to the caller, but the attempt still goes through: AssetFS may learn
        // the path later (a pak mounted since), and dropping the request here would
        // change behaviour callers rely on.
        resolved = false;
    }

    // Start the read-only asset prewarm now so textures and anims stream in during
    // the cover fade; by the Loading phase most are warm and the bar tracks the rest.
    // Only the faded path shows a bar, so only it prewarms.
    m_pendingParams = params;
    if (fade && m_assetMgr && !path.empty())
        m_activePreload = Resource::BeginScenePreload(path, *m_assetMgr);
    else
        m_activePreload.reset();

    // Dispatch the id, not the resolved path: ActivateScene resolves it again.
    if (fade && m_beginTransition)
        m_beginTransition(nameOrPath);
    else if (m_requestReplace)
        m_requestReplace(nameOrPath);
    else
    {
        LOG_ERROR("SceneManager: no transition hooks wired — cannot load '%s'",
                  nameOrPath.c_str());
        // Nothing was dispatched, so no ActivateScene runs and no SceneLoadedEvent
        // arrives: without this event a waiting Loading state would hang forever.
        EventBus::Get().Publish(Scene::SceneLoadFailedEvent{
            name.empty() ? nameOrPath : name, path,
            "no transition hooks wired (SceneManager::SetTransitionHooks not called)" });
        return false;
    }
    return resolved;
}

void SceneManager::BeginPreload(const std::string& nameOrPath)
{
    if (nameOrPath.empty())
    {
        LOG_WARNING("SceneManager: BeginPreload with an empty scene id — ignored");
        return;
    }

    std::string path, script, name;
    if (!ResolveScene(nameOrPath, path, script, name))
    {
        // Unlike RequestLoad, a preload has no fallback: there is no world to
        // substitute, only a poll that would never become ready. Fail loudly here so the
        // caller's flow can branch now.
        LOG_ERROR("SceneManager: BeginPreload('%s') — not a registered name or existing path",
                  nameOrPath.c_str());
        EventBus::Get().Publish(Scene::SceneLoadFailedEvent{
            nameOrPath, nameOrPath, "preload target did not resolve" });
        m_preload.reset();
        m_preloadRequest.clear();
        m_preloadPath.clear();
        m_preloadActive    = false;
        m_preloadCommitted = false;
        return;
    }

    if (m_preloadActive && m_preloadRequest != nameOrPath)
        LOG_INFO("SceneManager: BeginPreload('%s') replaces the in-flight preload of '%s'",
                 nameOrPath.c_str(), m_preloadRequest.c_str());

    m_preloadRequest   = nameOrPath;   // the id, not the path: CommitPreloaded
    m_preloadPath      = path;         // re-resolves it exactly like Reload does
    m_preloadCommitted = false;
    m_preloadActive    = true;
    // Null is legitimate (unreadable file, not a world asset): it means "nothing to
    // warm", not "not in flight", hence the separate m_preloadActive flag. The commit
    // still runs and reports the real outcome through the load events.
    m_preload = m_assetMgr ? Resource::BeginScenePreload(path, *m_assetMgr) : nullptr;

    LOG_INFO("SceneManager: preloading scene '%s' (path='%s')", name.c_str(), path.c_str());
}

bool SceneManager::PreloadReady(float& outProgress) const
{
    if (!m_preloadActive) { outProgress = 0.f; return false; }
    if (!m_assetMgr)      { outProgress = 1.f; return true;  }
    return Resource::PollScenePreload(m_preload, *m_assetMgr, outProgress);
}

bool SceneManager::CommitPreloaded()
{
    if (!m_preloadActive)
    {
        LOG_WARNING("SceneManager: CommitPreloaded with no preload in flight");
        return false;
    }

    const std::string request = m_preloadRequest;
    m_preloadActive    = false;
    m_preloadCommitted = true;   // m_preload stays alive until ActivateScene ends

    // Instant cut, not a fade: the caller owns the loading screen it has drawn since
    // BeginPreload, and the built-in cover on top would fade out art that already
    // covers the screen.
    const bool dispatched = RequestLoad(request, /*fade=*/false);
    if (!dispatched)
    {
        // Nothing will activate, so nothing will release the handles.
        m_preload.reset();
        m_preloadRequest.clear();
        m_preloadPath.clear();
        m_preloadCommitted = false;
    }
    return dispatched;
}

bool SceneManager::PollActivePreload(float& outProgress)
{
    if (!m_assetMgr) { outProgress = 1.f; return true; }
    return Resource::PollScenePreload(m_activePreload, *m_assetMgr, outProgress);
}

Scene::TransitionParams SceneManager::TakePendingTransitionParams()
{
    Scene::TransitionParams p = m_pendingParams;
    m_pendingParams = Scene::TransitionParams{}; // one-shot; next load defaults to fade
    return p;
}

bool SceneManager::Reload(bool fade)
{
    // Reuse the original request id (name or path), not m_currentName: that is a
    // lossy stem for path-loaded scenes and would not re-resolve, dropping the reload
    // into the base-scene fallback.
    if (m_currentRequest.empty())
    {
        LOG_WARNING("SceneManager: Reload with no current scene");
        return false;
    }
    return RequestLoad(m_currentRequest, fade);
}
