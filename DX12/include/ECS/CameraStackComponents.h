#pragma once

// CameraStackComponents.h - the ECS virtual camera stack (rationale:
// docs/design/camera_stack_system.md).
// Data flow: behavior systems (Follow / Aim / CameraMotion) write
// CameraPoseComponent on each VCam entity; CameraStackTickSystem keeps the
// per-channel priority list and the per-VCam blend state machine;
// CameraResolveTickSystem produces one LiveCameraComponent per channel, the only
// camera the Renderer reads.

#include "ECS/ECS.h"
#include "ECS/GuidComponent.h"

#include <DirectXMath.h>
#include <cstdint>
#include <vector>

// ==== Channel identification ====

// 32-bit hashed channel name. ChannelId{0} == invalid sentinel; the default
// "Main" channel is precomputed via Camera::HashChannel("Main") so scripts
// can pass either the literal hash or a string that the binding resolves.
using CameraChannelId = uint32_t;

namespace Camera
{
    // FNV-1a 32-bit so the hash is stable across runs / serialized data.
    constexpr CameraChannelId HashChannel(const char* s) noexcept
    {
        uint32_t h = 0x811C9DC5u;
        while (*s) { h ^= static_cast<uint8_t>(*s++); h *= 0x01000193u; }
        return h;
    }
    inline constexpr CameraChannelId kMainChannel = HashChannel("Main");
}

// ==== Per-VCam components ====

// VirtualCameraComponent - lens parameters + channel binding for a VCam entity,
// read by the stack / resolve pipeline. The main camera's lens is mirrored from its
// CameraComponent (which the inspector and serialization edit) every frame by
// App::TickCamera.
struct VirtualCameraComponent
{
    float           fov            = DirectX::XM_PI / 3.f;  // vertical, radians
    float           nearZ          = 0.1f;
    float           farZ           = 200.f;
    float           aspectOverride = 0.f;                   // 0 = inherit from viewport
    CameraChannelId channelId      = Camera::kMainChannel;
};

// CameraPoseComponent - the world-space pose this VCam wants THIS frame, written
// by the behavior systems (FollowCameraTickSystem, AimCameraTickSystem,
// CameraMotionTickSystem) before CameraResolveTickSystem reads it. A VCam without a
// behavior (the FPS camera driven by CameraSystem) gets it mirrored from
// GlobalTransform by CameraStackTickSystem's bridge.
struct CameraPoseComponent
{
    DirectX::XMFLOAT3   position = { 0.f, 0.f, 0.f };
    DirectX::XMFLOAT4   rotation = { 0.f, 0.f, 0.f, 1.f };  // quaternion
};

// VCamPriorityComponent - stack ordering. Within a channel the highest priority
// wins; ties blend by weight. `enabled = false` means "ignore me" (target died,
// behavior idle) without leaving the stack, so a lower-priority cam does not churn
// through BlendIn / BlendOut each time the higher one disables itself briefly.
struct VCamPriorityComponent
{
    int   priority = 0;
    float weight   = 1.f;
    bool  enabled  = true;
};

// Blend curve, applied to the normalized 0..1 progress through a blend. Custom has
// no curve table and maps to Linear.
enum class BlendCurve : uint8_t
{
    Linear     = 0,
    EaseIn     = 1,
    EaseOut    = 2,
    EaseInOut  = 3,
    Custom     = 4,
};

// SeqCameraOwnedTag - a Sequencer owns this VCam's blend; do not integrate it.
// CameraStackTickSystem normally ACCUMULATES VCamBlendComponent::currentBlend
// (`+= dt / blendInDuration`), a function of history: fine for gameplay, fatal for
// a cinematic (a parked scrubber keeps moving; scrubbing to frame N does not
// reproduce playing to N). A sequenced camera writes `currentBlend` ABSOLUTELY from
// the playhead and the stack publishes it as-is; winner election (a pure function
// of priority) still runs.
// For the same reason a Sequencer must never call Camera::PushVCam per cut:
// PushVCam resumes from the CURRENT blend value, the path-dependence a cut must
// not have.
struct SeqCameraOwnedTag {};

enum class BlendState : uint8_t
{
    Inactive    = 0,
    BlendingIn  = 1,
    Active      = 2,
    BlendingOut = 3,
};

// VCamBlendComponent - runtime blend state machine. CameraStackTickSystem moves
// `state` by whether this VCam wins or loses this frame; `currentBlend` is the
// source of truth for resolve weighting. A blendInDuration / blendOutDuration of 0
// is a hard cut in that direction; HardCutTo() sets both to 0 AND sets
// historyValid = false for one frame.
struct VCamBlendComponent
{
    float       blendInDuration  = 0.25f;   // seconds
    float       blendOutDuration = 0.25f;
    BlendCurve  curveIn          = BlendCurve::EaseOut;
    BlendCurve  curveOut         = BlendCurve::EaseIn;
    float       currentBlend     = 0.f;     // 0..1
    BlendState  state            = BlendState::Inactive;
};

