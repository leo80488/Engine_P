#pragma once
// Hitbox.IsActive - whether a hitbox slot of an entity is active this frame. The answer comes from
// the registered HitboxSystem (ECS/NotifyConsumerSystems.h).

namespace sol { class state; }
class World;
class SystemRegistry;

// Adds IsActive to the `Hitbox` table, so call it AFTER the Physics bindings, which
// create that table. The references are App's members and outlive the Lua VM.
void RegisterLuaHitboxBindings(sol::state& state, World& world, SystemRegistry& systemRegistry);
