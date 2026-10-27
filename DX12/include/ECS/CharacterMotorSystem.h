#pragma once

// CharacterMotorSystem: the single consumer of CharacterIntentComponent.
// Turns intent into CharacterControllerComponent drive fields and facing, the
// same for every brain (player / AI / cutscene): speed with optional accel ramps
// and air control, jump buffer + coyote time (an AI possessing the character
// gets the same jump feel), motor locks (dash / knockback), anim-graph root
// motion, and facing in two easing styles so tuned values keep their meaning
// (clamp-rate for a player turnRate in rad/s, exponential NLerp for a NavAgent
// turnRate as a lerp gain).
// Tuning comes from PlayerComponent, else NavAgentComponent, else defaults.
// Entities with an intent but no CharacterControllerComponent are skipped.
// Runs last in the AI phase, after every brain has written intent.

class World;

namespace Character
{
    void MotorTick(::World& world, float dt);
}
