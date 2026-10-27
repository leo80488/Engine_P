#include "ECS/LifetimeComponent.h"

#include "ECS/FrameContext.h"
#include "ECS/CommandBuffer.h"

#include <vector>

void LifetimeSystem::Update(World& world, const FrameContext& ctx)
{
    // Unscaled wall-clock dt, so a hit-stop (timeScale = 0) does not stretch VFX-prefab cleanup
    // indefinitely: a "2 second duration" means 2 wall-clock seconds even while time is paused.
    const float dt = ctx.deltaTime;
    if (dt <= 0.f) return;

    // Destroy through the CommandBuffer (applied at the phase boundary). Do not call
    // World::DestroyEntity here: it erases from EVERY pool and fires destroy listeners (Lua / Audio /
    // Renderer), which is illegal while this system is co-batched onto a worker, and it would make the
    // Write<LifetimeComponent>-only DeclareAccess a lie.
    world.ForEach<LifetimeComponent>([&](Entity e, LifetimeComponent& lt)
    {
        lt.remaining -= dt;
        if (lt.remaining <= 0.f && ctx.commandBuffer)
            ctx.commandBuffer->DestroyEntity(e);
    });
}

void LifetimeSystem::DeclareAccess(SystemAccessBuilder& b) const
{
    // In-place timer decrement only; destruction is deferred via the
    // CommandBuffer and lands serially at the phase boundary.
    b.Write<LifetimeComponent>();
}
