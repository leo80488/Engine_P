#pragma once

// CloudComponent: volumetric cloud authoring knobs (singleton).
// Usually on the Sky entity; the Renderer uses the first instance in the world
// each frame. Drives CloudPass: a quarter-res raymarch through a spherical shell
// [bottomAltitude, topAltitude] with the Nubis density model (Perlin-Worley base
// + weather map + per-type height gradient + Worley detail erosion) and Frostbite
// lighting (multi-scatter octaves, dual-lobe HG + silver lining,
// energy-conserving integration).
// Sun direction and colour come from TODOutputComponent each frame; the values
// here are independent of time of day.

#include <cstdint>
#include <DirectXMath.h>

struct CloudComponent
{
    // ---- Master ----
    bool        enabled = false;

    // ---- Layer geometry (world-space metres) ----
    float       bottomAltitude = 1500.0f;
    float       topAltitude    = 4000.0f;

    // ---- Shape ----
    // Global coverage. 0 = clear sky, 0.5 = the weather map's distinct
    // formations as authored, 1 = overcast (secondary fill field floods in).
    float       coverage  = 0.50f;
    // Multiplier on density after shaping. Higher = thicker clouds.
    float       density   = 1.0f;
    // World-units -> base-shape sample period. 0.00025 = 4 km per tile.
    float       noiseScale = 0.00025f;
    // World-units -> detail-erosion sample period. 0.002 = 500 m per tile.
    float       detailNoiseScale = 0.002f;
    // How deep the high-frequency erosion eats into cloud edges (0 = none).
    float       detailStrength = 0.30f;
    // World-units -> weather-map period. 0.00006 = 16.7 km per tile. Much larger
    // tiles (50 km: a 12.5 km dominant feature) put all the sky a ground camera sees
    // inside one feature, so overhead cover is uniformly empty or uniformly covered
    // and formations show only at the horizon.
    float       weatherScale = 0.00006f;
    // Shifts the weather map's cloud type: -1 = force stratus, 0 = as
    // authored, +1 = force cumulus.
    float       cloudTypeBias = 0.0f;
    // 0 = none, 1 = full cumulonimbus anvil spread near the layer top.
    float       anvilBias = 0.0f;

    // ---- Wind ----
    // Wind direction (does not have to be normalised). Multiplied by speed *
    // time and added to the noise sample position.
    DirectX::XMFLOAT3 windDirection { 1.0f, 0.0f, 0.0f };
    float             windSpeed     = 8.0f;   // m/s

    // ---- Lighting ----
    // Dual-lobe Henyey-Greenstein: forward lobe g (silver lining when the
    // sun is behind the cloud) ...
    float       anisotropy = 0.6f;
    // ... blended with a backward lobe so the anti-solar side keeps shape.
    float       phaseBackG = -0.2f;
    float       phaseBlend = 0.3f;
    // Extra forward "silver lining" lobe, combined with max() (Nubis).
    float       silverIntensity = 0.8f;
    float       silverSpread    = 0.25f;   // lobe g = 0.99 - spread
    // Beer's-law extinction coefficient (1/m). Real water clouds measure 0.04-0.12,
    // but there a core's sun-march optical depth reaches 8-11 and the 4-octave
    // Wrenninge multi-scatter approximation cannot carry light back out (interiors
    // read as soot). 0.02 keeps cores opaque to the view ray (mean transmittance
    // ~0.3 through a deck) while the sun supplies 40-50% of the radiance, which is
    // what gives visible form. Density multiplies on top.
    float       extinction = 0.02f;
    // Ambient skylight fill, scaled by sun luminance (tracks time of day) and a
    // bottom-occlusion height gradient. Keep it small: the sun term carries a
    // normalised HG phase (mean 1/4pi = 0.08) and the Nubis in-scatter probability
    // (~0.2), the ambient term neither, so 0.5 already acts like an isotropic phase
    // six times the sun's and flattens all form. 0.06 is roughly sky radiance /
    // solar irradiance on a clear day (~0.15/pi).
    float       ambientStrength = 0.06f;
    DirectX::XMFLOAT3 ambientTint { 0.55f, 0.65f, 0.85f };
    // Base cloud albedo tint. White by default (clouds get their warmth from
    // the sun colour driven by TOD).
    DirectX::XMFLOAT3 cloudColor { 1.0f, 1.0f, 1.0f };

    // ---- Cloud shadow ----
    // Master switch for the shadow the clouds cast on the world. Off is a real off:
    // the bake is skipped, the 512x512 map is released, and every consumer
    // (deferred lighting, water, volumetric light shafts) sees no map bound and
    // reads full sun. Turning it back on re-creates the map on the next frame.
    bool        cloudShadowEnabled = true;
    // Shadow density. Scales the OPTICAL DEPTH, so 0 makes the baked map exactly
    // 1.0 everywhere (a true no-op, not a washed-out grey) and skips the bake too.
    // Use cloudShadowEnabled to also free the texture.
    float       cloudShadowStrength = 1.0f;
    // HALF-size of the square the shadow map covers, in metres, centred on the
    // camera (so 20000 = a 40 km footprint). Larger covers more of a long view
    // at lower resolution; the outer 10% is faded so the moving footprint has
    // no visible border.
    float       cloudShadowExtent = 20000.0f;

    // ---- Quality ----
    // Ceiling on view-ray samples. The march targets a fixed 40 m world-space
    // step and only hits this cap on long near-horizon rays, so raising it buys
    // horizon quality and costs nothing looking up.
    float       maxSteps = 128.0f;
    // Reproject + accumulate the quarter-res raymarch across frames
    // (CloudTemporal.cs.hlsl). Off = the single-frame result is composited
    // directly and the ray jitter reverts to frame-static, which is noticeably
    // grainier but immune to any reprojection artefact.
    bool        temporalUpsample = true;
};
