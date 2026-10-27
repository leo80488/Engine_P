#pragma once

// ComponentReflection.h - the Reflect::Descriptor<T> specializations for every component the
// generic reflection-driven inspector edits.
// Conventions:
//   - One REFLECT_BEGIN/END block per type, grouped by header; enum option arrays sit above the
//     block that uses them.
//   - Components with hand-written widgets (texture pickers, conditional UI, action buttons)
//     reflect only their data fields and add the rest in RegisterReflectedComponent's postDraw.
//   - Components whose UI is mostly custom logic (MaterialComponent, hierarchy pickers, ...) have
//     no descriptor and keep a RegisterComponentEditor block.
//   - A field whose UI does not fit the kind grid names an editor drawer by id (REFLECT_CUSTOM;
//     Reflection/ReflectDrawers.h): the descriptor is data, so EngineCore compiles it too.
// No ImGui: EngineCore registers these descriptors (Reflect::RegisterCoreDescriptors), so Game.exe's
// catalog and tools see the fields; the editor draws them (Reflection/ReflectionEditor.h).

#include "Reflection/Reflect.h"

#include "ECS/Components.h"
#include "ECS/HierarchyComponents.h"
#include "ECS/CameraStackComponents.h"
#include "ECS/CameraMotionComponent.h"
#include "ECS/MotionDirectorComponent.h"
#include "Audio/AudioComponents.h"
#include "ECS/BillboardComponent.h"
#include "ECS/BillboardFXComponent.h"
#include "ECS/SkyboxComponent.h"
#include "ECS/AtmosphereComponent.h"
#include "ECS/WorldEnvironment.h"
#include "ECS/CloudComponent.h"
#include "ECS/HeightFogComponent.h"
#include "ECS/VolumetricFogComponent.h"
#include "ECS/LocalFogVolumeComponent.h"
#include "ECS/TODComponents.h"
#include "ECS/ReflectionProbeComponent.h"
#include "ECS/ProxyVolumeComponent.h"
#include "ECS/DDGIComponents.h"
#include "ECS/TrailComponent.h"
#include "ECS/AnimFXGateComponent.h"
#include "ECS/BeamComponent.h"
#include "ECS/PhysicsComponents.h"
#include "ECS/TagComponent.h"
#include "ECS/TagRegistry.h"
#include "ECS/CharacterControllerComponent.h"
#include "ECS/PlayerComponent.h"
#include "ECS/PerObjectShadowComponent.h"
#include "ECS/HairShadowCasterComponent.h"
#include "ECS/FaceOverlayComponent.h"
#include "ECS/DissolveComponent.h"
#include "ECS/AIIntentComponent.h"
#include "ECS/PerceptionComponent.h"
#include "Nav/NavComponents.h"
#include "ECS/AnimationComponents.h"
#include "ECS/AnimGraphComponent.h"
#include "ECS/IKRigComponent.h"
#include "ECS/LocomotionComponents.h"
#include "ECS/CharacterAnimDriver.h"
#include "ECS/CharacterIntentComponent.h"
#include "ECS/FaceShadingComponent.h"
#include "ECS/HeadTrackComponent.h"
#include "ECS/FacialAnimationComponent.h"

#include "ECS/ParticleComponent.h"
#include "ECS/TerrainComponent.h"
#include "ECS/GrassComponent.h"
#include "ECS/WaterComponent.h"
#include "ECS/WaterInteractionComponent.h"
#include "ECS/SocketSystem.h"
#include "ECS/FollowComponents.h"
#include "ECS/PostProcessVolumeComponent.h"
#include "AI/AIComponents.h"
#include "AI/AIBehavior.h"
#include "Physics/ChainPhysicsSystem.h"
#include "Scripting/ScriptComponent.h"
#include "Sequencer/SequenceComponents.h"
#include "UI/UICanvas.h"
#include "UI/UIDocument.h"      // UI::UIDocument (CvDoc)
#include "UI/WorldSpaceUI.h"

#include <DirectXMath.h>
#include <unordered_map>

// ==== HierarchyComponents: pure data ====

// VisibilityComponent: author-intent flags + per-view selection mask.
// flags (uint8) packs Visible / CastShadow / RenderInMainPass; viewMask (uint32) has one bit per
// ViewBit (main camera, each shadow cascade, reflection probe, secondary view). Both are bit sets:
// one checkbox per bit, since a drag-int cannot set single bits.

constexpr Reflect::EnumOption kVisibilityFlagBits[] = {
    { VisibilityComponent::Visible,          "Visible" },
    { VisibilityComponent::CastShadow,       "Cast Shadow" },
    { VisibilityComponent::RenderInMainPass, "Render In Main Pass" },
};
// Bit 6 is Preview::kSecondaryViewBit: the camera preview panel and the in-game PiP. The label names
// what vanishes when it is cleared.
constexpr Reflect::EnumOption kViewMaskBits[] = {
    { (int)ViewBit::MainCamera,      "Main Camera" },
    { (int)ViewBit::ShadowCascade0,  "Shadow Cascade 0" },
    { (int)ViewBit::ShadowCascade1,  "Shadow Cascade 1" },
    { (int)ViewBit::ShadowCascade2,  "Shadow Cascade 2" },
    { (int)ViewBit::ShadowCascade3,  "Shadow Cascade 3" },
    { (int)ViewBit::ReflectionProbe, "Reflection Probe" },
    { (int)ViewBit::SecondaryView,   "Secondary View (PiP / camera preview)" },
};
// Main+Shadow keeps SecondaryView set: MainCamera without bit 6 is visible to the player but hidden
// in every PiP and camera preview, which this preset does not promise. "Shadow Only" has neither
// MainCamera nor SecondaryView.
constexpr Reflect::FlagPreset kViewMaskPresets[] = {
    { ViewBit::All, "All" },
    { 0u,           "None" },
    { ViewBit::MainCamera | ViewBit::SecondaryView | ViewBit::ShadowAny, "Main+Shadow" },
    { ViewBit::ShadowAny, "Shadow Only" },
};

REFLECT_BEGIN(VisibilityComponent)
    REFLECT_FIELD(flags,           "Flags",     FlagBits(kVisibilityFlagBits), Unit("bitmask"))
    REFLECT_FIELD(viewMask,        "View Mask", FlagBits(kViewMaskBits), Presets(kViewMaskPresets), Unit("bitmask"),
                  Tip("Per-view participation: the views this entity is drawn in."))
    // Recomputed every frame by TransformSystem from the parent chain; not saved.
    REFLECT_BOOL  (inheritedHidden, "Inherited Hidden", Runtime, ReadOnly)
REFLECT_END()

REFLECT_BEGIN(RenderLayer)
    REFLECT_UINT(mask, "Layer Mask", 0u, 0xFFFFFFFFu)
REFLECT_END()

REFLECT_BEGIN(LocalAabb)
    REFLECT_FLOAT3(min, "Min", -1e6f, 1e6f)
    REFLECT_FLOAT3(max, "Max", -1e6f, 1e6f)
REFLECT_END()

// ==== CameraComponent: lens / view-projection parameters ====

REFLECT_BEGIN(CameraComponent)
    REFLECT_ANGLE (fov,      "FOV",     10.f,   170.f, Unit("radians"), Hard(0, 3.141592653589793), LuaWrite)
    REFLECT_FLOAT_FMT(nearZ, "Near Z",  0.001f, 10.f,   "%.3f", 0.001f, Unit("world_unit"), Hard(0, kFloatMax), LuaWrite)
    REFLECT_FLOAT_FMT(farZ,  "Far Z",   1.f,    5000.f, "%.0f", 1.f, Unit("world_unit"), Hard(0, kFloatMax), LuaWrite)REFLECT_END()

// ==== CameraControllerComponent: FPS controller state (yaw / pitch + tuning) ====
// The camera pose itself is edited through LocalTransform.

constexpr Reflect::EnumOption kCameraModeOptions[] = {
    { (int)CameraControllerComponent::Mode::Free,        "Free (fly-cam)"  },
    { (int)CameraControllerComponent::Mode::ThirdPerson, "Third-Person"    },
    { (int)CameraControllerComponent::Mode::FirstPerson, "First-Person"    },
};

REFLECT_BEGIN(CameraControllerComponent)
    REFLECT_DRAG(yaw,                 "Yaw (rad)",         -3.14159f, 3.14159f)
    REFLECT_DRAG(pitch,               "Pitch (rad)",       -1.5f,     1.5f)
    REFLECT_FLOAT_FMT(mouseSensitivity, "Mouse Sensitivity",  0.0001f,  0.05f,  "%.4f", 0.0001f)
    REFLECT_FLOAT_FMT(moveSpeed,        "Move Speed",         0.1f,     1000.f, "%.1f", 0.1f)
    REFLECT_ENUM(mode,                  "Mode",              kCameraModeOptions)
    // Follow-mode fields only relevant when mode != Free.
    REFLECT_IF([](const void* o) {
        return static_cast<const CameraControllerComponent*>(o)->mode
            != CameraControllerComponent::Mode::Free;
    })
        // followTarget is an AttachmentRef: its picker is drawn in EditorLayer's postDraw
        // (RegisterDefaultEditors), not here.
        REFLECT_FLOAT(thirdPersonDistance, "Third-Person Distance",   0.1f, 50.f)
        REFLECT_FLOAT3(headOffset,        "FP Head Offset",          -10.f, 10.f)
        REFLECT_FLOAT3(shoulderOffset,    "TP Shoulder Offset",      -10.f, 10.f)
        REFLECT_BOOL  (cameraCollisionEnabled, "TP Wall-Collision Probe")
        REFLECT_FLOAT (cameraProbeRadius, "TP Probe Radius",          0.01f, 1.0f)
        REFLECT_FLOAT_FMT(followLag,      "TP Follow Lag (s, 0=off)", 0.0f, 1.0f, "%.3f", 0.005f)
        REFLECT_FLOAT_FMT(distanceLag,    "TP Distance Lag (s)",      0.0f, 1.0f, "%.3f", 0.005f)
    REFLECT_ENDIF()
REFLECT_END()

// ==== CameraData: generic camera parameters (not the FPS controls) ====

REFLECT_BEGIN(CameraData)
    REFLECT_ANGLE(fov,                  "FOV",                  10.f,  170.f)
    REFLECT_FLOAT(nearZ,                "Near Z",               0.001f, 10.f)
    REFLECT_FLOAT(farZ,                 "Far Z",                1.f,    10000.f)
    REFLECT_FLOAT(aspectRatioOverride,  "Aspect Override (0=auto)", 0.f, 8.f)
REFLECT_END()

// ==== Camera stack: virtual camera, pose, priority, blend, live ====

REFLECT_BEGIN(VirtualCameraComponent)
    REFLECT_ANGLE    (fov,            "FOV",             10.f, 170.f)
    REFLECT_FLOAT_FMT(nearZ,          "Near Z",         0.001f, 10.f,   "%.3f", 0.001f)
    REFLECT_FLOAT_FMT(farZ,           "Far Z",          1.f,    5000.f, "%.0f", 1.f)
    REFLECT_FLOAT    (aspectOverride, "Aspect Override (0=auto)", 0.f, 8.f)
    REFLECT_UINT     (channelId,      "Channel Id (hashed)",      0u, 0xFFFFFFFFu)
REFLECT_END()

REFLECT_BEGIN(CameraPoseComponent)
    REFLECT_FLOAT3(position, "Position", -1e5f, 1e5f)
    REFLECT_FLOAT4(rotation, "Rotation (xyzw)", -1.f, 1.f)
REFLECT_END()

REFLECT_BEGIN(VCamPriorityComponent)
    REFLECT_INT  (priority, "Priority",   -1000, 10000)
    REFLECT_FLOAT(weight,   "Weight",     0.f,   1.f)
    REFLECT_BOOL (enabled,  "Enabled")
REFLECT_END()

constexpr Reflect::EnumOption kBlendCurveOptions[] = {
    { (int)BlendCurve::Linear,    "Linear"    },
    { (int)BlendCurve::EaseIn,    "Ease In"   },
    { (int)BlendCurve::EaseOut,   "Ease Out"  },
    { (int)BlendCurve::EaseInOut, "Ease In/Out" },
    { (int)BlendCurve::Custom,    "Custom"    },
};

constexpr Reflect::EnumOption kBlendStateOptions[] = {
    { (int)BlendState::Inactive,    "Inactive"    },
    { (int)BlendState::BlendingIn,  "Blending In" },
    { (int)BlendState::Active,      "Active"      },
    { (int)BlendState::BlendingOut, "Blending Out" },
};

REFLECT_BEGIN(VCamBlendComponent)
    REFLECT_FLOAT(blendInDuration,  "Blend-In Duration (s)",  0.f, 5.f)
    REFLECT_FLOAT(blendOutDuration, "Blend-Out Duration (s)", 0.f, 5.f)
    REFLECT_ENUM (curveIn,          "Curve In",  kBlendCurveOptions)
    REFLECT_ENUM (curveOut,         "Curve Out", kBlendCurveOptions)
    REFLECT_DRAG(currentBlend,    "Current Blend",  0.f, 1.f)
    REFLECT_ENUM (state,            "State", kBlendStateOptions)
REFLECT_END()

REFLECT_BEGIN(FollowCameraComponent)
    // target is an AttachmentRef: its picker is in the postDraw of EditorLayer::RegisterDefaultEditors.
    REFLECT_FLOAT3(offset,          "Offset (target-local)",  -50.f, 50.f)
    REFLECT_FLOAT3(lookAtOffset,    "LookAt Offset",          -50.f, 50.f)
    REFLECT_FLOAT (damping,         "Position Damping",       0.f, 100.f)
    REFLECT_FLOAT (rotationDamping, "Rotation Damping",       0.f, 100.f)
    REFLECT_BOOL  (useLookAt,       "Use LookAt")
REFLECT_END()

REFLECT_BEGIN(AimCameraComponent)
    // target is an AttachmentRef: its picker is in the postDraw of EditorLayer::RegisterDefaultEditors.
    REFLECT_FLOAT3(pivotOffset,    "Pivot Offset",    -50.f, 50.f)
    REFLECT_DRAG(yaw,            "Yaw (rad)",       -3.14159f, 3.14159f)
    REFLECT_DRAG(pitch,          "Pitch (rad)",     -1.5f, 1.5f)
    REFLECT_FLOAT (distance,       "Distance",        0.f, 50.f)
    REFLECT_FLOAT (pitchMin,       "Pitch Min",       -1.5f, 1.5f)
    REFLECT_FLOAT (pitchMax,       "Pitch Max",       -1.5f, 1.5f)
    REFLECT_BOOL  (collisionAvoid, "Wall-Collision Probe")
    REFLECT_FLOAT (probeRadius,    "Probe Radius",    0.01f, 1.f)
REFLECT_END()

REFLECT_BEGIN(CameraShakeComponent)
    REFLECT_DRAG(trauma,        "Trauma",                  0.f, 1.f)
    REFLECT_FLOAT (falloffPerSec, "Falloff /sec",            0.f, 10.f)
    REFLECT_FLOAT3(posAmplitude,  "Position Amplitude",      0.f, 1.f)
    REFLECT_FLOAT3(rotAmplitude,  "Rotation Amplitude (rad)", 0.f, 0.5f)
    REFLECT_FLOAT (frequency,     "Frequency (Hz-ish)",      0.f, 60.f)
REFLECT_END()

REFLECT_BEGIN(LiveCameraComponent)
    REFLECT_FLOAT3(position,     "Position",         -1e5f, 1e5f)
    REFLECT_FLOAT4(rotation,     "Rotation (xyzw)",  -1.f, 1.f)
    REFLECT_FLOAT3(forward,      "Forward",          -1.f, 1.f)
    REFLECT_ANGLE (fov,          "FOV",              10.f, 170.f)
    REFLECT_FLOAT (nearZ,        "Near Z",           0.001f, 10.f)
    REFLECT_FLOAT (farZ,         "Far Z",            1.f, 10000.f)
    REFLECT_UINT  (channelId,    "Channel Id",       0u, 0xFFFFFFFFu)
    REFLECT_BOOL  (historyValid, "History Valid")
REFLECT_END()

// ==== BillboardComponent ====

constexpr Reflect::EnumOption kBillboardModeOptions[] = {
    { (int)BillboardMode::Opaque,      "Opaque (GBuffer)"      },
    { (int)BillboardMode::AlphaClip,   "Alpha Clip (GBuffer)"  },
    { (int)BillboardMode::Transparent, "Transparent (Forward)" },
    { (int)BillboardMode::Additive,    "Additive (Forward)"    },
};

REFLECT_BEGIN(BillboardComponent)
    REFLECT_ENUM (mode,        "Mode",            kBillboardModeOptions)
    REFLECT_FLOAT(worldSize,   "World Size",      0.001f, 100.f)
    REFLECT_BOOL (fixedSize,   "Fixed Screen Size")
    REFLECT_FLOAT(fixedPixels, "Pixel Size",      1.f,    512.f)
REFLECT_END()

// ==== BillboardFXComponent: animated sprite-sheet billboard ====
// texturePath resolves to a bindless slot at run time (shown in postDraw).

constexpr Reflect::EnumOption kBillboardFXFaceOptions[] = {
    { (int)BillboardFXFace::Spherical,   "Spherical (full)"      },
    { (int)BillboardFXFace::Cylindrical, "Cylindrical (up-axis)" },
};
constexpr Reflect::EnumOption kBillboardFXBlendOptions[] = {
    { (int)BillboardFXBlend::Alpha,    "Alpha"    },
    { (int)BillboardFXBlend::Additive, "Additive" },
};
constexpr Reflect::EnumOption kBillboardFXPlaybackOptions[] = {
    { (int)BillboardFXPlayback::Loop,     "Loop"      },
    { (int)BillboardFXPlayback::Once,     "Once"      },
    { (int)BillboardFXPlayback::PingPong, "Ping-Pong" },
};

REFLECT_BEGIN(BillboardFXComponent)
    REFLECT_STRING_DROP(texturePath, "Sprite Sheet", "ITEX_PATH")
    REFLECT_HEADER   ("Atlas")
    REFLECT_INT      (columns,    "Columns",     1, 64)
    REFLECT_INT      (rows,       "Rows",        1, 64)
    REFLECT_INT      (frameCount, "Frame Count (0=all)", 0, 4096)
    REFLECT_FLOAT    (fps,        "FPS",         0.f, 120.f)
    REFLECT_ENUM     (playback,   "Playback",    kBillboardFXPlaybackOptions)
    REFLECT_BOOL     (playing,    "Playing")
    REFLECT_HEADER   ("Appearance")
    REFLECT_FLOAT    (size,       "Size",        0.001f, 100.f)
    REFLECT_COLOR4   (tint,       "Tint")
    REFLECT_FLOAT    (emissive,   "Emissive",    0.f, 64.f)
    REFLECT_DRAG   (opacity,    "Opacity",     0.f, 1.f)
    REFLECT_ENUM     (face,       "Face Mode",   kBillboardFXFaceOptions)
    REFLECT_ENUM     (blend,      "Blend",       kBillboardFXBlendOptions)
    REFLECT_BOOL     (depthTest,  "Depth Test (occluded by scene)")
