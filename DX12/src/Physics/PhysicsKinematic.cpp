// PhysicsSystem kinematic movers: a Kinematic body follows the pose its LocalTransform chain gives, moved with
// MoveKinematic at every fixed substep (FixedPhysicsPre), so it carries a velocity: what rests on it, leans on it or
// stands on it moves with it, and a dynamic body in its way is pushed instead of being ejected from an overlap.

#include "PhysicsInternal.h"
#include "Physics/PhysicsSystem.h"

#include "ECS/ECS.h"
#include "ECS/FollowComponents.h"
#include "ECS/HierarchyComponents.h"
#include "ECS/PhysicsComponents.h"
#include "System/CommandLine.h"

#include <Jolt/Jolt.h>

JPH_SUPPRESS_WARNINGS

#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/EActivation.h>

#include <algorithm>
#include <cmath>

namespace DX12Physics
{
namespace
{
    // A body further than this from its target in one substep was moved by something else (a script teleported
    // it): it is placed there, not driven, because a velocity of target / dt would fling whatever touches it.
    constexpr float kTeleportDistance = 5.0f;     // metres
    constexpr float kTeleportAngle    = 1.5f;     // radians
    // Closer than this to the target a body counts as arrived.
    constexpr float kRestDistance     = 1e-5f;    // metres
    constexpr float kRestAngle        = 1e-4f;    // radians

    // A follower's GlobalTransform comes from the entity it follows (a socket, another entity), which the
    // LocalTransform chain does not hold: such a body keeps the pose Propagate wrote last.
    bool IsFollower(World& world, Entity entity)
    {
        for (int depth = 0; depth < 64 && entity != NullEntity; ++depth)
        {
            if (world.HasComponent<FollowSocketComponent>(entity) || world.HasComponent<FollowEntityComponent>(entity))
                return true;
            const Parent* parent = world.GetComponent<Parent>(entity);
            entity = parent ? parent->entity : NullEntity;
        }
        return false;
    }
}

bool KinematicTeleportLegacy()
{
#if WITH_HARNESS
    // -physics_kinematic_teleport: a Kinematic body is teleported with SetPositionAndRotation to the previous
    // frame's GlobalTransform once per frame (PreAllSteps), as before MoveKinematic. The kcc_platform pack's
    // reverse arm.
    static const bool s_on = Sys::CommandLine::Get().Has("-physics_kinematic_teleport");
    return s_on;
#else
    return false;
#endif
}

void PhysicsSystem::MoveKinematicBodies(World& world)
{
    if (!m_impl || KinematicTeleportLegacy()) return;
    auto* rbPool = world.GetPool<RigidBodyComponent>();
    if (!rbPool) return;
    ZoneScopedN("Phys.Kinematic");

    JPH::BodyInterface& bodyIface = m_impl->physics->GetBodyInterface();
    const auto& entities = rbPool->Entities();
    auto&       bodies   = rbPool->Data();

    // The frame's motion is spread evenly over its substeps: this one covers 1 / (substeps left) of the way to
    // the target, so the last of them arrives and a platform does not move in one lunge and then stand still.
    const unsigned left = m_frameSubsteps > m_stepIndexInFrame ? m_frameSubsteps - m_stepIndexInFrame : 1u;
    const float fraction = 1.0f / static_cast<float>(left);

    for (size_t i = 0; i < bodies.size(); ++i)
    {
        RigidBodyComponent& rb = bodies[i];
        // A body that has not been placed yet gets its first pose from PreAllSteps.
        if (rb.motion != RigidBodyComponent::Motion::Kinematic || rb.bodyId == kInvalidPhysicsBodyId ||
            !rb.pushedPoseValid)
            continue;
        const Entity e = entities[i];
        if (!world.HasComponent<LocalTransform>(e)) continue;

        const WorldPose target = ReadEntityWorldPose(world, e, !IsFollower(world, e));
        const JPH::BodyID id(rb.bodyId);
        JPH::RVec3 position;
        JPH::Quat  rotation;
        bodyIface.GetPositionAndRotation(id, position, rotation);

        const JPH::Vec3 delta = target.pos - JPH::Vec3(position);
        const float distance = delta.Length();
        const float angle = 2.0f * std::acos((std::min)(1.0f, std::fabs(rotation.Dot(target.rot))));

        if (distance < kRestDistance && angle < kRestAngle)
        {
            // Arrived: stop the velocity the last MoveKinematic left, or Jolt keeps integrating it.
            if (rb.kinematicDriven)
            {
                bodyIface.SetLinearAndAngularVelocity(id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
                rb.kinematicDriven = false;
            }
            continue;
        }

        rb.pushedPos = { target.pos.GetX(), target.pos.GetY(), target.pos.GetZ() };
        rb.pushedRot = { target.rot.GetX(), target.rot.GetY(), target.rot.GetZ(), target.rot.GetW() };
        if (distance > kTeleportDistance || angle > kTeleportAngle)
        {
            bodyIface.SetPositionAndRotation(id, target.pos, target.rot, JPH::EActivation::Activate);
            bodyIface.SetLinearAndAngularVelocity(id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
            rb.kinematicDriven = false;
            continue;
        }

        const JPH::Vec3 next = JPH::Vec3(position) + delta * fraction;
        const JPH::Quat nextRotation = fraction >= 1.0f ? target.rot : rotation.SLERP(target.rot, fraction);
        bodyIface.MoveKinematic(id, next, nextRotation, Impl::kFixedDt);
        rb.kinematicDriven = true;
    }
}

} // namespace DX12Physics
