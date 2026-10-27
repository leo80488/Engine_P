// SSRTrace.cs.hlsl - stochastic GGX ray generation + FidelityFX SSSR Hi-Z traversal (pass 2 of the
// Hi-Z SSR pipeline; traversal math from Wicked Engine's ssr_raytraceCS.hlsl, a FidelityFX SSSR
// port). Reverse-Z (z_ndc 1 = near, 0 = far); the pyramid's .r = max(z) = NEAREST surface in a tile.
// Outputs (render resolution):
//   u0 hitBuffer  RGBA16F  .rgb = scene colour at the hit, .a = confidence in [0,1] (miss = 0)
//   u1 rayDirPDF  RGBA16F  .rgb = world-space reflection direction L, .a = PDF of the sampled H
//   u2 rayLength  R16F     world-space distance from P to the hit (0 on miss)
//   u3 rayClass   R8_UINT  geometry / sky / edge miss / thickness miss
// Scene colour is sampled here rather than in resolve (as Wicked does): resolve reweights
// already-sampled neighbour hits, so each pixel pays one pyramid fetch instead of nine.

cbuffer SSRCB : register(b0, space2)
{
    float4x4 viewProj;            // jittered - must match depth rasterization
    float4x4 invViewProj;         // jittered inverse
    float3   cameraPos;           float  nearZ;
    // renderW/H is the full-res G-buffer size, traceW/H the dispatch size (equal: SSR traces at full res).
    uint     renderW;             uint   renderH;
    uint     hizMipCount;         float  maxRayLength;
    float    farZ;                float  roughnessCutoff;
    uint     frameIndex;          uint   _pad1;
    // Tunables, set at runtime by SSRPass:
    //   traceThickness     - linear-Z tolerance for hit validation (UE uses 0.02-0.05 wu; Wicked's 1.5
    //                        accepts thin-surface tunnelling).
    //   hizMostDetailedLvl - finest mip the walker visits; 0 = per-pixel stepping (expensive on long
    //                        rays). UE starts at 1 or 2.
    //   coneMipMax         - upper bound of PickConeMip; higher = blurrier radiance on rough surfaces.
    //   depthBiasFactor    - fraction of the pixel's linear depth the ray origin moves toward the
    //                        camera. Without it the origin sits on the surface, the first cell's max
    //                        depth >= origin.z and the walker 'hits' at the origin, sampling the
    //                        reflecting plane's own colour (objects touching the plane vanish from the
    //                        reflection). Typical 0.002-0.01.
    //   finishLinearSteps  - pixel steps of the Lumen-style linear finish trace. Hi-Z stops at a cell
    //                        corner, not the hit pixel (often sky at mesh-vs-sky silhouettes); the
    //                        finish walks one pixel at a time until ray z crosses surface z. 16 is
    //                        generous; UE uses ~8.
    float    traceThickness;      uint   hizMostDetailedLvl;
    float    coneMipMax;          float  depthBiasFactor;
    uint     finishLinearSteps;   uint   traceW;
    uint     traceH;              uint   _pad2;
    uint     skyRadianceMips;     uint   hasSkyRadiance;
    float    roughFadeStart;      float  _pad3;
};

Texture2D<float4>   gNormal    : register(t0, space2);
Texture2D<float4>   gSurface   : register(t1, space2);
Texture2D<float>    gDepth     : register(t2, space2);
Texture2D<float2>   gDepthHier : register(t3, space2);   // 2-channel pyramid; .r = max = nearest
Texture2D<float4>   gHdrPyramid: register(t4, space2);   // pre-filtered HDR mip chain
TextureCube<float4> gSkyRadiance: register(t5, space2);  // direct sky hits
Texture2D<float2>   gVelocity  : register(t7, space2);   // NDC delta

RWTexture2D<float4> OutHit        : register(u0, space2);
RWTexture2D<float4> OutRayDirPDF  : register(u1, space2);
RWTexture2D<float>  OutRayLength  : register(u2, space2);
RWTexture2D<uint>   OutRayClass   : register(u3, space2);

SamplerState gLinClamp : register(s0, space2);

#include "indirect_specular.hlsli"

