#ifndef HYBRID_REFL_ENV_HLSLI
#define HYBRID_REFL_ENV_HLSLI

#include "bindless.hlsli"   // typed bindless handles
// ==== HR_MissEnvRadiance ====
// The shared environment stand-in for a ray with no usable traced result, used by
// both the SW and the HW reflection tier.
//
// Never output black on a miss (AMD SSSR contract). The fold alpha (`relia`) is a
// raw per-pixel channel REBLUR never filters, so a per-ray hit/miss decision
// written into it dithers per pixel and per frame (above mirrorRoughness the ray is
// a per-frame VNDF sample): invisible with TAA, glaring without. The screen<->env
// hand-off must happen in radiance space, where REBLUR integrates it, with relia
// held stable at ~1. Do not write `radiance = 0, conf = 0` on a miss in either
// tier (backface, no sky cube): call this.
//
// Tiers mirror the Lighting.ps fill: coverage-aware parallax probes first, then
// the directional sky / rough-DDGI fallback. The sky is sky light (x iblStrength)
// at every roughness, as in Lighting.ps.
//
// envValidW out: 1 with a sky cube, else the fallback coverage - FlatColor scenes
// with no probes keep relia 0 so Lighting's flat-ambient fill owns the pixel.
//
// Include order: needs DDGISampling / indirect_specular / reflection_probe, so
// include it after them (HybridReflCommon.hlsli comes first in both intersect
// shaders and so cannot host this).

