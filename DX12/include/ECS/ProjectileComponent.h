#pragma once

// ProjectileComponent + ProjectileSystem: straight-line / ballistic movers that stop on world
// geometry.
//
// Not a damage system: a projectile moves and blocks. Damage comes from an open hitbox slot on it,
// so HitboxSystem gives it the same overlap, dedupe, sub-stepping and affiliation filtering as a
// sword swing; do not add a second damage path here.
//
// Phase GameplayPostLogic: after Lua Logic, so a script that spawns or redirects a projectile is
// honoured the same frame; it writes LocalTransform, which LocalToWorld (TickPhase::Animation)
// turns into the GlobalTransform HitboxSystem (TickPhase::BoneAttachment) reads, so the damage
// volume is at the current position, not last frame's.
//
// Movement is a swept sphere cast, so a 40 m/s arrow still stops at a 0.2 m wall it crosses
// between frames. The system never destroys anything (structural changes belong to opaque
// systems): on impact it zeroes the velocity and expires the LifetimeComponent, if any, so
// LifetimeSystem destroys it next frame.

#include "ECS/ISystem.h"
#include "ECS/ECS.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>
#include <cstdint>

namespace DX12Physics { class PhysicsSystem; }

struct ProjectileComponent : ComponentBase
{
    // World-space velocity, m/s. Integrated every frame; gravity is applied to
    // Y only so a "straight line" projectile is just gravity = 0.
    DirectX::XMFLOAT3 velocity = { 0.f, 0.f, 0.f };
    float             gravity  = 0.f;      // m/s^2, negative = falls

    // Radius of the sweep used to find world blockers. Independent of the
    // damage volume's radius on purpose: an arrow is a thin thing that should
    // squeeze through a gap, while its damage volume can be generous.
    float             radius   = 0.1f;

    // Which layers stop it. 0 = StaticEnv | DynamicProp: the world and props, not characters -
    // characters are the damage volume's job, and a projectile that both stops on and damages a target
    // would resolve the two in an order nobody authored.
    std::uint16_t     blockMask = 0;

    // Orient the entity along its travel direction. Off for a spinning or
    // billboarded effect that owns its own rotation.
    bool              faceVelocity = true;

    // Zero the velocity and expire the LifetimeComponent (if any) on impact.
    bool              stopOnImpact = true;

    // ---- Runtime ----
    // Has the sweep ever found clear space? Until then a zero-distance hit means the projectile was
    // spawned inside geometry (a muzzle near a wall, the floor or the caster's own collider), so it
    // escapes instead of dying on frame one; blocking counts only once it has been in free space.
    bool              cleared    = false;
    bool              impacted   = false;      // latched after the first block
    Entity            hitEntity  = NullEntity; // what stopped it
    DirectX::XMFLOAT3 hitPoint   = { 0.f, 0.f, 0.f };
    DirectX::XMFLOAT3 hitNormal  = { 0.f, 1.f, 0.f };
};

// Published once, on the frame a projectile is blocked. Carries everything an
// impact VFX / decal / sound needs without the listener re-querying anything.
struct ProjectileImpactEvent
{
    Entity            projectile = NullEntity;
    Entity            hitEntity  = NullEntity;   // NullEntity = expired, not blocked
    DirectX::XMFLOAT3 point      = { 0.f, 0.f, 0.f };
    DirectX::XMFLOAT3 normal     = { 0.f, 1.f, 0.f };
};

class ProjectileSystem final : public SystemInPhase<TickPhase::GameplayPostLogic>
{
public:
    explicit ProjectileSystem(DX12Physics::PhysicsSystem* physics) : m_physics(physics) {}

    const char* GetName() const override { return "ProjectileSystem"; }
    void Update(World&, const FrameContext&) override;
    void DeclareAccess(SystemAccessBuilder& b) const override;

private:
    DX12Physics::PhysicsSystem* m_physics = nullptr;
};
