// NRD output -> the traced-reflection texture Lighting.ps opens bindlessly through
// LightCB.rtReflectionIdx. Unpacks OUT_SPEC_RADIANCE_HITDIST (REBLUR_SPECULAR) and
// writes the (rgb = reflected radiance, a = rtConf) contract Lighting.ps's 3-tier
// composite consumes.
//
// Confidence model: the hybrid trace covers every eligible pixel (roughness <= the
// trace gate) each frame and misses carry sky-cube radiance, so the denoised
// radiance is a complete reflection estimate: conf = the trace reliability on
// eligible pixels, feathered to 0 across the last 10% of the roughness band so the
// probe/sky tier takes over without a seam.

#include "HybridReflCommon.hlsli"

#define NRD_NORMAL_ENCODING    2
#define NRD_ROUGHNESS_ENCODING 1
#include "NRD.hlsli"

Texture2D<float>    gDepth      : register(t2,  space0);
Texture2D<float4>   gSurface    : register(t4,  space0);
Texture2D<float4>   gRawColor   : register(t28, space0);  // .a = trace reliability (see intersect NRD contract)
Texture2D<float4>   gNrdSpecOut : register(t30, space0);

RWTexture2D<float4> OutDenoised : register(u7,  space0);

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= screenW || DTid.y >= screenH) return;
    const uint2 pixel = DTid.xy;

    const float depth = gDepth.Load(int3(pixel, 0));
    if (depth <= 0.0)
    {
        OutDenoised[pixel] = 0;   // sky: rtConf 0, probe/sky tier untouched
        return;
    }

    const float roughness = gSurface.Load(int3(pixel, 0)).r;

    // Soft handoff to the probe/sky tier over the top 10% of the traced band, scaled
    // by the trace's per-pixel reliability (rawColor.a: 1 = front-face hit or
    // sky-covered miss; 0 = untraced / backface / no-sky miss, pixels that must keep
    // their probe/flat-ambient specular).
    const float4 raw  = gRawColor.Load(int3(pixel, 0));
    const float band  = max(roughnessThreshold, 1e-4);
    const float relia = saturate(raw.a);
    const float conf  = (1.0 - smoothstep(band * 0.9, band, roughness)) * relia;
    if (conf <= 0.0)
    {
        OutDenoised[pixel] = 0;
        return;
    }

    // Denoiser bypass (unifiedFlags bit 2): REBLUR did not run, gNrdSpecOut holds
    // nothing, and the raw single-sample trace is the output. Set for the translucent
    // front-layer instance (NRDPack skipped too; glass is at or near the mirror band -
    // deterministic reflect(), no GGX variance - and covers a small part of the frame,
    // so TAA absorbs the rest) and in ray-reconstruction mode (DLSS RR denoises the
    // raw trace downstream).
    if ((unifiedFlags & 4u) != 0u)
    {
        OutDenoised[pixel] = float4(raw.rgb, conf);
        return;
    }

    const float4 packed   = gNrdSpecOut.Load(int3(pixel, 0));
    const float4 unpacked = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(packed);

    // HR_SampleReflectionDir is deterministic through the mirror band, so the
    // full-res raw trace has no GGX sampling variance there, and REBLUR only hurts: a
    // reflected skinned hit has no primary-surface motion vector, and the short-history
    // spatial reconstruction turns the current pose into a wide soft blob even at
    // roughness 0. Keep the current-frame trace for a true mirror and hand glossy
    // pixels back to NRD over the same 1.0x -> 1.5x transition as the screen-radiance
    // cone-mip gate (no hard material seam). Rough reflections stay fully denoised.
    const float mirrorBand = max(mirrorRoughness, 1e-4);
    const float nrdWeight  = smoothstep(mirrorBand, mirrorBand * 1.5, roughness);
    const float3 radiance  = lerp(raw.rgb, unpacked.rgb, nrdWeight);

    OutDenoised[pixel] = float4(radiance, conf);
}
