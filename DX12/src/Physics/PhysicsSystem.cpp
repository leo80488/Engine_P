// PhysicsSystem implementation. All Jolt types are confined to this translation unit; the public
// header exposes only a pImpl handle.
//
// Object layers follow the gameplay split of docs/design/Collision_Architecture_Reference.md:
// STATIC_ENV / DYNAMIC_PROP / CHARACTER / RAGDOLL / HURT_BOX / ATTACK_BOX / PROJECTILE / SENSING.
// They fold into three broadphase layers (STATIC, MOVING, HURTBOX) so the broadphase trees stay
// cheap; whether a pair collides is decided by the ObjectLayerPairFilter at narrow-phase time.

#include "Physics/PhysicsSystem.h"
#include "Diagnostics/ProfilerMemory.h"   // the Jolt step-arena gauge
#include "Physics/PhysicsEvents.h"
#include "Physics/ContactTracker.h"
#include "PhysicsShapes.h"
#include "PhysicsInternal.h"

#include "ECS/ECS.h"
#include "ECS/HierarchyComponents.h"
#include "ECS/PhysicsComponents.h"
#include "ECS/CharacterControllerComponent.h"
#include "Scene/TransformSystem.h"   // PropagateSubtree: push the interpolated pose to child meshes
#include "Resource/AssetFS.h"      // mesh-collider source decode (read .meshlib / .imsh blobs)
#include "Resource/AssetHeader.h"
#include "Resource/MeshLibIdentity.h"   // a prim's library entry by SectionKey (MeshLibKeyTableAt)
#include "Resource/ModelCollision.h"    // a Mesh's asset collision (BO / PR records)
#include "ECS/StaticMeshComponents.h"   // a MeshAsset collider with no model uses the entity's StaticMesh
#include "System/Log.h"
#include "System/Check.h"          // ENGINE_CHECK_ONCE: the step-arena overflow report
#include "System/EventBus.h"
#include "System/TaskSystem.h"     // parallel mesh-shape prewarm
#include "System/TaskGraph.h"      // TaskGraphAccess: Jolt jobs on the engine pool
#include "System/CommandLine.h"
#include "System/PathUtil.h"       // UTF-8-aware fs::path construction for non-ASCII asset paths

#include <Jolt/Jolt.h>

JPH_SUPPRESS_WARNINGS

#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>   // a Mesh's asset collision
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Constraints/ContactConstraintManager.h>  // cMaxConstraintSize: sizes the step arena
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/EActivation.h>
#include <Jolt/Physics/Body/AllowedDOFs.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/CollideShape.h>   // KCC-vs-sensor overlap query
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollector.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>   // ClosestHitCollisionCollector<>: shape-cast closest-hit template
#include <Jolt/Physics/Collision/TransformedShape.h>         // leaf decomposition for AppendColliderTriangles
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseQuery.h>  // OverlapSphere
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>
#include <tuple>
#include <map>            // PrewarmMeshShapes groups by (path, shape kind)
#include <unordered_map>
#include <unordered_set>
#include <vector>

// The KCC diagnostics' level. 1: edge events and WARNINGs (a missing CharacterVirtual, ground
// transitions, a rejected jump, a fall out of the world), which fire only when something happens;
// 2: adds the per-character per-step snapshots, O(characters) per fire - opt in when debugging one
// actor. Off in Retail; harness builds run tier 1, whose "jump REJECTED" line ft_kcc's pins read.
// Defined here, ahead of every #if that reads it: an undefined macro reads as 0 in #if.
#ifndef KCC_DEBUG
#if defined(WITH_HARNESS) && WITH_HARNESS
#define KCC_DEBUG 1
#else
#define KCC_DEBUG 0
#endif
#endif

namespace DX12Physics
{
// ==== Jolt trace / assert sinks: Jolt's diagnostics go to the Logger ====
static void JoltTrace(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    LOG_INFO("[Jolt] %s", buf);
}

#ifdef JPH_ENABLE_ASSERTS
static bool JoltAssertFailed(const char* expr, const char* msg, const char* file, JPH::uint line)
{
    LOG_ERROR("[Jolt assert] %s:%u: (%s) %s", file, (unsigned)line, expr, msg ? msg : "");
    return true; // trigger breakpoint
}
#endif

// ==== Harness switches that put an older behaviour back (false in Retail) ====
bool SyncContactDispatchLegacy()
{
#if WITH_HARNESS
    // -physics_sync_contacts: PostAllSteps dispatches ContactBegan / ContactEnded to Lua itself, inside the
    // physics phase, instead of leaving them to EventDelivery's drain points. The contact_order pack's
    // reverse arm.
    static const bool s_on = Sys::CommandLine::Get().Has("-physics_sync_contacts");
    return s_on;
#else
    return false;
#endif
}

bool KccLegacy()
{
#if WITH_HARNESS
    // -kcc_legacy: the character controller as it was before PH1-3 (see PhysicsInternal.h). The reverse arm
    // of ft_kcc's flipped cells and the rider arm of kcc_platform.
    static const bool s_on = Sys::CommandLine::Get().Has("-kcc_legacy");
    return s_on;
#else
    return false;
#endif
}

// An entity's character in Impl::kccTable: keyed with the entity's generation, so a recycled id never finds the
// character of the entity that held it before (-kcc_legacy keys by id alone, as the table used to).
static std::uint64_t CharacterKeyOf(const World& world, Entity e)
{
    return KccKey(e, KccLegacy() ? 0u : world.GetGeneration(e));
}

// Whether a table key still names the live entity that holds its id.
static bool CharacterKeyLive(const World& world, std::uint64_t key)
{
    const Entity e = KccKeyEntity(key);
    return world.IsAlive(e) && (KccLegacy() || world.GetGeneration(e) == KccKeyGeneration(key));
}

// Jolt's own default for CharacterVirtualSettings::mMaxStrength (newtons): pushStrength 1 keeps it.
constexpr float kKccPushForce = 100.0f;

// The velocity a character really had over a step: its displacement over the step. Where that equals the commanded
// velocity to the resolution of the positions (a free move: the positions are floats), the commanded value is the
// exact answer and is kept; a blocked axis reads what it moved. A sideways or upward move beyond the commanded
// one is not the character's speed (it was pushed out of an overlap): that stays the commanded value, or a spawn
// that overlaps a wall would leave as a launch. Downward it is the floor being followed and counts. While on the
// ground before and after, the vertical part is the ground's: a stair step or a slope is not a vertical speed.
static JPH::Vec3 ActualVelocity(JPH::Vec3Arg pre, JPH::Vec3Arg post, JPH::Vec3Arg commanded, bool grounded,
                                JPH::Vec3Arg ground, float dt)
{
    const JPH::Vec3 moved = (post - pre) / dt;
    const JPH::Vec3 resolution = JPH::Vec3::sMax(pre.Abs(), post.Abs()) * (16.0f * FLT_EPSILON / dt);
    JPH::Vec3 out = commanded;
    for (int axis = 0; axis < 3; ++axis)
    {
        if (std::fabs(moved[axis] - commanded[axis]) <= resolution[axis]) continue;
        const bool followedFloor = axis == 1 && moved[axis] < commanded[axis];
        if (followedFloor || std::fabs(moved[axis]) < std::fabs(commanded[axis]))
            out.SetComponent(axis, moved[axis]);
    }
    if (grounded) out.SetY(ground.GetY());
    return out;
}

// ==== TaskSystemJoltJobs: Jolt's job system on the engine's TaskSystem workers ====
// Every Jolt job is a High task on the engine pool, profiled under the Jolt job's name
// ("Integrate", "FindCollisions", ...), so the step's fan-out shows on the Diag::Prof timeline and
// there is one pool to schedule; Jolt's own JobSystemThreadPool would add hardware_concurrency-1
// threads the profiler cannot see.
//
// Semantics are JobSystemThreadPool's minus the threads: a job with no dependencies is queued on
// creation; Job::Execute's state CAS runs a job exactly once even when a worker and the barrier's
// waiting thread both pick it up; every Jolt step job is also in the step's barrier, so the thread
// inside WaitForJobs (main) can finish the whole step alone - a busy pool cannot deadlock it.
// -joltpool uses Jolt's JobSystemThreadPool instead.
class TaskSystemJoltJobs final : public JPH::JobSystemWithBarrier
{
public:
    TaskSystemJoltJobs(JPH::uint inMaxJobs, JPH::uint inMaxBarriers)
    {
        JobSystemWithBarrier::Init(inMaxBarriers);
        mJobs.Init(inMaxJobs, inMaxJobs);
    }
    int GetMaxConcurrency() const override
    {
        return static_cast<int>(TaskSystem::Get().GetWorkerCount()) + 1;
    }
    JPH::JobHandle CreateJob(const char* inName, JPH::ColorArg inColor,
                             const JobFunction& inJobFunction, JPH::uint32 inNumDependencies) override
    {
        JPH::uint32 index;
        for (;;)
        {
            index = mJobs.ConstructObject(inName, inColor, this, inJobFunction, inNumDependencies);
            if (index != AvailableJobs::cInvalidObjectIndex) break;
            // Out of job slots (cMaxPhysicsJobs): the same back-off Jolt's pool uses.
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        NamedJob* job = &mJobs.Get(index);
        JPH::JobHandle handle(job);   // keep a reference: the job may complete right away
        if (inNumDependencies == 0) QueueJob(job);
        return handle;
    }
    // The thread inside the barrier (main, in JPH::PhysicsSystem::Update) runs barrier jobs itself
    // and otherwise blocks: the blocking part is a join wait, and the jobs it runs open their own named
    // scopes inside it.
    void WaitForJobs(Barrier* inBarrier) override
    {
        PROF_WAIT(WaitJoin, "Jolt.WaitForJobs");
        JobSystemWithBarrier::WaitForJobs(inBarrier);
    }

protected:
    void QueueJob(Job* inJob) override
    {
        // A closed pool (shutdown) queues nothing: the barrier's waiting thread
        // executes the job, exactly as Jolt's pool does with zero threads.
        if (!Sys::TaskGraphAccess::Acquire()) return;
        inJob->AddRef();
        try
        {
            Sys::TaskGraphAccess::Enqueue([inJob]
            {
                {
                    PROF_SCOPE_DYN(static_cast<NamedJob*>(inJob)->name);
                    inJob->Execute();
                }
                inJob->Release();
            });
        }
        catch (...)
        {
            inJob->Release();
            Sys::TaskGraphAccess::Release();
            throw;
        }
        Sys::TaskGraphAccess::Release();
    }
    void QueueJobs(Job** inJobs, JPH::uint inNumJobs) override
    {
        for (JPH::uint i = 0; i < inNumJobs; ++i) QueueJob(inJobs[i]);
    }
    void FreeJob(Job* inJob) override { mJobs.DestructObject(static_cast<NamedJob*>(inJob)); }

private:
    // Job::GetName exists only in Jolt profile builds, so the name rides here.
    struct NamedJob : Job
    {
        const char* name;

        NamedJob(const char* inName, JPH::ColorArg inColor, JPH::JobSystem* inSystem,
                 const JobFunction& inFunction, JPH::uint32 inDependencies)
            : Job(inName, inColor, inSystem, inFunction, inDependencies), name(inName) {}
    };
    using AvailableJobs = JPH::FixedSizeFreeList<NamedJob>;
    AvailableJobs mJobs;
};

// Translates the authors' inverted "locked" bitmask into Jolt's "allowed" enum: an AxisLock bit
// ON -> that EAllowedDOFs bit OFF. lockedAxes == 0 gives EAllowedDOFs::All (full 6-DOF), Jolt's
// default, so bodies without locks keep their mass-properties layout.
static JPH::EAllowedDOFs ToJoltAllowedDOFs(std::uint8_t lockedAxes)
{
    using JPH::EAllowedDOFs;
    std::uint8_t dof = static_cast<std::uint8_t>(EAllowedDOFs::All);
    if (lockedAxes & RigidBodyComponent::LockTranslationX) dof &= ~static_cast<std::uint8_t>(EAllowedDOFs::TranslationX);
    if (lockedAxes & RigidBodyComponent::LockTranslationY) dof &= ~static_cast<std::uint8_t>(EAllowedDOFs::TranslationY);
    if (lockedAxes & RigidBodyComponent::LockTranslationZ) dof &= ~static_cast<std::uint8_t>(EAllowedDOFs::TranslationZ);
    if (lockedAxes & RigidBodyComponent::LockRotationX)    dof &= ~static_cast<std::uint8_t>(EAllowedDOFs::RotationX);
    if (lockedAxes & RigidBodyComponent::LockRotationY)    dof &= ~static_cast<std::uint8_t>(EAllowedDOFs::RotationY);
    if (lockedAxes & RigidBodyComponent::LockRotationZ)    dof &= ~static_cast<std::uint8_t>(EAllowedDOFs::RotationZ);
    return static_cast<EAllowedDOFs>(dof);
}

// Collider fingerprint: every ColliderComponent field baked into the Jolt body at creation
// (MakeShape's inputs, the sensor flag, the layer), never runtime-only state such as `generation`.
// Phase 0.6 rebuilds the body when it drifts from RigidBodyComponent::lastBuiltShapeHash /
// lastBuiltPathHash, which catches Inspector reflection writes that never call MarkDirty(). A hash
// on the component beats a full snapshot in an entity-keyed map (a lookup and a ~100-byte cache
// miss per body per frame).
static std::uint64_t HashMix(std::uint64_t h, std::uint64_t v)
{
    h ^= v;
    return h * 1099511628211ull;                       // FNV-1a 64 prime
}
static std::uint64_t HashBytes(std::uint64_t h, const void* data, std::size_t n)
{
    const unsigned char* p = static_cast<const unsigned char*>(data);
    std::uint64_t block;
    while (n >= sizeof block)
    {
        std::memcpy(&block, p, sizeof block);
        h = HashMix(h, block);
        p += sizeof block;
        n -= sizeof block;
    }
    if (n)
    {
        block = 0;
        std::memcpy(&block, p, n);
        h = HashMix(h, block);
    }
    return h;
}
// The fingerprint is split in two. The path content hash reads c.meshLibPath.data(), an
// out-of-line heap buffer: on an imported level every body is a Mesh / ConvexHull collider, and
// hashing it per body per frame cost ~0.4 ms of scattered cache misses (~60% of the physics
// pre-step), while the inline fields stream in microseconds.
//
// So ColliderShapeHashInline covers everything reachable without leaving the component
// (meshLibPath.size() included: it is in the SSO header) and runs every frame for every body.
// ColliderPathHash covers the path bytes and runs on a sliced subset (kPathHashSlices in Phase
// 0.6) plus at body creation. A same-length path edit is detected within kPathHashSlices frames;
// every field the Inspector can drag is detected on the next frame.
static std::uint64_t ColliderShapeHashInline(const ColliderComponent& c)
{
    std::uint64_t h = 1469598103934665603ull;          // FNV-1a 64 offset basis
    h = HashMix(h, static_cast<std::uint64_t>(c.shape));
    // The sensor flag (BodyCreationSettings::mIsSensor) and the layer (the Jolt object layer) are
    // baked at body creation, so an Inspector toggle must rebuild the body like a shape edit.
    h = HashMix(h, c.isTrigger ? 1ull : 0ull);
    h = HashMix(h, static_cast<std::uint64_t>(c.layer));
    h = HashBytes(h, &c.offset,           sizeof c.offset);
    h = HashBytes(h, &c.rotationEulerDeg, sizeof c.rotationEulerDeg);
    switch (c.shape)
    {
        case ColliderComponent::Shape::Box:
            h = HashBytes(h, &c.halfExtents, sizeof c.halfExtents);
            break;
        case ColliderComponent::Shape::Sphere:
            h = HashBytes(h, &c.radius, sizeof c.radius);
            break;
        case ColliderComponent::Shape::Capsule:
            h = HashBytes(h, &c.radius,     sizeof c.radius);
            h = HashBytes(h, &c.halfHeight, sizeof c.halfHeight);
            break;
        case ColliderComponent::Shape::Mesh:
        case ColliderComponent::Shape::ConvexHull:
            h = HashMix(h, c.meshLibMeshId);
            // Length only, no heap dereference: catches the common path edits (another mesh, another folder)
            // at once; same-length edits fall to the sliced content hash (ColliderPathHash).
            h = HashMix(h, static_cast<std::uint64_t>(c.meshLibPath.size()));
            break;
        case ColliderComponent::Shape::MeshAsset:
            h = HashMix(h, c.assetMeshKey);
            h = HashMix(h, static_cast<std::uint64_t>(c.assetModel.size()));
            break;
    }
    return h;
}

// Content of the collider's source path (meshLibPath, or assetModel for a MeshAsset). Other
// colliders hash to the basis and never trip the sliced compare.
static std::uint64_t ColliderPathHash(const ColliderComponent& c)
{
    std::uint64_t h = 1469598103934665603ull;
    if (c.shape == ColliderComponent::Shape::Mesh ||
        c.shape == ColliderComponent::Shape::ConvexHull)
        h = HashBytes(h, c.meshLibPath.data(), c.meshLibPath.size());
    else if (c.shape == ColliderComponent::Shape::MeshAsset)
        h = HashBytes(h, c.assetModel.data(), c.assetModel.size());
    return h;
}

PhysicsSystem::PhysicsSystem()  = default;

PhysicsSystem::~PhysicsSystem() { Shutdown(); }

// ==== Init / Shutdown ====
void PhysicsSystem::Init()
{
    if (m_impl) return;

    JPH::RegisterDefaultAllocator();

    // The Jolt library was built with the feature defines this code was (debug renderer, profiler,
    // floating-point exceptions, precision): a mismatch changes struct layouts under the same names.
    ENGINE_CHECK(JPH::VerifyJoltVersionID(),
                 "PhysicsSystem: the linked Jolt library was built with other feature defines than this "
                 "configuration (Retail links tools/build_jolt.py's Jolt_Retail.lib)");

    JPH::Trace = JoltTrace;
    JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = JoltAssertFailed;)

    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();

    m_impl = std::make_unique<Impl>();

    // Per-step scratch arena, sized from Jolt's own constants (Impl::kJoltStepArenaBytes,
    // JoltStepAllocator).
    m_impl->tempAlloc = std::make_unique<JoltStepAllocator>(Impl::kJoltStepArenaBytes);

