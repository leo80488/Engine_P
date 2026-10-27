#pragma once

// Internal to src/Physics: what the Physics.* Lua binding files share.

#include "ECS/ECS.h"   // World
#include "Physics/PhysicsSystem.h"

#include <cstdint>

#include <sol/sol.hpp>

namespace DX12Physics
{
    // The one place that turns an authored layer selector into a mask, so every binding reads the same
    // value the same way. Accepts:
    //   "HurtBox"                      one layer by name
    //   { "StaticEnv", "DynamicProp" } several, OR-ed
    //   Physics.Layers.HurtBox         a raw mask (already bit-shifted)
    //   nil / anything else            @p fallback (also for a name or list with no known layer)
    std::uint16_t ParseLayerMask(const sol::object& v, std::uint16_t fallback, const char* whereForLog);

    // The `filter` argument of a query: anything ParseLayerMask reads, or a table
    //   { layers = <as above>, triggers = "include" | "exclude" | "only", ignore = <entity> }
    // (every key optional). @p fallbackMask is the layer mask when none is given.
    bool ParseQueryFilter(const sol::object& v, std::uint16_t fallbackMask, const char* whereForLog, World& world,
                          QueryFilter& out);

    // Binds Raycast, RayAll, SphereCast, BoxCast, CapsuleCast, OverlapSphere, OverlapBox and OverlapCapsule onto
    // @p physics (LuaPhysicsQueries.cpp).
    void RegisterLuaPhysicsQueries(sol::state& lua, sol::table& physics, PhysicsSystem& ps, World& world);
}
