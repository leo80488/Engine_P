#pragma once

// NavAgentSystem - pathfinding + steering for entities with a NavAgentComponent. Input: the
// agent's destination and facing fields (Lua Intent.MoveTo / Intent.LookAt from the BT write them).
//   NavAgent.destination -> NavMeshSystem::FindPath -> follow the waypoints -> steering, then
//     with a CharacterControllerComponent (CCC): write CharacterIntentComponent; CharacterMotorSystem
//       turns it into the CCC's drive fields and the KCC step in PhysicsStepSystem does the sweep,
//       ground snap, step up and wall slide (Jolt CharacterVirtual);
//     without one: write LocalTransform (translation and rotation) directly.
// Runs in TickPhase::AI after the BT tick and before CharacterMotorSystem (NavAgentTickSystem, which
// the Nav module registers right before the player brain).

#include "ECS/ISystem.h"

class World;   // global ::World

namespace Nav
{
    class NavMeshSystem;

    // Ticks every NavAgentComponent: plans and follows a path to its destination. An agent without a
    // destination, or every agent while @p nav is not ready, idles (zero move intent; facing is still
    // resolved). @p dt <= 0 does nothing.
    void NavAgentTick(::World& world, NavMeshSystem& nav, float dt);
}

// NavAgentTick as a tick system.
class NavAgentTickSystem : public SystemInPhase<TickPhase::AI>
{
public:
    explicit NavAgentTickSystem(Nav::NavMeshSystem& nav) : m_nav(nav) {}
    void Update(World& world, const FrameContext& ctx) override;
    void DeclareAccess(SystemAccessBuilder& b) const override;
    // Creates the intent pool that NavAgentTick's auto-promote writes, so Update never
    // changes structure.
    void PrepareStructure(World& world) override;
    SystemRunMask RunMask() const override { return SystemRunMask::Play; }
    const char* GetName() const override { return "NavAgentSystem"; }

private:
    Nav::NavMeshSystem& m_nav;
};