    // Jolt's jobs run on the engine TaskSystem (TaskSystemJoltJobs); -joltpool uses Jolt's own
    // JobSystemThreadPool instead.
    if (Sys::CommandLine::Get().Has("-joltpool"))
    {
        const int workerCount = std::max(1, (int)std::thread::hardware_concurrency() - 1);
        m_impl->jobSys = std::make_unique<JPH::JobSystemThreadPool>(
            JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, workerCount);
    }
    else
        m_impl->jobSys = std::make_unique<TaskSystemJoltJobs>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers);
    LOG_INFO("[PHYSICS] job system=%s concurrency=%d",
             Sys::CommandLine::Get().Has("-joltpool") ? "jolt-pool" : "tasksystem",
             m_impl->jobSys->GetMaxConcurrency());

    m_impl->bpLayerIface       = std::make_unique<BPLayerInterfaceImpl>();
    m_impl->objVsBpFilter      = std::make_unique<ObjectVsBPFilterImpl>();
    m_impl->objLayerPairFilter = std::make_unique<ObjectLayerPairFilterImpl>();

    m_impl->physics = std::make_unique<JPH::PhysicsSystem>();
    // kMaxBodyPairs / kMaxContacts feed ContactConstraintManager::Init, which mallocs two manifold
    // caches of
    //   kMaxBodyPairs * sizeof(BPKeyValue) + kMaxContacts * manifoldEntry
    // ~10 MiB in total. Do not lower them without a measured peak from the heaviest scene: both are
    // Jolt's documented values for a real project, the cost is a real cache, and an overflow does not
    // fail loudly - the allocator stops handing out entries and Update() raises an EPhysicsUpdateError
    // bit, so a too-small cap shows as contacts losing warm-start (jitter, sinking objects).
    m_impl->physics->Init(
        Impl::kMaxBodies, Impl::kNumBodyMutexes,
        Impl::kMaxBodyPairs, Impl::kMaxContacts,
        *m_impl->bpLayerIface, *m_impl->objVsBpFilter, *m_impl->objLayerPairFilter);

    bool legacyContacts = false;
#if WITH_HARNESS
    // -physics_legacy_contacts: one ContactBegan / ContactEnded per Jolt sub-shape contact in callback
    // order, a body's sleep ends its contacts, nothing is synthesized for a destroyed body. The
    // behaviour before ContactTracker, kept so a pack can prove the tracker changes what it claims.
    legacyContacts = Sys::CommandLine::Get().Has("-physics_legacy_contacts");
#endif
    m_impl->contactTracker = std::make_unique<ContactTracker>(legacyContacts);
    m_impl->physics->SetContactListener(m_impl->contactTracker->Listener());

    // KCC ExtendedUpdateSettings: stair walking with small forward / up offsets (as in Jolt's
    // CharacterVirtualTest) and stick-to-floor, so downhill walks do not micro-bounce. Every character
    // shares them except the step-up, which RunKccUpdates sets from CharacterControllerComponent::stepHeight
    // (this 0.4 m is what -kcc_legacy uses for all).
    m_impl->kccExtUpdate.mStickToFloorStepDown   = JPH::Vec3(0, -0.5f, 0);
    m_impl->kccExtUpdate.mWalkStairsStepUp       = JPH::Vec3(0, 0.4f, 0);
    m_impl->kccExtUpdate.mWalkStairsMinStepForward = 0.02f;
    m_impl->kccExtUpdate.mWalkStairsStepForwardTest = 0.15f;

    LOG_INFO("PhysicsSystem: initialized (Jolt %d.%d.%d, %d-way job concurrency, "
             "step arena %zu KiB = %u constraints x %u B + %zu KiB slack)",
             JPH_VERSION_MAJOR, JPH_VERSION_MINOR, JPH_VERSION_PATCH, m_impl->jobSys->GetMaxConcurrency(),
             Impl::kJoltStepArenaBytes / 1024,
             static_cast<unsigned>(Impl::kMaxContacts),
             static_cast<unsigned>(JPH::ContactConstraintManager::cMaxConstraintSize
                                   + sizeof(JPH::uint32)),
             Impl::kJoltStepSlackBytes / 1024);
}

const std::vector<std::uint8_t>* PhysicsSystem::EnsureCollisionBlob(const std::string& path)
{
    if (!m_impl || path.empty()) return nullptr;
    auto it = m_impl->blobCache.find(path);
    if (it != m_impl->blobCache.end()) return &it->second;

    std::vector<uint8_t> blob;
    if (!Resource::AssetFS::Get().ReadFile(path, blob) || blob.empty())
    {
        LOG_WARNING("PhysicsSystem::EnsureCollisionBlob — cannot read '%s'", path.c_str());
        return nullptr;
    }
    auto [ins, _] = m_impl->blobCache.emplace(path, std::move(blob));
    return &ins->second;
}

std::uint32_t PhysicsSystem::GetBodyCount() const
{
    if (!m_impl || !m_impl->physics) return 0;
    return static_cast<std::uint32_t>(m_impl->physics->GetNumBodies());
}

std::uint32_t PhysicsSystem::GetActiveBodyCount() const
{
    if (!m_impl || !m_impl->physics) return 0;
    return static_cast<std::uint32_t>(m_impl->physics->GetNumActiveBodies(JPH::EBodyType::RigidBody));
}

void PhysicsSystem::ApplyWorldSettings(float gravity, float timeBeforeSleep)
{
    if (!m_impl || !m_impl->physics) return;
    m_impl->physics->SetGravity(JPH::Vec3(0.f, gravity, 0.f));
    JPH::PhysicsSettings settings = m_impl->physics->GetPhysicsSettings();
    settings.mTimeBeforeSleep = timeBeforeSleep;
    m_impl->physics->SetPhysicsSettings(settings);
    LOG_INFO("[PHYSICS] gravity=%.3f timeBeforeSleep=%.3f", gravity, timeBeforeSleep);
}

#ifdef JPH_DEBUG_RENDERER
PhysicsSystem::DebugTarget PhysicsSystem::GetDebugTarget()
{
    DebugTarget target;
    if (!m_impl || !m_impl->physics) return target;
    target.physics = m_impl->physics.get();
    target.characters.reserve(m_impl->kccTable.size());
    for (const auto& kv : m_impl->kccTable)
        if (kv.second) target.characters.push_back(kv.second.GetPtr());
    target.renderer = &m_impl->debugDraw;
    return target;
}
#endif

bool PhysicsSystem::IsSleeping(Entity entity) const
{
    if (!m_impl || !m_impl->physics) return false;
    const auto it = m_impl->bodyIds.find(entity);
    if (it == m_impl->bodyIds.end()) return false;
    const JPH::BodyID id(it->second);
    const JPH::BodyInterface& bodyIface = m_impl->physics->GetBodyInterface();
    return bodyIface.IsAdded(id) && bodyIface.GetMotionType(id) == JPH::EMotionType::Dynamic &&
           !bodyIface.IsActive(id);
}

void PhysicsSystem::InvalidateMeshShapeCache(World& world)
{
    if (!m_impl) return;
    const size_t nShapes = m_impl->meshShapeCache.size();
    const size_t nBlobs  = m_impl->blobCache.size();
    m_impl->meshShapeCache.clear();
    ++m_impl->shapeCacheEpoch;   // invalidates every collider's prewarm memo
    m_impl->blobCache.clear();   // file contents may have changed on re-bake
    m_impl->blobStamp.clear();   // re-stat the sources on the next key build

    // Bump the generation of every Mesh / ConvexHull / MeshAsset collider so the runtime-swap
    // detector rebuilds its body next tick; otherwise a body built from a cleared entry keeps its
    // stale shape and diverges from the re-baked geometry.
    size_t nBumped = 0;
    if (auto* colPool = world.GetPool<ColliderComponent>())
    {
        auto& colData = colPool->Data();
        for (auto& col : colData)
        {
            // An asset collision's shape is built from its model's records (and their libraries), so a
            // re-bake or a model edit stales it too.
            if (col.shape != ColliderComponent::Shape::Mesh &&
                col.shape != ColliderComponent::Shape::ConvexHull &&
                col.shape != ColliderComponent::Shape::MeshAsset) continue;
            ++col.generation;
            ++nBumped;
        }
    }
    if (nShapes + nBlobs + nBumped > 0)
        LOG_INFO("PhysicsSystem: cleared %zu mesh shapes + %zu blob(s); "
                 "bumped %zu Mesh-collider generations for rebuild",
                 nShapes, nBlobs, nBumped);
}

// See the declaration. Called before the World is cleared (scene activation, editor loads,
// Play -> Stop), on the main thread outside any physics step, so RemoveBody / DestroyBody are safe
// here, as in the Phase 0.5 reaper.
void PhysicsSystem::OnWorldClear(bool dropShapeCache)
{
    if (!m_impl) return;

    JPH::BodyInterface& bodyIface = m_impl->physics->GetBodyInterface();
    for (auto& kv : m_impl->bodyIds)
    {
        const JPH::BodyID id(kv.second);
        if (!id.IsInvalid())
        {
            bodyIface.RemoveBody(id);
            bodyIface.DestroyBody(id);
        }
    }
    const size_t nBodies = m_impl->bodyIds.size();
    m_impl->bodyIds.clear();
    m_impl->poseSnapshots.clear();
    m_impl->shapeFailed.clear();
    // The shape hash, pushed pose and motion mirror live on RigidBodyComponent, which the World tears
    // down with everything else: nothing to clear here.
    if (dropShapeCache)
    {
        m_impl->meshShapeCache.clear();
    }
    else
    {
        // Drop the negative entries (a null ShapeRefC recorded when a read / decode / hull build failed).
        // Kept, a transient failure (an AV scanner holding the .meshlib open, a degenerate entry) would
        // stick for the session: the key is unchanged (same mtime), the prewarm scan and GetOrBuild* would
        // count it as cached, and MakeShape would stand a 0.5 m box in on every later Play without
        // repeating the error.
        for (auto it = m_impl->meshShapeCache.begin(); it != m_impl->meshShapeCache.end(); )
        {
            if (!it->second) it = m_impl->meshShapeCache.erase(it);
            else             ++it;
        }
    }
    // blobCache is dropped either way: it must share a lifetime with blobStamp. It is keyed by path
    // alone, so keeping it while the stamps are re-read would let a freshly stamped key
    // ('path@T2#id', a miss) be built from the pre-edit bytes still under 'path' and stored under the
    // post-edit key - the stale geometry the stamp exists to prevent. Costs one re-read per unique
    // .meshlib; PrewarmMeshShapes reads its own copy anyway, and EnsureCollisionBlob reloads a path
    // that is missing.
    m_impl->blobCache.clear();

    // The epoch bump and the stamp drop also happen either way. Keeping the shapes is the win, but
    // the per-collider prewarm memos must be invalidated (AdoptState restores ColliderComponents from
    // the EnterPlay clone, whose memos are stale), and the stamps re-read so a source edited during
    // Play becomes a fresh key on the next build.
    ++m_impl->shapeCacheEpoch;
    m_impl->blobStamp.clear();

    // KCC CharacterVirtuals: dropping the Ref destroys them. Safe while `physics` is alive (the same
    // order as Shutdown).
    const size_t nKcc = m_impl->kccTable.size();
    m_impl->kccTable.clear();
    m_impl->kccShapeSnaps.clear();

    // Drop queued contacts and the tracked pairs that reference destroyed bodies, so the next
    // PostAllSteps does not dispatch them against recycled IDs.
    if (m_impl->contactTracker) m_impl->contactTracker->Clear();
    m_impl->contactScratch.clear();
    // Same for KCC-vs-sensor overlap state: recycled entity IDs in the next
    // scene must not inherit "already inside the trigger".
    m_impl->kccSensorTouches.clear();

    LOG_INFO("PhysicsSystem: OnWorldClear — destroyed %zu body(ies) + %zu KCC(s)",
             nBodies, nKcc);
}

void PhysicsSystem::Shutdown()
{
    if (!m_impl) return;

    // CharacterVirtuals hold a raw pointer to JPH::PhysicsSystem and may touch BodyInterface when
    // destroyed (inner-body removal): drop them before physics.reset(), or it is a use-after-free.
    m_impl->kccTable.clear();
    m_impl->kccShapeSnaps.clear();
#ifdef JPH_DEBUG_RENDERER
    m_impl->debugDraw.reset();
#endif

    // Reverse order: bodies/broadphase live inside JPH::PhysicsSystem, so dropping
    // `physics` first lets its dtor unhook from the layer interfaces cleanly.
    m_impl->physics.reset();
    m_impl->contactTracker.reset();
    m_impl->objLayerPairFilter.reset();
    m_impl->objVsBpFilter.reset();
    m_impl->bpLayerIface.reset();
    m_impl->jobSys.reset();
    m_impl->tempAlloc.reset();
    m_impl.reset();

    JPH::UnregisterTypes();
    if (JPH::Factory::sInstance)
    {
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }

    LOG_INFO("PhysicsSystem: shutdown");
}

// ==== Conversion helpers ====
static JPH::EMotionType ToJolt(RigidBodyComponent::Motion m)
{
    switch (m)
    {
        case RigidBodyComponent::Motion::Static:    return JPH::EMotionType::Static;
        case RigidBodyComponent::Motion::Kinematic: return JPH::EMotionType::Kinematic;
        case RigidBodyComponent::Motion::Dynamic:   return JPH::EMotionType::Dynamic;
    }
    return JPH::EMotionType::Dynamic;
}

static bool IsIdentityMatrix(const DirectX::XMFLOAT4X4& m)
{
    static const DirectX::XMFLOAT4X4 kId(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1);
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            if (std::fabs(m.m[r][c] - kId.m[r][c]) > 1e-6f) return false;
    return true;
}

// Composes the entity's world matrix from the LocalTransform chain (row vectors: self * parent *
// grandparent * ...), for when GlobalTransform cannot be trusted (ReadEntityWorldPose).
static DirectX::XMMATRIX ComposeWorldFromLocalChain(World& world, Entity e)
{
    using namespace DirectX;
    XMMATRIX M = XMMatrixIdentity();
    Entity cur = e;
    for (int depth = 0; depth < 64 && cur != NullEntity; ++depth)
    {
        if (const LocalTransform* lt = world.GetComponent<LocalTransform>(cur))
            M = XMMatrixMultiply(M, lt->ToMatrix());
        const Parent* p = world.GetComponent<Parent>(cur);
        cur = p ? p->entity : NullEntity;
    }
    return M;
}

// World-space pose for body placement. Scene-graph mesh entities have an identity LocalTransform
// and inherit their pose from a parent node, so LocalTransform alone would put every static body
// at the origin: this decomposes GlobalTransform.matrix (kept fresh by TransformSystem::Propagate)
// instead, falling back to the LocalTransform chain.
// @p physicsPose skips GlobalTransform altogether: a DYNAMIC body's GlobalTransform is the
// render-interpolated pose (ApplyRenderInterpolation, up to a step behind), its LocalTransform what
// Jolt last simulated (PostAllSteps' Phase 4 write-back).
WorldPose ReadEntityWorldPose(World& world, Entity e, bool physicsPose)
{
    WorldPose out;
    out.pos   = JPH::Vec3::sZero();
    out.rot   = JPH::Quat::sIdentity();
    out.scale = { 1.f, 1.f, 1.f };

    using namespace DirectX;

    // Prefer the propagated GlobalTransform, except while it is still the default identity and the
    // LocalTransform chain is not: the entity was created (scene load / runtime spawn) and physics
    // ticked before TransformSystem::Propagate ran this frame. Trusting it would build the body at the
    // origin with unit scale (a tiny floor, and launch velocities erased by next frame's teleport
    // detection). A truly identity entity composes to identity through the chain too, so falling
    // through is lossless.
    const GlobalTransform* gt = physicsPose ? nullptr : world.GetComponent<GlobalTransform>(e);
    XMMATRIX M;
    if (gt && !IsIdentityMatrix(gt->matrix))
        M = XMLoadFloat4x4(&gt->matrix);
    else
        M = ComposeWorldFromLocalChain(world, e);

    XMVECTOR vScale, vQuat, vTrans;
    if (XMMatrixDecompose(&vScale, &vQuat, &vTrans, M))
    {
        XMFLOAT3 t, s; XMFLOAT4 q;
        XMStoreFloat3(&t, vTrans);
        XMStoreFloat4(&q, vQuat);
        XMStoreFloat3(&s, vScale);
        out.pos   = ToJolt(t);
        out.rot   = ToJoltQuat(q);
        out.scale = s;
        return out;
    }
    if (const LocalTransform* lt = world.GetComponent<LocalTransform>(e))
    {
        out.pos   = ToJolt(lt->translation);
        out.rot   = ToJoltQuat(lt->rotation);
        out.scale = lt->scale;
    }
    return out;
}

