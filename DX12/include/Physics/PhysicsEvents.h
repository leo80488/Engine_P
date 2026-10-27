#pragma once

// Events emitted by the physics layer.
// ContactBeganEvent is gameplay-agnostic ("bodyA and bodyB started touching this frame"):
// subscribers filter by TagComponent or component queries, so the bus keeps one broadcast instead
// of one variant per gameplay concept.
// Published on the main thread after the physics step: Jolt's contact callbacks run on worker
// threads, so ContactTracker buffers them under a mutex and PhysicsSystem::PostAllSteps drains them
// after the step. Subscribers (the Lua bridges) get them at EventDelivery's drain points, not inside
// the step.
// One event per pair of bodies, not per Jolt sub-shape contact (ContactTracker.h).

#include "ECS/ECS.h"

#include <DirectXMath.h>

struct ContactBeganEvent
{
    EntityHandle      bodyA;
    EntityHandle      bodyB;
    DirectX::XMFLOAT3 point;     // world-space contact point
    DirectX::XMFLOAT3 normal;    // world-space normal, pointing from bodyA -> bodyB
};

// A pair that began touching has stopped (the last of its contacts is gone), or one of its bodies
// was destroyed or rebuilt: the mirror of ContactBeganEvent, which drives Lua OnExit ("player left
// the trigger zone -> close the door"). A body that falls asleep still touches: no event. A
// destroyed body's side is a null handle. No point or normal: the tracker remembers each pair's
// entities at its first contact and reports only those.
struct ContactEndedEvent
{
    EntityHandle bodyA;
    EntityHandle bodyB;
};
