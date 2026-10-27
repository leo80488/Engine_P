#include "ECS/ProjectileComponent.h"

#include "ECS/ECS.h"
#include "ECS/FrameContext.h"
#include "ECS/HierarchyComponents.h"    // LocalTransform
#include "ECS/LifetimeComponent.h"
#include "ECS/ExclusiveResourceIds.h"
#include "Physics/PhysicsSystem.h"
#include "System/EventBus.h"
#include "System/Log.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

void ProjectileSystem::DeclareAccess(SystemAccessBuilder& b) const
{
    b.Write<ProjectileComponent>();
    b.Write<LocalTransform>();
    b.Write<LifetimeComponent>();   // expiring on impact, never adding one
    b.ExclusiveResource(ExclusiveRes::kEventBus);   // ProjectileImpactEvent
    // The swept-sphere step is a Jolt CastSphereClosest, which by engine convention takes
    // kPhysicsWorld.
    b.ExclusiveResource(ExclusiveRes::kPhysicsWorld);
}

void ProjectileSystem::Update(World& world, const FrameContext& ctx)
{
    auto* pool = world.GetPool<ProjectileComponent>();
    if (!pool) return;

    const float dt = ctx.deltaTime;
    if (dt <= 0.f) return;      // paused / stepped frame: do not integrate

    auto&        data     = pool->Data();
    const auto&  entities = pool->Entities();
    const size_t n        = data.size();
    auto&        bus      = EventBus::Get();

    for (size_t i = 0; i < n; ++i)
    {
        ProjectileComponent& p = data[i];
        const Entity         e = entities[i];
        if (p.impacted) continue;               // latched - nothing more to do

        auto* lt = world.GetComponent<LocalTransform>(e);
        if (!lt) continue;                      // nothing to move

        // Gravity first so the sweep uses the velocity actually travelled this frame (symplectic Euler:
        // applying it after would land a ballistic arc systematically short).
        p.velocity.y += p.gravity * dt;

        const XMVECTOR from = XMLoadFloat3(&lt->translation);
        const XMVECTOR vel  = XMLoadFloat3(&p.velocity);
        const XMVECTOR step = XMVectorScale(vel, dt);
        const float    dist = XMVectorGetX(XMVector3Length(step));

        XMVECTOR to = XMVectorAdd(from, step);

        // ---- Swept block test ----
        // A teleport-then-overlap would let anything faster than its own radius per frame pass through a
        // wall; CastSphereClosest sweeps.
        if (m_physics && dist > 1e-5f)
        {
            const DX12Physics::LayerMask mask =
                p.blockMask != 0
                    ? static_cast<DX12Physics::LayerMask>(p.blockMask)
                    : static_cast<DX12Physics::LayerMask>(
                          DX12Physics::MakeLayerMask(DX12Physics::Layer::StaticEnv) |
                          DX12Physics::MakeLayerMask(DX12Physics::Layer::DynamicProp));

            XMFLOAT3 fromF; XMStoreFloat3(&fromF, from);
            XMFLOAT3 dirF;  XMStoreFloat3(&dirF, XMVector3Normalize(step));

            const DX12Physics::ShapeCastHit hit = m_physics->CastSphereClosest(
                fromF, dirF, (std::max)(0.001f, p.radius), dist,
                0xffffffffu, mask);

            // Spawned inside something: ignore the hit until the sweep has seen clear space once (see
            // ProjectileComponent::cleared).
            const bool startedPenetrating = hit.hit && hit.distance <= 1e-4f && !p.cleared;
            if (!hit.hit) p.cleared = true;

            if (hit.hit && !startedPenetrating)
            {
                p.cleared = true;
                p.impacted  = true;
                p.hitEntity = hit.entity;
                p.hitPoint  = hit.point;
                p.hitNormal = hit.normal;
                to = XMVectorAdd(from, XMVectorScale(XMVector3Normalize(step),
                                                     (std::max)(0.f, hit.distance)));

                if (p.stopOnImpact)
                {
                    p.velocity = { 0.f, 0.f, 0.f };
                    // Hand the destroy to LifetimeSystem: destroying an entity is structural, and this system
                    // declares component access instead of going opaque.
                    if (auto* life = world.GetComponent<LifetimeComponent>(e))
                        life->remaining = 0.f;
                }

                ProjectileImpactEvent ev;
                ev.projectile = e;
                ev.hitEntity  = hit.entity;
                ev.point      = hit.point;
                ev.normal     = hit.normal;
                bus.Publish<ProjectileImpactEvent>(std::move(ev));
            }
        }

        XMStoreFloat3(&lt->translation, to);

        // ---- Facing ----
        if (p.faceVelocity && !p.impacted)
        {
            const float speedSq = XMVectorGetX(XMVector3LengthSq(vel));
            if (speedSq > 1e-6f)
            {
                // +Z is forward. Build the rotation that takes +Z onto the travel direction; guard the
                // antiparallel case, where the cross product degenerates and the quaternion would be NaN.
                const XMVECTOR fwd = XMVector3Normalize(vel);
                const XMVECTOR z   = XMVectorSet(0.f, 0.f, 1.f, 0.f);
                const float    d   = XMVectorGetX(XMVector3Dot(z, fwd));
                XMVECTOR q;
                if (d < -0.9999f)
                {
                    q = XMQuaternionRotationAxis(XMVectorSet(0.f, 1.f, 0.f, 0.f), XM_PI);
                }
                else
                {
                    const XMVECTOR axis = XMVector3Cross(z, fwd);
                    q = XMQuaternionNormalize(
                        XMVectorSetW(axis, 1.f + d));
                }
                XMStoreFloat4(&lt->rotation, q);
            }
        }
    }
}
