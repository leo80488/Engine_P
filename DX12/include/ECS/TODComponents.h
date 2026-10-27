#pragma once

// TODComponents.h: Time-of-Day singleton components (Unity DOTS / Bevy Resource style).
//   TODConfigComponent : input  - author-set knobs (time, speed, latitude, ...).
//   TODOutputComponent : output - written by TODEvaluationSystem (sun / moon direction, colour,
//                        isMoonActive, ...).
// Only one entity in the world carries them. Opt-in, no auto-spawn: the TOD pipeline runs only
// when a TODConfigComponent exists (the editor's Create > Environment > Time of Day, or
// SceneDefaults for new scenes); deleting the entity sticks. TODOutputComponent is recomputed at
// runtime, not serialized; the TODConfig deserializer re-attaches it on load.
// SunLightTag / MoonLightTag mark the LightData entities the TOD sync systems drive: an entity with
// the tag is the canonical sun (or moon) directional light.

#include <cstdint>
#include <string>
#include <DirectXMath.h>

// ==== TODConfigComponent: author-set input, persisted via SaveScene ====
struct TODConfigComponent
{
    // Master on/off. When false, TOD systems leave outputs untouched so the
    // scene can be lit by static directional lights as authored.
    bool        enabled = false;

    // 0 = midnight, 0.25 = sunrise, 0.5 = noon, 0.75 = sunset; wraps mod 1. Advanced by TODTickSystem
    // each frame when enabled and timeSpeed != 0.
    float       timeOfDay = 0.35f;

    // Day units per real second; 1/60 ~ one real minute per in-game day.
    float       timeSpeed = 0.0f;

    // Geographic latitude in radians; controls how high the sun climbs.
    float       latitudeRad = 0.6f;

    // Master sun-brightness multiplier. Applied to BOTH the geometric sun
    // (atmosphere scattering) and the active body (direct lighting + IBL).
    float       sunBrightnessScale = 1.0f;

    // Moon brightness relative to the sun's basePeak (~1.5% by default).
    float       moonIntensityScale = 0.015f;
};

// ==== TODOutputComponent: computed per frame by TODEvaluationSystem, not serialized ====
struct TODOutputComponent
{
    // Active body: the direction and colour the rest of the engine treats as the directional light -
    // the sun by day, the moon by night.
    DirectX::XMFLOAT3 activeDirection { 0.0f, 1.0f, 0.0f };
    DirectX::XMFLOAT3 activeColor     { 10.0f, 10.0f, 10.0f };

    // Geometric sun: true position even below the horizon, colour zero there. The atmosphere /
    // aerial-perspective shaders sample it so the sky does not compute Rayleigh from the moon at night.
    DirectX::XMFLOAT3 sunDirection { 0.0f, 1.0f, 0.0f };
    DirectX::XMFLOAT3 sunColor     { 10.0f, 10.0f, 10.0f };

    // Moon: opposite the sun, fades smoothly across the horizon.
    DirectX::XMFLOAT3 moonDirection { 0.0f, -1.0f, 0.0f };
    DirectX::XMFLOAT3 moonColor     { 0.055f, 0.070f, 0.100f };

    // True when the sun is below the horizon and the moon drives the active lighting.
    bool isMoonActive   = false;
    // True when the moon disk should be drawn (moon altitude > 0.01); the shader's horizon mask fades
    // its intensity.
    bool moonDiskVisible = false;

    // True every frame TODEvaluationSystem evaluates (TOD enabled), false when disabled: a "TOD is
    // live" flag, not a change edge.
    bool dirty = true;
};

// ==== TODLightComponent ====
// A local light (street lamp, neon sign, window) switched by the time of day, optionally driving a
// glowing mesh's emissive in lockstep. Put it on the light entity.
// C++, not a Lua Logic script (asset/scripts/logic/StreetLightTOD.lua): ScriptLogicSystem is
// SystemRunMask::Play, so a script would freeze the lamps in Edit mode while the author drags the
// Time-of-Day slider. This runs where TOD runs (Renderer::SyncSkyboxIBL, no EngineMode gate), so
// day / night is previewable while authoring; the Lua script stands down on an entity with it.
// The ramp is in sun altitude, not clock time: TODOutput::sunDirection is unit length, so .y is
// sin(elevation), which stays correct when latitudeRad changes (a clock threshold would drift).
// isMoonActive is exactly this crossing zero.
struct TODLightComponent
{
    bool  enabled     = true;

    // Light intensity at full night. Off is intensity 0 - LightData has no enable flag.
    float onIntensity = 25.0f;

    // Emissive strength pushed onto the bulb material. Ignored when
    // bulbMaterial is empty.
    float emissiveOn  = 8.0f;
    float emissiveOff = 0.0f;

    // Sun altitude (sin of elevation) where the lamp STARTS coming on, and
    // where it is FULLY on. duskEnd below 0 = a little past the horizon.
    float duskStart   = 0.12f;
    float duskEnd     = -0.05f;

    // Seconds for a full fade. Also smooths a time-of-day SCRUB, so dragging
    // the slider ramps instead of popping.
    float warmupTime  = 1.5f;

    // Which entity's subtree holds the glowing mesh, and the substring of that mesh's .imat path that
    // identifies it. Resolved by name + material, not an entity reference: the targets are usually
    // .iscn-spawned (no stable GUID), and SceneInstanceLoader names every mesh child `<node>_mesh`, so
    // a lamp's pole and bulb share a name and only the material tells them apart.
    // lampName empty  = use this entity's transform parent.
    // bulbMaterial "" = drive the light only, no emissive.
    std::string lampName;
    std::string bulbMaterial;

    // ---- Runtime, never serialized ----
    float    blend         = -1.0f;             // <0 = first sight, snap to target
    float    lastEmissive  = -1.0f;
    // uint32_t, not Entity: ECS.h defines Entity (== uint32_t) and includes
    // this header's neighbours, so naming the alias here would make the
    // component headers circular for no gain.
    uint32_t resolvedBulb  = 0;                 // NullEntity
    uint32_t resolvedEpoch = 0xFFFFFFFFu;       // World::Epoch() the resolve was done at
};

// ==== Tag components ====
// Empty markers: put one on the LightData (Directional) entity TOD should drive as the sun or the
// moon.
struct SunLightTag  {};
struct MoonLightTag {};