// Read the collider at the animated pose, independently of the fixed-step body.
std::size_t PhysicsSystem::SweepColliderAll(const ColliderComponent& collider,
    const DirectX::XMFLOAT4X4& previous, const DirectX::XMFLOAT4X4& current,
    std::vector<OverlapHit>& out, LayerMask layerMask, float fromFraction, float toFraction) const
{
    using namespace DirectX;
    if (!m_impl) return 0;
    if (collider.shape != ColliderComponent::Shape::Box &&
        collider.shape != ColliderComponent::Shape::Sphere &&
        collider.shape != ColliderComponent::Shape::Capsule) return 0;
    if (collider.shape == ColliderComponent::Shape::Box) {
        if (!(collider.halfExtents.x>0 && collider.halfExtents.y>0 && collider.halfExtents.z>0)) return 0;
    } else if (!(collider.radius>0) ||
               (collider.shape==ColliderComponent::Shape::Capsule && !(collider.halfHeight>0))) return 0;
    XMVECTOR scale,rotation,position,oldScale,oldRotation,oldPosition;
    if (!XMMatrixDecompose(&scale,&rotation,&position,XMLoadFloat4x4(&current)) ||
        !XMMatrixDecompose(&oldScale,&oldRotation,&oldPosition,XMLoadFloat4x4(&previous))) return 0;
    XMFLOAT3 sf; XMStoreFloat3(&sf,scale);
    if (!(sf.x>0 && sf.y>0 && sf.z>0)) return 0;
    // Same shape contract as the physics inspector and collider wireframe.
    const auto shape=MakeShape(m_impl->meshShapeCache,m_impl->blobCache,m_impl->blobStamp,collider,sf);
    if (!shape) return 0;
    const auto extent=shape->GetLocalBounds().GetExtent();
    const float spacing=(std::max)(.005f,(std::min)({extent.GetX(),extent.GetY(),extent.GetZ()})*.5f);
    const float reach=shape->GetCenterOfMass().Length()+extent.Length();
    const float angle=2.f*std::acos((std::min)(1.f,std::fabs(XMVectorGetX(XMQuaternionDot(rotation,oldRotation)))));
    const float distance=XMVectorGetX(XMVector3Length(position-oldPosition));
    // Rotation matters: a sword can cross a target while its pivot barely moves.
    const int steps=int((std::clamp)(std::ceil((distance+angle*reach)*(toFraction-fromFraction)/spacing),1.f,128.f));
    const auto before=out.size();
    for (int i=0;i<=steps;++i) {
        const float t=fromFraction+(toFraction-fromFraction)*float(i)/steps;
        XMFLOAT4 r; XMStoreFloat4(&r,XMQuaternionSlerp(oldRotation,rotation,t));
        XMFLOAT3 p; XMStoreFloat3(&p,XMVectorLerp(oldPosition,position,t));
        const JPH::Quat q(r.x,r.y,r.z,r.w);
        const auto com=JPH::RVec3(p.x,p.y,p.z)+q*shape->GetCenterOfMass();
        CollectMeleeOverlaps(*m_impl->physics,shape,JPH::RMat44::sRotationTranslation(q,com),out,layerMask,64);
    }
    return out.size()-before;
}

// ==== AppendColliderTriangles ====
// Defined here, after MakeShape, which it uses.
bool PhysicsSystem::AppendColliderTriangles(const ColliderComponent&        collider,
                                            const DirectX::XMMATRIX&        worldMatrix,
                                            std::vector<DirectX::XMFLOAT3>& outPositions,
                                            std::vector<std::uint32_t>&     outIndices)
{
    if (!m_impl) return false;
    using namespace DirectX;

    XMVECTOR s, r, t;
    if (!XMMatrixDecompose(&s, &r, &t, worldMatrix)) return false;
    XMFLOAT3 scale; XMStoreFloat3(&scale, s);
    XMFLOAT4 rotf;  XMStoreFloat4(&rotf, r);
    XMFLOAT3 posf;  XMStoreFloat3(&posf, t);

    // MakeShape folds in the collider offset / rotation and the entity scale, so only the world
    // position + rotation remain to be applied.
    JPH::ShapeRefC shape = MakeShape(m_impl->meshShapeCache, m_impl->blobCache, m_impl->blobStamp,collider, scale);
    if (!shape) return false;

    const JPH::Quat jrot(rotf.x, rotf.y, rotf.z, rotf.w);
    const JPH::Vec3 jpos(posf.x, posf.y, posf.z);
    // Jolt wants the centre-of-mass position, not the origin: for a RotatedTranslated / convex shape
    // they differ by the shape's COM.
    const JPH::Vec3 comPos = jpos + jrot * shape->GetCenterOfMass();

    // GetTrianglesStart / Next work only on leaf shapes. On the decorators MakeShape produces
    // (ScaledShape for a scaled entity, RotatedTranslatedShape for an offset / rotation) they are only
    // a JPH_ASSERT: Start builds no context and Next returns 0. JPH_ENABLE_ASSERTS is not defined in
    // any configuration here, so that failure is silent - zero triangles and a `true` return, which
    // would drop every scaled or offset collider from the navmesh. So decompose into leaves first, as
    // the assert says; that also bakes the scale / offset into each leaf's transform.
    JPH::AllHitCollisionCollector<JPH::TransformedShapeCollector> leaves;
    shape->CollectTransformedShapes(JPH::AABox::sBiggest(), comPos, jrot,
                                    JPH::Vec3::sReplicate(1.0f),
                                    JPH::SubShapeIDCreator(), leaves, {});
    if (leaves.mHits.empty()) return false;

    // Jolt hands back loose triangles (3 vertices each, nothing shared). Recast welds them, so
    // emitting them unindexed is fine.
    constexpr int kBatch = 256;   // >= cGetTrianglesMinTrianglesRequested (32)
    std::vector<JPH::Float3> verts(size_t(kBatch) * 3);
    size_t emitted = 0;
    for (const JPH::TransformedShape& ts : leaves.mHits)
    {
        JPH::Shape::GetTrianglesContext ctx;
        ts.GetTrianglesStart(ctx, JPH::AABox::sBiggest(), JPH::RVec3::sZero());
        for (;;)
        {
            const int n = ts.GetTrianglesNext(ctx, kBatch, verts.data());
            // Stop only on 0: MeshShape and ConvexHullShape return fewer than requested while geometry
            // remains (they stop the tree / face walk when the batch would overflow and resume on the next
            // call), so an `n < kBatch` break would truncate anything bigger than one batch.
            if (n <= 0) break;
            const uint32_t base = static_cast<uint32_t>(outPositions.size());
            outPositions.reserve(outPositions.size() + size_t(n) * 3);
            outIndices.reserve(outIndices.size() + size_t(n) * 3);
            for (int i = 0; i < n * 3; ++i)
            {
                outPositions.push_back(XMFLOAT3{ verts[i].x, verts[i].y, verts[i].z });
                outIndices.push_back(base + static_cast<uint32_t>(i));
            }
            emitted += size_t(n);
        }
    }
    return emitted > 0;
}

