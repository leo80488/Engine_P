// SSGIResolve.cs.hlsl - REBLUR output + filtered coverage -> the SSGI texture
// Lighting.ps folds through LightCB.ssgiResultIdx.
//
// Output contract (RGBA16F):
//   .rgb = denoised near-field irradiance, same units as
//          DDGI_SampleVolume(...).rgb (E/pi), before ddgiDiffuseScale
//   .a   = resolvedFraction, bilaterally filtered
// Lighting.ps closes it with `ssgi.rgb * ddgiDiffuseScale + farDiffuse * (1-a)`.
//
// ==== Why the coverage is filtered here and not left to NRD ====
// REBLUR denoises the radiance (rgb) and normalised hit distance (a) of its own
// signal. It never sees, and never filters, resolvedFraction.
//
// resolvedFraction is SUM c_i/N with continuous per-ray hit/history validity.
// At a low ray count it is a noisy Monte Carlo estimate, but of a smooth
// geometric quantity - "how much of this point's hemisphere can the
// screen/history resolve" - which neighbouring pixels on the same surface
// genuinely share. So a depth+normal-weighted average over a small
// neighbourhood is not a cosmetic blur, it is a better estimate of the same
// number. The weights keep it honest: bleeding coverage across a depth
// discontinuity or a normal break would import a neighbour's hemisphere, the
// halo artifact screen-space AO is famous for.
//
// ==== The `a == 0` path must stay exact ====
// A pixel where no ray resolved and no neighbour resolved must emit a == 0
// exactly: that is what makes Lighting.ps bit-identical to SSGI off. The
// bilateral below cannot manufacture coverage out of an all-zero
// neighbourhood (weighted mean of zeros is zero), and the sky path writes zero
// directly.

#include "SSGICommon.hlsli"

// Must match how NRD.lib was built, and must be defined before the include.
#define NRD_NORMAL_ENCODING    2
#define NRD_ROUGHNESS_ENCODING 1
#include "NRD.hlsli"

Texture2D<float>  gDepth        : register(t1, space0);
Texture2D<float4> gNormal       : register(t2, space0);
Texture2D<float4> gDiffDenoised : register(t6, space0);   // NRD OUT_DIFF_RADIANCE_HITDIST
Texture2D<float>  gRawCoverage  : register(t7, space0);   // trace confidence coverage
Texture2D<float2> gVelocity     : register(t3, space0);   // raw NDC delta
Texture2D<float>  gCovHistory   : register(t9, space0);   // PREVIOUS filtered coverage

SamplerState gLinearClamp : register(s0, space0);

RWTexture2D<float4> OutResult     : register(u5, space0);
RWTexture2D<float>  OutCovHistory : register(u6, space0);   // this frame's filtered coverage
RWTexture2D<float>  OutDepthHistory : register(u7, space0); // current reverse-Z for next trace

// One 8x8 full-res group covers a 4x4 half-res footprint. Its 3x3 gather needs
// only a 6x6 half-res tile, so load those 36 samples once instead of issuing
// 64 pixels * 9 taps * four textures = 2304 texture loads per group.
groupshared float4 sDiff[36];
groupshared float4 sGuideNormal[36];
groupshared float  sGuideDepth[36];
groupshared float  sCoverage[36];


