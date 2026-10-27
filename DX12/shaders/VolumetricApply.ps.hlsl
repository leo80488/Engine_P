// Composite of the volumetric fog froxel pipeline: a fullscreen triangle drawn after
// lighting has written HDR.
// Input: the 3D scattering texture (xy = screen UV, z = exponential view-Z slice) and depth.
// Output: (scattering.rgb, 1 - transmittance); with src = ONE / dst = INV_SRC_ALPHA this
// resolves to final = scattering + scene * transmittance.
// Uses the GRAPHICS root signature only (no space2): CB at b3 space0, t5 is the GBuffer
// depth slot, t6 (the skybox env-cube slot) carries the Texture3D scattering SRV here.

#include "DepthCommon.hlsli"

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

Texture2D<float>   gDepth      : register(t5, space0);
Texture3D<float4>  gScattering : register(t6, space0);
#include "sampler_heap.hlsli"   // samplers from the sampler heap
#define gLinear SmpFixed(SMP_LINEAR_CLAMP)

struct PSIn
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(PSIn i) : SV_TARGET
{
    float ndcZ = gDepth.SampleLevel(gLinear, i.uv, 0).r;
    // Sky is the cleared far plane, EXACTLY 0 under reverse-Z. Do not test with an epsilon:
    // ndcZ 1e-4 is ~900 m at near 0.1 / far 10000, so everything past it would lose its fog.
    // Same test as HeightFogApply, DecalApply and SSR*. The whole volume lies in front of the
    // sky: it takes the last slice, the integral to froxelFar, as a surface past it does.
    float w = 1.0;
    if (ndcZ > 0.0)
    {
        float viewZ = LinearizeReverseZ(ndcZ, NearZ, FarZ);
        if (viewZ <= FroxelNear) return float4(0, 0, 0, 0);

        // The slice with its +-half-slice screen-space dither (DepthCommon.hlsli): the same W
        // a forward surface samples through fog_apply.hlsli.
        w = FroxelApplyW(viewZ, i.pos.xy, FroxelNear, FroxelFar, FroxelDepth);
    }

    float4 scat = gScattering.SampleLevel(gLinear, float3(i.uv, w), 0);
    return float4(scat.rgb, 1.0 - scat.a);
}