// ==== Behavior components ====

struct FollowCameraTag : ComponentBase {};

struct FollowCameraComponent
{
    // Save-stable reference to the follow target. Persists across scene
    // reload via GUID; see docs/design/entity_persistence_architecture.md.
    AttachmentRef     target;
    DirectX::XMFLOAT3 offset          = { 0.f, 1.6f, -4.f };  // target-local offset to camera
    DirectX::XMFLOAT3 lookAtOffset    = { 0.f, 1.5f,  0.f };  // target-local point to aim at
    float             damping         = 8.f;                  // position lerp per sec; unused: FollowCameraTickSystem lerps a fixed 0.25/frame
    float             rotationDamping = 8.f;                  // rotation slerp per sec; unused, as damping
    bool              useLookAt       = true;
};

struct AimCameraTag : ComponentBase {};

struct AimCameraComponent
{
    // Pivot entity (usually the player root). Save-stable via GUID.
    AttachmentRef     target;
    DirectX::XMFLOAT3 pivotOffset  = { 0.f, 1.5f, 0.f }; // target-local pivot offset
    float             yaw          = 0.f;         // radians, driven by input/lua
    float             pitch        = 0.f;
    float             distance     = 3.5f;        // camera back-distance from pivot
    float             pitchMin     = -1.4f;       // ~-80 deg
    float             pitchMax     =  1.0f;       // ~+57 deg
    bool              collisionAvoid = true;
    float             probeRadius  = 0.20f;       // sweep sphere radius
};

// CameraShakeComponent - additive shake layer. Trauma decays each frame;
// CameraShakeTickSystem turns trauma^2 into translation + rotation noise on the
// Live pose AFTER resolve. Lives on the channel's LiveCamera entity, not on VCams:
// shake belongs to the view, not to a source.
struct CameraShakeComponent
{
    float             trauma         = 0.f;     // 0..1
    float             falloffPerSec  = 1.5f;    // trauma drain per second
    DirectX::XMFLOAT3 posAmplitude   = { 0.10f, 0.10f, 0.05f };
    DirectX::XMFLOAT3 rotAmplitude   = { 0.04f, 0.04f, 0.04f };
    float             frequency      = 18.f;
    uint32_t          seed           = 0x9E3779B9u;
    float             time           = 0.f;     // running phase accumulator
};

// ==== Channel-side resolved data ====

// CameraStackChannelSingleton - on a dedicated channel entity next to
// LiveCameraComponent. CameraStackTickSystem rebuilds `entries` every frame from
// VirtualCameraComponent + VCamPriorityComponent + VCamBlendComponent;
// CameraResolveTickSystem reads it. Separate systems so a channel can be resolved
// without re-sorting.
struct CameraStackChannelSingleton
{
    struct StackEntry
    {
        Entity     vcam         = NullEntity;
        int        priority     = 0;
        float      weight       = 1.f;
        float      currentBlend = 0.f;
    };

    CameraChannelId         channelId = Camera::kMainChannel;
    std::vector<StackEntry> entries;          // sorted by priority desc each frame
    bool                    requestHardCut = false; // set by Camera.HardCutTo, consumed by Resolve
};

// LiveCameraComponent - the per-channel resolved camera, the only camera the
// renderer reads; built by CameraResolveTickSystem from the stack entries.
// `historyValid = false` for one frame after a hard cut (and on the first frame):
// temporal passes (TAA, XeGTAO, VolumetricFog, SSR) reset their history.
struct LiveCameraComponent
{
    DirectX::XMFLOAT3 position     = { 0.f, 0.f, 0.f };
    DirectX::XMFLOAT4 rotation     = { 0.f, 0.f, 0.f, 1.f };
    DirectX::XMFLOAT3 forward      = { 0.f, 0.f, 1.f };
    float             fov          = DirectX::XM_PI / 3.f;
    float             nearZ        = 0.1f;
    float             farZ         = 200.f;
    CameraChannelId   channelId    = Camera::kMainChannel;
    bool              historyValid = false;       // false on hard cut / first frame
    bool              hasPrev      = false;       // false until at least one valid frame elapsed
};

// ==== Inline helpers (system implementations and Lua bindings) ====

namespace Camera
{
    // Map a 0..1 progress through a blend curve. Custom maps to Linear (no curve
    // table).
    inline float ApplyBlendCurve(BlendCurve c, float t) noexcept
    {
        if (t <= 0.f) return 0.f;
        if (t >= 1.f) return 1.f;
        switch (c)
        {
        case BlendCurve::Linear:    return t;
        case BlendCurve::EaseIn:    return t * t;
        case BlendCurve::EaseOut:   return 1.f - (1.f - t) * (1.f - t);
        case BlendCurve::EaseInOut: return t * t * (3.f - 2.f * t);  // smoothstep
        case BlendCurve::Custom:    return t;
        }
        return t;
    }
}
