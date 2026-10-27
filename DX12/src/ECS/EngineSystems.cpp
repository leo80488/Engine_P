#include "ECS/EngineSystems.h"
#include "AI/PerceptionSystem.h"
#include "AI/AIProfile.h"
#include "Graphics/AnimProfile.h"
#include <chrono>
#include "ECS/PerceptionComponent.h"
#include "ECS/FrameContext.h"
#include "ECS/ECS.h"
#include "ECS/ExclusiveResourceIds.h"
#include "ECS/HierarchyComponents.h"
#include "ECS/PhysicsComponents.h"
#include "ECS/Components.h"
#include "Audio/AudioComponents.h"
#include "ECS/CameraSystem.h"
#include "ECS/PlayerControllerSystem.h"
#include "ECS/PlayerComponent.h"
#include "ECS/CharacterControllerComponent.h"
#include "Scripting/ScriptSystem.h"
#include "ECS/CharacterIntentComponent.h"
#include "ECS/CharacterMotorSystem.h"
#include "ECS/AnimGraphComponent.h"
#include "ECS/CharacterAnimDriver.h"
#include "AI/AISystem.h"
#include "AI/AILODSystem.h"
#include "AI/AIComponents.h"
#include "Nav/NavComponents.h"
#include "ECS/AIIntentComponent.h"
#include "ECS/CharacterControllerComponent.h"
#include "AI/AITacticalSystem.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/TransformSystem.h"
#include "Audio/AudioClipSystem.h"
#include "Audio/AudioSystem.h"
#include "Audio/Audio3DSystem.h"
#include "Audio/AudioEngine.h"
#include "Audio/AudioEvents.h"
#include "System/EventBus.h"
#include "Renderer/Renderer.h"
#include "Gameplay/GameState.h"   // playtime accumulator (ScriptTimerSystem)
#include "Gameplay/GameplayTime.h"

// ==== GameplayPreLogic ====

void ScriptTimerSystem::Update(World& /*world*/, const FrameContext& ctx)
{
    // RunMask Play: ticked only while the sim advances (Playing / Stepping).
    // Real (unscaled) dt: hit-stop callbacks must expire even when a timeScale of ~0
    // freezes gameplay.
    m_script.TickTimers(ctx.deltaTime);

    // Playtime rides the same gate with REAL dt: editor Edit mode does not count,
    // hit-stops and pauses through timeScale do (wall-clock "time in game", persisted by
    // SaveGame).
    Gameplay::GameState::Get().AddPlaytime(ctx.deltaTime);
}

// ==== GameplayLogic ====

void ScriptLogicSystem::Update(World& world, const FrameContext& ctx)
{
    m_script.Update(world, ctx.scaledDeltaTime);
    m_script.CheckHotReload(world);
}

// ==== AI ====


namespace
{
    // Millisecond stopwatch for the AI-phase timings published to AIProfile.
    // steady_clock, not high_resolution_clock: the latter aliases system_clock in this
    // toolchain and is NOT monotonic, so an NTP correction mid-run would produce a
    // negative frame time and poison a p99.
    struct AIPhaseTimer
    {
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        float Ms() const
        {
            return std::chrono::duration<float, std::milli>(
                       std::chrono::steady_clock::now() - t0).count();
        }
    };
}

void AILODTickSystem::Update(World& world, const FrameContext& ctx)
{
    AIPhaseTimer t;
    m_lod.SetCameraEntity(static_cast<Entity>(ctx.cameraEntity));
    m_lod.Update(world);
    AI::AIProfile::Get().lodWallMs = t.Ms();
}

void AILODTickSystem::DeclareAccess(SystemAccessBuilder& b) const
{
    // Reads agent world poses (distance-from-camera), writes tickInterval.
    b.Read<GlobalTransform>()
     .Write<AIComponent>();
}

void PerceptionTickSystem::Update(World& world, const FrameContext& ctx)
{
    AIPhaseTimer t;
    m_perception.Update(world, m_physics, ctx.scaledDeltaTime);
    auto& prof = AI::AIProfile::Get();
    prof.perceptionWallMs = t.Ms();
    // cpuMs is summed per agent inside the system, so it exceeds wall when the
    // work fanned out and equals it when it did not.
    prof.perceptionCpuMs        = m_perception.lastCpuMs;
    prof.perceptionAgents       = m_perception.lastAgents;
    prof.perceptionTicked       = m_perception.lastTicked;
    prof.perceptionCandidates   = m_perception.lastCandidates;
    prof.perceptionRays         = m_perception.lastRays;
}