REFLECT_END()

// ==== WorldEnvironmentComponent: per-scene sky / atmosphere / IBL mode singleton ====
// Presence means the scene is authored under the optional-sky model; mode selects which backdrop
// and IBL source the renderer uses each frame.

constexpr Reflect::EnumOption kWorldEnvModeOptions[] = {
    { (int)WorldEnvironmentComponent::Mode::None,                 "None (no sky)"          },
    { (int)WorldEnvironmentComponent::Mode::FlatColor,            "Flat Color"             },
    { (int)WorldEnvironmentComponent::Mode::StaticSkybox,         "Static Skybox (cubemap)" },
    { (int)WorldEnvironmentComponent::Mode::ProceduralAtmosphere, "Procedural Atmosphere"  },
};

REFLECT_BEGIN(WorldEnvironmentComponent)
    REFLECT_ENUM  (mode,             "Mode", kWorldEnvModeOptions)
    REFLECT_INFO  ("Selects the sky/IBL backdrop. None/Flat Color need no Sky entity.")
    REFLECT_HEADER("Sky IBL Intensity (unified)")
    REFLECT_DRAG(iblStrength,        "IBL Strength", -1.f, 3.f)
    REFLECT_INFO  ("The sky light's one intensity: sky diffuse + specular fill and the sky every "
                   "traced path (DDGI, RT reflections, ReSTIR GI, the path tracer) sees. "
                   "-1 = inherit legacy Atmosphere value. DDGI/probes have their own scales.")
    REFLECT_HEADER("Background / Ambient (None & Flat Color)")
    REFLECT_COLOR3(flatColor,        "Flat Background")
    REFLECT_COLOR3(ambientColor,     "Ambient Color")
    REFLECT_DRAG(ambientIntensity, "Ambient Intensity", 0.f, 8.f)
REFLECT_END()

// ==== SkyboxComponent: cubemap and sky-material paths + mip count ====
// GPU handles are run-time state shown in EditorLayer's postDraw.

REFLECT_BEGIN(SkyboxComponent)
    REFLECT_STRING(irradiancePath,    "Irradiance Path")
    REFLECT_STRING(radiancePath,      "Radiance Path")
    REFLECT_STRING(skyboxPath,        "Skybox Path")
    REFLECT_STRING(materialPath,      "Dynamic Sky Material")
    REFLECT_STRING(baseMaterialPath,  "Sky Material Instance")
    REFLECT_STRING(cloudMaterialPath, "Cloud Material Instance")
    REFLECT_UINT  (radianceMipLevels, "Radiance Mips", 1u, 16u)
    // Not shown: iblStrength. The renderer does not read it; the sky light's intensity is
    // WorldEnvironmentComponent.iblStrength.
REFLECT_END()

// ==== AtmosphereComponent: procedural sky / time of day / aerial perspective / stars ====
// Source of truth; the Renderer pushes these into SkyIBLPass each frame.

constexpr Reflect::EnumOption kSkyboxSourceOptions[] = {
    { (int)AtmosphereComponent::SkyboxSource::Atmosphere, "Atmosphere (procedural)" },
    { (int)AtmosphereComponent::SkyboxSource::Static,     "Static Cubemap"          },
};

// Not shown: atmosphereEnabled and skyboxSource duplicate WorldEnvironmentComponent.mode, which
// decides; iblStrength is only the fallback when WorldEnvironmentComponent.iblStrength < 0. All
// three stay serialized for older scene files.
REFLECT_BEGIN(AtmosphereComponent)
    REFLECT_HEADER("Aerial Perspective")
    REFLECT_BOOL  (aerialCompositeEnabled, "Composite Aerial Perspective")
    REFLECT_INFO  ("Distance fog. Depends on world-unit-to-km scale.")
    REFLECT_HEADER("Sun Transmittance")
    REFLECT_BOOL  (sunTransmittanceEnabled,  "Filter Sun Through Atmosphere")
    REFLECT_DRAG  (sunTransmittanceStrength, "Strength", 0.f, 1.f)
    REFLECT_INFO  ("White sun turns warm on the way down, like Unreal's "
                   "Atmosphere Sun Light. Below-horizon sun goes black.")
    REFLECT_HEADER("Stars")
    REFLECT_DRAG(starDensity,            "Star Density",     1.f,   2048.f)
    REFLECT_DRAG(starBrightness,         "Star Brightness",  0.f,   2.f)
REFLECT_END()

// ==== TODConfigComponent: author-set time-of-day parameters (singleton) ====
REFLECT_BEGIN(TODConfigComponent)
    REFLECT_BOOL  (enabled,            "Enable Time-of-Day")
    REFLECT_DRAG(timeOfDay,          "Time (0=midnight, 0.5=noon)", 0.f, 1.f)
    REFLECT_DRAG(timeSpeed,          "Speed (day/sec)",             0.f, 0.5f)
    REFLECT_DRAG(latitudeRad,        "Latitude (rad)",             -1.55f, 1.55f)
    REFLECT_DRAG(sunBrightnessScale, "Sun Brightness",  0.f, 5.f)
    REFLECT_DRAG(moonIntensityScale, "Moon Intensity",  0.f, 1.f)
REFLECT_END()

// ==== TODOutputComponent: computed each frame, shown read-only ====
REFLECT_BEGIN(TODOutputComponent)
    REFLECT_INFO  ("Computed by TODEvaluationSystem. Read-only diagnostics.")
    REFLECT_FLOAT3(activeDirection, "Active Light Dir", -1.f, 1.f)
    REFLECT_COLOR3(activeColor,     "Active Light Color")
    REFLECT_FLOAT3(sunDirection,    "Sun Dir (geom)",   -1.f, 1.f)
    REFLECT_COLOR3(sunColor,        "Sun Color (geom)")
    REFLECT_FLOAT3(moonDirection,   "Moon Dir",         -1.f, 1.f)
    REFLECT_COLOR3(moonColor,       "Moon Disk Color")
    REFLECT_BOOL  (isMoonActive,    "Moon Drives Lighting")
    REFLECT_BOOL  (moonDiskVisible, "Moon Disk Visible")
REFLECT_END()

// ==== TODLightComponent: a local light switched by the sun (put it on the light) ====
REFLECT_BEGIN(TODLightComponent)
    REFLECT_INFO  ("Switches this light with the time of day, and drives a linked "
                   "bulb material's emissive off the SAME blend. Works in Edit mode: "
                   "drag Time-of-Day > Time and watch it respond.")
    REFLECT_BOOL  (enabled,      "Enabled")
    REFLECT_DRAG  (onIntensity,  "Intensity (lit)",   0.f, 500.f)
    REFLECT_DRAG  (emissiveOn,   "Emissive (lit)",    0.f, 64.f)
    REFLECT_DRAG  (emissiveOff,  "Emissive (dark)",   0.f, 64.f)
    REFLECT_DRAG  (duskStart,    "Dusk Start (sun altitude)", -1.f, 1.f)
    REFLECT_DRAG  (duskEnd,      "Dusk End (sun altitude)",   -1.f, 1.f)
    REFLECT_DRAG  (warmupTime,   "Fade Seconds",      0.f, 30.f)
    REFLECT_STRING(lampName,     "Lamp Entity (blank = parent)")
    REFLECT_STRING(bulbMaterial, "Bulb Material (substring; blank = light only)")
REFLECT_END()

// ==== SunLightTag / MoonLightTag: empty markers ====
// Attach one to a DirectionalLight entity to put it under TOD control.
REFLECT_BEGIN(SunLightTag)
    REFLECT_INFO("TOD will drive this directional light as the SUN.")
REFLECT_END()

REFLECT_BEGIN(MoonLightTag)
    REFLECT_INFO("TOD will drive this directional light as the MOON.")
REFLECT_END()

// ==== CloudComponent: volumetric cloud authoring knobs (singleton) ====
// Sun direction and colour come from TODOutputComponent at run time.
REFLECT_BEGIN(CloudComponent)
    REFLECT_BOOL  (enabled,         "Enabled")
    REFLECT_HEADER("Layer Altitude")
    REFLECT_FLOAT (bottomAltitude,  "Bottom (m)", 0.f,   20000.f)
    REFLECT_FLOAT (topAltitude,     "Top (m)",    0.f,   20000.f)
    REFLECT_HEADER("Shape")
    REFLECT_DRAG(coverage,        "Coverage",   0.f,   1.f)
    REFLECT_DRAG(density,         "Density",    0.f,   4.f)
    REFLECT_FLOAT_FMT(noiseScale,   "Base Noise Scale", 0.0001f, 0.01f, "%.5f", 0.0001f)
    REFLECT_FLOAT_FMT(detailNoiseScale, "Detail Noise Scale", 0.0002f, 0.02f, "%.5f", 0.0002f)
    REFLECT_DRAG(detailStrength,  "Detail Erosion", 0.f, 1.f)
    REFLECT_FLOAT_FMT(weatherScale, "Weather Scale", 0.000002f, 0.0002f, "%.6f", 0.000002f)
    REFLECT_DRAG(cloudTypeBias,   "Type Bias (stratus<->cumulus)", -1.f, 1.f)
    REFLECT_DRAG(anvilBias,       "Anvil Bias", 0.f, 1.f)
    REFLECT_HEADER("Wind")
    REFLECT_FLOAT3(windDirection,   "Wind Direction", -1.f, 1.f)
    REFLECT_DRAG(windSpeed,       "Wind Speed (m/s)", 0.f, 100.f)
    REFLECT_HEADER("Lighting")
    REFLECT_DRAG(anisotropy,      "Phase Forward G", 0.f, 0.99f)
    REFLECT_DRAG(phaseBackG,      "Phase Back G",   -0.99f, 0.f)
    REFLECT_DRAG(phaseBlend,      "Phase Blend",     0.f, 1.f)
    REFLECT_DRAG(silverIntensity, "Silver Intensity", 0.f, 2.f)
    REFLECT_DRAG(silverSpread,    "Silver Spread",   0.01f, 0.99f)
    REFLECT_DRAG(extinction,      "Extinction (1/m)", 0.f, 0.3f)
    REFLECT_DRAG(ambientStrength, "Ambient Fill",   0.f, 2.f)
    REFLECT_COLOR3(ambientTint,     "Ambient Tint")
    REFLECT_COLOR3(cloudColor,      "Cloud Albedo")
    REFLECT_HEADER("Cloud Shadow")
    REFLECT_BOOL  (cloudShadowEnabled, "Cast Shadow On World")
    REFLECT_DRAG(cloudShadowStrength, "Shadow Strength", 0.f, 2.f)
    REFLECT_FLOAT(cloudShadowExtent,  "Shadow Extent (m)", 1000.f, 100000.f)
    REFLECT_INFO("Shadow the clouds cast on the world along the sun. Off skips "
                 "the bake AND releases the map. Extent is the half-size of "
                 "the square footprint centred on the camera.")
    REFLECT_HEADER("Quality")
    REFLECT_DRAG(maxSteps,        "Max Ray Steps", 24.f, 256.f)
    REFLECT_BOOL  (temporalUpsample, "Temporal Reconstruction")
    REFLECT_INFO("Reprojects and accumulates the quarter-res raymarch across "
                 "frames. Off = grainier but free of reprojection artefacts.")
REFLECT_END()

// ==== HeightFogComponent: UE-style exponential height fog (singleton, Sky entity) ====
// Sun direction and colour come from LightCB at run time.
REFLECT_BEGIN(HeightFogComponent)
    REFLECT_BOOL  (enabled,             "Enabled")
    REFLECT_HEADER("Density")
    REFLECT_FLOAT_FMT(fogDensity,       "Fog Density (1/m)",    0.f, 0.05f, "%.5f", 0.0001f)
    REFLECT_FLOAT_FMT(fogHeightFalloff, "Height Falloff (1/m)", 0.f, 1.f,   "%.4f", 0.001f)
    REFLECT_FLOAT (fogHeight,           "Fog Height (m)",   -1000.f, 5000.f)
    REFLECT_FLOAT (startDistance,       "Start Distance (m)",   0.f, 5000.f)
    REFLECT_DRAG(maxOpacity,          "Max Opacity",          0.f, 1.f)
    REFLECT_HEADER("Inscattering")
    REFLECT_COLOR3(fogColor,            "Fog Color")
    REFLECT_DRAG(sunInscatterIntensity, "Sun Inscatter",      0.f, 4.f)
    REFLECT_DRAG(anisotropy,          "Sun Phase G",          0.f, 0.99f)
    REFLECT_HEADER("Path Tracing")
    REFLECT_COLOR3(albedo,              "Medium Albedo")
    REFLECT_INFO("In the path tracers the fog is a medium lit by the scene's lights: Fog "
                 "Color, Sun Inscatter, Start Distance and Max Opacity do not apply there.")
REFLECT_END()

// ==== VolumetricFogComponent: the scene's global froxel medium (singleton) ====
// Its presence overrides the player's Volumetric Fog option and the scene .ippc fog values.
REFLECT_BEGIN(VolumetricFogComponent)
    REFLECT_BOOL  (enabled,            "Enabled")
    REFLECT_INFO  ("Present = this scene owns its fog; the player's Volumetric Fog "
                   "option and the scene .ippc fog values are ignored.")
    REFLECT_HEADER("Medium")
    REFLECT_FLOAT_FMT(density,         "Density (1/m)",        0.f, 1.f,  "%.4f", 0.0005f)
    REFLECT_COLOR3(albedo,             "Albedo")
    REFLECT_DRAG  (anisotropy,         "Anisotropy (g)",      -0.95f, 0.95f)
    REFLECT_HEADER("Height")
    REFLECT_FLOAT (baseHeight,         "Base Height (m)",  -1000.f, 5000.f)
    REFLECT_FLOAT_FMT(heightFalloff,   "Height Falloff (1/m)", 0.f, 2.f,  "%.4f", 0.001f)
    REFLECT_HEADER("Lighting")
    REFLECT_COLOR3(ambientColor,       "Ambient Color")
    REFLECT_DRAG  (ambientIntensity,   "Ambient Intensity",    0.f, 8.f)
    REFLECT_COLOR3(emissive,           "Emissive Color")
    REFLECT_DRAG  (emissiveIntensity,  "Emissive Intensity",   0.f, 16.f)
    REFLECT_INFO  ("Sun shafts: give the sun a Volumetric Light component.")
    REFLECT_HEADER("Range & Quality")
    REFLECT_FLOAT (startDistance,      "Start Distance (m)",   0.05f, 10.f)
    REFLECT_FLOAT (viewDistance,       "View Distance (m)",    10.f, 1000.f)
    REFLECT_BOOL  (godRays,            "Light Shafts (raymarch)")
    REFLECT_BOOL  (temporal,           "Temporal Reprojection")
    REFLECT_IF([](const void* o) {
        return static_cast<const VolumetricFogComponent*>(o)->temporal;
    })
        REFLECT_DRAG(temporalBlend,    "Temporal Blend",       0.01f, 1.f)
    REFLECT_ENDIF()
REFLECT_END()

// ==== LocalFogVolumeComponent: bounded fog pocket ====
// Bounds follow the Transform (Box = scale half-extents, Sphere = ellipsoid radii = scale).
constexpr Reflect::EnumOption kLocalFogShapeOptions[] = {
    { (int)LocalFogShape::Box,    "Box"    },
    { (int)LocalFogShape::Sphere, "Sphere" },
};

REFLECT_BEGIN(LocalFogVolumeComponent)
    REFLECT_BOOL  (enabled,            "Enabled")
    REFLECT_ENUM  (shape,              "Shape", kLocalFogShapeOptions)
    REFLECT_INFO  ("Bounds follow Transform (Box = scale half-extents, Sphere = scale radii).")
    REFLECT_FLOAT_FMT(density,         "Density (1/m)",        0.f, 4.f,  "%.4f", 0.001f)
    REFLECT_COLOR3(albedo,             "Albedo")
    REFLECT_COLOR3(emissive,           "Emissive Color")
    REFLECT_DRAG  (emissiveIntensity,  "Emissive Intensity",   0.f, 16.f)
    REFLECT_DRAG  (edgeFade,           "Edge Fade",            0.f, 1.f)
    REFLECT_FLOAT_FMT(heightFalloff,   "Height Falloff (1/m)", 0.f, 4.f,  "%.3f", 0.005f)
REFLECT_END()

// ==== ReflectionProbeComponent: data fields only ====
// The "Bake This Probe" button and its status live in EditorLayer's postDraw: they need the Renderer.

// Shape combos for reflection probes and proxies: .value is written verbatim into the
// enum-class field (uint32 underlying).
constexpr Reflect::EnumOption kInfluenceShapeOptions[] = {
    { 0, "Box"    },
    { 1, "Sphere" },
};
constexpr Reflect::EnumOption kProxyShapeOptions[] = {
    { 0, "Infinite"     },
    { 1, "Sphere"       },
    { 2, "Oriented Box" },
};

