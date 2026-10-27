#pragma once

// ISceneTransition: a pluggable visual for a scene switch.
//
// SceneTransitionManager owns the orchestration (phase timing, the async asset prewarm, the
// blocking swap while the screen is covered); an ISceneTransition owns only how the cover /
// reveal looks (fade, wipe, dissolve, iris, a custom fullscreen shader, ...). A new effect is a
// subclass plus RegisterTransition("name", ...), with no change to the manager or the load path.
//
// Selection is data-driven through TransitionParams (game.json "transition":{...} or Lua
// Scene.Load(name, { transition = "wipe", ... })); an unknown params.type falls back to "fade".
// Effects read only the params they understand, so growing TransitionParams never breaks one.

#include <functional>
#include <memory>
#include <string>
#include <cstdint>

namespace UI { class UIDrawList; }

namespace Scene
{

// Effect-agnostic phase of a transition (the names fit any effect, not just a fade).
enum class TransitionPhase
{
    Idle,     // nothing in flight
    Cover,    // hide the old scene (fade to black / wipe in / ...)
    Loading,  // fully covered; async prewarm pumps + the blocking swap fires here
    Reveal,   // uncover the new scene
};

// Forward-compatible parameter bag: grow it freely; simple effects ignore fields they do not
// use.
struct TransitionParams
{
    std::string  type            = "fade"; // registry key -> which ISceneTransition
    float        durationScale   = 1.0f;   // multiplies the effect's cover/reveal seconds
    std::uint8_t r = 0, g = 0, b = 0;      // cover colour (fade/wipe interpret this)
    std::string  texture;                  // reserved: mask / pattern texture path
    std::string  shader;                   // reserved: custom fullscreen shader path
    float        param0 = 0.f, param1 = 0.f, param2 = 0.f, param3 = 0.f; // reserved knobs
    bool         showProgressBar = true;   // driver draws the shared loading bar on top
};

// Per-frame snapshot handed to an effect's Render(). Self-contained so an effect
// never has to reach back into engine/manager state.
struct TransitionFrame
{
    TransitionPhase phase     = TransitionPhase::Idle;
    float           phaseT    = 0.f; // 0..1 within the current phase
    float           cover     = 0.f; // 0..1 how hidden the screen is (1 = fully covered)
    float           progress  = 0.f; // 0..1 real load progress (for bars/spinners)
    float           dt        = 0.f;
    unsigned int    viewportW = 0;
    unsigned int    viewportH = 0;
};

// A pluggable transition effect; the default is fade-to-colour (SceneTransitions.cpp).
class ISceneTransition
{
public:
    virtual ~ISceneTransition() = default;

    virtual const char* Name() const = 0;

    // Effect-chosen phase durations (seconds), BEFORE params.durationScale.
    virtual float CoverSeconds()  const { return 0.40f; }
    virtual float RevealSeconds() const { return 0.55f; }

    // Optional lifecycle hooks, no-ops by default. OnBegin receives the resolved params (colour,
    // texture, custom knobs); OnPhaseEnter suits a per-phase SFX or camera shake.
    virtual void OnBegin(const TransitionParams& /*params*/) {}
    virtual void OnPhaseEnter(TransitionPhase /*phase*/)     {}
    virtual void OnEnd()                                     {}

    // Draw the effect fullscreen into the UI draw list. Called every active frame.
    virtual void Render(UI::UIDrawList& dl, const TransitionFrame& f) = 0;
};

// ==== Registry (name -> factory) ====
using TransitionFactory = std::function<std::unique_ptr<ISceneTransition>()>;

// Register a factory under @p name (idempotent overwrite). Built-ins ("fade",
// "wipe") self-register lazily; game code can add more at startup.
void RegisterTransition(const std::string& name, TransitionFactory factory);

// Create the effect registered under @p params.type, falling back to "fade" for
// an empty/unknown type. Never returns null.
std::unique_ptr<ISceneTransition> CreateTransition(const TransitionParams& params);

} // namespace Scene
