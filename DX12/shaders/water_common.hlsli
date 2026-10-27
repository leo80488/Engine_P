// water_common.hlsli - the engine-side service layer for WATER-domain materials.
//
// A water material writes one hook, like any other custom material:
//
//     #define MATERIAL_HAS_SURFACE
//     #include "custom_material.hlsli"
//     void EvaluateMaterial(MaterialPixelParams p, inout SurfaceData s) { ... }
//
// and calls the Water* helpers below for what a water surface needs: the screen UV, the scene
// depth and world position behind the surface, the refraction source, Fresnel, clustered
// local-light specular and the interactive ripple slope. The sky reflection and the sun glint come
// from the shared forward BRDF.
//
// Included unconditionally: only the WATER_SURFACE variant has the real implementations, but the
// hook also compiles in the deferred GBuffer variant (WATER_SURFACE is 0 there; the material preview
// draws water there, and a STANDARD material graph with SceneDepth / SceneColor / Fresnel nodes
// calls these helpers too), the vertex stage and the shadow variants (custom_material.hlsli lists
// them). So: real implementations under MS_IS_WATER, neutral stubs otherwise. Never wrap the
// declarations themselves in the macro: those variants would fail to compile, land in the negative
// cache and draw nothing. A water material declares `#define MATERIAL_DOMAIN_WATER`; the domain is
// recorded and shown, not enforced, so the stubs stay.
//
// Include this AFTER custom_surface.hlsli, whose MS_IS_FORWARD block declares LightCB (b2),
// gRadiance (t7), gIBLSampler (a fixed sampler-heap slot) and pulls in indirect_specular.hlsli;
// custom_material.hlsli does that for you.
#ifndef WATER_COMMON_HLSLI
#define WATER_COMMON_HLSLI

#include "bindless.hlsli"   // typed bindless handles

// `defined()` guard, not a bare `#if MS_IS_WATER`: in the VERTEX stage
// custom_surface.hlsli is never included, so the macro does not exist at all.
#if defined(MS_IS_WATER) && MS_IS_WATER

// CSM (gShadowCascades t9, and gShadowSampler, a fixed comparison sampler-heap slot) is declared
// by custom_surface.hlsli's MS_IS_FORWARD block for every forward permutation (WATER_SURFACE
// implies MS_IS_FORWARD). Do not re-declare it here: a second register(t9) in one TU is a
// redefinition error.

// ==== Clustered local lights ====
// Point / spot lights for WaterLocalLights. custom_surface.hlsli declares these buffers only when
// !MS_IS_WATER, so water declares them here: the registers Lighting.ps uses (t13/t14/t15, the
// shared graphics root signature's Gfx::ClusterLights / ClusterIndexList / ClusterGrid, bound by
// WaterSurfacePass). clusterNearZ / clusterFarZ / clusterLightCount / viewMatrix live in the
// LightCB this pass binds at b2.
#include "cluster_common.hlsli"
#include "light_eval.hlsli"   // the single punctual-falloff authority
StructuredBuffer<GPULight>       gLights         : register(t13, space0);
StructuredBuffer<uint>           gLightIndexList : register(t14, space0);
StructuredBuffer<LightGridEntry> gLightGrid      : register(t15, space0);
// Spot / point shadows: the spot views at t22, bound by WaterSurfacePass (or its "no atlas"
// fallback); LightCB and gShadowSampler come from custom_surface.hlsli.
#include "punctual_shadow.hlsli"

// ==== Screen-space addressing ====
// gWaterGlobals.xy = refraction-source / scene-depth bindless indices (0 = unbound),
//              .z  = ripple body count.
// gWaterRTSize.xy  = the size the frame is rasterised at (not the display size: they differ
//                    whenever the frame is upscaled).
float2 WaterScreenUV(MaterialPixelParams p)
{
    const float2 rtSize = max(gWaterRTSize.xy, float2(1.0, 1.0));
    return p.svPosition.xy / rtSize;
}

// World position of the opaque pixel behind this water pixel, reconstructed
// through LightCB's invViewProj. Returns false when there is nothing behind.
bool WaterSceneWorldPosBehind(MaterialPixelParams p, out float3 worldPos)
{
    worldPos = float3(0, 0, 0);
    if (gWaterGlobals.y == 0u) return false;        // depth not bound this frame

    Texture2D<float> sceneDepth = ResourceDescriptorHeap[gWaterGlobals.y];
    const float sceneZ = sceneDepth.Load(int3(int2(p.svPosition.xy), 0)).r;

    // Reverse-Z: the sky writes depth EXACTLY 0. Test <= 0.0, never an epsilon.
    if (sceneZ <= 0.0) return false;

    const float2 uv  = WaterScreenUV(p);
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    const float4 h   = mul(float4(ndc, sceneZ, 1.0), invViewProj);
    worldPos = h.xyz / h.w;
    return true;
}

