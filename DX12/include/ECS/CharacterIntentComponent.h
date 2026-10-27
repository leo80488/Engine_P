#pragma once

// CharacterIntentComponent + BrainComponent: the control seam of the movement
// stack.
//   brain (decides WHAT the character wants, writes intent every frame):
//     PlayerControllerSystem  (input + camera basis -> intent)
//     NavAgentSystem          (BT / Intent.MoveTo -> path -> steering -> intent)
//   -> CharacterIntentComponent (the ONLY contract between brain and motor)
//   -> CharacterMotorSystem (intent -> CCC desiredHorizontalVelocity /
//      jumpRequested / facing; jump buffer and coyote time live there so every
//      brain gets them)
//   -> KCC (PhysicsSystem)
// Possession: BrainComponent.type gates which brain system may write this
// entity's intent; switching control is one enum write (Lua Character.Possess)
// and the components stay attached. Without a BrainComponent every brain system
// whose components match runs.
// Conventions:
//   * moveDir is a WORLD-SPACE unit vector (zero = stop); desiredSpeed is m/s.
//     Brains choose the speed (player: walk/run config; AI: NavAgent.speed x
//     arrival slowdown).
//   * Continuous fields are overwritten by the brain every frame, never
//     accumulated. Edge fields (jump) are cleared by the Motor.
//   * action is a one-shot mailbox consumed by CharacterAnimDriverTickSystem.

#include "ECS/ECS.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>
#include <cstdint>
#include <string>

// Gait annotation for the animation driver. The Motor takes speed from
// desiredSpeed, not from this; it lets the driver tell an intentional slow walk
// from a run scaled down by analog input.
enum class CharacterGait : std::uint8_t { Walk = 0, Run = 1, Sprint = 2 };

// One-shot action requests for CharacterAnimDriverTickSystem / gameplay.
enum class CharacterActionType : std::uint8_t
{
    None     = 0,
    Attack   = 1,   // pick from the driver's attackAliases
    Dodge    = 2,
    Interact = 3,
    Custom   = 4,   // play `actionAlias` verbatim (hit reacts, death, emotes)
};

struct CharacterIntentComponent : ComponentBase
{
    // ---- Continuous (brain overwrites every frame) ----
    DirectX::XMFLOAT3 moveDir      = { 0.f, 0.f, 0.f };  // world-space unit dir; zero = stop
    float             desiredSpeed = 0.f;                 // m/s
    CharacterGait     gait         = CharacterGait::Walk;

    // Facing intent. When useFaceDir is false the Motor faces the movement
    // direction (above facingMinSpeed), for the player and FaceMovement AI alike.
    DirectX::XMFLOAT3 faceDir    = { 0.f, 0.f, 1.f };     // world-space, XZ
    bool              useFaceDir = false;

    // ---- Edge-triggered (brain sets; Motor consumes) ----
    bool jump   = false;   // pressed THIS frame (buffer/coyote live in Motor)
    bool crouch = false;   // held state (stored on the KCC; nothing reads it yet)

    // ---- Action mailbox (brain sets; AnimDriver consumes) ----
    CharacterActionType action        = CharacterActionType::None;
    std::uint8_t        actionVariant = 0;    // Attack/Dodge: 0 = driver picks
    std::string         actionAlias;          // Custom: alias to play verbatim
    bool                actionHold    = false;// stay on the final pose (death) until CancelAction or an interrupting action
    float               actionBlend   = -1.f; // <0 = use the driver's default
    // Pre-emption. Without it the driver leaves the mailbox alone while an action
    // runs: a free one-slot input buffer that fires the instant the current clip
    // ends, which is what chains an attack combo. Set it when the request must cut
    // the running action short instead: a dodge out of a swing, a hit react, a death.
    bool                actionInterrupt = false;

    // ---- Motor override (script-owned movement: dash, knockback, launch) ----
    // CharacterMotorSystem rewrites CCC::desiredHorizontalVelocity from the intent
    // after the Logic phase, so a script writing it directly is overwritten before
    // physics sees it. Use this instead: while lockTimer > 0 the Motor drives
    // lockDir * lockSpeed and ignores the brain's horizontal intent. The Motor owns
    // the countdown.
    DirectX::XMFLOAT3 lockDir   = { 0.f, 0.f, 0.f };  // world-space XZ, unit
    float             lockSpeed = 0.f;                // m/s
    float             lockTimer = 0.f;                // s remaining; <=0 = off
    bool              lockFace  = false;              // also pin facing to lockDir

    // ---- Motor-owned runtime (do not write from brains) ----
    float jumpBufferTimer = 0.f;

    bool MoveLocked() const { return lockTimer > 0.f; }
};

// BrainComponent: possession. Serialized (one byte) so scenes remember who drives
// each character. Absent = every matching brain system drives it.
enum class BrainType : std::uint8_t
{
    None   = 0,   // nobody writes intent (frozen puppet, cutscene hand-off)
    Player = 1,
    AI     = 2,
};

struct BrainComponent : ComponentBase
{
    BrainType type = BrainType::None;
};

// True when `brain` permits the brain system `who` to drive this entity. A
// missing BrainComponent permits every brain.
inline bool BrainAllows(const BrainComponent* brain, BrainType who)
{
    return !brain || brain->type == who;
}
