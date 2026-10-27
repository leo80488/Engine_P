#pragma once

// CharacterAnimDriverComponent + driver tick - automatic animation selection from
// intent + motor truth, shared by every brain (player and AI).
// Opt-in: only entities carrying this component are driven; content without it
// (EnemyStates.lua + BT SetState) is untouched.
// Per entity each frame the driver:
//   1. Resolves the ANIMATED entity (self -> SkeletonRef -> first animated
//      descendant) and caches it (generation-checked).
//   2. Yields entirely while another system owns the target's clock (externalTime:
//      MotionDirector, Sequencer).
//   3. LOCOMOTION BRIDGE - with an enabled LocomotionComponent on the target,
//      writes lc.speed (actual |velocity| from the KCC) and, in Blend2D mode,
//      lc.dirDeg (velocity direction in character space): the blend space's
//      runtime input.
//   4. Otherwise DISCRETE GAIT - idle / walk / run / air aliases via Animator.Play
//      thresholds on actual speed, re-issued only on state change
//      (QueueAnimatorPlay dedupes repeats anyway).
//   5. ACTION layer - consumes CharacterIntentComponent::action (one-shot
//      mailbox): plays an action alias variant, suppresses gait switching while
//      it runs, and detects completion by polling the animator (clip left the
//      current / target slot, or its one-shot time ran out); no wall-clock
//      duration tables.
// Runs in the Animation phase BEFORE RendererAnimationChainSystem so selections
// land in the same frame's mailbox (ResolveAnimationSets consumes it at end of
// frame; the usual +1-frame animator latency applies).

#include "ECS/ECS.h"
#include "Anim/Graph/AnimGraphTypes.h"

#include <cstdint>
#include <string>

namespace Resource { class AnimationClipSystem; }

struct CharacterAnimDriverComponent : ComponentBase
{
    static constexpr int MAX_ATTACKS  = 4;
    static constexpr int MAX_VARIANTS = 4;

    // ---- Discrete gait aliases (must exist in the AnimationSet) ----
    // Variant arrays: a random entry is picked on state ENTRY (idles optionally
    // re-roll every idleRerollSec). An empty run / air alias disables that state.
    // Directional walk aliases are optional: when present, the walk band classifies
    // velocity against the character's facing (+-60 deg cones) so a strafing character
    // back-pedals / side-steps with the right clip.
    std::string idleAliases[MAX_VARIANTS] = { "IDLE" };
    int         idleAliasCount = 1;
    std::string walkAliases[MAX_VARIANTS] = { "WALK" };   // forward variants
    int         walkAliasCount = 1;
    std::string walkBackAlias;            // optional directional walks
    std::string walkLeftAlias;
    std::string walkRightAlias;
    std::string runAlias = "RUN";
    std::string airAlias;                 // jump/fall pose (optional)

    float walkThreshold = 0.15f;          // m/s; below: idle
    float runThreshold  = 5.0f;           // m/s; above: run
    float gaitBlend     = 0.20f;          // cross-fade for gait switches
    float airTimeGrace  = 0.12f;          // s airborne before airAlias kicks in
    float idleRerollSec = 0.f;            // >0: re-roll idle variant this often

    // ---- Action layer ----
    // Attack and Dodge are variant lists (variant 0 in the request picks a random
    // entry; variant N plays entry N, which is how a combo addresses its steps).
    // Interact is a single clip. A kind with no alias drains its request without
    // playing anything.
    std::string attackAliases[MAX_ATTACKS];
    int         attackAliasCount = 0;
    std::string dodgeAliases[MAX_VARIANTS];
    int         dodgeAliasCount  = 0;
    std::string interactAlias;
    float       actionBlend      = 0.10f;

    // Graph bridge names may follow the project's parameter namespace.
    std::string graphSpeedParam="Speed";
    std::string graphDirectionParam="Direction";
    std::string graphGroundedParam="IsGrounded";
    std::string graphVerticalSpeedParam="VerticalSpeed";
    std::string graphAirTimeParam="AirTime";
    std::string graphTurnRateParam="TurnRate";
    std::string graphMovingParam="IsMoving";
    std::string graphGaitParam="Gait";
    std::string graphActionSlot="DefaultSlot";

    // ---- Runtime (not serialized) ----
    Entity      animTarget    = NullEntity;
    uint32_t    animTargetGen = 0;
    // Previous-frame motor truth, for the LocomotionTransitionComponent state
    // machine (speed crossings -> Start/Stop, direction jump -> Pivot, yaw rate
    // while stationary -> TurnInPlace).
    float       prevSpeed     = 0.f;
    float       prevDirDeg    = 0.f;
    float       prevYawDeg    = 0.f;
    bool        hasPrevYaw    = false;
    bool        wasGrounded   = true;   // ground/air edge for the jump states
    uint8_t     lastGait      = 0;        // GaitBucket (see .cpp)
    std::string activeGaitAlias;
    float       idleTimer     = 0.f;
    bool        actionActive  = false;
    bool        actionHold    = false;    // hold final pose (death) forever
    // Has the armed action actually STARTED on the animator? QueueAnimatorPlay only
    // writes an AnimatorPlayRequestComponent; currentClipName / targetClipName change
    // when ResolveAnimationSets consumes it later in the frame, so judging completion
    // before that sees the OUTGOING clip and retires the action one frame after
    // arming (an intermittent twitch). Nothing may treat the action as running (least
    // of all the locoWeight ramp, which hands the body over) until this is true.
    bool        actionStarted = false;
    float       actionArmTime = 0.f;      // s since arming, for the give-up timeout
    std::string activeActionAlias;
    // The alias an action left sitting on the animator after it finished, so
    // the driver can release EXACTLY its own leftover once locomotion owns the
    // body again. Releasing whatever happens to be on the animator instead
    // would silently kill a clip a script deliberately started.
    std::string parkedAlias;
    uint32_t    rng           = 0;        // xorshift state for variant picks
    Anim::Graph::SlotHandle graphAction;
    std::array<Anim::Graph::ParamHandle,8> graphParameters;
    bool graphMoving=false;
};

namespace Character
{
    void AnimDriverTick(::World& world,
                        Resource::AnimationClipSystem& clips,
                        float dt);

    // Drop the running action WITHOUT playing anything in its place: clears the action
    // latch and forces a gait re-issue, so the next tick cross-fades back to
    // locomotion. The only way out of a `hold` action (death pose) short of destroying
    // the entity, and a script's "cancel": clearing the mailbox alone does nothing,
    // because the latch that suppresses gait lives on the driver. Returns false when
    // the entity has no driver or no action was running.
    bool CancelAction(::World& world, Entity e);
}
