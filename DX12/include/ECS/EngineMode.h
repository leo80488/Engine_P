#pragma once

// EngineMode - the Edit / Play state machine that drives the whole engine
// (docs/design/editor_runtime_mode_architecture.md):
//   Edit     - authoring, no simulation; the editor fly camera drives the viewport,
//              gizmo / picking / transform edits run.
//   Playing  - PIE simulation: gameplay / physics / AI / scripts tick; the game
//              camera stack drives the viewport.
//   Paused   - simulation frozen (GameClock stops); the real clock keeps running so
//              the editor camera can still orbit and inspect.
//   Stepping - advance exactly one fixed tick, then fall back to Paused.
// Every ISystem declares a SystemRunMask; the Scheduler ticks a system in a phase
// only if its mask intersects the active mode's mask (ModeToMask).

#include <cstdint>

enum class EngineMode : uint8_t
{
    Edit,      // Authoring; no simulation. EditorWorld snapshot is authoritative.
    Playing,   // PIE simulation running.
    Paused,    // PIE but the GameClock is frozen; RealClock keeps ticking.
    Stepping,  // Advance exactly one fixed tick, then auto-return to Paused.
};

// Per-system run mask. A system runs in a mode iff its mask intersects that mode's
// mask (ModeToMask + HasAnyFlag). ISystem::RunMask() defaults to `Always`: runs in
// every mode (render, transform propagation, skinning, editor camera).
enum class SystemRunMask : uint8_t
{
    None   = 0,
    Edit   = 1 << 0,   // gizmo / picking / grid / selection outline / editor camera
    Play   = 1 << 1,   // physics / AI / script / gameplay / VFX / audio simulation
    Paused = 1 << 2,   // runs while paused (rare - most things freeze)
    Always = Edit | Play | Paused,
};

inline constexpr SystemRunMask operator|(SystemRunMask a, SystemRunMask b)
{
    return static_cast<SystemRunMask>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline constexpr SystemRunMask operator&(SystemRunMask a, SystemRunMask b)
{
    return static_cast<SystemRunMask>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

// True iff the two masks share any set bit. The Scheduler uses this against
// the current mode's mask to decide whether to tick a system this frame.
inline constexpr bool HasAnyFlag(SystemRunMask a, SystemRunMask b)
{
    return (static_cast<uint8_t>(a) & static_cast<uint8_t>(b)) != 0;
}

// Map the engine mode to the run mask that is "active" this frame. Stepping
// behaves like Playing (it advances one tick), so simulation systems (Play
// mask) fire during a step.
inline constexpr SystemRunMask ModeToMask(EngineMode m)
{
    switch (m)
    {
        case EngineMode::Edit:     return SystemRunMask::Edit;
        case EngineMode::Playing:  return SystemRunMask::Play;
        case EngineMode::Paused:   return SystemRunMask::Paused;
        case EngineMode::Stepping: return SystemRunMask::Play;
    }
    return SystemRunMask::Always;
}

// True while the simulation clock advances this frame (Playing or a single Step).
// Always-on systems that freeze their own time (animation playback, camera follow
// smoothing) read this instead of gating their whole Update: they still run every
// frame to rebuild per-frame GPU state, with dt=0.
inline constexpr bool IsSimAdvancing(EngineMode m)
{
    return m == EngineMode::Playing || m == EngineMode::Stepping;
}

// GameClock - the simulation timeline (physics / AI / gameplay / Notify), distinct
// from the editor wall clock (RealClock) that always advances and drives the
// editor camera, UI animation and render interpolation. Paused freezes GameClock
// only. The fixed-physics accumulator (and its spiral-of-death clamp) lives in App;
// GameClock is the single authoritative "simulation time since Play began",
// excluding paused frames.
struct GameClock
{
    double elapsed = 0.0;   // total simulation seconds since the last Reset()

    void Reset()                  { elapsed = 0.0; }
    void Advance(double simDt)    { elapsed += simDt; }
};