#include "core/math.hlsli"   // PI
static const float blendScreenEdgeFade     = 5.0;   // vignette sharpness
static const uint  HiZTraceIterationsMax   = 256;

// ==== Math helpers ====
#include "core/depth.hlsli"   // LinearizeReverseZ, WorldPosFromDepth
float3 ReconstructWorldPos(float2 uv, float depth)
{
    return WorldPosFromDepth(uv, depth, invViewProj);
}

float LinearizeReverseZ(float zNdc)
{
    return LinearizeReverseZ(zNdc, nearZ, farZ);
}

// Inverse of LinearizeReverseZ: eye-space Z -> reverse-Z NDC. Turns a linear-space depth offset
// back into the NDC start position of the Hi-Z march.
float InverseLinearDepth(float linZ)
{
    return (nearZ * farZ / max(linZ, 1e-4) - nearZ) / max(farZ - nearZ, 1e-4);
}

#include "core/hash.hlsli"

#include "core/sampling.hlsli"   // TangentFrameRefZ, Hammersley2D

// Hammersley with per-pixel Cranley-Patterson rotation. Decorrelates
// neighbouring pixels without giving up the base sequence's low discrepancy.
float2 SampleHammersley(uint2 pixel, uint frame, uint regen)
{
    uint  sampleIdx = (frame * 15u + regen) & 63u;
    float2 Xi = Hammersley2D(sampleIdx, 64u);

    uint h = HashWang((pixel.x + pixel.y * renderW) ^ (regen * 0x9E3779B9u));
    float2 offset = float2(h & 0xFFFFu, (h >> 16) & 0xFFFFu) * (1.0 / 65536.0);
    return frac(Xi + offset);
}

// Standard GGX importance sample -> tangent-space H (normal = +Z), pdf in .w.
float4 ImportanceSampleGGX(float2 Xi, float roughness)
{
    float a = roughness * roughness;
    float phi = 2.0 * PI * Xi.x;
    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    float3 H = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);

    // PDF of H: D(H) * cos(theta_h) (NDF sampling, not VNDF).
    float D = (a * a) /
              (PI * pow((cosTheta * cosTheta) * (a * a - 1.0) + 1.0, 2.0));
    float pdf = D * cosTheta;
    return float4(H, pdf);
}

float3x3 GetTangentBasis(float3 N)
{
    float3 T, B;
    TangentFrameRefZ(N, T, B);
    return float3x3(T, B, N);
}

// ==== FidelityFX SSSR Hi-Z traversal (from Wicked's ssr_raytraceCS.hlsl) ====
float2 GetMipResolution(float2 screenDim, int mipLevel)
{
    return screenDim * pow(0.5, mipLevel);
}

void InitialAdvanceRay(
    float3 origin, float3 direction,
    float2 currentMipResolution, float2 currentMipResolutionRcp,
    float2 floorOffset, float2 uvOffset,
    out float3 position, out float tCurrent)
{
    float2 currentMipPosition = currentMipResolution * origin.xy;

    // Intersect ray with the half box pointing away from origin.
    float2 xyPlane = floor(currentMipPosition) + floorOffset;
    xyPlane = xyPlane * currentMipResolutionRcp + uvOffset;

    float2 t = (xyPlane - origin.xy) / direction.xy;
    tCurrent = min(t.x, t.y);
    position = origin + tCurrent * direction;
}

bool AdvanceRay(
    float3 origin, float3 direction,
    float2 currentMipPosition, float2 currentMipResolutionRcp,
    float2 floorOffset, float2 uvOffset,
    float surfaceZ,
    inout float3 position, inout float tCurrent)
{
    float2 xyPlane = floor(currentMipPosition) + floorOffset;
    xyPlane = xyPlane * currentMipResolutionRcp + uvOffset;
    float3 boundaryPlanes = float3(xyPlane, surfaceZ);

    float3 t = (boundaryPlanes - origin) / direction;

    // Use the z-plane only when marching toward the far plane: under reverse-Z, direction.z < 0 goes
    // near -> far (the ray can hit surfaces ahead).
    t.z = direction.z < 0 ? t.z : 1e30;

    float tMin = min(min(t.x, t.y), t.z);

    // Reverse-Z: "larger z means closer to camera", so position is ABOVE the
    // surface when position.z > surfaceZ.
    bool aboveSurface = surfaceZ < position.z;

    // asuint bit equality detects a step that stopped exactly on the z-plane (NaN-safe).
    bool skippedTile = asuint(tMin) != asuint(t.z) && aboveSurface;

    tCurrent = aboveSurface ? tMin : tCurrent;
    position = origin + tCurrent * direction;

    return skippedTile;
}

