// Fullscreen composite of the per-pixel volumetric raymarch, AFTER the froxel
// VolumetricApply. Additive blend (src = ONE, dst = ONE) layers the sun + shadowed-spot
// in-scatter onto HDR: the raymarch attenuated each step's light from the camera already,
// so nothing after it may attenuate it again. Writes alpha 0.
// Input: the half-res temporal result (.a = the depth key each texel marched to,
// vol_raymarch_common.hlsli) and depth (each pixel's own key).
// Depth-aware upsample (the cloud composite's scheme): each pixel weighs the 4 half-res
// texels around it by bilinear weight x how well the texel's surface matches its own. A
// plain bilinear upsample smears in-scatter across silhouettes in a half-res staircase
// that TAA / the upscaler cannot resolve.

#include "vol_raymarch_common.hlsli"

cbuffer VolApplyCB : register(b3, space0)
{
    float NearZ;
    float FarZ;
    float FroxelNear;
    float FroxelFar;
    uint  FroxelDepth;
    uint  _pad0;
    uint  _pad1;
    uint  _pad2;
};

Texture2D<float>   gDepth     : register(t5, space0);
Texture2D<float4>  gRaymarch  : register(t6, space0);  // half-res; bilateral upsample here
#include "sampler_heap.hlsli"   // samplers from the sampler heap
#define gLinear SmpFixed(SMP_LINEAR_CLAMP)

struct PSIn
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(PSIn i) : SV_TARGET
{
    const int2  pix = int2(i.pos.xy);
    const float key = VolRmKey(gDepth.Load(int3(pix, 0)), NearZ, FarZ, FroxelFar);

    uint halfW, halfH;
    gRaymarch.GetDimensions(halfW, halfH);
    const int2 dim = int2(halfW, halfH) - 1;

    // This pixel's centre in half-res texel space (texel (x, y) covers the
    // full-res block 2x..2x+1, 2y..2y+1).
    const float2 hp = (float2(pix) + 0.5) * 0.5 - 0.5;
    const float2 hb = floor(hp);
    const float2 hf = hp - hb;

    float3 sum        = 0.0;
    float  wSum       = 0.0;
    float3 nearest    = 0.0;
    float  nearestW   = -1.0;
    [unroll] for (int k = 0; k < 4; ++k)
    {
        const int2   o  = int2(k & 1, k >> 1);
        const int2   tc = clamp(int2(hb) + o, int2(0, 0), dim);
        const float4 rm = gRaymarch.Load(int3(tc, 0));
        const float  wd = VolRmKeyWeight(rm.a, key);
        const float  w  = (o.x ? hf.x : 1.0 - hf.x) * (o.y ? hf.y : 1.0 - hf.y) * wd;
        sum  += rm.rgb * w;
        wSum += w;
        if (wd > nearestW) { nearestW = wd; nearest = rm.rgb; }
    }

    // Every tap marched to another surface (a sub-texel sliver): take the closest match.
    // Alpha stays 0: the HDR alpha is not this pass's.
    const float3 rgb = (wSum < 1.0e-4) ? nearest : sum / wSum;
    return float4(rgb, 0.0);
}
