#pragma once

// One flat water body, drawn by WaterPass (forward, after SkyboxPass): a Fresnel blend between an
// absorption-tinted body colour and the sky cubemap, plus a Blinn sun glint. Surface detail comes
// from two tangent-space normal maps counter-scrolled along flowDir and whiteout-blended (B at a
// finer tiling), plus an optional interactive ripple height field (WaterRippleSystem). An empty or
// unloaded normal-map path falls back to the pass's flat normal (calm water).
// Every live WaterComponent is its own body with its own placement, colours, maps, shading and
// ripples (lake + ponds + river); WaterPass draws them in one pass, one instanced sub-draw per body
// (per-instance b2 CB at a 256-byte offset; per-draw flow-map / heightmap / ripple-field SRVs).
// Water depth (absorption + shoreline fade) is computed from the first TerrainComponent's
// heightmap; without terrain the body is at full depth everywhere.
// Renderer::BuildScene_SyncWater syncs every live component each frame, including texture
// acquire / release when a path changes.

#include "Resource/SystemHandles.h"   // TextureHandle / kInvalidTextureHandle

#include <DirectXMath.h>
#include <cstdint>
#include <string>

struct WaterComponent
{
    bool enabled = true;

    // ---- Placement: worldCenter.y is the water level ----
    DirectX::XMFLOAT3 worldCenter { 0.0f, 0.0f, 0.0f };
    float             worldSize   = 1024.0f;

    // ---- Body colour / depth response ----
    DirectX::XMFLOAT3 deepColor    { 0.012f, 0.07f, 0.14f };  // linear
    DirectX::XMFLOAT3 shallowColor { 0.10f,  0.28f, 0.26f };
    float absorbDist = 6.0f;   // metres of water to reach the deep colour
    float shoreFade  = 1.2f;   // metres of alpha fade at the shoreline

    // ---- Flow normals ----
    // A is the broad swell layer; B scrolls against the flow at a finer tiling for detail.
    std::string normalMapAPath = "engine/textures/water/Water 1_normal.itex";
    std::string normalMapBPath = "engine/textures/water/Water 2_normal.itex";

    DirectX::XMFLOAT2 flowDir { 1.0f, 0.35f };  // normalized at sync
    float flowSpeed      = 0.55f;   // m/s scroll
    float normalTiling   = 0.35f;   // map A repeats per metre (B tiles 2.3x)
    float normalStrength = 0.45f;

    // Renderer-internal: written by BuildScene_SyncWater each frame.
    mutable Resource::TextureHandle normalMapAHandle = Resource::kInvalidTextureHandle;
    mutable Resource::TextureHandle normalMapBHandle = Resource::kInvalidTextureHandle;
    mutable uint64_t                normalMapASRV    = 0;
    mutable uint64_t                normalMapBSRV    = 0;

    // ---- Shading ----
    float fresnelF0    = 0.02f;   // water IOR 1.33
    float reflStrength = 1.0f;    // sky reflection multiplier
    float specPower    = 260.0f;  // sun glint tightness

    // ---- Interactive ripples (WaterRippleSystem) ----
    // Opt-in per body: the renderer allocates a ping-pong height field over the body's footprint,
    // integrates a 2D wave equation each frame and tilts the surface normal by the height gradient.
    // Sources: physics contacts touching the surface and Lua Engine.RippleAt(x, z, radius, strength).
    bool     enableRipples       = false;
    float    ripplePropagation   = 0.15f;  // wave speed (CFL-clamped to <= 0.30 on the GPU)
    float    rippleDamping        = 0.015f;// per-step energy loss (low = rings persist/expand)
    float    rippleNormalStrength = 3.0f;  // direct dial: height gradient -> normal tilt
    float    rippleAmplitude     = 1.0f;   // impulse height per unit source strength
    // Side of the square sim window in metres: ripples are simulated only there, around the
    // interaction, not over the whole body, so a stone in a 4 km ocean makes a local ripple. Keep it
    // close to the area actually interacted with (smaller = finer).
    float    rippleAreaSize      = 120.0f;

    // ---- Mesh ----
    // Flat surface (waves live in the normal field): the grid only needs enough triangles to cover the
    // footprint for depth / cloud / fog compositing.
    uint32_t gridQuads = 64;      // grid resolution per side

    // Renderer-internal: the ripple field's SRV (WaterRippleSystem, keyed on the owning entity);
    // 0 = no ripples this frame (flat).
    mutable uint64_t rippleFieldSRV = 0;
    // Renderer-internal: the sim window's world footprint this frame, published by WaterRippleSystem;
    // the water PS maps world XZ -> field UV with it.
    mutable float    rippleWinOriginX = 0.0f;
    mutable float    rippleWinOriginZ = 0.0f;
    mutable float    rippleWinSize    = 1.0f;
};