// Hierarchical ray march in screen space [0,1]^2 x [0,1].
float3 HierarchicalRaymarch(
    float3 origin, float3 direction, float2 screenDim,
    out uint rayClass)
{
    int mostDetailed = (int)hizMostDetailedLvl;
    int currentMip   = mostDetailed;

    float2 currentMipResolution     = GetMipResolution(screenDim, currentMip);
    float2 currentMipResolution_rcp = rcp(currentMipResolution);

    // Slight overshoot so the ray consistently crosses boundaries without
    // getting trapped on shared edges (Wicked's uvOffset trick).
    float2 uvOffset = 0.005 * exp2((float)mostDetailed) / screenDim;
    // select(): vector conditions (HLSL 2021's ?: needs a scalar one).
    uvOffset = select(direction.xy < 0, -uvOffset, uvOffset);

    // Which corner of the cell the ray exits through.
    float2 floorOffset = (float2)select(direction.xy < 0, int2(0, 0), int2(1, 1));

    float  tCurrent;
    float3 position;
    InitialAdvanceRay(origin, direction, currentMipResolution,
                      currentMipResolution_rcp, floorOffset, uvOffset,
                      position, tCurrent);

    int i = 0;
    int maxMip = (int)hizMipCount - 1;
    [loop]
    while (i < (int)HiZTraceIterationsMax && currentMip >= mostDetailed)
    {
        if (any(position.xy < 0.0) || any(position.xy > 1.0))
        {
            rayClass = SSR_MISS_EDGE;
            return position;
        }

        float2 currentMipPosition = currentMipResolution * position.xy;
        // Reverse-Z: .r = max depth = nearest surface in the tile.
        // CLAMP the load coordinate: currentMipResolution is the fractional renderDim/2^mip, but mip m
        // has renderDim >> m texels, so for non-pow2 sizes floor(uv * fracRes) can index one past the last
        // texel (~17% of the screen at mip 8 for a 1237-wide view). An out-of-bounds load returns 0 = FAR
        // plane under reverse-Z and the walker tunnels through geometry in whole cells. The clamp pairs
        // with the reduce's odd-dimension remainder fold (SSRDepthHierarchy.cs.hlsl): the last texel of
        // each mip also covers the orphan row/column, so the clamped load stays a conservative bound. The
        // cell-boundary math stays on the fractional grid, matching the reduce's 2^m-pixel tiles.
        int2   mipDims = int2(max(renderW >> uint(currentMip), 1u),
                              max(renderH >> uint(currentMip), 1u));
        int2   loadPx  = min(int2(currentMipPosition), mipDims - 1);
        float  surfaceZ = gDepthHier.Load(int3(loadPx, currentMip)).r;

        bool skippedTile = AdvanceRay(origin, direction,
                                      currentMipPosition, currentMipResolution_rcp,
                                      floorOffset, uvOffset,
                                      surfaceZ, position, tCurrent);

        currentMip         += skippedTile ? 1 : -1;
        currentMip          = min(currentMip, maxMip);
        currentMipResolution *= skippedTile ? 0.5 : 2.0;
        currentMipResolution_rcp *= skippedTile ? 2.0 : 0.5;

        i++;
    }

    rayClass = (i < (int)HiZTraceIterationsMax)
             ? SSR_HIT_GEOMETRY : SSR_MISS_THICKNESS;
    return position;
}

