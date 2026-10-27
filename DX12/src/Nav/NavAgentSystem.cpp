#include "Nav/NavAgentSystem.h"
#include "System/TaskSystem.h"   // ParallelFor over agents
#include <atomic>
#include "System/Check.h"   // ENGINE_CHECK_ONCE on the structural-change guard
#include "Nav/NavMeshSystem.h"
#include "Nav/NavComponents.h"

#include "ECS/ECS.h"
#include "ECS/HierarchyComponents.h"           // LocalTransform, GlobalTransform
#include "ECS/CharacterControllerComponent.h"  // CCC.wasBlockedLastStep / mode
#include "ECS/CharacterIntentComponent.h"      // unified intent + BrainComponent

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>   // NAV_DIAG varargs
#include <map>       // NAV_DIAG per-(entity,branch) visit counter
#include "AI/AIProfile.h"
#include "ECS/FrameContext.h"
#include <chrono>
#include <utility>

namespace Nav
{

namespace {
// Set while NavAgentTick is fanned across workers. AddComponent is a STRUCTURAL change that can
// reallocate the pool under every other worker, so creating an intent inside the parallel region is
// a hard error, not a slow path; the serial pre-pass in NavAgentTick creates them first.
inline std::atomic<bool> g_navTickParallel{ false };

inline float Dist2XZ(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b)
{
    const float dx = a.x - b.x, dz = a.z - b.z;
    return dx*dx + dz*dz;
}

// Quaternion rotating +Z onto the unit XZ-direction of `dir`. Engine
// convention (mesh-forward = +Z) matches Hierarchy widgets and view matrices.
DirectX::XMFLOAT4 LookRotationFromDir(float dirX, float dirZ)
{
    using namespace DirectX;
    const float lenXZ = std::sqrt(dirX * dirX + dirZ * dirZ);
    if (lenXZ < 1e-5f)
        return { 0.f, 0.f, 0.f, 1.f };
    const float yaw = std::atan2(dirX, dirZ);
    XMVECTOR q = XMQuaternionRotationRollPitchYaw(0.f, yaw, 0.f);
    XMFLOAT4 out;
    XMStoreFloat4(&out, q);
    return out;
}

DirectX::XMFLOAT4 NLerpQuat(const DirectX::XMFLOAT4& a,
                            const DirectX::XMFLOAT4& b,
                            float t)
{
    using namespace DirectX;
    XMVECTOR va = XMLoadFloat4(&a);
    XMVECTOR vb = XMLoadFloat4(&b);
    if (XMVectorGetX(XMVector4Dot(va, vb)) < 0.0f) vb = XMVectorNegate(vb);
    XMVECTOR mixed = XMVectorAdd(XMVectorScale(va, 1.f - t), XMVectorScale(vb, t));
    mixed = XMQuaternionNormalize(mixed);
    XMFLOAT4 out;
    XMStoreFloat4(&out, mixed);
    return out;
}

// Resolve the world-space point this agent should face, given its
// facingMode + lookTarget + facingTarget. Returns false if "face nothing"
// (Manual mode, or FaceTarget without a valid lookTarget/facingTarget).
bool ResolveFacingPoint(const ::World& world,
                       const NavAgentComponent& agent,
                       DirectX::XMFLOAT3& outPoint)
{
    switch (agent.facingMode)
    {
        case NavFacingMode::Manual:
            return false;

        case NavFacingMode::FaceTarget:
            if (agent.useLookAt)
            {
                outPoint = agent.lookTarget;
                return true;
            }
            if (agent.facingTarget != NullEntity)
            {
                if (const auto* gt = world.GetComponent<GlobalTransform>(agent.facingTarget))
                {
                    using namespace DirectX;
                    const XMMATRIX m = XMLoadFloat4x4(&gt->matrix);
                    outPoint = { XMVectorGetX(m.r[3]),
                                 XMVectorGetY(m.r[3]),
                                 XMVectorGetZ(m.r[3]) };
                    return true;
                }
            }
            return false;

        case NavFacingMode::FaceMovement:
        default:
            // The caller decides the steering direction: return false so its per-frame steering vector wins.
            return false;
    }
}

CharacterIntentComponent& GetOrCreateIntent(::World& world, Entity e)
{
    if (auto* it = world.GetComponent<CharacterIntentComponent>(e)) return *it;

    ENGINE_CHECK_ONCE(!g_navTickParallel.load(std::memory_order_relaxed),
        "NavAgentTick created a CharacterIntentComponent for entity %u INSIDE the "
        "parallel region. AddComponent can reallocate the pool under the other "
        "workers. The serial pre-pass in NavAgentTick is supposed to have created "
        "every intent already — something reached this path that it does not cover.",
        static_cast<uint32_t>(e));

    world.EnsurePool<CharacterIntentComponent>();
    world.AddComponent<CharacterIntentComponent>(e, {});
    return *world.GetComponent<CharacterIntentComponent>(e);
}

} // namespace


// NAV_DIAG - periodic per-(entity, branch) trace of the steering state machine. Off by default: set
// it to 1 when an agent will not move; it prints the branch each agent took and the state it took it
// from, every 120th visit.
//
// Every failure in the CCC path has the same symptom (intent zeroed, the KCC logs vDes=(0,0,0), the
// character stands still): no destination, path query failed, arrived and possession denied look
// alike from outside. Keep it periodic, not one-shot: a first-occurrence log shows how a state
// STARTED, not what it settled into (an agent that arrives and then has its destination replaced by
// its behaviour tree reads as "steering works"). Compiled out because at 500 agents the periodic
// print is real log volume.
#define NAV_DIAG_ENABLED 0
#if NAV_DIAG_ENABLED
namespace {
    void NavDiagOnce(Entity e, const char* branch, const char* fmt = nullptr, ...)
    {
        // Every 120th visit per (entity, branch): a branch's OUTPUT can decay after its first visit (a
        // steering tick with speedScale 0 is still a steering tick).
        static std::map<std::pair<uint32_t, const char*>, uint32_t> s_hits;
        if (s_hits[{ static_cast<uint32_t>(e), branch }]++ % 120u != 0u) return;
        char extra[256] = {};
        if (fmt)
        {
            va_list ap; va_start(ap, fmt);
            vsnprintf(extra, sizeof(extra), fmt, ap);
            va_end(ap);
        }
        LOG_INFO("NavAgent[ent=%u] -> %s %s", static_cast<uint32_t>(e), branch, extra);
    }
}
  #define NAV_DIAG(e, branch, ...) NavDiagOnce((e), (branch), __VA_ARGS__)
#else
  #define NAV_DIAG(e, branch, ...) ((void)0)
#endif

void NavAgentTick(::World& world, NavMeshSystem& nav, float dt)
{
    // Its own zone because nothing here is throttled: it runs every frame for every agent, and
    // AILODSystem only rewrites AIComponent::tickInterval, which this system never reads.
    ZoneScopedN("Nav.AgentTick");

    if (dt <= 0.f) return;
    const bool navReady = nav.IsReady();

    std::atomic<uint32_t> agentCount{0}, pathQueries{0}, pathFailures{0};

    // PARALLEL ACROSS AGENTS. Safe because:
    //   * every agent writes only its OWN NavAgentComponent, LocalTransform, CharacterIntentComponent and
    //     CharacterControllerComponent (BrainComponent and other entities' transforms are read only);
    //   * the loop performs NO structural change (no Add/RemoveComponent, no entity create/destroy): the
    //     serial pre-pass below creates the intents, and g_navTickParallel turns a miss into an error;
    //   * NavMeshSystem::FindPath uses a PER-THREAD dtNavMeshQuery (NavMeshSystem::ThreadQuery): a Detour
    //     query carries mutable search state and cannot be shared.
    // Path queries are the cost, not the follow maths.
    auto* navPool = world.GetPool<NavAgentComponent>();
    const size_t agentN = navPool ? navPool->Data().size() : 0u;
    if (agentN == 0) return;
    const std::vector<Entity>&        navEnts = navPool->Entities();
    std::vector<NavAgentComponent>&   navData = navPool->Data();

    auto tickOne = [&](uint32_t idx)
    {
        const Entity e = navEnts[idx];
        NavAgentComponent& agent = navData[idx];
        {
        agentCount.fetch_add(1, std::memory_order_relaxed);
        LocalTransform* lt = world.GetComponent<LocalTransform>(e);
        // Unconditional state snapshot BEFORE any early-out, so a branch that returns without a NAV_DIAG of
        // its own still shows up.
        NAV_DIAG(e, "state", "lt=%d hasDest=%d dest=(%.2f,%.2f) path=%d next=%d st=%d",
                 lt ? 1 : 0, agent.hasDestination ? 1 : 0,
                 agent.destination.x, agent.destination.z,
                 (int)agent.path.size(), (int)agent.nextWaypoint, (int)agent.state);
        if (!lt) return;

        // Possession gate: another brain drives this entity (the player took over, a cutscene froze it).
        // Pathing state stays parked.
        if (!BrainAllows(world.GetComponent<BrainComponent>(e), BrainType::AI))
        {
            NAV_DIAG(e, "possession-denied", nullptr);
            return;
        }

        // Motor selection: with a CCC the entity is Motor-driven - this system is its AI BRAIN and writes
        // CharacterIntentComponent (CharacterMotorSystem turns intent -> CCC -> KCC). Without a CCC (e.g. a
        // kinematic RigidBody enemy) it writes LocalTransform directly.
        CharacterControllerComponent* cc = world.GetComponent<CharacterControllerComponent>(e);

        // ---- Off-mesh-link handshake (CCC only; the no-CCC path has no mode) ----
        if (cc && cc->mode == MovementMode::LaunchedTraversal)
        {
            agent.state = NavAgentState::TraversingOffMeshLink;
            if (cc->launchFinished)
            {
                cc->launchFinished = false;
                if (agent.nextWaypoint < agent.path.size())
                    ++agent.nextWaypoint;
                agent.state = NavAgentState::Following;
            }
            return;
        }

        // ---- Facing helpers ----
        // The no-CCC path applies rotation directly.
        auto ApplyFacingTowards = [&](float fx, float fz) {
            const DirectX::XMFLOAT4 want = LookRotationFromDir(fx, fz);
            const float t = std::min(1.f, agent.turnRate * dt);
            lt->rotation = NLerpQuat(lt->rotation, want, t);
        };
        auto ApplyResolvedFacing = [&]() -> bool {
            DirectX::XMFLOAT3 pt;
            if (!ResolveFacingPoint(world, agent, pt)) return false;
            const float fx = pt.x - lt->translation.x;
            const float fz = pt.z - lt->translation.z;
            const float lenSq = fx * fx + fz * fz;
            if (lenSq < 1e-6f) return true;
            const float inv = 1.f / std::sqrt(lenSq);
            ApplyFacingTowards(fx * inv, fz * inv);
            return true;
        };

        // CCC path: express facing as INTENT. Mirrors ApplyResolvedFacing: a resolved look point becomes
        // faceDir with useFaceDir = true; otherwise useFaceDir = false and the Motor's face-movement fallback
        // (gated on rotateToFacing + FaceMovement) takes over.
        auto WriteResolvedFacingIntent = [&](CharacterIntentComponent& it) {
            DirectX::XMFLOAT3 pt;
            if (ResolveFacingPoint(world, agent, pt))
            {
                const float fx = pt.x - lt->translation.x;
                const float fz = pt.z - lt->translation.z;
                if (fx * fx + fz * fz > 1e-6f)
                {
                    it.faceDir    = { fx, 0.f, fz };
                    it.useFaceDir = true;
                    return;
                }
            }
            it.useFaceDir = false;
        };

        // ---- Idle / nav not ready ----
        const bool wantMove = agent.hasDestination && navReady;
        if (!wantMove)
        {
            NAV_DIAG(e, "idle", "hasDest=%d navReady=%d", agent.hasDestination ? 1 : 0, navReady ? 1 : 0);
            if (cc)
            {
                auto& it = GetOrCreateIntent(world, e);
                it.moveDir = { 0.f, 0.f, 0.f };
                it.desiredSpeed = 0.f;
                WriteResolvedFacingIntent(it);
            }
            else
            {
                // No-CCC path: translation is not written, so it stays put.
                ApplyResolvedFacing();
            }
            if (!agent.path.empty())
            {
                agent.path.clear();
                agent.nextWaypoint = 0;
                agent.pathValid = false;
            }
            agent.state = NavAgentState::Idle;
            return;
        }

        // ---- Repath gate ----
        const bool destinationDrifted =
            Dist2XZ(agent.destination, agent.lastPlannedTarget)
            > agent.repathDistance * agent.repathDistance;
        const bool needRepath =
            agent.pathDirty ||
            !agent.pathValid ||
            agent.path.empty() ||
            agent.nextWaypoint >= agent.path.size() ||
            destinationDrifted;

        if (needRepath)
        {
            agent.state = NavAgentState::Computing;
            // A full Detour query (findNearestPoly x2 + A* + findStraightPath) on this thread's
            // dtNavMeshQuery. Zoned per call: the number that matters is how many one frame holds (the
            // Nav.PathQueries plot).
            ZoneScopedN("Nav.FindPath");
            pathQueries.fetch_add(1, std::memory_order_relaxed);
            PathResult pr = nav.FindPath(lt->translation, agent.destination);
            agent.path              = std::move(pr.waypoints);
            agent.nextWaypoint      = 0;
            agent.pathValid         = pr.valid;
            agent.lastPlannedTarget = agent.destination;
            agent.pathDirty         = false;
            agent.stuckTimer        = 0.f;
            if (!agent.pathValid || agent.path.empty())
            {
                // A FAILING agent re-queries every frame: the repath gate tests !agent.pathValid, and this branch
                // leaves pathValid false while clearing pathDirty, so nothing breaks the loop. A steady nonzero
                // Nav.PathFailures therefore means N un-throttled A* runs per frame, not N idle agents.
                pathFailures.fetch_add(1, std::memory_order_relaxed);
                if (cc)
                {
                    auto& it = GetOrCreateIntent(world, e);
                    it.moveDir = { 0.f, 0.f, 0.f };
                    it.desiredSpeed = 0.f;
                    WriteResolvedFacingIntent(it);
                }
                else
                {
                    ApplyResolvedFacing();
                }
                agent.state = NavAgentState::Failed;
                return;
            }
            if (agent.path.size() > 1) agent.nextWaypoint = 1;
        }

        if (agent.nextWaypoint >= agent.path.size())
        {
            if (cc)
            {
                auto& it = GetOrCreateIntent(world, e);
                it.moveDir = { 0.f, 0.f, 0.f };
                it.desiredSpeed = 0.f;
                WriteResolvedFacingIntent(it);
            }
            else
            {
                ApplyResolvedFacing();
            }
            NAV_DIAG(e, "arrived", "next=%d n=%d", (int)agent.nextWaypoint,
                     (int)agent.path.size());
            agent.state = NavAgentState::Arrived;
            return;
        }

        agent.state = NavAgentState::Following;

        // ---- Steering geometry ----
        const DirectX::XMFLOAT3& wp = agent.path[agent.nextWaypoint];
        const float dx = wp.x - lt->translation.x;
        const float dy = wp.y - lt->translation.y;
        const float dz = wp.z - lt->translation.z;
        const float dist2   = dx*dx + dz*dz;
        const float arrive2 = agent.arriveRadius * agent.arriveRadius;

        if (dist2 < arrive2)
        {
            // Reached the corner: advance. Halt this tick so velocity recomputes toward the new waypoint next
            // frame.
            if (cc)
            {
                auto& it = GetOrCreateIntent(world, e);
                it.moveDir = { 0.f, 0.f, 0.f };
                it.desiredSpeed = 0.f;
                WriteResolvedFacingIntent(it);
            }
            else
            {
                // No-CCC: snap to the waypoint (XZ + Y) so the agent does not oscillate.
                lt->translation = wp;
                ApplyResolvedFacing();
            }
            ++agent.nextWaypoint;
            return;
        }

        const float dist    = std::sqrt(dist2);
        const float invDist = 1.f / dist;
        const float dirX    = dx * invDist;
        const float dirZ    = dz * invDist;

        // Arrival slowdown, only on the last corner. speedScale is the BT-driven gait control
        // (Intent.SetSpeedScale).
        float speed = agent.speed * agent.speedScale;
        const bool lastCorner = (agent.nextWaypoint + 1 == agent.path.size());
        if (lastCorner && agent.slowdownRadius > 1e-3f && dist < agent.slowdownRadius)
            speed *= std::max(0.f, dist / agent.slowdownRadius);

        if (cc)
        {
            // ---- CCC path: publish steering as intent ----
            auto& it = GetOrCreateIntent(world, e);
            NAV_DIAG(e, "steering", "speed=%.2f scale=%.2f dist=%.2f cc=1",
                     speed, agent.speedScale, dist);
            it.moveDir      = { dirX, 0.f, dirZ };
            it.desiredSpeed = speed;
            it.gait         = speed > agent.speed * 0.75f ? CharacterGait::Run
                                                          : CharacterGait::Walk;

            // Keep this asymmetry: while moving, facing is gated on rotateToFacing BEFORE the look point is
            // resolved; the idle branches above always resolve it.
            if (agent.rotateToFacing)
                WriteResolvedFacingIntent(it);
            else
                it.useFaceDir = false;

            // Stuck detection from CCC.wasBlockedLastStep (set by the KCC step in PhysicsSystem).
            if (cc->wasBlockedLastStep)
            {
                const float align =
                    cc->blockedNormal.x * (-dirX) + cc->blockedNormal.z * (-dirZ);
                if (align > 0.5f)
                {
                    agent.stuckTimer += dt;
                    if (agent.stuckTimer > 0.5f)
                    {
                        agent.pathDirty  = true;
                        agent.stuckTimer = 0.f;
                    }
                }
            }
            else
            {
                agent.stuckTimer = std::max(0.f, agent.stuckTimer - dt);
            }
        }
        else
        {
            // ---- No-CCC path: write LocalTransform directly ----
            // Step-advance + Y smoothing for entities without a CCC (e.g. a kinematic RigidBody). No ground-snap
            // raycast: add a CCC for sweep-and-slide, stairs or single-triangle ground accuracy.
            const float step = std::min(speed * dt, dist);
            lt->translation.x += dirX * step;
            lt->translation.z += dirZ * step;
            lt->translation.y += dy * std::min(1.f, dt * 8.f);

            // ---- No-CCC rotation ----
            if (!agent.rotateToFacing) return;
            if (!ApplyResolvedFacing())
            {
                if (agent.facingMode == NavFacingMode::FaceMovement)
                    ApplyFacingTowards(dirX, dirZ);
            }
        }
        }
    };

    // Below this the fork/join costs more than the work saved (the same threshold shape the renderer's
    // gather and the anim cull use).
    constexpr size_t kNavParallelThreshold = 16;
    const bool goParallel =
        agentN >= kNavParallelThreshold && TaskSystem::Get().GetWorkerCount() > 1;

    if (goParallel)
    {
        // SERIAL PRE-PASS: create every CharacterIntentComponent up front. The body writes intent through
        // GetOrCreateIntent, whose creation branch calls AddComponent - a structural change that reallocates
        // the pool, hidden behind the helper at every call site. Costs one pass over the agents on the frame
        // they first appear and nothing afterwards.
        world.EnsurePool<CharacterIntentComponent>();
        for (uint32_t i = 0; i < static_cast<uint32_t>(agentN); ++i)
        {
            const Entity e = navEnts[i];
            if (world.GetComponent<CharacterControllerComponent>(e) &&
                !world.GetComponent<CharacterIntentComponent>(e))
                world.AddComponent<CharacterIntentComponent>(e, {});
        }

        g_navTickParallel.store(true, std::memory_order_relaxed);
        TaskSystem::Get().ParallelFor(0u, static_cast<uint32_t>(agentN), tickOne);
        g_navTickParallel.store(false, std::memory_order_relaxed);
    }
    else
    {
        for (uint32_t i = 0; i < static_cast<uint32_t>(agentN); ++i) tickOne(i);
    }

    // Published unconditionally, not only to Tracy: the AI benchmark reports path requests per second.
    {
        auto& prof = AI::AIProfile::Get();
        prof.navAgents    = agentCount.load(std::memory_order_relaxed);
        prof.pathQueries  = pathQueries.load(std::memory_order_relaxed);
        prof.pathFailures = pathFailures.load(std::memory_order_relaxed);
    }
#ifdef TRACY_ENABLE
    TracyPlot("Nav.Agents",       static_cast<int64_t>(agentCount.load()));
    TracyPlot("Nav.PathQueries",  static_cast<int64_t>(pathQueries.load()));
    TracyPlot("Nav.PathFailures", static_cast<int64_t>(pathFailures.load()));
#else
    (void)agentCount; (void)pathQueries; (void)pathFailures;
#endif
}

} // namespace Nav

// ==== NavAgentTickSystem ====
void NavAgentTickSystem::Update(World& world, const FrameContext& ctx)
{
    const auto t0 = std::chrono::steady_clock::now();
    Nav::NavAgentTick(world, m_nav, ctx.scaledDeltaTime);
    AI::AIProfile::Get().navWallMs =
        std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void NavAgentTickSystem::PrepareStructure(World& world)
{
    world.EnsurePool<CharacterIntentComponent>();
}

void NavAgentTickSystem::DeclareAccess(SystemAccessBuilder& b) const
{
    // NavAgent holds the path runtime state mutated here. As the AI brain it publishes
    // steering into CharacterIntentComponent (may grow that pool via auto-promote). It
    // writes LocalTransform on the no-CCC path and clears CCC.launchFinished in the
    // off-mesh handshake.
    b.Read<NavAgentComponent>()
     .Write<NavAgentComponent>()
     .Write<CharacterIntentComponent>()
     .Write<CharacterControllerComponent>()
     .Read<BrainComponent>()
     .Write<LocalTransform>();
}