REFLECT_BEGIN(ReflectionProbeComponent)
    REFLECT_INFO   ("Capture point = entity GlobalTransform + Capture Offset.")
    REFLECT_INFO   ("Where one is needed: glossy surfaces (glass, polished floors) reflect a probe that covers them. Without one - and without RT reflections - they reflect the SKY scaled by DDGI's sky visibility, which cannot block a street's sides, so shop windows mirror the open sky (IndirectLighting_Environment_Review_Plan.md sec.11.11).")
    REFLECT_INFO   ("The bake captures MeshLibRef geometry only (primitive shapes bake as sky). With DDGI on, the capture is scaled down where the surroundings are darker than at its capture point (D-IE5).")
    REFLECT_BOOL   (showVizSphere,        "Show Viz Sphere")
    REFLECT_INFO   ("Per-probe. Hiding the ball never stops the bake.")
    REFLECT_ENUM   (influenceShape,       "Influence Shape", kInfluenceShapeOptions)
    REFLECT_HEADER ("Influence (Box)")
    REFLECT_FLOAT3 (influenceHalfExtents, "Half-Extents",  0.01f, 1000.f)
    REFLECT_FLOAT3 (fadePos,              "Fade +X/+Y/+Z", 0.0f,  1000.f)
    REFLECT_FLOAT3 (fadeNeg,              "Fade -X/-Y/-Z", 0.0f,  1000.f)
    REFLECT_HEADER ("Influence (Sphere)")
    REFLECT_FLOAT  (influenceOuterRadius, "Outer Radius",  0.01f, 1000.f)
    REFLECT_FLOAT  (influenceInnerRadius, "Inner Radius",  0.0f,  1000.f)
    REFLECT_HEADER ("Capture")
    REFLECT_FLOAT3 (captureOffsetLS,      "Capture Offset", -1000.f, 1000.f)
    REFLECT_DRAG   (intensity,            "Intensity", 0.f, 8.f)
    REFLECT_INFO   ("Proxy Volume / Priority / Face Resolution in the footer below.")
    REFLECT_HEADER ("Realtime")
    REFLECT_BOOL   (realtime,             "Realtime")
    REFLECT_UINT   (tickIntervalFrames,   "Tick Interval (frames)", 1u, 3600u, Speed(Reflect::BoundedIntDragSpeed(1, 3600)))
REFLECT_END()

// ==== ProxyVolumeComponent: parallax proxy, shareable across probes ====

REFLECT_BEGIN(ProxyVolumeComponent)
    REFLECT_INFO   ("Parallax proxy geometry. Shareable across probes via AttachmentRef.")
    REFLECT_ENUM   (shape,          "Shape", kProxyShapeOptions)
    REFLECT_FLOAT3 (centerOffsetLS, "Center Offset", -1000.f, 1000.f)
    REFLECT_FLOAT3 (halfExtents,    "Half-Extents (Box)", 0.01f, 1000.f)
    REFLECT_FLOAT  (sphereRadius,   "Radius (Sphere)", 0.01f, 1000.f)
    REFLECT_INFO   ("Entity scale is ignored — box uses these metres directly.")
REFLECT_END()

// ==== VolumetricLightComponent (LightData's sibling) ====

REFLECT_BEGIN(VolumetricLightComponent)
    REFLECT_BOOL  (enabled,        "Enabled")
    REFLECT_DRAG(intensityScale, "Intensity Scale", 0.f, 10.f)
    REFLECT_INFO  ("Boost only the in-fog brightness; surface lit")
    REFLECT_INFO  ("intensity remains the LightData value.")
REFLECT_END()

// ==== DDGIVolumeComponent ====
// The DXR readiness badge and the total-probe warning live in EditorLayer's postDraw: they read
// live Renderer state.

// Rays per probe is a fixed choice of three power-of-two budgets: the ray-data buffer, the
// bucketed adaptive dispatch (DDGI_RAY_BUCKET_COUNT) and the Halton block sweep work best there,
// and values in between add noise without a quality tier. EnumOption.value is written verbatim
// into the uint field (ReflectionEditor::DrawEnum), so these are the actual ray counts.
constexpr Reflect::EnumOption kDDGIRayCountOptions[] = {
    { 64,  "64 (Performance)" },
    { 128, "128 (Default)"    },
    { 256, "256 (Quality)"    },
};

REFLECT_BEGIN(DDGIVolumeComponent)
    REFLECT_HEADER ("Volume")
    REFLECT_FLOAT3 (origin,            "Origin",             -10000.f, 10000.f)
    REFLECT_FLOAT3 (extent,            "Half-Extent",        0.1f,     1000.f)
    REFLECT_HEADER ("Probe Grid")
    REFLECT_UINT   (probeCountsX,      "Probes X",           4u, 32u)
    REFLECT_UINT   (probeCountsY,      "Probes Y",           4u, 32u)
    REFLECT_UINT   (probeCountsZ,      "Probes Z",           4u, 32u)
    REFLECT_ENUM   (raysPerProbe,      "Rays / Probe",       kDDGIRayCountOptions)
    REFLECT_UINT   (probeUpdateDivisor,"Update Divisor",     1u, 8u)
    REFLECT_INFO   ("Trace/relight only 1/N of the probes per frame (spatially interleaved). GPU cost scales ~1/N. The relight blend is auto-compensated so convergence TIME is unchanged. 2-4 is visually free at hysteresis >= 0.92; raise it before cutting probe counts.")
    REFLECT_INFO   ("This is a FLOOR. The camera-relevance scheduler raises it on its own as the camera moves away (Near >=2, Far >=6) and skips the volume entirely past 4x its extent.")
    REFLECT_FLOAT  (updateCompensation, "Update Compensation", 0.0f, 1.0f)
    REFLECT_INFO   ("What the Update Divisor's blend compensation preserves. 1 = response TIME (historical): bigger steps, less often - but steady-state noise variance grows ~N x, so at N=4 hysteresis 0.99 filters like 0.96. 0 = the NOISE FLOOR: as smooth as N=1, response N x slower. Static lighting (night scenes) wants 0; fast-changing lighting wants 1.")
    REFLECT_HEADER ("Scheduling")
    REFLECT_DRAG_INT(priority,         "Priority",           0, 8)
    REFLECT_INFO   ("Artist override for the camera-relevance scheduler: >0 promotes this volume one tier, so a hero interior keeps updating at full rate after the camera steps outside. Costs GPU time - use it on the few volumes that matter.")
    REFLECT_INFO   ("Note: the effective divisor is also capped at 0.5/(1-Hysteresis). Past that the relight EMA saturates into a plain overwrite and distant volumes flicker, so raising the divisor further would hurt rather than help.")
    REFLECT_HEADER ("Sampling")
    REFLECT_FLOAT  (hysteresis,        "Hysteresis",         0.5f,   0.999f)
    REFLECT_FLOAT  (changeResponseBoost, "Change Response",  0.f,    8.f)
    REFLECT_INFO   ("0 = off (plain EMA). Above 0, a probe texel whose lighting actually CHANGED blends up to (1+x) faster, so lights switching on / the sun moving land quickly without lowering Hysteresis everywhere. Per-texel and feed-forward, so it cannot form the feedback loop that got adaptive ray count disabled.")
    REFLECT_INFO   ("Opt-in on purpose: with ray jitter on, MC noise can sit near the trigger and a permanently-engaged boost is just a lower hysteresis (= flicker). Raise until changes feel prompt, then back off until a static shot is steady. Try 2-4.")
    REFLECT_FLOAT  (rotationJitterScale, "Ray Jitter Scale", 0.0f,   1.0f)
    REFLECT_INFO   ("Lower = steadier sun lighting (less flicker on hard shadows), slight bias. ~0.3-0.5 for stable directional GI.")
    REFLECT_FLOAT  (normalBias,        "Normal Bias",        0.f,    2.f)
    REFLECT_FLOAT  (viewBias,          "View Bias",          0.f,    2.f)
    REFLECT_FLOAT  (biasSpacingScale,  "Bias / Probe Spacing", 0.f,  1.f)
    REFLECT_INFO   ("Adds biasSpacingScale * min(probeSpacing) to BOTH biases above. This is the one to use: it tracks the cell size automatically, so changing probe counts or extent never needs a bias re-tune. Try ~0.25.")
    REFLECT_INFO   ("Soft blotches on large flat floors = bias too small for the spacing. The absolute defaults (0.25 / 0.10) were tuned for a sub-metre interior grid; at 3.5 m spacing they are ~1/14 of a cell and the Chebyshev self-occlusion guard stops working. Raise NORMAL bias, not VIEW bias - view bias makes GI swim as the camera moves (they only look equivalent looking straight down at a floor).")
    REFLECT_FLOAT  (boundaryFadeRatio, "Boundary Fade",      0.01f,  0.5f)
    REFLECT_INFO   ("The outer fraction of the half-extent (per axis) where the volume fades out. A surface in that band gets (1 - fade) of its indirect light from the SKY fallback instead of the probes: less bounce, more unoccluded sky. Keep what the camera sees out of it - prefer ~0.1 and a larger volume.")
    REFLECT_INFO   ("Trap: a tall volume whose bottom face sits just under the street puts the street itself in the band. Tokyo HybridGI2 at 0.5 gave the street a weight of 0.28 and shadows at 0.76x the path tracer; 0.1 -> 1.0x (IndirectLighting_Environment_Review_Plan.md sec.11.10).")
    REFLECT_FLOAT  (perceptualBlendWeight, "Perceptual Blend", 0.0f, 1.0f)
    REFLECT_INFO   ("1 = reference gamma-2 blend across the 8 probes; 0 = plain linear. The gamma blend is a POWER MEAN, so it always reads darker than linear by an amount that grows with the contrast between neighbouring probes (~21% at 10x, ~40% at 100x).")
    REFLECT_INFO   ("Symptom of leaving it at 1: soft rounded DARK LOBES straddling sharp sun/shadow boundaries, which get smaller with more probes but never vanish. Trade-off: lowering it can bring back probe-cell seams in very dark areas, which is what the gamma blend was for.")
    REFLECT_HEADER ("Tinting")
    REFLECT_COLOR3 (diffuseTint,       "Diffuse Tint")
    REFLECT_FLOAT  (diffuseScale,      "Diffuse Scale",      0.f,    4.f)
    REFLECT_HEADER ("Phase 3 (toggles persisted; runtime impact pending)")
    REFLECT_BOOL   (enableRelocation,     "Probe Relocation")
    REFLECT_BOOL   (enableClassification, "Probe Classification")
    REFLECT_BOOL   (enableAdaptiveRayBudget, "Adaptive Ray Budget")
    REFLECT_INFO   ("OFF by default. Varies rays per probe by convergence.")
    REFLECT_INFO   ("This mechanism previously caused slow whole-scene brightness")
    REFLECT_INFO   ("breathing and was disabled after three failed fixes; the")
    REFLECT_INFO   ("re-implementation audits at full budget every 16 updates to")
    REFLECT_INFO   ("break the feedback loop, but is NOT long-run validated.")
    REFLECT_INFO   ("Prefer Update Divisor 2-4 for the same saving.")
    REFLECT_HEADER ("Debug")
    REFLECT_BOOL   (debugDraw,         "Debug Draw")
REFLECT_END()

// ==== IndirectLightingSettingsComponent: scene-singleton knobs ====

// Not shown: reflectionProbePriorityOverDDGI / useDDGIForRoughSpecularFallback (nothing reads
// them; still serialized for scene-file compatibility).
// DDGI / SSGI Enabled are the scene's intent: what runs is that AND the player's features.ddgi /
// features.ssgi.
REFLECT_BEGIN(IndirectLightingSettingsComponent)
    REFLECT_BOOL  (ddgiEnabled,           "DDGI Enabled")
    REFLECT_INFO  ("Whether this scene uses DDGI. The player's DDGI setting can still turn it off.")
    REFLECT_FLOAT (ddgiDiffuseScale,      "DDGI Diffuse Scale",   0.f, 8.f)
    // 0 = no AO on DDGI (trust probe visibility), 1 = full AO (double occlusion), ~0.4 recommended.
    // The sky-fallback path always gets full AO.
    REFLECT_FLOAT (ddgiAONearFieldStrength, "DDGI Near-Field AO",  0.f, 1.f)
    REFLECT_HEADER("Screen-Space GI (near field)")
    REFLECT_BOOL  (ssgiEnabled,           "SSGI Enabled")
    REFLECT_INFO  ("Whether this scene uses SSGI. The player's SSGI setting can still turn it off.")
    REFLECT_INFO  ("Near-field half of the hybrid: DDGI keeps the far field and")
    REFLECT_INFO  ("the two split the hemisphere, so OFF is bit-identical.")
    REFLECT_INFO  ("Owns its Hi-Z; reflections borrow it when both are enabled.")
    REFLECT_FLOAT (ssgiIntensity,         "SSGI Intensity",      -1.f, 1.f)
    REFLECT_INFO  ("Kill switch / A-B fade only. Brightness is DDGI Diffuse Scale,")
    REFLECT_INFO  ("shared with DDGI on purpose - a second scale would band.")
    REFLECT_FLOAT (ssgiMaxDistance,       "SSGI Max Distance",    0.5f, 20.f)
    REFLECT_INFO  ("World metres. Keep it below probe spacing - this tier exists")
    REFLECT_INFO  ("to supply what the probe grid cannot resolve.")
    REFLECT_UINT  (ssgiRayCount,          "SSGI Rays / Pixel",    2u, 16u)
    REFLECT_INFO  ("Legacy mode only. Reservoir mode emits one half-res ray (0.25 full-res rpp).")
    REFLECT_BOOL  (ssgiReservoirEnabled,  "SSGI Reservoir Reuse")
    REFLECT_UINT  (ssgiReservoirSpatialCandidates, "Reservoir Neighbours", 0u, 8u)
    REFLECT_UINT  (ssgiReservoirMaxM,     "Reservoir Max M",      1u, 63u)
    REFLECT_UINT  (ssgiReservoirMaxAge,   "Reservoir Max Age",    1u, 63u)
    REFLECT_UINT  (ssgiReservoirValidationSteps, "Reservoir Visibility Taps", 4u, 6u)
    REFLECT_FLOAT (ssgiThickness,         "SSGI Thickness",       0.01f, 1.f)
    REFLECT_INFO  ("Hit-uncertainty gate. Raise if thin geometry loses bounce;")
    REFLECT_INFO  ("lower if rays accept surfaces they actually passed behind.")
    REFLECT_UINT  (ssgiDebugMode,         "SSGI Debug Mode",      0u, 4u)
    REFLECT_INFO  ("1 = calibration (frame must NOT change - proves units).")
    REFLECT_INFO  ("2 = MAGENTA binding test.  3 = coverage.  4 = near term.")
    REFLECT_INFO  ("Run 2 FIRST: it ignores the trace entirely, so a screen that")
    REFLECT_INFO  ("does not turn magenta means the fold itself is broken.")
    REFLECT_HEADER("NPR")
    REFLECT_FLOAT (nprMaxBrightness,      "NPR Max Brightness",   0.f, 8.f)
    REFLECT_INFO  ("Scene-wide HDR luminance ceiling for NPR pixels. 0 = no ceiling.")
    REFLECT_INFO  ("Caps blown-out toon pixels so bloom / auto-exposure stay stable;")
    REFLECT_INFO  ("min()s with each material's own Max Brightness (tighter wins).")
    REFLECT_HEADER("Reflection Probes")
    REFLECT_FLOAT (reflectionProbeBakeAmbient, "Probe Bake Ambient", 0.f, 2.f)
    REFLECT_INFO  ("Scales sky ambient baked into probe cubemaps (x iblStrength).")
    REFLECT_INFO  ("Lower to darken over-bright interior probe captures. Re-bake to apply.")
REFLECT_END()

// ==== TrailComponent: ribbon trail ====
// trailSlot / lastSamplePos / hasLastSample are run-time state; only the visual and sampling
// parameters are shown.

constexpr Reflect::EnumOption kTrailPointCountOptions[] = {
    { 1, "1 — Legacy Camera-Facing" },
    { 2, "2 — Weapon Base+Tip" },
    { 3, "3 Points" },
    { 4, "4 Points" },
};
constexpr Reflect::EnumOption kTrailUVModeOptions[] = {
    { 0, "Age" },
    { 1, "Arc Length (head-anchored)" },
    { 2, "Arc — World Anchored (in-place)" },
};
constexpr Reflect::EnumOption kTrailBlendOptions[] = {
    { 0, "Alpha" },
    { 1, "Additive" },
};

// Trail is shape only: shading (texture / blend / custom shader / params) lives on the entity's
// MaterialComponent. TrailSystem moves the component's own look fields (texturePath / blendMode /
// customShaderPath / customParams) into a new MaterialComponent once and clears them, so they are
// not shown here. The gradient colours and HDR intensity stay: they are the ribbon's per-trail
// vertex colour (head -> tail over lifetime, which one material colour cannot express), multiplied
// by the material.
REFLECT_BEGIN(TrailComponent)
    REFLECT_BOOL  (enabled,           "Enabled")
    REFLECT_ENUM  (pointCount,        "Point Count",          kTrailPointCountOptions)
    REFLECT_FLOAT (maxAge,            "Max Age (s)",          0.05f,  30.f)
    REFLECT_FLOAT (minSampleDistance, "Min Sample Distance",  0.f,    5.f)
    REFLECT_IF    ([](const void* o){ return ((const TrailComponent*)o)->pointCount <= 1; })
        REFLECT_FLOAT (width,         "Width",                0.001f, 5.f)
    REFLECT_ENDIF ()
    REFLECT_IF    ([](const void* o){ return ((const TrailComponent*)o)->pointCount >= 2; })
        REFLECT_HEADER("Ribbon Points (local space, base → tip)")
        REFLECT_FLOAT3(localPoints[0], "P0 (Base)",           -100.f, 100.f)
        REFLECT_FLOAT3(localPoints[1], "P1",                  -100.f, 100.f)
    REFLECT_ENDIF ()
    REFLECT_IF    ([](const void* o){ return ((const TrailComponent*)o)->pointCount >= 3; })
        REFLECT_FLOAT3(localPoints[2], "P2",                  -100.f, 100.f)
    REFLECT_ENDIF ()
    REFLECT_IF    ([](const void* o){ return ((const TrailComponent*)o)->pointCount >= 4; })
        REFLECT_FLOAT3(localPoints[3], "P3",                  -100.f, 100.f)
    REFLECT_ENDIF ()
    REFLECT_HEADER("Gradient (vertex color, head \xE2\x86\x92 tail)")
    REFLECT_COLOR4(startColor,        "Start Color (head)")
    REFLECT_COLOR4(endColor,          "End Color (tail)")
    REFLECT_FLOAT (hdrIntensity,      "HDR Intensity",        0.f,    32.f)
    REFLECT_INFO  ("Per-trail color the shading multiplies. Custom-shader materials receive white x intensity instead (drive color from the material's own params).")
    REFLECT_HEADER("UV / Temporal")
    REFLECT_ENUM  (uvMode,            "UV Mode (U)",          kTrailUVModeOptions)
    REFLECT_FLOAT (uvTileLength,      "UV Tile Length (m)",   0.f,    50.f)
    REFLECT_INFO  ("Arc-length U: 0 = stretch once over the live trail; >0 = metres per texture repeat.")
    REFLECT_BOOL  (excludeFromTAA,    "Exclude From TAA")
    REFLECT_INFO  ("Additive + fast swings should exclude from TAA (trails write no velocity).")
    REFLECT_IF    ([](const void* o){ return ((const TrailComponent*)o)->pointCount >= 2; })
        REFLECT_HEADER("Smoothing")
        REFLECT_UINT  (subdivisions,      "Subdivisions",     1u, 4u)
        REFLECT_FLOAT (subSampleAngleDeg, "Substep Angle (deg)", 0.f, 90.f)
        REFLECT_INFO  ("Extra samples are inserted when the blade rotates more than this per frame. 0 = off.")
    REFLECT_ENDIF ()
