#pragma once

// CharacterStateSystem: advances the CharacterStateComponent state machines not
// yet migrated to the Animator (animatorMigrationComplete false).
// Per tick: lazily binds each StateEntry's clip (AnimationClipSystem::AcquireClip
// -> BindToSkeleton), then drives the currentIdx -> pendingIdx cross-fade through
// the AnimationComponent primary / secondary clip, times and blendWeight,
// promoting secondary to primary when the blend completes.
// The frame does not tick it: Character.* plays through the Animator and
// ResolveAnimationSets migrates the states into the AnimationSet. A caller of
// Update must run it before AnimationSystem for a switch to show the same frame.

#include "ECS/ECS.h"

namespace Resource { class AnimationClipSystem; }
class SkeletonRegistry;

class CharacterStateSystem
{
public:
    explicit CharacterStateSystem(SkeletonRegistry& skeletons,
                                  Resource::AnimationClipSystem* clipSys)
        : m_skeletons(skeletons), m_clipSys(clipSys) {}

    // dt advances the blend. clipLib is the global ClipLibrary that newly bound clips
    // are registered into, so AnimationSystem can sample them.
    void Update(World& world, float dt, class ClipLibrary& clipLib);

    // Hot-swap if AnimationClipSystem rebinds (e.g., editor reload).
    void SetClipSystem(Resource::AnimationClipSystem* c) { m_clipSys = c; }

private:
    SkeletonRegistry&              m_skeletons;
    Resource::AnimationClipSystem* m_clipSys = nullptr;
};
