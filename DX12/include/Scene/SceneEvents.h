#pragma once

// Events SceneManager publishes around a scene swap: the edge anything waiting on a load (a
// GameFlow "Loading" state, a streaming budget, an editor panel) reacts to, instead of the new
// scene's own script calling back from OnSceneEnter. In their own header, like
// FollowEvents.h, so a subscriber need not include the load orchestrator.
//
// Delivery: SceneManager publishes on the main thread and EventBus::Publish is queued, so
// subscribers run at the next DispatchAll, never inside ActivateScene. By the time a handler
// runs the blocking swap is finished, the World holds the new scene, CurrentScene() is the new
// name and the new scene script is bound, so a handler may request the next scene load
// without reentering one still running.

#include <string>

namespace Scene
{

// Published at the very end of SceneManager::ActivateScene: after the World is populated, the
// navmesh / post-process companions are resolved, a pending save-game slot is applied and the
// scene script is bound.
//
// `ok` is ActivateScene's own return value: false when the requested scene failed and the
// base scene or a synthesised default world was substituted. `name` / `path` always describe
// the scene actually live (the substitute, on a fallback), the same answer CurrentScene()
// gives. It fires for every activation, fallbacks included: a Loading state needs one
// guaranteed edge to leave on, or a broken .iscene strands it forever.
struct SceneLoadedEvent
{
    std::string name;      // registry name, or the path stem for an unregistered scene
    std::string path;      // .iscene actually loaded
    bool        ok = false; // false = a fallback world is live, not what was asked for
};

// Published whenever a requested scene could not be brought up as asked; `name` / `path`
// describe what was asked for, not the substitute. A failure inside ActivateScene's load is
// followed (same dispatch pass, if the process got that far) by a SceneLoadedEvent for whatever
// is live instead, so a failure-only subscriber still sees the recovery and a load-only one
// never hangs. A request that never reaches a load (no World wired, no transition hooks, a
// preload target that does not resolve) gets no SceneLoadedEvent after it.
struct SceneLoadFailedEvent
{
    std::string name;
    std::string path;
    std::string reason;    // human-readable; for logs/telemetry, not for branching
};

} // namespace Scene
