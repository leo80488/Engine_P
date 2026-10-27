// Shared cbuffer and helpers for the RTShadowPass NRD chain (RTShadow.cs.hlsl fused
// trace + pack, RTShadowResolve.cs.hlsl). The cbuffer mirrors
// RTShadowPass.h::RTShadowConstants: keep the field order in lockstep (the CPU struct
// has a static_assert on the byte size).
#ifndef RT_SHADOW_COMMON_HLSLI
#define RT_SHADOW_COMMON_HLSLI

cbuffer RTShadowConstants : register(b0, space0)
{
    float4x4 invViewProj;       // current, jittered - matches the depth buffer (uploaded transposed)
    float3   sunDirection;      // light travel direction (LightCB.lightDir convention)
    float    sunTanHalfAngle;   // tan(sun angular radius): penumbra width
    uint     screenW;           uint  screenH;
    float    invScreenW;        float invScreenH;
    float    normalBias;        float rayTMax;
    uint     frameIndex;        float nearZ;
    // Alpha-cutout occluders. alphaTestEnabled == 0 -> accept every candidate (every
    // occluder solid). alphaMipLevel is the explicit mip for the cutout sample: an inline
    // RayQuery has no ray differentials, so the mip cannot be implicit.
    float    farZ;               uint  alphaTestEnabled;
    uint     alphaInstanceCount; float alphaMipLevel;
};

// Reverse-Z world-position reconstruction (engine convention:
// mul(vector, matrix), matrices uploaded transposed).
#include "core/depth.hlsli"   // LinearizeReverseZ, WorldPosFromDepth
float3 RTS_ReconstructWorldPos(float2 uv, float depth)
{
    return WorldPosFromDepth(uv, depth, invViewProj);
}

// Reverse-Z NDC depth -> positive linear view distance.
float RTS_LinearizeReverseZ(float zNdc)
{
    return LinearizeReverseZ(zNdc, nearZ, farZ);
}

#endif // RT_SHADOW_COMMON_HLSLI