REFLECT_END()

// ==== AnimFXGateComponent: hands this entity's effect group to the animation ====
// lastApplied / hasApplied are run-time latches and stay hidden.

REFLECT_BEGIN(AnimFXGateComponent)
    REFLECT_STRING(group,            "Group")
    REFLECT_INFO  ("Must match the FXGate notify's Group on the attack clip. Blank on either side = match anything.")
    REFLECT_BOOL  (includeFollowers, "Include Followers")
    REFLECT_INFO  ("Also drives child entities and anything FollowEntity-bound to this one \xE2\x80\x94 that is how a prefab's Trail / Particle Emitter are attached.")
    REFLECT_BOOL  (forceOn,          "Force On (look-dev)")
    REFLECT_INFO  ("Ignore the animation and keep the effects live. Turn this on while dragging the weapon around to tune the look, off for gameplay.")
REFLECT_END()


// ==== LocalTransform: TRS, rotation edited as Euler degrees ====

// The keys are the Transform authoring contract's (Entity.set "Transform").
REFLECT_BEGIN(LocalTransform)
    REFLECT_FLOAT3(translation, "Translation",  -1e6f, 1e6f, Key("position"), Unit("world_unit"),
                   Hard(-kFloatMax, kFloatMax), LuaWrite)
    REFLECT_FIELD (rotation,    "Rotation",     AsQuat, Unit("unitless"), Hard(-kFloatMax, kFloatMax), LuaWrite)
    REFLECT_FLOAT3(scale,       "Scale",        0.001f, 1000.f, Unit("unitless"), Hard(-kFloatMax, kFloatMax), LuaWrite)
REFLECT_END()

// ==== LightData: the light type drives which fields are visible ====

constexpr Reflect::EnumOption kLightTypeOptions[] = {
    { (int)LightType::Directional, "Directional" },
    { (int)LightType::Point,       "Point"       },
    { (int)LightType::Spot,        "Spot"        },
};

REFLECT_BEGIN(LightData)
    REFLECT_ENUM (type,         "Type",         kLightTypeOptions, Unit("enum"), Hard(0, 2), LuaWrite)
    REFLECT_INFO ("Position + aim: use LocalTransform (not stored in Light)")
    // No direction field: orientation is the entity's forward (+Z) axis, edited
    // with the normal rotate gizmo like every other transform. See LightUtil.h.
    REFLECT_IF([](const void* o) {
        const auto* l = static_cast<const LightData*>(o);
        return l->type == LightType::Directional || l->type == LightType::Spot;
    })
        REFLECT_INFO("Aim with the Rotate gizmo — the beam follows +Z.")
    REFLECT_ENDIF()
    // Radius: Point / Spot only (directional is infinite).
    REFLECT_IF([](const void* o) {
        const auto* l = static_cast<const LightData*>(o);
        return l->type == LightType::Point || l->type == LightType::Spot;
    })
        REFLECT_FLOAT(radius, "Radius", 0.1f, 500.f, Unit("world_unit"), Hard(0, kFloatMax), LuaWrite)
        // Emitter size: the ray-traced paths' penumbra (raster ignores it).
        REFLECT_FLOAT(sourceRadius, "Source Radius", 0.f, 10.f, Unit("world_unit"), Hard(0, kFloatMax), LuaWrite)
    REFLECT_ENDIF()
    // Cone angle: Spot only.
    REFLECT_IF([](const void* o) {
        return static_cast<const LightData*>(o)->type == LightType::Spot;
    })
        REFLECT_ANGLE(spotAngle, "Cone Angle", 1.f, 90.f, Unit("radians"), Hard(0, 1.5707963267948966), LuaWrite)
    REFLECT_ENDIF()
    REFLECT_HEADER ("Shading")
    REFLECT_COLOR3 (color,     "Color", Unit("linear_rgb"), Hard(0, kFloatMax), LuaWrite)
    REFLECT_BOOL   (enabled,   "Enabled", Unit("unitless"), LuaWrite)
    REFLECT_FLOAT  (intensity, "Intensity", 0.f, 1000.f, Unit("engine_intensity"), Hard(0, kFloatMax), LuaWrite)
    // Cast shadow: Spot (spot atlas) and Point (cube atlas), both opt-in and capacity-limited.
    // Directional always uses CSM.
    REFLECT_IF([](const void* o) {
        const auto t = static_cast<const LightData*>(o)->type;
        return t == LightType::Spot || t == LightType::Point;
    })
        REFLECT_BOOL(castsShadow, "Cast Shadow", Unit("unitless"), LuaWrite)
    REFLECT_ENDIF()
REFLECT_END()

// ==== MeshHandle: primitive picker ====

// Must stay in sync with Resource::PrimitiveMeshType (ProceduralMesh.h): .value is the enum value.
constexpr Reflect::EnumOption kPrimitiveMeshOptions[] = {
    { 0, "Cube"   },
    { 1, "Sphere" },
    { 2, "Cone"   },
    { 3, "Plane"  },
    { 4, "Torus"  },
};

REFLECT_BEGIN(MeshHandle)
    REFLECT_ENUM(gpuMeshID, "Primitive", kPrimitiveMeshOptions)
REFLECT_END()

REFLECT_BEGIN(PerObjectShadowComponent)
    REFLECT_DRAG(radiusScale, "Radius Scale", 0.25f, 4.0f)
REFLECT_END()

REFLECT_BEGIN(DissolveComponent)
    // amount is run-time state, but exposing it makes the inspector field a live preview (the
    // renderer reads the component every frame).
    REFLECT_DRAG(amount, "Amount", 0.0f, 1.0f)
    REFLECT_COLOR3(edgeColor, "Edge Color")
    REFLECT_DRAG(edgeWidth, "Edge Width", 0.0f, 0.3f)
    REFLECT_DRAG(edgeIntensity, "Edge Intensity", 0.0f, 32.0f)
    REFLECT_DRAG(noiseScale, "Noise Scale", 0.05f, 16.0f)
REFLECT_END()

constexpr Reflect::EnumOption kHairShadowModeOptions[] = {
    { 0, "Screen Mark" },
    { 1, "Shadow Map"  },
    { 2, "Both"        },
};

REFLECT_BEGIN(HairShadowCasterComponent)
    REFLECT_ENUM(mode, "Mode", kHairShadowModeOptions)
    REFLECT_DRAG(strength, "Strength", 0.0f, 1.0f)
    REFLECT_DRAG(fitScale, "Fit Scale (Map)", 0.25f, 4.0f)
    REFLECT_DRAG(screenOffsetX, "Mark Offset X", -8.0f, 8.0f)
    REFLECT_DRAG(screenOffsetY, "Mark Offset Y", -8.0f, 8.0f)
    REFLECT_DRAG(depthPush, "Mark Depth Push", 0.0f, 8.0f)
REFLECT_END()

REFLECT_BEGIN(FaceOverlayComponent)
    REFLECT_DRAG(alpha, "Alpha", 0.0f, 1.0f)
    REFLECT_DRAG(maxHairDistance, "Max Hair Distance", 0.005f, 0.3f)
REFLECT_END()

// ==== PhysicsComponents: RigidBody (motion-dependent mass), Collider (shape-dependent extents) ====

constexpr Reflect::EnumOption kRigidBodyMotionOptions[] = {
    { (int)RigidBodyComponent::Motion::Static,    "Static"    },
    { (int)RigidBodyComponent::Motion::Kinematic, "Kinematic" },
    { (int)RigidBodyComponent::Motion::Dynamic,   "Dynamic"   },
};


// "Group/Name": the bits of one group share a row.
constexpr Reflect::EnumOption kAxisLockBits[] = {
    { RigidBodyComponent::LockTranslationX, "Freeze Position/X" },
    { RigidBodyComponent::LockTranslationY, "Freeze Position/Y" },
    { RigidBodyComponent::LockTranslationZ, "Freeze Position/Z" },
    { RigidBodyComponent::LockRotationX,    "Freeze Rotation/X" },
    { RigidBodyComponent::LockRotationY,    "Freeze Rotation/Y" },
    { RigidBodyComponent::LockRotationZ,    "Freeze Rotation/Z" },
};
constexpr Reflect::FlagPreset kAxisLockPresets[] = {
    { 0u, "None" },
    { RigidBodyComponent::LockRotationAll, "Lock Rot", true },
    { RigidBodyComponent::LockTranslationZ | RigidBodyComponent::LockRotationX | RigidBodyComponent::LockRotationY,
      "Plane 2D (XY)" },
};

REFLECT_BEGIN(RigidBodyComponent)
    REFLECT_ENUM (motion,         "Motion",          kRigidBodyMotionOptions, Unit("enum"), Hard(0, 2), LuaWrite)
    REFLECT_IF([](const void* o) {
        return static_cast<const RigidBodyComponent*>(o)->motion
            == RigidBodyComponent::Motion::Dynamic;
    })
        REFLECT_FLOAT_FMT(mass, "Mass (kg)", 0.001f, 10000.f, "%.3f", 0.1f, Unit("kg"), Hard(0, kFloatMax), LuaWrite)
    REFLECT_ENDIF()
    REFLECT_FLOAT_FMT(linearDamping,  "Linear Damping",  0.f,  1.f, "%.3f", 0.005f, Key("linDamp"), Unit("per_second"), Hard(0, kFloatMax), LuaWrite)
    REFLECT_FLOAT_FMT(angularDamping, "Angular Damping", 0.f,  1.f, "%.3f", 0.005f, Key("angDamp"), Unit("per_second"), Hard(0, kFloatMax), LuaWrite)
    REFLECT_FLOAT_FMT(friction,       "Friction",        0.f,  2.f, "%.2f", 0.01f, Unit("unitless"), Hard(0, kFloatMax), LuaWrite)
    REFLECT_FLOAT_FMT(restitution,    "Restitution",     0.f,  1.f, "%.2f", 0.01f, Unit("unitless"), Hard(0, 1), LuaWrite)
    REFLECT_FLOAT_FMT(gravityFactor,  "Gravity Factor", -2.f,  5.f, "%.2f", 0.05f, Key("gravity"), Unit("multiplier"), Hard(-kFloatMax, kFloatMax), LuaWrite)
    // Axis locks only affect Dynamic bodies: hidden on Static / Kinematic so nobody toggles inert
    // checkboxes.
    REFLECT_IF([](const void* o) {
        return static_cast<const RigidBodyComponent*>(o)->motion
            == RigidBodyComponent::Motion::Dynamic;
    })
        REFLECT_FIELD(lockedAxes, "Constraints", FlagBits(kAxisLockBits), Presets(kAxisLockPresets),
                      Unit("bitmask"), Hard(0, 63), LuaWrite,
                      Tip("Axis locks freeze the body's velocity on the locked axis."))
    REFLECT_ENDIF()
REFLECT_END()

constexpr Reflect::EnumOption kColliderShapeOptions[] = {
    { (int)ColliderComponent::Shape::Box,     "Box"     },
    { (int)ColliderComponent::Shape::Sphere,  "Sphere"  },
    { (int)ColliderComponent::Shape::Capsule, "Capsule" },
    { (int)ColliderComponent::Shape::Mesh,    "Mesh (trimesh, static only)" },
    { (int)ColliderComponent::Shape::ConvexHull, "Convex Hull" },
    // The model's authored asset collision (its BO / PR records).
    { (int)ColliderComponent::Shape::MeshAsset,  "Mesh Asset (the model's collision)" },
};

// Collision-layer dropdown (Unity Layer analogue). Auto = derived from the motion type, with
// isTrigger colliders routed to Sensing.
constexpr Reflect::EnumOption kCollisionLayerOptions[] = {
    { (int)ColliderComponent::CollisionLayer::Auto,        "Auto (motion / trigger)" },
    { (int)ColliderComponent::CollisionLayer::StaticEnv,   "Static Env"   },
    { (int)ColliderComponent::CollisionLayer::DynamicProp, "Dynamic Prop" },
    { (int)ColliderComponent::CollisionLayer::Character,   "Character"    },
    { (int)ColliderComponent::CollisionLayer::Ragdoll,     "Ragdoll"      },
    { (int)ColliderComponent::CollisionLayer::HurtBox,     "Hurt Box"     },
    { (int)ColliderComponent::CollisionLayer::AttackBox,   "Attack Box"   },
    { (int)ColliderComponent::CollisionLayer::Projectile,  "Projectile"   },
    { (int)ColliderComponent::CollisionLayer::Sensing,     "Sensing"      },
};

// CharacterControllerComponent: KCC tuning. Run-time state (velocity, isGrounded, groundEntity,
// timeInAir, bodyId, generation) is not shown: every physics step overwrites it.
REFLECT_BEGIN(CharacterControllerComponent)
    REFLECT_FLOAT_FMT(capsuleRadius,     "Capsule Radius",      0.05f, 5.0f, "%.3f", 0.01f)
    REFLECT_FLOAT_FMT(capsuleHalfHeight, "Capsule Half-Height", 0.05f, 5.0f, "%.3f", 0.01f)
    REFLECT_FLOAT_FMT(stepHeight,        "Step Height",         0.0f,  2.0f, "%.3f", 0.01f)
    REFLECT_ANGLE    (maxSlopeRad,       "Max Walkable Slope",  0.f,   80.f)
    REFLECT_FLOAT_FMT(skinWidth,         "Skin Width",          0.0f,  0.1f, "%.4f", 0.001f)
    REFLECT_FLOAT_FMT(mass,              "Mass (kg)",           1.0f,  500.f,"%.1f", 0.5f)
    REFLECT_FLOAT_FMT(pushStrength,      "Push Strength",       0.0f,  4.0f, "%.2f", 0.05f)
    REFLECT_FLOAT_FMT(gravity,           "Gameplay Gravity",    -80.f, 0.f,  "%.2f", 0.5f)
    REFLECT_FLOAT_FMT(maxFallSpeed,      "Max Fall Speed",      1.0f,  200.f,"%.1f", 1.0f)
    REFLECT_FLOAT_FMT(jumpSpeed,         "Jump Speed",          0.0f,  30.f, "%.2f", 0.1f)
    REFLECT_HEADER("Momentum (0 = instant, the legacy feel)")
    REFLECT_FLOAT_FMT(groundAccel,       "Ground Accel (m/s2)", 0.0f, 100.f, "%.1f", 0.5f)
    REFLECT_FLOAT_FMT(groundDecel,       "Ground Decel (m/s2)", 0.0f, 100.f, "%.1f", 0.5f)
    REFLECT_FLOAT_FMT(airAccel,          "Air Accel (m/s2)",    0.0f, 100.f, "%.1f", 0.5f)
    REFLECT_INFO("Air Accel > 0 REPLACES PlayerComponent.Air Control: it steers the speed you jumped with instead of scaling the target.")
    REFLECT_BOOL  (queryBody, "Findable by Queries")
REFLECT_END()

constexpr Reflect::EnumOption kPlayerFacingModeOptions[] = {
    { (int)PlayerFacingMode::FaceMovement, "Face Movement (turn into input)" },
    { (int)PlayerFacingMode::FaceCamera,   "Face Camera (strafe / 8-way)"    },
    { (int)PlayerFacingMode::Locked,       "Locked (never auto-rotate)"      },
};

// PlayerComponent: high-level avatar tuning. cameraEntity is an AttachmentRef (its picker is in
// EditorLayer's postDraw); jumpBufferTimer is run-time only.
REFLECT_BEGIN(PlayerComponent)
    REFLECT_FLOAT_FMT(walkSpeed,      "Walk Speed",       0.0f, 50.f, "%.2f", 0.1f)
    REFLECT_FLOAT_FMT(runSpeed,       "Run Speed",        0.0f, 50.f, "%.2f", 0.1f)
    REFLECT_DRAG   (airControl,     "Air Control",      0.0f,  1.0f)
    REFLECT_ENUM   (facingMode,     "Facing Mode",      kPlayerFacingModeOptions)
    REFLECT_FLOAT_FMT(turnRate,       "Turn Rate (rad/s)",0.0f, 40.f, "%.2f", 0.25f)
    REFLECT_FLOAT_FMT(facingMinSpeed, "Facing Min Speed", 0.0f,  5.0f,"%.2f", 0.05f)
    REFLECT_FLOAT_FMT(coyoteTime,     "Coyote Time (s)",  0.0f,  0.5f, "%.3f", 0.005f)
    REFLECT_FLOAT_FMT(jumpBufferTime, "Jump Buffer (s)",  0.0f,  0.5f, "%.3f", 0.005f)
REFLECT_END()



constexpr Reflect::EnumOption kAIGoalOptions[] = {
    { (int)AIGoal::Idle,        "Idle" },
    { (int)AIGoal::Patrol,      "Patrol" },
    { (int)AIGoal::Investigate, "Investigate" },
    { (int)AIGoal::Attack,      "Attack" },
    { (int)AIGoal::Flee,        "Flee" },
    { (int)AIGoal::TakeCover,   "Take Cover" },
    { (int)AIGoal::Follow,      "Follow" },
    { (int)AIGoal::UseObject,   "Use Object" },
};
constexpr Reflect::EnumOption kNavFacingModeOptions[] = {
    { (int)NavFacingMode::FaceMovement, "Face Movement" },
    { (int)NavFacingMode::FaceTarget,   "Face Target" },
    { (int)NavFacingMode::Manual,       "Manual" },
};

REFLECT_BEGIN(AIIntentComponent)
    // Strategic-layer intent: BehaviorTreeSystem writes it; AITacticalSystem translates it into
    // NavAgent.destination + facingMode.
    REFLECT_ENUM  (currentGoal,   "Current Goal", kAIGoalOptions)
    REFLECT_FLOAT3(goalPosition,  "Goal Position", -1.0e6f, 1.0e6f)
    REFLECT_FLOAT (goalPriority,  "Goal Priority", 0.0f, 100.0f)
    REFLECT_FLOAT (goalStartTime, "Goal Start (debug)", 0.0f, 1.0e6f)
REFLECT_END()

