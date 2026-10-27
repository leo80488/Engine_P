#pragma once

// AIIntentComponent - the strategic layer of the layered movement model
// (docs/design/character_movement_architecture.md):
//
//   AIIntentComponent             what do I want to achieve right now?
//     | AITacticalTick: goal -> NavAgent.destination + facing
//   NavAgentComponent             tactical: where to go, how to face
//     | NavAgentSystem
//   CharacterControllerComponent  execution: Jolt sweep
//
// A goal outlives a path (a path corner advances every ~1 s, an attack goal may last 30 s), so
// they live in different components and a path replan never clears the goal. A separate
// component also keeps its readers from coupling to NavAgent or the behavior tree.
// Writers: the Lua AI.SetGoal / AI.ClearGoal bindings. Reader: AITacticalTick. Neither is
// installed: AITacticalTickSystem is not registered and `AI` stays nil in Lua, so shipped
// scripts drive the tactical layer through Intent.MoveTo; the component is still serialized
// and editable.

#include "ECS/ECS.h"

#include <DirectXMath.h>

#include <cstdint>

enum class AIGoal : std::uint8_t
{
    Idle,         // no active intent
    Patrol,       // walk a route / area; goalPosition = next waypoint
    Investigate,  // move to a sound / sight cue; goalPosition = cue origin
    Attack,       // engage targetEntity; AITactical computes attack position
    Flee,         // run away from targetEntity
    TakeCover,    // find/move to cover relative to targetEntity (AITacticalTick leaves NavAgent untouched)
    Follow,       // tail behind targetEntity at a comfortable distance
    UseObject,    // interact with targetEntity (lever, door, ...); AITacticalTick leaves NavAgent untouched
};

struct AIIntentComponent : ComponentBase
{
    // The active strategic goal; AITacticalTick re-applies it to the NavAgent every tick.
    AIGoal             currentGoal     = AIGoal::Idle;

    // The entity this goal is aimed at (attack/flee/follow target, the
    // object to use, etc.). NullEntity when irrelevant (Idle, Patrol).
    Entity             targetEntity    = NullEntity;

    // World-space anchor for goals that target a position rather than an entity (Patrol ->
    // next patrol point, Investigate -> noise origin, TakeCover -> cover slot). Ignored when the
    // goal uses targetEntity.
    DirectX::XMFLOAT3  goalPosition    = { 0.f, 0.f, 0.f };

    // Utility-AI priority for competing goals (higher wins). Serialized and editable; no system
    // reads it.
    float              goalPriority    = 0.f;

    // When the current goal was set (ctx:Elapsed() seconds), for debug overlays and goal
    // staleness heuristics. AI.SetGoal leaves it 0.
    float              goalStartTime   = 0.f;
};
