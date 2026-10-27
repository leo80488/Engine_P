#pragma once

// LifetimeComponent - fixed-duration auto-destroy timer for runtime-spawned entities (VFX prefab
// instances, projectiles, short-lived debug markers).
//
// Intentionally has no ComponentSerializer: a saved timer would expire mid-save. An .iscn-loaded
// entity that needs a lifetime gets it attached after load.
//
// LifetimeSystem decrements `remaining` by frame dt every tick and queues a deferred destroy on the
// phase CommandBuffer once it reaches zero (flushed serially at the phase boundary). Children are
// NOT auto-destroyed: the canonical pattern is at most one level of attached follower, wired by the
// spawning code (FollowSocket / FollowEntity).

#include "ECS/ECS.h"
#include "ECS/ISystem.h"

struct LifetimeComponent
{
    float remaining = 1.f;   // seconds until destroy; <=0 triggers destroy this frame
    float total     = 1.f;   // initial duration, kept for inspector readouts / fades
};

// Runs early in the frame so an expired emitter skips one more TransformSystem / particle update.
// GameplayPreLogic also precedes Animation / TimelineSystem, so a notify fired this frame cannot
// destroy an entity spawned the same frame.
class LifetimeSystem final : public SystemInPhase<TickPhase::GameplayPreLogic>
{
public:
    const char* GetName() const override { return "LifetimeSystem"; }
    void Update(World& world, const FrameContext& ctx) override;
    void DeclareAccess(SystemAccessBuilder& b) const override;
};
