#pragma once

// EngineSystems.h - thin ISystem adapters over the engine's concrete systems
// (ScriptSystem, AISystem, PhysicsSystem, ...) so the Scheduler drives them.
// Each adapter wraps one system's per-frame call(s) through references taken in its
// constructor; App owns the systems, and App::RegisterTickSystems declares the
// tick order (an engine module's systems register from its RegisterSystems,
// which it calls). Phases:
//   GameplayPreLogic      ScriptTimerSystem
//   GameplayLogic         ScriptLogicSystem (Update + script hot reload)
//   AI                    AILOD -> Perception -> AIBT -> NavAgent (the Nav module's) -> PlayerControl
//                         -> CharacterMotor (the single intent consumer, last)
//   FixedPhysicsPre       PhysicsKinematicSystem (MoveKinematic), every substep
//   FixedPhysics          PhysicsStepSystem, stepped by App::RunFixedPhysicsLoop
//   PhysicsInterpolation  TransformPropagate (always on) -> PhysicsInterpApply
//                         -> CameraFollowResolve
//   Animation             CharacterAnimDriver -> RendererAnimationChain
//   PreRender             AudioTickSystem, after physics interpolation so collision
//                         SFX fire in the same frame as the collision

#include "ISystem.h"

#include <functional>

// LambdaSystem - a phase-typed adapter that just invokes a callback, for work whose
// dependencies are too many or too transient for a class + services struct. The
// canonical client is App's render phase: it touches many App locals and members,
// but its only scheduler contract is "run once in the Render phase". The lambda
// captures by reference, which is safe only because App::ShutdownAllSystems shuts
// the registry down before App::Run returns and its captured locals die.
template <TickPhase PHASE>
class LambdaSystem : public SystemInPhase<PHASE>
{
public:
    using Fn = std::function<void(World&, const FrameContext&)>;
    LambdaSystem(const char* name, Fn fn)
        : m_name(name), m_fn(std::move(fn)) {}
    void Update(World& world, const FrameContext& ctx) override
    {
        m_fn(world, ctx);
    }
    const char* GetName() const override { return m_name; }
private:
    const char* m_name;
    Fn          m_fn;
};

// Render-phase alias: App registers the frame's render phase through it.
using RenderSystem = LambdaSystem<TickPhase::Render>;

// ---- Forward declarations of the concrete systems ----
class World;
class Renderer;
class ScriptSystem;
class PlayerControllerSystem;
class CameraSystem;
namespace AI       { class AISystem; class AILODSystem; class PerceptionSystem; }
namespace Resource { class AnimationClipSystem; }
namespace DX12Physics { class PhysicsSystem; }
namespace Audio    {
    class AudioClipSystem;
    class AudioSystem;
    class Audio3DSystem;
    class AudioEngine;
}

// ==== GameplayPreLogic ====

class ScriptTimerSystem : public SystemInPhase<TickPhase::GameplayPreLogic>
{
public:
    explicit ScriptTimerSystem(ScriptSystem& s) : m_script(s) {}
    void Update(World& world, const FrameContext& ctx) override;
    SystemRunMask RunMask() const override { return SystemRunMask::Play; }
    const char* GetName() const override { return "ScriptTimerSystem"; }
private:
    ScriptSystem& m_script;
};

// ==== GameplayLogic ====

class ScriptLogicSystem : public SystemInPhase<TickPhase::GameplayLogic>
{
public:
    explicit ScriptLogicSystem(ScriptSystem& s) : m_script(s) {}
    void Update(World& world, const FrameContext& ctx) override;
    SystemRunMask RunMask() const override { return SystemRunMask::Play; }
    const char* GetName() const override { return "ScriptLogicSystem"; }
private:
    ScriptSystem& m_script;
};

// ==== AI ====

class AILODTickSystem : public SystemInPhase<TickPhase::AI>
{
public:
    AILODTickSystem(AI::AILODSystem& s) : m_lod(s) {}
    void Update(World& world, const FrameContext& ctx) override;
    void DeclareAccess(SystemAccessBuilder& b) const override;
    SystemRunMask RunMask() const override { return SystemRunMask::Play; }
    const char* GetName() const override { return "AILODSystem"; }
private:
    AI::AILODSystem& m_lod;
};

