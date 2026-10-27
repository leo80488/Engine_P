#pragma once

// GrassComponent: GoT-style procedural grass field (one component = one square field of grass
// patches).
//
// Nothing is authored per blade: every blade is hash-generated on the GPU each frame
// (Grass.as/ms/ps.hlsl). The field is a patchesPerSide x patchesPerSide grid; the amplification
// shader frustum- and distance-culls patches and picks a LOD, the mesh shader builds Bezier-ribbon
// blades pinned to the heightmap of the scene's first TerrainComponent (without terrain, to the
// plane y = worldCenter.y).
//
// Renderer::BuildScene_SyncGrass gathers this component and the terrain heightmap SRV each frame
// and arms GrassPass. Singleton by convention: only the first live GrassComponent renders.

#include <DirectXMath.h>
#include <cstdint>

struct GrassComponent
{
    bool enabled = true;

    // ---- Field placement (same convention as TerrainComponent) ----
    // X in [worldCenter.x - worldSize/2, worldCenter.x + worldSize/2], same for Z.
    // worldCenter.y is the ground plane only when no terrain exists.
    DirectX::XMFLOAT3 worldCenter { 0.0f, 0.0f, 0.0f };
    float             worldSize   = 1024.0f;

    // Field subdivision. patchSize = worldSize / patchesPerSide; the AS culls per patch, so smaller
    // patches cull tighter but dispatch more AS threads. 256 -> 4 m patches on a 1024 m field.
    uint32_t patchesPerSide = 256;

    // ---- Density & LOD ----
    float density   = 8.0f;     // blades per m^2 at LOD0 (near field)
    float lod0Dist  = 24.0f;    // < this: 7-segment blades
    float lod1Dist  = 64.0f;    // < this: 3-segment blades; beyond: 2-segment
    float cullDist  = 140.0f;   // hard patch cull (density fades out before it)

    // ---- Blade shape ----
    float bladeHeight    = 0.85f;  // m
    float bladeHeightVar = 0.35f;  // +- fraction
    float bladeWidth     = 0.045f; // m (half-profile at the root)
    float tiltMaxDeg     = 38.0f;  // max static lean from vertical
    float bendAmount     = 0.55f;  // Bezier mid-point slack [0..1]

    // ---- Wind ----
    DirectX::XMFLOAT2 windDir { 0.8f, 0.6f };  // XZ, normalized at sync
    float windStrength = 0.30f;   // tip displacement (m)
    float windSpeed    = 1.3f;
    float windScale    = 0.06f;   // gust-noise cycles per metre

    // ---- Clumping (Voronoi-lite cells, GoT clump identity) ----
    float clumpCellSize = 1.7f;   // m
    float clumpBlend    = 0.55f;  // 0 = independent blades, 1 = full clump

    // ---- Placement gates ----
    float minWorldY   = -1.0e6f;  // no grass below (set to the water line!)
    float maxWorldY   =  1.0e6f;  // no grass above (snow line)
    float maxSlopeDeg = 38.0f;    // no grass on cliffs

    // ---- Look ----
    DirectX::XMFLOAT3 baseColor { 0.045f, 0.16f, 0.030f };  // root (linear)
    DirectX::XMFLOAT3 tipColor  { 0.32f,  0.50f, 0.12f  };  // tip  (linear)
    float colorNoiseScale  = 0.045f;  // patchwork hue cycles per metre
    float colorNoiseAmount = 0.35f;
    float rootAO      = 0.35f;   // fake occlusion at the root
    float normalBlend = 0.55f;   // blade normal -> up-vector soften
    float viewThicken = 0.6f;    // edge-on width compensation
    float farWidthMul = 1.7f;    // distant blades widen (coverage keeper)
    float roughness   = 0.70f;

    uint32_t seed = 1337u;
};
