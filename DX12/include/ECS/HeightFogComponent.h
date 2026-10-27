#pragma once

#include <DirectXMath.h>

// HeightFogComponent: UE-style exponential height fog (analytic, fullscreen).
//
// Lives on the Sky entity next to AtmosphereComponent / CloudComponent; the default environment
// (Scene::SpawnDefaultEnvironment / Scene::MigrateEnvironment) adds it disabled. HeightFogFeature
// pushes the first one into HeightFogPass each frame; sun direction and colour come from LightCB,
// which the time of day owns.
//
// density(h) = fogDensity * exp(-fogHeightFalloff * (h - fogHeight)); the pass integrates the
// closed form along each view ray, sky pixels included (the analytic t -> inf limit), so the
// horizon fogs like UE's height fog. All distances and heights in world metres.
struct HeightFogComponent
{
    bool  enabled            = false;

    float fogDensity         = 0.002f;   // extinction at h == fogHeight (1/m)
    float fogHeightFalloff   = 0.02f;    // 1/m - density halves every ~35 m above fogHeight
    float fogHeight          = 0.0f;     // world Y (m) where density == fogDensity
    float startDistance      = 0.0f;     // fog-free metres in front of the camera
    float maxOpacity         = 1.0f;     // UE FogMaxOpacity: clamp on (1 - transmittance)

    DirectX::XMFLOAT3 fogColor { 0.45f, 0.55f, 0.70f }; // ambient inscatter (linear)
    float sunInscatterIntensity = 1.0f;  // scale on the HG sun term; 0 = off
    float anisotropy            = 0.7f;  // HG g (forward scattering toward the sun)

    // The fog as a participating medium in the path tracers and the ray-traced froxel
    // lighting: sigma_s / sigma_t per channel. There the fog is lit by the scene's lights, so
    // fogColor, sunInscatterIntensity, startDistance and maxOpacity do not apply.
    DirectX::XMFLOAT3 albedo { 1.0f, 1.0f, 1.0f };
};
