// Screen-space intersection stage of the hybrid-reflection pipeline
// (ClassifyTiles -> PrepareArgsSW -> this, indirect -> PrepareArgsHW -> IntersectHW
// -> NRDPack -> REBLUR -> NRDResolve).
//
// Port of the AMD FidelityFX SSSR v1.1.4 Hi-Z traversal (ffx_sssr_intersect.h:
// InitialAdvanceRay / AdvanceRay / HierarchicalRaymarch / ValidateHit) with the HSR
// sample's SW -> HW demotion (wave-aggregated HW ray-list append + hit-counter
// feedback). Functions keep the FFX names with an HR_ prefix for diffing.
//
// The engine is reverse-Z, so FFX_SSSR_OPTION_INVERTED_DEPTH is always on. The four
// sites are marked "INVERTED-DEPTH SITE (n)":
//   (1) background: sky/cleared depth is exactly 0.0 -> test `z <= 0.0`
//   (2) AdvanceRay: the depth plane is used only when direction.z < 0 (toward far)
//   (3) aboveSurface = surfaceZ < position.z (bigger z = nearer)
//   (4) the Hi-Z reduce is MAX -> gDepthHier.r = nearest; always read `.r`
//
// Outcomes (rtOn = (unifiedFlags & 2) != 0 is hybrid; the SSR-only column assumes
// missEnvFallback != 0, the default: env = HR_MissEnvRadiance = parallax probes ->
// sky, mixed in radiance, relia = max(envW, sConf)):
//   state                     | hybrid (rtOn)                       | SSR-only (!rtOn)
//   --------------------------+-------------------------------------+---------------------------------
//   origin sky (depth <= 0)   | (0,0,0,0), dist 0 [early return]    | same
//   sky landing               | (skyRad, hasSky), rayTMax, HIT      | (env, envW), rayTMax
//   confident hit + fetch OK  | (rad,1), dist rayLen, ctr HIT       | (lerp(env,rad,conf), relia), rayLen*
//   partial hit (< threshold) | DEMOTE -> HW list, no writes, MISS  | (lerp(env,rad,conf), relia), rayLen*
//   miss / exhausted / exited | DEMOTE -> HW list, no writes, MISS  | (env, envW), rayTMax
//     screen / rejected /     |                                     |
//     fetch failed / degen.   |                                     |
//   (* hitDist of the dominant tier: rayLen when conf > 0.5, else rayTMax; -rayLen for
//   a dynamic-surface hit.)
// A dynamic-surface fetch demotes in hybrid mode. With missEnvFallback == 0 the
// SSR-only column is the conservative baseline: partial (rad, conf), miss zeros ->
// Lighting's dim fill. Hit-counter atomics run only when rtOn && (unifiedFlags & 1).
// NaN radiance is scrubbed to zero before every write.

#include "HybridReflCommon.hlsli"
#include "HybridReflFetch.hlsli"   // owns t5 / t20 / t21 / s0 + HR_FetchPrevRadiance
#include "DDGISampling.hlsli"
#include "indirect_specular.hlsli"

// Reflection probes for the SSR-only environment fallback: the buffer is
// opened from the heap (HybridReflConstants.probeBufferIdx), as in IntersectHW.
#include "reflection_probe.hlsli"
#define HR_PROBE_CUBEMAP_MIPS 7   // mirrors Reflection::kProbeCubemapMips

// ==== Registers owned by this stage ====
Texture2D<float>    gDepth      : register(t2,  space0); // main depth: ray-origin pixel only
Texture2D<float4>   gNormal     : register(t3,  space0); // GBuffer normal, *2-1
Texture2D<float4>   gSurface    : register(t4,  space0); // .r = perceptual roughness
StructuredBuffer<DDGIVolumeGPU> gDDGIVolumes : register(t7, space0);
Texture2D<float4> gDDGIIrradiance[DDGI_MAX_VOLUMES] : register(t0, space4);
Texture2D<float2> gDDGIDepth[DDGI_MAX_VOLUMES] : register(t0, space5);
StructuredBuffer<DDGIProbeData> gDDGIProbeData[DDGI_MAX_VOLUMES] : register(t0, space6);
TextureCube<float4> gSkyCube    : register(t23, space0); // prefiltered sky
Texture2D<float2>   gDepthHier  : register(t27, space0); // Hi-Z, .r = max = NEAREST (grass-free)

RWTexture2D<float4>      OutRawColor   : register(u0, space0);
RWTexture2D<float>       OutHitDist    : register(u1, space0);
RWStructuredBuffer<uint> g_SWRayList   : register(u2, space0);
RWStructuredBuffer<uint> g_HWRayList   : register(u3, space0);
RWStructuredBuffer<uint> g_RayCounter  : register(u4, space0);
RWTexture2D<uint>        OutHitCounter : register(u6, space0);

SamplerState gSampWrap : register(s1, space0);

#define HR_FLOAT_MAX   3.402823466e+38
#define HR_HIZ_MAX_MIP 6   // FFX_SSSR_DEPTH_HIERARCHY_MAX_MIP
#define HR_FINISH_TRACE_STEPS 16u