REFLECT_BEGIN(NavAgentComponent)
    // Tactical-layer intent + steering tunables. AITacticalSystem writes
    // destination + facingMode, NavAgentSystem reads them and pushes
    // desired velocity onto CharacterControllerComponent.
    REFLECT_BOOL  (hasDestination, "Has Destination")
    REFLECT_FLOAT3(destination,    "Destination", -1.0e6f, 1.0e6f)
    REFLECT_BOOL  (useLookAt,      "Use Look-At")
    REFLECT_FLOAT3(lookTarget,     "Look Target", -1.0e6f, 1.0e6f)
    REFLECT_ENUM  (facingMode,     "Facing Mode",        kNavFacingModeOptions)
    // Steering tunables.
    REFLECT_FLOAT (speed,          "Speed",              0.0f,  100.0f)
    REFLECT_FLOAT (arriveRadius,   "Arrive Radius",      0.01f,  10.0f)
    REFLECT_FLOAT (slowdownRadius, "Slowdown Radius",    0.0f,   20.0f)
    REFLECT_FLOAT (repathDistance, "Repath Distance",    0.0f,  100.0f)
    REFLECT_BOOL  (rotateToFacing, "Rotate To Facing")
    REFLECT_FLOAT (turnRate,       "Turn Rate (rad/s)",  0.0f,   50.0f)
REFLECT_END()

REFLECT_BEGIN(PerceptionComponent)
    REFLECT_UINT(faction,"Faction",0u,65535u)
    // Sensing tunables, read by AI::PerceptionSystem.
    REFLECT_FLOAT (sightRange,   "Sight Range",      0.0f, 200.0f)
    REFLECT_FLOAT (sightConeDeg, "Sight Cone (deg)", 0.0f, 180.0f)
    REFLECT_FLOAT (eyeHeight,    "Eye Height",       0.0f,   3.0f)
    REFLECT_FLOAT (threatMemory, "Threat Memory (s)", 0.0f, 60.0f)
    REFLECT_FLOAT (tickInterval, "Tick Interval (s)", 0.0f,  2.0f)
    REFLECT_BOOL  (requireLineOfSight, "Require Line of Sight")
    REFLECT_FLOAT (hearingRange, "Hearing Range",    0.0f, 100.0f)
    REFLECT_FLOAT (audibleFor,   "Sound Memory (s)", 0.0f, 60.0f)
REFLECT_END()

REFLECT_BEGIN(PerceptibleComponent)
    REFLECT_BOOL(enabled,"Targetable")
    REFLECT_UINT(faction,"Faction",0u,65535u)
    REFLECT_FLOAT (offsetY, "Body Offset Y", 0.0f, 3.0f)
REFLECT_END()


REFLECT_BEGIN(ColliderComponent)
    REFLECT_ENUM (layer, "Layer", kCollisionLayerOptions)
    REFLECT_ENUM (shape, "Shape", kColliderShapeOptions)
    REFLECT_IF([](const void* o) {
        return static_cast<const ColliderComponent*>(o)->shape
            == ColliderComponent::Shape::Box;
    })
        REFLECT_FLOAT3(halfExtents, "Half Extents", 0.001f, 1000.f)
    REFLECT_ENDIF()
    REFLECT_IF([](const void* o) {
        const auto* c = static_cast<const ColliderComponent*>(o);
        return c->shape == ColliderComponent::Shape::Sphere
            || c->shape == ColliderComponent::Shape::Capsule;
    })
        REFLECT_FLOAT(radius, "Radius", 0.001f, 1000.f)
    REFLECT_ENDIF()
    REFLECT_IF([](const void* o) {
        return static_cast<const ColliderComponent*>(o)->shape
            == ColliderComponent::Shape::Capsule;
    })
        REFLECT_FLOAT(halfHeight, "Half-Height (excl. caps)", 0.001f, 1000.f)
    REFLECT_ENDIF()
    REFLECT_IF([](const void* o) {
        const auto* c = static_cast<const ColliderComponent*>(o);
        return c->shape == ColliderComponent::Shape::Mesh
            || c->shape == ColliderComponent::Shape::ConvexHull;
    })
        REFLECT_STRING(meshLibPath,   "Mesh Lib")
        REFLECT_UINT  (meshLibMeshId, "Mesh ID", 0u, 65535u)
    REFLECT_ENDIF()
    REFLECT_IF([](const void* o) {
        return static_cast<const ColliderComponent*>(o)->shape
            == ColliderComponent::Shape::MeshAsset;
    })
        // The model whose asset collision this is (empty = this entity's StaticMesh); its MeshKey is
        // serialized on the same line (meshKey=).
        REFLECT_STRING(assetModel, "Model (.iscn; empty = this StaticMesh)")
    REFLECT_ENDIF()
    REFLECT_BOOL(isTrigger, "Is Trigger (sensor)")
    REFLECT_CUSTOM(offset, "Offset", "colliderOffset")
REFLECT_END()

// ==== TagComponent: gameplay identity tags (Unity Tag analogue, multi-tag) ====
// One custom widget draws the whole editor: the current tags with remove buttons and a picker to
// add more. 8 slots x 31 chars (TagComponent::MAX / LEN).


REFLECT_BEGIN(TagComponent)
    REFLECT_CUSTOM(tags, "Tags", "tags")
REFLECT_END()

// ==== ChainPhysicsComponent: sliders grouped by collapsing headers ====

REFLECT_BEGIN(ChainPhysicsComponent)
    REFLECT_BOOL(enabled, "Enabled")
    REFLECT_COLLAPSE("Chain (global)")
        REFLECT_DRAG   (damping,        "Damping",          0.f, 1.f)
        REFLECT_FLOAT_FMT(gravity,        "Gravity",       -100.f, 0.f, "%.1f", 0.5f)
        REFLECT_DRAG   (maxVelocity,    "Max Velocity",     0.f, 5.f)
        REFLECT_DRAG   (localStiffness, "Local Stiffness",  0.f, 1.f)
        REFLECT_DRAG   (tipHold,        "Tip Hold",         0.f, 1.f)
        REFLECT_FLOAT_FMT(limitAngle,   "Limit Angle",      0.f, 180.f, "%.1f", 0.5f)
        REFLECT_DRAG   (limitTipScale,  "Limit Tip Scale",  0.f, 4.f)
        REFLECT_DRAG   (inertiaScale,   "Inertia",          0.f, 1.f)
        REFLECT_INFO     ("Limit 0=off; Inertia 1=full reaction, 0=rides the root")
        REFLECT_INFO     ("Tip Hold = floor for the root->tip Local Stiffness falloff")
    REFLECT_COLLAPSE_END()
    REFLECT_COLLAPSE("Cloth (global)")
        REFLECT_DRAG   (skirtDamping,        "Damping",         0.f, 1.f)
        REFLECT_FLOAT_FMT(skirtGravity,        "Gravity",      -100.f, 0.f, "%.1f", 0.5f)
        REFLECT_DRAG   (skirtMaxVelocity,    "Max Velocity",    0.f, 5.f)
        REFLECT_DRAG   (skirtLocalStiffness, "Local Stiffness", 0.f, 1.f)
        REFLECT_DRAG   (skirtTipHold,        "Tip Hold",        0.f, 1.f)
        REFLECT_FLOAT_FMT(skirtLimitAngle,   "Limit Angle",     0.f, 180.f, "%.1f", 0.5f)
        REFLECT_DRAG   (skirtLimitTipScale,  "Limit Tip Scale", 0.f, 4.f)
        REFLECT_DRAG   (skirtInertiaScale,   "Inertia",         0.f, 1.f)
        REFLECT_INFO     ("Per-type presets of the Chain block above — same solver math")
    REFLECT_COLLAPSE_END()
    // World-motion inertia: the ENTITY's motion (KCC, turn in place, root-motion
    // driver) reaching the chains. KawaiiPhysics WorldDamping = 1 - these.
    REFLECT_COLLAPSE("World Motion (entity)")
        REFLECT_DRAG   (worldInertiaLocation, "Inertia: location",  0.f, 1.f)
        REFLECT_DRAG   (worldInertiaRotation, "Inertia: rotation",  0.f, 1.f)
        REFLECT_FLOAT_FMT(teleportDistance,   "Teleport distance",  0.f, 10.f,  "%.3f", 0.01f)
        REFLECT_FLOAT_FMT(teleportAngle,      "Teleport angle",     0.f, 180.f, "%.1f", 0.5f)
        REFLECT_INFO     ("Inertia: share of the entity's world motion the chains feel;")
        REFLECT_INFO     ("0 = they ride it rigidly (legacy), 1 = physical. Only CHANGES")
        REFLECT_INFO     ("of motion kick them - start, stop, turn - not a steady walk.")
        REFLECT_INFO     ("Teleport: a step moving more (world units / degrees, per")
        REFLECT_INFO     ("frame) is ignored - no whip. 0 = no threshold.")
        REFLECT_INFO     ("Every group follows these unless it has 'Own world inertia'.")
    REFLECT_COLLAPSE_END()
    // Constraint-bucket materials, not per-type presets: Structural drives the along-chain links of
    // both Chain and Cloth strands; Ring and Shear exist only on a Cloth piece (the hoop between
    // neighbouring chains, and the diagonals across levels). Ring and Shear are tuned independently.
    REFLECT_COLLAPSE("Link Stiffness (global)")
        REFLECT_DRAG   (structuralStiffness, "Structural (along chain)", 0.1f, 1.f)
        REFLECT_DRAG   (ringStiffness,       "Ring (cloth hoop)",        0.1f, 5.f)
        REFLECT_DRAG   (shearStiffness,      "Shear (cloth diagonal)",   0.1f, 5.f)
        REFLECT_DRAG   (skirtHorizDecay,     "Ring+Shear Decay",         0.f,  1.f)
        REFLECT_INFO     ("PBD clamps to 1.0: inert above 1.0 / 1.25 / 2.0 respectively")
        REFLECT_INFO     ("Decay: 0 = tip has no ring/shear hold, 1 = uniform")
    REFLECT_COLLAPSE_END()
    REFLECT_COLLAPSE("Contact (global)")
        REFLECT_FLOAT_FMT(contactSkin,     "Skin (margin x)",  1.f, 1.2f, "%.4f", 0.001f)
        REFLECT_DRAG   (contactFriction,   "Friction",         0.f, 1.f)
        REFLECT_INFO     ("Skin: proportional contact margin. For absolute clearance")
        REFLECT_INFO     ("use Cloth thickness instead — below 1 cloth sinks in.")
        REFLECT_INFO     ("Friction: 0 = slides freely, 1 = grips. Raise it when cloth")
        REFLECT_INFO     ("shivers ALONG the body rather than popping in and out.")
    REFLECT_COLLAPSE_END()
    REFLECT_COLLAPSE("Solver (cost, not look)")
        REFLECT_DRAG_INT(iterations,    "Iterations",       1, 20)
        REFLECT_DRAG_INT(substeps,      "Substeps",         1, 8)
        REFLECT_INFO     ("Capsule collision runs substeps*(1+iterations)+1 times per island")
    REFLECT_COLLAPSE_END()
    REFLECT_COLLAPSE("Ambient Sway (global)")
        REFLECT_FLOAT3 (windDir,        "Direction",       -1.f, 1.f)
        REFLECT_DRAG   (windStrength,   "Strength",         0.f, 0.5f)
        REFLECT_DRAG   (windFreq,       "Frequency (Hz)",   0.f, 5.f)
        REFLECT_DRAG   (windTurbulence, "Turbulence",       0.f, 4.f)
        REFLECT_INFO     ("Strength = tip offset in world units. Needs Local Stiffness > 0.")
    REFLECT_COLLAPSE_END()
    REFLECT_COLLAPSE("Spring Bones (Root)")
        REFLECT_FLOAT_FMT(springStiffness, "Stiffness",  10.f, 300.f, "%.1f", 1.f)
        REFLECT_FLOAT_FMT(springDamping,   "Damping",     1.f,  30.f, "%.1f", 0.5f)
        REFLECT_FLOAT_FMT(springMass,      "Mass",        0.1f, 10.f, "%.2f", 0.1f)
        REFLECT_FLOAT_FMT(springGravity,   "Gravity",   -50.f,  0.f,  "%.1f", 0.5f)
        REFLECT_FLOAT_FMT(springMaxDisp,   "Max Disp",    0.1f, 10.f, "%.2f", 0.1f)
        REFLECT_INFO     ("Virtual root (Chest_Root) + independent bones")
    REFLECT_COLLAPSE_END()
    REFLECT_COLLAPSE("Spring Bones (Child)")
        REFLECT_FLOAT_FMT(springChildStiffness, "Stiffness",  50.f, 500.f, "%.1f", 1.f)
        REFLECT_FLOAT_FMT(springChildDamping,   "Damping",     1.f,  40.f, "%.1f", 0.5f)
        REFLECT_FLOAT_FMT(springChildMass,      "Mass",        0.1f,  5.f, "%.2f", 0.1f)
        REFLECT_FLOAT_FMT(springChildGravity,   "Gravity",   -50.f,  0.f,  "%.1f", 0.5f)
        REFLECT_FLOAT_FMT(springChildMaxDisp,   "Max Disp",    0.1f,  5.f, "%.2f", 0.1f)
        REFLECT_INFO     ("Chest L/R relative to virtual root — needs 2+ Spring groups")
        REFLECT_INFO     ("sharing one Pair id. A lone Spring bone uses the Root block.")
    REFLECT_COLLAPSE_END()
REFLECT_END()

// ==== FaceShadingComponent: NPR SDF face-shadow driver (head-bone axis feed) ====

constexpr Reflect::EnumOption kFaceAxisOptions[] = {
    { 0, "+X" }, { 1, "-X" }, { 2, "+Y" }, { 3, "-Y" }, { 4, "+Z" }, { 5, "-Z" },
};

REFLECT_BEGIN(FaceShadingComponent)
    REFLECT_STRING(headBoneName,  "Head Bone Name")
    REFLECT_INFO  ("PMX/MMD head is usually \"\xE9\xA0\xAD\"; FBX often \"Head\".")
    REFLECT_ENUM  (forwardAxis,   "Face Forward Axis", kFaceAxisOptions)
    REFLECT_ENUM  (rightAxis,     "Face Right Axis",   kFaceAxisOptions)
    REFLECT_INFO  ("PMX head bones often run +Y; pick the axes that point out of / to the side of the face.")
    REFLECT_BOOL  (horizontalize, "Horizontalize (yaw-only sweep)")
    REFLECT_HEADER("Computed (read-only)")
    REFLECT_FLOAT3(faceForward,   "Face Forward", -1.f, 1.f)
    REFLECT_FLOAT3(faceRight,     "Face Right",   -1.f, 1.f)
REFLECT_END()

// ==== Animation: IK rig, anim graph, locomotion ====
// LocomotionComponent is an input-driven 1D blend space (idle / walk / run by speed); its samples
// are edited in the custom section below the fields (drag .ianim from the Asset browser).

REFLECT_BEGIN(IKRigComponent)
    REFLECT_BOOL(enabled, "Enabled")
REFLECT_END()

REFLECT_BEGIN(AnimGraphComponent)
    REFLECT_BOOL(enabled, "Enabled")
    REFLECT_STRING_DROP(graphPath, "Graph", "IANIMGRAPH_PATH")
    REFLECT_STRING_DROP(animSetPath, "Animation Set", "IANIMSET_PATH")
    REFLECT_BOOL(motorRootMotion, "Motor Root Motion")
    REFLECT_DRAG_INT(lod, "Graph LOD", 0, 8)
    REFLECT_DRAG_INT(updateInterval, "Update Interval", 1, 60)
REFLECT_END()

REFLECT_BEGIN(LocomotionComponent)
    REFLECT_BOOL  (enabled,       "Enabled")
    REFLECT_DRAG(speed,         "Speed (input)",  0.f, 10.f)
    REFLECT_DRAG(damping,       "Damping (1/s)",  0.f, 20.f)
    REFLECT_DRAG(playRate,      "Play Rate",      0.f, 3.f)
    REFLECT_INFO  ("Gameplay writes Speed each frame; the system smooths it and phase-syncs the bracketing clips.")
REFLECT_END()

// ==== LocomotionTransitionComponent: start / stop / pivot / turn one-shots over the blend space ====
// The clip list is drawn in the custom section.
REFLECT_BEGIN(LocomotionTransitionComponent)
    REFLECT_BOOL(enabled,      "Enabled")
    REFLECT_DRAG(startSpeed,   "Start Speed (m/s)",   0.f,  5.f)
    REFLECT_DRAG(stopSpeed,    "Stop Speed (m/s)",    0.f,  5.f)
    REFLECT_DRAG(pivotAngle,   "Pivot Angle (deg)",   0.f, 180.f)
    REFLECT_DRAG(turnRate,     "Turn Rate (deg/s)",   0.f, 720.f)
    REFLECT_DRAG(blendIn,      "Blend In (s)",        0.f,  1.f)
    REFLECT_DRAG(blendOut,     "Blend Out (s)",       0.f,  1.f)
    REFLECT_DRAG(cooldown,     "Cooldown (s)",        0.f,  2.f)
    REFLECT_DRAG(releaseLead,  "Release Lead (s)",    0.f,  1.f)
    REFLECT_INFO("Stop Speed must stay BELOW Start Speed — that gap is the hysteresis that keeps a character hovering at the threshold from re-firing every frame.")
REFLECT_END()

// ==== CharacterAnimDriverComponent: the movement -> animation bridge ====
// Alias strings must not contain spaces: the scene parser splits tokens on whitespace.
REFLECT_BEGIN(CharacterAnimDriverComponent)
    REFLECT_INFO("With a LocomotionComponent on the animated entity the blend space owns the pose and every alias below is ignored. They are the fallback for characters WITHOUT a blend space.")
    REFLECT_HEADER("Gait thresholds")
    REFLECT_DRAG (walkThreshold, "Walk Threshold (m/s)", 0.f, 5.f)
    REFLECT_DRAG (runThreshold,  "Run Threshold (m/s)",  0.f, 15.f)
    REFLECT_DRAG (gaitBlend,     "Gait Blend (s)",       0.f, 1.f)
    REFLECT_DRAG (airTimeGrace,  "Air Grace (s)",        0.f, 1.f)
    REFLECT_DRAG (idleRerollSec, "Idle Re-roll (s)",     0.f, 30.f)
    REFLECT_HEADER("Discrete aliases (no blend space)")
    REFLECT_STRING(walkBackAlias,  "Walk Back Alias")
    REFLECT_STRING(walkLeftAlias,  "Walk Left Alias")
    REFLECT_STRING(walkRightAlias, "Walk Right Alias")
    REFLECT_STRING(runAlias,       "Run Alias")
    REFLECT_STRING(airAlias,       "Air Alias")
    REFLECT_HEADER("Action layer")
    REFLECT_DRAG (actionBlend,   "Action Blend (s)",     0.f, 1.f)
    REFLECT_HEADER("Graph bridge")
    REFLECT_STRING(graphSpeedParam, "Speed")
    REFLECT_STRING(graphDirectionParam, "Direction")
    REFLECT_STRING(graphGroundedParam, "IsGrounded")
    REFLECT_STRING(graphVerticalSpeedParam, "VerticalSpeed")
    REFLECT_STRING(graphAirTimeParam, "AirTime")
    REFLECT_STRING(graphTurnRateParam, "TurnRate")
    REFLECT_STRING(graphMovingParam, "IsMoving")
    REFLECT_STRING(graphGaitParam, "Gait")
    REFLECT_STRING(graphActionSlot, "DefaultSlot")