[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID,
          uint3 gtid : SV_GroupThreadID,
          uint3 gid  : SV_GroupID)
{
    const uint lane = gtid.y * 8u + gtid.x;
    const int2 hmax = int2((int)traceW - 1, (int)traceH - 1);
    const int2 cacheBase = int2(gid.xy * 4u) - 1;
    if (lane < 36u)
    {
        const int2 local = int2((int)(lane % 6u), (int)(lane / 6u));
        const int2 q = clamp(cacheBase + local, int2(0, 0), hmax);
        const int2 g = min(q << 1, int2((int)screenW - 1, (int)screenH - 1));
        sDiff[lane]        = gDiffDenoised.Load(int3(q, 0));
        sCoverage[lane]    = gRawCoverage.Load(int3(q, 0));
        sGuideDepth[lane]  = gDepth.Load(int3(g, 0)).r;
        sGuideNormal[lane] = gNormal.Load(int3(g, 0));
    }
    GroupMemoryBarrierWithGroupSync();

    const int2 px = int2(dtid.xy);
    if (px.x >= (int)screenW || px.y >= (int)screenH) return;

    const float ndcZ = gDepth.Load(int3(px, 0)).r;
    // Written for every in-bounds pixel, sky included. Trace consumes this only
    // after m_depthHistoryValid becomes true on the next completed frame.
    OutDepthHistory[px] = ndcZ;
    if (ndcZ <= 0.0)
    {
        // Sky: no surface, no hemisphere, no contribution. a = 0 hands the
        // pixel entirely back to the far-field chain.
        OutResult[px] = float4(0, 0, 0, 0);
        OutCovHistory[px] = 0.0;
        return;
    }

    const float  linZ = SSGI_LinearDepth(ndcZ);
    const float3 N    = normalize(gNormal.Load(int3(px, 0)).rgb * 2.0 - 1.0);

    float3 irrSum = float3(0, 0, 0);
    float  covSum = 0.0;
    float  wSum   = 0.0;
    // Neighbourhood bounds for the temporal clamp below - free, because the
    // gather loop already visits and validates exactly these samples.
    float  covMin = 1.0;
    float  covMax = 0.0;

    // ---- Bilateral upsample from the half-resolution trace ----
    // This dispatch is full res; everything it reads (radiance, coverage, and
    // their guides) is half res. A half-res texel q stood for full-res pixel q*2
    // when the trace ran, so its depth/normal guide is simply the G-buffer at
    // q*2, which is why no second set of half-res guide textures is bound.
    //
    // A 3x3 half-res neighbourhood covers a 6x6 full-res footprint. That is the
    // point of half res for quality as well as cost: each full-res pixel is
    // reconstructed from 9 traced samples x rayCount rays, so at 4 rays it draws
    // on 36 rays while paying for one quarter of the tracing. Per-pixel private
    // rays (full-res tracing) cannot pool like that at any affordable ray count -
    // the same trade Lumen makes with screen probes, at a milder ratio.
    //
    // Radiance and coverage are accumulated under the same weights so that
    // Lighting.ps's reconstruction `nearIrr + farDiffuse * (1 - cov)` stays
    // self-consistent: filtering the two halves of a partition with different
    // footprints re-introduces noise even when each is individually smooth.
    const int2 hc = px >> 1;

    [unroll] for (int dy = -1; dy <= 1; ++dy)
    [unroll] for (int dx = -1; dx <= 1; ++dx)
    {
        const int2 local = hc + int2(dx, dy) - cacheBase;
        const uint si = (uint)(local.y * 6 + local.x);
        const float qz = sGuideDepth[si];
        if (qz <= 0.0) continue;                    // sky sample: no hemisphere

        // Relative depth tolerance - scales with distance the way both depth
        // precision and geometric scale do; an absolute one over-rejects far geometry
        // and under-rejects near.
        const float qlin = SSGI_LinearDepth(qz);
        if (abs(qlin - linZ) / max(linZ, 1e-3) > 0.05) continue;

        const float3 qn = normalize(sGuideNormal[si].rgb * 2.0 - 1.0);
        const float  wn = pow(saturate(dot(qn, N)), 8.0);
        if (wn <= 1e-3) continue;

        const float4 qPacked = sDiff[si];
        irrSum += REBLUR_BackEnd_UnpackRadianceAndNormHitDist(qPacked).rgb * wn;
        const float qCov = sCoverage[si];
        covMin = min(covMin, qCov);
        covMax = max(covMax, qCov);
        covSum += qCov * wn;
        wSum   += wn;
    }

    // wSum == 0 means all nine samples were rejected - a thin sliver or a
    // silhouette pixel with no valid half-res neighbour. Fall back to the nearest
    // sample rather than to zero: zero would drop this pixel's near-field GI
    // entirely and read as a dark speckle along every edge.
    const int2 hn = clamp(hc, int2(0, 0), hmax);
    const int2 hnLocal = hn - cacheBase;
    const uint hnIndex = (uint)(hnLocal.y * 6 + hnLocal.x);
    const float covSpatial = (wSum > 1e-4)
        ? saturate(covSum / wSum)
        : saturate(sCoverage[hnIndex]);
    if (wSum <= 1e-4)
        covMin = covMax = covSpatial;

    // ---- Temporal coverage accumulation ----
    // The radiance half of the partition goes through REBLUR, which is temporal
    // and spatial. Without this the coverage half would be filtered spatially
    // only, and the two halves of `nearIrr + farDiffuse * (1 - cov)` would see
    // different operators in time (the shared-weight gather above matches them in
    // space). Low-spp coverage is noisy and the ray directions rotate every frame
    // (SSGI_Rand2 mixes frameIndex), so the blend weight between SSGI and the
    // DDGI/sky far field would shimmer while both inputs are stable. TAA hides
    // it; accumulating here keeps SSGI usable with TAA off.
    float cov = covSpatial;
    if (covHistoryValid != 0u)
    {
        const float2 uv  = (float2(px) + 0.5) * float2(invScreenW, invScreenH);
        const float2 mv  = gVelocity.Load(int3(px, 0)).xy;
        // Raw NDC delta -> UV delta (Y flips), same convention as the trace.
        const float2 prevUV = SSGI_PreviousUV(uv,mv);
        if (all(prevUV >= 0.0) && all(prevUV <= 1.0))
        {
            // Neighbourhood clamp, the standard rejection: history that cannot
            // be produced by anything visible in this pixel's own
            // neighbourhood is stale (disocclusion, a moving object, a cut) and
            // gets pulled back into range instead of ghosting. Cheap here
            // because covMin/covMax came free from the gather.
            const float hist = clamp(gCovHistory.SampleLevel(gLinearClamp, prevUV, 0),
                                     covMin, covMax);

            // Blend rate follows screen motion: a still camera can afford a long
            // window (coverage is a geometric quantity and genuinely static),
            // while fast motion must let the new estimate win or the weight
            // lags the geometry it is weighting.
            const float velPx  = length(mv * float2(screenW, screenH) * 0.5);
            const float alpha  = lerp(0.08, 0.5, saturate(velPx / 8.0));
            cov = lerp(hist, covSpatial, alpha);
        }
    }
    OutCovHistory[px] = cov;

    // REBLUR's OUT_DIFF_RADIANCE_HITDIST is packed (YCoCg), not linear RGB, and
    // must go through the matching back-end unpack, as the specular path does
    // (HybridReflNRDResolve.cs.hlsl). Reading .rgb raw hands the shader
    // luminance in .r and chroma in .gb.
    float3 nearIrr = (wSum > 1e-4)
        ? (irrSum / wSum)
        : REBLUR_BackEnd_UnpackRadianceAndNormHitDist(
              sDiff[hnIndex]).rgb;

    // Non-negative and finite. NRD can emit small negatives from its spatial
    // kernels, and a negative irradiance folded into Lighting.ps would subtract
    // light from the far-field term - visible as dark rims exactly at coverage
    // edges. Same reasoning as the DDGI atlas's write-site sanitisation.
    nearIrr = (any(isnan(nearIrr)) || any(isinf(nearIrr))) ? float3(0, 0, 0)
                                                          : max(nearIrr, 0.0);

    // ---- Debug ladder ----
    // Each rung isolates one link, so a failure names its own cause instead of
    // looking like "SSGI does nothing" (every failure in this chain renders
    // identically to the feature being off).
    //
    //   2 MAGENTA  - pure binding test. Ignores the trace, the denoiser and the
    //                coverage entirely: writes a constant at a = 1, so Lighting.ps
    //                drops the far field and the screen must turn magenta. If it
    //                does not, LightCB.ssgiResultIdx is wrong or the fold is not
    //                running, and nothing downstream matters.
    //   3 COVERAGE - the march. Grey = fraction of the hemisphere resolved.
    //   4 NEAR     - the fetched radiance. Black here with grey in 3 means the
    //                march hits but the previous-frame colour fetch returns 0.
    //   1 CALIB    - far field at coverage 1; the frame must not change. A null
    //                test, so it is only meaningful once 2 has passed.
    if (debugMode == 2u)
    {
        OutResult[px] = float4(1.0, 0.0, 1.0, 1.0);
        return;
    }
    if (debugMode == 3u)
    {
        OutResult[px] = float4(cov.xxx * 2.0, 1.0);
        return;
    }
    if (debugMode == 4u)
    {
        OutResult[px] = float4(nearIrr * 4.0, 1.0);
        return;
    }

    OutResult[px] = float4(nearIrr, cov);
}
