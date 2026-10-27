#pragma once

// CameraStackSystem.h - the systems that drive virtual cameras
// (docs/design/camera_stack_system.md).
//   PhysicsInterpolation:
//     * FollowCameraTickSystem / AimCameraTickSystem - CameraPoseComponent of
//       Follow / Aim VCams
//     * MotionDirectorTickSystem, CameraMotionTickSystem - motion-file tracks
//   PreRender:
//     * CameraStackTickSystem   - rebuilds the per-channel stack, advances blend state
//     * CameraResolveTickSystem - blends VCam poses into LiveCameraComponent
//     * CameraShakeTickSystem   - additive shake on the resolved Live pose

#include "ECS/ISystem.h"
#include "ECS/TickPhase.h"
#include "ECS/CameraStackComponents.h"

class World;
struct FrameContext;

namespace DX12Physics { class PhysicsSystem; }
namespace Resource    { class AnimationClipSystem; }
class ClipLibrary;   // skinned-clip durations (MotionDirector auto-resolve)

namespace Camera
{
    // ---- Channel-singleton management ----
    // GetOrCreateChannelEntity returns the entity carrying the
    // CameraStackChannelSingleton + LiveCameraComponent for channelId, creating it on
    // first touch. App::TickCamera ensures the Main channel each frame; PushVCam and
    // HardCutTo call it implicitly.
    Entity GetOrCreateChannelEntity(World& world, CameraChannelId channelId);

    Entity FindChannelEntity(World& world, CameraChannelId channelId);

    // ---- Stack manipulation (Lua + behavior systems) ----
    // PushVCam adds (or refreshes) a VCam on the stack. If the entity already has the
    // four core VCam components only Priority / Blend are patched, so a re-push during
    // BlendingOut goes back to BlendingIn from the current blend, never from 0.
    struct PushVCamArgs
    {
        int             priority         = 10;
        float           weight           = 1.f;
        float           blendInDuration  = 0.25f;
        float           blendOutDuration = 0.25f;
        BlendCurve      curveIn          = BlendCurve::EaseOut;
        BlendCurve      curveOut         = BlendCurve::EaseIn;
        CameraChannelId channelId        = Camera::kMainChannel;
    };

    void PushVCam(World& world, Entity vcam, const PushVCamArgs& args);

    // PopVCam triggers a BlendingOut transition; the entity itself stays
    // alive. blendOutOverride < 0 = use the entity's existing blendOutDuration.
    void PopVCam(World& world, Entity vcam,
                 float blendOutOverride = -1.f,
                 BlendCurve curveOverride = BlendCurve::Custom);

    // HardCutTo: sets LiveCameraComponent.historyValid = false on this channel for one
    // frame (requestHardCut). A non-null `vcam` is pushed at priority 10000 with zero
    // blend in / out, so it is fully blended in this frame (other VCams still blend out
    // at their own blendOutDuration).
    void HardCutTo(World& world, Entity vcam, CameraChannelId channelId);

    // ---- The player's shake scale (accessibility.cameraShake) ----
    // Multiplies every channel's shake: 0 = none, 1 = as authored. Any thread.
    void  SetShakeScale(float scale);
    float ShakeScale();
}

// ==== System adapters ====

// FollowCameraTickSystem - drives all FollowCameraTag entities: reads the target's
// GlobalTransform, writes its own CameraPoseComponent. Runs in
// PhysicsInterpolation, after TransformPropagate, so target poses are fresh.
class FollowCameraTickSystem
    : public SystemInPhase<TickPhase::PhysicsInterpolation>
{
public:
    void Update(World& world, const FrameContext& ctx) override;
    void DeclareAccess(SystemAccessBuilder& b) const override;
    const char* GetName() const override { return "FollowCameraSystem"; }
};

// AimCameraTickSystem - third-person orbit cam with optional spherecast wall
// collision; a null PhysicsSystem disables the sweep (e.g. cinematic preview).
class AimCameraTickSystem
    : public SystemInPhase<TickPhase::PhysicsInterpolation>
{
public:
    explicit AimCameraTickSystem(DX12Physics::PhysicsSystem* p = nullptr)
        : m_physics(p) {}
    void Update(World& world, const FrameContext& ctx) override;
    void DeclareAccess(SystemAccessBuilder& b) const override;
    const char* GetName() const override { return "AimCameraSystem"; }
private:
    DX12Physics::PhysicsSystem* m_physics = nullptr;
};