void PerceptionTickSystem::DeclareAccess(SystemAccessBuilder& b) const
{
    // Writes only its own component (per agent, disjoint); reads transforms for
    // position / facing and PerceptibleComponent to classify candidates. The
    // line-of-sight test is a Jolt CastRayClosest (read-only query), which takes
    // kPhysicsWorld by engine convention.
    b.Write<PerceptionComponent>()
     .Read<GlobalTransform>()
     .Read<PerceptibleComponent>()
     .ExclusiveResource(ExclusiveRes::kPhysicsWorld);
}

// AIBTTickSystem is INTENTIONALLY opaque (no DeclareAccess): BT leaves run arbitrary
// Lua on the shared ScriptSystem VM (unbounded component access), TickEntity lazily
// adds BlackboardComponent (AISystem.cpp), and the Lua Intent.* / AI.* bindings add
// NavAgent / AIIntent - pool creation or growth that phase-mates (NavAgentTickSystem
// iterates that pool) must never see mid-flight. Serializing it against the whole
// AI phase is required, not conservatism.
void AIBTTickSystem::Update(World& world, const FrameContext& ctx)
{
    // Bind the world for the tick so Engine.* bindings work inside BT actions. The
    // deferred-destroy window makes Engine.DestroyEntity queue instead of erasing pool
    // storage that the AI iteration (and ctx.blackboard pointers) still reference.
    AIPhaseTimer t;
    World* const prev = m_script.ExchangeBoundWorld(&world);
    m_script.BeginDeferredDestroyWindow();

    m_ai.CheckHotReload(world);
    m_ai.Update(world, ctx.scaledDeltaTime);
    {
        auto& prof = AI::AIProfile::Get();
        prof.btWallMs = t.Ms();
        prof.aiAgents = m_ai.LastAgentCount();
        prof.btTicks  = m_ai.LastTickCount();
    }

    m_script.EndDeferredDestroyWindow();
    m_script.ExchangeBoundWorld(prev);
}

void AITacticalTickSystem::Update(World& world, const FrameContext& ctx)
{
    AI::AITacticalTick(world, ctx.scaledDeltaTime);
}

void AITacticalTickSystem::DeclareAccess(SystemAccessBuilder& b) const
{
    // AIIntent is the strategic input (written by the BT earlier in this phase),
    // translated into NavAgent fields (destination, facingMode, ...).
    b.Read<AIIntentComponent>()
     .Write<NavAgentComponent>()
     .Read<GlobalTransform>();   // for resolving target entity positions
}

void PlayerControlTickSystem::Update(World& world, const FrameContext& ctx)
{
    // ctx.cameraEntity (the main camera) is the fallback for a PlayerComponent whose
    // cameraEntity is NullEntity.
    m_pc.Update(world, static_cast<Entity>(ctx.cameraEntity),
                ctx.scaledDeltaTime);
}

void PlayerControlTickSystem::DeclareAccess(SystemAccessBuilder& b) const
{
    // Player brain: reads input + camera basis, writes intent only (may grow the intent
    // pool via auto-promote). PlayerComponent is Write because AttachmentRef::Resolve
    // refreshes its cached entity. Input::Get() is a per-frame snapshot taken before
    // the scheduler runs, so it is safe on workers.
    b.Write<PlayerComponent>()
     .Read<GlobalTransform>()
     .Read<LocalTransform>()      // Locked facing mode samples the body yaw
     .Read<BrainComponent>()
     .Write<CharacterIntentComponent>();
}

void CharacterMotorTickSystem::OnRegister(World& world)
{
    // Brains auto-promote intents, but pre-create the pool so an empty scene has it too
    // (no first Add inside a parallel batch).
    world.EnsurePool<CharacterIntentComponent>();
}

void CharacterMotorTickSystem::Update(World& world, const FrameContext& ctx)
{
    AIPhaseTimer t;
    Character::MotorTick(world, ctx.scaledDeltaTime);
    AI::AIProfile::Get().motorWallMs = t.Ms();
}

void CharacterMotorTickSystem::DeclareAccess(SystemAccessBuilder& b) const
{
    // Consumes intent (clears edge flags, owns jumpBufferTimer), drives the
    // CCC + facing. Tuning fallbacks read Player/NavAgent components.
    b.Write<CharacterIntentComponent>()
     .Write<CharacterControllerComponent>()
     .Write<LocalTransform>()
     .Read<PlayerComponent>()
     .Read<NavAgentComponent>()
     .Write<AnimGraphComponent>().Read<AnimationComponent>().Read<SkeletonRef>().Read<Children>();
}

// ==== FixedPhysics ====

void PhysicsKinematicSystem::Update(World& world, const FrameContext& /*ctx*/)
{
    m_physics.MoveKinematicBodies(world);
}

