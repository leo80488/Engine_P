#pragma once

// FollowSystem: drives the GlobalTransform of entities with Follow* components.
//
// Order (Renderer::TickAnimationChain): Animation -> IK -> HeadTrack -> ChainPhysics ->
// LocalToWorld -> SocketSystem -> FollowSystem -> ... -> SkinMatrix. After SocketSystem so
// FollowSocketComponent reads this frame's socket worldTransform cache; TransformSystem::Propagate
// ran earlier in the frame, so hierarchy-driven targets are current too.
//
// Follow chains (trail follows weapon follows hand socket) resolve in this single pass: Update
// walks each follower's attach dependency to its deepest unresolved link and applies back up.
// That is required for correctness: Propagate treats a parentless follower as a root and
// overwrites its GlobalTransform with its raw LocalTransform every frame, so a follower composed
// against a not-yet-applied target gets a constant matrix, not a one-frame-stale one (see
// FollowSystem::Update).
//
// Known gap: SocketSystem runs before this system and uses the skeleton entity's GlobalTransform
// as its root space, so a character that is itself a follower (on a moving platform) publishes
// socket transforms one frame behind.

#include "ECS/ECS.h"

#include <DirectXMath.h>

class FollowSystem
{
public:
    void Update(World& world);
};

namespace Follow
{
    // Resolve the space `e`'s LocalTransform is expressed in: the matrix FollowSystem composes it
    // against this frame. Returns false when `e` is not attached or its target/socket no longer
    // resolves; the entity is then a plain root and its LocalTransform is world-space. Refreshes the
    // follower's socket-index cache; does not fire target-lost.
    //
    // Priority FollowSocket > FollowEntity, matching who writes GlobalTransform last in
    // FollowSystem::Update. The editor gizmo calls this so a drag produces a LocalTransform in the
    // space FollowSystem reads it back from; otherwise the entity snaps back next frame.
    bool ResolveAttachParentWorld(World& world, Entity e, DirectX::XMMATRIX& out);
}
