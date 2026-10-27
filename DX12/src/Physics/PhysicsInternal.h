#pragma once

// Internal to src/Physics: what PhysicsSystem.cpp and its sibling translation units share - the object layers
// and the collision matrix, the query filters, the per-step scratch arena and PhysicsSystem::Impl. The public
// header (Physics/PhysicsSystem.h) hides all of it behind the pImpl.

#include "Physics/PhysicsSystem.h"
#include "Physics/ContactTracker.h"

#include "ECS/ECS.h"
#include "System/Check.h"          // ENGINE_CHECK_ONCE: the step-arena overflow report

#include <Jolt/Jolt.h>

JPH_SUPPRESS_WARNINGS

#include <Jolt/Core/JobSystem.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Constraints/ContactConstraintManager.h>  // cMaxConstraintSize: sizes the step arena

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace DX12Physics
{
// ==== Layer definitions ====
namespace Layers
{
    // Eight gameplay-aware object layers. Values MUST match the public
    // DX12Physics::Layer enum in PhysicsSystem.h so caller-supplied LayerMask
    // bits line up with the internal layer indices.
    static constexpr JPH::ObjectLayer STATIC_ENV   = 0;  // walls, floors, terrain - never moves
    static constexpr JPH::ObjectLayer DYNAMIC_PROP = 1;  // boxes, debris, vehicles - full physics
    static constexpr JPH::ObjectLayer CHARACTER    = 2;  // player + NPC movement capsule
    static constexpr JPH::ObjectLayer RAGDOLL      = 3;  // death ragdoll per-bone bodies
    static constexpr JPH::ObjectLayer HURT_BOX     = 4;  // per-bone hit-detection (query only)
    static constexpr JPH::ObjectLayer ATTACK_BOX   = 5;  // weapon swing volume (query only)
    static constexpr JPH::ObjectLayer PROJECTILE   = 6;  // bullets / arrows / thrown objects
    static constexpr JPH::ObjectLayer SENSING      = 7;  // AI vision / hearing and trigger volumes
    static constexpr JPH::ObjectLayer NUM_LAYERS   = 8;
}

namespace BPLayers
{
    // Each BP layer is a separate quadtree. A bucket is split out only when it is both (a) often
    // queried on its own and (b) a poor fit for the tree it would otherwise share.
    //
    //   STATIC   StaticEnv: thousands of never-moving bodies on an imported level; their tree is
    //            built once and never re-balanced.
    //   MOVING   everything that takes part in ordinary simulation.
    //   HURTBOX  damage-receiving volumes only, case (a)+(b): melee queries ask for HurtBox alone,
    //            several times per swing, and would otherwise descend the MOVING tree past every
    //            prop, projectile and sensor. HurtBox bodies collide with almost nothing
    //            (kCollidesWith), so in MOVING they would only add bulk.
    static constexpr JPH::BroadPhaseLayer STATIC { 0 };
    static constexpr JPH::BroadPhaseLayer MOVING { 1 };
    static constexpr JPH::BroadPhaseLayer HURTBOX{ 2 };
    static constexpr JPH::uint            NUM_LAYERS = 3;
}

static constexpr std::uint16_t LayerBit(JPH::ObjectLayer l) { return std::uint16_t(1u << l); }

// Symmetric pair-collision matrix: kCollidesWith[L] is the bitmask of layers L interacts with.
// Keep it symmetric (A in kCollidesWith[B] <-> B in kCollidesWith[A]): Jolt may call ShouldCollide
// in either order, and ObjectVsBPFilterImpl derives its bucket table from the same rows.
static constexpr std::uint16_t kCollidesWith[Layers::NUM_LAYERS] = {
    /* STATIC_ENV   */ std::uint16_t(LayerBit(Layers::DYNAMIC_PROP) | LayerBit(Layers::CHARACTER) | LayerBit(Layers::RAGDOLL) | LayerBit(Layers::PROJECTILE)),
    /* DYNAMIC_PROP */ std::uint16_t(LayerBit(Layers::STATIC_ENV)   | LayerBit(Layers::DYNAMIC_PROP) | LayerBit(Layers::CHARACTER) | LayerBit(Layers::RAGDOLL) | LayerBit(Layers::PROJECTILE) | LayerBit(Layers::SENSING)),
    /* CHARACTER    */ std::uint16_t(LayerBit(Layers::STATIC_ENV)   | LayerBit(Layers::DYNAMIC_PROP) | LayerBit(Layers::CHARACTER) | LayerBit(Layers::PROJECTILE) | LayerBit(Layers::SENSING)),
    /* RAGDOLL      */ std::uint16_t(LayerBit(Layers::STATIC_ENV)   | LayerBit(Layers::DYNAMIC_PROP) | LayerBit(Layers::RAGDOLL)   | LayerBit(Layers::PROJECTILE)),
    /* HURT_BOX     */ std::uint16_t(LayerBit(Layers::ATTACK_BOX)   | LayerBit(Layers::PROJECTILE)),
    /* ATTACK_BOX   */ std::uint16_t(LayerBit(Layers::HURT_BOX)),
    /* PROJECTILE   */ std::uint16_t(LayerBit(Layers::STATIC_ENV)   | LayerBit(Layers::DYNAMIC_PROP) | LayerBit(Layers::CHARACTER) | LayerBit(Layers::RAGDOLL) | LayerBit(Layers::HURT_BOX) | LayerBit(Layers::SENSING)),
    // SENSING hosts trigger volumes (isTrigger colliders route here by default) and AI vision /
    // hearing: it must see characters and the things that can trip a trigger (props, projectiles).
    // Sensor bodies have no collision response, so the extra pairs cost only narrow-phase tests while
    // they overlap.
    /* SENSING      */ std::uint16_t(LayerBit(Layers::CHARACTER)     | LayerBit(Layers::DYNAMIC_PROP) | LayerBit(Layers::PROJECTILE)),
};

static constexpr JPH::BroadPhaseLayer ObjectLayerToBP(JPH::ObjectLayer l)
{
    if (l == Layers::STATIC_ENV) return BPLayers::STATIC;
    if (l == Layers::HURT_BOX)   return BPLayers::HURTBOX;
    return BPLayers::MOVING;
}

class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface
{
public:
    BPLayerInterfaceImpl()
    {
        for (JPH::ObjectLayer l = 0; l < Layers::NUM_LAYERS; ++l)
            m_map[l] = ObjectLayerToBP(l);
    }
    JPH::uint            GetNumBroadPhaseLayers() const override { return BPLayers::NUM_LAYERS; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer l) const override { return m_map[l]; }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer l) const override
    {
        if (l == BPLayers::STATIC)  return "STATIC";
        if (l == BPLayers::HURTBOX) return "HURTBOX";
        return "MOVING";
    }
#endif
private:
    JPH::BroadPhaseLayer m_map[Layers::NUM_LAYERS];
};

class ObjectVsBPFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter
{
public:
    // For object layer A, which BP buckets can hold anything A collides with? Derived from
    // kCollidesWith + ObjectLayerToBP rather than written per bucket, so a new bucket cannot leave the
    // table stale (a hand-written `else` would lump HURTBOX in with MOVING and undo the split).
    ObjectVsBPFilterImpl()
    {
        for (JPH::ObjectLayer a = 0; a < Layers::NUM_LAYERS; ++a)
        {
            std::uint32_t buckets = 0;
            for (JPH::ObjectLayer b = 0; b < Layers::NUM_LAYERS; ++b)
                if (kCollidesWith[a] & LayerBit(b))
                    buckets |= std::uint32_t(1u) << ObjectLayerToBP(b).GetValue();
            m_buckets[a] = buckets;
        }
    }

    bool ShouldCollide(JPH::ObjectLayer a, JPH::BroadPhaseLayer b) const override
    {
        if (a >= Layers::NUM_LAYERS) return false;
        return (m_buckets[a] & (std::uint32_t(1u) << b.GetValue())) != 0;
    }
private:
    std::uint32_t m_buckets[Layers::NUM_LAYERS] = {};
};

class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override
    {
        if (a >= Layers::NUM_LAYERS || b >= Layers::NUM_LAYERS) return false;
        return (kCollidesWith[a] & LayerBit(b)) != 0;
    }
};

// Query-side broad-phase twin of LayerMaskFilter: a BP tree is visited only if some layer in the
// public LayerMask lives there. Used by OverlapSphere, the melee overlaps and RunKccUpdates'
// dynamic-body probe, so e.g. a hurtbox query does not descend the whole static level only to
// reject every leaf. Results are identical to passing no filter: a body in a skipped bucket has a
// layer outside the mask, which the object filter would reject anyway.
class LayerMaskBPFilter final : public JPH::BroadPhaseLayerFilter
{
public:
    explicit LayerMaskBPFilter(std::uint16_t mask)
    {
        for (JPH::ObjectLayer l = 0; l < Layers::NUM_LAYERS; ++l)
            if (mask & LayerBit(l))
                m_buckets |= std::uint32_t(1u) << ObjectLayerToBP(l).GetValue();
    }
    bool ShouldCollide(JPH::BroadPhaseLayer l) const override
    {
        return (m_buckets & (std::uint32_t(1u) << l.GetValue())) != 0;
    }
private:
    std::uint32_t m_buckets = 0;
};

