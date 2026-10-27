#pragma once

// PlayModeController - the authoritative Edit / Play state machine
// (docs/design/editor_runtime_mode_architecture.md). Hosts (the editor toolbar and hotkeys, the
// -play / -play_at command line, a Game build's menu key) express intent via Request*(); this
// controller owns the single EngineMode, the GameClock and the Enter / Exit-Play edge detection.
// A pure policy object: it owns no World, renderer or systems. The host (App) does a transition's
// heavy lifting (snapshot the editor world, rebuild physics / scripts, invalidate temporal history)
// in onEnterPlay / onExitPlay, fired exactly once on the Edit -> Playing and
// (Playing|Paused|Stepping) -> Edit edges.
// In EngineCore so Game builds share it: they start in Playing via SetMode, which fires no
// callback, so no editor-world snapshot is taken.

#include "ECS/EngineMode.h"

#include <functional>

class PlayModeController
{
public:
    // Fixed step length while Stepping: 1/60 s, the physics fixed timestep.
    static constexpr float kFixedStep = 1.0f / 60.0f;

    // ---- Queries ----
    EngineMode    Mode()        const { return m_mode; }
    SystemRunMask ActiveMask()  const { return ModeToMask(m_mode); }
    // True while the simulation clock advances this frame (Playing, or a single Step); App passes
    // it on as FrameContext::runUpdate.
    bool          SimAdvancing() const { return IsSimAdvancing(m_mode); }
    bool          IsPlaying()   const { return m_mode == EngineMode::Playing; }
    bool          IsPaused()    const { return m_mode == EngineMode::Paused; }
    bool          IsEditing()   const { return m_mode == EngineMode::Edit; }

    const GameClock& Clock() const { return m_clock; }

    // A request queued and not yet applied by BeginFrame. There is one slot: a
    // second request before the next BeginFrame replaces the first.
    bool HasPendingRequest() const { return m_pending != Request::None; }

    // The effective per-frame delta for the simulation: real wall-clock dt
    // normally, but exactly one fixed step (kFixedStep) while Stepping. Valid
    // only after BeginFrame() has run this frame.
    float EffectiveDeltaTime() const { return m_effectiveDt; }

    // ---- Intent (editor toolbar / hotkeys) ----
    // These only QUEUE a request; BeginFrame() applies it so every consumer sees one mode for the whole
    // frame.
    void RequestPlay()        { m_pending = Request::Play; }
    void RequestStop()        { m_pending = Request::Stop; }
    void RequestPause()       { m_pending = Request::Pause; }
    void RequestStep()        { m_pending = Request::Step; }
    // Play/Pause toggle convenience for the single combined toolbar button.
    void RequestTogglePlay()
    {
        m_pending = (m_mode == EngineMode::Playing) ? Request::Pause : Request::Play;
    }

    // ---- Per-frame driver ----
    // Processes the queued request (firing onEnterPlay / onExitPlay on edges), resolves a one-frame
    // Stepping back to Paused, advances the GameClock and caches EffectiveDeltaTime(). Call once at the
    // top of every frame, before the simulation phases run. @p realDt is unscaled wall-clock dt.
    void BeginFrame(float realDt);

    // Force the mode without firing transition callbacks. Used by Game builds
    // to pin the engine to Playing at boot (no editor, no snapshot/restore).
    void SetMode(EngineMode m) { m_mode = m; m_pending = Request::None; }

private:
    enum class Request : uint8_t { None, Play, Stop, Pause, Step };

public:
    // ---- Host hooks (App wires these) ----
    // Fired once on the Edit -> Playing edge: snapshot the pristine editor world and invalidate
    // temporal history (jump to the game camera).
    std::function<void()> onEnterPlay;
    // Fired once on the (Playing|Paused|Stepping) -> Edit edge: tear down
    // runtime state + restore the editor-world snapshot + invalidate temporal
    // history (jump back to editor camera).
    std::function<void()> onExitPlay;

private:
    EngineMode m_mode        = EngineMode::Edit;
    Request    m_pending     = Request::None;
    GameClock  m_clock;
    float      m_effectiveDt = 0.0f;
};
