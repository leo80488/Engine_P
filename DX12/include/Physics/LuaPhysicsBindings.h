#pragma once

// Lua bindings for the physics system: the Physics table (Raycast, SphereCast, OverlapSphere,
// BodyCount, Layers / MaskDefault / MaskAll, SpawnBall) and the Hitbox and Projectile tables.
// Registered once at startup, after PhysicsSystem is initialised and before any user script runs.

namespace sol { class state; }
class World;

namespace DX12Physics
{
    class PhysicsSystem;

    // @p world is where SpawnBall and Projectile.Spawn create their entities and Hitbox.* finds
    // its mailboxes.
    void RegisterLuaPhysicsBindings(sol::state& lua, PhysicsSystem& physics, World& world);
}
