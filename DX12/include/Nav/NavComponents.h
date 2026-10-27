#pragma once

// NavAgentComponent - the tactical layer of the movement model
// (docs/design/character_movement_architecture.md):
//   AIIntentComponent             strategic goal; AITacticalSystem (when registered) -> destination
//   NavAgentComponent             tactical: where to go and how (this file); NavAgentSystem ->
//   CharacterIntentComponent      move / face intent; CharacterMotorSystem ->
//   CharacterControllerComponent  execution: the KCC step sweep-and-slides via Jolt CharacterVirtual
//   LocalTransform
// Owns the destination, the path (corners from NavMeshSystem::FindPath), the facing override and
// mode, the follow state and stuck-detection scratch. Not position / rotation (LocalTransform),
// velocity / grounding / collision (CharacterControllerComponent) or the high-level goal
// (AIIntentComponent).
//
// Lua: BT actions call Intent.MoveTo / Intent.Stop / Intent.LookAt / Intent.ClearLookAt
// (Intent/LuaIntentBindings.cpp), which write these fields: BT scripts speak tactical-intent verbs
// ("move to point X, face Y") that map 1:1 onto this layer.

#include "ECS/ECS.h"

#include <DirectXMath.h>

#include <cstdint>
#include <vector>

// Who decides where the agent faces this tick; NavAgentSystem picks the rotation target from it.
enum class NavFacingMode : std::uint8_t
{
    FaceMovement,   // align with the steering direction (default: walking forward)
    FaceTarget,     // face `lookTarget` regardless of motion (combat strafe)
    Manual,         // hands-off: some other system owns rotation this tick
};

// High-level path-follow state. Driven by NavAgentSystem; readable by BT
// conditions ("am I still pathing?") and debug overlays.
enum class NavAgentState : std::uint8_t
{
    Idle,                   // no destination
    Computing,              // FindPath issued this tick
    Following,              // walking the path
    Arrived,                // reached the final corner
    Failed,                 // FindPath returned no valid path
    TraversingOffMeshLink,  // CCC.mode == LaunchedTraversal: path following paused
};

struct NavAgentComponent : ComponentBase
{
    // ---- Tactical intent (written by Lua Intent.* from the BT, or AITacticalSystem) ----
    // World-space goal. NavAgentSystem re-plans toward it only past the repath gate (repathDistance,
    // pathDirty, a lost path), not every tick.
    DirectX::XMFLOAT3 destination     = { 0.0f, 0.0f, 0.0f };
    bool              hasDestination  = false;
    // Look-at point: with useLookAt and facingMode == FaceTarget, NavAgentSystem faces it (combat
    // strafe-around-the-player).
    DirectX::XMFLOAT3 lookTarget      = { 0.0f, 0.0f, 0.0f };
    bool              useLookAt       = false;
    // What decides facing this tick. FaceMovement by default (walk forward, look forward);
    // Intent.LookAt switches to FaceTarget, cutscenes use Manual.
    NavFacingMode     facingMode      = NavFacingMode::FaceMovement;
    // Entity to face when facingMode == FaceTarget and useLookAt is off (its GlobalTransform position).
    // NullEntity disables.
    Entity            facingTarget    = NullEntity;

    // ---- Steering tunables (Inspector-editable, saved with the scene) ----
    float speed             = 3.0f;   // desired horizontal speed when path active (m/s)
    float arriveRadius      = 0.25f;  // advance to next waypoint within this XZ distance
    float slowdownRadius    = 1.5f;   // begin arrival-deceleration this far from destination
    float repathDistance    = 1.0f;   // destination must drift this far to trigger a fresh FindPath
    bool  rotateToFacing    = true;   // honour facingMode (false = rotation fully manual)
    float turnRate          = 8.0f;   // facing blend rate per second (no-CCC path: nlerp t = turnRate x dt)
    // Runtime gait control (NOT serialized): BT actions scale the authored `speed` per behaviour phase
    // via Intent.SetSpeedScale (e.g. 0.5 strafing / patrolling, 1.0 chasing), so the character's REAL
    // velocity changes and CharacterAnimDriver picks walk vs run from it.
    float speedScale        = 1.0f;

    // ---- Stuck detection ----
    // NavAgentSystem accumulates stuckTimer while CCC.wasBlockedLastStep is set AND the block normal
    // opposes the steering direction; past 0.5 s it sets pathDirty so the next tick re-plans.
    float             stuckTimer        = 0.0f;
    DirectX::XMFLOAT3 lastPositionSample = { 0.0f, 0.0f, 0.0f };

    // ---- Runtime: owned by NavAgentSystem, do not edit externally ----
    std::vector<DirectX::XMFLOAT3> path;
    std::uint32_t     nextWaypoint     = 0;
    DirectX::XMFLOAT3 lastPlannedTarget = { 0.0f, 0.0f, 0.0f };
    bool              pathValid        = false;
    bool              pathDirty        = false;   // request a fresh FindPath this tick (stuck recovery, dest moved)
    NavAgentState     state            = NavAgentState::Idle;
};