REFLECT_END()

// ==== BrainComponent: possession ====
// Absent = who drives the character is inferred from which components it has.
constexpr Reflect::EnumOption kBrainTypeOptions[] = {
    { (int)BrainType::None,   "None (frozen puppet)" },
    { (int)BrainType::Player, "Player"               },
    { (int)BrainType::AI,     "AI"                   },
};

REFLECT_BEGIN(BrainComponent)
    REFLECT_ENUM(type, "Brain", kBrainTypeOptions)
    REFLECT_INFO("Gates which brain system may write this character's intent. Removing the component restores the legacy 'whichever brain matches the components' behaviour.")
REFLECT_END()

// ==== AnimLayerComponent: masked overlay layers (upper-body aim over locomotion) ====
// Layers are edited in the custom section (drag .ianim from the Asset browser).

REFLECT_BEGIN(AnimLayerComponent)
    REFLECT_INFO  ("Each layer plays a clip through a bone mask (UpperBody = Spine subtree from the humanoid profile).")
REFLECT_END()

// ==== HeadTrackComponent: head look-at toward the active camera ====
// Reuses kFaceAxisOptions (0 = +X .. 5 = -Z) for the forward / up axis pickers.

REFLECT_BEGIN(HeadTrackComponent)
    REFLECT_BOOL  (enabled,        "Enabled")
    REFLECT_STRING(headBoneName,   "Head Bone Name")
    REFLECT_INFO  ("PMX/MMD head is usually \"\xE9\xA0\xAD\"; FBX often \"Head\". Or pick from the dropdown below.")
    REFLECT_ENUM  (forwardAxis,    "Head Forward Axis", kFaceAxisOptions)
    REFLECT_ENUM  (upAxis,         "Head Up Axis",      kFaceAxisOptions)
    REFLECT_INFO  ("Forward = out of the face, Up = top of the head. PMX heads often run +Y.")
    REFLECT_DRAG(yawLimitDeg,    "Yaw Limit (deg)",   0.f, 90.f)
    REFLECT_DRAG(pitchLimitDeg,  "Pitch Limit (deg)", 0.f, 90.f)
    REFLECT_DRAG(weight,         "Tracking Weight",   0.f, 1.f)
    REFLECT_DRAG(smoothingSpeed, "Smoothing Speed",   0.f, 30.f)
    REFLECT_DRAG(maxTurnSpeedDeg,"Max Turn Speed (deg/s)", 0.f, 720.f)
    REFLECT_INFO  ("Caps how fast the head swings. 0 = uncapped. Lower = more deliberate (try 90-180).")
    REFLECT_DRAG(turnAccelDeg,   "Turn Accel (deg/s\xC2\xB2)", 0.f, 2000.f)
    REFLECT_INFO  ("Ease-in / reaction delay: >0 makes the head take a beat to reach full turn speed. 0 = off.")
    REFLECT_DRAG(returnSpeed,    "Return Speed",      0.f, 20.f)
    REFLECT_INFO  ("How fast the head eases back to rest when the target leaves the cone / disappears. Lower = lazier return. (Target picker is above.)")
    REFLECT_HEADER("Computed (read-only)")
    REFLECT_FLOAT3(lookForward,    "Look Forward", -1.f, 1.f)
REFLECT_END()

// ==== FacialAnimationComponent: morph-target (blend shape) control ====
// Scalars only: every morph name here is a char[64], Reflect::FieldKind has no char[] kind
// (REFLECT_STRING is std::string only), and REFLECT_ARRAY is out because DrawVectorImpl memsets
// the next slot on '+'. The blink-morph and viseme dropdowns and the Authored Channels editor live
// in the editor's postDraw, which has the skeleton's morph-name list.

REFLECT_BEGIN(FacialAnimationComponent)
    REFLECT_BOOL  (enabled,             "Enabled")
    REFLECT_INFO  ("Drives morph targets (blend shapes). Unrelated to Face Shading, which is SDF face shadows.")
    REFLECT_INFO  ("Must sit on the entity that carries SkeletonComponent - anywhere else it is a silent no-op.")
    REFLECT_HEADER("Auto Blink")
    REFLECT_BOOL  (autoBlinkEnabled,    "Auto Blink")
    REFLECT_DRAG  (blinkIntervalMinSec, "Interval Min (s)", 0.f, 20.f)
    REFLECT_DRAG  (blinkIntervalMaxSec, "Interval Max (s)", 0.f, 20.f)
    REFLECT_DRAG  (blinkCloseSec,       "Close (s)",        0.f, 1.f)
    REFLECT_DRAG  (blinkHoldSec,        "Hold (s)",         0.f, 1.f)
    REFLECT_DRAG  (blinkOpenSec,        "Open (s)",         0.f, 1.f)
    REFLECT_DRAG  (blinkMaxWeight,      "Max Weight",       0.f, 1.f)
    REFLECT_INFO  ("Blink is the MAX layer, applied last - it closes fully over a partial squint.")
    REFLECT_INFO  ("The blink timer does not advance in Edit (dt = 0). Use the Blink Now button below.")
REFLECT_END()

// ==== ScriptComponent ====

// ScriptComponent holds a std::vector<ScriptInstance> (several scripts per entity), which a
// static field descriptor cannot express: the descriptor is empty and the whole inspector
// (per-slot path / enabled / exposed vars / add / remove) is drawn in EditorLayer's postDraw, like
// SocketComponent's list.
REFLECT_BEGIN(ScriptComponent)
REFLECT_END()

// ==== AnimationComponent: paused / looping / speed ====
// The clip combo needs a ClipLibrary lookup and lives in postDraw.

REFLECT_BEGIN(AnimationComponent)
    REFLECT_BOOL    (paused,  "Paused")
    REFLECT_BOOL    (looping, "Looping")
    REFLECT_FLOAT_FMT(speed,  "Speed", -4.f, 4.f, "%.2f x", 0.01f)
REFLECT_END()

// ==== CameraMotionComponent: speed / looping / playOnLoad ====
// The path and the Play / Stop transport live in postDraw.

REFLECT_BEGIN(CameraMotionComponent)
    REFLECT_FLOAT_FMT(speed,    "Speed", -4.f, 4.f, "%.2f x", 0.01f)
    REFLECT_FLOAT_FMT(translationScale, "Translation Scale", 0.0001f, 1000.f, "%.4f x", 0.001f)
    REFLECT_BOOL     (looping,    "Looping")
    REFLECT_BOOL     (playOnLoad, "Play On Load")
REFLECT_END()

// ==== MotionDirectorComponent: master-clock transport ====
// The track list (entity pickers) lives in postDraw.

REFLECT_BEGIN(MotionDirectorComponent)
    REFLECT_FLOAT_FMT(masterSpeed, "Master Speed", -4.f, 4.f, "%.2f x", 0.01f)
    REFLECT_FLOAT_FMT(duration,    "Duration",      0.f, 600.f, "%.2f s (0=auto)", 0.1f)
    REFLECT_BOOL     (looping,     "Looping")
    REFLECT_BOOL     (playOnLoad,  "Play On Load")
REFLECT_END()

// ==== SequencePlayerComponent: cinematic sequence playback ====
// Reflected: the asset path (accepts an ISEQ_PATH drop) and the authored playback options. The
// cast (role -> entity) and the transport live in postDraw: the cast needs entity pickers and the
// transport the sequence's frame count.

constexpr Reflect::EnumOption kSeqSkipPolicyOptions[] = {
    { (int)SequencePlayerComponent::SkipPolicy::None,            "Not skippable" },
    { (int)SequencePlayerComponent::SkipPolicy::JumpToEnd,       "Jump to end" },
    { (int)SequencePlayerComponent::SkipPolicy::StopImmediately, "Stop immediately" },
};

REFLECT_BEGIN(SequencePlayerComponent)
    REFLECT_STRING_DROP(sequencePath, "Sequence", "ISEQ_PATH")
    REFLECT_BOOL       (playOnLoad,   "Play On Load")
    REFLECT_BOOL       (overrideLooping, "Override Looping")
    REFLECT_BOOL       (looping,      "Looping")
    REFLECT_FLOAT_FMT  (rate,         "Rate", -4.f, 4.f, "%.2f x (negative = reverse)", 0.01f)
    REFLECT_HEADER     ("Gameplay")
    REFLECT_STRING     (gameFlowState, "Game Flow State")
    REFLECT_ENUM       (skipPolicy,    "On Skip", kSeqSkipPolicyOptions)
    REFLECT_BOOL       (recordWatched, "Record As Watched")
REFLECT_END()

// ==== AudioSourceComponent: 2D / 3D sound emitter ====
// clipPath takes an ACLIP_PATH drag-drop; the Play / Pause / Stop transport lives in postDraw.

constexpr Reflect::EnumOption kAudioBusOptions[] = {
    { (int)Audio::BusType::Master,  "Master"  },
    { (int)Audio::BusType::Music,   "Music"   },
    { (int)Audio::BusType::SFX,     "SFX"     },
    { (int)Audio::BusType::Voice,   "Voice"   },
    { (int)Audio::BusType::Ambient, "Ambient" },
    { (int)Audio::BusType::UI,      "UI"      },
};

constexpr Reflect::EnumOption kAttenuationCurveOptions[] = {
    { (int)Audio::AttenuationCurve::Linear,        "Linear"         },
    { (int)Audio::AttenuationCurve::Logarithmic,   "Logarithmic"    },
    { (int)Audio::AttenuationCurve::InverseSquare, "Inverse Square" },
};

REFLECT_BEGIN(Audio::AudioSourceComponent)
    REFLECT_STRING_DROP(clipPath,     "Clip (.aclip)", "ACLIP_PATH")
    REFLECT_ENUM       (bus,          "Bus",            kAudioBusOptions)
    REFLECT_DRAG     (volume,       "Volume",         0.f, 2.f)
    REFLECT_FLOAT_FMT  (pitch,        "Pitch",          0.1f, 4.f, "%.2f x", 0.01f)
    REFLECT_BOOL       (loop,         "Loop")
    REFLECT_BOOL       (is3D,         "3D Spatialized")
    REFLECT_BOOL       (playOnEnable, "Play On Enable")
    REFLECT_HEADER("3D Attenuation (when 3D)")
    REFLECT_FLOAT      (minDistance,  "Min Distance",   0.f, 1000.f)
    REFLECT_FLOAT      (maxDistance,  "Max Distance",   0.f, 5000.f)
    // Saved but not drawn: X3DAudio plays its default curves whatever this says (only Linear is wired).
    REFLECT_ENUM       (curve,        "Curve",          kAttenuationCurveOptions, Hidden)
REFLECT_END()

// ==== AudioListenerComponent: listener pose source (usually the active camera) ====

REFLECT_BEGIN(Audio::AudioListenerComponent)
    REFLECT_DRAG(masterVolume, "Master Volume", 0.f, 2.f)
    REFLECT_BOOL  (active,       "Active")
REFLECT_END()

// ==== MorphComponent: paused / looping ====
// The clip combo and per-channel weight editor stay in postDraw (they need MorphClipLibrary).

REFLECT_BEGIN(MorphComponent)
    REFLECT_BOOL(paused,  "Paused")
    REFLECT_BOOL(looping, "Looping")
REFLECT_END()

// ==== Display-only components ====
// Empty descriptors so RegisterReflectedComponent compiles; the read-only display is drawn
// entirely in postDraw.

REFLECT_BEGIN(SkeletonRef)
REFLECT_END()

// PrefabSourcePath: read-only prefab-instance link (postDraw shows the path; removing the
// component detaches the instance from its prefab).
REFLECT_BEGIN(PrefabSourcePath)
REFLECT_END()

REFLECT_BEGIN(SkeletonComponent)
REFLECT_END()

REFLECT_BEGIN(CapsuleColliderComponent)
REFLECT_END()

REFLECT_BEGIN(SocketComponent)
REFLECT_END()

REFLECT_BEGIN(FollowEntityComponent)
REFLECT_END()

// ==== Entity-as-widget Canvas UI ====
// UV addressing modes used by Canvas images.
constexpr Reflect::EnumOption kUIWrapModeOptions[] = {
    { (int)UI::UIWrapMode::Clamp,  "Clamp" },
    { (int)UI::UIWrapMode::Wrap,   "Wrap (tile)" },
    { (int)UI::UIWrapMode::Mirror, "Mirror" },
};

// Canvas render and scale modes.
constexpr Reflect::EnumOption kCanvasRenderModeOptions[] = {
    { (int)UI::CanvasRenderMode::ScreenSpaceOverlay, "Screen Space - Overlay" },
};
constexpr Reflect::EnumOption kCanvasScaleModeOptions[] = {
    { (int)UI::CanvasScaleMode::ConstantPixelSize,   "Constant Pixel Size" },
    { (int)UI::CanvasScaleMode::ScaleWithScreenSize, "Scale With Screen Size" },
};
REFLECT_BEGIN(UI::UICanvas)
    REFLECT_ENUM  (renderMode,          "Render Mode",          kCanvasRenderModeOptions, Unit("enum"), Hard(0, 0), LuaWrite)
    REFLECT_ENUM  (scaleMode,           "Scale Mode",           kCanvasScaleModeOptions, Unit("enum"), Hard(0, 1), LuaWrite)
    REFLECT_FLOAT2(referenceResolution, "Reference Resolution", 1.f, 8192.f, Key("refW,refH"), Unit("px"), Hard(0, kFloatMax), LuaWrite)
    REFLECT_DRAG(matchWidthOrHeight,  "Match Width<->Height", 0.f, 1.f, Key("match"), Unit("unitless"), Hard(0, 1), LuaWrite)
    REFLECT_INT   (sortOrder,           "Sort Order",           -1000, 1000, Unit("order"), Hard(-21000, 21000), LuaWrite)
    REFLECT_BOOL  (visible,             "Visible (whole canvas)", Unit("unitless"), LuaWrite)REFLECT_END()

REFLECT_BEGIN(UI::UIRect)
    REFLECT_FLOAT2(anchorMin, "Anchor Min",  0.f, 1.f, Key("aMinX,aMinY"), Unit("fraction"), Hard(-kFloatMax, kFloatMax), LuaWrite)
    REFLECT_FLOAT2(anchorMax, "Anchor Max",  0.f, 1.f, Key("aMaxX,aMaxY"), Unit("fraction"), Hard(-kFloatMax, kFloatMax), LuaWrite)
    REFLECT_FLOAT2(pivot,     "Pivot",       0.f, 1.f, Key("pivX,pivY"), Unit("fraction"), Hard(-kFloatMax, kFloatMax), LuaWrite)
    REFLECT_FLOAT2(size,      "Size Delta (ref px)", -8192.f, 8192.f, Key("szX,szY"), Unit("reference_px"), Hard(-kFloatMax, kFloatMax), LuaWrite)
    REFLECT_FLOAT2(offset,    "Offset (ref px)",     -8192.f, 8192.f, Key("offX,offY"), Unit("reference_px"), Hard(-kFloatMax, kFloatMax), LuaWrite)
    REFLECT_BOOL  (visible,   "Visible (Subtree)", Unit("unitless"), LuaWrite)
    REFLECT_INT   (siblingIndex, "Sibling Index", -1000000, 1000000, Key("sibling"), Unit("order"), Hard(-1e9, 1e9), LuaWrite)REFLECT_END()

REFLECT_BEGIN(UI::UIClipRect)
    REFLECT_BOOL(enabled, "Enabled", Unit("unitless"), LuaWrite)REFLECT_END()

// A scene's reference to a .iui asset; the instance exists only at run time.
REFLECT_BEGIN(UI::UIDocument)
    REFLECT_STRING_DROP(path, "UI Asset (.iui)", "IUI_PATH", Unit("path"), LuaWrite)REFLECT_END()

REFLECT_BEGIN(UI::UIImage)
    REFLECT_BOOL  (visible,     "Visible", Unit("unitless"), LuaWrite)
    REFLECT_STRING_DROP(texturePath, "Texture", "ITEX_PATH", Key("tex"), Unit("path"), LuaWrite)
    REFLECT_COLOR4(color,       "Color", Unit("linear_rgba"), Hard(0, kFloatMax), LuaWrite)
    REFLECT_FLOAT2(uv0,         "UV0", 0.f, 4.f, Unit("uv"), Hard(-kFloatMax, kFloatMax), LuaWrite)
    REFLECT_FLOAT2(uv1,         "UV1", 0.f, 4.f, Unit("uv"), Hard(-kFloatMax, kFloatMax), LuaWrite)
    REFLECT_ENUM  (wrapMode,    "UV Wrap",      kUIWrapModeOptions, Key("wrap"), Unit("enum"), Hard(0, 2), LuaWrite)
    REFLECT_BOOL  (pointFilter, "Point Filter", Key("point"), Unit("unitless"), LuaWrite)
    // Border insets in source-texture pixels (L/T/R/B), not UV fractions (UIImage::sliceBorder).
    // All zero = one stretched quad.
    REFLECT_FLOAT4(sliceBorder, "9-Slice Border L/T/R/B (tex px)", 0.f, 512.f, Key("slice"), Unit("texture_px"), Hard(0, kFloatMax), LuaWrite)REFLECT_END()

