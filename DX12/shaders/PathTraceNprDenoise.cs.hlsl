// Denoises PathTrace.cs.hlsl NprAmbient's per-frame sample (rgb = sphere-mean incoming
// radiance, a = sun visibility; a = -1 on non-toon pixels) into the texture Lighting.ps's NPR
// variant reads on the path-trace path (LightCB.gNprPtAmbientIdx).
//
//   Temporal  reproject last frame's accumulation through the GBuffer motion
//             (skinned characters move: the vectors carry their pose delta),
//             reject on a view-depth mismatch, running mean capped at 32
//             frames for the ambient and 8 for the visibility (a moving cast
//             shadow must not trail); a sample far above a settled history is
//             clamped (one emissive hit must not stain 32 frames)
//   Spatial   separable, ambient only: the toon ambient is isotropic by design
//             (no normal response to preserve), so the filter weighs depth
//             and receiver membership, never normals. The visibility keeps
//             the temporal result untouched - shadow edges stay sharp.
//
// PathTracePass's root signature (b0 + a directly indexed heap).

cbuffer NprDenoiseParams : register(b0)
{
    uint  width, height, depthIdx, velocityIdx;
    uint  noisyIdx, historyIdx, historyMetaIdx, historyOutUav;
    uint  historyMetaOutUav, spatialInIdx, spatialOutUav, direction;
    float nearZ, farZ; uint reset, nprPad;
};

#include "core/depth.hlsli"   // LinearizeReverseZ, WorldPosFromDepth
float NprLinearizeReverseZ(float z)
{
    return LinearizeReverseZ(z, nearZ, farZ);
}
#include "core/color.hlsli"   // Luminance
float NprLuminance(float3 c) { return Luminance(max(c, 0.0)); }

[numthreads(8,8,1)]
void Temporal(uint3 tid : SV_DispatchThreadID)
{
    const uint2 px = tid.xy;
    if (any(px >= uint2(width, height))) return;
    Texture2D<float4>   noisyTex = ResourceDescriptorHeap[noisyIdx];
    RWTexture2D<float4> histOut  = ResourceDescriptorHeap[historyOutUav];
    RWTexture2D<float2> metaOut  = ResourceDescriptorHeap[historyMetaOutUav];
    float4 s = noisyTex.Load(int3(px, 0));
    if (s.a < 0.0) { histOut[px] = float4(0, 0, 0, -1); metaOut[px] = float2(0, 0); return; }
    Texture2D<float>  depthTex    = ResourceDescriptorHeap[depthIdx];
    Texture2D<float2> velocityTex = ResourceDescriptorHeap[velocityIdx];
    const float viewZ = NprLinearizeReverseZ(depthTex.Load(int3(px, 0)));
    // The engine's motion is the NDC delta (cur - prev).
    const float2 ndcDelta = velocityTex.Load(int3(px, 0));
    const float2 prevUV = (float2(px) + 0.5) / float2(width, height) + float2(-0.5 * ndcDelta.x, 0.5 * ndcDelta.y);
    const int2 prevPx = int2(floor(prevUV * float2(width, height)));
    float4 h = 0;
    float n = 0;
    if (reset == 0u && all(prevPx >= 0) && all(prevPx < int2(width, height)))
    {
        Texture2D<float4> histTex = ResourceDescriptorHeap[historyIdx];
        Texture2D<float2> metaTex = ResourceDescriptorHeap[historyMetaIdx];
        const float2 meta = metaTex.Load(int3(prevPx, 0));
        if (meta.y > 0.0 && abs(meta.x - viewZ) <= 0.05 * viewZ + 0.02)
        {
            h = histTex.Load(int3(prevPx, 0));
            n = meta.y;
        }
    }
    if (n >= 4.0)
    {
        const float settled = 8.0 * NprLuminance(h.rgb) + 0.05;
        const float lum = NprLuminance(s.rgb);
        if (lum > settled) s.rgb *= settled / lum;
    }
    const float nAmbient = min(n + 1.0, 32.0);
    const float nVisibility = min(n + 1.0, 8.0);
    const float3 ambient = n > 0.0 ? lerp(h.rgb, s.rgb, 1.0 / nAmbient) : s.rgb;
    const float visibility = n > 0.0 ? lerp(h.a, s.a, 1.0 / nVisibility) : s.a;
    histOut[px] = float4(ambient, saturate(visibility));
    metaOut[px] = float2(viewZ, nAmbient);
}

[numthreads(8,8,1)]
void Spatial(uint3 tid : SV_DispatchThreadID)
{
    const uint2 px = tid.xy;
    if (any(px >= uint2(width, height))) return;
    Texture2D<float4>   inTex  = ResourceDescriptorHeap[spatialInIdx];
    RWTexture2D<float4> outTex = ResourceDescriptorHeap[spatialOutUav];
    const float4 c = inTex.Load(int3(px, 0));
    if (c.a < 0.0) { outTex[px] = c; return; }
    Texture2D<float> depthTex = ResourceDescriptorHeap[depthIdx];
    const float viewZ = NprLinearizeReverseZ(depthTex.Load(int3(px, 0)));
    const float zScale = 0.02 * viewZ + 0.01;
    const int2 step = direction == 0u ? int2(2, 0) : int2(0, 2);
    float3 sum = c.rgb;
    float wsum = 1.0;
    [unroll]
    for (int k = -6; k <= 6; ++k)
    {
        if (k == 0) continue;
        const int2 q = int2(px) + step * k;
        if (any(q < 0) || any(q >= int2(width, height))) continue;
        const float4 s = inTex.Load(int3(q, 0));
        if (s.a < 0.0) continue;
        const float zq = NprLinearizeReverseZ(depthTex.Load(int3(q, 0)));
        const float w = exp(-float(k * k) / 18.0) * exp(-abs(zq - viewZ) / zScale);
        sum += s.rgb * w;
        wsum += w;
    }
    outTex[px] = float4(sum / wsum, c.a);
}
