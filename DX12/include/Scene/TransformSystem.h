#pragma once

// TransformSystem: propagates LocalTransform -> GlobalTransform down the scene hierarchy
// (parents before children), inherits VisibilityComponent::inheritedHidden and rebuilds
// the WorldAabb of each child that has a LocalAabb and a WorldAabb.
//   root (no Parent, or Parent == NullEntity): global = local; its WorldAabb is not
//     written here (its owner, e.g. a skeleton's bone-merged box, writes it).
//   child: global = local x parent global.
// An entity without a LocalTransform keeps the GlobalTransform its owner writes.
// Every GlobalTransform owner is visited per call (O(N)); a per-entity value cache skips
// the recompose when no input changed, and large scenes fan out to TaskSystem workers.
// Call once per frame before rendering, after all transform mutations.

class World;

class TransformSystem
{
public:
    static void Propagate(World& world);

    // Re-propagate only the descendants of @p root, taking @p root's GlobalTransform as
    // current (the root itself is not recomputed; no-op without Children). Call it after an
    // out-of-band write to one entity's GlobalTransform once Propagate has run, e.g.
    // PhysicsSystem::ApplyRenderInterpolation's interpolated pose: without it a child (the
    // skinned mesh under a CharacterController capsule) renders one physics step behind the
    // body, seen as jitter that grows as the camera gets closer.
    static void PropagateSubtree(World& world, unsigned int root);
};
