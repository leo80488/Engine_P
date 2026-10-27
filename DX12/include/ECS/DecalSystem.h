#pragma once

// DecalLifetimeSystem - ticks the lifetime countdown and fade-out of every DecalComponent.
// - Dynamic decals (lifetime >= 0) lose dt per frame; static ones (lifetime < 0) are skipped.
// - fadeAlpha goes 1 -> 0 linearly over the last fadeOutDuration seconds.
// - On expiry it destroys the entity or only removes the DecalComponent
//   (DecalComponent::destroyEntityOnExpire).
// Entities and components are mutated only after the pool scan (expired ids are collected first).

#include "ECS/ECS.h"

class DecalLifetimeSystem
{
public:
    // Once per frame; Renderer::TickAnimationChain calls it after the transform / follow stages.
    void Update(World& world, float dt);
};
