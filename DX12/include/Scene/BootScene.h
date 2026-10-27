#pragma once
// The boot scene: -scene=<name-or-path>, else game.json's startup_scene, else the engine's
// base scene.

#include <functional>
#include <string>

class SceneManager;

namespace Scene
{
    // Loads the boot scene (blocking) unless @p bootScriptsRefused (WM_QUIT is already queued)
    // or @p flowOwnsBootScene (the startup state's on_enter loads the first world).
    // An unresolvable -scene= refuses the run (PostQuitMessage(PreconditionFail)) instead of
    // booting into a different world than the one requested.
    // @p deferLoad, when set, is handed the scene instead of the blocking load, and a scene
    // transition loads it behind the loading screen: the boot precompile is still running
    // (@p pipelinesPreparing, for the log), or BootThroughTransition.
    void ActivateBootScene(SceneManager& sceneManager, bool bootScriptsRefused,
                           bool flowOwnsBootScene,
                           const std::function<void(const std::string&)>& deferLoad = {},
                           bool pipelinesPreparing = false);

    // True when the boot scene should load behind the loading screen although no precompile runs: a
    // Game.exe a person started. A blocking load takes seconds (a big scene, ~20 s) in which the
    // window cannot be moved or closed. The editor adopts the scene at boot and a harness run needs
    // it at frame 0, so both load blocking; the harness's -boot_transition asks for the screen.
    bool BootThroughTransition();
}
