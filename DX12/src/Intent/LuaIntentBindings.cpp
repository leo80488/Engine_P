#include "Intent/LuaIntentBindings.h"
#include "AI/AIComponents.h"   // Intent.Wake -> AIComponent::wakeRequested

#include "ECS/ECS.h"
#include "ECS/CharacterControllerComponent.h"
#include "Nav/NavComponents.h"
#include "System/Log.h"

#include <sol/sol.hpp>

#include "Scripting/LuaEntityId.h"   // LuaEntity + LuaId::* (needs sol first)

namespace Intent
{

namespace {

// Fetch or create the NavAgentComponent on `e`. EnsurePool first, so AddComponent on a
// poolless type does not take the slow path.
inline NavAgentComponent& GetOrCreateNavAgent(::World& world, Entity e)
{
    if (auto* p = world.GetComponent<NavAgentComponent>(e)) return *p;
    world.EnsurePool<NavAgentComponent>();
    NavAgentComponent fresh{};
    world.AddComponent<NavAgentComponent>(e, fresh);
    return *world.GetComponent<NavAgentComponent>(e);
}

} // namespace

void RegisterLuaIntentBindings(sol::state& lua, ::World& world)
{
    sol::table tbl = lua.create_table();

    // Intent.MoveTo(eid, x, y, z): publish a tactical move destination in
    // NavAgentComponent.destination. NavAgentSystem turns it into a path query and writes
    // CharacterController.desiredHorizontalVelocity (without a CCC, its fallback writes
    // LocalTransform.translation).
    // Do not set pathDirty here: NavAgentSystem repaths only when the destination moved more
    // than repathDistance from lastPlannedTarget, so the same-destination MoveTo a BT issues
    // every tick stays cheap. Forcing pathDirty runs FindPath every BT tick and resets
    // nextWaypoint to 1: the agent jitters, re-targeting the first corner.
    tbl.set_function("MoveTo",
        [&world](LuaEntity entityId, float x, float y, float z) -> bool
        {
            // ResolveLive rejects stale / dead ids, so a recycled id cannot park a
            // NavAgentComponent on the wrong (or a dead) entity.
            const Entity e = LuaId::ResolveLive(world, entityId);
            if (e == NullEntity) return false;
            auto& a = GetOrCreateNavAgent(world, e);
            a.destination    = { x, y, z };
            a.hasDestination = true;
            return true;
        });

    // Intent.Stop(eid): drop the move destination; NavAgentSystem zeroes
    // desiredHorizontalVelocity next tick. Does not promote a NavAgentComponent: without one
    // there is nothing to stop.
    tbl.set_function("Stop",
        [&world](LuaEntity entityId) -> bool
        {
            const Entity e = LuaId::ResolveLive(world, entityId);
            auto* a = e != NullEntity ? world.GetComponent<NavAgentComponent>(e) : nullptr;
            if (!a) return false;
            a->hasDestination = false;
            a->pathValid      = false;
            return true;
        });

    // Intent.LookAt(eid, x, y, z): face a world point. Sets facingMode = FaceTarget and
    // lookTarget; NavAgentSystem honours it even while the agent stands still (combat
    // strafe).
    tbl.set_function("LookAt",
        [&world](LuaEntity entityId, float x, float y, float z) -> bool
        {
            const Entity e = LuaId::ResolveLive(world, entityId);
            if (e == NullEntity) return false;
            auto& a = GetOrCreateNavAgent(world, e);
            a.lookTarget = { x, y, z };
            a.useLookAt  = true;
            a.facingMode = NavFacingMode::FaceTarget;
            return true;
        });

    // Intent.FaceEntity(eid, targetEid): face a moving entity, re-resolved every frame by
    // NavAgentSystem from the target's GlobalTransform. LookAt takes a fixed point, which a
    // BT leaf can only refresh at its ~10 Hz tick (slower under AILOD), so the agent would
    // aim up to 100 ms behind a strafing player.
    tbl.set_function("FaceEntity",
        [&world](LuaEntity entityId, LuaEntity targetId) -> bool
        {
            const Entity e = LuaId::ResolveLive(world, entityId);
            if (e == NullEntity) return false;
            const Entity target = LuaId::ResolveLive(world, targetId);
            if (target == NullEntity) return false;
            auto& a = GetOrCreateNavAgent(world, e);
            a.facingTarget = target;
            a.useLookAt    = false;    // facingTarget supersedes lookTarget
            a.facingMode   = NavFacingMode::FaceTarget;
            return true;
        });

    // Intent.Wake(eid): tick this entity's BT next frame, ignoring tickInterval. For edges
    // the ~10 Hz poll would delay by up to a tick: an animation notify, damage taken, a
    // trigger firing. On Intent because the `AI` table is not published.
    tbl.set_function("Wake",
        [&world](LuaEntity entityId) -> bool
        {
            const Entity e = LuaId::ResolveLive(world, entityId);
            auto* ai = e != NullEntity ? world.GetComponent<AIComponent>(e) : nullptr;
            if (!ai) return false;
            ai->wakeRequested = true;
            return true;
        });

    // Intent.ClearLookAt(eid): release the facing override; rotation reverts to
    // FaceMovement (the default: align with the steering direction).
    tbl.set_function("ClearLookAt",
        [&world](LuaEntity entityId) -> bool
        {
            const Entity e = LuaId::ResolveLive(world, entityId);
            auto* a = e != NullEntity ? world.GetComponent<NavAgentComponent>(e) : nullptr;
            if (!a) return false;
            a->useLookAt    = false;
            a->facingTarget = NullEntity;   // also release Intent.FaceEntity
            a->facingMode   = NavFacingMode::FaceMovement;
            return true;
        });

    // Intent.HasMoveTarget(eid): for BTs that branch on "am I under move orders".
    tbl.set_function("HasMoveTarget",
        [&world](LuaEntity entityId) -> bool
        {
            const Entity e = LuaId::ResolveLive(world, entityId);
            const auto* a = world.GetComponent<NavAgentComponent>(e);
            return a && a->hasDestination;
        });

    // Intent.SetSpeedScale(eid, scale): runtime gait control, scaling the authored
    // NavAgent.speed (0.5 = strafe / patrol walk, 1.0 = full chase). The character's real
    // velocity changes, so CharacterAnimDriver picks walk / run from the motor instead of
    // the BT guessing an animation.
    tbl.set_function("SetSpeedScale",
        [&world](LuaEntity entityId, float scale) -> bool
        {
            const Entity e = LuaId::ResolveLive(world, entityId);
            if (e == NullEntity) return false;
            auto& a = GetOrCreateNavAgent(world, e);
            a.speedScale = scale < 0.f ? 0.f : scale;
            return true;
        });

    // Intent.RequestStop(eid): escape hatch for cutscene / teleport, zeroing CCC velocity on
    // the next physics step. With Intent.Stop (which clears nav) it fully freezes the
    // entity:
    //   Intent.Stop(e); Intent.RequestStop(e)
    tbl.set_function("RequestStop",
        [&world](LuaEntity entityId) -> bool
        {
            const Entity e = LuaId::ResolveLive(world, entityId);
            auto* cc = e != NullEntity ? world.GetComponent<CharacterControllerComponent>(e) : nullptr;
            if (!cc) return false;
            cc->instantStopRequested = true;
            return true;
        });

    lua["Intent"] = tbl;
    LOG_INFO("Intent: registered Lua bindings");
}

} // namespace Intent