// Traversal outcomes (loop exits of HR_HierarchicalRaymarch).
#define HR_TRACE_HIT         0u  // currentMip < mostDetailedMip -> candidate hit
#define HR_TRACE_EXHAUSTED   1u  // iteration cap reached; not a hit
#define HR_TRACE_EXIT_SCREEN 2u  // HSR early break: position.xy left [0,1]^2
#define HR_TRACE_SKY         3u  // HSR early break: marched into background

// Hi-Z depth load with the non-power-of-2 mip clamp: the real mip grid is
// floor-halved, so the load coordinate is clamped to the mip's actual extent (no
// out-of-bounds tunneling on non-pow2 screens).
float HR_LoadDepthHier(float2 mipPosition, int mip)
{
    // INVERTED-DEPTH SITE (4): the Hi-Z reduce is MAX, so under reverse-Z the .r
    // channel holds the nearest depth of the footprint. Always read .r.
    uint mipW   = max(screenW >> (uint)mip, 1u);
    uint mipH   = max(screenH >> (uint)mip, 1u);
    int2 loadPx = min(int2(mipPosition), int2((int)mipW - 1, (int)mipH - 1));
    return gDepthHier.Load(int3(loadPx, mip)).r;
}

// Verbatim port of FFX_SSSR_InitialAdvanceRay.
void HR_InitialAdvanceRay(float3 origin, float3 direction, float3 invDirection,
                          float2 currentMipResolution,
                          float2 currentMipResolutionInv,
                          float2 floorOffset, float2 uvOffset,
                          out float3 position, out float currentT)
{
    float2 currentMipPosition = currentMipResolution * origin.xy;

    // Intersect ray with the half box that is pointing away from the ray origin.
    float2 xyPlane = floor(currentMipPosition) + floorOffset;
    xyPlane = xyPlane * currentMipResolutionInv + uvOffset;

    // o + d * t = p' => t = (p' - o) / d
    float2 t  = xyPlane * invDirection.xy - origin.xy * invDirection.xy;
    currentT = min(t.x, t.y);
    position  = origin + currentT * direction;
}

// Port of FFX_SSSR_AdvanceRay with the inverted-depth branches resolved.
bool HR_AdvanceRay(float3 origin, float3 direction, float3 invDirection,
                   float2 currentMipPosition,
                   float2 currentMipResolutionInv,
                   float2 floorOffset, float2 uvOffset, float surfaceZ,
                   inout float3 position, inout float currentT)
{
    float2 xyPlane = floor(currentMipPosition) + floorOffset;
    xyPlane = xyPlane * currentMipResolutionInv + uvOffset;
    float3 boundaryPlanes = float3(xyPlane, surfaceZ);

    // Intersect ray with the half box that is pointing away from the ray origin.
    // o + d * t = p' => t = (p' - o) / d
    float3 t = boundaryPlanes * invDirection - origin * invDirection;

    // INVERTED-DEPTH SITE (2): do not use the z plane when shooting out of the depth
    // buffer; under reverse-Z only rays marching toward far (direction.z < 0) may clamp
    // at the depth plane.
    t.z = select(direction.z < 0.0, t.z, HR_FLOAT_MAX);

    // Choose nearest intersection with a boundary.
    float tMin = min(min(t.x, t.y), t.z);

    // INVERTED-DEPTH SITE (3): larger z means closer to the camera.
    bool aboveSurface = surfaceZ < position.z;

    // asuint comparison avoids NaN/Inf logic: bitwise equality tells whether tMin
    // is the t.z fed into the min above.
    bool skippedTile = asuint(tMin) != asuint(t.z) && aboveSurface;

    // Make sure to only advance the ray if we're still above the surface.
    currentT = select(aboveSurface, tMin, currentT);

    position = origin + currentT * direction;

    return skippedTile;
}

