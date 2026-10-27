#include "AI/AITacticalSystem.h"

#include "ECS/ECS.h"
#include "ECS/AIIntentComponent.h"
#include "ECS/HierarchyComponents.h"  // GlobalTransform for target positions
#include "Nav/NavComponents.h"

#include <DirectXMath.h>

#include <cmath>

namespace AI
{

namespace {

// World-space position of e, or the origin when e is null or has no GlobalTransform.
DirectX::XMFLOAT3 EntityWorldPos(const ::World& world, Entity e)
{
    if (e == NullEntity) return { 0.f, 0.f, 0.f };
    if (const auto* gt = world.GetComponent<GlobalTransform>(e))
    {
        using namespace DirectX;
        const XMMATRIX m = XMLoadFloat4x4(&gt->matrix);
        return { XMVectorGetX(m.r[3]),
                 XMVectorGetY(m.r[3]),
                 XMVectorGetZ(m.r[3]) };
    }
    return { 0.f, 0.f, 0.f };
}

} // namespace

void AITacticalTick(::World& world, float /*dt*/)
{
    world.ForEach<AIIntentComponent>([&](Entity e, AIIntentComponent& intent)
    {
        auto* nav = world.GetComponent<NavAgentComponent>(e);
        if (!nav) return;   // an intent on an entity without nav: skip

        switch (intent.currentGoal)
        {
            case AIGoal::Idle:
            {
                // Clear the destination but leave facing alone (the BT may still want a look-at).
                nav->hasDestination = false;
                nav->pathValid      = false;
                break;
            }

            case AIGoal::Patrol:
            case AIGoal::Investigate:
            {
                // Go to a fixed point, facing the movement; Investigate behaves like Patrol.
                nav->destination     = intent.goalPosition;
                nav->hasDestination  = true;
                nav->facingMode      = NavFacingMode::FaceMovement;
                nav->facingTarget    = NullEntity;
                nav->useLookAt       = false;
                break;
            }

            case AIGoal::Attack:
            case AIGoal::Follow:
            {
                // Hold a distance from the target (Attack 2.5 m, Follow 4 m), facing it.
                if (intent.targetEntity == NullEntity)
                {
                    nav->hasDestination = false;
                    break;
                }
                const DirectX::XMFLOAT3 tgt = EntityWorldPos(world, intent.targetEntity);
                const DirectX::XMFLOAT3 me  = EntityWorldPos(world, e);
                const float engageDist =
                    (intent.currentGoal == AIGoal::Attack) ? 2.5f : 4.0f;
                const float dx = me.x - tgt.x;
                const float dz = me.z - tgt.z;
                const float d  = std::sqrt(dx * dx + dz * dz);
                DirectX::XMFLOAT3 dest;
                if (d > 1e-3f)
                {
                    const float inv = 1.f / d;
                    dest = { tgt.x + dx * inv * engageDist,
                             tgt.y,
                             tgt.z + dz * inv * engageDist };
                }
                else
                {
                    dest = me;   // on top of the target: stand still
                }
                nav->destination     = dest;
                nav->hasDestination  = true;
                nav->facingMode      = NavFacingMode::FaceTarget;
                nav->facingTarget    = intent.targetEntity;
                nav->useLookAt       = false;   // facingTarget supersedes lookTarget
                break;
            }

            case AIGoal::Flee:
            {
                // Project a destination kFleeDist beyond self along (self - threat); if the path is short or
                // unreachable the agent at least backs away.
                const Entity threat = intent.targetEntity;
                if (threat == NullEntity)
                {
                    nav->hasDestination = false;
                    break;
                }
                const DirectX::XMFLOAT3 t = EntityWorldPos(world, threat);
                const DirectX::XMFLOAT3 me = EntityWorldPos(world, e);
                const float dx = me.x - t.x;
                const float dz = me.z - t.z;
                const float d  = std::sqrt(dx * dx + dz * dz);
                constexpr float kFleeDist = 12.f;
                DirectX::XMFLOAT3 dest;
                if (d > 1e-3f)
                {
                    const float inv = 1.f / d;
                    dest = { me.x + dx * inv * kFleeDist,
                             me.y,
                             me.z + dz * inv * kFleeDist };
                }
                else
                {
                    dest = { me.x + kFleeDist, me.y, me.z };
                }
                nav->destination     = dest;
                nav->hasDestination  = true;
                nav->facingMode      = NavFacingMode::FaceMovement;
                nav->facingTarget    = NullEntity;
                nav->useLookAt       = false;
                break;
            }

            case AIGoal::TakeCover:
            case AIGoal::UseObject:
            default:
                // TakeCover / UseObject are not implemented: leave NavAgent untouched so a previously set
                // destination keeps the agent busy instead of dropping it to Idle.
                break;
        }
    });
}

} // namespace AI
