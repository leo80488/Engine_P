#pragma once

// TickPhase: engine-wide tick partitioning.
//
// The main loop calls Scheduler::RunPhase() for every value in execution order; every ISystem
// belongs to exactly one phase. Phase boundaries are the only sync points: no system in phase N+1
// starts until every system in phase N has returned.

#include <cstdint>

enum class TickPhase : uint8_t
{
    Input,                  // Input snapshot, Window message pump, Event dispatch
    GameplayPreLogic,       // Spawn / Despawn schedule, Lifecycle, camera-entity hint
    GameplayLogic,          // Lua Logic scripts, gameplay decisions
    GameplayPostLogic,      // Lua System scripts, CommandQueue flush, scene transitions
    AI,                     // Behavior trees, NavAgent path-follow, AI LOD

    FixedPhysicsPre,        // Apply forces, kinematic targets (runs inside accumulator loop)
    FixedPhysics,           // Jolt step at 60Hz (runs inside accumulator loop)
    FixedPhysicsPost,       // Contact events -> EventBus (runs inside accumulator loop)

    PhysicsInterpolation,   // Per-Dynamic-body prev/curr lerp into GlobalTransform
    Animation,              // Pose sampling, blend, SkinMatrix
    BoneAttachment,         // Socket / FollowBone (depends on Animation)
    // Empty: chain physics (hair / skirt / spring bones) and face shading run inside
    // TickPhase::Animation (RendererAnimationChainSystem -> Renderer::TickAnimationChain). Kept for the
    // -brssplit publish / join bracket in App, which therefore overlaps nothing.
    SecondaryPhysics,

    // Near-empty despite the name (0.00-0.09 ms on the heaviest scene): culling, cluster build, shadow
    // setup and the whole scene build run inside TickPhase::Render (RenderSystem ->
    // Renderer::BeginFrame -> BuildRenderScene). Use -phaselog_every to see where the time is.
    PreRender,              // (near-empty; see note above)
    Render,                 // DX12 CL record + submit (MAIN THREAD)
    PostRender,             // TAA history swap, debug overlay, frame fence signal

    COUNT
};

struct PhaseDescriptor
{
    TickPhase   phase;
    const char* name;
    bool        isFixedTimestep;    // Runs inside the accumulator catch-up loop
    bool        allowsParallel;     // Phase contents may be batched onto worker threads
    bool        requiresMainThread; // Must execute on the thread that owns the DX12 queue
};

inline constexpr PhaseDescriptor kPhaseDescriptors[] = {
    { TickPhase::Input,                "Input",                 false, false, false },
    { TickPhase::GameplayPreLogic,     "GameplayPreLogic",      false, true,  false },
    { TickPhase::GameplayLogic,        "GameplayLogic",         false, true,  false },
    { TickPhase::GameplayPostLogic,    "GameplayPostLogic",     false, false, false },
    { TickPhase::AI,                   "AI",                    false, true,  false },
    { TickPhase::FixedPhysicsPre,      "FixedPhysicsPre",       true,  true,  false },
    { TickPhase::FixedPhysics,         "FixedPhysics",          true,  false, false },
    { TickPhase::FixedPhysicsPost,     "FixedPhysicsPost",      true,  false, false },
    { TickPhase::PhysicsInterpolation, "PhysicsInterpolation",  false, true,  false },
    { TickPhase::Animation,            "Animation",             false, true,  false },
    { TickPhase::BoneAttachment,       "BoneAttachment",        false, true,  false },
    { TickPhase::SecondaryPhysics,     "SecondaryPhysics",      false, true,  false },
    { TickPhase::PreRender,            "PreRender",             false, true,  false },
    { TickPhase::Render,               "Render",                false, false, true  },
    { TickPhase::PostRender,           "PostRender",            false, true,  false },
};

static_assert(sizeof(kPhaseDescriptors) / sizeof(kPhaseDescriptors[0])
              == static_cast<size_t>(TickPhase::COUNT),
              "kPhaseDescriptors out of sync with TickPhase enum");

inline constexpr const PhaseDescriptor& GetPhaseDescriptor(TickPhase p)
{
    return kPhaseDescriptors[static_cast<size_t>(p)];
}
