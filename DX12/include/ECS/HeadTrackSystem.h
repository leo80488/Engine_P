#pragma once

// HeadTrackSystem: a look-at bone constraint that swings a character's head bone toward its
// target (HeadTrackComponent), clamped to per-entity yaw/pitch limits.
//
// Call order (driven from Renderer::TickAnimationChain, like IKSystem / ChainPhysicsSystem):
//   AnimationSystem::Update (clips, pre-IK grants) -> IKSystem / IKRigSystem ->
//   AnimationSystem::ApplyPostIKGrants (post-IK D-bones) -> HeadTrackSystem::Update (this: the
//   head's local pose) -> the modules' AfterHeadTrack hooks (a tracked face) ->
//   ChainPhysicsSystem::Update (hair reacts to the turn) -> LocalToWorldSystem::Update (local -> world).
//
// It reads the head bone's animated local rotation, finds the direction to the target in the
// head's parent frame, clamps the deviation from the animated look to the yaw/pitch limits, and
// composes a swing on top of the animation (never overwrites it, which would discard the keyed
// pose). The swing is weighted by HeadTrackComponent::weight and smoothed while dt > 0.
//
// No teardown: per-entity smoothing state lives on the component, so a world clear or an entity
// destroy needs nothing here.

#include "ECS/ECS.h"
#include "ECS/AnimationSystem.h"
#include "Resource/SkeletonAsset.h"

#include <unordered_set>

class HeadTrackSystem
{
public:
    explicit HeadTrackSystem(AnimationSystem& animSys, SkeletonRegistry& skeletons)
        : m_animSys(animSys), m_skeletons(skeletons) {}

    // Swing every HeadTrackComponent's head bone toward its target. @p dt is the animation delta
    // (0 in Edit / paused: tracking applies instantly); @p activeSet, when non-null, limits the update
    // to the anim-visible entities.
    void Update(World& world, float dt,
                const std::unordered_set<Entity>* activeSet = nullptr);

    // Editor A/B toggle: when false, Update() returns at once and heads keep their raw animated pose.
    bool IsEnabled() const { return m_enabled; }
    void SetEnabled(bool v) { m_enabled = v; }

    // How much of @p e's head another system holds this frame, 0..1 (a tracked face, set by its
    // module before Update runs): the swing gives way by it. Null = nothing else holds a head.
    using AuthorityFn = float (*)(World& world, Entity e);
    void SetHeadAuthority(AuthorityFn authority) { m_headAuthority = authority; }

private:
    AnimationSystem&  m_animSys;
    SkeletonRegistry& m_skeletons;
    bool              m_enabled = true;
    AuthorityFn       m_headAuthority = nullptr;
};
