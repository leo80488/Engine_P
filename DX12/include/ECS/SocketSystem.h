#pragma once

// SocketSystem: publishes per-entity socket world transforms every frame.
//
// For every entity with a SocketComponent it stores each socket's world transform in place
// (Socket::worldTransform). Followers read that cache through FollowSocketComponent + FollowSystem;
// SocketSystem does not know which entities are attached.
//
// Order (Renderer::TickAnimationChain): AnimationSystem -> IK -> ChainPhysics -> LocalToWorldSystem
// -> SocketSystem -> FollowSystem -> SkinMatrix.
//
// The PoseRingBuffer holds skin matrices (invBindPose * boneModelMatrix) at this point, so
// modelBone = bindPose * skin and worldBone = modelBone * rootGlobalTransform. An entity with
// sockets and no skeleton (a StaticMesh owner's mesh sockets from its model's `SO mesh=` records, a
// prop's own Sockets record) has them at offset x its GlobalTransform.

#include "ECS/ECS.h"

class PoseRingBuffer;
class SkeletonRegistry;

class SocketSystem
{
public:
    SocketSystem(PoseRingBuffer& poseBuffer, SkeletonRegistry& skeletons)
        : m_poseBuffer(poseBuffer), m_skeletons(skeletons) {}

    void Update(World& world);

    // Places @p e's sockets when it has no skeleton: offset x its GlobalTransform as it is now.
    // FollowSystem calls it for a follower it just moved (and its subtree), whose sockets this frame's
    // pass placed from the pre-follow matrix (a held prop's mesh sockets would sit near the origin).
    // Does nothing for an entity with a skeleton (its sockets are on bones) or without sockets.
    static void PlaceWithoutSkeleton(World& world, Entity e);

private:
    // The skinned owners: world = offset x boneModel x rootWorld.
    void UpdateSkeletonOwners(World& world);
    // An owner without a skeleton (a StaticMesh owner's mesh sockets, a prop's own):
    // world = offset x its GlobalTransform.
    void UpdateOwnersWithoutSkeleton(World& world);

    PoseRingBuffer&   m_poseBuffer;
    SkeletonRegistry& m_skeletons;
};
