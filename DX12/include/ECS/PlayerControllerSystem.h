#pragma once

// PlayerControllerSystem - the PLAYER BRAIN. Ticks every entity with a PlayerComponent whose
// BrainComponent, if any, permits Player control:
//   1. Resolve the camera basis: forward / right projected onto XZ from the player's
//      `cameraEntity` (else the main camera passed to Update).
//   2. Read the ActionState (Move / Jump / Sprint).
//   3. Write CharacterIntentComponent (moveDir / desiredSpeed / gait / jump edge) and nothing
//      else: CharacterMotorSystem turns intent into CCC drive fields, jump buffer / coyote and
//      facing identically for every brain, so possession swaps keep the same feel.
// Must run BEFORE CharacterMotorSystem in the AI phase.

#include "ECS/ECS.h"   // Entity / NullEntity - used in Update's signature

class World;

class PlayerControllerSystem
{
public:
    // Ticks every player entity in @p world. @p mainCamera is the fallback basis for a
    // PlayerComponent whose `cameraEntity` is empty (the App already tracks the main camera). @p dt is
    // the real frame delta in seconds.
    void Update(World& world, Entity mainCamera, float dt);
};