// Perspective-correct NDC z of the world ray at a screen UV. The ray P(s) = worldPos + s * L maps
// to clip(s) = startClip + s * dirClip; back-solve s from uv on the axis with the larger
// denominator (numerically stable), then z = clip(s).z / clip(s).w. The walker's linear cur.z
// underestimates this (reverse-Z NDC z is convex in t), so it crosses the surface early and
// reports a hit too close to the start; the error grows with distance and shows as the reflection
// sliding while the camera pulls away.
float EvalRayNdcZ(float2 uv, float4 sClip, float4 dClip, out bool valid)
{
    float ndcX = uv.x * 2.0 - 1.0;
    float ndcY = 1.0 - uv.y * 2.0;
    float denX = ndcX * dClip.w - dClip.x;
    float denY = ndcY * dClip.w - dClip.y;
    float s = (abs(denX) > abs(denY))
              ? (sClip.x - ndcX * sClip.w) / denX
              : (sClip.y - ndcY * sClip.w) / denY;

    // Only the s >= 0, clipW > 0 part of the projected line is in front of the camera. Past the
    // on-screen vanishing point (L strongly forward) the back-solve gives a phantom point behind the
    // camera whose depth the walker would always accept (realZ <= sZ), clustering every floor
    // reflection onto one region. The sentinel >> 1 makes the walker treat the pixel as unreachable,
    // keep going, and exit off-screen to the IBL miss path.
    float clipW = sClip.w + s * dClip.w;
    if (s < 0.0 || clipW < 1e-3) { valid = false; return 1e9; }

    float clipZ = sClip.z + s * dClip.z;
    float z = clipZ / clipW;
    // Reverse-Z: z <= 0 is at or beyond the far plane (a phantom point). A negative realZ satisfies
    // realZ <= sZ against any surface and fakes a crossing, so flag it unreachable too; the finish
    // trace skips invalid samples.
    valid = (z > 0.0);
    return valid ? z : 1e9;
}

// Linear finish trace after Hi-Z. HierarchicalRaymarch skips empty tiles and lands within ~1 cell
// of the crossing (given a correct pyramid: zero-depth mips make aboveSurface always true and Hi-Z
// walks off-screen); this refines it with pixel-precise backward + forward walks on the
// perspective-correct realZ. Returns false, with outHit = hzPosition and outSurfZ its surface
// depth, when the step budget runs out.
bool LinearFinishTrace(float3 hzPosition, float3 origin, float3 direction,
                       float4 sClip, float4 dClip,
                       float2 screenDim, uint maxSteps,
                       out float3 outHit, out float outSurfZ)
{
    float2 dirPxLen = abs(direction.xy) * screenDim;
    float  pxLen    = max(max(dirPxLen.x, dirPxLen.y), 1e-6);
    float  dt       = 1.0 / pxLen;       // 1 t-unit = 1 dominant-axis pixel
    float3 step     = direction * dt;

    float3 cur = hzPosition;
    int2   ssize = int2(screenDim);

    // 1. Back up while below the surface (real z <= surface z) or on sky, so cur lands strictly
    //    ABOVE a real surface and the forward walk finds a clean above -> below crossing.
    [loop]
    for (uint b = 0; b < maxSteps / 2u; ++b)
    {
        int2  px    = clamp(int2(cur.xy * screenDim), int2(0, 0), ssize - 1);
        float sZ    = gDepth.Load(int3(px, 0));
        bool  vUp; float realZ = EvalRayNdcZ(cur.xy, sClip, dClip, vUp);
        // Stop only on a VALID (in front of the camera) sample above a real surface. If the Hi-Z landing
        // is past the on-screen vanishing point (grazing far-water rays), keep backing toward the origin,
        // which is always valid.
        if (vUp && sZ > 0.0 && realZ > sZ) { cur.z = realZ; break; }
        cur -= step;
        if (any(cur.xy < 0.0) || any(cur.xy > 1.0)) { cur += step; break; }
    }

    // 2. Walk forward one pixel at a time until the real ray z crosses the surface, then interpolate
    //    between the previous (above) and current (below) sample for the sub-pixel hit UV. Without it,
    //    adjacent pixels whose rays differ by < 1 pixel snap to integer hit pixels: staircase artefacts
    //    on slanted reflected surfaces, worst at high-contrast edges.
    float3 prev    = cur;
    bool   prevValid; float prevRZ = EvalRayNdcZ(cur.xy, sClip, dClip, prevValid);

    [loop]
    for (uint f = 0; f < maxSteps; ++f)
    {
        if (any(cur.xy < 0.0) || any(cur.xy > 1.0)) break;
        int2  px    = clamp(int2(cur.xy * screenDim), int2(0, 0), ssize - 1);
        float sZ    = gDepth.Load(int3(px, 0));
        bool  valid; float realZ = EvalRayNdcZ(cur.xy, sClip, dClip, valid);

        if (valid && sZ > 0.0 && realZ <= sZ)
        {
            // Sub-pixel crossing: solve realZ(t) = sZ between prev and cur, assuming sZ is constant over the
            // pixel and realZ linear between steps. Range-check t, do NOT saturate: at a depth discontinuity
            // (silhouette of a foreground object above the receiver) prev and cur realZ are nearly equal, the
            // solved crossing lies outside [prev, cur], and it is a fake hit - the 3D ray missed the
            // foreground. Skip it and keep marching: the ray crosses the foreground further on or exits.
            float denom = prevRZ - realZ;
            float t_raw = (abs(denom) > 1e-6) ? (prevRZ - sZ) / denom : 1.0;
            if (prevValid && t_raw >= -0.01 && t_raw <= 1.01)
            {
                float t   = saturate(t_raw);
                outSurfZ  = sZ;
                outHit.xy = lerp(prev.xy, cur.xy, t);
                outHit.z  = lerp(prevRZ,  realZ,  t);
                return true;
            }
            // Discontinuity false crossing: fall through to keep marching.
        }

        prev      = cur;
        prevRZ    = realZ;
        prevValid = valid;
        cur      += step;
    }

    // Budget exhausted: return the Hi-Z position with its own surface depth; the caller's ValidateHit
    // decides by thickness.
    int2 px = clamp(int2(hzPosition.xy * screenDim), int2(0, 0), ssize - 1);
    outSurfZ = gDepth.Load(int3(px, 0));
    outHit   = hzPosition;
    return false;
}