// Query-side object-layer filter: accepts a layer whose bit is set in the public LayerMask, so a
// caller can carve out a gameplay subset (e.g. a movement probe without HURT_BOX).
class LayerMaskFilter final : public JPH::ObjectLayerFilter
{
public:
    explicit LayerMaskFilter(std::uint16_t mask) : m_mask(mask) {}
    bool ShouldCollide(JPH::ObjectLayer l) const override
    {
        if (l >= Layers::NUM_LAYERS) return false;
        return (m_mask & LayerBit(l)) != 0;
    }
private:
    std::uint16_t m_mask;
};

// Body filter that excludes one BodyID: skips the character's own body during ground probes and
// collide-and-slide, so the cast does not hit itself at once.
class IgnoreBodyFilter final : public JPH::BodyFilter
{
public:
    explicit IgnoreBodyFilter(JPH::BodyID ignore) : m_ignore(ignore) {}
    bool ShouldCollide(const JPH::BodyID& id) const override
    {
        return !m_ignore.IsInvalid() ? (id != m_ignore) : true;
    }
    bool ShouldCollideLocked(const JPH::Body& body) const override
    {
        return !m_ignore.IsInvalid() ? (body.GetID() != m_ignore) : true;
    }
private:
    JPH::BodyID m_ignore;
};

// ==== JoltStepAllocator: the per-step scratch arena handed to JPH::PhysicsSystem::Update ====
// Jolt takes every per-step working buffer from a TempAllocator and frees them in LIFO order
// before Update() returns, so the arena covers ONE step's peak; it is not a scene budget and does
// not scale with body count.
//
// A wrapper rather than a plain JPH::TempAllocatorImpl, because TempAllocatorImpl::Allocate calls
// std::abort() when its block is exhausted. Here an over-budget step falls back to malloc
// (correct, slower) and reports once through ENGINE_CHECK_ONCE, naming the constant to raise.
class JoltStepAllocator final : public JPH::TempAllocator
{
public:
    explicit JoltStepAllocator(size_t fixedBytes) : m_fixed(fixedBytes) {}

    void* Allocate(JPH::uint inSize) override
    {
        if (inSize == 0) return nullptr;
        if (m_fixed.CanAllocate(inSize))
        {
            void* p = m_fixed.Allocate(inSize);
            if (m_fixed.GetUsage() > m_peakBytes) m_peakBytes = m_fixed.GetUsage();
            return p;
        }
        // Jolt orders allocate / free by job dependencies, so no locking: the counters and the ONCE latch
        // have a single writer.
        ++m_fallbackCount;
        ENGINE_CHECK_ONCE(false,
            "Jolt step arena exhausted: %u-byte request did not fit the %zu KiB "
            "fixed block (peak %zu KiB). Falling back to malloc — correct but "
            "slower. Raise kJoltStepSlackBytes in PhysicsSystem.cpp if this "
            "repeats.", inSize, m_fixed.GetSize() / 1024, m_peakBytes / 1024);
        return m_malloc.Allocate(inSize);
    }

    void Free(void* inAddress, JPH::uint inSize) override
    {
        if (inAddress == nullptr) return;
        // LIFO order is preserved within each half: anything that spilled to
        // malloc was allocated after everything still in the fixed block, so it
        // is also freed first.
        if (m_fixed.OwnsMemory(inAddress)) m_fixed.Free(inAddress, inSize);
        else                               m_malloc.Free(inAddress, inSize);
    }

    size_t GetPeakBytes()     const { return m_peakBytes; }
    size_t GetFallbackCount() const { return m_fallbackCount; }

private:
    JPH::TempAllocatorImpl   m_fixed;
    JPH::TempAllocatorMalloc m_malloc;
    size_t                   m_peakBytes     = 0;
    size_t                   m_fallbackCount = 0;
};