// Port of FFX_SSSR_HierarchicalRaymarch with:
//   * mostDetailedMip fixed to 0 (classify already thins rays; the engine CB has no
//     per-ray mirror/mip split and no min_traversal_occupancy),
//   * the HSR sample's early loop breaks (screen exit / background landing),
//   * the FFX v1.1.4 nextMipIsOutOfRange clamp (mip and resolution freeze together
//     so they never desynchronize),
//   * the non-pow2 mip load clamp of HR_LoadDepthHier.
// origin and direction must be in screen space [0,1]^2.
float3 HR_HierarchicalRaymarch(float3 origin, float3 direction,
                               float2 screenSize, out uint outcome)
{
    // select(): the condition is a vector (HLSL 2021's ?: needs a scalar one).
    const float3 invDirection =
        select(direction != float3(0.0, 0.0, 0.0),
               float3(1.0, 1.0, 1.0) / direction,
               float3(HR_FLOAT_MAX, HR_FLOAT_MAX, HR_FLOAT_MAX));

    // Start on mip with highest detail.
    const int mostDetailedMip = 0;
    int currentMip = mostDetailedMip;

    // Could recompute these every iteration, but it's faster to hoist them
    // out and update them.
    float2 currentMipResolution     = screenSize;   // mip 0
    float2 currentMipResolutionInv = rcp(currentMipResolution);

    // Offset to the bounding boxes uv space to intersect the ray with the
    // center of the next pixel. This means we ever so slightly over shoot
    // into the next region.
    float2 uvOffset = 0.005 * exp2((float)mostDetailedMip) / screenSize;
    uvOffset.x = select(direction.x < 0.0, -uvOffset.x, uvOffset.x);
    uvOffset.y = select(direction.y < 0.0, -uvOffset.y, uvOffset.y);

    // Offset applied depending on current mip resolution to move the boundary
    // to the left/right upper/lower border depending on ray direction.
    float2 floorOffset;
    floorOffset.x = select(direction.x < 0.0, 0.0, 1.0);
    floorOffset.y = select(direction.y < 0.0, 0.0, 1.0);

    // Initially advance ray to avoid immediate self intersections.
    float  currentT;
    float3 position;
    HR_InitialAdvanceRay(origin, direction, invDirection,
                         currentMipResolution, currentMipResolutionInv,
                         floorOffset, uvOffset, position, currentT);

    // Don't ascend past mips we did not generate (clamped defensively so a
    // hizMipCount of 0 degrades to a mip-0-only march instead of UB).
    const int maxMip = clamp((int)hizMipCount - 1, 0, HR_HIZ_MAX_MIP);

    uint i = 0;
    [loop]
    while (and(i < maxTraversalIters, currentMip >= mostDetailedMip))
    {
        // HSR early break: ray left the screen.
        if (or(any(position.xy > float2(1.0, 1.0)),
               any(position.xy < float2(0.0, 0.0))))
        {
            outcome = HR_TRACE_EXIT_SCREEN;
            return position;
        }
        // INVERTED-DEPTH SITE (1): background. Sky/cleared depth is exactly 0.0 under
        // reverse-Z, so the ray marched into the sky when its own z reaches 0. Test
        // `<= 0.0`, never an epsilon.
        if (position.z <= 0.0)
        {
            outcome = HR_TRACE_SKY;
            return position;
        }

        float2 currentMipPosition = currentMipResolution * position.xy;
        float  surfaceZ = HR_LoadDepthHier(currentMipPosition, currentMip);
        bool   skippedTile = HR_AdvanceRay(origin, direction, invDirection,
                                            currentMipPosition,
                                            currentMipResolutionInv,
                                            floorOffset, uvOffset, surfaceZ,
                                            position, currentT);

        // Don't increase mip further than this because we did not generate it
        // (FFX v1.1.4): when frozen, resolution is frozen WITH the mip so the
        // two never fall out of sync.
        bool nextMipIsOutOfRange = and(skippedTile, currentMip >= maxMip);
        if (!nextMipIsOutOfRange)
        {
            currentMip += select(skippedTile, 1, -1);
            currentMipResolution     *= select(skippedTile, 0.5, 2.0);
            currentMipResolutionInv *= select(skippedTile, 2.0, 0.5);
        }

        ++i;
    }

    // currentMip below mostDetailedMip -> converged on a candidate hit; iteration
    // cap reached -> exhausted (not a hit).
    outcome = (currentMip < mostDetailedMip) ? HR_TRACE_HIT
                                                : HR_TRACE_EXHAUSTED;
    return position;
}

// Perspective-correct depth of the ray at a screen UV. The projected world ray is
// a straight line in screen XY, but its NDC depth is not linear in the screen-space
// ray parameter; the Hi-Z walker follows a linear float3 for speed, so on distant
// reverse-Z surfaces it can report a crossing several pixels early. Back-solves the
// world-ray parameter from uv and returns the real clipZ / clipW; valid is false
// when the ray never reaches uv or lands on sky / beyond far.
float HR_EvalRayNdcZ(float2 uv, float4 startClip, float4 dirClip,
                     out bool valid)
{
    const float ndcX = uv.x * 2.0 - 1.0;
    const float ndcY = 1.0 - uv.y * 2.0;
    const float denX = ndcX * dirClip.w - dirClip.x;
    const float denY = ndcY * dirClip.w - dirClip.y;

    const bool useX = abs(denX) > abs(denY);
    const float den  = select(useX, denX, denY);
    if (abs(den) < 1e-8)
    {
        valid = false;
        return HR_FLOAT_MAX;
    }

    const float s = select(useX,
                           (startClip.x - ndcX * startClip.w) / den,
                           (startClip.y - ndcY * startClip.w) / den);
    const float clipW = startClip.w + s * dirClip.w;
    if (s < 0.0 || clipW < 1e-3)
    {
        valid = false;
        return HR_FLOAT_MAX;
    }

    const float z = (startClip.z + s * dirClip.z) / clipW;
    // Preserve the engine reverse-Z contract: exactly/below zero is sky or
    // beyond the far plane and must never be accepted as a geometry crossing.
    valid = (z > 0.0);
    return select(valid, z, HR_FLOAT_MAX);
}