// ==== Hit validation (Wicked's pattern: edge vignette x depth thickness) ====
float CalculateEdgeVignette(float2 hitUV)
{
    float2 hitNDC = hitUV * 2.0 - 1.0;
    float2 vignette = saturate(abs(hitNDC) * blendScreenEdgeFade -
                               (blendScreenEdgeFade - 1.0));
    return saturate(1.0 - dot(vignette, vignette));
}

// Ray-end depth (hit.z) vs the surface depth at the same UV: reject a ray that passes THROUGH a
// thin surface by too much.
float ValidateHit(float3 hit, float surfaceDepthAtHit, float2 prevHitUV)
{
    float vignette     = CalculateEdgeVignette(hit.xy);
    float vignettePrev = CalculateEdgeVignette(prevHitUV);
    vignette = min(vignette, vignettePrev);

    // Depth delta -> linear thickness. traceThickness comes from the CB (UE ships 0.02-0.05 wu).
    float linRay = LinearizeReverseZ(hit.z);
    float linSrf = LinearizeReverseZ(surfaceDepthAtHit);
    float thickness = abs(linRay - linSrf);
    // The tolerance scales with hit distance. Only rays whose finish trace did not converge have a
    // non-trivial thickness (clean crossings land at ~0), and at range the uncertainty grows: one Hi-Z
    // cell on grazing terrain spans several wu of depth. A fixed 0.5 wu rejects nearly every far
    // fall-through hit (sky speckles inside distant water reflections); 0.4% of hit depth (2 wu at
    // 500 m) accepts a marginally-off colour instead, which reads far better than a speckle.
    float tol = max(max(traceThickness, 1e-4), linSrf * 0.004);
    float confidence = 1.0 - smoothstep(0.0, tol, thickness);

    return vignette * confidence;
}

// Cone-footprint mip of the scene colour pyramid (roughness x ray length), capped by coneMipMax. A
// high cap blurs rough reflections but bleeds whatever surrounds small reflected objects (usually
// sky) into the sample; like UE's stochastic SSR, leave most low-pass work to spatial + temporal.
// 3-4 is a reasonable cap.
float PickConeMip(float roughness, float rayLen, float hitLinZ)
{
    float coneRadius  = roughness * roughness * rayLen;
    float pixelRadius = coneRadius * float(renderH) * 0.5 / max(hitLinZ, 0.1);
    return clamp(log2(max(pixelRadius, 1.0)), 0.0, coneMipMax);
}

