// Shared trail GPU layouts; they must match include/Graphics/TrailSystem.h exactly, whose
// static_asserts pin the sizes: TrailSegment 32 B, TrailHeader 80 B, TrailAppendRequest 128 B,
// TrailSystemParams 16 B. Change both sides together.

#ifndef TRAIL_HLSLI
#define TRAIL_HLSLI

// MUST mirror TrailSystem::kMaxPointsPerTrail / kMaxSubdivisions.
#define TRAIL_MAX_POINTS 4
#define TRAIL_MAX_SUBDIV 4

// Point-major record addressing:
//   record = ((trailSlot * maxSegments) + sampleSlot) * TRAIL_MAX_POINTS + p
struct TrailSegment
{
    float3 position;  // world-space position of point p of this sample
    float  age;       // seconds since the sample spawned
    float  arcLen;    // cumulative polyline length of THIS point row, metres
    uint   _pad0;
    uint   _pad1;
    uint   _pad2;
};                     // 32 bytes

struct TrailHeader
{
    uint   head;               // next write slot, in SAMPLES (ring cursor)
    uint   count;              // live sample count
    float  width;              // camera-facing half-width (pointCount == 1)
    float  maxAge;
    float4 startColor;
    float4 endColor;
    uint   pointCount;         // 1..TRAIL_MAX_POINTS; a zero-initialised 0 reads as 1
    uint   textureBindlessIdx; // 0xFFFFFFFF = untextured
    uint   renderFlags;        // bits 0 arc UV, 1 additive, 2 TAA-exclude, 3 world-anchored UV; 8-15 shader bucket id
    uint   subdivisions;       // 1..TRAIL_MAX_SUBDIV
    float  uvTileLength;       // 0 = stretch once over the live trail
    float  hdrIntensity;
    uint   _reserved[2];
};                              // 80 bytes

struct TrailAppendRequest
{
    // First 64 bytes: the base point and the header parameters.
    float3 position;           // point 0 (base) world position
    float  dt;
    uint   trailSlot;
    float  width;
    float  maxAge;
    uint   flags;              // bit0 ResetHeader; bit1 ResetOnly; bit2 RefreshParams
    float4 startColor;
    float4 endColor;
    // Last 64 bytes: points 1-3 and the multi-point parameters.
    float4 position1;          // xyz = point 1; w = uvTileLength
    float4 position2;          // xyz = point 2; w = hdrIntensity
    float4 position3;          // xyz = point 3; w = ageOffset
    uint   pointCount;
    uint   textureBindlessIdx;
    uint   renderFlags;
    uint   subdivisions;
};                              // 128 bytes

struct TrailSystemParams
{
    float deltaTime;
    uint  requestCount;
    uint  maxSegments;
    uint  maxTrails;
};                              // 16 bytes

#endif