// One character's KCC step (StepOnce's three passes): what the pre-step pass read and wrote, and
// whether the update may run on the task pool (Impl::RunKccUpdates).
struct KccStep
{
    size_t                  i = 0;          // index in the CharacterController pool
    Entity                  e = NullEntity;
    JPH::CharacterVirtual*  cv = nullptr;
    JPH::Vec3               prePos;
    bool                    launched = false;
    bool                    onGroundPre = false;    // standing on ground when the step began
    float                   stepUp = 0.4f;          // the character's step-up (CharacterControllerComponent::stepHeight)
    // KCC_DEBUG >= 1 diagnostics (the write-back pass's detectors).
    JPH::Vec3               lvPre, posPre;
    JPH::CharacterBase::EGroundState gsPre = JPH::CharacterBase::EGroundState::InAir;
    bool                    preWasGrounded = false;
    // RunKccUpdates' classification.
    JPH::AABox              reach;
    bool                    nearBody = false;
};

// ==== Impl ====
struct PhysicsSystem::Impl
{
    static constexpr float        kFixedDt        = 1.0f / 60.0f;
    static constexpr JPH::uint    kMaxBodies      = 65536;
    static constexpr JPH::uint    kNumBodyMutexes = 0;  // 0 = Jolt picks a default
    // kMaxBodyPairs / kMaxContacts size the ContactConstraintManager's two persistent manifold caches
    // (~10 MiB in total). They stay at Jolt's documented "real project" values on purpose (see Init).
    static constexpr JPH::uint    kMaxBodyPairs   = 65536;
    static constexpr JPH::uint    kMaxContacts    = 10240;
    static constexpr int          kCollisionSteps = 1;

    // ---- Per-step scratch arena size, derived from Jolt's own constants ----
    // The dominant term, and the only one that scales with this configuration:
    // ContactConstraintManager::PrepareConstraintBuffer takes
    //   kMaxContacts * (cMaxConstraintSize + sizeof(uint32))
    // for the whole step (cMaxConstraintSize is 832 B on x64: ContactConstraint<Dynamic,Dynamic> + 3
    // extra WorldContactPoints).
    //
    // Everything else Jolt takes from the arena is small and bounded:
    //   * Update()'s in-flight body-pair queue: PhysicsSettings::mMaxInFlightBodyPairs (16384) x
    //     sizeof(BodyPair) (8) = 128 KiB, independent of kMaxBodyPairs;
    //   * IslandBuilder + LargeIslandSplitter: 32-bit index arrays, ~24 B per active body plus ~12 B
    //     per contact - under 400 KiB with all 7k bodies of the heaviest known scene awake;
    //   * soft-body update contexts: none, the engine creates no soft bodies.
    // kJoltStepSlackBytes covers that tail with room for another per-step array in a future Jolt, and
    // an overflow is survivable anyway (JoltStepAllocator). Deriving the total means raising
    // kMaxContacts cannot under-size the arena: 10240 x 836 B + 2 MiB = ~10.2 MiB.
    static constexpr size_t kJoltStepSlackBytes = 2u * 1024u * 1024u;
    static constexpr size_t kJoltStepArenaBytes =
        static_cast<size_t>(kMaxContacts)
            * (JPH::ContactConstraintManager::cMaxConstraintSize + sizeof(JPH::uint32))
        + kJoltStepSlackBytes;
    // Tripwire: if a Jolt upgrade grows the per-constraint record so far that the arena passes 16 MiB,
    // re-derive the slack assumption instead of absorbing the growth silently.
    static_assert(kJoltStepArenaBytes < 16u * 1024u * 1024u,
                  "Jolt step arena now exceeds the 16 MiB it replaced — re-derive "
                  "kJoltStepSlackBytes against the current Jolt version");

    struct PoseSnapshot
    {
        JPH::Vec3 prevPos = JPH::Vec3::sZero();
        JPH::Quat prevRot = JPH::Quat::sIdentity();
        JPH::Vec3 currPos = JPH::Vec3::sZero();
        JPH::Quat currRot = JPH::Quat::sIdentity();
    };

    struct KccShapeSnap { float radius; float halfHeight; bool queryBody; };

    std::unique_ptr<JoltStepAllocator>          tempAlloc;
    std::unique_ptr<JPH::JobSystem>             jobSys;   // TaskSystemJoltJobs, or Jolt's thread pool under -joltpool
    std::unique_ptr<BPLayerInterfaceImpl>       bpLayerIface;
    std::unique_ptr<ObjectVsBPFilterImpl>       objVsBpFilter;
    std::unique_ptr<ObjectLayerPairFilterImpl>  objLayerPairFilter;
    std::unique_ptr<JPH::PhysicsSystem>         physics;
    std::unique_ptr<ContactTracker>             contactTracker;
#ifdef JPH_DEBUG_RENDERER
    std::shared_ptr<PhysicsDebugDraw>           debugDraw;   // made on the first DrawDebug, in its TU
#endif