void PhysicsStepSystem::Update(World& world, const FrameContext& ctx)
{
    // Runs once per RunPhase(FixedPhysics); App::RunFixedPhysicsLoop owns the
    // accumulator, loops while accumulator >= fixedDt, and calls
    // PhysicsSystem::PreAllSteps / PostAllSteps around the substeps.
    m_physics.StepOnce(world);
}

// ==== PhysicsInterpolation ====

void TransformPropagateSystem::OnRegister(World& world)
{
    // Pre-create the GlobalTransform pool on the main thread. Propagate calls
    // world.EnsurePool<GlobalTransform>() every tick; with the pool present that is a
    // find-only fast path, but the CREATE branch mutates the World's pool map, which is
    // illegal if this system ever runs co-batched.
    world.EnsurePool<GlobalTransform>();
}

void TransformPropagateSystem::Update(World& world, const FrameContext& /*ctx*/)
{
    // Always on (no runUpdate gate). The editor gizmo writes LocalTransform and patches
    // its own GlobalTransform; only this pass propagates the change to descendants.
    // Skipping it while paused leaves children pinned at stale world positions.
    TransformSystem::Propagate(world);
}

void TransformPropagateSystem::DeclareAccess(SystemAccessBuilder& b) const
{
    // Propagate composes LocalTransform down the hierarchy into GlobalTransform, pushes
    // inheritedHidden and recomputes WorldAabb. GlobalTransform is Write because Add can
    // grow the pool for freshly spawned children (legal only under Write<> exclusivity).
    b.Read<LocalTransform>()
     .Read<Parent>()
     .Read<Children>()
     .Read<LocalAabb>()
     .Write<GlobalTransform>()
     .Write<VisibilityComponent>()
     .Write<WorldAabb>();
}

void PhysicsInterpApplySystem::Update(World& world, const FrameContext& ctx)
{
    // RunMask Play: skipped while Stopped / Paused, so the interpolation overwrite never
    // stomps an editor gizmo drag. physicsAlpha = accumulator / fixedDt, set by
    // App::RunFixedPhysicsLoop after the substeps.
    m_physics.ApplyRenderInterpolation(world, ctx.physicsAlpha);
}

void PhysicsInterpApplySystem::DeclareAccess(SystemAccessBuilder& b) const
{
    // Lerps pose snapshots into GlobalTransform for Dynamic bodies / KCCs, then
    // PropagateSubtree pushes each body's descendants (visibility + WorldAabb).
    // poseSnapshots is PhysicsSystem-internal state written in FixedPhysics, so
    // kPhysicsWorld keeps every other physics-touching phase-mate exclusive.
    b.Read<RigidBodyComponent>()
     .Read<CharacterControllerComponent>()
     .Read<LocalTransform>()
     .Read<Children>()
     .Read<LocalAabb>()
     .Write<GlobalTransform>()
     .Write<VisibilityComponent>()
     .Write<WorldAabb>()
     .ExclusiveResource(ExclusiveRes::kPhysicsWorld);
}

void CameraFollowResolveSystem::Update(World& world, const FrameContext& ctx)
{
    const Entity camEnt = static_cast<Entity>(ctx.cameraEntity);
    if (camEnt == NullEntity) return;

    auto* ctrl = world.GetComponent<CameraControllerComponent>(camEnt);
    if (!ctrl) return;
    if (ctrl->mode == CameraControllerComponent::Mode::Free) return;

    auto* lt = world.GetComponent<LocalTransform>(camEnt);
    if (!lt) return;

    // ResolveFollowing reads the followTarget's freshly propagated world pose, which is
    // why this system runs AFTER TransformPropagate in the same phase.
    // dt drives the ThirdPerson follow smoothing: scaled gameplay time, so the lag
    // freezes with the time-scaled target. When the sim is not running (editor Stopped
    // / Paused) pass 0 so the camera snaps instead of gliding: this system is not
    // runUpdate-gated and would otherwise drift toward a gizmo-dragged target.
    const float followDt = ctx.runUpdate ? ctx.scaledDeltaTime : 0.0f;
    CameraSystem::ResolveFollowing(world, *ctrl, *lt, followDt, &m_physics);

    // The camera is a root entity, so its world matrix is its local matrix. Patch
    // GlobalTransform here so this frame's RenderSystem sees the resolved pose without
    // a second Propagate.
    if (auto* gt = world.GetComponent<GlobalTransform>(camEnt))
        DirectX::XMStoreFloat4x4(&gt->matrix, lt->ToMatrix());
}

