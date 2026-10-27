#pragma once

// A module's stages in the animation chain (Renderer::TickAnimationChain): the chain calls every
// registered hook at two fixed points, in registration order, on the chain's thread. A hook is
// owned by its module and must outlive the renderer's frames (SkinnedMeshSubsystem::AddChainHook).

#include "ECS/ECS.h"

#include <unordered_set>

class IAnimChainHook
{
public:
    virtual ~IAnimChainHook() = default;

    // After the clip sample, before the facial fold: a hook may write the morph inputs the fold
    // reads. @p dt is the chain's animation delta (0 in Edit / paused); @p activeSet, when non-null,
    // limits the work to the anim-visible entities.
    virtual void BeforeFacial(World& world, float dt, const std::unordered_set<Entity>* activeSet) = 0;

    // After HeadTrack, before chain physics: a hook may write bones (journal each write, or the next
    // AnimationSystem::Update cannot put it back), and hair still follows.
    virtual void AfterHeadTrack(World& world, const std::unordered_set<Entity>* activeSet) = 0;
};
