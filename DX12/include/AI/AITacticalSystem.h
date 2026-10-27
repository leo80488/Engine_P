#pragma once

// AITacticalTick - turns the strategic AIIntentComponent goal into tactical NavAgentComponent
// fields (destination, facingMode, facingTarget); NavAgentSystem then paths to the destination and
// the KCC step moves the character (docs/design/character_movement_architecture.md).
// AITacticalTickSystem is not registered: no BT sets a goal (trees write Intent.MoveTo, which sets
// NavAgent.destination directly) and enemy positioning lives in Lua. Registered, it would clear the
// destination every tick of each entity with a NavAgent and an AIIntentComponent left at the default
// Idle goal, overriding Intent.MoveTo.

class World;

namespace AI
{
    void AITacticalTick(::World& world, float dt);
}