// Runs BEFORE AIBTTickSystem on purpose: a tree that asks "can I see the
// player" should get this frame's answer, not last frame's.
class PerceptionTickSystem : public SystemInPhase<TickPhase::AI>
{
public:
    PerceptionTickSystem(AI::PerceptionSystem& s, DX12Physics::PhysicsSystem& p)
        : m_perception(s), m_physics(p) {}
    void Update(World& world, const FrameContext& ctx) override;
    void DeclareAccess(SystemAccessBuilder& b) const override;
    SystemRunMask RunMask() const override { return SystemRunMask::Play; }
    const char* GetName() const override { return "PerceptionSystem"; }
private:
    AI::PerceptionSystem&        m_perception;
    DX12Physics::PhysicsSystem&  m_physics;
};

class AIBTTickSystem : public SystemInPhase<TickPhase::AI>
{
public:
    // ScriptSystem is borrowed for the tick so Engine.* bindings (gated on
    // ScriptSystem::m_world) work inside BT actions instead of silently returning 0.
    AIBTTickSystem(AI::AISystem& s, ScriptSystem& script)
        : m_ai(s), m_script(script) {}
    void Update(World& world, const FrameContext& ctx) override;
    SystemRunMask RunMask() const override { return SystemRunMask::Play; }
    const char* GetName() const override { return "AIBTSystem"; }
private:
    AI::AISystem& m_ai;
    ScriptSystem& m_script;
};

// Not registered: the strategic AIGoal layer has no users (BTs write Intent.MoveTo
// directly). Register it in App::RegisterTickSystems to revive that layer.
class AITacticalTickSystem : public SystemInPhase<TickPhase::AI>
{
public:
    AITacticalTickSystem() = default;
    void Update(World& world, const FrameContext& ctx) override;
    void DeclareAccess(SystemAccessBuilder& b) const override;
    SystemRunMask RunMask() const override { return SystemRunMask::Play; }
    const char* GetName() const override { return "AITacticalSystem"; }
};

class PlayerControlTickSystem : public SystemInPhase<TickPhase::AI>
{
public:
    PlayerControlTickSystem(PlayerControllerSystem& pc) : m_pc(pc) {}
    void Update(World& world, const FrameContext& ctx) override;
    void DeclareAccess(SystemAccessBuilder& b) const override;
    SystemRunMask RunMask() const override { return SystemRunMask::Play; }
    const char* GetName() const override { return "PlayerBrainSystem"; }
private:
    PlayerControllerSystem& m_pc;
};

// The single consumer of CharacterIntentComponent: turns every brain's intent into
// CCC drive fields + facing (Character::MotorTick). Must be registered AFTER all
// brain systems in the AI phase.
class CharacterMotorTickSystem : public SystemInPhase<TickPhase::AI>
{
public:
    CharacterMotorTickSystem() = default;
    void OnRegister(World& world) override;
    void Update(World& world, const FrameContext& ctx) override;
    void DeclareAccess(SystemAccessBuilder& b) const override;
    SystemRunMask RunMask() const override { return SystemRunMask::Play; }
    const char* GetName() const override { return "CharacterMotorSystem"; }
};

// ==== GameplayPostLogic ====
// No adapter here: ProjectileSystem (ECS/ProjectileComponent.h) and
// SequencerTickSystem (Sequencer/SequencerSystem.h) run in this phase from their own
// headers. The pending scene swap is no system: App::Run drains it inline because
// it needs the live GameModeContext.

// ==== FixedPhysics ====

// FixedPhysicsPre, every substep: moves the Kinematic bodies (PhysicsSystem::MoveKinematicBodies).
class PhysicsKinematicSystem : public SystemInPhase<TickPhase::FixedPhysicsPre>
{
public:
    PhysicsKinematicSystem(DX12Physics::PhysicsSystem& p) : m_physics(p) {}
    void Update(World& world, const FrameContext& ctx) override;
    SystemRunMask RunMask() const override { return SystemRunMask::Play; }
    const char* GetName() const override { return "PhysicsKinematicSystem"; }
private:
    DX12Physics::PhysicsSystem& m_physics;
};

class PhysicsStepSystem : public SystemInPhase<TickPhase::FixedPhysics>
{
public:
    PhysicsStepSystem(DX12Physics::PhysicsSystem& p) : m_physics(p) {}
    void Update(World& world, const FrameContext& ctx) override;
    SystemRunMask RunMask() const override { return SystemRunMask::Play; }
    const char* GetName() const override { return "PhysicsStepSystem"; }
private:
    DX12Physics::PhysicsSystem& m_physics;
};

// ==== PhysicsInterpolation ====

class TransformPropagateSystem
    : public SystemInPhase<TickPhase::PhysicsInterpolation>
{
public:
    void OnRegister(World& world) override;
    void Update(World& world, const FrameContext& ctx) override;
    void DeclareAccess(SystemAccessBuilder& b) const override;
    const char* GetName() const override { return "TransformPropagateSystem"; }
};