// Metres of water between the surface and the opaque geometry behind it, measured VERTICALLY
// (surface Y minus opaque Y), which is what an absorption / shoreline curve wants. Returns 1e6
// where nothing is behind, so absorption saturates to the deep colour and the shoreline stays
// opaque instead of fading to sky.
float WaterSceneDepthBelow(MaterialPixelParams p)
{
    float3 behind;
    if (!WaterSceneWorldPosBehind(p, behind)) return 1e6;
    return max(p.worldPos.y - behind.y, 0.0);
}

// The lit opaque scene at uv, for refraction. Returns fallback where the source is unbound, uv is
// off screen or the sample lands on sky.
float3 WaterSceneColor(MaterialPixelParams p, float2 uv, float3 fallback)
{
    if (gWaterGlobals.x == 0u) return fallback;
    if (any(uv != saturate(uv)))  return fallback;

    // Reject sky through the depth snapshot: the refraction source is a post-lighting OPAQUE snapshot,
    // black wherever sky shows, so sampling it there would paint black bands at grazing angles.
    if (gWaterGlobals.y != 0u)
    {
        Texture2D<float> sceneDepth = ResourceDescriptorHeap[gWaterGlobals.y];
        const float2 rtSize = max(gWaterRTSize.xy, float2(1.0, 1.0));
        const float  z = sceneDepth.Load(int3(int2(uv * rtSize), 0)).r;
        if (z <= 0.0) return fallback;
    }

    Texture2D<float4> sceneColor = ResourceDescriptorHeap[gWaterGlobals.x];
    return sceneColor.SampleLevel(gIBLSampler, uv, 0).rgb;
}

// ==== Shading services ====
// Schlick Fresnel for a scalar f0 against the material normal.
float WaterFresnel(MaterialPixelParams p, SurfaceData s, float f0)
{
    const float NdotV = saturate(dot(normalize(s.normalWS), p.viewDirWS));
    return f0 + (1.0 - f0) * pow(1.0 - NdotV, 5.0);
}

// Clustered point / spot lights on the water, SPECULAR ONLY: a lamp adds glints, it does not wash
// the surface out like a Lambert floor. Uses the deferred pass's per-light functions
// (surface_eval.hlsli: PunctualLightReach for the falloff and early-outs, EvalPunctualSurface for
// the BRDF) and each light's spot / point shadow, so a lamp and the sun agree on the lobe. The
// falloff is light_eval.hlsli's, with the max(d^2, 0.01) floor every other surface has.
// There is no WaterSkyReflection / WaterSunGlint: the shared forward path's specular lobe against
// gRadiance and the sun is the reflection and the glint, so a hook gets both by filling
// normalWS / roughness / reflectance.
float3 WaterLocalLights(MaterialPixelParams p, SurfaceData s)
{
    if (clusterLightCount == 0) return float3(0, 0, 0);

    const float3 N = normalize(s.normalWS);
    const float3 V = p.viewDirWS;
    const float  roughness = max(s.roughness, 0.045);
    const float  f0Scalar  = 0.16 * s.reflectance * s.reflectance;
    const float3 F0        = float3(f0Scalar, f0Scalar, f0Scalar);   // water is never metallic
    const SurfaceShading sm = InitSurfaceShading();   // Standard: no transmission, no wrap
    const bool   receiveShadow =
        (g_Materials[p.materialIndex].materialFlags & MAT_FLAG_DISABLE_RECEIVE_SHADOW) == 0u;
    // Geometric receiver derivatives, BEFORE the divergent loop (SampleSpotShadow's
    // depth-gradient fit; Lighting.ps and the forward loop do the same).
    const float3 worldDx = ddx(p.worldPos);
    const float3 worldDy = ddy(p.worldPos);

    const float  viewZ = abs(mul(float4(p.worldPos, 1.0), viewMatrix).z);
    const uint3  coord = GetClusterCoord(WaterScreenUV(p), viewZ,
                                         clusterNearZ, clusterFarZ);
    const uint   key    = ClusterIndex(coord);
    const uint   offset = gLightGrid[key].offset;
    const uint   count  = gLightGrid[key].count;

    float3 acc = float3(0, 0, 0);
    for (uint li = 0; li < count; ++li)
    {
        uint lightIdx = li;
        if (offset != 0xFFFFFFFFu) lightIdx = gLightIndexList[offset + li];
        const GPULight light = gLights[lightIdx];
        // The sun (type 0) is the shared path's job; out of range, outside the
        // cone or facing away adds nothing.
        PunctualReach r;
        if (!PunctualLightReach(light, p.worldPos, N, sm, r)) continue;
        float lightVis = 1.0;
        if (receiveShadow && light.type == 2u && light.shadowSliceIdx != 0xFFFFFFFFu)
            lightVis = SampleSpotShadow(p.worldPos, worldDx, worldDy, N, light.position,
                                        light.spotAngle, light.shadowSliceIdx);
        else if (receiveShadow && light.type == 1u && light.shadowSliceIdx != 0xFFFFFFFFu)
            lightVis = SamplePointShadow(p.worldPos, N, light.position, light.radius,
                                         light.shadowSliceIdx);
        if (!(lightVis > 0.0)) continue;
        const float3 radiance = light.color * light.intensity * (r.atten * r.spotFactor);
        DirectLighting d;
        float3 specL, kDl;
        EvalPunctualSurface(r, radiance, N, V, F0, float3(0, 0, 0), roughness, 0.0, 1.0,
                            sm, d, specL, kDl);
        acc += d.specular * lightVis;     // specular only: the diffuse half is not water's
    }
    return acc;
}

