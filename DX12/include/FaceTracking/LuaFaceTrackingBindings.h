#pragma once
// FaceTracking.* - the face tracking service's sources, and the characters bound to them, from
// Lua. Documented in Lua_API_Reference.md.

namespace sol { class state; }
class World;
class SkeletonRegistry;

namespace FaceTracking
{
    // Registers the global `FaceTracking` table. @p world / @p skeletons: the characters'.
    void RegisterLuaBindings(sol::state& state, World& world, SkeletonRegistry& skeletons);
}
