#pragma once

// Flat-binding follow components.
//
// Dynamic attachment (a fire emitter following a sword, a trail following a projectile) is a
// component reference, not a parent-child hierarchy. FollowSystem runs after SocketSystem and
// TransformSystem::Propagate, so the target's GlobalTransform is already this frame's.
//
// Transform contract, the same one Parent/Children obey:
//     follower.GlobalTransform = follower.LocalTransform * attachParentWorld
// attachParentWorld is the target's GlobalTransform (FollowEntity) or the socket's cached
// worldTransform (FollowSocket). LocalTransform is the authored offset in attach-parent space,
// exactly as for a real child, and FollowSystem only reads it: the authored scale survives, and
// the gizmo, inspector and serializer need no special case.
//
// Identity: target is an AttachmentRef, serialized as the target's 128-bit Guid and cached at
// runtime as a generation-checked Entity. A raw EntityHandle is a volatile slot index that cannot
// survive a scene save/load. AttachmentRef::Resolve costs one branch on the steady path and probes
// GuidRegistry only when the cache goes stale (target died, world reloaded).

#include "ECS/ECS.h"
#include "ECS/GuidComponent.h"   // AttachmentRef

#include <cstdint>

// FollowEntityComponent: the follower's attach space is target.GlobalTransform.
// Placed on the follower entity (effect / trail / camera rig).
struct FollowEntityComponent
{
    AttachmentRef target;
};

// FollowSocketComponent: the follower's attach space is a named socket published by target.
//
// The target must own a SocketComponent (AnimationComponents.h). The socket is addressed by name
// hash, not array index: removing a socket shifts later entries down, so a baked index would
// silently retarget the attachment. socketIndex is the resolved index, revalidated whenever
// SocketComponent::generation moves. The follower reads SocketSystem's per-frame worldTransform
// cache directly (no skeleton / pose-buffer access).
//
// Attach weapons, shields and VFX to a character with
//   Command::EquipToSocket(world, character, item, "hand_r") / Command::UnequipItem(world, item).
// The character keeps no record of its followers: query the FollowSocket pool for reverse lookup.
struct FollowSocketComponent
{
    AttachmentRef target;               // the character that owns SocketComponent

    // ---- Serialized ----
    // 0 means "address by socketIndex": the VFX bone-attach path (a raw bone index) and asset files
    // without a hash. Otherwise the hash is the authority and socketIndex a cache.
    uint32_t socketNameHash = 0;

    // ---- Resolved index + its validity stamp ----
    // Also serialized: the address for a ref whose socketNameHash is 0.
    uint32_t socketIndex     = 0;
    uint32_t cachedSocketGen = 0;   // vs SocketComponent::generation
};