// CameraMotionTickSystem - plays CameraMotionComponent tracks (camera .ianim, e.g.
// an MMD camera VMD) by driving a dedicated high-priority VCam. Same phase as
// Follow / Aim so the pose is ready for the PreRender stack / resolve; needs the
// AnimationClipSystem to acquire and read the clip.
// Deliberately opaque (no DeclareAccess): the first-push path mutates structure at
// runtime (PushVCam -> EnsureVCamComponents adds 4 components,
// GetOrCreateChannelEntity creates an entity + 2 components) and the borrowed
// AnimationResource* escapes the clip-system lock. Route those through the
// CommandBuffer before declaring access.
class CameraMotionTickSystem
    : public SystemInPhase<TickPhase::PhysicsInterpolation>
{
public:
    explicit CameraMotionTickSystem(Resource::AnimationClipSystem* clips = nullptr)
        : m_clips(clips) {}
    void Update(World& world, const FrameContext& ctx) override;
    const char* GetName() const override { return "CameraMotionSystem"; }
private:
    Resource::AnimationClipSystem* m_clips = nullptr;
};

// MotionDirectorTickSystem - master-clock conductor: drives several motion tracks
// (skinned AnimationComponent + CameraMotionComponent) in lockstep from one
// masterTime, so separate camera-motion and skinned-motion files play in sync.
// Must run BEFORE CameraMotionTickSystem (it sets per-track time) and before the
// Animation phase (AnimationSystem reads primaryTime).
class MotionDirectorTickSystem
    : public SystemInPhase<TickPhase::PhysicsInterpolation>
{
public:
    // Clip sources are optional: with them the director auto-resolves its total span
    // (countdown / UI) when it has no authored `duration`; without them
    // resolvedDuration stays 0 and the transport still runs.
    explicit MotionDirectorTickSystem(Resource::AnimationClipSystem* cameraClips = nullptr,
                                      const ClipLibrary*             skinnedClips = nullptr)
        : m_cameraClips(cameraClips), m_skinnedClips(skinnedClips) {}
    void Update(World& world, const FrameContext& ctx) override;
    void DeclareAccess(SystemAccessBuilder& b) const override;
    const char* GetName() const override { return "MotionDirectorSystem"; }
private:
    Resource::AnimationClipSystem* m_cameraClips  = nullptr; // camera-track clip durations
    const ClipLibrary*             m_skinnedClips = nullptr; // skinned-track clip durations
};

// CameraStackTickSystem - per channel: rebuild entries from the VCams, elect the
// winner, advance every VCam's blend state machine. Does NOT touch
// LiveCameraComponent (CameraResolveTickSystem does).
class CameraStackTickSystem
    : public SystemInPhase<TickPhase::PreRender>
{
public:
    void Update(World& world, const FrameContext& ctx) override;
    void DeclareAccess(SystemAccessBuilder& b) const override;
    const char* GetName() const override { return "CameraStackSystem"; }
};

// CameraResolveTickSystem - blends contributing VCam poses (currentBlend > 0) into
// LiveCameraComponent, with the prev / curr bookkeeping for historyValid.
class CameraResolveTickSystem
    : public SystemInPhase<TickPhase::PreRender>
{
public:
    void Update(World& world, const FrameContext& ctx) override;
    void DeclareAccess(SystemAccessBuilder& b) const override;
    const char* GetName() const override { return "CameraResolveSystem"; }
};

// CameraShakeTickSystem - applies CameraShakeComponent (on the channel entity)
// additively on top of the resolved Live pose; trauma drains each frame. After
// Resolve so shake does not double up across blending VCams.
class CameraShakeTickSystem
    : public SystemInPhase<TickPhase::PreRender>
{
public:
    void Update(World& world, const FrameContext& ctx) override;
    void DeclareAccess(SystemAccessBuilder& b) const override;
    const char* GetName() const override { return "CameraShakeSystem"; }
};
