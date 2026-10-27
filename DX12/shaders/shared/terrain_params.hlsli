// The terrain constant buffer (b2, space0) as C++ fills it and the five terrain stages
// read it: Terrain.as / .ms / .ps and Terrain.shadow.as / .ms. Each shader declares
//     ConstantBuffer<TerrainParams> g_terrain : register(b2, space0);
// and C++ names it RendererDetail::TerrainParamsCB (include/Renderer/Renderer.h).
//
// Geometry only: per-layer material data lives in a variable-count
// StructuredBuffer<TerrainLayerGPU>, so the layer count is data-driven and the geometry /
// shadow stages carry no PS-only rows. Only the colour AS reads frustumPlanes. A constant
// buffer: every member is HLSL_ROW_SAFE, the array HLSL_ROW_ARRAY.
//
// Keep this file pure ASCII (gpu_struct_types.hlsli).

#ifndef SHARED_TERRAIN_PARAMS_HLSLI
#define SHARED_TERRAIN_PARAMS_HLSLI

#include "gpu_struct_types.hlsli"

GPU_SHARED_BEGIN

struct GPU_SHARED_ALIGN16 TerrainParams
{
    float2 worldOrigin;           // [0]  the bottom-left corner (XZ; .y is world Z): MS = origin + grid * step
    float  worldSize;             // [8]
    float  heightScale;           // [12]

    float2 heightmapUVOffset;     // [16]
    float2 heightmapUVScale;      // [24]

    float  heightmapTexel;        // [32] 1 / heightmap width (the analytic normal's gradient step)
    uint   hasHeightmap;          // [36]
    uint   hasSplatmap;           // [40]
    float  worldCenterY;          // [44]

    uint   tilesPerSide;          // [48]
    uint   enableFrustumCull;     // [52] the colour pass culls against the camera frustum
    uint   layerCount;            // [56] valid entries in the layer StructuredBuffer (PS)
    uint   heightBlendEnable;     // [60] 0 = plain linear blend (PS)

    float  heightBlendStrength;   // [64] displacement bias on the base weight (PS)
    float  heightBlendRange;      // [68] soft cutoff width around the local max (PS)
    float  _pad1;
    float  _pad2;

    float4 frustumPlanes[6];      // [80] colour AS only: inward normals, dot(p.xyz, P) + p.w >= 0 is inside
};

GPU_SHARED_SIZE(TerrainParams, 176)
HLSL_ROW_SAFE(TerrainParams, worldOrigin)
HLSL_ROW_SAFE(TerrainParams, worldSize)
HLSL_ROW_SAFE(TerrainParams, heightScale)
HLSL_ROW_SAFE(TerrainParams, heightmapUVOffset)
HLSL_ROW_SAFE(TerrainParams, heightmapUVScale)
HLSL_ROW_SAFE(TerrainParams, heightmapTexel)
HLSL_ROW_SAFE(TerrainParams, hasHeightmap)
HLSL_ROW_SAFE(TerrainParams, hasSplatmap)
HLSL_ROW_SAFE(TerrainParams, worldCenterY)
HLSL_ROW_SAFE(TerrainParams, tilesPerSide)
HLSL_ROW_SAFE(TerrainParams, enableFrustumCull)
HLSL_ROW_SAFE(TerrainParams, layerCount)
HLSL_ROW_SAFE(TerrainParams, heightBlendEnable)
HLSL_ROW_SAFE(TerrainParams, heightBlendStrength)
HLSL_ROW_SAFE(TerrainParams, heightBlendRange)
HLSL_ROW_ARRAY(TerrainParams, frustumPlanes)
GPU_SHARED_OFFSET(TerrainParams, heightmapUVOffset, 16)
GPU_SHARED_OFFSET(TerrainParams, heightmapTexel, 32)
GPU_SHARED_OFFSET(TerrainParams, tilesPerSide, 48)
GPU_SHARED_OFFSET(TerrainParams, heightBlendStrength, 64)
GPU_SHARED_OFFSET(TerrainParams, frustumPlanes, 80)

GPU_SHARED_END

#endif // SHARED_TERRAIN_PARAMS_HLSLI
