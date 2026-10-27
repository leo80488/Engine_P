// Temporal reprojection of the half-res volumetric raymarch; FroxelTemporal.cs.hlsl's
// structure in 2D screen space.
//   Current (t0) - this frame's VolumetricRaymarch output
//   History (t1) - last frame's blended result
//   Output  (u0) - this frame's blended result (next frame's history)
// .a of all three is the texel's depth key (vol_raymarch_common.hlsli), never blended:
// the output carries this frame's.
// The anchor is the world position of the surface the texel marched to (same pixel and
// depth as the raymarch, VolRmMarchPixel), pushed through prevViewProj; a sky texel's is its
// direction (a point at infinity). History is fetched bilaterally and clamped to same-surface
// neighbours, so a silhouette never mixes the object's and the background's in-scatter; then
// lerp(history, current, temporalAlpha).

#include "FroxelCommon.hlsli"
#include "vol_raymarch_common.hlsli"

cbuffer FroxelCB : register(b0, space2)
{
    FroxelParams P;
};

Texture2D<float4>   CurrentSRV  : register(t0, space2);
Texture2D<float4>   HistorySRV  : register(t1, space2);
Texture2D<float>    SceneDepth  : register(t3, space2);
SamplerState        LinearSampler : register(s0, space2);

RWTexture2D<float4> OutputUAV   : register(u0, space2);

// Full-res uv + scene-depth ndcZ -> world position (invViewProj, as in
// VolumetricRaymarch's BuildViewRay).
#include "core/depth.hlsli"   // LinearizeReverseZ, WorldPosFromDepth
float3 ReconstructWorldPos(float2 uv, float ndcZ)
{
    return WorldPosFromDepth(uv, ndcZ, P.invViewProj);
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint halfW, halfH;
    OutputUAV.GetDimensions(halfW, halfH);
    if (DTid.x >= halfW || DTid.y >= halfH) return;

    float4 current = CurrentSRV[DTid.xy];
    const float key = current.a;

    // The pixel and depth the raymarch marched for this texel.
    float ndcZ;
    const int2 px = VolRmMarchPixel(SceneDepth, DTid.xy, ndcZ);
    uint fullW, fullH;
    SceneDepth.GetDimensions(fullW, fullH);
    float2 uv = (float2(px) + 0.5) / float2(fullW, fullH);

    // Sky / empty: the ray's direction, w 0; its key is the far end of the fog, as the march's.
    const bool sky = ndcZ <= 1e-6;
    float4 prevClip;
    if (sky)
    {
        const float4 pH = mul(float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.5, 1.0), P.invViewProj);
        prevClip = mul(float4(normalize(pH.xyz / pH.w - P.cameraPos), 0.0), P.prevViewProj);
    }
    else
        prevClip = mul(float4(ReconstructWorldPos(uv, ndcZ), 1.0), P.prevViewProj);
    if (prevClip.w <= 0.0)
    {
        OutputUAV[DTid.xy] = current;
        return;
    }
    float2 prevUV = float2( prevClip.x / prevClip.w * 0.5 + 0.5,
                           -prevClip.y / prevClip.w * 0.5 + 0.5);
    if (any(prevUV < 0.0) || any(prevUV > 1.0))
    {
        OutputUAV[DTid.xy] = current;
        return;
    }

    // Bilateral history fetch. History .a is each texel's key in LAST frame's view, so it
    // is compared with this surface's previous view-Z (prevClip.w) and camera motion alone
    // does not reject it. A texel of another surface, or never-written history (key 0), gets
    // ~no weight; no weight left -> no usable history.
    const float prevKey = sky ? P.froxelFar : min(prevClip.w, P.froxelFar);
    int2 dim = int2(halfW, halfH) - 1;
    // Where this texel was last frame: its own position moved by the anchor's motion. Not the
    // anchor's previous UV itself: the anchor is one pixel of the 2x2 block, a quarter texel off
    // the texel's centre, and a still camera would blend that much of a neighbour in every frame,
    // smearing the shafts by texels toward the anchor's side.
    float2 hp = (prevUV - uv) * float2(halfW, halfH) + float2(DTid.xy);
    float2 hb = floor(hp);
    float2 hf = hp - hb;
    float3 hSum = 0.0;
    float  hW   = 0.0;
    [unroll] for (int k = 0; k < 4; ++k)
    {
        const int2   o  = int2(k & 1, k >> 1);
        const int2   hc = clamp(int2(hb) + o, int2(0, 0), dim);
        const float4 h  = HistorySRV.Load(int3(hc, 0));
        const float  wb = (o.x ? hf.x : 1.0 - hf.x) * (o.y ? hf.y : 1.0 - hf.y);
        const float  w  = wb * VolRmKeyWeight(h.a, prevKey);
        hSum += h.rgb * w;
        hW   += w;
    }
    if (hW < 1e-4)
    {
        OutputUAV[DTid.xy] = current;
        return;
    }
    float3 history = hSum / hW;

    // 3x3 neighbourhood clamp over the neighbours that marched to a similar surface, so an
    // edge texel's box is not widened by the other side and disocclusions reject history.
    float3 nMin = current.rgb, nMax = current.rgb;
    [unroll] for (int dy = -1; dy <= 1; ++dy)
    [unroll] for (int dx = -1; dx <= 1; ++dx)
    {
        if (dx == 0 && dy == 0) continue;
        int2   nc = clamp(int2(DTid.xy) + int2(dx, dy), int2(0, 0), dim);
        float4 n  = CurrentSRV[nc];
        if (VolRmKeyWeight(n.a, key) < 0.5) continue;
        nMin = min(nMin, n.rgb);
        nMax = max(nMax, n.rgb);
    }
    history = clamp(history, nMin, nMax);

    // temporalAlpha is the same CB field the froxel temporal pass uses. The key is this frame's.
    OutputUAV[DTid.xy] = float4(lerp(history, current.rgb, P.temporalAlpha), key);
}
