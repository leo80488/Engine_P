#pragma once

// SceneTransitionManager: drives a Cover -> Loading -> Reveal transition around a scene
// switch, hiding the blocking scene load behind a pluggable full-screen effect and a real
// progress bar.
//
// It lives outside the scene because a scene switch is one atomic
// SceneManager::ActivateScene (clear the old world + blocking load), so a "loading scene"
// could never cover the old one. Flow:
//   1. Cover  : old scene still renders; the effect hides it.
//   2. Loading: fully covered; pump the async asset prewarm across frames (real progress),
//              then trigger the blocking swap while covered so the stall is masked.
//   3. Reveal : new scene renders; the effect uncovers it.
// The overlay goes straight into the UIPass draw list (DrawOverlay), not through ECS UI
// entities, so the new scene's World::Clear cannot wipe it.
//
// App wires SetLoadPoll (prewarm progress 0..1 + "ready to swap") and SetParamsProvider
// (effect + knobs, from game.json / Lua). The visual is an ISceneTransition (default fade);
// the bar/text on top is replaceable per game via SetOverlayHook.

#include "Scene/ISceneTransition.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace UI { class UIDrawList; }

class SceneTransitionManager
{
public:
    // Performs the actual swap; takes the target scene id (a registry name or a raw .iscene
    // path).
    using SceneSwapFn = std::function<void(const std::string&)>;

    // Load-progress poll: fills @p progress (0..1); returns true once the async asset prewarm
    // is warm enough to commit the blocking swap. An empty poll commits immediately.
    using LoadPollFn = std::function<bool(float& progress)>;

    // Supplies the params (effect type + knobs) for the transition being begun.
    using ParamsProviderFn = std::function<Scene::TransitionParams()>;

    // Game-supplied loading-screen art, drawn by DrawOverlay every active frame instead of the
    // built-in bar + "Now Loading..." text, with the same TransitionFrame the effect gets.
    // Do not touch entities, components or the scene's UI canvas from it: it runs across the
    // World::Clear of the swap, when none of them is guaranteed to exist.
    using OverlayDrawFn = std::function<void(UI::UIDrawList&, const Scene::TransitionFrame&)>;

    // The built-in overlay's line while the load waits on something other than the scene
    // ("Preparing shaders... 42%" during the boot precompile); "" keeps "Now Loading...".
    using MessageProviderFn = std::function<std::string()>;

    SceneTransitionManager() = default;
    // The implicit destructor is fine: ISceneTransition is a complete type here.

    // Begin a transition to scene @p sceneId; ignored while one is already in flight. The
    // effect and timing come from the params provider (plain fade fallback).
    // Do not refuse an empty id: it reaches SceneManager::ActivateScene's base-scene fallback,
    // which publishes the SceneLoaded/SceneLoadFailed pair a GameFlow Loading state must leave
    // on (Scene/SceneEvents.h). Hence the pending target is an optional, not an empty-string
    // sentinel.
    void Begin(std::string sceneId);

    // Advance the state machine by @p dt seconds. Once covered it pumps the prewarm; when warm
    // it calls @p swapScene(sceneId) to trigger the blocking swap, which App drains the same
    // frame behind full cover.
    void Tick(float dt, const SceneSwapFn& swapScene);

    // Append the effect + shared progress bar/text to @p dl. No-op when idle.
    // Call once per frame AFTER the normal UI tick and BEFORE the UIPass consumes
    // the draw list inside renderer.Render().
    void DrawOverlay(UI::UIDrawList& dl, unsigned int viewportW,
                     unsigned int viewportH) const;

    bool IsActive() const { return m_phase != Scene::TransitionPhase::Idle; }

    void SetLoadPoll(LoadPollFn fn)             { m_loadPoll = std::move(fn); }
    void SetParamsProvider(ParamsProviderFn fn) { m_paramsProvider = std::move(fn); }

    // Install (or clear, with an empty function) the custom overlay; without one the built-in
    // bar + text is drawn. params.showProgressBar == false suppresses both: that flag means
    // the effect draws everything itself.
    void SetOverlayHook(OverlayDrawFn fn)       { m_overlayHook = std::move(fn); }
    bool HasOverlayHook() const                 { return static_cast<bool>(m_overlayHook); }
    void SetMessageProvider(MessageProviderFn fn) { m_messageProvider = std::move(fn); }

private:
    using Phase = Scene::TransitionPhase;

    void EnterPhase(Phase p); // sets phase + resets elapsed + fires effect OnPhaseEnter

    Phase                                    m_phase = Phase::Idle;
    // Target scene id. Engaged = a swap is pending; the id may be empty (Scene.Load("") must
    // still reach ActivateScene, see Begin).
    std::optional<std::string>               m_target;
    std::unique_ptr<Scene::ISceneTransition> m_effect;      // the pluggable visual
    Scene::TransitionParams                  m_params;       // resolved at Begin
    LoadPollFn                               m_loadPoll;
    ParamsProviderFn                         m_paramsProvider;
    OverlayDrawFn                            m_overlayHook;  // empty = built-in bar
    MessageProviderFn                        m_messageProvider;

    float m_elapsed       = 0.f; // seconds within the current phase
    float m_cover         = 0.f; // 0 = clear, 1 = fully covered
    float m_progress      = 0.f; // 0..1 bar fill (real, from the prewarm poll)
    bool  m_swapRequested = false;
    float m_coverSeconds  = 0.40f; // effect->CoverSeconds()  * durationScale (set at Begin)
    float m_revealSeconds = 0.55f; // effect->RevealSeconds() * durationScale (set at Begin)
};