#include "core/color.hlsli"   // Luminance

// Trace ONE reflection ray for pixel along world-space L. Returns true with hitColor, confidence
// and rayLen on a validated geometry hit or a sky landing (rayLen = maxRayLength), false on any miss
// or reject, so one bad sample just drops out of the multi-sample average; rayClass reports the
// outcome. Steps: projection -> Hi-Z walk -> linear finish -> validate -> cone-mip fetch.
bool TraceOneReflection(uint2 pixel, float3 worldPos, float depth,
                        float roughness, float3 L,
                        out float3 hitColor, out float confidence, out float rayLen,
                        out uint rayClass)
{
    hitColor   = float3(0, 0, 0);
    confidence = 0.0;
    rayLen     = 0.0;
    rayClass   = SSR_MISS_EDGE;

    float4 startClip = mul(float4(worldPos,     1.0), viewProj);
    float4 endClip   = mul(float4(worldPos + L, 1.0), viewProj);
    if (startClip.w <= 1e-3) return false;        // origin behind near plane: bail

    float3 startSS;
    {
        float3 startNDC = startClip.xyz / startClip.w;
        startSS.xy      = startNDC.xy * float2(0.5, -0.5) + 0.5;
        startSS.z       = startNDC.z;

        float linZ       = LinearizeReverseZ(startSS.z);
        // The push toward the camera must beat the depth buffer's local quantization step, or the first
        // Hi-Z cell still reads >= origin.z, the walk hits at the origin, fails minRayLen and the ray drops
        // (sky speckles on distant water). quantStep models the D24_UNORM worst case
        // (linZ^2 * (far - near) / (near * far * 2^24), ~0.5 wu at 900 m with near 0.1 / far 1000). Main
        // depth is D32_FLOAT_S8X24, far below that bound, so this is a conservative margin that survives a
        // format change. Near the camera the 0.02 wu floor rules, keeping contact lines tight.
        float quantStep  = linZ * linZ * (farZ - nearZ)
                         / max(nearZ * farZ * 16777216.0, 1e-4);
        float biasAbs    = max(linZ * depthBiasFactor,
                               max(0.02, quantStep * 2.0));
        float linZBiased = max(linZ - biasAbs, nearZ);
        startSS.z        = saturate(InverseLinearDepth(linZBiased));
    }

    const float kClipW = 1e-3;
    if (endClip.w <= kClipW)
    {
        float denom = endClip.w - startClip.w;
        float t     = saturate((kClipW - startClip.w) / denom);
        endClip     = lerp(startClip, endClip, t);
    }

    float3 endSS = endClip.xyz / endClip.w;
    endSS.xy = endSS.xy * float2(0.5, -0.5) + 0.5;

    float3 rayDirSS = endSS - startSS;

    {
        float2 invDir = rcp(abs(rayDirSS.xy) + 1e-6);
        float2 tEdge  = select(rayDirSS.xy > 0, 1.0 - startSS.xy, startSS.xy) * invDir;
        float  tMax   = max(min(tEdge.x, tEdge.y), 1.0);
        rayDirSS     *= tMax;
    }

    uint traceClass = SSR_MISS_EDGE;
    float3 hit = HierarchicalRaymarch(startSS, rayDirSS,
                                      float2(renderW, renderH), traceClass);
    rayClass = traceClass;
    if (traceClass != SSR_HIT_GEOMETRY) return false;

    float4 dirClip = mul(float4(L, 0.0), viewProj);
    float  surfaceDepthAtHit;
    bool   finishOk = LinearFinishTrace(hit, startSS, rayDirSS,
                                        startClip, dirClip,
                                        float2(renderW, renderH),
                                        max(finishLinearSteps, 1u),
                                        hit, surfaceDepthAtHit);

    if (surfaceDepthAtHit <= 0.0)
    {
        rayClass = SSR_HIT_SKY;
        if (hasSkyRadiance == 0u) return false;
        const float skyMip = roughness * float(max(skyRadianceMips, 1u) - 1u);
        hitColor = gSkyRadiance.SampleLevel(gLinClamp, L, skyMip).rgb;
        confidence = IndirectSpecular_RoughFade(
            roughness, roughFadeStart, roughnessCutoff);
        if (confidence <= 0.0)
        {
            rayClass = SSR_MISS_THICKNESS;
            return false;
        }
        rayLen = maxRayLength;
        return true;
    }

    confidence = ValidateHit(hit, surfaceDepthAtHit, startSS.xy);
    rayClass = (CalculateEdgeVignette(hit.xy) <= 0.0)
             ? SSR_MISS_EDGE : SSR_MISS_THICKNESS;

    {
        int2 hitPxFull = clamp(int2(hit.xy * float2(renderW, renderH)),
                               int2(0, 0), int2(renderW - 1, renderH - 1));
        float3 hitN  = normalize(gNormal.Load(int3(hitPxFull, 0)).rgb * 2.0 - 1.0);
        // Backface rejection that keeps GRAZING hits: a near-horizontal water/floor ray skimming onto
        // upward-facing ground has front ~ 0, often slightly negative on flat shores, and rejecting it
        // leaves a bright sky band along every shoreline / wall-floor contact. Reject only clearly
        // back-facing hits (a ray emerging from inside geometry); ValidateHit's thickness check still
        // catches pass-through leaks.
        float  front = dot(hitN, -L);
        confidence  *= smoothstep(-0.25, -0.05, front);
    }
    if (confidence <= 0.0) return false;

    float3 hitWS     = ReconstructWorldPos(hit.xy, surfaceDepthAtHit);
    rayLen           = length(hitWS - worldPos);
    float  hitLinZ   = LinearizeReverseZ(surfaceDepthAtHit);
    float  startLinZ = LinearizeReverseZ(depth);
    float  minRayLen = max(startLinZ * 0.005, 0.01);
    if (rayLen < minRayLen) return false;         // self-intersection

    float  mipLvl   = (roughness < 0.1)
                      ? 1.0
                      : PickConeMip(roughness, rayLen, hitLinZ);
    hitColor = gHdrPyramid.SampleLevel(gLinClamp, hit.xy, mipLvl).rgb;
    confidence *= IndirectSpecular_RoughFade(
        roughness, roughFadeStart, roughnessCutoff);
    if (confidence <= 0.0)
    {
        rayClass = SSR_MISS_THICKNESS;
        return false;
    }
    rayClass = SSR_HIT_GEOMETRY;
    return true;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    // Full-res dispatch: one thread per G-buffer pixel.
    if (DTid.x >= traceW || DTid.y >= traceH) return;

    const uint2  pixel = DTid.xy;
    const float2 uv    = (float2(pixel) + 0.5) / float2(renderW, renderH);

    // Write miss by default; early-returns below leave these values.
    OutHit[pixel]       = 0;
    OutRayDirPDF[pixel] = 0;
    OutRayLength[pixel] = 0;
    OutRayClass[pixel]  = SSR_MISS_EDGE;

    float depth = gDepth.Load(int3(pixel, 0));
    if (depth <= 0.0) return;                     // sky

    float4 surf = gSurface.Load(int3(pixel, 0));
    float  roughness = max(surf.r, 0.045);
    if (roughness > roughnessCutoff)
    {
        OutRayClass[pixel] = SSR_MISS_THICKNESS;
        return;
    }

    float3 N = normalize(gNormal.Load(int3(pixel, 0)).rgb * 2.0 - 1.0);
    float3 worldPos = ReconstructWorldPos(uv, depth);
    float3 V = normalize(cameraPos - worldPos);

    // ---- Multi-sample reflection ----
    // Average kTraceSamples independent rays per pixel per frame. One stochastic GGX ray is too noisy
    // for the denoisers on curved / low-coverage glossy surfaces (a basin, a sphere): neighbours
    // diverge, so the resolve finds few valid taps, and the temporal pass keeps rejecting history.
    // N rays cut the per-frame variance ~1/sqrt(N) at the source, so SSR does not lean on TAA; the
    // cost is N x the Hi-Z walk. Mirror surfaces (roughness <= kMirrorThreshold) carry no stochastic
    // noise and take one sample.
    const float kMirrorThreshold = 0.05;
    const uint  kMaxRegen        = 15u;
    const uint  kTraceSamples    = 4u;

    const bool isMirror = (roughness <= kMirrorThreshold);
    const uint nSamples = isMirror ? 1u : kTraceSamples;
    float3x3   TBN      = GetTangentBasis(N);   // glossy GGX basis (hoisted)

    float3 accumColor = float3(0, 0, 0);   // sum of Karis(color) * confidence
    float  accumW     = 0.0;               // sum of confidence over VALID hits
    float  sumConf    = 0.0;               // sum of confidence over ALL attempts (miss = 0)
    float  bestRayLen = 1e9;
    float3 bestL      = reflect(-V, N);
    float  bestPdf    = 1.0;
    uint   nValid     = 0u;
    uint   nGeometry  = 0u;
    uint   nSky       = 0u;
    uint   nEdgeMiss  = 0u;
    uint   nThickMiss = 0u;

    [loop]
    for (uint s = 0; s < nSamples; ++s)
    {
        // ---- Pick this sample's reflection direction ----
        float3 L; float pdf; float RdotN = 0.0;
        if (isMirror)
        {
            L = reflect(-V, N); pdf = 1.0; RdotN = dot(N, L);
            if (RdotN <= 0.01) break;
        }
        else
        {
            [loop]
            for (uint r = 0; r < kMaxRegen && RdotN <= 0.01; ++r)
            {
                // Fold the sample index s into the regen counter so each sample draws a distinct Hammersley
                // point (SampleHammersley also decorrelates per pixel and per frame).
                float2 Xi   = SampleHammersley(pixel, frameIndex, s * kMaxRegen + r);
                float4 Hpdf = ImportanceSampleGGX(Xi, roughness);
                float3 H    = mul(Hpdf.xyz, TBN);
                L           = reflect(-V, H);
                pdf         = Hpdf.w;
                RdotN       = dot(N, L);
            }
            if (RdotN <= 0.01) continue;       // no valid ray this sample
        }

        // ---- Trace it ----
        float3 hc; float conf; float rl; uint rayClass;
        const bool valid = TraceOneReflection(
            pixel, worldPos, depth, roughness, L, hc, conf, rl, rayClass);
        nGeometry  += (rayClass == SSR_HIT_GEOMETRY)   ? 1u : 0u;
        nSky       += (rayClass == SSR_HIT_SKY)        ? 1u : 0u;
        nEdgeMiss  += (rayClass == SSR_MISS_EDGE)      ? 1u : 0u;
        nThickMiss += (rayClass == SSR_MISS_THICKNESS) ? 1u : 0u;
        if (!valid)
            continue;

        // Karis-weighted, confidence-weighted accumulate (firefly-robust mean,
        // matching the resolve so one sky hit can't swamp the average).
        float Yin = max(Luminance(hc), 0.0);
        accumColor += hc * rcp(1.0 + Yin) * conf;
        accumW     += conf;
        sumConf    += conf;
        nValid++;
        if (rl < bestRayLen) { bestRayLen = rl; bestL = L; bestPdf = pdf; }
    }

    OutRayClass[pixel] = (nGeometry > 0u) ? SSR_HIT_GEOMETRY
                       : (nSky > 0u)      ? SSR_HIT_SKY
                       : (nThickMiss > nEdgeMiss)
                           ? SSR_MISS_THICKNESS : SSR_MISS_EDGE;

    if (nValid == 0u || accumW <= 1e-6) return;

    // Karis inverse-expand the weighted mean back to HDR.
    float3 meanColor = accumColor / accumW;
    float  Yavg      = saturate(Luminance(meanColor));
    meanColor       *= rcp(max(1.0 - Yavg, 1e-3));

    // Confidence averaged over EVERY attempted sample (misses score 0) so a
    // pixel whose lobe partly leaves the screen fades rather than pops.
    float outConf = sumConf / float(nSamples);

    OutHit[pixel]       = float4(meanColor, outConf);
    // Representative ray = nearest valid hit, for the resolve's parallax
    // reconstruction + the temporal reflection-reprojection.
    OutRayDirPDF[pixel] = float4(bestL, bestPdf);
    OutRayLength[pixel] = bestRayLen;
}
