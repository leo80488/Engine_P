#pragma once

// Anim::ResolveAnimEntity - the ONE definition of "which entity holds the animation" for a
// character. On a PMX/FBX character the entity gameplay holds (the root: PlayerComponent,
// CharacterController, scripts) is NOT the one carrying SkeletonComponent +
// AnimationComponent + AnimationSetComponent - that is a descendant spawned from the .iscn.
// Do not copy this walk into a subsystem: copies drift (one without a visited set hangs the
// Animation phase on a cyclic Children graph; one that stops at SkeletonRef authors
// Animator.* aliases onto the root, where the driver never finds them and nothing plays).
//
// Resolution order (self first, so passing an already-resolved entity is free and
// idempotent):
//   1. self, if it carries AnimationComponent
//   2. SkeletonRef->entity, if live
//   3. depth-first Children walk, first entity carrying AnimationComponent
//   4. NullEntity - nothing animated under here
// Callers that want "the root or nothing" fall back to the input themselves
// (ResolveAnimEntityOrSelf); this returns NullEntity rather than guessing.

#include "ECS/ECS.h"
#include "ECS/AnimationComponents.h"   // AnimationComponent / SkeletonRef
#include "ECS/HierarchyComponents.h"   // Children

#include <unordered_set>
#include <vector>

namespace Anim
{

inline Entity ResolveAnimEntity(::World& world, Entity root)
{
    if (root == NullEntity || !world.IsAlive(root)) return NullEntity;
    if (world.GetComponent<AnimationComponent>(root)) return root;
    if (const auto* ref = world.GetComponent<SkeletonRef>(root))
        if (ref->entity != NullEntity && world.IsAlive(ref->entity))
            return ref->entity;

    // Visited-set guarded: an authored Children cycle must not hang the tick.
    std::unordered_set<Entity> visited;
    std::vector<Entity> stack{ root };
    while (!stack.empty())
    {
        const Entity e = stack.back();
        stack.pop_back();
        if (!visited.insert(e).second) continue;
        if (world.GetComponent<AnimationComponent>(e)) return e;
        if (const auto* kids = world.GetComponent<Children>(e))
            for (auto it = kids->entities.rbegin(); it != kids->entities.rend(); ++it)
                stack.push_back(*it);
    }
    return NullEntity;
}

// "Resolve, or keep what you were given." The shape every Animator.* entry
// point wants: a non-character entity with no animated descendant must still
// address itself rather than becoming a silent no-op.
inline Entity ResolveAnimEntityOrSelf(::World& world, Entity root)
{
    if (root == NullEntity || !world.IsAlive(root)) return NullEntity;
    const Entity resolved = ResolveAnimEntity(world, root);
    return resolved != NullEntity ? resolved : root;
}

} // namespace Anim
