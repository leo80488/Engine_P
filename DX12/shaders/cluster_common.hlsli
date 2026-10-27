// cluster_common.hlsli - shared definitions for clustered deferred shading.
// Used by ClusterBuild.cs.hlsl, ClusterCull.cs.hlsl and Lighting.ps.hlsl.

#ifndef CLUSTER_COMMON_HLSLI
#define CLUSTER_COMMON_HLSLI

// ==== Grid constants ====
// Must match the C++ side (ClusterPass.h).
// XY is a FIXED TILE COUNT, not a pixel size: the tile footprint scales with the render
// resolution. 48x27 is exactly 16:9, so tiles stay square on a 16:9 target (40x40 px at
// 1080p, 80x80 px at 4K). 31104 clusters put the light index list at 3.8 MiB
// (31104 * 32 * 4 B) and the cluster working set at ~8.3 MiB; 16x16-px tiles would need
// 195840 clusters / 25 MB for the index list alone, and the cull is brute-force
// O(clusters * lights).
#define CLUSTER_TILE_X   48
#define CLUSTER_TILE_Y   27
#define CLUSTER_SLICES   24
#define CLUSTER_COUNT    (CLUSTER_TILE_X * CLUSTER_TILE_Y * CLUSTER_SLICES)

#define MAX_LIGHT_INDEX_COUNT  (CLUSTER_COUNT * 32)  // average ~32 lights per cluster budget

// ==== Structures ====

struct ClusterAABB
{
    float3 minPt;
    float  _pad0;
    float3 maxPt;
    float  _pad1;
};

// GPULight (64 B): one definition for C++ and every shader. powerBits /
// sourceRadiusBits are IEEE floats in integer storage - decode with asfloat().
#include "shared/gpu_light.hlsli"

struct LightGridEntry
{
    uint offset; // 0xFFFFFFFF: overflow; consume lights [0,count) directly
    uint count;
};

// ==== Helpers ====

uint ClusterIndex(uint3 coord)
{
    return coord.x
         + coord.y * CLUSTER_TILE_X
         + coord.z * CLUSTER_TILE_X * CLUSTER_TILE_Y;
}

uint3 GetClusterCoord(float2 screenUV, float linearDepth, float nearZ, float farZ)
{
    uint tileX = (uint)(screenUV.x * CLUSTER_TILE_X);
    uint tileY = (uint)(screenUV.y * CLUSTER_TILE_Y);

    // Exponential Z-slice: denser near the camera.
    float logRatio = log(linearDepth / nearZ) / log(farZ / nearZ);
    uint  slice    = (uint)(logRatio * CLUSTER_SLICES);
    slice = clamp(slice, 0, CLUSTER_SLICES - 1);

    return uint3(
        min(tileX, CLUSTER_TILE_X - 1),
        min(tileY, CLUSTER_TILE_Y - 1),
        slice);
}

bool SphereVsAABB(float3 center, float radius, ClusterAABB aabb)
{
    float3 closest = clamp(center, aabb.minPt, aabb.maxPt);
    float3 diff = closest - center;
    return dot(diff, diff) <= (radius * radius);
}

#endif // CLUSTER_COMMON_HLSLI