// ==== PrewarmMeshShapes ====
// Defined after the decode helpers and MeshShapeKey, which it uses.
void PhysicsSystem::PrewarmMeshShapes(const std::vector<MeshShapeRequest>& requests)
{
    if (!m_impl || requests.empty()) return;
    const auto tStart = std::chrono::steady_clock::now();

    // Group by (source path, shape kind) so each .meshlib is read once per kind. Mesh and ConvexHull
    // can be requested for the same entry and their keys differ by the "hull|" prefix, so they must
    // not share a bucket.
    std::map<std::pair<std::string, bool>, std::vector<uint32_t>> byPath;
    for (const auto& r : requests)
    {
        if (r.path.empty()) continue;
        const std::string key = MeshShapeKey(m_impl->blobStamp, r.path, r.meshId, r.convexHull);
        if (m_impl->meshShapeCache.count(key) != 0) continue; // already cached
        byPath[{ r.path, r.convexHull }].push_back(r.meshId);
    }
    // Dedupe: the cache check only rejects ids already cached, so two colliders sharing one (path,
    // meshId, kind) in the same batch would both get a full BVH build and the later emplace would
    // drop one. On a level that instances one collision mesh many times that would be the dominant
    // cost, and invisible (totalBuilt would just report more).
    for (auto& [k, ids] : byPath)
    {
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    }
    if (byPath.empty())
    {
        LOG_INFO("PhysicsSystem: PrewarmMeshShapes — all %zu requests already cached",
                 requests.size());
        return;
    }

    size_t totalBuilt = 0;
    size_t totalFailed = 0;

    for (const auto& [pathKind, meshIds] : byPath)
    {
        const std::string& path       = pathKind.first;
        const bool         convexHull = pathKind.second;
        // Read the file once per (path, kind) bucket, not once per body as the lazy path would.
        std::vector<uint8_t> blob;
        if (!Resource::AssetFS::Get().ReadFile(path, blob) || blob.empty())
        {
            // Negative-cached like GetOrBuildMeshShape's failures, so the per-frame scan does not re-read the
            // file and repeat this ERROR every frame. MakeShape stands a 0.5 m box in for each.
            LOG_ERROR("PhysicsSystem: PrewarmMeshShapes — cannot read '%s' (%zu %s collider shape(s) "
                      "get a 0.5 m stand-in box)", path.c_str(), meshIds.size(), convexHull ? "hull" : "mesh");
            for (uint32_t id : meshIds)
                m_impl->meshShapeCache.emplace(MeshShapeKey(m_impl->blobStamp, path, id, convexHull),
                                               JPH::ShapeRefC());
            totalFailed += meshIds.size();
            continue;
        }

        std::string ext;
        {
            std::filesystem::path p = Sys::U8Path(path);
            ext = Sys::PathToUtf8(p.extension());
            for (auto& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }

        // .imsh is a single-mesh format: DecodeImshPositions takes no meshId and returns the whole file,
        // so K meshIds in one .imsh bucket would build and cache the same geometry K times. Build one and
        // alias the rest: every key still carries its meshId, so every lookup resolves, to the same
        // shape. (.meshlib really slices by meshId.)
        const bool singleMeshFormat = (ext == ".imsh");
        const uint32_t N = singleMeshFormat
            ? 1u : static_cast<uint32_t>(meshIds.size());
        std::vector<JPH::ShapeRefC> built(N);

        TaskSystem::Get().ParallelFor(0, N, [&](uint32_t i)
        {
            std::vector<DirectX::XMFLOAT3> positions;
            std::vector<uint32_t>          indices;
            bool decoded = false;
            if      (ext == ".meshlib") decoded = DecodeMeshLibSlice(blob, meshIds[i], positions, indices);
            else if (ext == ".imsh")    decoded = DecodeImshPositions(blob, positions, indices);
            if (convexHull)
            {
                // A hull needs the points only and is cheap next to a MeshShape BVH, but hundreds of them in
                // parallel still beat building them serially in the first frame of body creation.
                if (!decoded || positions.size() < 4) return;
                JPH::Array<JPH::Vec3> points;
                points.reserve(positions.size());
                for (const auto& p : positions)
                    points.emplace_back(p.x, p.y, p.z);
                JPH::ConvexHullShapeSettings hullSettings(points);
                auto hullResult = hullSettings.Create();
                if (hullResult.HasError()) return;
                built[i] = hullResult.Get();
                return;
            }

            if (!decoded || positions.empty() || indices.size() < 3 || indices.size() % 3 != 0)
                return;

            JPH::VertexList          verts;
            JPH::IndexedTriangleList tris;
            verts.reserve(positions.size());
            for (const auto& p : positions)
                verts.emplace_back(p.x, p.y, p.z);
            tris.reserve(indices.size() / 3);
            for (size_t j = 0; j < indices.size(); j += 3)
            {
                if (indices[j] == indices[j+1] || indices[j+1] == indices[j+2] || indices[j] == indices[j+2])
                    continue;
                tris.emplace_back(indices[j], indices[j+1], indices[j+2], 0u);
            }
            if (tris.empty()) return;

            JPH::MeshShapeSettings settings(std::move(verts), std::move(tris));
            auto result = settings.Create();
            if (result.HasError()) return;
            built[i] = result.Get();
        }, TaskSystem::TaskPriority::High);

        // Bulk insert on the main thread: no mutex needed.
        for (size_t k = 0; k < meshIds.size(); ++k)
        {
            // singleMeshFormat: every id aliases the one built shape.
            const uint32_t src = singleMeshFormat ? 0u : static_cast<uint32_t>(k);
            std::string key = MeshShapeKey(m_impl->blobStamp, path, meshIds[k], convexHull);
            if (!built[src])
            {
                // Logged once and negative-cached, like the read failure.
                LOG_ERROR("PhysicsSystem: %s collider source '%s' entry %u does not build - a 0.5 m "
                          "stand-in box collides instead", convexHull ? "convex" : "mesh", path.c_str(),
                          meshIds[k]);
                m_impl->meshShapeCache.emplace(std::move(key), JPH::ShapeRefC());
                ++totalFailed;
                continue;
            }
            m_impl->meshShapeCache.emplace(std::move(key), built[src]);
            ++totalBuilt;
        }
    }

    const auto tEnd  = std::chrono::steady_clock::now();
    const float ms   = std::chrono::duration<float, std::milli>(tEnd - tStart).count();
    LOG_INFO("PhysicsSystem: PrewarmMeshShapes built %zu shapes (%zu failed) in %.1f ms",
             totalBuilt, totalFailed, ms);
}

// Allocation-free identity stamp for the per-frame mesh-shape prewarm scan. Mixes only integers
// (never the path characters), so the steady-state scan is a few ALU ops per collider. Never
// returns 0: 0 is the "not yet confirmed cached" default of a fresh ColliderComponent.
static std::uint64_t ShapePrewarmMemo(const ColliderComponent& c, std::uint32_t cacheEpoch)
{
    std::uint64_t h = 1469598103934665603ull;          // FNV-1a 64 offset basis
    const auto mix = [&h](std::uint64_t v)
    {
        h ^= v;
        h *= 1099511628211ull;                         // FNV-1a 64 prime
    };
    mix(static_cast<std::uint64_t>(c.shape));
    mix(c.meshLibMeshId);
    mix(c.generation);
    mix(c.meshLibPath.size());
    mix(c.assetMeshKey);
    mix(c.assetModel.size());
    mix(cacheEpoch);
    return h | 1ull;
}

// ==== PreAllSteps ====
void PhysicsSystem::PreAllSteps(World& world, bool editSync)
{
    // Reset the per-frame substep counter (see StepOnce).
    m_stepIndexInFrame = 0;
    if (!m_impl) return;
#if WITH_HARNESS
    // -physics_no_edit_sync: Edit mode builds no bodies (the behaviour before SyncEditMode).
    static const bool s_noEditSync = Sys::CommandLine::Get().Has("-physics_no_edit_sync");
    if (editSync && s_noEditSync) return;
#endif

    // One zone per phase: every phase below is O(bodies) and several do per-body hash lookups, so the
    // zones say which one dominates. This zone's self time is what the sub-phases do not cover.
    ZoneScopedN("Phys.PreAllSteps");

    // Auto-prewarm: find the Mesh / ConvexHull / MeshAsset colliders whose shape is not built yet and
    // build them in parallel before Phase 1. Otherwise body creation builds them one at a time on the
    // main thread (the lazy GetOrBuildMeshShape path): seconds of serial BVH construction for a
    // freshly loaded .iscene with thousands of mesh colliders.
    //
    // The scan runs every frame, so it must not allocate: a std::string key per collider was ~14k heap
    // allocations per frame on an imported level. ColliderComponent::shapePrewarmMemo remembers an
    // identity already confirmed cached, so the steady-state scan is one 64-bit compare per collider
    // (the memo's declaration has its invalidation contract).
    {
        ZoneScopedN("Phys.Prewarm");
        std::vector<MeshShapeRequest> uncached;
        std::unordered_set<std::string> assetSeen;   // one expansion per (model, mesh) per scan
        if (auto* colPool = world.GetPool<ColliderComponent>())
        {
            auto& colData = colPool->Data();
            for (auto& col : colData)
            {
                // A Mesh's asset collision: its hull / trimesh prims go into the same parallel batch; the cheap
                // compound is assembled at body creation from the cached pieces.
                if (col.shape == ColliderComponent::Shape::MeshAsset)
                {
                    if (col.assetModel.empty()) continue;
                    const std::uint64_t memo = ShapePrewarmMemo(col, m_impl->shapeCacheEpoch);
                    if (col.shapePrewarmMemo == memo) continue;
                    const std::string key = AssetShapeKey(m_impl->blobStamp, col.assetModel, col.assetMeshKey);
                    if (m_impl->meshShapeCache.count(key) != 0)
                    {
                        col.shapePrewarmMemo = memo;
                        continue;
                    }
                    if (assetSeen.insert(key).second)
                        AssetLibraryPrims(col.assetModel, col.assetMeshKey, uncached);
                    continue;
                }
                // ConvexHull counts too: an imported level is mostly hulls, which would otherwise all build
                // serially in the first frame of body creation.
                const bool isHull = (col.shape == ColliderComponent::Shape::ConvexHull);
                if (col.shape != ColliderComponent::Shape::Mesh && !isHull) continue;
                if (col.meshLibPath.empty()) continue;

                const std::uint64_t memo = ShapePrewarmMemo(col, m_impl->shapeCacheEpoch);
                if (col.shapePrewarmMemo == memo) continue;   // already cached - no key, no allocation

                const std::string key = MeshShapeKey(m_impl->blobStamp, col.meshLibPath,
                                                     col.meshLibMeshId, isHull);
                if (m_impl->meshShapeCache.count(key) != 0)
                {
                    col.shapePrewarmMemo = memo;   // confirmed present - skipped from the next frame on
                    continue;
                }
                uncached.push_back({ col.meshLibPath, col.meshLibMeshId, isHull });
            }
        }
        if (!uncached.empty())
            PrewarmMeshShapes(uncached);
    }

    JPH::BodyInterface& bodyIface = m_impl->physics->GetBodyInterface();

    auto* rbPool = world.EnsurePool<RigidBodyComponent>();
    const auto& rbEntities = rbPool->Entities();
    auto&       rbData     = rbPool->Data();
    const size_t n = rbData.size();

    // Phase 0.5 - reap Jolt bodies that no RigidBodyComponent owns any more: the entity was destroyed,
    // lost its RigidBodyComponent, or had it REPLACED. World::DestroyEntity removes the component (and
    // its bodyId) from the pool before this runs, so the live-pool loops never see these orphans. A
    // replace is the same orphan on an entity that still has a RigidBodyComponent: every deserialize
    // builds a fresh component (bodyId invalid) over the slot - Engine.AddComponent / Entity.set on a
    // live body, an undo - and Component Transfer removes and re-adds it within one frame. So "alive
    // and has the component" is not ownership; only the component that still holds THAT body id is.
    // Keyed on the first, Phase 1 would build a second body and overwrite bodyIds[e], leaving the
    // first in the broadphase as a ghost collider nothing can reach (not even OnWorldClear). Runs on
    // the main thread outside the physics step, so RemoveBody / DestroyBody are safe.
    //
    // Gated on a count mismatch: bodyIds holds one entry per live Jolt body, the pool one valid bodyId
    // per body still owned by a component. They diverge only when an owning component was destroyed or
    // replaced (Phase 0.6 / Phase 1 update both together; a replacement starts with bodyId invalid), so
    // a dense count, with no hashing and no World lookups, decides whether the sweep runs at all (the
    // full sweep costs ~0.2 ms per frame on a 7k-body level).
    {
    ZoneScopedN("Phys.Reap");
    size_t ownedBodies = 0;
    for (size_t i = 0; i < n; ++i)
        if (rbData[i].bodyId != kInvalidPhysicsBodyId) ++ownedBodies;

    if (m_impl->bodyIds.size() != ownedBodies)
    {
    for (auto it = m_impl->bodyIds.begin(); it != m_impl->bodyIds.end(); )
    {
        const Entity e = it->first;
        RigidBodyComponent* owner = world.IsAlive(e) ? rbPool->Get(e) : nullptr;
        if (owner && owner->bodyId == it->second)
        {
            ++it;
            continue;
        }
        const JPH::BodyID deadId(it->second);
        if (!deadId.IsInvalid())
        {
            // A replaced component waits for Phase 1 to build its body from the new values. Hand it the old
            // body's velocity first, so rewriting (say) the mass of a moving body does not stop it dead; Phase 1
            // applies it like a launch velocity. A velocity the new component carries itself wins.
            if (owner && owner->bodyId == kInvalidPhysicsBodyId && !owner->hasInitialVelocity &&
                owner->motion == RigidBodyComponent::Motion::Dynamic)
            {
                JPH::Vec3 lin, ang;
                bodyIface.GetLinearAndAngularVelocity(deadId, lin, ang);
                owner->initialVelocity        = { lin.GetX(), lin.GetY(), lin.GetZ() };
                owner->initialAngularVelocity = { ang.GetX(), ang.GetY(), ang.GetZ() };
                owner->hasInitialVelocity     = true;
            }
            bodyIface.RemoveBody(deadId);
            bodyIface.DestroyBody(deadId);
            m_impl->contactTracker->BodyRemoved(it->second);
        }
        // The shape hash, pushed pose and motion mirror live on the RigidBodyComponent, which is gone or
        // fresh: nothing else to drop here.
        m_impl->poseSnapshots.erase(e);
        it = m_impl->bodyIds.erase(it);
    }
    }   // orphans present
    }   // Phys.Reap

    // Phase 0.6 - runtime collider swap: destroy the body of every entity whose ColliderComponent
    // changed in a shape-affecting way, detected by
    //   (a) ColliderComponent::generation advancing (MarkDirty, or InvalidateMeshShapeCache after a
    //       re-bake), or
    //   (b) the collider fingerprint drifting from the one recorded at build (ColliderShapeHashInline
    //       every frame, ColliderPathHash on a rotating 1/kPathHashSlices subset), which catches
    //       Inspector reflection writes that bypass MarkDirty.
    // Two RigidBodyComponent fields baked at creation rebuild it too: lockedAxes and the motion type
    // (not switched in place, see below). Phase 1 then recreates the body from the new fields. The
    // pool is fetched once, not per body.
    {
    ZoneScopedN("Phys.ColliderDiff");
    static constexpr size_t kPathHashSlices = 16;
    const size_t pathSlice = m_impl->colliderDiffSlice;
    m_impl->colliderDiffSlice = (m_impl->colliderDiffSlice + 1) % kPathHashSlices;

    auto* colPoolDiff = world.GetPool<ColliderComponent>();
    for (size_t i = 0; i < n; ++i)
    {
        RigidBodyComponent& rb = rbData[i];
        if (rb.bodyId == kInvalidPhysicsBodyId) continue;

        const Entity e = rbEntities[i];
        // Non-const: a rebuild clears the prewarm memo, so a mesh swap the fingerprint caught (but
        // generation did not) is picked up by the next frame's batch prewarm, not the serial lazy build.
        ColliderComponent* col = colPoolDiff ? colPoolDiff->Get(e) : nullptr;
        if (!col)
        {
            // The collider was removed: destroy the orphaned body so Phase 1 does not re-attach it.
            bodyIface.RemoveBody(JPH::BodyID(rb.bodyId));
            bodyIface.DestroyBody(JPH::BodyID(rb.bodyId));
            m_impl->contactTracker->BodyRemoved(rb.bodyId);
            rb.bodyId = kInvalidPhysicsBodyId;
            rb.lastBuiltGeneration    = 0;
            rb.lastBuiltShapeHash     = 0;
            rb.lastBuiltPathHash      = 0;
            rb.pushedPoseValid        = false;
            rb.poseSrcValid           = false;
            rb.lastAppliedMotionValid = false;
            m_impl->bodyIds.erase(e);
            m_impl->poseSnapshots.erase(e);
            continue;
        }

        bool needsRebuild = (rb.lastBuiltGeneration != col->generation);
        if (!needsRebuild && rb.lastBuiltLockedAxes != rb.lockedAxes)
        {
            // Jolt has no public BodyInterface call for a runtime DOF change: recreate the body so
            // MotionProperties::SetMassProperties recomputes the inertia tensor with the new allowed-DOF mask.
            needsRebuild = true;
        }
        // The motion type is rebuilt too, through Phase 1, not switched in place. Jolt's SetMotionType
        // cannot do it: a body created Static has no MotionProperties, so switching it to Kinematic /
        // Dynamic dereferences null (an access violation in Release); a body built Kinematic would turn
        // Dynamic with the shape's default mass and no axis locks, both applied at creation only; and the
        // object layer is the Collider's to decide (an explicit layer, a trigger on Sensing). Phase 1
        // derives all of it, including the Mesh-collider clamp to Static.
        const bool motionChanged = !rb.lastAppliedMotionValid || rb.lastAppliedMotion != rb.motion;
        if (!needsRebuild && motionChanged)
            needsRebuild = true;
        if (!needsRebuild && rb.lastBuiltShapeHash != ColliderShapeHashInline(*col))
            needsRebuild = true;
        // The path content is the expensive half (an out-of-line heap read), so only this frame's slice
        // pays for it. A same-length path edit that also skipped MarkDirty is caught within
        // kPathHashSlices frames; every other collider field on the next frame.
        if (!needsRebuild && (i % kPathHashSlices) == pathSlice &&
            rb.lastBuiltPathHash != ColliderPathHash(*col))
            needsRebuild = true;
        if (!needsRebuild) continue;

        const JPH::BodyID id(rb.bodyId);
        bodyIface.RemoveBody(id);
        bodyIface.DestroyBody(id);
        m_impl->contactTracker->BodyRemoved(rb.bodyId);
        rb.bodyId = kInvalidPhysicsBodyId;
        m_impl->bodyIds.erase(e);   // Phase 1 re-adds it when the body is rebuilt
        m_impl->poseSnapshots.erase(e);
        rb.pushedPoseValid        = false;   // rebuilt body must re-receive its pose
        rb.poseSrcValid           = false;
        rb.lastAppliedMotionValid = false;
        col->shapePrewarmMemo = 0;   // re-check against the shape cache next scan
        if (motionChanged)
            LOG_INFO("PhysicsSystem: motion type changed on entity %u (%d -> %d) — rebuilding body",
                     e, (int)rb.lastAppliedMotion, (int)rb.motion);
        else
            LOG_INFO("PhysicsSystem: collider changed on entity %u — rebuilding body", e);
    }
    }   // Phys.ColliderDiff

    // Phase 1 - lazily create Jolt bodies for entities that have none yet.
    //
    // Bodies are created here but added to the broadphase in one bulk batch after the loop. Adding
    // them one at a time (CreateAndAddBody) means N incremental insertions - 7k in one frame on the
    // first Play press of an imported level, a ~100 ms spike - while AddBodiesPrepare / Finalize
    // builds one subtree for the batch. Split by activation because AddBodiesFinalize takes one
    // EActivation per batch. In the steady state this is an early-out sweep over every body; the zone
    // shows that cost apart from the frames that create something.
    {
    ZoneScopedN("Phys.CreateBodies");
    std::vector<JPH::BodyID> pendingStaticAdds;
    std::vector<JPH::BodyID> pendingActiveAdds;
    size_t createdBodies = 0;

    for (size_t i = 0; i < n; ++i)
    {
        RigidBodyComponent& rb = rbData[i];
        if (rb.bodyId != kInvalidPhysicsBodyId) continue;

        const Entity e = rbEntities[i];
        const ColliderComponent* col = world.GetComponent<ColliderComponent>(e);
        if (!col) continue;

        // A shape that did not build is not retried until the collider changes (the failure is recorded
        // against its hash).
        const std::uint64_t failKey = (std::uint64_t(world.GetGeneration(e)) << 32) | std::uint64_t(e);
        const std::uint64_t failHash = ColliderShapeHashInline(*col) ^ ColliderPathHash(*col);
        if (!m_impl->shapeFailed.empty())
            if (const auto f = m_impl->shapeFailed.find(failKey); f != m_impl->shapeFailed.end() && f->second == failHash)
                continue;
        // A MeshAsset collider with no model names the entity's StaticMesh. The shape is built from the
        // resolved copy; every hash below stays the live component's (Phase 0.6 compares against those).
        ColliderComponent        resolvedCol;
        const ColliderComponent* shapeSrc = col;
        if (col->shape == ColliderComponent::Shape::MeshAsset && col->assetModel.empty())
        {
            const StaticMeshComponent* sm = world.GetComponent<StaticMeshComponent>(e);
            resolvedCol = *col;
            if (sm)
            {
                resolvedCol.assetModel   = sm->model;
                resolvedCol.assetMeshKey = sm->meshKey;
            }
            shapeSrc = &resolvedCol;
        }

        // A CharacterControllerComponent takes precedence over a RigidBody on the same entity (they
        // cannot coexist): skip the body so the CharacterVirtual is the character's only collider.
        // Otherwise a kinematic body and a CharacterVirtual sit at the same pose, and the
        // CharacterVirtual's penetration recovery pushes the character off its own body ("magic motion" /
        // jitter).
        if (world.HasComponent<CharacterControllerComponent>(e))
        {
            static std::unordered_set<Entity> s_warned;
            if (s_warned.insert(e).second)
            {
                LOG_WARNING("PhysicsSystem: entity %u has both CharacterControllerComponent "
                            "and RigidBodyComponent — skipping RB body creation (CCC wins). "
                            "Remove RigidBody + Collider from this entity to silence this "
                            "warning; CCC's CharacterVirtual handles its own collision.",
                            static_cast<uint32_t>(e));
            }
            continue;
        }

        // A Dynamic body starts where Jolt last had the entity (LocalTransform, written back in Phase 4),
        // not at the render-interpolated GlobalTransform. Rebuilt from the latter (a replaced component, a
        // collider or motion change) it would start up to a step behind, and Phase 2 would take the gap for
        // a teleport: snap it forward and zero its velocity, so a moving body restarts from rest and the
        // reaper's handed-over velocity is lost. A first build reads the same pose either way: nothing
        // interpolates an entity before it has a body.
        const WorldPose wp = ReadEntityWorldPose(world, e, rb.motion == RigidBodyComponent::Motion::Dynamic);
        const JPH::Vec3 pos = wp.pos;
        const JPH::Quat rot = wp.rot;
        const DirectX::XMFLOAT3 scl = wp.scale;

        // Jolt allows trimesh shapes only on static bodies: clamp the motion type when a Mesh collider is
        // paired with a Dynamic / Kinematic RigidBody.
        const JPH::EMotionType rawMt = ToJolt(rb.motion);
        const bool trimesh = shapeSrc->shape == ColliderComponent::Shape::Mesh ||
                             (shapeSrc->shape == ColliderComponent::Shape::MeshAsset &&
                              AssetNeedsStatic(shapeSrc->assetModel, shapeSrc->assetMeshKey));
        const JPH::EMotionType mt =
            (trimesh && rawMt != JPH::EMotionType::Static) ? JPH::EMotionType::Static : rawMt;
        if (mt != rawMt)
        {
            LOG_WARNING("PhysicsSystem: entity %u uses Mesh collider but motion=%d; "
                        "coercing to Static (Jolt MeshShape doesn't support dynamic bodies)",
                        e, (int)rb.motion);
            rb.motion = RigidBodyComponent::Motion::Static;
        }
        // Layer: an explicit ColliderComponent::layer pins the body (enum value == Jolt layer index + 1).
        // Auto maps by motion type - Static -> STATIC_ENV, Dynamic / Kinematic -> DYNAMIC_PROP - except
        // trigger colliders, which go to SENSING so invisible volumes do not intercept gameplay raycasts
        // (LAYER_MASK_DEFAULT excludes SENSING). Characters do not go through RigidBodyComponent
        // (CharacterControllerComponent + CharacterVirtual instead).
        JPH::ObjectLayer layer;
        if (col->layer != ColliderComponent::CollisionLayer::Auto)
            layer = static_cast<JPH::ObjectLayer>(
                static_cast<std::uint8_t>(col->layer) - 1);
        else if (col->isTrigger)
            layer = Layers::SENSING;
        else
            layer = (mt == JPH::EMotionType::Static) ? Layers::STATIC_ENV
                                                     : Layers::DYNAMIC_PROP;
        if (layer >= Layers::NUM_LAYERS) layer = Layers::STATIC_ENV;  // corrupt save guard

        JPH::ShapeRefC shape = MakeShape(m_impl->meshShapeCache, m_impl->blobCache, m_impl->blobStamp, *shapeSrc, scl);
        if (!shape)
        {
            // No body rather than a stand-in (the reason was logged once).
            m_impl->shapeFailed[failKey] = failHash;
            continue;
        }
        if (!m_impl->shapeFailed.empty()) m_impl->shapeFailed.erase(failKey);
        JPH::BodyCreationSettings settings(shape, pos, rot, mt, layer);
        settings.mLinearDamping  = rb.linearDamping;
        settings.mAngularDamping = rb.angularDamping;
        settings.mFriction       = rb.friction;
        settings.mRestitution    = rb.restitution;
        settings.mGravityFactor  = rb.gravityFactor;
        // Axis locks matter only on Dynamic bodies (Static / Kinematic run no velocity integration);
        // leaving them unset otherwise keeps Jolt on its default path, which skips mass properties.
        if (mt == JPH::EMotionType::Dynamic)
            settings.mAllowedDOFs = ToJoltAllowedDOFs(rb.lockedAxes);
        if (mt == JPH::EMotionType::Dynamic)
        {
            settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            settings.mMassPropertiesOverride.mMass = rb.mass;
        }

        // The owning entity rides in the user data, so the ContactListener needs no BodyID -> Entity map.
        settings.mUserData = PackBodyUser(e, world.GetGeneration(e));

        // Trigger volumes: a sensor still fires OnContactAdded / Removed (-> Lua OnEnter / OnExit) but has
        // no collision response, so the volume does not block or push what enters it
        // (ColliderComponent::isTrigger).
        if (col->isTrigger)
            settings.mIsSensor = true;

        const bool isStaticBody = (mt == JPH::EMotionType::Static);
        JPH::Body* body = bodyIface.CreateBody(settings);
        if (body == nullptr)
        {
            LOG_ERROR("PhysicsSystem: CreateBody failed for entity %u (body pool full?)", e);
            continue;
        }
        // Queued for the bulk broadphase add after the loop. Everything below needs the body only to
        // exist in the body manager (CreateBody guarantees it), not to be in the broadphase.
        const JPH::BodyID id = body->GetID();
        (isStaticBody ? pendingStaticAdds : pendingActiveAdds).push_back(id);
        ++createdBodies;

        rb.bodyId = id.GetIndexAndSequenceNumber();
        rb.lastBuiltGeneration = col->generation;
        rb.lastBuiltLockedAxes = rb.lockedAxes;
        // Track entity -> bodyId so the Phase 0.5 reaper can destroy this body once the entity is
        // destroyed (its RigidBodyComponent and bodyId will be gone from the pool by then).
        m_impl->bodyIds[e] = rb.bodyId;
        // Fingerprint the shape-defining fields so Phase 0.6 can detect direct Inspector edits without
        // MarkDirty().
        rb.lastBuiltShapeHash     = ColliderShapeHashInline(*col);
        rb.lastBuiltPathHash      = ColliderPathHash(*col);
        rb.lastAppliedMotion      = rb.motion;
        rb.lastAppliedMotionValid = true;
        rb.pushedPoseValid        = false;   // Phase 2 pushes the first pose
        rb.poseSrcValid           = false;
        // Not logged per body: a freshly loaded level creates ~7k bodies in one frame, and 7k formatted
        // LOG_INFO calls are a measurable slice of it. One summary line after the loop.

        // Seed the interpolation snapshot so the first render frame after creation does not lerp from a
        // stale previous pose at the origin.
        if (mt == JPH::EMotionType::Dynamic)
        {
            auto& snap = m_impl->poseSnapshots[e];
            snap.currPos = bodyIface.GetPosition(id);
            snap.currRot = bodyIface.GetRotation(id);
            snap.prevPos = snap.currPos;
            snap.prevRot = snap.currRot;

            // One-shot launch velocity (Physics.SpawnBall, or the reaper's hand-over), applied here so it
            // survives the lazy body-creation gap: the script sets the field, the body picks it up when first
            // built. Cleared so a later collider-swap rebuild does not re-fire it.
            if (rb.hasInitialVelocity)
            {
                bodyIface.SetLinearAndAngularVelocity(
                    id,
                    JPH::Vec3(rb.initialVelocity.x, rb.initialVelocity.y, rb.initialVelocity.z),
                    JPH::Vec3(rb.initialAngularVelocity.x, rb.initialAngularVelocity.y, rb.initialAngularVelocity.z));
                rb.hasInitialVelocity = false;
            }
        }
    }

    // Phase 1b - bulk broadphase insertion of everything Phase 1 created. AddBodiesPrepare reorders
    // the id array, which is fine: all per-entity bookkeeping (rb.bodyId, bodyIds, the shape hashes,
    // poseSnapshots) was written inside the loop and is keyed by entity, not by position.
    if (createdBodies > 0)
    {
        const auto bulkAdd = [&bodyIface](std::vector<JPH::BodyID>& ids, JPH::EActivation act)
        {
            if (ids.empty()) return;
            const int count = static_cast<int>(ids.size());
            JPH::BodyInterface::AddState st = bodyIface.AddBodiesPrepare(ids.data(), count);
            bodyIface.AddBodiesFinalize(ids.data(), count, st, act);
        };
        bulkAdd(pendingStaticAdds, JPH::EActivation::DontActivate);
        bulkAdd(pendingActiveAdds, JPH::EActivation::Activate);

        LOG_INFO("PhysicsSystem: created %zu body(ies) (%zu static, %zu active)",
                 createdBodies, pendingStaticAdds.size(), pendingActiveAdds.size());
    }
    }   // Phys.CreateBodies

    // Phase 1c - Kinematic Character Controllers (KCC). Independent of RigidBody / Collider: a
    // JPH::CharacterVirtual does not register a body with the broadphase itself. Created lazily on
    // first sight, rebuilt when the capsule changes (CharacterVirtual has no live shape swap), torn
    // down when the component goes. Not in Edit mode: nothing there would move a character to follow an
    // edit, and Play creates it at the pose it starts from.
    if (!editSync)
    {
        ZoneScopedN("Phys.KCC");
        auto* ccPool = world.GetPool<CharacterControllerComponent>();

        // Tear down KCCs whose entity lost (or never had) the component, or died and left its id to a new
        // entity (the key carries the generation).
        for (auto it = m_impl->kccTable.begin(); it != m_impl->kccTable.end(); )
        {
            const Entity e = KccKeyEntity(it->first);
            if (!CharacterKeyLive(world, it->first) || !world.HasComponent<CharacterControllerComponent>(e))
            {
                LOG_INFO("PhysicsSystem: tearing down KCC for entity %u (component removed)", e);
                m_impl->kccShapeSnaps.erase(it->first);
                it = m_impl->kccTable.erase(it);
            }
            else ++it;
        }

        if (ccPool)
        {
            const auto& ccEntities = ccPool->Entities();
            auto&       ccData     = ccPool->Data();
            const size_t ncc       = ccData.size();
            for (size_t i = 0; i < ncc; ++i)
            {
                CharacterControllerComponent& cc = ccData[i];
                const Entity e = ccEntities[i];

                const std::uint64_t key = CharacterKeyOf(world, e);
                auto cvIt = m_impl->kccTable.find(key);
                const bool exists = (cvIt != m_impl->kccTable.end());

                // What the character needs: (a) a CharacterVirtual, when there is none yet or the broad-phase
                // proxy was toggled (the constructor creates the inner body, and there is no API to add or drop it
                // later); (b) a new shape, when the capsule differs from the one built (a direct Inspector edit
                // that bypassed MarkDirty counts); (c) a re-seed, when cc.generation moved past lastBuiltGeneration
                // (MarkDirty: Character.Teleport, SetCapsule). (b) and (c) change the CharacterVirtual in place, so
                // its inner body, ground state and contacts stay. -kcc_legacy rebuilds for all three.
                bool needsBuild = !exists;
                bool reshape    = false;
                bool reseed     = false;
                if (exists)
                {
                    const auto snapIt = m_impl->kccShapeSnaps.find(key);
                    if (snapIt == m_impl->kccShapeSnaps.end() || snapIt->second.queryBody != cc.queryBody)
                        needsBuild = true;
                    else
                    {
                        reshape = snapIt->second.radius != cc.capsuleRadius ||
                                  snapIt->second.halfHeight != cc.capsuleHalfHeight;
                        reseed  = cc.generation != cc.lastBuiltGeneration;
                    }
                    if ((reshape || reseed) && KccLegacy())
                    {
                        needsBuild = true;
                        reshape = reseed = false;
                    }
                }

                if (!needsBuild && (reshape || reseed))
                {
                    JPH::CharacterVirtual* cv = cvIt->second.GetPtr();
                    if (reshape)
                    {
                        JPH::DefaultBroadPhaseLayerFilter bpFilter(*m_impl->objVsBpFilter, Layers::CHARACTER);
                        JPH::DefaultObjectLayerFilter    objFilter(*m_impl->objLayerPairFilter, Layers::CHARACTER);
                        const JPH::ShapeRefC capsule(new JPH::CapsuleShape(cc.capsuleHalfHeight, cc.capsuleRadius));
                        // The offset first: SetShape tests nothing with FLT_MAX, but the inner body follows both.
                        cv->SetShapeOffset(JPH::Vec3(0, cc.capsuleHalfHeight + cc.capsuleRadius, 0));
                        cv->SetShape(capsule, FLT_MAX, bpFilter, objFilter, JPH::BodyFilter(), JPH::ShapeFilter(),
                                     *m_impl->tempAlloc);
                        if (cc.queryBody) cv->SetInnerBodyShape(capsule);
                        m_impl->kccShapeSnaps[key] = { cc.capsuleRadius, cc.capsuleHalfHeight, cc.queryBody };
                    }
                    if (reseed)
                    {
                        // To where LocalTransform says (the write-back keeps it equal to the character's own
                        // position), not to the interpolated GlobalTransform a step behind it.
                        cv->SetPosition(ReadEntityWorldPose(world, e, true).pos);
                        cv->SetLinearVelocity(JPH::Vec3::sZero());
                        cc.velocity       = { 0.f, 0.f, 0.f };
                        cc.groundVelocity = { 0.f, 0.f, 0.f };
                        cc.isGrounded     = false;
                        cc.wasGrounded    = false;
                        cc.groundNormal   = { 0.f, 1.f, 0.f };
                        cc.groundEntity   = NullEntity;
                        cc.timeInAir      = 0.f;
                    }
                    cc.lastBuiltGeneration = cc.generation;
                    LOG_INFO("PhysicsSystem: %s CharacterVirtual of entity %u in place (radius=%.2f halfHeight=%.2f)",
                             reseed ? "re-seeded" : "reshaped", e, cc.capsuleRadius, cc.capsuleHalfHeight);
                    continue;
                }
                if (!needsBuild) continue;

                // A rebuild of a live character starts where it stands (LocalTransform); a new one where the
                // entity's world pose says.
                const WorldPose wp = ReadEntityWorldPose(world, e, exists && !KccLegacy());

                JPH::Ref<JPH::CharacterVirtualSettings> settings = new JPH::CharacterVirtualSettings();
                settings->mShape = JPH::ShapeRefC(new JPH::CapsuleShape(cc.capsuleHalfHeight, cc.capsuleRadius));
                settings->mUp = JPH::Vec3(0, 1, 0);
                settings->mMaxSlopeAngle = cc.maxSlopeRad;
                settings->mMass = cc.mass;
                // Lift the capsule so its bottom hemisphere touches the entity pivot; otherwise a mesh pivoted at
                // the feet sinks half its height into the floor.
                settings->mShapeOffset = JPH::Vec3(0, cc.capsuleHalfHeight + cc.capsuleRadius, 0);
                settings->mCharacterPadding = cc.skinWidth;
                settings->mPredictiveContactDistance = 0.1f;

                // Broad-phase proxy (CharacterControllerComponent::queryBody): the same capsule as the character.
                // Jolt creates it Kinematic, keeps it in sync from Update() / MoveToContact(), stamps the
                // CharacterVirtual's user data (the entity id, passed below) onto it, excludes it from the
                // character's own collide-and-slide (IgnoreSingleBodyFilterChained) and destroys it in
                // ~CharacterVirtual: nothing to unwind here.
                //
                // Not a sensor, on purpose: the KCC trigger sweep treats every sensor body as a trigger volume, so
                // a sensor proxy would make every character a trigger volume to every other character.
                if (cc.queryBody)
                {
                    settings->mInnerBodyShape = settings->mShape;
                    settings->mInnerBodyLayer = Layers::CHARACTER;
                }

                JPH::Ref<JPH::CharacterVirtual> cv = new JPH::CharacterVirtual(
                    settings.GetPtr(), wp.pos, wp.rot,
                    PackBodyUser(e, world.GetGeneration(e)), m_impl->physics.get());

                m_impl->kccTable[key]      = cv;
                m_impl->kccShapeSnaps[key] = { cc.capsuleRadius, cc.capsuleHalfHeight,
                                               cc.queryBody };
                cc.lastBuiltGeneration   = cc.generation;
                // Published for gameplay / tools that want the proxy's BodyID. Not registered in
                // m_impl->bodyIds: that map is the RigidBodyComponent body ledger, and Phase 0.5 compares its size
                // with the number of components that own a body. Jolt owns this body's lifetime.
                cc.bodyId = cv->GetInnerBodyID().IsInvalid()
                    ? kInvalidPhysicsBodyId
                    : cv->GetInnerBodyID().GetIndexAndSequenceNumber();
                // Reset the run-time state: the character starts at rest.
                cc.velocity     = { 0.f, 0.f, 0.f };
                cc.groundVelocity = { 0.f, 0.f, 0.f };
                cc.isGrounded   = false;
                cc.wasGrounded  = false;
                cc.groundNormal = { 0.f, 1.f, 0.f };
                cc.groundEntity = NullEntity;
                cc.timeInAir    = 0.f;

                #if KCC_DEBUG >= 2
                // First build / rebuild diagnostic: dumps the world-pose sources to spot LT / Global mismatches
                // and parent links. A KCC rebuild during play is suspicious, so it is logged loudly. Tier 2: one
                // line per character built, which a spawn wave multiplies.
                {
                    const LocalTransform* lt = world.GetComponent<LocalTransform>(e);
                    const GlobalTransform* gt = world.GetComponent<GlobalTransform>(e);
                    const auto* parent = world.GetComponent<Parent>(e);
                    LOG_INFO("[KCC ent=%u] CV (re)build: wp.pos=(%.3f,%.3f,%.3f) "
                             "LT.t=(%.3f,%.3f,%.3f) GT[3]=(%.3f,%.3f,%.3f) parent=%u  "
                             "(if LT vs GT diverge → parented; if rebuild happens mid-play → MarkDirty unexpectedly)",
                             static_cast<uint32_t>(e),
                             wp.pos.GetX(), wp.pos.GetY(), wp.pos.GetZ(),
                             lt ? lt->translation.x : 0.f,
                             lt ? lt->translation.y : 0.f,
                             lt ? lt->translation.z : 0.f,
                             gt ? gt->matrix._41 : 0.f,
                             gt ? gt->matrix._42 : 0.f,
                             gt ? gt->matrix._43 : 0.f,
                             parent ? static_cast<uint32_t>(parent->entity) : 0u);
                }
                #endif

                LOG_INFO("PhysicsSystem: created CharacterVirtual for entity %u "
                         "(radius=%.2f halfHeight=%.2f slope=%.0fdeg)",
                         e, cc.capsuleRadius, cc.capsuleHalfHeight,
                         cc.maxSlopeRad * 57.2957795f);
            }
        }
    }

    // Phase 2 - reconcile Inspector-side edits with Jolt before simulating:
    //   * a motion type changed in the UI is Phase 0.6's (the body is rebuilt, not switched in place);
    //   * Static / Kinematic: the world pose is authoritative and is pushed to Jolt when it moves, so
    //     switching back to Dynamic starts from the edited pose, not the stale Jolt one;
    //   * Dynamic: a LocalTransform that differs from Jolt's pose is a teleport.
    // Zoned so its cost does not read as PreAllSteps self time.
    {
    ZoneScopedN("Phys.PoseReconcile");

    // Pool pointers hoisted out of the loop: World::GetComponent<T> is a type_index hash probe per
    // call, the pool's Get() two array loads, and this loop visits every body (~7k on an imported
    // level) every frame, nearly all of them on the "nothing changed" fast path. Safe for the whole
    // loop: nothing in it adds or removes a component, so no pool can reallocate.
    auto* gtPool = world.GetPool<GlobalTransform>();
    auto* ltPool = world.GetPool<LocalTransform>();

    for (size_t i = 0; i < n; ++i)
    {
        RigidBodyComponent& rb = rbData[i];
        if (rb.bodyId == kInvalidPhysicsBodyId) continue;

        const JPH::BodyID id = JPH::BodyID(rb.bodyId);
        // No motion-type switch here: a live body's motion always matches its component, because Phase
        // 0.6 rebuilds one that drifted (and Phase 1 seeds a Dynamic body's interpolation snapshot).

        if (rb.motion != RigidBodyComponent::Motion::Dynamic)
        {
            // A Kinematic body that has its first pose follows its LocalTransform chain from here on, moved
            // with MoveKinematic every substep (MoveKinematicBodies). Edit mode has no step: teleport there.
            if (rb.motion == RigidBodyComponent::Motion::Kinematic && rb.pushedPoseValid && !editSync &&
                !KinematicTeleportLegacy())
                continue;

            // Fast path: if the GlobalTransform matrix is byte-identical to the one this body's Jolt pose came
            // from, nothing downstream can differ - skip the decompose. Guarded like ReadEntityWorldPose's
            // trust of GlobalTransform, so an identity matrix (pose from the LocalTransform chain) never takes
            // the shortcut.
            const GlobalTransform* gt = gtPool ? gtPool->Get(rbEntities[i]) : nullptr;
            // Byte-compare before IsIdentityMatrix: equivalent, because poseSrcValid is only set together
            // with a matrix IsIdentityMatrix already rejected (its sole writer is below), so a byte-equal match
            // implies gtAuthoritative. Cheaper, because IsIdentityMatrix scans row-major and a static level is
            // translation-only matrices, which differ from identity only in the last row: 12 float compares
            // per body per frame.
            if (gt && rb.poseSrcValid
                && std::memcmp(&gt->matrix, &rb.poseSrcGlobal, sizeof gt->matrix) == 0)
            {
                continue;
            }
            const bool gtAuthoritative = gt && !IsIdentityMatrix(gt->matrix);

            // Static / Kinematic: use the world-space pose (GlobalTransform) so scene-graph children placed
            // by parent transforms end up at their visual position; LocalTransform alone is identity for mesh
            // entities under a SceneNodeTag parent.
            const WorldPose wp = ReadEntityWorldPose(world, rbEntities[i]);
            // Push only on a real change: an imported level has thousands of never-moving static bodies, and
            // SetPositionAndRotation takes a body lock and notifies the broadphase of an AABB change on every
            // call. The compare is a few float subtractions against the pose last pushed, read off the
            // component being iterated.
            const JPH::Vec3 lastPos(rb.pushedPos.x, rb.pushedPos.y, rb.pushedPos.z);
            const JPH::Quat lastRot(rb.pushedRot.x, rb.pushedRot.y, rb.pushedRot.z, rb.pushedRot.w);
            constexpr float kPosEps = 1e-5f;
            constexpr float kRotEps = 1e-6f;
            const bool moved =
                !rb.pushedPoseValid ||
                (wp.pos - lastPos).LengthSq() > kPosEps * kPosEps ||
                std::fabs(std::fabs(wp.rot.Dot(lastRot)) - 1.0f) > kRotEps;
            if (moved)
            {
                bodyIface.SetPositionAndRotation(id, wp.pos, wp.rot, JPH::EActivation::Activate);
                rb.pushedPos = { wp.pos.GetX(), wp.pos.GetY(), wp.pos.GetZ() };
                rb.pushedRot = { wp.rot.GetX(), wp.rot.GetY(), wp.rot.GetZ(), wp.rot.GetW() };
                rb.pushedPoseValid = true;
            }
            // Remember the exact source matrix so next frame can short-circuit.
            // Recorded even when `moved` was false: the epsilon test already
            // decided this matrix needs no push, so it must not be re-decomposed.
            if (gtAuthoritative)
            {
                rb.poseSrcGlobal = gt->matrix;
                rb.poseSrcValid  = true;
            }
            else
            {
                rb.poseSrcValid = false;   // pose came from the LocalTransform chain
            }
            continue;
        }

        // Dynamic: read LocalTransform, where Phase 4 writes the body back, to detect external teleports.
        const LocalTransform* lt = ltPool ? ltPool->Get(rbEntities[i]) : nullptr;
        if (!lt) continue;

        const JPH::Vec3 ltPos = ToJolt(lt->translation);
        const JPH::Quat ltRot = ToJoltQuat(lt->rotation);

        {
            // Physics is normally authoritative, but Phase 4 only writes back what Jolt computed: a
            // LocalTransform that differs from Jolt's current pose was edited since (Inspector, script,
            // gizmo). Treat it as a teleport: push the edit to Jolt and zero the velocity, so the body drops
            // from the new pose without stale momentum.
            const JPH::Vec3 joltPos = bodyIface.GetPosition(id);
            const JPH::Quat joltRot = bodyIface.GetRotation(id);

            const bool posChanged = !joltPos.IsClose(ltPos);
            const bool rotChanged =
                std::fabs(joltRot.GetX() - ltRot.GetX()) > 1e-6f ||
                std::fabs(joltRot.GetY() - ltRot.GetY()) > 1e-6f ||
                std::fabs(joltRot.GetZ() - ltRot.GetZ()) > 1e-6f ||
                std::fabs(joltRot.GetW() - ltRot.GetW()) > 1e-6f;

            if (posChanged || rotChanged)
            {
                bodyIface.SetPositionAndRotation(id, ltPos, ltRot, JPH::EActivation::Activate);
                bodyIface.SetLinearAndAngularVelocity(id, JPH::Vec3::sZero(), JPH::Vec3::sZero());

                // Re-seed the interpolation snapshot; otherwise ApplyRenderInterpolation would lerp from the old
                // pose to the new one and smear across the gap for one physics step.
                auto& snap = m_impl->poseSnapshots[rbEntities[i]];
                snap.currPos = ltPos;
                snap.currRot = ltRot;
                snap.prevPos = snap.currPos;
                snap.prevRot = snap.currRot;
            }
        }
    }
    } // Phys.PoseReconcile

    // No step runs in Edit mode, so nothing drains what the reaper queued.
    if (editSync) m_impl->contactTracker->Clear();
}

// ==== RunKccUpdates ====
// Every character's CharacterVirtual::ExtendedUpdate for this step, as islands on the task pool.
//
// Two characters depend on each other this step when their reach boxes overlap (one may see the
// other's inner body at either end of its move), and all characters near a DYNAMIC body share one
// island (a push changes that body's velocity for whoever touches it next). Islands are the
// connected components of that relation: inside an island the updates run in index order, islands
// run in parallel. A character never sees anything outside its island, so the result is the serial
// loop's bit for bit, whatever the schedule.
// Jolt allows concurrent character updates: queries are read-only and moving an inner body takes
// the broad phase's shared lock; each thread brings its own TempAllocator (Jolt's is a
// single-threaded stack).
namespace
{
    // How far past its own capsule a character's update can reach, beyond its
    // displacement this step. HORIZONTAL: walk-stairs' forward test (0.15),
    // predictive contact distance (0.1), padding (skinWidth) and penetration
    // recovery, with margin. VERTICAL: stick-to-floor (0.5 down) plus
    // walk-stairs' step up (0.4) and its step back down. Displacement is added
    // in EVERY horizontal direction, not only along the velocity: a slide
    // along a wall redirects the move.
    constexpr float  kKccReachH = 0.5f;
    constexpr float  kKccReachV = 1.0f;
    constexpr size_t kKccMinParallel = 8;

    // Early-out collector: is there any DYNAMIC body in the box? Static,
    // kinematic and sensor bodies never change during the KCC pass, and the
    // other characters' (kinematic) inner bodies are the pairwise test's job.
    class DynamicBodyProbe final : public JPH::CollideShapeBodyCollector
    {
    public:
        explicit DynamicBodyProbe(const JPH::BodyInterface& bi) : m_bi(bi) {}
        void AddHit(const JPH::BodyID& id) override
        {
            if (m_bi.GetMotionType(id) == JPH::EMotionType::Dynamic)
            {
                found = true;
                ForceEarlyOut();
            }
        }
        bool found = false;
    private:
        const JPH::BodyInterface& m_bi;
    };

    uint32_t IslandRoot(std::vector<uint32_t>& parent, uint32_t x)
    {
        while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
        return x;
    }
}

void PhysicsSystem::Impl::RunKccUpdates(std::vector<KccStep>& steps)
{
    const size_t ns = steps.size();
    if (ns == 0) return;
    // The weight a character puts on the dynamic body it stands on is its mass x gravity; the character integrates
    // its own fall, so this reaches ExtendedUpdate for that impulse alone.
    const JPH::Vec3 weightGravity = KccLegacy() ? JPH::Vec3::sZero() : physics->GetGravity();
    auto update = [this, weightGravity](KccStep& st, JPH::TempAllocator& temp)
    {
        JPH::DefaultBroadPhaseLayerFilter bpFilter(*objVsBpFilter, Layers::CHARACTER);
        JPH::DefaultObjectLayerFilter    objFilter(*objLayerPairFilter, Layers::CHARACTER);
        JPH::BodyFilter                  bodyFilter;   // accept all
        JPH::ShapeFilter                 shapeFilter;  // accept all
        // One collide-and-slide (up to mMaxCollisionIterations sweeps) plus
        // StickToFloor and WalkStairs, per character, per FIXED STEP - so it
        // multiplies by the substep count, not the frame count. Nothing
        // throttles it by distance or visibility, so N = every
        // CharacterControllerComponent alive (the AI population plus the player).
        ZoneScopedN("Phys.KCC.ExtendedUpdate");
        JPH::CharacterVirtual::ExtendedUpdateSettings settings = kccExtUpdate;
        settings.mWalkStairsStepUp = JPH::Vec3(0, st.stepUp, 0);
        st.cv->ExtendedUpdate(
            kFixedDt,
            weightGravity,              // the fall itself is integrated in the pre-step
            settings,
            bpFilter, objFilter, bodyFilter, shapeFilter,
            temp);
    };

    static const bool s_serial = Sys::CommandLine::Get().Has("-kccserial");
    uint32_t pairs = 0, nearDyn = 0, islandCount = 1, largest = static_cast<uint32_t>(ns);
    bool islands = false;
    if (!s_serial && ns >= kKccMinParallel)
    {
        PROF_SCOPE("Phys.KCC.Classify");
        for (KccStep& st : steps)
        {
            JPH::AABox box = st.cv->GetShape()->GetWorldSpaceBounds(
                st.cv->GetCenterOfMassTransform(), JPH::Vec3::sReplicate(1.0f));
            const float d = st.cv->GetLinearVelocity().Length() * kFixedDt;
            // A taller step-up reaches higher, and back down by as much.
            const float stepReach = 2.f * (std::max)(0.f, st.stepUp - 0.4f);
            box.ExpandBy(JPH::Vec3(d + kKccReachH, d + kKccReachV + stepReach, d + kKccReachH));
            st.reach    = box;
            st.nearBody = false;
        }
        // Dynamic bodies in reach: read-only broad-phase queries, so they can
        // run on the pool themselves (nothing moves until the updates below).
        const JPH::BodyInterface& bi = physics->GetBodyInterface();
        const std::uint16_t movable = std::uint16_t(0xFFFFu & ~LayerBit(Layers::STATIC_ENV));
        TaskSystem::Get().ParallelFor(0, static_cast<uint32_t>(ns), [&](uint32_t k)
        {
            DynamicBodyProbe probe(bi);
            physics->GetBroadPhaseQuery().CollideAABox(
                steps[k].reach, probe, LayerMaskBPFilter(movable), LayerMaskFilter(movable));
            steps[k].nearBody = probe.found;
        });
        // Union-find over the dependence relation. Node ns stands for "near a
        // dynamic body": everything that touches one shares one island.
        auto& parent = kccIslandParent;
        parent.resize(ns + 1);
        for (uint32_t k = 0; k <= ns; ++k) parent[k] = k;
        auto join = [&](uint32_t x, uint32_t y)
        {
            x = IslandRoot(parent, x);
            y = IslandRoot(parent, y);
            if (x == y) return;
            if (x < y) parent[y] = x; else parent[x] = y;
        };
        for (uint32_t k = 0; k < ns; ++k)
            if (steps[k].nearBody) { join(k, static_cast<uint32_t>(ns)); ++nearDyn; }
        auto& order = kccParallel;   // sweep-and-prune on min.x
        order.resize(ns);
        for (uint32_t k = 0; k < ns; ++k) order[k] = k;
        std::sort(order.begin(), order.end(), [&](uint32_t x, uint32_t y)
                  { return steps[x].reach.mMin.GetX() < steps[y].reach.mMin.GetX(); });
        for (size_t x = 0; x < ns; ++x)
        {
            const JPH::AABox& rx = steps[order[x]].reach;
            for (size_t y = x + 1; y < ns; ++y)
            {
                const JPH::AABox& ry = steps[order[y]].reach;
                if (ry.mMin.GetX() > rx.mMax.GetX()) break;
                if (rx.Overlaps(ry)) { join(order[x], order[y]); ++pairs; }
            }
        }
        // Islands, members in ascending index order (the fill loop ascends).
        auto& slotOf = kccIslandSlot;
        auto& count  = kccIslandCount;
        slotOf.assign(ns + 1, ~0u);
        count.clear();
        for (uint32_t k = 0; k < ns; ++k)
        {
            const uint32_t r = IslandRoot(parent, k);
            if (slotOf[r] == ~0u) { slotOf[r] = static_cast<uint32_t>(count.size()); count.push_back(0); }
            ++count[slotOf[r]];
        }
        islandCount = static_cast<uint32_t>(count.size());
        largest = count.empty() ? 0u : *std::max_element(count.begin(), count.end());
        islands = islandCount >= 2 && largest < ns;
        if (islands)
        {
            auto& start   = kccIslandStart;
            auto& members = kccIslandMembers;
            start.resize(islandCount + 1);
            start[0] = 0;
            for (uint32_t i = 0; i < islandCount; ++i) start[i + 1] = start[i] + count[i];
            members.resize(ns);
            for (uint32_t i = 0; i < islandCount; ++i) count[i] = start[i];   // reused as a write cursor
            for (uint32_t k = 0; k < ns; ++k)
                members[count[slotOf[IslandRoot(parent, k)]]++] = k;
            // Biggest island first: with the interleaved schedule every chunk
            // starts on one of the expensive ones.
            auto& byCost = kccParallel;
            byCost.resize(islandCount);
            for (uint32_t i = 0; i < islandCount; ++i) byCost[i] = i;
            std::stable_sort(byCost.begin(), byCost.end(), [&](uint32_t x, uint32_t y)
                             { return start[x + 1] - start[x] > start[y + 1] - start[y]; });
        }
    }

    if (islands)
    {
        const auto& start   = kccIslandStart;
        const auto& members = kccIslandMembers;
        const auto& byCost  = kccParallel;
        TaskSystem::Get().ParallelFor(0, islandCount, [&](uint32_t i)
        {
            static thread_local std::unique_ptr<JPH::TempAllocatorImplWithMallocFallback> t_temp;
            if (!t_temp) t_temp = std::make_unique<JPH::TempAllocatorImplWithMallocFallback>(256 * 1024);
            const uint32_t isl = byCost[i];
            for (uint32_t m = start[isl]; m < start[isl + 1]; ++m)
                update(steps[members[m]], *t_temp);
        }, TaskSystem::TaskPriority::High, TaskSystem::ParallelSchedule::Interleaved);
    }
    else
    {
        for (KccStep& st : steps) update(st, *tempAlloc);
    }

    auto& k = kccStats;
    ++k.steps;
    k.chars   += ns;
    k.islands += islands ? islandCount : 1;
    k.largest += islands ? largest : ns;
    k.pairs   += pairs;
    k.bodies  += nearDyn;
    static const int s_every = Sys::CommandLine::Get().Has("-phaselog_every")
                                   ? Sys::CommandLine::Get().GetInt("-phaselog_every", 60) : 0;
    if (s_every > 0 && k.steps % (static_cast<uint64_t>(s_every) * 2) == 0)
    {
        LOG_INFO("[KCC_PARALLEL] steps=%llu chars/step=%.1f islands/step=%.1f largestIsland/step=%.1f "
                 "pairs/step=%.1f nearDynamic/step=%.1f serialFlag=%d",
                 static_cast<unsigned long long>(k.steps), double(k.chars) / k.steps,
                 double(k.islands) / k.steps, double(k.largest) / k.steps,
                 double(k.pairs) / k.steps, double(k.bodies) / k.steps, s_serial ? 1 : 0);
    }
}

// ==== StepOnce ====
// One fixed-dt substep (see the declaration).
void PhysicsSystem::StepOnce(World& world)
{
    // Which substep of this frame is this? PreAllSteps resets the counter once per frame before the
    // substep loop; StepOnce runs up to 5 times after it. FrameContext::physicsStepIndex cannot answer
    // this: it is monotonic for the whole process.
    const bool firstSubstepThisFrame = (m_stepIndexInFrame++ == 0);
    if (!m_impl) return;

    JPH::BodyInterface& bodyIface = m_impl->physics->GetBodyInterface();
    auto* rbPool = world.EnsurePool<RigidBodyComponent>();
    const auto& rbEntities = rbPool->Entities();
    auto&       rbData     = rbPool->Data();
    const size_t n = rbData.size();

    {
        // Before the step: roll the last "current" pose into "previous", so prev / curr bracket the last
        // [t - dt, t] physics interval that ApplyRenderInterpolation's alpha spans.
        for (size_t i = 0; i < n; ++i)
        {
            const RigidBodyComponent& rb = rbData[i];
            if (rb.bodyId == kInvalidPhysicsBodyId) continue;
            if (rb.motion != RigidBodyComponent::Motion::Dynamic) continue;
            auto it = m_impl->poseSnapshots.find(rbEntities[i]);
            if (it == m_impl->poseSnapshots.end()) continue;
            it->second.prevPos = it->second.currPos;
            it->second.prevRot = it->second.currRot;
        }

        // KCC tick: runs before physics->Update so the character's motion (including pushes on dynamic
        // props) lands in the same simulation step. Each CharacterVirtual does its own collide-and-slide;
        // it is fed a target velocity.
        if (auto* ccPool = world.GetPool<CharacterControllerComponent>())
        {
            PROF_SCOPE("Phys.KCC");
            const auto& ccEntities = ccPool->Entities();
            auto&       ccData     = ccPool->Data();
            const size_t ncc       = ccData.size();

            // Diagnostic throttle: fires every 30th step (~twice per second at 60 Hz), but the per-character
            // lines then print once per character per fire, so they flood with a big population (the tiers:
            // KCC_DEBUG, at the top of the file).
            #if KCC_DEBUG >= 1
            static int s_kccLogCounter = 0;
            ++s_kccLogCounter;
            const bool kccLogFire = (s_kccLogCounter % 30) == 0;
            #endif

            // ---- The tick in three passes ----
            //   1. pre-step for everyone (velocity integration, SetLinearVelocity; reads only the character's
            //      own CharacterVirtual and component);
            //   2. ExtendedUpdate as islands (RunKccUpdates): characters that can affect each other this step
            //      stay together in their original order, independent islands run in parallel;
            //   3. write-back for everyone, in index order (component fields, interpolation snapshots, the
            //      diagnostics and their logs).
            // Bit-exact with one serial loop: passes 1 and 3 touch only the character's own state, and what
            // one character's update can change for another - its inner body (queryBody: a CHARACTER-layer
            // body, and CHARACTER collides with CHARACTER) and the velocity of a dynamic body it pushes - stays
            // inside its island. -kccserial runs the updates serially in index order (escape hatch and A/B).
            auto& steps = m_impl->kccSteps;
            steps.clear();
            steps.reserve(ncc);
            for (size_t i = 0; i < ncc; ++i)
            {
                CharacterControllerComponent& cc = ccData[i];
                const Entity e = ccEntities[i];
                auto cvIt = m_impl->kccTable.find(CharacterKeyOf(world, e));
                if (cvIt == m_impl->kccTable.end()) {
                    #if KCC_DEBUG >= 1
                    if (kccLogFire)
                        LOG_WARNING("[KCC ent=%u] has CharacterControllerComponent but NO CharacterVirtual in kccTable — not driven this step",
                                    static_cast<uint32_t>(e));
                    #endif
                    continue;
                }
                JPH::CharacterVirtual* cv = cvIt->second.GetPtr();

                #if KCC_DEBUG >= 1
                // Snapshot pre-step values for the per-step log line.
                const JPH::Vec3 lvPre  = cv->GetLinearVelocity();
                const JPH::Vec3 posPre = cv->GetPosition();
                const auto      gsPre  = cv->GetGroundState();
                const bool      preWasGrounded = cc.isGrounded;
                #endif

                // ---- Escape hatch: instant stop ----
                // Cutscene / teleport sets this to flush inertia; it acts before any velocity integration, so the
                // next sweep starts clean.
                if (cc.instantStopRequested)
                {
                    cv->SetLinearVelocity(JPH::Vec3::sZero());
                    cc.desiredHorizontalVelocity = { 0.f, 0.f, 0.f };
                    cc.velocity                  = { 0.f, 0.f, 0.f };
                    cc.instantStopRequested      = false;
                    #if KCC_DEBUG >= 1
                    LOG_INFO("[KCC ent=%u] instant-stop consumed", static_cast<uint32_t>(e));
                    #endif
                }

                // Was the character on ground at the start of this step? The CharacterVirtual's state from the
                // previous step gates jump vs continued fall. cc.isGrounded must agree: a re-seed clears it, and
                // the state the CharacterVirtual kept from where it stood says nothing about where it is now.
                const auto prevGround = cv->GetGroundState();
                const bool legacy = KccLegacy();
                const bool wasOnGround =
                    (prevGround == JPH::CharacterBase::EGroundState::OnGround) && (legacy || cc.isGrounded);

                // What the component asks of the CharacterVirtual, applied every step so a change in Play counts.
                if (!legacy)
                {
                    cv->SetMaxSlopeAngle(cc.maxSlopeRad);
                    cv->SetMass(cc.mass);
                    cv->SetMaxStrength(cc.pushStrength * kKccPushForce);
                }

                // Pre-step position, for the actual displacement (wasBlockedLastStep detection).
                const JPH::Vec3 prePos = cv->GetPosition();

                // ---- Off-mesh-link traversal ----
                // In LaunchedTraversal the NavAgent has pre-set the CharacterVirtual's velocity to a ballistic
                // launch: gravity still integrates, but desiredHorizontalVelocity does not overwrite the linear
                // velocity - let the projectile fly.
                const bool launched = (cc.mode == MovementMode::LaunchedTraversal);

                // Vertical integration is the KCC's, not Jolt's PhysicsSystem gravity. Grounded -> cancel any
                // leftover -Y from the previous step so it does not accumulate; airborne -> add the gameplay
                // gravity.
                // On a moving platform the character takes the ground's velocity into its own, as Jolt's
                // CharacterVirtual documents (velocity = ground velocity + the input + a jump).
                float vy = cv->GetLinearVelocity().GetY();
                JPH::Vec3 ground = JPH::Vec3::sZero();
                if (wasOnGround && !launched)
                {
                    if (!legacy) ground = cv->GetGroundVelocity();
                    vy = ground.GetY();
                    if (cc.jumpRequested)
                    {
                        vy += cc.jumpSpeed;
                        cc.jumpRequested = false;
                        #if KCC_DEBUG >= 1
                        LOG_INFO("[KCC ent=%u] JUMP applied vy=%.2f (grounded)",
                                 static_cast<uint32_t>(e), vy);
                        #endif
                    }
                }
                else if (!legacy && !launched && cc.jumpRequested && vy <= 0.f && cc.timeInAir <= cc.jumpGraceTime)
                {
                    // Coyote time: the step off a ledge still jumps. The take-off step adds no gravity, like a
                    // grounded one.
                    vy = cc.jumpSpeed;
                    cc.jumpRequested = false;
                    #if KCC_DEBUG >= 1
                    LOG_INFO("[KCC ent=%u] JUMP applied vy=%.2f (coyote, %.3f s in the air)",
                             static_cast<uint32_t>(e), vy, cc.timeInAir);
                    #endif
                }
                else
                {
                    vy += cc.gravity * Impl::kFixedDt;
                    if (vy < -cc.maxFallSpeed) vy = -cc.maxFallSpeed;
                    #if KCC_DEBUG >= 1
                    if (cc.jumpRequested)
                        LOG_INFO("[KCC ent=%u] jump REJECTED (airborne, wasOnGround=%d launched=%d)",
                                 static_cast<uint32_t>(e), wasOnGround ? 1 : 0, launched ? 1 : 0);
                    #endif
                    cc.jumpRequested = false;
                }

                if (launched)
                {
                    // Keep the launch vector's horizontal part; only patch Y.
                    const JPH::Vec3 lvNow = cv->GetLinearVelocity();
                    cv->SetLinearVelocity(JPH::Vec3(lvNow.GetX(), vy, lvNow.GetZ()));
                }
                else
                {
                    cv->SetLinearVelocity(JPH::Vec3(
                        cc.desiredHorizontalVelocity.x + ground.GetX(), vy,
                        cc.desiredHorizontalVelocity.z + ground.GetZ()));
                }


                KccStep st{};
                st.i = i; st.e = e; st.cv = cv; st.prePos = prePos; st.launched = launched;
                st.onGroundPre = wasOnGround;
                st.stepUp = legacy ? 0.4f : cc.stepHeight;
                #if KCC_DEBUG >= 1
                st.lvPre = lvPre; st.posPre = posPre; st.gsPre = gsPre; st.preWasGrounded = preWasGrounded;
                #endif
                steps.push_back(st);
            }

            m_impl->RunKccUpdates(steps);

            // -kcc_statehash: an FNV-1a of every character's position and
            // velocity right after the updates, logged every 60 steps. The
            // bit-exactness proof for anything that reorders the KCC tick
            // (-kccserial vs the default must print identical lines); the
            // chain hash cannot stand in for it - chains solve in skeleton
            // space and never see where the character walked.
            static const bool s_kccHash = Sys::CommandLine::Get().Has("-kcc_statehash");
            if (s_kccHash)
            {
                static uint64_t s_hashStep = 0;
                uint64_t h = 1469598103934665603ull;
                auto mix = [&h](const void* data, size_t n)
                {
                    const auto* b = static_cast<const unsigned char*>(data);
                    for (size_t k = 0; k < n; ++k) { h ^= b[k]; h *= 1099511628211ull; }
                };
                for (const KccStep& st : steps)
                {
                    const JPH::Vec3 pos = st.cv->GetPosition();
                    const JPH::Vec3 vel = st.cv->GetLinearVelocity();
                    const float v[6] = { pos.GetX(), pos.GetY(), pos.GetZ(), vel.GetX(), vel.GetY(), vel.GetZ() };
                    mix(&st.e, sizeof(st.e));
                    mix(v, sizeof(v));
                }
                if (++s_hashStep % 60 == 0)
                    LOG_INFO("[KCCHASH] step=%llu chars=%zu hash=%016llx",
                             static_cast<unsigned long long>(s_hashStep), steps.size(),
                             static_cast<unsigned long long>(h));
            }

            for (const KccStep& st : steps)
            {
                CharacterControllerComponent& cc = ccData[st.i];
                const Entity e = st.e;
                JPH::CharacterVirtual* cv = st.cv;
                const JPH::Vec3 prePos = st.prePos;
                const bool launched = st.launched;
                #if KCC_DEBUG >= 1
                const JPH::Vec3 lvPre  = st.lvPre;
                const JPH::Vec3 posPre = st.posPre;
                const auto      gsPre  = st.gsPre;
                const bool      preWasGrounded = st.preWasGrounded;
                #endif

                // Write post-step state back so gameplay can read it next frame.
                cc.wasGrounded = cc.isGrounded;
                const auto gs = cv->GetGroundState();
                cc.isGrounded =
                    (gs == JPH::CharacterBase::EGroundState::OnGround);
                if (cc.isGrounded) cc.timeInAir = 0.f;
                else               cc.timeInAir += Impl::kFixedDt;

                const JPH::Vec3 gn = cv->GetGroundNormal();
                cc.groundNormal = { gn.GetX(), gn.GetY(), gn.GetZ() };

                const JPH::BodyID gbid = cv->GetGroundBodyID();
                if (!gbid.IsInvalid())
                {
                    JPH::BodyLockRead lock(m_impl->physics->GetBodyLockInterface(), gbid);
                    cc.groundEntity = lock.Succeeded()
                        ? BodyUserEntity(lock.GetBody().GetUserData())
                        : NullEntity;
                }
                else cc.groundEntity = NullEntity;

                // ---- Mode update + off-mesh-link landing ----
                // LaunchedTraversal latches until the CharacterVirtual touches ground; on landing flip to Walking
                // and raise launchFinished so the NavAgent advances the path corner. Otherwise the mode follows the
                // grounded state.
                if (launched)
                {
                    if (cc.isGrounded)
                    {
                        cc.mode = MovementMode::Walking;
                        cc.launchFinished = true;
                    }
                    // else: stay LaunchedTraversal, still in the air
                }
                else
                {
                    cc.mode = cc.isGrounded ? MovementMode::Walking
                                            : MovementMode::Falling;
                }

                // ---- Blocked-this-step report ----
                // Compares the desired horizontal displacement with the actual XZ displacement and fires only on a
                // hard pin (actual < 5% of requested): slopes, step-up frames and oblique wall slides all reduce
                // horizontal displacement legitimately, and a tighter threshold (25%) gave false stuck detections
                // that forced constant replans on uneven terrain.
                const JPH::Vec3 postPos = cv->GetPosition();

                // What the character really did over the step, and the ground it did it on.
                const bool onGroundNow = cc.isGrounded;
                const JPH::Vec3 groundVel = (onGroundNow && !KccLegacy()) ? cv->GetGroundVelocity() : JPH::Vec3::sZero();
                cc.groundVelocity = { groundVel.GetX(), groundVel.GetY(), groundVel.GetZ() };
                cc.velocity = FromJolt(KccLegacy()
                    ? cv->GetLinearVelocity()
                    : ActualVelocity(prePos, postPos, cv->GetLinearVelocity(), st.onGroundPre && onGroundNow, groundVel,
                                     Impl::kFixedDt));

                // Render-interpolation snapshot: bracket this step's [prePos, postPos] so PhysicsInterpApplySystem
                // lerps the capsule like the dynamic rigid bodies (~1 step in the past); a capsule at the raw step
                // position next to an interpolated pushed box makes pushing look janky at high frame rates. Both
                // ends are captured from the CharacterVirtual each step, so there is no first-frame glitch and no
                // prev <- curr shift.
                {
                    auto& kccSnap   = m_impl->poseSnapshots[e];
                    kccSnap.prevPos = prePos;
                    kccSnap.currPos = postPos;
                }

                const float dx = postPos.GetX() - prePos.GetX();
                const float dz = postPos.GetZ() - prePos.GetZ();
                const float actualSq  = dx * dx + dz * dz;
                const float desiredX  = cc.desiredHorizontalVelocity.x;
                const float desiredZ  = cc.desiredHorizontalVelocity.z;
                const float desiredSq = (desiredX * desiredX + desiredZ * desiredZ)
                                        * (Impl::kFixedDt * Impl::kFixedDt);
                constexpr float kBlockedThreshold = 0.05f * 0.05f; // 5% of intent
                if (desiredSq > 1e-6f && actualSq < desiredSq * kBlockedThreshold)
                {
                    cc.wasBlockedLastStep = true;
                    // Use -desired as a coarse blocked-surface normal: Jolt's CharacterBase does not expose the
                    // wall-contact normal, and the consumer's aligned-with-intent check is what matters anyway.
                    cc.blockedNormal = { -desiredX, 0.f, -desiredZ };
                    const float bn2 = cc.blockedNormal.x * cc.blockedNormal.x
                                    + cc.blockedNormal.z * cc.blockedNormal.z;
                    if (bn2 > 1e-6f)
                    {
                        const float inv = 1.f / std::sqrt(bn2);
                        cc.blockedNormal.x *= inv;
                        cc.blockedNormal.z *= inv;
                    }
                }
                else
                {
                    cc.wasBlockedLastStep = false;
                    cc.blockedNormal      = { 0.f, 0.f, 0.f };
                }

                #if KCC_DEBUG >= 1
                // ---- MAGIC-MOTION detector (unthrottled, edge-only) ----
                // Fires when the CharacterVirtual moved horizontally although the caller asked for (near-)zero
                // velocity. Catches:
                //   * penetration recovery pushing the character out of geometry
                //   * moving kinematic platforms dragging it along
                //   * gravity projected onto a slope giving a horizontal slide
                //   * something else writing LT.translation between steps (which would also show as a position
                //     jump)
                {
                    const float prePos_dx = postPos.GetX() - posPre.GetX();
                    const float prePos_dz = postPos.GetZ() - posPre.GetZ();
                    const float prePos_dy = postPos.GetY() - posPre.GetY();
                    const float prePos_dxz_sq = prePos_dx*prePos_dx + prePos_dz*prePos_dz;
                    const float reqSpeed_sq =
                        cc.desiredHorizontalVelocity.x * cc.desiredHorizontalVelocity.x +
                        cc.desiredHorizontalVelocity.z * cc.desiredHorizontalVelocity.z;
                    constexpr float kMotionThreshSq = 0.05f * 0.05f;  // 5 cm/step
                    constexpr float kZeroIntentSq   = 0.10f * 0.10f;  // |v|<10cm/s = "no intent"
                    if (prePos_dxz_sq > kMotionThreshSq && reqSpeed_sq < kZeroIntentSq)
                    {
                        LOG_WARNING("[KCC ent=%u] MAGIC MOTION: dXZ=%.3f dY=%+.3f "
                                    "(vDes=(%.3f,_,%.3f)≈0, mode=%d, grounded:%d→%d, groundEnt=%u)  "
                                    "→ pre=(%.3f,%.3f,%.3f) post=(%.3f,%.3f,%.3f)",
                                    static_cast<uint32_t>(e),
                                    std::sqrt(prePos_dxz_sq), prePos_dy,
                                    cc.desiredHorizontalVelocity.x,
                                    cc.desiredHorizontalVelocity.z,
                                    static_cast<int>(cc.mode),
                                    preWasGrounded ? 1 : 0,
                                    cc.isGrounded ? 1 : 0,
                                    static_cast<uint32_t>(cc.groundEntity),
                                    posPre.GetX(), posPre.GetY(), posPre.GetZ(),
                                    postPos.GetX(), postPos.GetY(), postPos.GetZ());
                    }
                }

                // ---- LT-DESYNC detector ----
                // Compares the CharacterVirtual's pre-step position with the LocalTransform written last frame:
                // more than 5 cm apart means something wrote LT between PostAllSteps (LT <- CV) and this StepOnce,
                // so an external system teleports the entity and the CharacterVirtual does not track it.
                //
                // First substep only: StepOnce runs up to 5 times per frame (App::RunFixedPhysicsLoop) but
                // PostAllSteps, the only LT writer, runs once after the loop, so on later substeps LT is stale by
                // construction. Reporting those flooded the log and made the warning total (an automated run's
                // verdict) worthless. Do not compare against a value stashed at write-back instead: that delta is
                // identically zero and would hide the real findings (see the fell-out-of-world check).
                // A declared teleport is not a desync: Character.Teleport (and anything else that moves a
                // CharacterController correctly) writes LT and calls MarkDirty, so a CharacterVirtual rebuild is
                // already scheduled and the pose is consistent again this frame.
                const bool rebuildPending = (cc.generation != cc.lastBuiltGeneration);
                if (firstSubstepThisFrame && !rebuildPending)
                {
                    if (const LocalTransform* ltCheck = world.GetComponent<LocalTransform>(e))
                    {
                        const float ldx = ltCheck->translation.x - posPre.GetX();
                        const float ldy = ltCheck->translation.y - posPre.GetY();
                        const float ldz = ltCheck->translation.z - posPre.GetZ();
                        if (ldx*ldx + ldy*ldy + ldz*ldz > 0.05f * 0.05f)
                        {
                            LOG_WARNING("[KCC ent=%u] LT/CV DESYNC at step start: "
                                        "LT=(%.3f,%.3f,%.3f) CV=(%.3f,%.3f,%.3f) "
                                        "(someone wrote LT between physics frames)",
                                        static_cast<uint32_t>(e),
                                        ltCheck->translation.x, ltCheck->translation.y, ltCheck->translation.z,
                                        posPre.GetX(), posPre.GetY(), posPre.GetZ());
                        }
                    }
                }

                // ---- Fell out of the world ----
                // A KCC spawned without ground under it free-falls at maxFallSpeed forever (there is no kill-Z);
                // warn once when timeInAir crosses the threshold. Stateless edge: timeInAir is maintained per step,
                // so the crossing needs no latch, static map or new component field. (A function-local static map
                // keyed by Entity would be wrong anyway: entity ids are recycled, so a fresh agent would inherit a
                // dead one's state.)
                {
                    constexpr float kFellOutOfWorldSec = 5.f;
                    const float prevAir = cc.timeInAir - Impl::kFixedDt;
                    if (cc.timeInAir >= kFellOutOfWorldSec && prevAir < kFellOutOfWorldSec)
                    {
                        LOG_WARNING("[KCC ent=%u] has been airborne for %.1fs at (%.2f,%.2f,%.2f) — "
                                    "no ground beneath it. Check the spawn placement against the "
                                    "collision geometry; there is no kill-Z, so it will keep falling.",
                                    static_cast<uint32_t>(e), cc.timeInAir,
                                    cv->GetPosition().GetX(), cv->GetPosition().GetY(),
                                    cv->GetPosition().GetZ());
                    }
                }

                // Throttled snapshot: pre-step intent + post-step result. The most useful line for "I asked the
                // agent to walk and it did not move", and the biggest source of log spam at scale, since it is per
                // character per fire: tier 2 (see KCC_DEBUG).
                #if KCC_DEBUG >= 2
                if (kccLogFire)
                {
                    LOG_INFO("[KCC ent=%u] in: vDes=(%.2f,_,%.2f) vyIn=%.2f preGround=%d mode=%d  "
                             "out: dXZ=%.4f vel=(%.2f,%.2f,%.2f) grounded=%d blocked=%d pos=(%.2f,%.2f,%.2f)",
                             static_cast<uint32_t>(e),
                             cc.desiredHorizontalVelocity.x, cc.desiredHorizontalVelocity.z,
                             lvPre.GetY(),
                             (gsPre == JPH::CharacterBase::EGroundState::OnGround) ? 1 : 0,
                             static_cast<int>(cc.mode),
                             std::sqrt(actualSq),
                             cc.velocity.x, cc.velocity.y, cc.velocity.z,
                             cc.isGrounded ? 1 : 0,
                             cc.wasBlockedLastStep ? 1 : 0,
                             postPos.GetX(), postPos.GetY(), postPos.GetZ());
                }
                #endif
                // Edge: ground state transition (independent of throttle).
                if (preWasGrounded != cc.isGrounded)
                {
                    LOG_INFO("[KCC ent=%u] ground %s (vel=(%.2f,%.2f,%.2f) timeInAir=%.2f)",
                             static_cast<uint32_t>(e),
                             cc.isGrounded ? "LANDED" : "AIRBORNE",
                             cc.velocity.x, cc.velocity.y, cc.velocity.z,
                             cc.timeInAir);
                }
                // Edge: the blocked rising edge, logged only when it first trips (a character walking into a wall
                // would spam).
                static std::unordered_map<Entity, bool> s_kccPrevBlocked;
                bool& prevBlock = s_kccPrevBlocked[e];
                if (!prevBlock && cc.wasBlockedLastStep)
                {
                    LOG_INFO("[KCC ent=%u] BLOCKED: desired=(%.2f,_,%.2f) actualXZ=%.4f normal=(%.2f,_,%.2f)",
                             static_cast<uint32_t>(e),
                             cc.desiredHorizontalVelocity.x, cc.desiredHorizontalVelocity.z,
                             std::sqrt(actualSq),
                             cc.blockedNormal.x, cc.blockedNormal.z);
                }
                prevBlock = cc.wasBlockedLastStep;
                #endif
            }
        }

        // The Jolt solve, zoned apart from the engine-side bookkeeping around it: the two have different
        // fixes.
        {
            ZoneScopedN("Phys.JoltStep");
            PROF_SCOPE("Phys.JoltStep");
            m_impl->contactTracker->BeginStep();
            const JPH::EPhysicsUpdateError err = m_impl->physics->Update(
                Impl::kFixedDt, Impl::kCollisionSteps,
                m_impl->tempAlloc.get(), m_impl->jobSys.get());
            Diag::Prof::SetGauge(Diag::Prof::Gauge::JoltStepPeakKB,
                                 static_cast<double>(m_impl->tempAlloc->GetPeakBytes()) / 1024.0);

            // Jolt reports a full manifold / body-pair / contact-constraint cache only by return value and
            // carries on with contacts dropped: on screen objects jitter, sink or pass through, with nothing in
            // the log naming a cap. Report each bit once per run, naming the constant it comes from
            // (Impl::kMaxContacts / kMaxBodyPairs).
            if (err != JPH::EPhysicsUpdateError::None)
            {
                using E = JPH::EPhysicsUpdateError;
                if ((err & E::ManifoldCacheFull) != E::None)
                    ENGINE_CHECK_ONCE(false,
                        "Jolt manifold cache full (Impl::kMaxContacts = %u) — contacts "
                        "dropped this step; expect jitter/sinking. Raise the constant.",
                        static_cast<unsigned>(Impl::kMaxContacts));
                if ((err & E::BodyPairCacheFull) != E::None)
                    ENGINE_CHECK_ONCE(false,
                        "Jolt body-pair cache full (Impl::kMaxBodyPairs = %u) — contact "
                        "warm-start lost this step. Raise the constant.",
                        static_cast<unsigned>(Impl::kMaxBodyPairs));
                if ((err & E::ContactConstraintsFull) != E::None)
                    ENGINE_CHECK_ONCE(false,
                        "Jolt contact-constraint buffer full (Impl::kMaxContacts = %u) — "
                        "contacts not solved this step. Raise the constant.",
                        static_cast<unsigned>(Impl::kMaxContacts));
            }
        }

        // After the step: capture the new pose as "current".
        for (size_t i = 0; i < n; ++i)
        {
            const RigidBodyComponent& rb = rbData[i];
            if (rb.bodyId == kInvalidPhysicsBodyId) continue;
            if (rb.motion != RigidBodyComponent::Motion::Dynamic) continue;
            auto& snap = m_impl->poseSnapshots[rbEntities[i]];
            const JPH::BodyID id(rb.bodyId);
            snap.currPos = bodyIface.GetPosition(id);
            snap.currRot = bodyIface.GetRotation(id);
        }

    }
}

// ==== PostAllSteps ====
void PhysicsSystem::PostAllSteps(World& world)
{
    if (!m_impl) return;

    // Contact drain + the per-KCC CollideShape sensor sweep: O(bodies) / O(KCCs) every frame.
    ZoneScopedN("Phys.PostAllSteps");

    JPH::BodyInterface& bodyIface = m_impl->physics->GetBodyInterface();
    auto* rbPool = world.EnsurePool<RigidBodyComponent>();
    const auto& rbEntities = rbPool->Entities();
    auto&       rbData     = rbPool->Data();
    const size_t n = rbData.size();

    // The frame's contact events, published once at the end of the contact work: the rigid bodies'
    // (what the workers buffered during the steps), then the character sweep's below, as one queue
    // sorted by (step, entity pair). Published on the main thread: the EventBus is not thread-safe.
    ContactTracker&                     tracker = *m_impl->contactTracker;
    std::vector<ContactTracker::Event>& events  = m_impl->contactScratch;
    events.clear();
    tracker.Drain([&bodyIface](std::uint32_t bodyId)
    {
        // A dynamic body that is asleep: its contacts are gone but it still touches what it rests on.
        const JPH::BodyID id(bodyId);
        return bodyIface.IsAdded(id) && bodyIface.GetMotionType(id) == JPH::EMotionType::Dynamic &&
               !bodyIface.IsActive(id);
    }, events);
    // A Began needs both entities. An Ended needs one: a destroyed body's end reaches the entity left
    // inside the trigger (the dead side publishes as a null handle). The per-sub-shape behaviour drops
    // both.
    events.erase(std::remove_if(events.begin(), events.end(), [&](const ContactTracker::Event& c)
    {
        const bool aliveA = world.IsAlive(c.a), aliveB = world.IsAlive(c.b);
        return (c.began || tracker.Legacy()) ? !(aliveA && aliveB) : !(aliveA || aliveB);
    }), events.end());
    const std::uint32_t lastStep = tracker.Step();
    auto addCharacterEvent = [&](Entity a, Entity b, bool began, const DirectX::XMFLOAT3& point,
                                 const DirectX::XMFLOAT3& normal)
    {
        ContactTracker::Event e;
        e.step = lastStep; e.a = a; e.b = b; e.began = began; e.point = point; e.normal = normal;
        events.push_back(e);
    };

    // ---- KCC vs sensor overlap -> synthesized ContactBegan / ContactEnded ----
    // A CharacterVirtual bypasses the rigid-body ContactListener, and Jolt's CharacterVirtual contact
    // callbacks use a move-direction heuristic that can drop a notification while overlapping. So
    // triggers are made character-aware here: one CollideShape query per KCC per render frame against
    // the layers the CharacterVirtual collides with, keeping only sensor bodies, diffed against the
    // previous frame's set. The events take the same ContactBeganEvent / ContactEndedEvent path as
    // rigid bodies, so the ScriptSystem OnEnter / OnExit bridge is shared. Order: bodyA = the trigger
    // volume, bodyB = the character.
    if (!m_impl->kccTable.empty())
    {
        const JPH::NarrowPhaseQuery& npq = m_impl->physics->GetNarrowPhaseQuery();
        JPH::DefaultBroadPhaseLayerFilter bpFilter(*m_impl->objVsBpFilter, Layers::CHARACTER);
        JPH::DefaultObjectLayerFilter    objFilter(*m_impl->objLayerPairFilter, Layers::CHARACTER);

        struct FirstHit { DirectX::XMFLOAT3 point; DirectX::XMFLOAT3 normal; };

        // The query exists only to find sensors: reject solid bodies while Jolt holds the body read lock,
        // before TransformedShape runs narrow-phase collision (expensive on static mesh floors). The layer
        // filters still apply, and collectSensors re-checks every hit.
        struct SensorBodyFilter final : JPH::BodyFilter
        {
            bool ShouldCollideLocked(const JPH::Body& body) const override
            { return body.IsSensor(); }
        } sensorFilter;
        JPH::BodyFilter allBodies;
        bool baseline = false, verify = false;
#if WITH_HARNESS
        static const bool s_baseline = Sys::CommandLine::Get().Has("-kccsensorbaseline");
        static const bool s_verify = Sys::CommandLine::Get().Has("-verifykccsensors");
        baseline = s_baseline;
        verify = s_verify;
#endif
        static bool s_reported = false;
        if (!s_reported)
        {
            LOG_INFO("[KCCSENSOR] query=%s verify=%s", baseline ? "legacy" : "sensor-only",
                     verify ? "on" : "off");
            s_reported = true;
        }
        const JPH::BodyFilter& queryFilter = baseline
            ? allBodies : static_cast<const JPH::BodyFilter&>(sensorFilter);
        uint32_t verifiedQueries = 0, verifiedContacts = 0;

        // The query and the optional reference audit (-verifykccsensors) share these acceptance / dedup
        // rules. Nothing mutates the World or delivers events until the comparison is done.
        auto collectSensors = [&](const auto& hits, Entity ent, auto& current, auto& hitInfo)
        {
            for (const JPH::CollideShapeResult& hit : hits)
            {
                JPH::BodyLockRead lock(m_impl->physics->GetBodyLockInterface(), hit.mBodyID2);
                if (!lock.Succeeded()) continue;
                const JPH::Body& body = lock.GetBody();
                if (!body.IsSensor()) continue;
                const Entity sensorEnt = BodyUserEntity(body.GetUserData());
                if (sensorEnt == NullEntity || sensorEnt == ent
                    || !world.IsAlive(sensorEnt)) continue;
                if (!current.insert(sensorEnt).second) continue;
                const JPH::Vec3 normal = -hit.mPenetrationAxis.NormalizedOr(JPH::Vec3::sZero());
                hitInfo[sensorEnt] = FirstHit{
                    { hit.mContactPointOn2.GetX(), hit.mContactPointOn2.GetY(), hit.mContactPointOn2.GetZ() },
                    { normal.GetX(), normal.GetY(), normal.GetZ() } };
            }
        };

        for (auto& kv : m_impl->kccTable)
        {
            const Entity ent = KccKeyEntity(kv.first);
            const JPH::Ref<JPH::CharacterVirtual>& cv = kv.second;
            if (!CharacterKeyLive(world, kv.first) || cv == nullptr) continue;

            JPH::CollideShapeSettings settings;
            JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
            {
                ZoneScopedN("Phys.KCCSensors.Query");
                npq.CollideShape(cv->GetShape(), JPH::Vec3::sReplicate(1.0f),
                                 cv->GetCenterOfMassTransform(),   // includes mShapeOffset
                                 settings, JPH::RVec3::sZero(), collector,
                                 bpFilter, objFilter, queryFilter);
            }

            std::unordered_set<Entity>           current;
            std::unordered_map<Entity, FirstHit> hitInfo;
            collectSensors(collector.mHits, ent, current, hitInfo);
            if (verify)
            {
                ZoneScopedN("Phys.KCCSensors.ReferenceAudit");
                JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> reference;
                // Always compare against the other filter, also under -kccsensorbaseline.
                const JPH::BodyFilter& otherFilter = baseline
                    ? static_cast<const JPH::BodyFilter&>(sensorFilter) : allBodies;
                npq.CollideShape(cv->GetShape(), JPH::Vec3::sReplicate(1.0f),
                                 cv->GetCenterOfMassTransform(), settings, JPH::RVec3::sZero(),
                                 reference, bpFilter, objFilter, otherFilter);
                std::unordered_set<Entity> expected;
                std::unordered_map<Entity, FirstHit> expectedInfo;
                collectSensors(reference.mHits, ent, expected, expectedInfo);
                bool same = current == expected;
                auto nearVector = [](const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b) {
                    return std::abs(a.x - b.x) <= 1e-5f && std::abs(a.y - b.y) <= 1e-5f
                        && std::abs(a.z - b.z) <= 1e-5f;
                };
                if (same)
                    for (const auto& [sensor, hit] : hitInfo)
                    {
                        const FirstHit& ref = expectedInfo.at(sensor);
                        same = same && nearVector(hit.point, ref.point) && nearVector(hit.normal, ref.normal);
                    }
                ENGINE_CHECK(same, "KCC sensor filter changed contacts for entity %u", ent);
                ++verifiedQueries;
                verifiedContacts += static_cast<uint32_t>(current.size());
            }

            std::unordered_set<Entity>& prev = m_impl->kccSensorTouches[kv.first];
            for (const Entity s : current)
                if (!prev.count(s))
                {
                    const FirstHit& h = hitInfo[s];
                    addCharacterEvent(s, ent, true, h.point, h.normal);
                }
            for (const Entity s : prev)
                if (!current.count(s) && world.IsAlive(s))
                    addCharacterEvent(s, ent, false, {}, {});
            prev = std::move(current);
        }
        if (verify)
        {
            // Deliberately only in audit runs; ordinary frames pay no logging cost.
            LOG_INFO("[KCCSENSOR_AUDIT] queries=%u contacts=%u", verifiedQueries, verifiedContacts);
        }

    }

    // Reap the overlap state of KCCs torn down this frame (also when the last KCC just went and
    // kccTable is empty). Ended fires for every still-alive sensor whether or not the character
    // survives: gating on the character would skip the exit edge in exactly the "occupant despawned
    // inside the zone" case and latch the trigger's occupancy forever. A dead character resolves to 0
    // in the Lua callback.
    for (auto it = m_impl->kccSensorTouches.begin();
         it != m_impl->kccSensorTouches.end(); )
    {
        if (m_impl->kccTable.count(it->first)) { ++it; continue; }
        for (const Entity s : it->second)
            if (world.IsAlive(s))
                addCharacterEvent(s, KccKeyEntity(it->first), false, {}, {});
        it = m_impl->kccSensorTouches.erase(it);
    }

    // Every Began before any Ended, each by (step, entity pair): a trigger that hands a body over in one frame
    // never reads empty in between. The bus delivers in publish order.
    std::stable_sort(events.begin(), events.end(), [](const ContactTracker::Event& x, const ContactTracker::Event& y)
    {
        return std::make_tuple(x.began ? 0 : 1, x.step, x.a, x.b) < std::make_tuple(y.began ? 0 : 1, y.step, y.a, y.b);
    });
    for (const ContactTracker::Event& c : events)
    {
        if (c.began)
            EventBus::Get().Publish(ContactBeganEvent{
                world.MakeHandle(c.a), world.MakeHandle(c.b), c.point, c.normal });
        else
            EventBus::Get().Publish(ContactEndedEvent{
                world.MakeHandle(c.a), world.MakeHandle(c.b) });
    }
    events.clear();

    // Phase 4 - write Jolt's instantaneous pose back into LocalTransform. Keeping LT == Jolt's current
    // pose lets Phase 2's teleport detection tell an Inspector drag (LT diverges from Jolt) from
    // simulation (LT == Jolt). Render interpolation runs after TransformSystem::Propagate in
    // ApplyRenderInterpolation and overwrites only GlobalTransform, so it never feeds back into Jolt.
    for (size_t i = 0; i < n; ++i)
    {
        RigidBodyComponent& rb = rbData[i];
        if (rb.bodyId == kInvalidPhysicsBodyId)                 continue;
        if (rb.motion != RigidBodyComponent::Motion::Dynamic)   continue;

        LocalTransform* lt = world.GetComponent<LocalTransform>(rbEntities[i]);
        if (!lt) continue;

        const JPH::BodyID id(rb.bodyId);
        lt->translation = FromJolt(bodyIface.GetPosition(id));
        lt->rotation    = FromJolt(bodyIface.GetRotation(id));
    }

    // Phase 4b - write the KCC position back to LocalTransform. Same contract as RigidBody: LT stays
    // in sync with the physics truth, so a teleport through LocalTransform shows as LT-DESYNC unless it
    // calls MarkDirty, which re-seeds the CharacterVirtual. Rotation is not written: CharacterVirtual
    // does not rotate the capsule, and the animation drivers own the entity's facing.
    if (auto* ccPool = world.GetPool<CharacterControllerComponent>())
    {
        const auto& ccEntities = ccPool->Entities();
        const size_t ncc = ccPool->Data().size();

        #if KCC_DEBUG >= 1
        static int s_writebackCounter = 0;
        ++s_writebackCounter;
        const bool writebackLogFire = (s_writebackCounter % 60) == 0;
        #endif

        for (size_t i = 0; i < ncc; ++i)
        {
            const Entity e = ccEntities[i];
            auto cvIt = m_impl->kccTable.find(CharacterKeyOf(world, e));
            if (cvIt == m_impl->kccTable.end()) {
                #if KCC_DEBUG >= 1
                if (writebackLogFire)
                    LOG_WARNING("[KCC ent=%u] PostAllSteps writeback: NO CV in kccTable",
                                static_cast<uint32_t>(e));
                #endif
                continue;
            }

            LocalTransform* lt = world.GetComponent<LocalTransform>(e);
            if (!lt) {
                #if KCC_DEBUG >= 1
                if (writebackLogFire)
                    LOG_WARNING("[KCC ent=%u] PostAllSteps writeback: NO LocalTransform",
                                static_cast<uint32_t>(e));
                #endif
                continue;
            }

            #if KCC_DEBUG >= 1
            const DirectX::XMFLOAT3 ltBefore = lt->translation;
            #endif

            const JPH::Vec3 p = cvIt->second->GetPosition();
            lt->translation = { p.GetX(), p.GetY(), p.GetZ() };

            // Tier 2: per character per fire, so it scales with population like the vDes snapshot. The NO-CV /
            // NO-LocalTransform warnings stay at tier 1: they report real breakage and are silent in a healthy
            // scene.
            #if KCC_DEBUG >= 2
            if (writebackLogFire)
            {
                LOG_INFO("[KCC ent=%u] PostAllSteps writeback: LT %.3f,%.3f,%.3f → %.3f,%.3f,%.3f",
                         static_cast<uint32_t>(e),
                         ltBefore.x, ltBefore.y, ltBefore.z,
                         lt->translation.x, lt->translation.y, lt->translation.z);
            }
            #endif
        }
    }

    // The events stay queued: Gameplay/EventDelivery drains the bus at the end of BoneAttachment (this
    // frame) in publish order with every other C++ event, outside the physics phase.
    if (SyncContactDispatchLegacy())
    {
        EventBus::Get().DispatchOne<ContactBeganEvent>();
        EventBus::Get().DispatchOne<ContactEndedEvent>();
    }
}

void PhysicsSystem::ApplyRenderInterpolation(World& world, float renderAlpha)
{
    if (!m_impl) return;
    // renderAlpha is the caller's accumulator / fixedDt (FrameContext::physicsAlpha). Clamped so a
    // stale value cannot slerp outside the [prev, curr] bracket.
    const float alpha = std::clamp(renderAlpha, 0.0f, 1.0f);

    auto* rbPool = world.GetPool<RigidBodyComponent>();
    if (!rbPool) return;
    auto& rbEntities = rbPool->Entities();
    auto& rbData     = rbPool->Data();
    const size_t n   = rbData.size();

    for (size_t i = 0; i < n; ++i)
    {
        const RigidBodyComponent& rb = rbData[i];
        if (rb.bodyId == kInvalidPhysicsBodyId)                 continue;
        if (rb.motion != RigidBodyComponent::Motion::Dynamic)   continue;

        auto snapIt = m_impl->poseSnapshots.find(rbEntities[i]);
        if (snapIt == m_impl->poseSnapshots.end()) continue;
        const auto& snap = snapIt->second;

        // Interpolated WORLD pose (Jolt operates in world space).
        const JPH::Vec3 interpPos = snap.prevPos + (snap.currPos - snap.prevPos) * alpha;
        const JPH::Quat interpRot = snap.prevRot.SLERP(snap.currRot, alpha);

        GlobalTransform* gt = world.GetComponent<GlobalTransform>(rbEntities[i]);
        if (!gt) continue;

        // Jolt doesn't carry scale; preserve whatever TransformSystem just
        // composed from LocalTransform.scale.
        const LocalTransform* lt = world.GetComponent<LocalTransform>(rbEntities[i]);
        const DirectX::XMFLOAT3 scl =
            lt ? lt->scale : DirectX::XMFLOAT3{ 1.f, 1.f, 1.f };

        using namespace DirectX;
        const XMFLOAT4 rotF = FromJolt(interpRot);
        const XMFLOAT3 posF = FromJolt(interpPos);
        const XMMATRIX S = XMMatrixScalingFromVector(XMLoadFloat3(&scl));
        const XMMATRIX R = XMMatrixRotationQuaternion(XMLoadFloat4(&rotF));
        const XMMATRIX T = XMMatrixTranslationFromVector(XMLoadFloat3(&posF));
        XMStoreFloat4x4(&gt->matrix, S * R * T);

        // Push the interpolated pose to the children. This frame's Propagate ran before and composed
        // their GlobalTransform from the pre-interpolation pose; without re-propagating, a child mesh under
        // this body renders one physics step behind -> jitter (worse up close).
        TransformSystem::PropagateSubtree(world, rbEntities[i]);
    }

    // ---- CharacterController (KCC): interpolate the position only ----
    // The capsule's facing + scale live in LocalTransform (animation / AI own the rotation; the KCC
    // never rotates the capsule), and TransformSystem composed them into gt->matrix. Only the
    // translation row is replaced with the interpolated physics position, so the character renders
    // with the same ~1-step latency as the dynamic bodies it pushes.
    if (auto* ccPool = world.GetPool<CharacterControllerComponent>())
    {
        const auto& ccEnts = ccPool->Entities();
        const size_t ncc   = ccPool->Data().size();
        for (size_t i = 0; i < ncc; ++i)
        {
            const Entity e = ccEnts[i];
            auto snapIt = m_impl->poseSnapshots.find(e);
            if (snapIt == m_impl->poseSnapshots.end()) continue;
            GlobalTransform* gt = world.GetComponent<GlobalTransform>(e);
            if (!gt) continue;

            const auto& snap = snapIt->second;
            const JPH::Vec3 ip = snap.prevPos + (snap.currPos - snap.prevPos) * alpha;
            // Overwrite only the translation row (row-major _4x); keep the
            // rotation/scale TransformSystem composed from LocalTransform.
            gt->matrix._41 = ip.GetX();
            gt->matrix._42 = ip.GetY();
            gt->matrix._43 = ip.GetZ();

            // Push the interpolated capsule pose to the children: the visible (often skinned) character mesh
            // is usually parented under the capsule, and would otherwise track the non-interpolated pose and
            // jitter against the camera, which follows the interpolated capsule (the "third-person camera
            // shakes up close" symptom).
            TransformSystem::PropagateSubtree(world, e);
        }
    }
}

} // namespace DX12Physics
