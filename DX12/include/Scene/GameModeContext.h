#pragma once

// GameModeContext: App's shared services / data bag, passed by reference every frame so
// engine-level code (the editor layer, camera tick, Lua-facing scene switches) can reach the
// graphics device, renderer, frame index and display viewport size (the editor's Viewport panel,
// else the window). Despite the name there is no game-mode object; it is only this bag.
//
// Naming model:
//   - The runtime ECS container is `World` (engine-lifetime, owned by App).
//   - A "scene" on disk is a `.iscene` asset deserialised by SceneSerializer.
//   - A scene id is a plain string: a name in game.json's scene registry, or a raw .iscene
//     path. SceneManager resolves it.

#include "ECS/ECS.h"
#include "Graphics/RenderTypes.h"   // FrameIndex, RHI::CommandList

#include <functional>
#include <string>

class IGraphicsDevice;
class Renderer;
class SceneManager;
namespace Resource
{
    class ResourceManager;
    class TextureSystem;
    class MeshLibrary;
    class AssetManager;
}

// A local of App::Run(), which fills the service pointers at startup and updates the rest every
// frame.
struct GameModeContext
{
    IGraphicsDevice& gfx;
    Renderer&        renderer;
    FrameIndex       frame     = 0;
    float            deltaTime = 0.0f; // elapsed seconds since previous frame
    unsigned int     viewportW = 0;   // display width in px; 0 until App::UpdateViewportSize runs
    unsigned int     viewportH = 0;   // display height in px; 0 until App::UpdateViewportSize runs

    // Camera mouse-look delta in pixels, as this frame's input snapshot resolved it: in the editor
    // the right-drag over the Viewport panel from the previous frame's EditorLayer state, in Game
    // the OS mouse (OsMouseLook); a harness build may inject it (Test.Look, -input_replay).
    float mouseViewportDX        = 0.f;
    float mouseViewportDY        = 0.f;
    // RMB only. Gates the Free-mode WASDQE fly branch; deliberately not OR'd with the gamepad, or
    // a resting stick would arm keyboard flight and collide with the editor's W/E/R gizmo keys.
    bool  viewportRightMouseHeld = false;

    // Device-agnostic rotation gate: right mouse held or the look stick off centre. Keep it
    // separate from viewportRightMouseHeld: while the camera is idle App re-seeds yaw/pitch from
    // the transform every frame, so an RMB-only gate would discard every stick write on the same
    // frame and the stick would seem dead.
    bool  cameraLookActive       = false;

    // Active World. Set once by App::Run at startup: the World is engine-lifetime and scene loads
    // replace its contents, never the pointer. The editor's frame hook (EditorFrameHooks::BuildUI)
    // passes it to EditorLayer::SetWorld() before every OnUIRender().
    World* world = nullptr;

    // Resource systems: owned by App, valid for the whole session.
    Resource::ResourceManager* resourceMgr = nullptr;
    Resource::TextureSystem*   textureSys  = nullptr;
    Resource::MeshLibrary*     meshLib     = nullptr;  // mesh-pool store

    // Unified asset cache (wraps textureSys with path-based dedup).
    Resource::AssetManager*    assetMgr    = nullptr;

    // Data-driven scene registry and loader, owned by App: App loads the boot scene's .iscene and
    // activates its Lua scene script through it; Lua's Scene.Load() switches scenes by name
    // through it. Null until App wires it up.
    SceneManager*              sceneManager = nullptr;

    // Scene swap request: replaces the current scene with the one named by a scene id (registry
    // name or raw .iscene path). It only records the id; App::Run's once-per-frame drain (after
    // the fixed physics loop, on frames the simulation advances) runs the blocking load, so it
    // never runs mid-iteration, and a later request before the drain replaces an earlier one. An
    // empty id is still a request: SceneManager::ActivateScene falls back to the base scene and
    // publishes the load events. Null until App wires it up.
    std::function<void(const std::string&)> requestReplaceMode = nullptr;

    // Like requestReplaceMode, but through the SceneTransitionManager: fade-out -> loading screen
    // -> fade-in instead of an instant cut. Prefer it for player-facing changes;
    // requestReplaceMode stays the raw instant swap (the transition manager uses it internally).
    // Null until App wires it up.
    std::function<void(const std::string&)> beginTransition = nullptr;
};