// Sprite-sheet (atlas) animation: drives the entity's UIImage uv0 / uv1.
REFLECT_BEGIN(UI::UISpriteAnimComponent)
    REFLECT_BOOL (playing,    "Playing", Unit("unitless"), LuaWrite)
    REFLECT_BOOL (loop,       "Loop", Unit("unitless"), LuaWrite)
    REFLECT_BOOL (pingpong,   "Ping-Pong", Unit("unitless"), LuaWrite)
    REFLECT_INT  (columns,    "Columns",            1, 64, Key("cols"), Unit("count"), Hard(0, 4096), LuaWrite)
    REFLECT_INT  (rows,       "Rows",               1, 64, Unit("count"), Hard(0, 4096), LuaWrite)
    REFLECT_INT  (frameCount, "Frame Count (0=all)", 0, 4096, Key("frames"), Unit("count"), Hard(0, 1048576), LuaWrite)
    REFLECT_FLOAT(fps,        "Frames / sec",        0.f, 120.f, Unit("hz"), Hard(0, kFloatMax), LuaWrite)REFLECT_END()

constexpr Reflect::EnumOption kUITextAlignHOptions[] = {
    { 0, "Left" }, { 1, "Center" }, { 2, "Right" },
};
constexpr Reflect::EnumOption kUITextAlignVOptions[] = {
    { 0, "Top" }, { 1, "Middle" }, { 2, "Bottom" },
};
REFLECT_BEGIN(UI::UIText)
    REFLECT_BOOL  (visible,   "Visible", Unit("unitless"), LuaWrite)
    REFLECT_STRING(text,      "Text", Unit("utf8"), LuaWrite)
    REFLECT_STRING_DROP(fontPath, "Font (drop .ttf, empty = default)", "IFONT_PATH", Key("font"), Unit("path"), LuaWrite)
    REFLECT_COLOR4(color,     "Color", Unit("linear_rgba"), Hard(0, kFloatMax), LuaWrite)
    REFLECT_DRAG(fontScale, "Font Scale", 0.1f, 8.f, Unit("multiplier"), Hard(0, kFloatMax), LuaWrite)
    REFLECT_ENUM  (alignH,    "Align H", kUITextAlignHOptions, Unit("enum"), Hard(0, 2), LuaWrite)
    REFLECT_ENUM  (alignV,    "Align V", kUITextAlignVOptions, Unit("enum"), Hard(0, 2), LuaWrite)
    REFLECT_BOOL  (fxOutline,      "FX: Outline", Key("fxO"), Unit("unitless"), LuaWrite)
    REFLECT_BOOL  (fxGlow,         "FX: Glow", Key("fxG"), Unit("unitless"), LuaWrite)
    REFLECT_BOOL  (fxShadow,       "FX: Drop Shadow", Key("fxS"), Unit("unitless"), LuaWrite)
    REFLECT_BOOL  (fxJitter,       "FX: Jitter", Key("fxJ"), Unit("unitless"), LuaWrite)
    REFLECT_COLOR4(outlineColor,   "Outline Color", Key("outlineCol"), Unit("linear_rgba"), Hard(0, kFloatMax), LuaWrite)
    REFLECT_FLOAT (outlineWidth,   "Outline Width (px)", 0.f, 16.f, Key("outlineW"), Unit("px"), Hard(0, kFloatMax), LuaWrite)
    REFLECT_COLOR4(glowColor,      "Glow Color", Key("glowCol"), Unit("linear_rgba"), Hard(0, kFloatMax), LuaWrite)
    REFLECT_FLOAT (glowWidth,      "Glow Width (px)",    0.f, 32.f, Key("glowW"), Unit("px"), Hard(0, kFloatMax), LuaWrite)
    REFLECT_COLOR4(shadowColor,    "Shadow Color", Key("shadowCol"), Unit("linear_rgba"), Hard(0, kFloatMax), LuaWrite)
    REFLECT_FLOAT (shadowOffsetX,  "Shadow Offset X (px)", -32.f, 32.f, Key("shadowOX"), Unit("px"), Hard(-kFloatMax, kFloatMax), LuaWrite)
    REFLECT_FLOAT (shadowOffsetY,  "Shadow Offset Y (px)", -32.f, 32.f, Key("shadowOY"), Unit("px"), Hard(-kFloatMax, kFloatMax), LuaWrite)
    REFLECT_FLOAT (jitterAmplitude,"Jitter Amplitude (px)", 0.f, 16.f, Key("jitterA"), Unit("px"), Hard(0, kFloatMax), LuaWrite)
    REFLECT_FLOAT (jitterFrequency,"Jitter Frequency (Hz)", 0.f, 60.f, Key("jitterF"), Unit("hz"), Hard(0, kFloatMax), LuaWrite)
    REFLECT_STRING_DROP(customShaderPath, "Custom Shader (drop .hlsl, empty = none)", "IHLSL_PATH", Key("customShader"), Unit("path"), LuaWrite)
    REFLECT_FLOAT4(customParam0,   "Custom Params A", -1000.f, 1000.f, Key("cpA"), Unit("unitless"), Hard(-kFloatMax, kFloatMax), LuaWrite)
    REFLECT_FLOAT4(customParam1,   "Custom Params B", -1000.f, 1000.f, Key("cpB"), Unit("unitless"), Hard(-kFloatMax, kFloatMax), LuaWrite)
    REFLECT_FLOAT (wrapWidth,      "Wrap Width (0 = off)", 0.f, 4096.f, Key("wrapW"), Unit("reference_px"), Hard(0, kFloatMax), LuaWrite)REFLECT_END()

REFLECT_BEGIN(UI::UIInteractable)
    REFLECT_BOOL  (raycastTarget,  "Raycast Target", Key("raycast"), Unit("unitless"), LuaWrite)
    REFLECT_BOOL  (disabled,       "Disabled", Unit("unitless"), LuaWrite)
    REFLECT_BOOL  (focusable,      "Focusable", Unit("unitless"), LuaWrite)
    REFLECT_BOOL  (absorbNavX,     "Absorb Left/Right", Unit("unitless"), LuaWrite)
    REFLECT_BOOL  (tintTransition, "Tint On State", Key("tint"), Unit("unitless"), LuaWrite)
    REFLECT_COLOR4(normalColor,    "Normal Color", Key("normal"), Unit("linear_rgba"), Hard(0, kFloatMax), LuaWrite)
    REFLECT_COLOR4(hoverColor,     "Hover Color", Key("hover"), Unit("linear_rgba"), Hard(0, kFloatMax), LuaWrite)
    REFLECT_COLOR4(pressedColor,   "Pressed Color", Key("pressed"), Unit("linear_rgba"), Hard(0, kFloatMax), LuaWrite)
    REFLECT_COLOR4(disabledColor,  "Disabled Color", Key("disabledCol"), Unit("linear_rgba"), Hard(0, kFloatMax), LuaWrite)REFLECT_END()

// World-space UI: billboarded 3D quads (HP bars, name plates, damage numbers) drawn by
// WorldUIBillboardPass. Placed by the entity's GlobalTransform; drive it through LocalTransform /
// FollowEntity.
constexpr Reflect::EnumOption kWorldUIScalingOptions[] = {
    { (int)UI::ScalingMode::ConstantPixel, "Constant Pixel (HUD-on-3D)" },
    { (int)UI::ScalingMode::ConstantWorld, "Constant World (size in metres)" },
};
constexpr Reflect::EnumOption kWorldUIDepthOptions[] = {
    { (int)UI::DepthMode::Always,       "Always (overlay)" },
    { (int)UI::DepthMode::Test,         "Depth Test"       },
    { (int)UI::DepthMode::TestWithFade, "Depth Test + Fade (TBD)" },
};

REFLECT_BEGIN(UI::WorldSpaceUIComponent)
    REFLECT_FLOAT2(baseSize,          "Base Size (px or m)",       0.001f, 1e4f)
    REFLECT_FLOAT2(pivot,             "Pivot",                     0.f,    1.f)
    REFLECT_FLOAT3(screenSpaceOffset, "Screen-Space Offset",       -1e3f,  1e3f)
    REFLECT_ENUM (scalingMode,        "Scaling Mode",              kWorldUIScalingOptions)
    REFLECT_FLOAT(fadeNear,           "Fade Near (m)",             0.f,    1e3f)
    REFLECT_FLOAT(fadeFar,            "Fade Far (m)",              0.f,    1e4f)
    REFLECT_BOOL (hideWhenBehindCamera, "Hide When Behind Camera")
    REFLECT_ENUM (depthMode,          "Depth Mode",                kWorldUIDepthOptions)
REFLECT_END()

REFLECT_BEGIN(UI::WorldUIBarComponent)
    REFLECT_BOOL  (visible,     "Visible")
    REFLECT_DRAG(value,       "Value (0..1)", 0.f, 1.f)
    REFLECT_FLOAT (borderThick, "Border Thickness", 0.f, 1.f)
    REFLECT_COLOR4(fillColor,       "Fill Color")
    REFLECT_COLOR4(backgroundColor, "Background Color")
    REFLECT_COLOR4(borderColor,     "Border Color")
REFLECT_END()

REFLECT_BEGIN(UI::WorldUITextComponent)
    REFLECT_BOOL  (visible,  "Visible")
    REFLECT_STRING(text,     "Text")
    REFLECT_STRING_DROP(fontPath, "Font (drop .ttf, empty = default)", "IFONT_PATH")
    REFLECT_COLOR4(color,    "Color")
    REFLECT_DRAG(scale,    "Scale", 0.1f, 8.f)
    REFLECT_BOOL  (fxOutline,      "FX: Outline")
    REFLECT_BOOL  (fxGlow,         "FX: Glow")
    REFLECT_BOOL  (fxShadow,       "FX: Drop Shadow")
    REFLECT_BOOL  (fxJitter,       "FX: Jitter")
    REFLECT_COLOR4(outlineColor,   "Outline Color")
    REFLECT_FLOAT (outlineWidth,   "Outline Width (px)", 0.f, 16.f)
    REFLECT_COLOR4(glowColor,      "Glow Color")
    REFLECT_FLOAT (glowWidth,      "Glow Width (px)",    0.f, 32.f)
    REFLECT_COLOR4(shadowColor,    "Shadow Color")
    REFLECT_FLOAT (shadowOffsetX,  "Shadow Offset X (px)", -32.f, 32.f)
    REFLECT_FLOAT (shadowOffsetY,  "Shadow Offset Y (px)", -32.f, 32.f)
    REFLECT_FLOAT (jitterAmplitude,"Jitter Amplitude (px)", 0.f, 16.f)
    REFLECT_FLOAT (jitterFrequency,"Jitter Frequency (Hz)", 0.f, 60.f)
    REFLECT_STRING_DROP(customShaderPath, "Custom Shader (drop .hlsl, empty = none)", "IHLSL_PATH")
    REFLECT_FLOAT4(customParam0,   "Custom Params A", -1000.f, 1000.f)
    REFLECT_FLOAT4(customParam1,   "Custom Params B", -1000.f, 1000.f)
REFLECT_END()

REFLECT_BEGIN(UI::WorldUIImageComponent)
    REFLECT_BOOL  (visible, "Visible")
    REFLECT_COLOR4(tint,    "Tint")
    REFLECT_FLOAT2(uv0,     "UV0", 0.f, 1.f)
    REFLECT_FLOAT2(uv1,     "UV1", 0.f, 1.f)
REFLECT_END()

REFLECT_BEGIN(UI::DamageNumberComponent)
    REFLECT_STRING(text,         "Text")
    REFLECT_STRING_DROP(fontPath, "Font (drop .ttf, empty = default)", "IFONT_PATH")
    REFLECT_COLOR4(color,        "Color")
    REFLECT_FLOAT (lifetime,      "Lifetime Remaining (s)", 0.f,  60.f)
    REFLECT_FLOAT (totalLifetime, "Total Lifetime (s)",     0.01f, 60.f)
    REFLECT_FLOAT3(velocity,      "Velocity (m/s)",         -50.f, 50.f)
    REFLECT_BOOL  (fxOutline,     "FX: Outline")
    REFLECT_BOOL  (fxGlow,        "FX: Glow")
    REFLECT_BOOL  (fxShadow,      "FX: Drop Shadow")
    REFLECT_BOOL  (fxJitter,      "FX: Jitter")
    REFLECT_COLOR4(outlineColor,  "Outline Color")
    REFLECT_FLOAT (outlineWidth,  "Outline Width (px)", 0.f, 16.f)
    REFLECT_COLOR4(glowColor,     "Glow Color")
    REFLECT_FLOAT (glowWidth,     "Glow Width (px)",    0.f, 32.f)
    REFLECT_STRING_DROP(customShaderPath, "Custom Shader (drop .hlsl, empty = none)", "IHLSL_PATH")
    REFLECT_FLOAT4(customParam0,  "Custom Params A", -1000.f, 1000.f)
    REFLECT_FLOAT4(customParam1,  "Custom Params B", -1000.f, 1000.f)
REFLECT_END()

REFLECT_BEGIN(FollowSocketComponent)
REFLECT_END()

REFLECT_BEGIN(DecalComponent)
REFLECT_END()

REFLECT_BEGIN(TerrainComponent)
REFLECT_END()

// ==== GrassComponent: GoT-style procedural grass field ====
// Blades are generated on the GPU each frame (Grass.as/ms/ps.hlsl); roots anchor to the scene's
// first TerrainComponent heightmap.
REFLECT_BEGIN(GrassComponent)
    REFLECT_BOOL  (enabled,          "Enabled")
    REFLECT_HEADER("Field Placement")
    REFLECT_FLOAT3(worldCenter,      "World Center", -100000.f, 100000.f)
    REFLECT_FLOAT (worldSize,        "World Size (m)",    1.f, 100000.f)
    REFLECT_UINT  (patchesPerSide,   "Patches Per Side",  1u,  1024u)
    REFLECT_HEADER("Density / LOD")
    REFLECT_FLOAT (density,          "Density (blades/m2)", 0.f, 64.f)
    REFLECT_FLOAT (lod0Dist,         "LOD0 Distance (m)",   1.f, 500.f)
    REFLECT_FLOAT (lod1Dist,         "LOD1 Distance (m)",   1.f, 1000.f)
    REFLECT_FLOAT (cullDist,         "Cull Distance (m)",   1.f, 2000.f)
    REFLECT_HEADER("Blade Shape")
    REFLECT_FLOAT (bladeHeight,      "Height (m)",       0.02f, 4.f)
    REFLECT_DRAG(bladeHeightVar,   "Height Variance",  0.f,   0.95f)
    REFLECT_FLOAT (bladeWidth,       "Width (m)",        0.005f, 0.5f)
    REFLECT_FLOAT (tiltMaxDeg,       "Max Tilt (deg)",   0.f,   85.f)
    REFLECT_DRAG(bendAmount,       "Bend",             0.f,   1.f)
    REFLECT_HEADER("Wind")
    REFLECT_FLOAT2(windDir,          "Wind Direction (XZ)", -1.f, 1.f)
    REFLECT_FLOAT (windStrength,     "Wind Strength (m)",   0.f, 2.f)
    REFLECT_FLOAT (windSpeed,        "Wind Speed",          0.f, 10.f)
    REFLECT_FLOAT_FMT(windScale,     "Gust Scale (1/m)", 0.001f, 1.f, "%.3f", 0.001f)
    REFLECT_HEADER("Clumping")
    REFLECT_FLOAT (clumpCellSize,    "Clump Cell (m)",   0.1f, 10.f)
    REFLECT_DRAG(clumpBlend,       "Clump Blend",      0.f,  1.f)
    REFLECT_HEADER("Placement Gates")
    REFLECT_FLOAT (minWorldY,        "Min World Y (m)",  -100000.f, 100000.f)
    REFLECT_FLOAT (maxWorldY,        "Max World Y (m)",  -100000.f, 100000.f)
    REFLECT_FLOAT (maxSlopeDeg,      "Max Slope (deg)",  0.f, 89.f)
    REFLECT_HEADER("Look")
    REFLECT_COLOR3(baseColor,        "Root Color")
    REFLECT_COLOR3(tipColor,         "Tip Color")
    REFLECT_FLOAT_FMT(colorNoiseScale, "Color Noise Scale (1/m)", 0.001f, 1.f, "%.3f", 0.001f)
    REFLECT_DRAG(colorNoiseAmount, "Color Noise Amount", 0.f, 1.f)
    REFLECT_DRAG(rootAO,           "Root AO",          0.f, 1.f)
    REFLECT_DRAG(normalBlend,      "Normal Up-Blend",  0.f, 1.f)
    REFLECT_DRAG(viewThicken,      "View Thicken",     0.f, 2.f)
    REFLECT_FLOAT (farWidthMul,      "Far Width Mul",    1.f, 4.f)
    REFLECT_DRAG(roughness,        "Roughness",        0.02f, 1.f)
    REFLECT_UINT  (seed,             "Seed",             0u, 0xFFFFFFFFu)
REFLECT_END()

// ==== Water ====
// WaterComponent: flat water tile, Fresnel sky reflection + flow normals; worldCenter.y is the
// water level. Depth and shore fade come from the terrain heightmap (analytic), so no scene-depth
// readback is involved.
// WaterInteractionComponent: interactive ripples for a ShadingModel::Water surface. Placement and
// size come from the entity transform, so these fields are simulation budget only.
REFLECT_BEGIN(WaterInteractionComponent)
    REFLECT_BOOL  (enabled,        "Enabled")
    REFLECT_HEADER("Wave Simulation")
    REFLECT_FLOAT (propagation,    "Propagation (wave speed)", 0.f,  0.30f)
    REFLECT_FLOAT (damping,        "Damping",                  0.f,  1.f)
    REFLECT_FLOAT (amplitude,      "Impulse Amplitude",        0.f,  10.f)
    REFLECT_FLOAT (normalStrength, "Normal Strength",          0.f,  20.f)
    REFLECT_FLOAT (areaSize,       "Sim Window (m)",           8.f,  4096.f)
REFLECT_END()

