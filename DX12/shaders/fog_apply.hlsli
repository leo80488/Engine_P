#ifndef FOG_APPLY_HLSLI
#define FOG_APPLY_HLSLI

// fog_apply.hlsli - the fog the opaque scene got, for a surface drawn after the
// fog composites: forward translucency, particles, trails, tracers, BillboardFX,
// world-space video. Without it they composite unfogged over a fogged background.
//
// The opaque pixel ends up as  C = S_v + T_v x (C0 x (1 - a_h) + L_h):
// HeightFogApply's premultiplied (L_h, a_h) first, then VolumetricApply's (S_v, T_v)
// from the froxel scattering volume. A surface in front gets the same two terms at
// its own depth (Fog_Evaluate):
//   transmittance  T = T_v x (1 - a_h)
//   in-scatter     S = S_v + T_v x L_h
// and the pass applies them per its blend state (Fog_Apply*). The math is shared
// with HeightFogApply.ps.hlsl / VolumetricApply.ps.hlsl (height_fog.hlsli,
// DepthCommon.hlsli's FroxelApplyW).
//
// Not reproduced: the raymarched god-ray tier (VolumetricRaymarch, integrated to
// the opaque depth only) and aerial perspective (Lighting.ps only).
//
// Binding: every pass drawing a shader that includes this calls FogApply::Bind
// (Graphics/FogApply.h): b4 space0 FogApplyCB (the RG slot FogApply, written per
// frame). The froxel sampler is the SMP_LINEAR_CLAMP heap slot. The volume and the
// opaque depth snapshot are descriptor-heap indices, 0 = absent. gFogFlags == 0
// makes every function here a no-op (fog off, a debug view mode, a preview graph,
// -fwdfog_off).

#include "DepthCommon.hlsli"   // WorldPosFromDepth, LinearizeReverseZ, FroxelApplyW, SoftParticleFade
#include "height_fog.hlsli"    // HeightFog_AtSurface

#define FOG_APPLY_HEIGHT 1u    // HeightFogApply ran this frame
#define FOG_APPLY_FROXEL 2u    // VolumetricApply ran this frame

// Mirror: Graphics/FogApply.h (FogApply::Constants, 192 B), checked against
// this declaration by the layout specs (Renderer/LayoutSpecs.cpp).
cbuffer FogApplyCB : register(b4, space0)
{
    float4x4 gFogInvViewProj;      // the frame's jittered inverse view-projection (LightCB.invViewProj)
    float3   gFogCameraPos;        uint  gFogFlags;
    float3   gFogDirToSun;         float gFogNearZ;          // dirToSun = -LightCB.lightDir
    float3   gFogSunColor;         float gFogFarZ;
    float2   gFogInvRenderSize;    uint  gFogFroxelIdx;      uint gFogOpaqueDepthIdx;
    // HeightFogPass's constants, clamped as its Execute clamps them
    float    gFogHfDensity;        float gFogHfFalloff;      float gFogHfHeight;    float gFogHfStart;
    float3   gFogHfColor;          float gFogHfMaxOpacity;
    float    gFogHfSunInscatter;   float gFogHfAnisotropy;   float gFogFroxelNear;  float gFogFroxelFar;
    uint     gFogFroxelDepth;      float gFogSoftFadeRange;  float2 _gFogPad;
};

#include "sampler_heap.hlsli"   // samplers from the sampler heap
#define gFogLinearClamp SmpFixed(SMP_LINEAR_CLAMP)

struct FogSample
{
    float3 inscatter;       // radiance the fog adds in front of the surface
    float  transmittance;   // fraction of the surface's own radiance that arrives
};

