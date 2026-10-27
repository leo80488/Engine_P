#ifndef GRASS_HLSLI
#define GRASS_HLSLI

// Grass.hlsli - shared declarations for the GoT-style procedural grass pass.
//
// Ghost-of-Tsushima-inspired pipeline (GDC 2021 "Procedural Grass in 'Ghost of
// Tsushima'", adapted to this engine's AS -> MS architecture):
//   - The grass field is a patchesPerSide x patchesPerSide grid of patches.
//   - Grass.as.hlsl: one AS thread per patch - frustum + distance cull, pick a LOD
//     (segment count / blades-per-group), compute how many MS groups the patch
//     needs, prefix-sum them (group-scope scan) into the per-lane payload.
//   - Grass.ms.hlsl: one MS group emits up to N blades. Every blade is generated
//     from a hash - no vertex / instance buffers exist anywhere. A blade is a
//     quadratic Bezier ribbon: root pinned to the terrain heightmap, tip
//     tilted / bent and displaced by wind noise.
//   - Grass.ps.hlsl: writes the deferred GBuffer (the same 6 MRTs as Terrain.ps),
//     so grass receives CSM shadows / DDGI / SSAO through LightingPass with no
//     special-casing downstream.
//
// The CB layout MUST match GrassCBData in GrassPass.h bit for bit (20 rows).

cbuffer GrassCB : register(b2, space0)
{
    // row 0 - grass field placement (bottom-left corner, world XZ)
    float2 g_grassOrigin;
    float  g_grassSize;
    float  g_baseY;             // terrain base Y (heightmap value 0)

    // row 1
    float  g_heightScale;       // terrain heightmap world amplitude
    float  g_hmTexel;           // 1 / heightmap resolution
    uint   g_hasHeightmap;      // 0 -> flat field at g_baseY
    uint   g_patchesPerSide;

    // row 2 - terrain heightmap UV remap (matches the Terrain.ms.hlsl math)
    float2 g_hmUVOffset;
    float2 g_hmUVScale;

    // row 3 - the TERRAIN tile mapping (may differ from the grass field)
    float2 g_terrainOrigin;
    float  g_terrainSize;
    float  _g_padT;

    // row 4
    float3 g_cameraPos;
    float  g_time;

    // row 5
    float  g_prevTime;
    float  g_lod0Dist;
    float  g_lod1Dist;
    float  g_cullDist;

    // row 6
    float  g_density;           // blades per m^2 at LOD0
    float  g_bladeHeight;
    float  g_bladeHeightVar;    // +-fraction of bladeHeight
    float  g_bladeWidth;        // half-profile base width (m)

    // row 7
    float  g_tiltMax;           // radians - max static lean from vertical
    float  g_bendAmount;        // Bezier mid-point slack [0..1]
    float  g_windStrength;      // tip displacement amplitude (m)
    float  g_windSpeed;

    // row 8
    float  g_windScale;         // noise cycles per metre
    float2 g_windDir;           // normalized XZ
    float  g_clumpCellSize;     // Voronoi-ish clump cell size (m)

    // row 9
    float  g_clumpBlend;        // 0 = independent blades, 1 = full clump identity
    float  g_minWorldY;         // no grass below (e.g. water level)
    float  g_maxWorldY;         // no grass above (e.g. snow line)
    float  g_maxSlopeCos;       // cos(max slope): gate on dot(geometric normal, up)

    // rows 10/11
    float4 g_baseColor;         // rgb, a unused
    float4 g_tipColor;

    // row 12
    float  g_colorNoiseScale;   // cycles per metre
    float  g_colorNoiseAmount;
    float  g_rootAO;            // AO at the blade root (1 at tip)
    float  g_normalBlend;       // blade normal -> up-vector soften

    // row 13
    float  g_viewThicken;       // edge-on view width compensation [0..1]
    float  g_farWidthMul;       // LOD2 width multiplier (coverage keeper)
    float  g_roughness;
    uint   g_seed;

    // rows 14..19
    float4 g_grassFrustumPlanes[6];   // inward normals: dot(n,P)+d >= 0 inside
};

