#include "Scene/BootScene.h"

#include "Scene/SceneManager.h"
#include "Scene/SceneDefaults.h"   // Scene::kBaseScenePath (the default start scene)
#include "System/BootTimeline.h"   // bootPhasesMs marks
#include "System/CommandLine.h"
#include "System/HostPolicy.h"     // IsEditorHost: the editor adopts the boot scene at boot
#include "System/Log.h"
#include "Harness/ExitCodes.h"
#include "Harness/RunLedger.h"

#include <string>

bool Scene::BootThroughTransition()
{
#ifdef WITH_HARNESS
    static const bool s_forced = Sys::CommandLine::Get().Has("-boot_transition");
    if (s_forced)
        return true;
#endif
    const Sys::CommandLine& cl = Sys::CommandLine::Get();
    return !Sys::IsEditorHost() && !cl.Has("-unattended") && !cl.Has("-hidden");
}

// Every build boots the data-driven startup scene through SceneManager (see the declaration).
void Scene::ActivateBootScene(SceneManager& sceneManager, bool bootScriptsRefused,
                              bool flowOwnsBootScene,
                              const std::function<void(const std::string&)>& deferLoad,
                              bool pipelinesPreparing)
{
    std::string startup = sceneManager.StartupScene();
    bool sceneResolved  = true;

    // -scene=<name-or-path> overrides game.json's startup_scene, so an
    // automated run can pin the world it is about to assert on without
    // editing a tracked config file out from under the user.
    const std::string cliScene = Sys::CommandLine::Get().Get("-scene");
    if (!cliScene.empty())
    {
        // Resolve explicitly, before the load: nothing downstream fails the run on an unresolvable
        // scene (ActivateScene falls back to the base scene or a default world, so the run would pass
        // having tested nothing), and ActivateScene blocks, so checking after it would first pay the
        // full load (~20 s).
        std::string path, script, name;
        if (!sceneManager.ResolveScene(cliScene, path, script, name))
        {
            LOG_ERROR("App: -scene='%s' does not resolve to a registered "
                      "scene name or an existing .iscene path. Refusing to "
                      "boot into a different world than the one requested.",
                      cliScene.c_str());

            // PostQuitMessage, not an early return: returning would skip Run()'s teardown
            // (ShutdownAllSystems, SavePSOLibrary), dropping the PSO cache so the next launch pays a full
            // DXIL->PSO rebuild. WM_QUIT is only queued: init finishes (cheaply, without the scene load),
            // the first Plat::Window::Pump of Run()'s loop returns its exit code, and the orderly exit
            // returns this code.
            PostQuitMessage(Harness::PreconditionFail);
            sceneResolved = false;
        }
        else
        {
            startup = cliScene;
            LOG_INFO("App: -scene override = '%s' -> '%s'",
                     cliScene.c_str(), path.c_str());
        }
    }

    // bootScriptsRefused: a refused script-manifest boot already queued WM_QUIT - skip the load,
    // like the -scene refusal. !flowOwnsBootScene: the accepted startup_state owns the first world;
    // its on_enter (GameFlowResolveSystem, frame 1) issues Scene.Load itself, so loading here
    // would build a world only to discard it one frame later.
    if (sceneResolved && !bootScriptsRefused && !flowOwnsBootScene)
    {
        if (startup.empty())
        {
            startup = Scene::kBaseScenePath;
            LOG_WARNING("App: game.json has no startup_scene — booting the base scene '%s'",
                        startup.c_str());
        }
        LOG_INFO("App: boot scene = '%s'", startup.c_str());
        Harness::RunLedger::Get().NoteScene(startup);
        if (deferLoad)
        {
            LOG_INFO(pipelinesPreparing ? "App: the boot scene loads once the pipelines are prepared"
                                        : "App: the boot scene loads behind the loading screen");
            deferLoad(startup);
            return;
        }
        sceneManager.ActivateScene(startup);
        // Absent when a startup_state owns the first scene (loaded on frame 1).
        Sys::Boot::Mark("sceneLoaded");
    }
}
