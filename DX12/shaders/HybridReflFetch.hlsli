#ifndef HYBRID_REFL_FETCH_HLSLI
#define HYBRID_REFL_FETCH_HLSLI

// HybridReflFetch.hlsli - prev-frame screen-radiance fetch shared by IntersectSW and
// IntersectHW. Include AFTER HybridReflCommon.hlsli.
//
// OWNS the declarations of t5 / t20 / t21 / s0: including files must not redeclare
// gVelocity / gPrevHDR / gPrevDepth / gSampClamp.
//
// Scheme (AMD HSR Intersect.hlsl + DeferredShade):
//   fetch UV  = hitUV - velocity(hitUV) decoded to UV (object-motion aware; error bounded
//               by ~1 texel). NEVER derive the fetch UV from a depth reprojection: Hi-Z
//               landing depth is cell-quantized, and matrix-reprojecting it gives
//               static dither.
//   validation: prevD sky test always; matrix z-visibility test ONLY when the mv agrees
//               with the static-scene reprojection (camera motion over a static
//               surface, where the matrix assumption is exact and catches
//               disocclusion). For moving surfaces the two disagree and the mv is
//               trusted (AMD samples history blindly; a 1-frame stale sample is
//               REBLUR's job).

Texture2D<float2> gVelocity  : register(t5,  space0);   // raw NDC delta
Texture2D<float4> gPrevHDR   : register(t20, space0);   // prev-frame scene-color pyramid
Texture2D<float>  gPrevDepth : register(t21, space0);   // prev-frame NDC depth
SamplerState      gSampClamp : register(s0,  space0);

// Screen-hit radiance feeds NRD before the final Lighting fold. Keeps the mirror band's
// HDR headroom but converges rough reflections to the 8-nit guard the analytic environment
// fallback uses. A safety rail for bad/stale history samples; the reflection-free Lighting
// MRT is the primary recursion fix.
float3 HR_ClampScreenReuseRadiance(float3 value, float roughFactor)
{
    if (or(any(isnan(value)), any(isinf(value))))
        return float3(0.0, 0.0, 0.0);

    value = max(value, 0.0);
    // Curve delegated to HR_ClampTracedRadiance (HybridReflCommon.hlsli) so the three clamp
    // sites - screen-reuse here and the HW/SW shared write sites - cannot drift apart. This one
    // keeps its own NaN/negative scrub because it runs on history data, which the write sites
    // have already sanitised.
    return HR_ClampTracedRadiance(value, roughFactor);
}

// hitUV/hitWS describe the hit surface as seen ON SCREEN THIS FRAME (hitWS reconstructed
// from full-res depth at the hit pixel, not a Hi-Z cell value). rayLen = world-space length
// of the traced ray to this hit; it turns the roughness into an actual FOOTPRINT in the
// reflected image, without which a long rough ray (whose lobe covers a large patch of the
// scene) would fetch a near-sharp mip.
bool HR_FetchPrevRadiance(float3 hitWS, float2 hitUV, float roughFactor,
                          float rayLen, out float3 radiance,
                          out bool dynamicSurface)
{
    radiance = float3(0.0, 0.0, 0.0);
    dynamicSurface = false;
    if (hasPrevHdr == 0 || resetHistory != 0)
        return false;

    const float2 screenDim = float2(screenW, screenH);
    int2 hitPx = int2(clamp(hitUV * screenDim, float2(0.0, 0.0), screenDim - 1.0));
    const float2 vel    = gVelocity.Load(int3(hitPx, 0));
    const float2 prevUV = HR_PrevUVFromVelocity(hitUV, vel);
    if (or(any(prevUV <= 0.0), any(prevUV >= 1.0)))
        return false;

    // POINT-sample the depth history: bilinear filtering at a sub-texel prevUV mixes
    // foreground/background depth across prev-frame edges into a value no real surface has,
    // and the z-oracle below then false-rejects a ~1px band along every reflected silhouette
    // (AMD samples depth with point semantics too; filtering is for color only).
    const int2 prevPx = int2(clamp(prevUV * screenDim,
                                   float2(0.0, 0.0), screenDim - 1.0));
    const float prevD = gPrevDepth.Load(int3(prevPx, 0));
    if (prevD <= 0.0)               // reverse-Z: sky last frame - the surface was not there
        return false;

    // Static-scene reprojection of the hit point (exact for static surfaces).
    const float4 prevClip = mul(float4(hitWS, 1.0), prevViewProj);
    if (prevClip.w > 1e-3)
    {
        const float2 prevUVMatrix = float2(prevClip.x / prevClip.w * 0.5 + 0.5,
                                           0.5 - prevClip.y / prevClip.w * 0.5);
        const float2 deltaPx = abs(prevUVMatrix - prevUV) * screenDim;
        if (max(deltaPx.x, deltaPx.y) < 1.5)
        {
            // mv agrees with the static assumption -> the matrix z is a valid visibility oracle:
            // reject when last frame's camera saw a DIFFERENT surface at that pixel (disocclusion).
            const float prevZ = prevClip.z / prevClip.w;
            if (prevZ <= 0.0)
                return false;
            const float linExpect = HR_LinearizeReverseZ(saturate(prevZ));
            const float linGot    = HR_LinearizeReverseZ(prevD);
            if (abs(linExpect - linGot) > max(0.02 * linGot, 0.05))
                return false;
        }
        else
        {
            // The landing surface moved independently of the camera. The
            // radiance fetch above is still valid, but the MIRROR pixel's
            // GBuffer velocity cannot describe this motion. Propagate the bit
            // so NRD can reject history for this reflected sample instead of
            // accumulating the previous pose at a static mirror coordinate.
            dynamicSurface = true;
        }
    }

    // Cone-footprint mip, at parity with the engine's PickConeMip (SSRTrace.cs.hlsl): the
    // lobe's cone radius at rayLen, projected to pixels at the hit's depth. The
    // `roughFactor * 3.0` term alone ignores ray length and hit distance and tops out at 3 mips
    // however large a patch the lobe covers (under-filtered for a 1-spp estimator); it stays as
    // the floor, so the mip is never sharper than it, and the smoothstep gate leaves the mirror
    // band on that floor alone.
    //
    // Uses the TRACED lobe roughness, not the GBuffer roughness, so the prefilter matches the
    // HR_LOBE_BIAS narrowing in HR_SampleReflectionDir. The 4.0 clamp matches SSRTracePass's
    // m_coneMipMax: beyond it, whatever surrounds a small reflected object (usually sky)
    // bleeds into the sample. hitLinZ comes from prevD, one frame stale; harmless for an LOD
    // selector.
    const float roughness   = roughFactor * max(roughnessThreshold, 1e-4);
    const float lobeRough   = HR_TracedLobeRoughness(roughness);
    const float hitLinZ     = HR_LinearizeReverseZ(prevD);
    const float coneRadius  = lobeRough * lobeRough * rayLen;
    const float pixelRadius = coneRadius * float(screenH) * 0.5 / max(hitLinZ, 0.1);
    const float coneMip     = clamp(log2(max(pixelRadius, 1.0)), 0.0, 4.0);
    const float glossy      =
        smoothstep(mirrorRoughness, mirrorRoughness * 1.5, roughness);

    const float mip = min(max(roughFactor * 3.0, coneMip * glossy), prevHdrMaxMip);
    radiance = gPrevHDR.SampleLevel(gSampClamp, prevUV, mip).rgb;
    radiance = HR_ClampScreenReuseRadiance(radiance, roughFactor);
    return true;
}

#endif // HYBRID_REFL_FETCH_HLSLI