// ==== LOD table ====
// Segments / blades-per-MS-group per LOD. MS limits: 256 verts / 256 prims.
//   LOD0: 7 segs -> 15 v / 13 t per blade x 16 blades = 240 v / 208 t
//   LOD1: 3 segs ->  7 v /  5 t per blade x 32 blades = 224 v / 160 t
//   LOD2: 2 segs ->  5 v /  3 t per blade x 48 blades = 240 v / 144 t
static const uint kGrassSegs  [3] = { 7, 3, 2 };
static const uint kGrassBPG   [3] = { 16, 32, 48 };  // blades per MS group
static const float kGrassDensityMul[3] = { 1.0, 0.45, 0.18 };
#define GRASS_MAX_BLADES_PER_PATCH 256
#define GRASS_AS_GROUP_SIZE 32

struct GrassPayload
{
    // Per-AS-lane entries (NOT compacted): a culled lane has bladeCount 0, so its MS
    // group range is empty and the MS scan can never match it.
    uint patchIdx  [GRASS_AS_GROUP_SIZE];
    uint groupBase [GRASS_AS_GROUP_SIZE];   // exclusive prefix sum of group counts
    uint bladeCount[GRASS_AS_GROUP_SIZE];
    uint lodLevel  [GRASS_AS_GROUP_SIZE];
};

// ==== Hashing ====

#include "core/hash.hlsli"

// [0,1) float from three uints (patch id, blade id, stream)
float GrassHash(uint a, uint b, uint stream)
{
    uint h = HashLowbias32(a * 0x9E3779B9u + b + stream * 0x85EBCA6Bu + g_seed);
    return float(h & 0x00FFFFFFu) / 16777216.0;
}


// 8-bit bit reversal: maps a blade index -> a scattered cell of a 16x16 grid, so
// "take the first N blades" (the distance density falloff) stays spatially uniform
// AND blade positions stay stable across LOD / density changes (GoT: blades must
// not swim when density drops with distance).
uint GrassBitRev8(uint v)
{
    v = ((v & 0x55u) << 1) | ((v & 0xAAu) >> 1);
    v = ((v & 0x33u) << 2) | ((v & 0xCCu) >> 2);
    v = ((v & 0x0Fu) << 4) | ((v & 0xF0u) >> 4);
    return v;
}

// ==== Value noise (analytic, no texture) ====

float GrassValueNoise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    float a = HashSin22(i + float2(0, 0)).x;
    float b = HashSin22(i + float2(1, 0)).x;
    float c = HashSin22(i + float2(0, 1)).x;
    float d = HashSin22(i + float2(1, 1)).x;
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

// Wind sway in [0..1]: a travelling sine front + a gust noise field, both
// scrolling along windDir. The GoT "scrolling noise" feel without a texture fetch
// (AS/MS friendly).
float GrassWindAmp(float2 xz, float t)
{
    float2 wdir = g_windDir;
    float wave = sin(dot(xz, wdir) * (g_windScale * 6.2831853) * 0.45
                     + t * g_windSpeed * 2.1) * 0.5 + 0.5;
    // Gust = TWO value-noise octaves scrolling at different rates. A single octave
    // stalls: the Hermite fade's time derivative is zero at every lattice crossing, so
    // the slow sway holds-then-lurches (~1 Hz at defaults), which reads as dropped
    // frames. Decorrelated octave rates keep the derivative zeros from aligning.
    float2 p = xz * g_windScale - wdir * (t * g_windSpeed * 0.7);
    float gust = 0.62 * GrassValueNoise(p)
               + 0.38 * GrassValueNoise(p * 2.13 + 19.7 - wdir * (t * g_windSpeed * 0.37));
    // The coefficients sum to exactly 1.0: a larger sum exceeds the saturate()
    // ceiling in strong gusts and flat-tops (blades pinned rigid at max displacement).
    return saturate(0.30 + 0.35 * gust + 0.35 * wave * gust);
}

// ==== Terrain height (must mirror Terrain.ms.hlsl SampleHeight) ====
// Callers supply the heightmap SRV + clamp sampler (registers differ per stage).

float GrassTerrainUVValid(float2 worldXZ, out float2 hmUV)
{
    float2 norm = (worldXZ - g_terrainOrigin) / max(g_terrainSize, 1e-3);
    hmUV = g_hmUVOffset + norm * g_hmUVScale;
    return (norm.x >= 0.0 && norm.x <= 1.0 && norm.y >= 0.0 && norm.y <= 1.0) ? 1.0 : 0.0;
}

#endif // GRASS_HLSLI
