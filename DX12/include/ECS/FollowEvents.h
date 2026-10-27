#pragma once

// Events emitted by FollowSystem.
//
// A separate header (not FollowComponents.h) so gameplay code can Subscribe to them without
// pulling in the DirectXMath-heavy component layouts.

#include "ECS/ECS.h"

// FollowTargetLostEvent: the first frame a follower's target ref, resolved before, no longer
// resolves (target destroyed, or a generation mismatch after slot reuse). FollowSystem clears the
// follower's AttachmentRef right after publishing, so it fires exactly once per lost target.
// Typical subscribers: an equipment manager unequipping the orphaned weapon, a VFX pool
// recycling an emitter whose host died, cleanup / lifetime scripts.
struct FollowTargetLostEvent
{
    Entity       follower;        // entity that was following
    EntityHandle formerTarget;    // the now-stale handle it was chasing
};
