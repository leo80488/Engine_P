#include "ECS/DissolveComponent.h"

#include "ECS/FrameContext.h"
#include "ECS/CommandBuffer.h"

#include <algorithm>

void DissolveSystem::OnRegister(World& world)
{
    // Lua adds the component lazily, but this system runs in parallel (Write<DissolveComponent>
    // only): pre-create the pool so it never grows on a worker mid-tick.
    world.EnsurePool<DissolveComponent>();
}

void DissolveSystem::Update(World& world, const FrameContext& ctx)
{
    // Scaled dt: dissolve pauses / slows with hit-stop and time-scale, like the Lua tween queue.
    const float dt = ctx.scaledDeltaTime;
    if (dt <= 0.f) return;

    // Destruction goes through the CommandBuffer (flushed at the phase boundary), keeping the
    // Write<DissolveComponent>-only DeclareAccess honest.
    world.ForEach<DissolveComponent>([&](Entity e, DissolveComponent& d)
    {
        if (d.amount != d.target)
        {
            // duration is for a full 0 -> 1 sweep, so partial moves keep constant speed; <= 0 snaps.
            const float step = (d.duration > 1e-4f) ? dt / d.duration : 1.0f;
            d.amount = (d.amount < d.target)
                     ? std::min(d.amount + step, d.target)
                     : std::max(d.amount - step, d.target);
        }

        if (d.destroyOnComplete && d.amount >= 1.0f && ctx.commandBuffer)
            ctx.commandBuffer->DestroyEntity(e);
    });
}

void DissolveSystem::DeclareAccess(SystemAccessBuilder& b) const
{
    // In-place lerp only; destruction is deferred via the CommandBuffer.
    b.Write<DissolveComponent>();
}
