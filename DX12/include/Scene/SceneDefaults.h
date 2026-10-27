#pragma once

// SceneDefaults: the engine's base scene path, the generic default world built in code when no
// .iscene can be loaded, the out-of-the-box sky a new scene starts with, and the environment
// migration run after every scene load. The default world is scene-agnostic (a camera, a
// directional light, a procedural sky) so the viewport is not black; a scene's identity comes
// from its .iscene (content) and its Lua scene script (behaviour), not from C++.

#include "ECS/ECS.h"

namespace Scene
{
    // The engine's built-in starting world, named only here: App boots it (editor and game) when
    // game.json has no startup_scene, and SceneManager falls back to it when a requested scene
    // cannot be loaded. It is data (editable in the editor, re-savable), not a C++ scene.
    // Project-root-relative so AssetFS resolves it identically in a loose build and inside a .pak.
    inline constexpr const char* kBaseScenePath =
        "engine/world/BaseScene.iscene";

    // The sky light strength a new scene is created with: physical - with the procedural
    // atmosphere, 1 is the sky this scene's sun really produces. Only creation writes it (File >
    // New, Editor.NewScene, the load-failure world); a loaded scene keeps what it authored, and
    // one that authored nothing keeps inheriting 0.1 (WorldEnvironment -1 -> AtmosphereComponent),
    // so no existing scene changes.
    inline constexpr float kNewSceneSkyStrength = 1.0f;

    // Gives the world's environment singleton the new-scene sky strength (no-op when it has none).
    // For callers that seed a new scene through MigrateEnvironment, which keeps the inherited
    // value for existing files.
    void ApplyNewSceneSkyStrength(World& world);

    // Spawns a camera, a directional light and the default (procedural) environment into
    // @p world, which the caller has already cleared. Used by SceneManager::ActivateScene when
    // neither the requested .iscene nor the base scene loads.
    void SpawnDefaultWorld(World& world);

    // Seeds the per-scene environment singleton (WorldEnvironmentComponent, mode
    // ProceduralAtmosphere) plus the "Sky" entity (Atmosphere + Clouds + HeightFog) and a disabled
    // "Time of Day" entity: the out-of-the-box sky for a new scene. Runs once, from
    // SpawnDefaultWorld, never inside the render loop (File > New gets the same sky through
    // MigrateEnvironment).
    void SpawnDefaultEnvironment(World& world);

    // One-time load migration: a world that already has a WorldEnvironmentComponent is left
    // alone; otherwise one is synthesised from any AtmosphereComponent / SkyboxComponent so a
    // .iscene without one keeps rendering as authored. Call once after a scene finishes loading.
    void MigrateEnvironment(World& world);
}