// Refines the coarse Hi-Z landing against mip 0. Returns true with refinedHit at
// the perspective-correct crossing, or false (refinedHit = hizPosition) when the walk
// finds none within maxSteps. Do not accept a failed refinement on the dynamic
// thickness tolerance alone: that brings back distant-floor false reflections; the
// caller grades it with the rayLen-independent finishConf residual.
bool HR_LinearFinishTrace(float3 hizPosition, float3 direction,
                          float4 startClip, float4 dirClip,
                          float2 screenSize, uint maxSteps,
                          out float3 refinedHit)
{
    const float2 dirPxLen = abs(direction.xy) * screenSize;
    const float pxLen = max(max(dirPxLen.x, dirPxLen.y), 1e-6);
    const float3 step = direction / pxLen; // one dominant-axis pixel

    float3 cur = hizPosition;
    const int2 screenI = int2(screenSize);
    bool foundAbove = false;

    // Move back to a valid sample strictly above a real surface.
    [loop]
    for (uint backI = 0; backI < maxSteps / 2u; ++backI)
    {
        if (or(any(cur.xy < 0.0), any(cur.xy > 1.0)))
        {
            cur += step;
            break;
        }

        const int2 px = clamp(int2(cur.xy * screenSize), int2(0, 0),
                              screenI - 1);
        const float surfaceZ = gDepthHier.Load(int3(px, 0)).r;
        bool validZ;
        const float rayZ = HR_EvalRayNdcZ(cur.xy, startClip, dirClip,
                                           validZ);
        if (validZ && surfaceZ > 0.0 && rayZ > surfaceZ)
        {
            cur.z = rayZ;
            foundAbove = true;
            break;
        }
        cur -= step;
    }

    // No above-anchor found: restart the forward walk from the coarse Hi-Z landing
    // and let the crossing test plus the caller's graded thickness decide (SSRTrace
    // does not gate on foundAbove either). Do not hard-drop here: whether the backward
    // walk finds an above sample within maxSteps/2 depends on sub-pixel phase at
    // grazing angles, so a drop flips accept/reject between adjacent rays and shows a
    // coherent radiance stripe. Wall/column false hits still fail: their perspective
    // ray stays nearer than the surface, so the forward walk finds no crossing.
    if (!foundAbove)
        cur = hizPosition;

    float3 prev = cur;
    bool prevValid;
    float prevRayZ = HR_EvalRayNdcZ(prev.xy, startClip, dirClip,
                                      prevValid);

    // Walk forward until the perspective-correct ray crosses mip-0 depth.
    [loop]
    for (uint forwardI = 0; forwardI < maxSteps; ++forwardI)
    {
        if (or(any(cur.xy < 0.0), any(cur.xy > 1.0)))
            break;

        const int2 px = clamp(int2(cur.xy * screenSize), int2(0, 0),
                              screenI - 1);
        const float surfaceZ = gDepthHier.Load(int3(px, 0)).r;
        bool validZ;
        const float rayZ = HR_EvalRayNdcZ(cur.xy, startClip, dirClip,
                                           validZ);

        if (and(and(validZ, surfaceZ > 0.0), and(rayZ <= surfaceZ, prevValid)))
        {
            // Per-pixel change of the perspective-correct ray depth. Under reverse-Z a
            // descending crossing has prevRayZ > rayZ, so denom is positive when well formed.
            // At grazing angles denom collapses toward 0 and its sign flips on sub-pixel /
            // normal-map noise, making the exact solve tRaw = (prevRayZ - surfaceZ) / denom
            // ill-conditioned: tRaw leaves the [-0.01, 1.01] window on one ray and not the
            // next, a visible radiance stripe. Split on conditioning:
            const float denom = prevRayZ - rayZ;

            // Scale-robust near-parallel threshold: the reverse-Z NDC gap shrinks with
            // distance, so a fixed epsilon misclassifies far surfaces. Tie it to a small
            // fraction of the local ray depth, floored at 1e-6.
            const float parallelEps = max(1e-6, prevRayZ * 1e-3);

            if (denom > parallelEps)
            {
                // Well-conditioned (steeper) crossing: the exact sub-pixel solve plus the
                // depth-discontinuity guard keep steep hits pixel-accurate and reject distant
                // wall/column false hits precisely (tRaw outside the window = the surface jumped,
                // not a real crossing).
                const float tRaw = (prevRayZ - surfaceZ) / denom;
                if (tRaw >= -0.01 && tRaw <= 1.01)
                {
                    const float t = saturate(tRaw);
                    refinedHit.xy = lerp(prev.xy, cur.xy, t);
                    refinedHit.z  = lerp(prevRayZ, rayZ, t);
                    return true;
                }
                // else: foreground/background depth discontinuity -> keep marching.
            }
            else
            {
                // Grazing / near-parallel: tRaw is meaningless (denom -> 0). The crossing itself
                // is the wall discriminator - a distant-wall false hit keeps the perspective ray
                // nearer than the surface and never reaches rayZ <= surfaceZ - so accept at the
                // current sample with the perspective-correct ray z (never the coarse linear Hi-Z
                // z) and let HR_ValidateHit grade it by smooth thickness. A genuine grazing
                // crossing has rayZ ~= surfaceZ and high confidence; an off-surface skim fades
                // out smoothly instead of striping.
                refinedHit.xy = cur.xy;
                refinedHit.z  = rayZ;
                return true;
            }
        }

        prev         = cur;
        prevRayZ   = rayZ;
        prevValid   = validZ;
        cur          += step;
    }

    refinedHit = hizPosition;
    return false;
}