// The fog between the camera and the fragment at `svPos` (SV_Position: pixel
// centre in render-target pixels, z the reverse-Z device depth).
FogSample Fog_Evaluate(float4 svPos)
{
    FogSample f;
    f.inscatter     = float3(0.0, 0.0, 0.0);
    f.transmittance = 1.0;
    [branch] if (gFogFlags == 0u) return f;

    const float2 uv = svPos.xy * gFogInvRenderSize;

    [branch] if (gFogFlags & FOG_APPLY_HEIGHT)
    {
        HeightFogParams hf;
        hf.density       = gFogHfDensity;
        hf.heightFalloff = gFogHfFalloff;
        hf.height        = gFogHfHeight;
        hf.startDistance = gFogHfStart;
        hf.color         = gFogHfColor;
        hf.maxOpacity    = gFogHfMaxOpacity;
        hf.sunInscatter  = gFogHfSunInscatter;
        hf.anisotropy    = gFogHfAnisotropy;
        const float3 wp = WorldPosFromDepth(uv, svPos.z, gFogInvViewProj);
        const float4 h  = HeightFog_AtSurface(hf, gFogCameraPos, wp, gFogDirToSun, gFogSunColor);
        f.inscatter     = h.rgb;
        f.transmittance = 1.0 - h.a;
    }

    [branch] if (gFogFlags & FOG_APPLY_FROXEL)
    {
        // Planar view depth, as VolumetricApply looks it up (the grid itself is
        // radial; copied as-is so a surface matches the opaque at equal depth).
        const float viewZ = LinearizeReverseZ(svPos.z, gFogNearZ, gFogFarZ);
        if (viewZ > gFogFroxelNear)
        {
            Texture3D<float4> volume = ResourceDescriptorHeap[gFogFroxelIdx];
            const float  w    = FroxelApplyW(viewZ, svPos.xy, gFogFroxelNear, gFogFroxelFar, gFogFroxelDepth);
            const float4 scat = volume.SampleLevel(gFogLinearClamp, float3(uv, w), 0);
            // Composited over the height fog, as VolumetricApply over HeightFogApply.
            f.inscatter     = scat.rgb + f.inscatter * scat.a;
            f.transmittance = f.transmittance * scat.a;
        }
    }
    return f;
}

// ==== Applying it per blend state ====
// Each takes the pixel shader's output as the blend state will consume it.
//
// Straight alpha (SRC_ALPHA, INV_SRC_ALPHA): the surface's radiance is fogged;
// the blend weighs both terms by its coverage.
float4 Fog_ApplyAlpha(float4 c, FogSample f)
{
    return float4(c.rgb * f.transmittance + f.inscatter, c.a);
}
// Premultiplied (ONE, INV_SRC_ALPHA): rgb already carries the coverage, so the
// in-scatter is weighed by it here. Additive-as-alpha-0 (trails) falls out.
float4 Fog_ApplyPremultiplied(float4 c, FogSample f)
{
    return float4(c.rgb * f.transmittance + f.inscatter * c.a, c.a);
}
// Additive (ONE, ONE or SRC_ALPHA, ONE): added light is only attenuated - the
// fog in front is already in the background it is added to.
float4 Fog_ApplyAdditive(float4 c, FogSample f)
{
    return float4(c.rgb * f.transmittance, c.a);
}
// Multiply (DEST_COLOR, INV_SRC_ALPHA): dst' = dst x (rgb + 1 - a). Scaling
// both terms by T makes the factor lerp(1, rgb + 1 - a, T): the layer fades
// out with the fog in front of it, premultiplied or not.
float4 Fog_ApplyMultiply(float4 c, FogSample f)
{
    return float4(c.rgb * f.transmittance, c.a * f.transmittance);
}

// ==== Soft fade against the opaque scene ====
// 0 where the fragment touches the opaque surface behind it, 1 at
// gFogSoftFadeRange metres in front of it (DepthCommon's SoftParticleFade). The
// depth is the opaque snapshot (SSRTraceDepth: opaque + terrain, no grass, no
// water); a pass reading it declares the read. 1 when there is none or the range
// is 0.
float Fog_SoftFade(float4 svPos)
{
    [branch] if (gFogOpaqueDepthIdx == 0u || gFogSoftFadeRange <= 0.0) return 1.0;
    Texture2D<float> opaque = ResourceDescriptorHeap[gFogOpaqueDepthIdx];
    const float sceneZ = opaque.Load(int3(int2(svPos.xy), 0));
    return SoftParticleFade(svPos.z, sceneZ, gFogNearZ, gFogFarZ, gFogSoftFadeRange);
}

#endif // FOG_APPLY_HLSLI
