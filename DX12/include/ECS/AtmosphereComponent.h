#pragma once

// AtmosphereComponent - atmospheric / IBL / aerial / star knobs only. Time of day
// (sun / moon direction, time, latitude, brightness, moon intensity) lives in
// [[TODComponents.h]] (TODConfigComponent + TODOutputComponent + SunLightTag +
// MoonLightTag).
// The Renderer pushes these fields into SkyIBLPass each frame; SaveScene serialises
// them. Pairs with [[SkyboxComponent]] on the same entity for static IBL cubemaps
// when skyboxSource == Static.

#include <cstdint>
#include <DirectXMath.h>

struct AtmosphereComponent
{
    enum class SkyboxSource : uint8_t { Atmosphere = 0, Static = 1 };

    // ---- Master ----
    // Procedural atmosphere on/off. Off: SkyIBLPass falls back to the static IBL
    // cubemaps from SkyboxComponent.
    bool        atmosphereEnabled = true;

    // Backdrop source for SkyboxPass.
    //   Atmosphere = procedural sky cubemap from Hillaire LUTs
    //   Static     = .itex referenced by paired SkyboxComponent
    SkyboxSource skyboxSource = SkyboxSource::Atmosphere;

    // ---- IBL ----
    // Deprecated: the live gate is WorldEnvironmentComponent.iblStrength (Environment
    // panel). This field is only the inherit fallback when
    // WorldEnvironment.iblStrength < 0; hidden in the inspector, still serialized for
    // scene-file compatibility. Gates SKY-DERIVED indirect only (DDGI has
    // ddgiDiffuseScale).
    float       iblStrength   = 0.1f;

    // ---- Aerial Perspective ----
    // Distance-fog composite in LightingPass. Off by default: it depends on the
    // scene's world-unit-to-km scale.
    bool        aerialCompositeEnabled = false;

    // ---- Sun transmittance ----
    // Filter the directional sun colour by the atmosphere it crosses (Unreal's
    // "Atmosphere Sun Light"): Rayleigh strips blue first, so a white sun comes out
    // warm, strongly near the horizon, keeping the warm-key / cool-sky daylight look.
    // Applied by Renderer::SyncSkyboxIBL to LightCB.lightColor AFTER the sky and cloud
    // passes took their untinted copy: they integrate transmittance themselves and would
    // double-count it. Procedural atmosphere only; a static skybox bakes its own sun.
    // Below the horizon the transmittance is zero, so a sun aimed upward goes black:
    // right for a setting sun, wrong for an up-facing "bounce fill" directional (turn
    // this off or use a real fill light).
    bool        sunTransmittanceEnabled = true;

    // Dial the effect back without losing it: 0 = authored colour untouched,
    // 1 = full physical transmittance. Lerps colour toward the transmitted
    // value, so intermediate values keep some of the dimming as well as the tint.
    float       sunTransmittanceStrength = 1.0f;

    // ---- Stars ----
    // Stars per cubemap face (procedural star bake density).
    float       starDensity    = 256.0f;
    // Star brightness multiplier (0..1 typical).
    float       starBrightness = 0.7f;

    // Volumetric fog sun strength is owned by the directional light: attach a
    // VolumetricLightComponent to a LightData(Directional) entity to opt it into fog
    // god-rays. This component deliberately has no fog-strength field.
};