// restirGiVisibility: the caller's ray did not really escape (a screen-trace
// miss, a stand-in for an untrusted proxy or a backface hit), so the receiver's
// ReSTIR GI coverage - the cosine-weighted share of its hemisphere whose rays
// hit geometry - occludes the sky, and its hit radiance is the rough band
// (Lighting.ps does the same with this frame's GI). False for a ray that truly
// missed. restirGiPrevIdx == 0 (no ReSTIR GI last frame, the front-layer
// instance) leaves DDGI's answer.
float3 HR_MissEnvRadiance(float3 P, float3 N, float3 V, float3 R,
                          float roughness, out float envValidW,
                          bool restirGiVisibility)
{
    float skyVisibility = 1.0;
    // The receiver's own pixel in last frame's GI (coverage moves slowly, so
    // no reprojection); a = coverage, 0 = no estimate (DDGI keeps it).
    float4 restirGi = float4(0.0, 0.0, 0.0, 0.0);
    if (restirGiVisibility && restirGiPrevIdx != 0u)
    {
        const float4 clip = mul(float4(P, 1.0), viewProj);
        if (clip.w > 0.0)
        {
            const float2 uv = float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
            if (and(all(uv >= 0.0), all(uv < 1.0)))
            {
                Texture2D<float4> giTex = ResourceDescriptorHeap[restirGiPrevIdx];
                uint giW, giH;
                giTex.GetDimensions(giW, giH);
                restirGi = giTex.Load(int3(int2(uv * float2(giW, giH)), 0));
            }
        }
    }
    const bool giTier = and(restirGi.a > 0.0, all(isfinite(restirGi)));
    float3 ddgiSpecular = float3(0.0, 0.0, 0.0);
    float ddgiSpecularWeight = 0.0;
    // The DDGI radiance sample below is consumed by
    //   roughBlend = smoothstep(0.4, 0.7, roughness) * saturate(ddgiSpecularWeight)
    //   distant    = lerp(skyTerm, ddgiSpecular, roughBlend)
    // which is exactly zero at or below 0.4, so on a sky-cube scene the sample is dead
    // work for smoother reflections.
    //
    // Do not drop the `hasSkyCube == 0u` term: without a sky cube ddgiSpecularWeight
    // is also the coverage signal for `envValidW = saturate(probes.coverage +
    // ddgiSpecularWeight)` below, at every roughness. Gating on roughness alone would
    // report zero coverage on smooth surfaces in None/FlatColor environments and hand
    // those pixels to the flat-ambient fill.
    const bool needDDGISpec = (hasSkyCube == 0u)
                           || (roughness > INDIRECT_SPECULAR_DDGI_ROUGH_START);
    // Shared DDGI arbitration (DDGISampling.hlsli): Lighting.ps also calls
    // DDGI_PickVolumes and DDGI_CombineWinners, so an overlapped pixel resolves the
    // same on screen and in a reflection of itself.
    //
    // DDGI binds here through register arrays (gDDGIIrradiance[vi] et al), not
    // ResourceDescriptorHeap, which is why the arbitration is split into pick/combine
    // and does not own the sampling. The [unroll] over all slots keeps `vi` a
    // compile-time constant for those arrays.
    DDGIVolumePick pick = DDGI_PickVolumes(P, gDDGIVolumes);
    float2 bestSky   = float2(1.0, 0.0);
    float2 secondSky = float2(1.0, 0.0);
    float4 bestSpec   = float4(0.0, 0.0, 0.0, 0.0);
    float4 secondSpec = float4(0.0, 0.0, 0.0, 0.0);

    [unroll] for (uint vi = 0; vi < DDGI_MAX_VOLUMES && ddgiVolumeCount > 0u; ++vi)
    {
        if (!DDGI_IsPicked(pick, vi)) continue;
        DDGIVolumeGPU vol = gDDGIVolumes[vi];
        const bool isBest = (vi == (uint)pick.bestSlot);

        float2 sky = DDGI_SampleSkyVisibility(P, N, V, vol,
                                               gDDGIProbeData[vi]);
        if (isBest) bestSky = sky; else secondSky = sky;

        if (needDDGISpec)
        {
            float4 gi = DDGI_SampleVolume(P, R, V, vol,
                                          gDDGIIrradiance[vi], gDDGIDepth[vi],
                                          gDDGIProbeData[vi], gSampWrap);
            if (isBest) bestSpec = gi; else secondSpec = gi;
        }
    }

    const float2 skyResolved = DDGI_CombineSkyVisibility(bestSky, secondSky, pick);
    if (skyResolved.y > 1e-4)
        skyVisibility = lerp(1.0, skyResolved.x, saturate(skyResolved.y));
    if (giTier) skyVisibility = 1.0 - saturate(restirGi.a);

    const float4 specResolved = DDGI_CombineWinners(bestSpec, secondSpec, pick);
    ddgiSpecularWeight = specResolved.a;
    if (ddgiSpecularWeight > 1e-4)
        ddgiSpecular = specResolved.rgb * ddgiDiffuseScale;

    IndirectSpecularProbeAccum probes = IndirectSpecular_MakeProbeAccum();
    const uint nProbes = min(probeCount, 64u);
    for (uint pi = 0; pi < nProbes && probes.coverage < 0.999; ++pi)
    {
        StructuredBuffer<ReflectionProbe> probeBuf = BindlessSBuf<ReflectionProbe>(probeBufferIdx);
        ReflectionProbe probe = probeBuf[pi];
        float w = ComputeProbeWeight(P, probe);
        if (w <= 1e-3) continue;
        float4 probeSample = SampleProbe(probe, P, R, roughness, gSampClamp);
        IndirectSpecular_AddProbe(probes, probeSample, w, probe.intensity);
    }

    float3 skyTerm = float3(0.0, 0.0, 0.0);
    if (hasSkyCube != 0u)
    {
        const float mipS = roughness * float(max(skyCubeMips, 1u) - 1u);
        skyTerm = gSkyCube.SampleLevel(gSampWrap, R, mipS).rgb
                * iblStrength
                * saturate(skyVisibility)
                * IndirectSpecular_HorizonVisibility(R);
        envValidW = 1.0;
    }
    else
    {
        envValidW = saturate(probes.coverage + ddgiSpecularWeight);
    }
    const float roughBlend = smoothstep(INDIRECT_SPECULAR_DDGI_ROUGH_START,
                                        INDIRECT_SPECULAR_DDGI_ROUGH_END,
                                        roughness)
                           * saturate(ddgiSpecularWeight);
    float3 distant = lerp(skyTerm, ddgiSpecular, roughBlend);
    // ReSTIR GI's rough band: lerp(skyTerm, gi + skyTerm, band).
    if (giTier)
        distant = skyTerm + smoothstep(INDIRECT_SPECULAR_DDGI_ROUGH_START,
                                       INDIRECT_SPECULAR_DDGI_ROUGH_END, roughness)
                          * max(restirGi.rgb, 0.0) * ddgiDiffuseScale;
    // Covers all fallback tiers at once: baked probes and DDGI can carry the
    // sun through openings just like the sky cube (see HR_ClampEnvRadiance).
    return HR_ClampEnvRadiance(
               IndirectSpecular_ResolveProbeFallback(probes, distant));
}

#endif // HYBRID_REFL_ENV_HLSLI
