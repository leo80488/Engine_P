#pragma once

// CharacterStateComponent: the Lua character state list (IDLE / WALK / RUN /
// ATTACK / ...). Each entry pairs a state name with its .ianim clip; the component
// keeps the list so it survives save / load without the Logic script
// re-registering.
// Playback goes through the Animator: Character.AddState also authors an
// AnimationSet alias of the same name, Character.SetState becomes Animator.Play
// when that alias exists, and AnimationClipSystem::ResolveAnimationSets mirrors
// any remaining entries into AnimationSetComponent once (animatorMigrationComplete).
// After that the component only serves the Character.* management / query calls.

#include "ECS/ECS.h"
#include "Resource/SystemHandles.h"   // Resource::AnimHandle
#include "Resource/SkeletonAsset.h"   // kInvalidClipIndex

#include <cstdint>
#include <string>
#include <vector>

struct CharacterStateComponent : ComponentBase
{
    struct StateEntry
    {
        std::string  name;                            // "IDLE", "WALK", ...
        std::string  clipPath;                        // "asset/anims/idle.ianim"
        Resource::AnimHandle clipResHandle{};         // AnimationClipSystem ref
        uint32_t     clipLibIdx     = kInvalidClipIndex; // ClipLibrary index after BindToSkeleton
        bool         looping        = true;
        float        playbackSpeed  = 1.0f;
    };

    std::vector<StateEntry> states;

    // -1 = no state selected yet (no clip playing).
    int32_t currentIdx = -1;
    // -1 = no cross-fade pending; otherwise the state being blended to
    // (blendElapsed -> blendDuration, then it becomes currentIdx).
    int32_t pendingIdx = -1;

    // Active blend parameters.
    float blendDuration = 0.20f;     // total seconds for the in-flight transition
    float blendElapsed  = 0.0f;      // seconds since SetState was called

    // Used when SetState is called without an explicit blend arg.
    float defaultBlendDuration = 0.20f;

    // Runtime-only, one-way: set once ResolveAnimationSets has mirrored these states
    // into the AnimationSet. From then on CharacterStateSystem must not acquire or
    // overwrite clips, so a deleted Animator alias stays deleted instead of being
    // recreated on the next frame.
    bool animatorMigrationComplete = false;
};