REFLECT_BEGIN(WaterComponent)
    REFLECT_BOOL  (enabled,        "Enabled")
    REFLECT_HEADER("Placement")
    REFLECT_FLOAT3(worldCenter,    "World Center (Y = level)", -100000.f, 100000.f)
    REFLECT_FLOAT (worldSize,      "World Size (m)", 1.f, 100000.f)
    REFLECT_UINT  (gridQuads,      "Grid Quads",     1u,  512u)
    REFLECT_HEADER("Body Color")
    REFLECT_COLOR3(deepColor,      "Deep Color")
    REFLECT_COLOR3(shallowColor,   "Shallow Color")
    REFLECT_FLOAT (absorbDist,     "Absorb Depth (m)", 0.05f, 100.f)
    REFLECT_FLOAT (shoreFade,      "Shore Fade (m)",   0.05f, 20.f)
    REFLECT_HEADER("Flow Normals")
    REFLECT_STRING_DROP(normalMapAPath, "Normal Map A (swell)",  "ITEX_PATH")
    REFLECT_STRING_DROP(normalMapBPath, "Normal Map B (detail)", "ITEX_PATH")
    REFLECT_FLOAT2(flowDir,        "Flow Direction (XZ)", -1.f, 1.f)
    REFLECT_FLOAT (flowSpeed,      "Flow Speed (m/s)", 0.f, 10.f)
    REFLECT_FLOAT_FMT(normalTiling, "Normal Tiling (1/m)", 0.001f, 5.f, "%.3f", 0.001f)
    REFLECT_DRAG(normalStrength, "Normal Strength", 0.f, 2.f)
    REFLECT_HEADER("Shading")
    REFLECT_DRAG(fresnelF0,      "Fresnel F0",      0.f, 0.2f)
    REFLECT_DRAG(reflStrength,   "Reflection",      0.f, 2.f)
    REFLECT_FLOAT (specPower,      "Sun Glint Power", 8.f, 2048.f)
    REFLECT_HEADER("Ripples (Interactive)")
    REFLECT_BOOL  (enableRipples,        "Enable Ripples")
    REFLECT_DRAG(ripplePropagation,    "Wave Speed",      0.f, 0.30f)
    REFLECT_DRAG(rippleDamping,        "Damping",         0.f, 0.5f)
    REFLECT_DRAG(rippleNormalStrength, "Ripple Strength", 0.f, 12.f)
    REFLECT_FLOAT (rippleAmplitude,      "Splash Amplitude", 0.f, 4.f)
    REFLECT_FLOAT (rippleAreaSize,       "Sim Window (m)",  16.f, 2000.f)
REFLECT_END()

REFLECT_BEGIN(BlackboardComponent)
REFLECT_END()

constexpr Reflect::EnumOption kAITargetOptions[]={{0,"Nearest visible hostile"},{1,"Keep current hostile"},{2,"Fixed target"},{3,"No target (patrol only)"}};
constexpr Reflect::EnumOption kAIPatrolOptions[]={{0,"Loop"},{1,"Ping-pong"},{2,"Once"}};
REFLECT_BEGIN(AIBehaviorComponent)
    REFLECT_BOOL(enabled,"Enabled")
    REFLECT_ENUM(targetMode,"Target selection",kAITargetOptions)
    REFLECT_BOOL(fixedRequiresSight,"Fixed target requires sight")
    REFLECT_FLOAT(memorySeconds,"Lose target after (s)",0.f,120.f)
    REFLECT_FLOAT(leashRadius,"Home leash (m, 0 = off)",0.f,10000.f)
    REFLECT_BOOL(trackDuringMemory,"Track hidden target during memory")
    REFLECT_ENUM(patrolMode,"Patrol mode",kAIPatrolOptions)
    REFLECT_FLOAT(arriveRadius,"Arrival radius (m)",.01f,100.f)
    REFLECT_FLOAT(waitSeconds,"Wait at point (s)",0.f,120.f)
    REFLECT_FLOAT(legTimeout,"Unreachable point timeout (s)",.1f,600.f)
REFLECT_END()

// ==== BeamComponent ====
// controlPoints is a vector of reflected BeamControlPoint records, so the generic vector drawer
// handles add / remove / reorder.

REFLECT_BEGIN(BeamControlPoint)
    REFLECT_FLOAT3(position,  "Position",  -1e6f, 1e6f)
    REFLECT_FLOAT (radius,    "Radius",    0.001f, 5.f)
    REFLECT_COLOR4(colorTint, "Color Tint")
REFLECT_END()

REFLECT_BEGIN(BeamComponent)
    REFLECT_FLOAT (globalRadiusScale, "Radius Scale",     0.001f, 10.f)
    REFLECT_FLOAT (wobbleAmplitude,   "Wobble Amplitude", 0.f,    0.5f)
    REFLECT_FLOAT (wobbleSpeed,       "Wobble Speed",     0.f,    20.f)
    REFLECT_VECTOR(controlPoints,     "Control Points",   BeamControlPoint)
REFLECT_END()

// ==== Particle components ====
// Every emitter shape parameter is gated on the shape enum; the texture path accepts an ITEX_PATH
// drag-drop.

constexpr Reflect::EnumOption kParticleBlendOptions[] = {
    { (int)ParticleBlendMode::Alpha,    "Alpha"    },
    { (int)ParticleBlendMode::Additive, "Additive" },
};

constexpr Reflect::EnumOption kParticleVisualOptions[] = {
    { (int)ParticleVisualMode::Flat,     "Flat"     },
    { (int)ParticleVisualMode::Fire,     "Fire"     },
    { (int)ParticleVisualMode::Smoke,    "Smoke"    },
    { (int)ParticleVisualMode::Electric, "Electric" },
};

constexpr Reflect::EnumOption kParticleShapeOptions[] = {
    { (int)ParticleShape::Point,  "Point"  },
    { (int)ParticleShape::Sphere, "Sphere" },
    { (int)ParticleShape::Cone,   "Cone"   },
    { (int)ParticleShape::Box,    "Box"    },
    { (int)ParticleShape::Circle, "Circle" },
    { (int)ParticleShape::Mesh,   "Mesh"   },
    { (int)ParticleShape::Trail,  "Trail (Weapon Swing)" },
};

// ---- ParticleRenderComponent: shared appearance (pair with a behaviour) ----
REFLECT_BEGIN(ParticleRenderComponent)
    REFLECT_HEADER   ("Color")
    REFLECT_COLOR4   (startColor,     "Start Color")
    REFLECT_COLOR4   (endColor,       "End Color")
    REFLECT_FLOAT_FMT(intensity,      "Intensity",        0.f, 64.f, "%.2f", 0.05f)
    REFLECT_FLOAT_FMT(emissiveFade,   "Emissive Fade (0=off)", 0.f, 8.f, "%.2f", 0.1f)
    REFLECT_HEADER   ("Size")
    REFLECT_FLOAT    (startSize,      "Start Size",       0.001f, 10.f)
    REFLECT_FLOAT3   (startScale,     "Start Scale (XYZ)", 0.0f, 100.f)
    REFLECT_INFO     ("Per-axis size x startSize. Z only affects custom/oriented (Horizontal/Vertical) quads.")
    REFLECT_HEADER   ("Render")
    REFLECT_ENUM     (blendMode,      "Blend Mode",      kParticleBlendOptions)
    REFLECT_ENUM     (visualMode,     "Visual Mode",     kParticleVisualOptions)
    REFLECT_FLOAT_FMT(velocityStretch,"Velocity Stretch", 0.f, 2.f, "%.3f", 0.005f)
    REFLECT_INFO     ("Only used by the 'Velocity Stretched' orientation. Streak length = speed x this.")
    REFLECT_STRING_DROP(texturePath,  "Texture",         "ITEX_PATH")
REFLECT_END()

// ---- ParticleEmitterComponent: standard emit behaviour ----
REFLECT_BEGIN(ParticleEmitterComponent)
    REFLECT_BOOL     (enabled,        "Enabled")
    REFLECT_FLOAT_FMT(spawnRate,      "Spawn Rate (/s)",  0.f, 100000.f, "%.1f", 10.f)
    REFLECT_INT      (maxParticles,   "Max Particles",    1, 131072)
    REFLECT_INFO     ("Live-particle ceiling for this emitter. Raise this AND Spawn Rate for dense effects. Needs a sibling Particle Render component for appearance.")
    REFLECT_INT      (burstCount,     "Burst Count (one-shot)", 0, 8192)
    REFLECT_INFO     ("Set spawnRate=0 + a burst for impact sparks. Use the 'Fire Burst' button below to test.")
    REFLECT_FLOAT_FMT(startLifetime,  "Start Lifetime",   0.05f, 30.f,  "%.2f", 0.05f)
    REFLECT_DRAG   (sizeVariance,   "Size Variance",    0.f, 1.f)
    REFLECT_DRAG   (lifeVariance,   "Life Variance",    0.f, 1.f)
    REFLECT_HEADER   ("Velocity")
    REFLECT_FLOAT3   (velocityMin,    "Velocity Min",    -1e3f, 1e3f)
    REFLECT_FLOAT3   (velocityMax,    "Velocity Max",    -1e3f, 1e3f)
    REFLECT_FLOAT3   (gravity,        "Gravity",         -100.f, 100.f)
    REFLECT_FLOAT_FMT(drag,           "Drag (/s)",        0.f, 20.f, "%.2f", 0.05f)
    REFLECT_HEADER   ("Shape")
    REFLECT_ENUM     (shape,          "Shape",           kParticleShapeOptions)
    REFLECT_IF([](const void* o) {
        return static_cast<const ParticleEmitterComponent*>(o)->shape
            == ParticleShape::Sphere;
    })
        REFLECT_FLOAT(sphereRadius,       "Radius",            0.001f, 100.f)
        REFLECT_BOOL (sphereSpawnOnShell, "Spawn On Shell (surface only)")
    REFLECT_ENDIF()
    REFLECT_IF([](const void* o) {
        return static_cast<const ParticleEmitterComponent*>(o)->shape
            == ParticleShape::Cone;
    })
        REFLECT_ANGLE (coneHalfAngle, "Half Angle",      1.f, 89.f)
        REFLECT_FLOAT3(coneDirection, "Direction",      -1.f, 1.f)
        REFLECT_FLOAT (coneLength,    "Length",          0.f, 100.f)
    REFLECT_ENDIF()
    REFLECT_IF([](const void* o) {
        return static_cast<const ParticleEmitterComponent*>(o)->shape
            == ParticleShape::Box;
    })
        REFLECT_FLOAT3(boxHalfExtents, "Half Extents",   0.001f, 100.f)
    REFLECT_ENDIF()
    REFLECT_IF([](const void* o) {
        return static_cast<const ParticleEmitterComponent*>(o)->shape
            == ParticleShape::Circle;
    })
        REFLECT_FLOAT (circleRadius, "Radius",          0.001f, 100.f)
        REFLECT_FLOAT3(circleNormal, "Normal",         -1.f, 1.f)
    REFLECT_ENDIF()
    REFLECT_IF([](const void* o) {
        return static_cast<const ParticleEmitterComponent*>(o)->shape
            == ParticleShape::Mesh;
    })
        REFLECT_UINT(meshSourceEntity, "Mesh Source Entity ID", 0u, 0xFFFFFFFEu)
        REFLECT_INFO("Pick an entity with MeshHandle or MeshLibRef")
    REFLECT_ENDIF()
    REFLECT_IF([](const void* o) {
        return static_cast<const ParticleEmitterComponent*>(o)->shape
            == ParticleShape::Trail;
    })
        REFLECT_INFO("Sparks along a swinging blade's arc. Put this emitter on the weapon socket "
                     "(same attach as the Trail ribbon). Spawn Rate is IGNORED — Sparks Per Meter "
                     "drives emission so fast and slow swings get the same density along the arc.")
        REFLECT_FLOAT3   (bladeTipLocal,   "Blade Tip (local)", -100.f, 100.f)
        REFLECT_INFO     ("Tip offset in the entity's LOCAL space. Match the Trail component's last "
                          "Local Point to put sparks exactly on the edge (default +Y = PMX bone axis).")
        REFLECT_FLOAT_FMT(sparksPerMeter,  "Sparks Per Meter",  0.f, 2000.f, "%.1f", 1.f)
        REFLECT_FLOAT_FMT(minSwingSpeed,   "Min Swing Speed (m/s)", 0.f, 50.f, "%.2f", 0.25f)
        REFLECT_INFO     ("ABSOLUTE world tip speed below which nothing spawns. It must out-run the "
                          "CARRIER too — an attached emitter inherits locomotion (run ~7.5 m/s), while "
                          "a real swing whips a 1 m tip to ~12-20 m/s. Set below run speed and a "
                          "running character trails sparks.")
        REFLECT_FLOAT_FMT(inheritVelocity, "Inherit Velocity",  0.f, 4.f, "%.2f", 0.05f)
        REFLECT_INFO     ("Fraction of the blade-tip velocity each spark is flung with, on top of "
                          "the Velocity Min/Max scatter. 0 = sparks hang where born.")
    REFLECT_ENDIF()
REFLECT_END()

// ---- ParticleFlockComponent: Boids behaviour ----
REFLECT_BEGIN(ParticleFlockComponent)
    REFLECT_BOOL     (enabled,          "Enabled")
    REFLECT_INFO     ("One-shot persistent flock steered on the GPU. Needs a sibling Particle Render component; use its 'Velocity Stretched' orientation for heading-aligned birds/fish. Re-spawns on world reload.")
    REFLECT_INT      (flockCount,       "Flock Count",        1, 1250)
    REFLECT_FLOAT_FMT(perceptionRadius, "Perception Radius",  0.1f, 50.f,  "%.2f", 0.05f)
    REFLECT_FLOAT_FMT(separationRadius, "Separation Radius",  0.1f, 50.f,  "%.2f", 0.05f)
    REFLECT_FLOAT_FMT(separationWeight, "Separation Weight",  0.f,  10.f,  "%.2f", 0.05f)
    REFLECT_FLOAT_FMT(alignmentWeight,  "Alignment Weight",   0.f,  10.f,  "%.2f", 0.05f)
    REFLECT_FLOAT_FMT(cohesionWeight,   "Cohesion Weight",    0.f,  10.f,  "%.2f", 0.05f)
    REFLECT_FLOAT_FMT(maxSpeed,         "Max Speed",          0.1f, 100.f, "%.2f", 0.1f)
    REFLECT_FLOAT_FMT(maxForce,         "Max Force",          0.1f, 100.f, "%.2f", 0.1f)
    REFLECT_FLOAT_FMT(boundsRadius,     "Containment Radius", 0.f,  200.f, "%.2f", 0.1f)
    REFLECT_FLOAT_FMT(boundsWeight,     "Containment Weight", 0.f,  10.f,  "%.2f", 0.05f)
REFLECT_END()

// ---- ParticleAmbientComponent: camera-follow toroidal wrap volume ----
REFLECT_BEGIN(ParticleAmbientComponent)
    REFLECT_BOOL     (enabled, "Enabled")
    REFLECT_INFO     ("Pre-fills a box that follows the camera; particles wrap toroidally at the "
                      "faces so a fixed budget reads as an infinite field (dust / snow / rain / "
                      "fireflies). Needs a sibling Particle Render component. Max 2 volumes.")
    REFLECT_INT      (ambientCount,     "Count",              1, 131072)
    REFLECT_FLOAT_FMT(ambientDensity,   "Density (/m3, 0=count)", 0.f, 1000.f, "%.3f", 0.1f)
    REFLECT_INFO     ("Population = Density x box volume when Density > 0, else Count. Capped by the 131072 per-bucket slice.")
    REFLECT_FLOAT3   (ambientExtents,   "Box Half Extents (m)", 0.01f, 500.f)
    REFLECT_FLOAT3   (ambientOffset,    "Offset From Camera (world m)", -500.f, 500.f)
    REFLECT_FLOAT_FMT(ambientEdgeFade,  "Edge Fade Band (m)", 0.f, 50.f, "%.2f", 0.05f)
    REFLECT_INFO     ("Face fade that hides pop-in AND the wrap teleport. Must exceed per-frame "
                      "relative motion (camera + particle speed) x ~8 frames or wraps may ghost "
                      "under TAA (~1.5 m walking, 3-5 m for fast rain). 0 = hard faces.")
    REFLECT_FLOAT3   (ambientWind,      "Wind (m/s)",        -50.f, 50.f)
    REFLECT_FLOAT_FMT(ambientNearFade0, "Near Fade Start (m)", 0.f, 5.f,  "%.2f", 0.01f)
    REFLECT_FLOAT_FMT(ambientNearFade1, "Near Fade End (m)",   0.01f, 10.f, "%.2f", 0.01f)
    REFLECT_HEADER   ("Motion")
    REFLECT_FLOAT3   (velocityMin,    "Velocity Min",    -1e3f, 1e3f)
    REFLECT_FLOAT3   (velocityMax,    "Velocity Max",    -1e3f, 1e3f)
    REFLECT_FLOAT_FMT(drag,           "Drag (/s)",        0.f, 20.f, "%.2f", 0.05f)
    REFLECT_FLOAT_FMT(startLifetime,  "Lifetime (0=inf)", 0.0f,  60.f, "%.2f", 0.05f)
REFLECT_END()

// ==== AIComponent: enabled + tick interval ====
// The tree path is read-only info; the per-tick trace lives in postDraw (it needs BTAsset / BTNode
// lookups).

REFLECT_BEGIN(AIComponent)
    REFLECT_BOOL (enabled,      "Enabled")
    REFLECT_FLOAT_FMT(tickInterval, "Tick Interval (s)", 0.f, 5.f, "%.3f", 0.005f)
    REFLECT_INFO_FN("Tree", [](const void* o, char* buf, std::size_t cap) {
        const auto* ai = static_cast<const AIComponent*>(o);
        std::snprintf(buf, cap, "%s",
            ai->treePath.empty() ? "(programmatic)" : ai->treePath.c_str());
    })
    REFLECT_INFO_FN("Time since last tick", [](const void* o, char* buf, std::size_t cap) {
        std::snprintf(buf, cap, "%.3fs",
            static_cast<const AIComponent*>(o)->timeSinceLastTick);
    })
REFLECT_END()

// ==== ECS::PostProcessVolumeComponent: spatial knobs ====
// The referenced PostProcessProfile (per-property override inspector) is drawn in EditorLayer's
// postDraw: it needs ProfileSystem and the X-macro override widgets. Bounds come from the entity
// Transform, so they are not reflected.

constexpr Reflect::EnumOption kPPVolumeShapeOptions[] = {
    { (int)ECS::PPVolumeShape::Box,    "Box"    },
    { (int)ECS::PPVolumeShape::Sphere, "Sphere" },
};

REFLECT_BEGIN(ECS::PostProcessVolumeComponent)
    REFLECT_BOOL (isGlobal, "Global (unbounded)")
    REFLECT_IF([](const void* o) {
        return !static_cast<const ECS::PostProcessVolumeComponent*>(o)->isGlobal;
    })
        REFLECT_ENUM (shape, "Shape", kPPVolumeShapeOptions)
        REFLECT_INFO ("Bounds follow Transform (Box = scale half-extents, Sphere = scale.x radius).")
        REFLECT_FLOAT (blendDistance, "Blend Distance", 0.f, 100.f)
    REFLECT_ENDIF()
    REFLECT_FLOAT (blendWeight, "Blend Weight", 0.f, 1.f)
    REFLECT_FLOAT_FMT(priority, "Priority", -1000.f, 1000.f, "%.1f", 1.0f)
REFLECT_END()