// ==== Interactive ripples ====
// Tangent-space XZ slope from the WaterRippleSystem height field, scaled by strength; 0 where this
// pixel is outside every simulated body.
// Bodies are found by WORLD XZ CONTAINMENT in gWaterBodies, not by a per-draw identity: DrawPacket
// carries no entity, and two lakes sharing one .imat merge into one packet. Mirrors the CPU side,
// WaterRippleSystem::GatherWorldImpulse.
float2 WaterRippleSlope(MaterialPixelParams p, float strength)
{
    const uint count = min(gWaterGlobals.z, WATER_MAX_RIPPLE_BODIES);
    for (uint b = 0; b < count; ++b)
    {
        const float4 win = gWaterBodies[b * 2u + 1u];   // xy origin, z size, w texIdx
        const uint   texIdx = asuint(win.w);
        // texIdx is a handle into the bindless TABLE: "no field" is the Tex2D null (not BindlessNone,
        // since table index 0 is texture handle_id 0), and 0 is what a body nobody wrote holds.
        if (texIdx == 0u || texIdx == BINDLESS_NULL_TEX2D_FLOAT) continue;

        const float2 ruv = (p.worldPos.xz - win.xy) / max(win.z, 1e-3);
        if (any(ruv != saturate(ruv))) continue;

        Texture2D<float2> field = BindlessTex2D<float2>(texIdx);
        // Central-difference the height field. The texel step comes from the
        // field's own dimensions so the gradient is resolution-independent.
        uint fw, fh;
        field.GetDimensions(fw, fh);
        const float e = 1.0 / float(max(fw, 1u));

        const float hL = field.SampleLevel(gIBLSampler, ruv - float2(e, 0), 0).r;
        const float hR = field.SampleLevel(gIBLSampler, ruv + float2(e, 0), 0).r;
        const float hD = field.SampleLevel(gIBLSampler, ruv - float2(0, e), 0).r;
        const float hU = field.SampleLevel(gIBLSampler, ruv + float2(0, e), 0).r;

        // Scale by the knob only, never divide by body size: that makes ripples vanish on large bodies
        // and explode on small ones.
        return -float2(hR - hL, hU - hD) * strength;
    }
    return float2(0.0, 0.0);
}

#else  // !MS_IS_WATER: neutral stubs, so a water hook compiles in every other variant
float2 WaterScreenUV(MaterialPixelParams p)                       { return float2(0.5, 0.5); }
bool   WaterSceneWorldPosBehind(MaterialPixelParams p, out float3 wp) { wp = float3(0, 0, 0); return false; }
float  WaterSceneDepthBelow(MaterialPixelParams p)                { return 1e6; }
float3 WaterSceneColor(MaterialPixelParams p, float2 uv, float3 fb){ return fb; }
float  WaterFresnel(MaterialPixelParams p, SurfaceData s, float f0){ return f0; }
float3 WaterLocalLights(MaterialPixelParams p, SurfaceData s)      { return float3(0, 0, 0); }
float2 WaterRippleSlope(MaterialPixelParams p, float strength)     { return float2(0, 0); }

#endif // MS_IS_WATER

#endif // WATER_COMMON_HLSLI
