// HeightFogApply.ps.hlsl - UE-style analytic exponential height fog.
// Fullscreen triangle drawn AFTER CloudPass (HDR already holds lit geometry, aerial
// perspective, skybox, water and clouds) and BEFORE VideoPass / the froxel
// VolumetricFogPass (near volumetric detail composes OVER this far fog).
// Blend ONE / INV_SRC_ALPHA: out = inscatter x opacity + scene x (1 - opacity).
//
// The math lives in height_fog.hlsli: surfaces drawn after this pass (forward
// translucency, particles, trails, ...) evaluate the same functions at their own depth
// through fog_apply.hlsli.
//
// Graphics root signature only: LightCB at b1 (sun dir/color, cameraPos, invViewProj),
// HeightFogCB at b2, depth at t5 space0.

#define LIGHT_CB_REGISTER b1
#include "light_cb.hlsli"      // lightDir (FROM-light), lightColor, cameraPos, invViewProj
#include "DepthCommon.hlsli"   // WorldPosFromDepth, ViewRayFromUV
#include "height_fog.hlsli"    // HeightFog_* (no register bindings)

cbuffer HeightFogCB : register(b2, space0)
{
    float  FogDensity;          // d0: extinction at h == FogHeight (1/m)
    float  FogHeightFalloff;    // k (1/m)
    float  FogHeight;           // H, world metres
    float  StartDistance;       // metres of fog-free range in front of the camera

    float3 FogColor;            // ambient inscatter (linear HDR)
    float  MaxOpacity;          // UE FogMaxOpacity (0..1)

    float  SunInscatterIntensity; // 0 disables the sun term
    float  Anisotropy;            // HG g toward the sun
    float  _hfPad0;
    float  _hfPad1;
};

Texture2D<float> gDepth  : register(t5, space0);
#include "sampler_heap.hlsli"   // samplers from the sampler heap
#define gLinear SmpFixed(SMP_LINEAR_CLAMP)

struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 main(PSIn i) : SV_TARGET
{
    if (FogDensity <= 0.0) return float4(0, 0, 0, 0);

    HeightFogParams f;
    f.density       = FogDensity;
    f.heightFalloff = FogHeightFalloff;
    f.height        = FogHeight;
    f.startDistance = StartDistance;
    f.color         = FogColor;
    f.maxOpacity    = MaxOpacity;
    f.sunInscatter  = SunInscatterIntensity;
    f.anisotropy    = Anisotropy;

    // Load, not bilinear: depth interpolated across a geometry/sky edge gives phantom
    // mid-range distances and a 1-px fog halo at silhouettes.
    float ndcZ = gDepth.Load(int3(int2(i.pos.xy), 0));

    float3 dirToSun = -lightDir;   // LightCB lightDir is FROM-light

    // Sky is the cleared far plane, EXACTLY 0 in reversed-Z. Do not test against an epsilon:
    // ndcZ 1e-4 is ~900 m at near 0.1 / far 10000, so far geometry and water would take the
    // sky branch, whose grazing rays force tau = 1e9 -> MaxOpacity at any FogDensity (a sharp
    // horizon fog seam that also washes out SSR). Every depth-consuming pass (DecalApply,
    // SSRTrace/Composite/Upsample) treats only ndcZ <= 0 as sky.
    float3 rayDir;
    float  tau;
    if (ndcZ <= 0.0)      // sky (reversed-Z: far = 0) - fog the backdrop too
    {
        rayDir = ViewRayFromUV(i.uv, invViewProj);
        tau    = HeightFog_TauToSky(f, cameraPos, rayDir);
    }
    else
    {
        // HeightFog_AtSurface spelled out, so both branches share the resolve below (one copy
        // in the bytecode).
        float3 wp = WorldPosFromDepth(i.uv, ndcZ, invViewProj);
        float3 d  = wp - cameraPos;
        float dist = max(length(d), 1e-4);
        rayDir = d / dist;
        if (!HeightFog_TauToSurface(f, cameraPos, rayDir, dist, tau))
            return float4(0, 0, 0, 0);
    }

    return HeightFog_Resolve(f, tau, rayDir, dirToSun, lightColor);
}
