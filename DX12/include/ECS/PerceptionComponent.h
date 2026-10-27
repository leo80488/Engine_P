#pragma once

// PerceptionComponent - the AI sensing cache. AI::PerceptionSystem (physics broad phase +
// line-of-sight rays) writes it; it is read by
//   AI::UpdateBehavior - AIBehaviorComponent target selection into the blackboard, run by AISystem
//                        before each agent's BT tick;
//   Lua Perception.*   - GetTarget / CanSee / GetLastKnown / GetStats, for BT leaves and benchmarks.
// The readers in the design sketch (docs/design/character_movement_architecture.md section 3.1)
// are not the code: its BehaviorTreeSystem does not exist and AITacticalSystem never reads this.
// Perception runs on its own tickInterval (default 0.1 s), not every frame: visibility rays are
// expensive.

#include "ECS/ECS.h"

#include <DirectXMath.h>

#include <cstdint>
#include <vector>

// One entity the AI can currently see, with its sighting times.
struct PerceivedEntity
{
    Entity            entity        = NullEntity;
    DirectX::XMFLOAT3 lastKnownPos  = { 0.f, 0.f, 0.f };
    float             firstSeenTime = 0.f;   // when this target entered LOS this session
    float             lastSeenTime  = 0.f;   // for "I saw them N seconds ago" gates
    uint32_t          generation = 0;       // guards cached sightings after entity reuse
};

// One sound the AI heard.
struct PerceivedSound
{
    DirectX::XMFLOAT3 origin       = { 0.f, 0.f, 0.f };
    float             loudness     = 0.f;   // gameplay-defined; higher = more attention
    float             heardAtTime  = 0.f;
    Entity            instigator   = NullEntity;  // who made the noise (NullEntity = unknown)
};

struct PerceptionComponent : ComponentBase
{
    // Things the AI currently sees, rebuilt every PerceptionSystem tick by the broad phase,
    // faction, view-cone and line-of-sight tests.
    std::vector<PerceivedEntity> visibleEntities;

    // Sounds heard within the last `audibleFor` seconds.
    // TODO: hearing - nothing fills or prunes this yet; hearingRange / audibleFor are authored but
    // unread.
    std::vector<PerceivedSound>  heardSounds;

    // Sticky last known threat: survives losing line of sight so the AI can investigate where the
    // target disappeared. Cleared once `threatMemory` seconds pass without a sighting, or when the
    // threat dies.
    Entity            lastKnownThreat         = NullEntity;
    DirectX::XMFLOAT3 lastKnownThreatPosition = { 0.f, 0.f, 0.f };
    float             lastSeenTime            = 0.f;

    // ---- Tunables ----
    float             sightRange   = 15.f;
    float             sightConeDeg = 90.f;  // HALF-angle
    float             hearingRange = 12.f;
    float             audibleFor   = 8.f;   // sound memory (s); unread until hearing exists

    // Ray origin above the entity origin. Perception rays leave the eyes, not
    // the feet: from the floor, every kerb and step reads as an occluder.
    float             eyeHeight    = 1.6f;

    // Only entities whose PerceptibleComponent::faction DIFFERS are reported.
    // 0 vs 0 therefore means "nobody sees anybody", the safe default for a
    // component added without reading this comment.
    std::uint32_t     faction      = 0;

    // How long lastKnownThreat survives after the target leaves view.
    float             threatMemory = 5.f;

    // Own cadence, independent of AIComponent::tickInterval. Perception is
    // worth running faster than the tree (you want to notice on the frame it
    // happens) but it is also the expensive half, so it gets its own knob.
    float             tickInterval = 0.1f;
    float             timeSinceLastTick = 0.f;
    // Runtime: has this agent been given its anti-convoy phase offset? Seeded on the first tick from
    // a hash of the entity id (see AIComponent: a population spawned on one frame must not stay in
    // lockstep).
    bool              phaseSeeded  = false;

    // Which physics layers the broad-phase sweep collects. Configurable because what is perceivable
    // is a project decision, and a wrong hardcoded layer fails silently: zero candidates, full query
    // cost, and nothing reports that nobody can see anything (agents spawned via Physics.SpawnBall
    // land on DynamicProp, not Character). Bits are DX12Physics::MakeLayerMask (1 << Layer); 0 = the
    // Character layer, which a scene without the key gets.
    std::uint16_t     layerMask    = 0;

    // Candidate cap per broad-phase query. Bounds the worst case in a crowd:
    // 500 agents in one plaza legitimately return hundreds of candidates, and
    // ray-testing all of them is how a p99 spike gets built.
    std::uint16_t     maxCandidates = 64;
    bool              requireLineOfSight = true;

    // Last tick's query counts (candidates, rays), so the perception benchmark can report them
    // without a profiler build.
    std::uint16_t     lastCandidates = 0;
    std::uint16_t     lastRays       = 0;
};

// Anything that can be SEEN. Passive and cheap: PerceptionSystem never iterates this pool, it
// only looks the component up on overlap candidates. Split from PerceptionComponent so a player
// or a prop is perceivable without paying to perceive.
struct PerceptibleComponent : ComponentBase
{
    bool enabled = true;
    std::uint32_t faction = 0;
    // Where the "body" is relative to the entity origin. Line-of-sight rays
    // aim here, so a target behind a low wall is still visible; also what gets
    // recorded as lastKnownThreatPosition.
    float         offsetY = 1.2f;
};
