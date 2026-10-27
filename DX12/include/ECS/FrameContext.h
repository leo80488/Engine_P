#pragma once

// FrameContext: per-tick state handed to every ISystem::Update.
//
// Lives on the stack in the App::Run frame loop and is mutated in place as the loop crosses phase
// groups (physicsAlpha is meaningful only after the fixed-physics loop, isFixedTickPhase is true
// only during the catch-up loop). Systems read the fields they need and ignore the rest.

#include <cstdint>

#include "ECS/EngineMode.h"

class CommandBuffer;
class TaskSystem;

struct FrameContext
{
    // ---- Timing ----
    // Real wall-clock dt (sec), unscaled (one fixed 1/60 s tick while Stepping): hit-stop timers,
    // anything that must keep ticking while gameplay time is frozen. Animation runs on
    // scaledDeltaTime, per entity through TimeDilationComponent (Gameplay/GameplayTime.h). The path
    // tracer's time freeze (Renderer::BeginWorldClock) stops it too: 0, or one Render.ptStep.
    float    deltaTime        = 0.f;
    // deltaTime, except that the time freeze does not stop it: UI painting and scene transitions.
    float    realDeltaTime    = 0.f;
    // deltaTime x the script time scale (x the GameFlow scale once the Input phase has run):
    // gameplay, physics and AI logic.
    float    scaledDeltaTime  = 0.f;
    float    fixedDeltaTime   = 1.f / 60.f;  // Fixed-physics step length (sec).
    // [0,1] = accumulator / fixedDt; meaningful only after the fixed-physics loop.
    float    physicsAlpha     = 0.f;

    // ---- Frame indices ----
    uint64_t frameIndex       = 0;       // Wraps at 2^64; safe to use as cache key.
    uint64_t physicsStepIndex = 0;       // Increments once per Jolt step.

    // ---- Mode flags ----
    // True only inside the accumulator catch-up loop (FixedPhysicsPre/Step/Post); variable-rate
    // systems should ignore the tick.
    bool     isFixedTickPhase = false;

    // ---- Engine mode ----
    // Edit / Playing / Paused / Stepping. The Scheduler filters each system by
    // ModeToMask(mode) & RunMask(). Game builds stay Playing; the editor drives it via
    // PlayModeController.
    EngineMode mode           = EngineMode::Playing;
    // == IsSimAdvancing(mode): the sim clock advances this frame (Playing or a single Step).
    // Simulation systems are gated by RunMask; always-on systems that freeze their own dt while
    // paused (animation playback, camera-follow smoothing) read this to choose dt = 0.
    bool     runUpdate        = true;

    // ---- Per-frame entity references ----
    // App keeps the "main camera" hint outside the scheduler (the entity can disappear when a scene
    // unloads) and republishes it here every tick so systems need not re-resolve it. Raw Entity id
    // (uint32_t, NullEntity == 0) to keep this header dependency-free.
    uint32_t cameraEntity     = 0;

    // ---- Services ----
    TaskSystem*    jobSystem     = nullptr;  // For parallelism inside a phase.
    CommandBuffer* commandBuffer = nullptr;  // For deferred structural ECS changes.
};