// FFX_SSSR_ValidateHit semantics on engine helpers. Returns confidence in [0,1]
// (0 = rejected); skyOut reports a background landing found during validation (the
// caller merges it with the traversal's own sky break). Deviations from AMD:
//   * thickness distance is |linearZ(ray) - linearZ(surface)| via
//     HR_LinearizeReverseZ (the CB has no inverse projection), with the HSR dynamic
//     tolerance base + roughness^2 x roughScale + rayLen x lenFactor,
//   * an extra self-hit guard on the reconstructed world ray length,
//   * the hit surface comes from gDepthHier mip 0 (grass-free full-res), not the
//     main depth buffer.
float HR_ValidateHit(float3 hit, float2 uv, float3 R, float3 originN,
                     float roughness,
                     float originDepth, float3 originWS,
                     out bool skyOut, out float3 hitWS, out float rayLen)
{
    skyOut = false;
    hitWS  = float3(0.0, 0.0, 0.0);
    rayLen = 0.0;

    // Reject hits outside the view frustum.
    if (hit.x < 0.0 || hit.y < 0.0 || hit.x > 1.0 || hit.y > 1.0)
        return 0.0;

    const float2 screenSize = float2(screenW, screenH);

    // Reject the hit if we didn't advance the ray significantly to avoid
    // immediate self reflection.
    float2 manhattanDist = abs(hit.xy - uv);
    if (manhattanDist.x < (2.0 / screenSize.x) &&
        manhattanDist.y < (2.0 / screenSize.y))
        return 0.0;

    // Don't lookup radiance from the background (AMD checks mip 1).
    int2 texelCoords = int2(screenSize * hit.xy);
    int2 mip1Dims     = int2((int)max(screenW >> 1u, 1u),
                             (int)max(screenH >> 1u, 1u));
    float surfZmip1   = gDepthHier.Load(
        int3(min(texelCoords / 2, mip1Dims - 1), 1)).r;
    if (surfZmip1 <= 0.0)   // reverse-Z sky: EXACTLY 0.0
    {
        skyOut = true;
        return 0.0;
    }

    // Surface normal at the hit. The backface / grazing / self-hit rejections below
    // are smooth confidence multipliers, never hard early-outs: a hard
    // `dot(hitN,R) > 0` (or coplanar-slab) cut flips accept/reject pixel to pixel
    // wherever the reflected ray skims a surface at a grazing angle (normal-map wobble
    // pushes it across the threshold), which reads as coherent stripe bands. SSRTrace
    // fades grazing hits with a smoothstep for the same reason.
    int2 hitPx = min(texelCoords, int2((int)screenW - 1, (int)screenH - 1));
    float3 hitN = normalize(gNormal.Load(int3(hitPx, 0)).xyz * 2.0 - 1.0);

    // Full-res grass-free trace depth for the hit surface reconstruction.
    float surfZ0 = gDepthHier.Load(int3(hitPx, 0)).r;
    if (surfZ0 <= 0.0)      // reverse-Z sky: EXACTLY 0.0
    {
        skyOut = true;
        return 0.0;
    }

    hitWS  = HR_ReconstructWorldPos(hit.xy, surfZ0);
    rayLen = length(hitWS - originWS);

    // Self-hit guard: depth-proportional floor on the world ray length.
    if (rayLen < max(HR_LinearizeReverseZ(originDepth) * 0.005, 0.01))
        return 0.0;

    // Thickness confidence: accept hits within a reasonable distance below the
    // surface; the tolerance grows with roughness (HSR) and ray length.
    float thicknessDist = abs(HR_LinearizeReverseZ(hit.z) -
                              HR_LinearizeReverseZ(surfZ0));
    float tol = depthThicknessBase
              + roughness * roughness * thicknessRoughScale
              + rayLen * thicknessLenFactor;
    float confidence = 1.0 - smoothstep(0.0, tol, thicknessDist);
    confidence *= confidence;

    // Smooth grazing / backface fade instead of a hard `dot(hitN,R) > 0` reject.
    // front > 0: the hit surface faces the incoming ray (valid); front < 0: a back face
    // or a skim of the ray's own surface. The soft window gives REBLUR/TAA a gradient
    // to integrate instead of a per-pixel band (same window as SSRTrace).
    const float front = dot(hitN, -R);
    confidence *= smoothstep(-0.25, -0.05, front);

    // Smooth self-hit fade instead of a hard coplanar-slab reject. A reflected ray
    // leaves its origin surface into the +normal hemisphere, so it never legitimately
    // re-hits the same coplanar, same-facing plane, yet at grazing angles the
    // reverse-Z/Hi-Z crossing error lands false hits back on the origin floor. Fade
    // confidence toward 0 as the hit becomes both coplanar (normals aligned) and close
    // to the origin plane - a product of two smoothsteps, no hard edge. Perpendicular
    // walls (low samePlaneNormal) and separated parallel geometry (large
    // planeDistance) keep full confidence.
    const float samePlaneNormal = dot(hitN, originN);
    const float planeDistance   = abs(dot(hitWS - originWS, originN));
    const float distanceScale   = max(rayLen, HR_LinearizeReverseZ(originDepth));
    const float planeTol = clamp(distanceScale * 0.001, 0.01,
                                 max(depthThicknessBase * 0.5, 0.01));
    const float coplanar = smoothstep(0.95, 0.99, samePlaneNormal);
    const float inSlab   = 1.0 - smoothstep(planeTol * 0.5, planeTol,
                                            planeDistance);
    confidence *= 1.0 - coplanar * inSlab;

    // Fade out hits near the screen borders (AMD vignette).
    float2 fov    = 0.05 * float2((float)screenH / (float)screenW, 1.0);
    float2 border = smoothstep(float2(0.0, 0.0), fov, hit.xy)
                  * (1.0 - smoothstep(float2(1.0, 1.0) - fov,
                                      float2(1.0, 1.0), hit.xy));
    confidence *= border.x * border.y;

    return confidence;
}

