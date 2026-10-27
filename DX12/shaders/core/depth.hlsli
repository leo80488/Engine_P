// core/depth.hlsli - the engine's depth conventions as arithmetic. The rules every core header
// follows are at the top of core/gbuffer_codec.hlsli.
//
// Reversed Z everywhere: NDC z = 1 at the near plane, 0 at the far plane (the renderer swaps near
// and far in its projection builders; the sky is exactly 0). Matrices are uploaded transposed:
// positions are row vectors, mul(v, M). Screen UV has its origin top-left, so ndc.y = 1 - uv.y * 2.

#ifndef CORE_DEPTH_HLSLI
#define CORE_DEPTH_HLSLI

// Reverse-Z NDC depth -> positive linear view distance, for the finite
// perspective the renderer builds (near and far swapped, not an infinite far):
//   ndc.z  = (n f) / (view.z (f - n)) - n / (f - n)
//   view.z = (n f) / (n + ndc.z (f - n));  ndc 1 -> n, ndc 0 -> f.
float LinearizeReverseZ(float ndcZ, float nearZ, float farZ)
{
    return (nearZ * farZ) / (nearZ + (farZ - nearZ) * ndcZ);
}

// The world position a screen UV sees at an NDC depth.
float3 WorldPosFromDepth(float2 uv, float ndcZ, float4x4 invViewProj)
{
    float2 ndcXY = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 hp    = mul(float4(ndcXY, ndcZ, 1.0), invViewProj);
    return hp.xyz / hp.w;
}

// A unit world-space view ray through a screen UV, from the near plane to the
// far plane - independent of the depth buffer (ray marching from the camera).
float3 ViewRayFromUV(float2 uv, float4x4 invViewProj)
{
    float2 ndcXY = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 nearH = mul(float4(ndcXY, 1.0, 1.0), invViewProj);
    float4 farH  = mul(float4(ndcXY, 0.0, 1.0), invViewProj);
    return normalize(farH.xyz / farH.w - nearH.xyz / nearH.w);
}

#endif // CORE_DEPTH_HLSLI
