#include "ECS/DecalSystem.h"
#include "ECS/Components.h"

#include <vector>

void DecalLifetimeSystem::Update(World& world, float dt)
{
    if (dt <= 0.0f) return;

    auto* pool = world.GetPool<DecalComponent>();
    if (!pool || pool->Size() == 0) return;

    // Fields are mutated in place; destroy / remove is deferred until after the loop so a
    // pool reshuffle (swap-with-back erase) cannot disturb the iteration.
    std::vector<Entity> toDestroy;
    std::vector<Entity> toRemoveComp;

    const auto& entities = pool->Entities();
    auto& data = pool->Data();

    for (size_t i = 0; i < entities.size(); ++i)
    {
        Entity e = entities[i];
        DecalComponent& dc = data[i];

        // Static decals: nothing to tick.
        if (dc.lifetime < 0.0f) continue;

        dc.lifetime -= dt;

        if (dc.lifetime <= 0.0f)
        {
            // Expired: queue for post-loop cleanup.
            if (dc.destroyEntityOnExpire) toDestroy.push_back(e);
            else                          toRemoveComp.push_back(e);
            continue;
        }

        // fadeOutDuration == 0 means "pop off at expiry, no fade" (and guards the divide).
        if (dc.fadeOutDuration > 0.0f && dc.lifetime < dc.fadeOutDuration)
            dc.fadeAlpha = dc.lifetime / dc.fadeOutDuration;
        else
            dc.fadeAlpha = 1.0f;
    }

    for (Entity e : toRemoveComp)
        world.RemoveComponent<DecalComponent>(e);
    for (Entity e : toDestroy)
        world.DestroyEntity(e);
}
