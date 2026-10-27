#pragma once

#include <DirectXMath.h>

// The scene's global participating medium (UE "Exponential Height Fog > Volumetric Fog", HDRP
// "Fog" override). Singleton by convention (first pool entry wins), usually on the Sky entity.
// Its presence is the ownership switch:
//   present -> it alone drives the global froxel fog; the player's "Volumetric Fog" option in
//              settings.json and a scene .ippc's VolFog line are ignored.
//   absent  -> settings.json / the .ippc drive VolumetricFogPass directly.
// LocalFogVolumeComponent volumes inject in both cases and keep the froxel pipeline running even
// when the global medium is off.
// Medium model (HDRP-style, one extinction + an albedo):
//   sigma_t(h) = density x exp(-heightFalloff x max(h - baseHeight, 0))   [1/m]
//   sigma_s    = sigma_t x albedo   (albedo 1 = no absorption; tints the scatter)
//   Le         = emissive x emissiveIntensity x sigma_t   (unlit self-illumination)
// Sun shafts come from the directional light's VolumetricLightComponent (the light-side opt-in,
// UE's "Volumetric Scattering Intensity").
struct VolumetricFogComponent
{
    bool  enabled         = true;

    // ---- Medium ----
    float density         = 0.02f;   // extinction sigma_t at / below baseHeight (1/m)
    DirectX::XMFLOAT3 albedo { 1.0f, 1.0f, 1.0f }; // sigma_s / sigma_t per channel
    float anisotropy      = 0.4f;    // Henyey-Greenstein g (+ = forward scatter)

    // ---- Height ----
    float baseHeight      = 0.0f;    // world Y (m); density is constant below it
    float heightFalloff   = 0.05f;   // 1/m above baseHeight (0 = uniform)

    // ---- Lighting ----
    DirectX::XMFLOAT3 ambientColor { 0.4f, 0.55f, 0.85f }; // sky stand-in (linear)
    float ambientIntensity = 0.4f;
    DirectX::XMFLOAT3 emissive { 0.0f, 0.0f, 0.0f };      // self-illumination colour
    float emissiveIntensity = 0.0f;

    // ---- Range and quality ----
    float startDistance   = 0.5f;    // froxel grid near plane (m)
    float viewDistance    = 80.0f;   // froxel grid far plane (m); beyond = no volumetric fog
    bool  godRays         = true;    // per-pixel raymarch tier (sun + shadowed spots)
    bool  temporal        = true;    // temporal reprojection of the froxel volume
    float temporalBlend   = 0.12f;   // new-frame weight (lower = smoother, slower)
};