    // The frame's contact events (rigid bodies and the KCC sensor sweep), reused so a frame allocates
    // nothing.
    std::vector<ContactTracker::Event> contactScratch;

    // Built-shape cache: MeshShapeKey / AssetShapeKey (kind prefix, path, source stamp, entry) ->
    // shape; a null entry records a failed build. Many entities share one baked entry, so each shape
    // is built once.
    std::unordered_map<std::string, JPH::ShapeRefC> meshShapeCache;

    // Bumped whenever meshShapeCache is cleared or its stamps are dropped. Mixed into
    // ColliderComponent::shapePrewarmMemo, so it invalidates every collider's "already cached" memo at
    // once, also on paths such as OnWorldClear() that have no World& to walk the collider pool.
    std::uint32_t shapeCacheEpoch = 1;

    // Rotating slice index of Phase 0.6's mesh-path content re-hash (kPathHashSlices).
    std::uint32_t colliderDiffSlice = 0;

    // Source-file cache: meshLibPath -> raw file bytes. One .meshlib usually holds entries shared by
    // many entities; without it the lazy body creation in Phase 1 would re-read the same multi-MB
    // blob once per entity. The shape cache keys on (path, meshId), so it cannot dedupe the file read
    // by itself.
    std::unordered_map<std::string, std::vector<uint8_t>> blobCache;

    // meshLibPath -> loose-file modification stamp, folded into every shape-cache key by
    // MeshShapeKey() (SourceStamp says why). Cleared with the caches, so each invalidation re-stats
    // once per path.
    std::unordered_map<std::string, std::int64_t> blobStamp;

    // Bodies whose shape did not build (a MeshAsset whose model / prim cannot be read), keyed by
    // (generation, entity), valued with the collider's shape hash at the failure. Phase 1 skips them
    // until the collider changes: no body, and no rebuild attempt (or string key) every frame. Cleared
    // with the world.
    std::unordered_map<std::uint64_t, std::uint64_t> shapeFailed;

    // Pose snapshots for render-side interpolation. Physics steps at a fixed 60 Hz, so a faster render
    // frame often sees no step and the same Jolt pose, and the object would stutter. The pose at the
    // start and the end of the latest step lets ApplyRenderInterpolation lerp / slerp between them by
    // accumulator / fixedDt. Kept for Dynamic bodies (position and rotation) and for character
    // controllers (StepOnce writes prevPos / currPos only; LocalTransform keeps the capsule's rotation).
    std::unordered_map<Entity, PoseSnapshot> poseSnapshots;

    // Per-body change-detection state lives on RigidBodyComponent, not in entity-keyed maps here:
    // PreAllSteps reads it for every body three times per frame, and a hash lookup each time would
    // cost more than the checks it feeds.

    // entity -> live Jolt BodyID of every created RigidBody body (all motion types).
    // World::DestroyEntity erases the RigidBodyComponent (and its bodyId) before physics next ticks,
    // so the Phase 0.6 / 1 loops, which walk only the live pool, never see a destroyed entity; the
    // Phase 0.5 reaper walks this map to find the orphans.
    std::unordered_map<Entity, std::uint32_t> bodyIds;

    // ---- Kinematic Character Controller (KCC) ----
    // One JPH::CharacterVirtual per entity with a CharacterControllerComponent. The Ref keeps it
    // alive and dropping the entry destroys it (a CharacterVirtual does not register with the
    // broadphase itself, so there is no separate body removal). Lifecycle mirrors RigidBody: created
    // lazily on first sight, rebuilt on a capsule change, torn down when the component goes.
    std::unordered_map<std::uint64_t, JPH::Ref<JPH::CharacterVirtual>> kccTable;

    // The capsule (and query-body choice) each CharacterVirtual was built from, to detect a runtime
    // resize and rebuild: CharacterVirtual keeps its shape internally and has no cheap shape swap.
    std::unordered_map<std::uint64_t, KccShapeSnap> kccShapeSnaps;