void CameraFollowResolveSystem::DeclareAccess(SystemAccessBuilder& b) const
{
    // Writes the camera's controller smoothing state (including the followTarget
    // AttachmentRef's mutable resolve cache inside the component), its LocalTransform,
    // and its GlobalTransform. The spring-arm probe is a Jolt narrow-phase CastShape ->
    // kPhysicsWorld. GuidRegistry::Find on the Resolve slow path is a read-only lookup
    // (see the kGuidRegistry note in ExclusiveResourceIds.h).
    b.Write<CameraControllerComponent>()
     .Write<LocalTransform>()
     .Write<GlobalTransform>()
     .Read<CharacterControllerComponent>()  // follow target's own body, excluded from the probe
     .Read<RigidBodyComponent>()
     .ExclusiveResource(ExclusiveRes::kPhysicsWorld);
}

// ==== Animation ====

void CharacterAnimDriverTickSystem::Update(World& world, const FrameContext& ctx)
{
    // Timed on its own (AnimProfile::animDriverMs) so the Animation phase's cost splits
    // into this driver, the chain and scheduler overhead.
    Anim::StageTimer t;
    Character::AnimDriverTick(world, m_clips, ctx.scaledDeltaTime);
    Anim::AnimProfile::Get().animDriverMs = t.Ms();
}

void RendererAnimationChainSystem::Update(World& world, const FrameContext& ctx)
{
    // The chain ALWAYS runs: skinning / pose buffers must be rebuilt for this frame's
    // GPU slot every frame, or a frozen character renders a stale pose slot. Only the
    // time advance is gated: in the editor runUpdate is false while Stopped / Paused
    // and true while Playing or on a single Step; in a Game build it is always true.
    // AnimationSystem::Update samples every call and advances primaryTime only when
    // dt > 0, so dt = 0 freezes the pose while still rendering it.
    // The chain runs on GAME time: a Time.RequestScale pause or hit-stop stops poses,
    // attack clocks and notifies together with the rest of gameplay. One entity keeps
    // real time, or is slowed on its own, through TimeDilationComponent, which the chain
    // resolves per entity from the clocks published here.
    const float realDt = ctx.runUpdate ? ctx.deltaTime : 0.0f;
    const GameplayTime::Clocks clocks{
        GameplayTime::LegacyClock() ? realDt : (ctx.runUpdate ? ctx.scaledDeltaTime : 0.0f), realDt };
    GameplayTime::Publish(clocks);
    m_renderer.TickAnimationChain(world, ctx.frameIndex, clocks.game);
}

// ==== PreRender ====

namespace { bool s_audioEditPreview = false; }
void AudioTickSystem::SetEditPreviewEnabled(bool v) { s_audioEditPreview = v; }
bool AudioTickSystem::EditPreviewEnabled()          { return s_audioEditPreview; }

void AudioTickSystem::Update(World& world, const FrameContext& ctx)
{
    // Edit mode without preview audio: drain the transport edges so nothing latches
    // into the next Play, then stop.
    if (ctx.mode == EngineMode::Edit && !s_audioEditPreview)
    {
        if (auto* pool = world.GetPool<Audio::AudioSourceComponent>())
            for (Audio::AudioSourceComponent& src : pool->Data())
            {
                src.playRequested      = false;
                src.stopRequested      = false;
                src.playStartOffsetSec = 0.f;
            }
        return;
    }

    // Promote RM-loaded .aclip resources to local Ready FIRST, so events /
    // playOnEnable that landed this frame can resolve.
    m_clip.Tick();

    EventBus::Get().DispatchOne<Audio::PlaySoundEvent>();
    EventBus::Get().DispatchOne<Audio::StopSoundEvent>();
    EventBus::Get().DispatchOne<Audio::SetAudioParamEvent>();
    EventBus::Get().DispatchOne<Audio::BusVolumeChangedEvent>();

    m_audio  .Update(world, ctx.scaledDeltaTime);
    m_audio3D.Update(world, ctx.scaledDeltaTime);
    m_engine .Update(ctx.scaledDeltaTime);
}

void AudioTickSystem::DeclareAccess(SystemAccessBuilder& b) const
{
    // src/Audio makes no structural world mutations (no AddComponent / CreateEntity /
    // DestroyEntity); AudioSource lifecycle fields are written in place, and Audio3D
    // reads listener poses from GlobalTransform (camera fallback: CameraComponent).
    // The AudioEngine voice pool and AudioClipSystem maps are internally mutexed but
    // stay behind kAudioEngine by engine convention. The four DispatchOne calls run
    // subscriber callbacks (AudioSystem::On*), and EventBus is main-thread-only by
    // contract -> kEventBus.
    b.Write<Audio::AudioSourceComponent>()
     .Read<Audio::AudioListenerComponent>()
     .Read<CameraComponent>()
     .Read<GlobalTransform>()
     .ExclusiveResource(ExclusiveRes::kAudioEngine)
     .ExclusiveResource(ExclusiveRes::kEventBus);
}