class PhysicsInterpApplySystem
    : public SystemInPhase<TickPhase::PhysicsInterpolation>
{
public:
    PhysicsInterpApplySystem(DX12Physics::PhysicsSystem& p) : m_physics(p) {}
    void Update(World& world, const FrameContext& ctx) override;
    void DeclareAccess(SystemAccessBuilder& b) const override;
    SystemRunMask RunMask() const override { return SystemRunMask::Play; }
    const char* GetName() const override { return "PhysicsInterpolation"; }
private:
    DX12Physics::PhysicsSystem& m_physics;
};

class CameraFollowResolveSystem
    : public SystemInPhase<TickPhase::PhysicsInterpolation>
{
public:
    CameraFollowResolveSystem(DX12Physics::PhysicsSystem& p) : m_physics(p) {}
    void Update(World& world, const FrameContext& ctx) override;
    void DeclareAccess(SystemAccessBuilder& b) const override;
    const char* GetName() const override { return "CameraFollowResolveSystem"; }
private:
    DX12Physics::PhysicsSystem& m_physics;
};

// ==== Animation ====
// RendererAnimationChainSystem wraps Renderer::TickAnimationChain: character state
// -> AnimationSystem sample -> IK -> ChainPhysics -> LocalToWorld -> Socket/Follow
// -> bone AABB merge -> SkinMatrix -> BuildSkinJobs. Always on, so pose/skinning
// buffers are rebuilt for this frame's GPU slot every frame; only the TIME is gated
// (animDt = runUpdate ? deltaTime : 0), so in the editor while Stopped/Paused every
// clip stays frozen. A panel that plays a clip in Edit mode drives the time itself
// (the Chain Physics Preview: AnimationComponent::externalTime + its own StageClock).

// Automatic animation selection from intent + motor truth (loco param bridge,
// discrete gait fallback, action layer). Registered BEFORE
// RendererAnimationChainSystem so its Animator.Play requests land in this frame's
// mailbox. Opaque (QueueAnimatorPlay adds mailbox components).
class CharacterAnimDriverTickSystem
    : public SystemInPhase<TickPhase::Animation>
{
public:
    explicit CharacterAnimDriverTickSystem(Resource::AnimationClipSystem& c)
        : m_clips(c) {}
    void Update(World& world, const FrameContext& ctx) override;
    SystemRunMask RunMask() const override { return SystemRunMask::Play; }
    const char* GetName() const override { return "CharacterAnimDriver"; }
private:
    Resource::AnimationClipSystem& m_clips;
};

class RendererAnimationChainSystem
    : public SystemInPhase<TickPhase::Animation>
{
public:
    RendererAnimationChainSystem(Renderer& r) : m_renderer(r) {}
    void Update(World& world, const FrameContext& ctx) override;
    const char* GetName() const override { return "RendererAnimationChain"; }
private:
    Renderer& m_renderer;
};

// ==== PreRender ====

class AudioTickSystem : public SystemInPhase<TickPhase::PreRender>
{
public:
    AudioTickSystem(Audio::AudioClipSystem& clip,
                    Audio::AudioSystem&     audio,
                    Audio::Audio3DSystem&   audio3D,
                    Audio::AudioEngine&     engine)
        : m_clip(clip), m_audio(audio), m_audio3D(audio3D), m_engine(engine) {}
    void Update(World& world, const FrameContext& ctx) override;
    void DeclareAccess(SystemAccessBuilder& b) const override;
    // Always, not Play: this system is the ONLY consumer of
    // AudioSourceComponent::playRequested / stopRequested, so skipping Edit would leave
    // those edges latched (an inspector or Sequencer preview would be silent, then fire
    // on the next Play). In Edit it drains the edges but starts no voice unless preview
    // audio is on (SetEditPreviewEnabled).
    SystemRunMask RunMask() const override { return SystemRunMask::Always; }
    const char* GetName() const override { return "AudioTickSystem"; }

    // Opt-in editor preview audio. Off by default: re-triggering a voice on
    // every frame of a scrub drag is a machine-gun, not a preview.
    static void SetEditPreviewEnabled(bool v);
    static bool EditPreviewEnabled();
private:
    Audio::AudioClipSystem& m_clip;
    Audio::AudioSystem&     m_audio;
    Audio::Audio3DSystem&   m_audio3D;
    Audio::AudioEngine&     m_engine;
};