    // One ExtendedUpdateSettings serves every KCC: the step-offset / snap-to-ground tuning depends on
    // the capsule, not the character (a per-character override would go into the
    // CharacterControllerComponent fields). Built once in Init().
    JPH::CharacterVirtual::ExtendedUpdateSettings kccExtUpdate;
    // The per-step KCC records (reused, one per character) and the parallel subset's indices
    // (StepOnce / RunKccUpdates).
    std::vector<KccStep>  kccSteps;
    // RunKccUpdates' island scratch (kept for capacity, rebuilt every step).
    std::vector<uint32_t> kccParallel, kccIslandParent, kccIslandStart, kccIslandMembers,
                          kccIslandSlot, kccIslandCount;
    struct KccStats { uint64_t steps = 0, chars = 0, islands = 0, largest = 0, pairs = 0, bodies = 0; } kccStats;

    // Per KCC, the sensor entities its capsule overlaps, refreshed by a CollideShape query in
    // PostAllSteps. A CharacterVirtual never reaches the rigid-body ContactListener, so trigger volumes
    // would not see the player without it; diffing previous vs current publishes ContactBeganEvent /
    // ContactEndedEvent on the rigid bodies' path. Cleared in OnWorldClear.
    std::unordered_map<std::uint64_t, std::unordered_set<Entity>> kccSensorTouches;

    void RunKccUpdates(std::vector<KccStep>& steps);
};

// World-space pose for body placement (ReadEntityWorldPose).
struct WorldPose
{
    JPH::Vec3         pos;
    JPH::Quat         rot;
    DirectX::XMFLOAT3 scale;
};

// The world pose of an entity. By default GlobalTransform (kept fresh by TransformSystem::Propagate, so it is the
// pose of the previous frame's end), falling back to the LocalTransform chain; @p physicsPose skips
// GlobalTransform and composes the LocalTransform chain, which is what Jolt last simulated for a Dynamic body and
// what a script wrote this frame for a Kinematic one.
WorldPose ReadEntityWorldPose(World& world, Entity e, bool physicsPose = false);

// The key of a character in Impl::kccTable: its entity and that entity's generation, so a recycled id never finds
// the character of the entity that held it before.
inline std::uint64_t KccKey(Entity entity, std::uint32_t generation)
{
    return (static_cast<std::uint64_t>(generation) << 32) | static_cast<std::uint64_t>(entity);
}
inline Entity KccKeyEntity(std::uint64_t key) { return static_cast<Entity>(key & 0xffffffffu); }
inline std::uint32_t KccKeyGeneration(std::uint64_t key) { return static_cast<std::uint32_t>(key >> 32); }

// Harness switches that put an older behaviour back, for a pack's reverse arm; false in Retail.
// -physics_sync_contacts: PostAllSteps dispatches the contact events to Lua itself.
bool SyncContactDispatchLegacy();
// -kcc_legacy: the character controller feels as it did before PH1-3 (an airborne jump is refused, velocity is the
// commanded one, every character steps up 0.4 m, pushStrength / mass / the ground's velocity do nothing, a
// MarkDirty or a capsule edit rebuilds the CharacterVirtual from the interpolated pose).
bool KccLegacy();
// -physics_kinematic_teleport: a Kinematic body is teleported once a frame to the previous frame's GlobalTransform,
// not driven with MoveKinematic.
bool KinematicTeleportLegacy();

// A body's user data names the entity that owns it and that entity's generation when the body was built: the
// id alone is recycled, so a query result for a body whose entity died would otherwise name a stranger.
inline JPH::uint64 PackBodyUser(Entity entity, std::uint32_t generation)
{
    return (static_cast<JPH::uint64>(generation) << 32) | static_cast<JPH::uint64>(entity);
}
inline Entity BodyUserEntity(JPH::uint64 user) { return static_cast<Entity>(user & 0xffffffffu); }
inline std::uint32_t BodyUserGeneration(JPH::uint64 user) { return static_cast<std::uint32_t>(user >> 32); }

// The melee overlap behind OverlapCapsuleAll, OverlapBoxAll and SweepColliderAll: every body @p shape (at the
// centre-of-mass transform @p com) really overlaps, one row per entity, deepest contact kept.
std::size_t CollectMeleeOverlaps(JPH::PhysicsSystem& physics,
    JPH::ShapeRefC shape, const JPH::RMat44& com, std::vector<OverlapHit>& out,
    const QueryFilter& filter, std::size_t maxResults);
std::size_t CollectMeleeOverlaps(JPH::PhysicsSystem& physics,
    JPH::ShapeRefC shape, const JPH::RMat44& com, std::vector<OverlapHit>& out,
    LayerMask layerMask, std::size_t maxResults);

} // namespace DX12Physics