// HR_MissEnvRadiance is in the shared header so the HW tier uses the identical
// radiance-space hand-off (rationale there).
#include "hybrid_refl_env.hlsli"

// ==== Entry ====
[numthreads(32, 1, 1)]   // = HR_INTERSECT_GROUP_SIZE
void main(uint3 Gid : SV_GroupID, uint3 GTid : SV_GroupThreadID)
{
    // 2D-split indirect dispatch linearization (see HybridReflCommon).
    const uint rayIdx = (Gid.y * HR_GROUPS_PER_ROW + Gid.x)
                      * HR_INTERSECT_GROUP_SIZE + GTid.x;
    if (rayIdx >= g_RayCounter[HR_CTR_SW_HIST])
        return;

    const uint2 pixel = HR_UnpackRay(g_SWRayList[rayIdx]);
    if (or(pixel.x >= screenW, pixel.y >= screenH))
        return;   // defensive: classify never emits these

    // Ray origin uses MAIN depth (gDepth); the Hi-Z pyramid is grass-free.
    const float depth = gDepth.Load(int3(pixel, 0));
    if (depth <= 0.0)   // reverse-Z: origin pixel is sky (exactly 0.0)
    {
        OutRawColor[pixel] = float4(0.0, 0.0, 0.0, 0.0);
        OutHitDist[pixel]  = 0.0;
        return;
    }

    // Surface and reflection ray. Determinism contract: IntersectHW calls
    // HR_SampleReflectionDir with the same arguments to re-derive this ray.
    const float2 uv       = (float2(pixel) + 0.5) * float2(invScreenW, invScreenH);
    const float3 worldPos = HR_ReconstructWorldPos(uv, depth);
    const float3 N        = normalize(gNormal.Load(int3(pixel, 0)).xyz * 2.0 - 1.0);
    const float3 V        = normalize(cameraPos - worldPos);
    const float  roughness = gSurface.Load(int3(pixel, 0)).r;
    const float3 R = HR_SampleReflectionDir(V, N, roughness, pixel, frameIndex);

    // Screen-space ray from the world matrices (the CB has no separate view/proj):
    // project the origin and a 1 wu step and subtract, as AMD's ProjectDirection does.
    const float3 originSS  = float3(uv, depth);
    const float4 startClip = mul(float4(worldPos, 1.0), viewProj); // w>0: on screen
    float4 endClip = mul(float4(worldPos + R, 1.0), viewProj);
    if (endClip.w <= 1e-3)
    {
        // The end point crossed the camera plane: shrink the world step until the
        // projection is numerically safe.
        float dw = endClip.w - startClip.w;
        float tShrink = clamp((1e-3 - startClip.w) / dw * 0.99, 1e-4, 1.0);
        endClip = mul(float4(worldPos + R * tShrink, 1.0), viewProj);
    }
    float3 endSS;
    endSS.xy = float2(endClip.x / endClip.w * 0.5 + 0.5,
                      0.5 - endClip.y / endClip.w * 0.5);
    endSS.z  = endClip.z / endClip.w;
    // Any positive scale of the true screen direction works for the traversal: the
    // hit is reconstructed from the landing depth, not from t.
    const float3 directionSS = endSS - originSS;
    const bool degenerate = all(abs(directionSS) < 1e-8);

    // ---- Traversal ----
    const float2 screenDim = float2(screenW, screenH);
    uint   outcome = HR_TRACE_EXHAUSTED;   // degenerate rays fall to the miss path
    float3 hit     = float3(0.0, 0.0, 0.0);
    if (!degenerate)
        hit = HR_HierarchicalRaymarch(originSS, directionSS, screenDim, outcome);

    // ---- Validate ----
    bool   skyLanding = (outcome == HR_TRACE_SKY);
    bool   candidate  = (outcome == HR_TRACE_HIT);

    // Hi-Z used a linear NDC-z approximation: sharpen the landing to the
    // perspective-correct crossing, but never hard-drop a non-converged finish walk.
    // Whether the walk converges within HR_FINISH_TRACE_STEPS depends on sub-pixel
    // phase at grazing angles, so a drop flips accept/reject between adjacent rays (in
    // SSR-only mode lerp(env, rad, ..) snaps between env and screen radiance: a visible
    // stripe). Instead every landing is graded by a perspective-depth-residual
    // confidence (finishConf) computed identically on both branches (on failure hit.z
    // becomes the perspective-correct ray z). The tolerance max(base, linSurf x 0.004)
    // is independent of rayLen and roughness, so a distant wall/column false hit (huge
    // residual) still fades to 0.
    float finishConf = 1.0;
    if (candidate)
    {
        const float4 dirClip = mul(float4(R, 0.0), viewProj);
        float3 refinedHit;
        bool   vRay = true;
        if (HR_LinearFinishTrace(hit, directionSS, startClip, dirClip,
                                 screenDim, HR_FINISH_TRACE_STEPS, refinedHit))
        {
            hit = refinedHit;             // pixel-precise crossing; z is already perspective-correct
        }
        else
        {
            // Budget exhausted: keep the coarse landing xy, but replace the
            // linear Hi-Z z with the PERSPECTIVE-correct ray z so the residual
            // gate below measures the TRUE ray depth against the surface.
            const float rz = HR_EvalRayNdcZ(hit.xy, startClip, dirClip, vRay);
            if (vRay)
                hit.z = rz;
        }

        // Single graded gate shared by BOTH branches (no converge/exhaust seam).
        // vRay == false means the ray is past the on-screen vanishing point and
        // never reaches this pixel -> genuine miss (finishConf 0), reached
        // CONTINUOUSLY because the residual is already large as vRay approaches
        // its flip. resid / ftol are linear view depth (world units).
        const int2  hpx     = clamp(int2(hit.xy * screenDim), int2(0, 0),
                                    int2((int)screenW - 1, (int)screenH - 1));
        const float surfZ   = gDepthHier.Load(int3(hpx, 0)).r;
        const float linSurf = HR_LinearizeReverseZ(surfZ);
        const float ftol    = max(depthThicknessBase, linSurf * 0.004);
        const float resid   = abs(HR_LinearizeReverseZ(hit.z) - linSurf);
        finishConf = select(and(vRay, surfZ > 0.0),
                            1.0 - smoothstep(0.0, ftol, resid),
                            0.0);
    }

    float  conf   = 0.0;
    float3 hitWS  = float3(0.0, 0.0, 0.0);
    float  rayLen = 0.0;
    if (candidate)
    {
        bool validateSky;
        conf = HR_ValidateHit(hit, uv, R, N, roughness, depth, worldPos,
                              validateSky, hitWS, rayLen);
        conf *= finishConf;               // smooth finish-trace gate (no hard drop)
        if (validateSky)
        {
            skyLanding = true;
            candidate  = false;
        }
    }

    // Hybrid: hand the hardest screen-space corner to the RT tier. Screen-space hit
    // precision collapses for a near-mirror reflection seen at a grazing angle on a far
    // surface: one Hi-Z cell spans many world units, so the SW hit lands short (pulled
    // toward the plane) and, since REBLUR barely smooths the mirror band, the per-cell
    // quantization reads as stepping. Fade SW confidence below ssrConfidenceThreshold in
    // exactly that corner so the ray demotes to HW, which traces the true world ray. The
    // fade is a smooth product near-mirror x grazing-view x far reflection, never a hard
    // threshold (that stripes). Hybrid only: SSR-only has no RT to demote to, and a
    // slightly short reflection beats the env fallback.
    if (candidate && (unifiedFlags & 2u) != 0u)
    {
        const float mirrorness = 1.0 - smoothstep(mirrorRoughness * 0.5,
                                                  mirrorRoughness, roughness);
        const float grazing    = 1.0 - smoothstep(0.2, 0.5, saturate(dot(V, N)));
        // Absolute world-unit band, deliberately not rayTMax x (0.2, 0.6): scaling with
        // the ray budget re-tunes SW confidence whenever the range knob moves (at rayTMax
        // 150 the band opens to 30-90 wu and the 8-30 wu grazing mirror rays stop being
        // demoted). The values are the validated rayTMax 40 x (0.2, 0.6); ray length and
        // confidence tuning are separate concerns.
        const float farField   = smoothstep(8.0, 24.0, rayLen);
        conf *= 1.0 - mirrorness * grazing * farField;
    }

    // Shared prev-frame radiance fetch (same source as the HW tier).
    float3 rad = float3(0.0, 0.0, 0.0);
    bool fetchOK      = false;
    bool fetchDynamic = false;
    if (candidate && conf > 0.0)
        fetchOK = HR_FetchPrevRadiance(hitWS, hit.xy, HR_RoughFactor(roughness),
                                       rayLen, rad, fetchDynamic);

    // ---- Decision ----
    // All outcomes funnel to the single tail below so the wave-aggregated demotion runs
    // in one place (AMD structure).
    const bool rtOn = (unifiedFlags & 2u) != 0u;   // hybrid = HW tier live

    bool   demote      = false;
    uint   counterFlag = 0u;
    bool   doWrite     = false;
    float4 outColor    = float4(0.0, 0.0, 0.0, 0.0);
    float  outDist     = 0.0;

    if (rtOn)
    {
        // Hybrid: SW answers confidently or hands the ray to HW.
        if (skyLanding)
        {
            // Sky is a final answer; never demote it. The lobe-matched mip and the sky light's
            // iblStrength match the HW miss branch exactly, so skipping the HW ray is free.
            // The mip must come from the shared HR_SkyMissMip: if the tiers disagree, a
            // sky-reflecting tile flipping FULL_SW <-> FULL_HW shows a radiance step. On-screen
            // skybox pixels are not read: a ray that lands on the sky sees sky light, as every
            // other sky term does.
            float3 skyRad = float3(0.0, 0.0, 0.0);
            if (hasSkyCube != 0u)
            {
                // Sun-disk firefly guard (see HR_ClampEnvRadiance).
                skyRad = HR_ClampEnvRadiance(
                             gSkyCube.SampleLevel(gSampWrap, R,
                                                  HR_SkyMissMip(roughness)).rgb)
                       * iblStrength;
            }
            doWrite     = true;
            outColor    = float4(skyRad, (hasSkyCube != 0u) ? 1.0 : 0.0);
            outDist     = rayTMax;
            counterFlag = HR_HITCOUNTER_SW_HIT_FLAG;
        }
        // A dynamic-surface fetch (fetchDynamic) demotes instead of accepting: the
        // mv-trusted prev-frame sample is pose N-1 without depth validation
        // (HR_FetchPrevRadiance's moving-surface branch has no z-oracle), so a deforming
        // reflector tears against neighbouring pixels' HW analytic shading. The HW re-trace
        // shades the same hit at the current pose - one estimator for the whole moving
        // reflector - and the SW_MISS counter drifts skinned-dominated tiles toward
        // FULL_HW. Sub-1.5 px motion stays here: the fetch is depth-validated in that
        // regime and the error is sub-pixel. SSR-only mode has no HW to demote to.
        else if (and(and(candidate, conf >= ssrConfidenceThreshold),
                     and(fetchOK, !fetchDynamic)))
        {
            doWrite     = true;
            outColor    = float4(rad, 1.0);
            outDist     = rayLen;
            counterFlag = HR_HITCOUNTER_SW_HIT_FLAG;
        }
        else
        {
            // Miss / exhausted / exited screen / rejected / low confidence / fetch failed /
            // degenerate direction: demote. The HW pass re-derives the same ray and owns this
            // pixel's output.
            demote      = true;
            counterFlag = HR_HITCOUNTER_SW_MISS_FLAG;
        }
    }
    else if (missEnvFallback != 0u)
    {
        // SSR-only with the env fallback (default): radiance-space hand-off. The march
        // confidence must not reach the fold as an alpha: REBLUR never filters the conf
        // channel, so a per-pixel / per-frame conf becomes fold dither once TAA is off, and
        // tolerance tuning cannot fix that. Every outcome mixes screen radiance with the
        // probe/sky environment in radiance (screen fraction = march conf; miss / sky
        // landing = pure env) and folds at a stable relia: REBLUR integrates the mixture,
        // and the R2 temporal rotation turns per-frame conf jitter into a converging
        // estimate instead of flicker.
        const float sConf = select(and(and(candidate, fetchOK), !skyLanding),
                                   saturate(conf), 0.0);
        float  envW;
        float3 env = HR_MissEnvRadiance(worldPos, N, V, R, roughness, envW, true);
        doWrite  = true;
        outColor = float4(lerp(env, rad, sConf),
                          max(envW, sConf));   // FlatColor + no-probe miss -> 0
        // hitDist of the dominant tier, never a lerp of hitT across sources: REBLUR's
        // virtual-motion reprojection needs a consistent field.
        outDist  = select(sConf > 0.5, select(fetchDynamic, -rayLen, rayLen), rayTMax);
    }
    else
    {
        // SSR-only, conservative baseline (missEnvFallback == 0).
        doWrite = true;
        if (and(and(!skyLanding, candidate), and(conf > 0.02, fetchOK)))
        {
            outColor = float4(rad, conf);
            outDist  = fetchDynamic ? -rayLen : rayLen;
        }
        // else: zeros already staged (Lighting's dim probe/sky fill owns it)
    }

    // ---- Single tail ----

    // Firefly cap at the shared write site: covers confident-hit, partial and
    // sky-landing radiance with the same curve and mirror gate as the HW tier's write
    // site, so tile-class flips never step the dynamic range. This tier derives
    // roughFactor with HR_RoughFactor, as HW does.
    if (roughness > mirrorRoughness)
        outColor.rgb = HR_ClampTracedRadiance(outColor.rgb,
                                              HR_RoughFactor(roughness));

    // NaN scrub before every radiance write.
    if (any(isnan(outColor)))
        outColor = float4(0.0, 0.0, 0.0, 0.0);

    if (doWrite)
    {
        OutRawColor[pixel] = outColor;
        OutHitDist[pixel]  = outDist;
    }

    // Hit-counter feedback for next frame's classifier (hybrid only). Per-thread
    // atomics, not wave-aggregated: ray-list waves can straddle 8x8 tiles.
    if (and(and(rtOn, (unifiedFlags & 1u) != 0u), counterFlag != 0u))
    {
        InterlockedAdd(OutHitCounter[pixel >> 3], counterFlag);
    }

    // Wave-aggregated HW ray-list append (AMD HSR deferred-raytracing
    // scheme). Lanes that returned earlier are simply inactive here.
    uint hwCount = WaveActiveCountBits(demote);
    uint hwScan  = WavePrefixCountBits(demote);
    uint hwBase  = 0u;
    if (WaveIsFirstLane() && hwCount != 0u)
    {
        InterlockedAdd(g_RayCounter[HR_CTR_HW_COUNT], hwCount, hwBase);
    }
    hwBase = WaveReadLaneFirst(hwBase);
    if (demote)
    {
        g_HWRayList[hwBase + hwScan] = HR_PackRay(pixel);
    }
}
