// SSGITrace.cs.hlsl - screen-space diffuse GI trace + NRD input packing. Half-res dispatch,
// 8x8 threads, one trace pixel each. Trace and pack are fused: the pack needs exactly the surface
// data the trace already unpacked, and a second dispatch would re-read the G-buffer.
//
// ==== The estimator ====
// Cosine-sampled irradiance is E = (PI/N) * SUM_i L_i. Rays the screen march cannot resolve
// (off-screen, iteration-exhausted, ambiguous depth) are filled by the far field. With a continuous
// validity c_i in [0,1] per ray (screen hit x history agreement):
//
//     E ~= [ (PI/N) * SUM_i c_i L_i ]  +  (1 - SUM_i c_i/N) * E_far
//          \_________ .rgb _________/     \_______ 1-.a _______/
//
// The near term goes to OutNrdDiffIn.rgb (stored as E/PI, see the units note in main) and the
// resolved fraction to OutRawCoverage; Lighting.ps closes the identity with
// ssgi.rgb + farDiffuse * (1 - a). Two consequences carry the safety argument:
//  1. SUM c_i == 0 -> .rgb = 0 and .a = 0 -> Lighting.ps reproduces the frame without SSGI
//     bit-identically (the kill switch).
//  2. The two tiers own disjoint solid angles: nothing is counted twice, by construction.
//
// ==== Why .a is a filterable coverage ====
// SUM c_i/N averages soft depth/facing/history tests over N directions, so SSGIResolve may filter
// it bilaterally (depth + normal weights): it estimates how much of the local hemisphere the
// screen/history resolves. Filtering a hard per-ray radiance decision would not be justified.
// One ray alone has too much angular variance: the gather path runs with rayCount >= 2 (SSGIPass
// clamps it); the reservoir path traces one fresh ray because temporal + spatial reuse turn it
// into a multi-candidate estimate.
//
// ==== Screen-hit validation ====
// An uncertain march result counts as a miss (Lumen's bHit && !bUncertain): a false diffuse hit
// injects wrong energy over a whole hemisphere. Sky landings also count as unresolved: sampling
// the prefiltered sky here lets single ~2000x sun-disk samples through for REBLUR to smear into
// glowing patches. The far-field chain owns the sky term.

#include "SSGICommon.hlsli"

// MUST match how NRD.lib was built (DDGI/RTAO/RTR assert this pairing at runtime):
// NRD_NORMAL_ENCODING 2 = R10_G10_B10_A2_UNORM, ROUGHNESS 1 = linear. Define both BEFORE the
// include: NRD.hlsli has no defaults that would fail loudly when they are missing.
#define NRD_NORMAL_ENCODING    2
#define NRD_ROUGHNESS_ENCODING 1
#include "NRD.hlsli"

Texture2D<float>   gDepth        : register(t1, space0);
Texture2D<float4>  gNormal       : register(t2, space0);
Texture2D<float2>  gVelocity     : register(t3, space0);
Texture2D<float2>  gDepthHier    : register(t4, space0);
Texture2D<float4>  gPrevHDR      : register(t5, space0);
Texture2D<float>   gPrevDepth    : register(t10, space0);
Texture2D<float4>  gReservoirHistoryDirection : register(t11, space0);
Texture2D<float4>  gReservoirHistoryRadiance  : register(t12, space0);
Texture2D<float2>  gReservoirHistoryAux       : register(t13, space0);
Texture2D<uint>    gReservoirHistoryMeta      : register(t14, space0);

StructuredBuffer<DDGIVolumeGPU> gDDGIVolumes                       : register(t8, space0);
Texture2D<float4>               gDDGIIrradiance[DDGI_MAX_VOLUMES]  : register(t0, space4);
Texture2D<float2>               gDDGIDepth     [DDGI_MAX_VOLUMES]  : register(t0, space5);
StructuredBuffer<DDGIProbeData> gDDGIProbeData [DDGI_MAX_VOLUMES]  : register(t0, space6);

RWTexture2D<float>  OutRawCoverage    : register(u0, space0);
RWTexture2D<float4> OutNrdNormalRough : register(u1, space0);
RWTexture2D<float>  OutNrdViewZ       : register(u2, space0);
RWTexture2D<float2> OutNrdMV          : register(u3, space0);
RWTexture2D<float4> OutNrdDiffIn      : register(u4, space0);
RWTexture2D<float4> OutReservoirTemporalDirection : register(u8,  space0);
RWTexture2D<float4> OutReservoirTemporalRadiance  : register(u9,  space0);
RWTexture2D<float2> OutReservoirTemporalAux       : register(u10, space0);
RWTexture2D<uint>   OutReservoirTemporalMeta      : register(u11, space0);

SamplerState gLinearClamp : register(s0, space0);
SamplerState gLinearWrap  : register(s1, space0);

#include "SSGIReservoir.hlsli"

// A hit closer than this many full-res pixels (Manhattan) to the ray origin is a self-hit and
// rejected. maxDistance (the per-ray world cap, default 3 m) is deliberately short: this tier
// supplies the near field below the probe spacing (~3.5 m on the reference scene); marching further
// buys pixels the far field already describes, at more iterations and more wrong hits.
static const float kSelfHitTexels = 2.0;

// Far-field irradiance at P: the fill for the unresolved share of the hemisphere, and the whole
// output in debugMode 1. Use the RAW sampler (no per-volume tint/scale): Lighting.ps applies
// ddgiDiffuseScale to this output already, so the tinted variant would apply it twice.
float3 SSGI_FarFieldIrradiance(float3 P, float3 N, float3 V)
{
    if (ddgiEnabled == 0u || ddgiVolumeCount == 0u) return float3(0, 0, 0);

    DDGIVolumePick pick = DDGI_PickVolumes(P, gDDGIVolumes);
    float4 best = float4(0, 0, 0, 0), second = float4(0, 0, 0, 0);
    [unroll] for (uint vi = 0; vi < DDGI_MAX_VOLUMES; ++vi)
    {
        if (!DDGI_IsPicked(pick, vi)) continue;
        DDGIVolumeGPU vol = gDDGIVolumes[vi];
        const float4 s = DDGI_SampleVolumeRaw(P, N, V, vol,
                                              gDDGIIrradiance[vi], gDDGIDepth[vi],
                                              gDDGIProbeData[vi], gLinearWrap);
        if (vi == (uint)pick.bestSlot) best = s; else second = s;
    }
    const float4 r = DDGI_CombineWinners(best, second, pick);
    // .a is the volume coverage. Outside every volume the far field is the sky term, which
    // Lighting.ps owns and re-adds through (1 - resolvedFraction), so zero is correct here.
    return r.rgb * r.a;
}

// Hi-Z screen march: the cell-boundary ascent/descent scheme of HybridReflIntersectSW (FFX SSSR),
// specialised for a finite diffuse ray.
#define SSGI_FLOAT_MAX 3.402823466e+38
#define SSGI_HIZ_MAX_MIP 6
#define SSGI_FINISH_STEPS 8u

float SSGI_LoadHiZ(float2 mipPosition, int mip)
{
    const uint mipW = max(screenW >> (uint)mip, 1u);
    const uint mipH = max(screenH >> (uint)mip, 1u);
    const int2 p = clamp(int2(mipPosition), int2(0, 0),
                         int2((int)mipW - 1, (int)mipH - 1));
    return gDepthHier.Load(int3(p, mip)).r;
}

void SSGI_InitialAdvance(float3 origin, float3 direction, float3 invDirection,
                         float2 mipResolution, float2 invMipResolution,
                         float2 floorOffset, float2 uvOffset,
                         out float3 position, out float currentT)
{
    const float2 mipPosition = mipResolution * origin.xy;
    float2 xyPlane = floor(mipPosition) + floorOffset;
    xyPlane = xyPlane * invMipResolution + uvOffset;
    const float2 t = xyPlane * invDirection.xy - origin.xy * invDirection.xy;
    currentT = min(t.x, t.y);
    position = origin + currentT * direction;
}

bool SSGI_Advance(float3 origin, float3 direction, float3 invDirection,
                  float2 mipPosition, float2 invMipResolution,
                  float2 floorOffset, float2 uvOffset, float surfaceZ,
                  inout float3 position, inout float currentT)
{
    float2 xyPlane = floor(mipPosition) + floorOffset;
    xyPlane = xyPlane * invMipResolution + uvOffset;
    const float3 planes = float3(xyPlane, surfaceZ);
    float3 t = planes * invDirection - origin * invDirection;
    // Reverse-Z: only a ray moving toward far (decreasing Z) can intersect the
    // conservative nearest-depth plane.
    t.z = select(direction.z < 0.0, t.z, SSGI_FLOAT_MAX);
    const float tMin = min(min(t.x, t.y), t.z);
    const bool aboveSurface = surfaceZ < position.z;
    const bool skippedTile = asuint(tMin) != asuint(t.z) && aboveSurface;
    currentT = select(aboveSurface, tMin, currentT);
    position = origin + currentT * direction;
    return skippedTile;
}

bool SSGI_HierarchicalMarch(float3 origin, float3 direction,
                            out float3 position)
{
    // select(): a vector condition (HLSL 2021's ?: needs a scalar one).
    const float3 invDirection =
        select(direction != float3(0.0, 0.0, 0.0),
               rcp(direction),
               float3(SSGI_FLOAT_MAX, SSGI_FLOAT_MAX, SSGI_FLOAT_MAX));
    int mip = 0;
    float2 mipResolution = float2(screenW, screenH);
    float2 invMipResolution = rcp(mipResolution);
    float2 uvOffset = 0.005 / mipResolution;
    uvOffset.x = select(direction.x < 0.0, -uvOffset.x, uvOffset.x);
    uvOffset.y = select(direction.y < 0.0, -uvOffset.y, uvOffset.y);
    const float2 floorOffset = float2(select(direction.x < 0.0, 0.0, 1.0),
                                      select(direction.y < 0.0, 0.0, 1.0));

    float currentT;
    SSGI_InitialAdvance(origin, direction, invDirection,
                        mipResolution, invMipResolution, floorOffset, uvOffset,
                        position, currentT);
    const int maxMip = clamp((int)hizMipCount - 1, 0, SSGI_HIZ_MAX_MIP);

    [loop] for (uint i = 0u; and(i < maxTraversalIters, mip >= 0); ++i)
    {
        // Unlike reflection's effectively infinite projected ray, this segment
        // ends at maxDistance. Never let cell stepping accept geometry beyond
        // that world-space cap.
        if (or(currentT > 1.0, currentT < 0.0)) return false;
        if (or(or(any(position.xy < 0.0), any(position.xy > 1.0)), position.z <= 0.0))
            return false;

        const float2 mipPosition = mipResolution * position.xy;
        const float surfaceZ = SSGI_LoadHiZ(mipPosition, mip);
        const bool skipped = SSGI_Advance(origin, direction, invDirection,
                                          mipPosition, invMipResolution,
                                          floorOffset, uvOffset, surfaceZ,
                                          position, currentT);
        if (currentT > 1.0) return false;

        const bool atTop = and(skipped, mip >= maxMip);
        if (!atTop)
        {
            mip += select(skipped, 1, -1);
            mipResolution     *= select(skipped, 0.5, 2.0);
            invMipResolution *= select(skipped, 2.0, 0.5);
        }
    }
    return mip < 0;
}

// Perspective-correct ray NDC depth and finite world-ray parameter at a UV.
// The Hi-Z walker uses a linear float3 for speed; validation must not.
float SSGI_EvalRayAtUV(float2 uv, float4 startClip, float4 dirClip,
                       out float rayParam, out bool valid)
{
    const float ndcX = uv.x * 2.0 - 1.0;
    const float ndcY = 1.0 - uv.y * 2.0;
    const float denX = ndcX * dirClip.w - dirClip.x;
    const float denY = ndcY * dirClip.w - dirClip.y;
    const bool useX = abs(denX) > abs(denY);
    const float den = select(useX, denX, denY);
    if (abs(den) < 1e-8)
    {
        rayParam = 0.0;
        valid = false;
        return SSGI_FLOAT_MAX;
    }
    rayParam = select(useX, (startClip.x - ndcX * startClip.w) / den,
                            (startClip.y - ndcY * startClip.w) / den);
    const float clipW = startClip.w + rayParam * dirClip.w;
    valid = rayParam >= 0.0 && rayParam <= 1.0 && clipW > 1e-4;
    const float z = select(valid, (startClip.z + rayParam * dirClip.z) / clipW, 0.0);
    valid = valid && z > 0.0;
    return select(valid, z, SSGI_FLOAT_MAX);
}

bool SSGI_FinishTrace(float3 coarse, float3 screenDir,
                      float4 startClip, float4 dirClip,
                      out float2 refinedUV, out float rayZ,
                      out float rayParam)
{
    refinedUV = coarse.xy;
    rayZ = 0.0;
    rayParam = 0.0;
    const float2 screenSize = float2(screenW, screenH);
    const float pxLen = max(max(abs(screenDir.x) * screenW,
                                abs(screenDir.y) * screenH), 1e-6);
    const float3 step = screenDir / pxLen;
    float3 cur = coarse;
    bool foundAbove = false;

    [loop] for (uint backStep = 0u; backStep < SSGI_FINISH_STEPS / 2u; ++backStep)
    {
        if (or(any(cur.xy < 0.0), any(cur.xy > 1.0))) break;
        const int2 p = clamp(int2(cur.xy * screenSize), int2(0, 0),
                             int2((int)screenW - 1, (int)screenH - 1));
        const float surfaceZ = gDepthHier.Load(int3(p, 0)).r;
        float s; bool ok;
        const float z = SSGI_EvalRayAtUV(cur.xy, startClip, dirClip, s, ok);
        if (and(and(ok, surfaceZ > 0.0), z > surfaceZ))
        {
            foundAbove = true;
            break;
        }
        cur -= step;
    }
    if (!foundAbove) cur = coarse;

    float3 prev = cur;
    float prevS; bool prevValid;
    float prevZ = SSGI_EvalRayAtUV(prev.xy, startClip, dirClip, prevS, prevValid);
    [loop] for (uint forwardStep = 0u; forwardStep < SSGI_FINISH_STEPS; ++forwardStep)
    {
        if (or(any(cur.xy < 0.0), any(cur.xy > 1.0))) break;
        const int2 p = clamp(int2(cur.xy * screenSize), int2(0, 0),
                             int2((int)screenW - 1, (int)screenH - 1));
        const float surfaceZ = gDepthHier.Load(int3(p, 0)).r;
        float s; bool ok;
        const float z = SSGI_EvalRayAtUV(cur.xy, startClip, dirClip, s, ok);
        if (and(and(ok, prevValid), and(surfaceZ > 0.0, z <= surfaceZ)))
        {
            const float denom = prevZ - z;
            const float f = select(denom > max(1e-6, prevZ * 1e-3),
                                   saturate((prevZ - surfaceZ) / denom), 1.0);
            refinedUV = lerp(prev.xy, cur.xy, f);
            rayZ = SSGI_EvalRayAtUV(refinedUV, startClip, dirClip,
                                    rayParam, ok);
            return ok;
        }
        prev = cur;
        prevZ = z;
        prevS = s;
        prevValid = ok;
        cur += step;
    }
    return false;
}

bool SSGI_ScreenTrace(float3 originWS, float3 dirWS,
                      out float2 hitUV, out float outHitT,
                      out float3 hitWS, out float hitLinearZ,
                      out float hitConfidence)
{
    hitUV   = float2(0, 0);
    outHitT = maxDistance;
    hitWS = float3(0, 0, 0);
    hitLinearZ = farZ;
    hitConfidence = 0.0;
    if (hizMipCount == 0u) return false;

    const float3 endWS = originWS + dirWS * maxDistance;

    float4 pClip = mul(float4(originWS, 1.0), viewProj);
    float4 eClip = mul(float4(endWS,    1.0), viewProj);
    if (pClip.w <= 1e-6 || eClip.w <= 1e-6) return false;   // behind the eye

    const float3 pNdc = pClip.xyz / pClip.w;
    const float3 eNdc = eClip.xyz / eClip.w;

    const float2 pUV = float2(pNdc.x * 0.5 + 0.5, 0.5 - pNdc.y * 0.5);
    const float2 eUV = float2(eNdc.x * 0.5 + 0.5, 0.5 - eNdc.y * 0.5);

    const float3 screenOrigin = float3(pUV, pNdc.z);
    const float3 screenDir = float3(eUV - pUV, eNdc.z - pNdc.z);
    float3 coarse;
    if (!SSGI_HierarchicalMarch(screenOrigin, screenDir, coarse)) return false;

    float rayParam;
    const float4 dirClip = eClip - pClip;
    if (!SSGI_FinishTrace(coarse, screenDir, pClip, dirClip,
                          hitUV, /*out*/hitLinearZ, rayParam)) return false;
    const float rayNdcZ = hitLinearZ;
    const int2 hitPx = clamp(int2(hitUV * float2(screenW, screenH)), int2(0, 0),
                             int2((int)screenW - 1, (int)screenH - 1));
    const float surfaceZ = gDepthHier.Load(int3(hitPx, 0)).r;
    if (surfaceZ <= 0.0) return false;

    const float2 dPx = (hitUV - pUV) * float2(screenW, screenH);
    if (abs(dPx.x) + abs(dPx.y) < kSelfHitTexels) return false;

    const float linRay = SSGI_LinearDepth(rayNdcZ);
    const float linSurface = SSGI_LinearDepth(surfaceZ);
    const float tolerance = max(thickness, min(linSurface * 0.004, 0.5));
    const float residual = abs(linRay - linSurface);
    const float depthConfidence = 1.0 - smoothstep(tolerance * 0.35,
                                                   tolerance, residual);
    const float3 hitN = normalize(gNormal.Load(int3(hitPx, 0)).rgb * 2.0 - 1.0);
    const float facingConfidence = smoothstep(0.0, 0.2, -dot(hitN, dirWS));
    hitConfidence = depthConfidence * facingConfidence;
    if (hitConfidence <= 1e-3) return false;

    hitWS = SSGI_WorldFromUVDepth(hitUV, surfaceZ);
    outHitT = length(hitWS - originWS);
    hitLinearZ = linSurface;
    return outHitT <= maxDistance * 1.01;
}

// Fetch previous-frame lit radiance at a screen hit. Motion vectors choose the
// sample; static-scene matrix reprojection is used only as a depth oracle when
// it agrees with that motion, matching HybridReflFetch's dynamic-surface rule.
bool SSGI_FetchPrevRadiance(float2 hitUV, float3 hitWS, float hitT,
                            float hitLinearZ, out float3 radiance,
                            out float historyConfidence)
{
    radiance = float3(0, 0, 0);
    historyConfidence = 0.0;
    if (hasPrevHdr == 0u) return false;
    const int2 hitPx = clamp(int2(hitUV * float2(screenW, screenH)), int2(0, 0),
                             int2((int)screenW - 1, (int)screenH - 1));
    const float2 mv = gVelocity.Load(int3(hitPx, 0)).xy;
    // gVelocity is a raw NDC delta (curNDC - prevNDC); UV delta flips Y.
    const float2 prevUV = SSGI_PreviousUV(hitUV,mv);
    if (or(any(prevUV < 0.0), any(prevUV > 1.0))) return false;

    // Static-world reprojection validates previous depth only when it agrees
    // with the hit pixel's motion vector. A disagreement identifies a moving
    // surface; in that case the matrix's static-world Z is not a valid oracle,
    // so trust the motion-vector sample as the reflection path does.
    const float4 prevClip = mul(float4(hitWS, 1.0), prevViewProj);
    const int2 prevPx = clamp(int2(prevUV * float2(screenW, screenH)), int2(0, 0),
                              int2((int)screenW - 1, (int)screenH - 1));
    const float prevDepth = gPrevDepth.Load(int3(prevPx, 0)).r;
    if (prevDepth <= 0.0) return false;
    historyConfidence = 1.0;
    if (prevClip.w > 1e-3)
    {
        const float3 prevNdc = prevClip.xyz / prevClip.w;
        const float2 matrixPrevUV = float2(prevNdc.x * 0.5 + 0.5,
                                           0.5 - prevNdc.y * 0.5);
        const float2 deltaPx = abs(prevUV - matrixPrevUV) *
                               float2(screenW, screenH);
        if (max(deltaPx.x, deltaPx.y) < 1.5)
        {
            if (prevNdc.z <= 0.0) return false;
            const float expectedZ = SSGI_LinearDepth(saturate(prevNdc.z));
            const float actualZ = SSGI_LinearDepth(prevDepth);
            const float depthTolerance = max(0.05, actualZ * 0.02);
            const float depthError = abs(expectedZ - actualZ);
            historyConfidence = 1.0 - smoothstep(depthTolerance * 0.25,
                                                  depthTolerance, depthError);
            if (historyConfidence <= 1e-3) return false;
        }
    }

    // ---- Mip selection: do NOT point-sample mip 0 ----
    // A diffuse ray carries the average radiance over its footprint (the DDGI_BOUNCE_ALBEDO_MIP
    // argument). Mip 0 lets one ray pick up a highlight, emissive texel or sun-lit sliver at full
    // intensity: fireflies that REBLUR smears into blobs. Ray-cone footprint: it widens with hit
    // distance and with fewer rays (more solid angle per ray), attacking the variance at its source.
    // Erring blurry is free for a diffuse signal, erring sharp is not. The mip is clamped to
    // [1, min(prevHdrMaxMip, 5)] (to that top mip alone when it is below 1).
    const float coneScale = rsqrt(float(max(rayCount, 1u)));
    const float footprintPx = max(hitT * coneScale * float(screenH) * 0.5 /
                                  max(hitLinearZ, 0.1), 1.0);
    const float maxDiffuseMip = min(max(prevHdrMaxMip, 0.0), 5.0);
    const float minDiffuseMip = min(1.0, maxDiffuseMip);
    const float mip = clamp(log2(footprintPx), minDiffuseMip, maxDiffuseMip);
    radiance = gPrevHDR.SampleLevel(gLinearClamp, prevUV, mip).rgb;
    return and(!any(isnan(radiance)), !any(isinf(radiance)));
}

void SSGI_WriteTemporalReservoir(uint2 tp, float3 receiverNormal,
                                 SSGIReservoirState r)
{
    const float meanHitT = r.M > 0u
        ? clamp(r.hitDistanceSum / float(r.M), 0.0, maxDistance)
        : maxDistance;
    const float meanConfidence = r.M > 0u
        ? saturate(r.confidenceSum / float(r.M)) : 0.0;
    if (!r.hasSelected || r.weightSum <= 1e-5)
    {
        OutReservoirTemporalDirection[tp] = 0.0;
        OutReservoirTemporalRadiance[tp]  = float4(0.0, 0.0, 0.0, 0.0);
    }
    else
    {
        OutReservoirTemporalDirection[tp] =
            float4(OctEncode(r.direction), r.hitT, r.selectedConfidence);
        OutReservoirTemporalRadiance[tp] = float4(r.radiance, r.weightSum);
    }
    OutReservoirTemporalAux[tp] = float2(meanHitT, meanConfidence);
    OutReservoirTemporalMeta[tp] =
        SSGI_PackReservoirMeta(receiverNormal, r.M, r.age);
}

// Reproject the previous final reservoir through the PRIMARY receiver motion,
// reconstruct its selected hit from the old receiver + direction/distance,
// reconnect that hit to this receiver, then refresh radiance from the current
// previous-HDR pyramid. Reusing stale stored radiance would turn an otherwise
// valid geometric reservoir into a multi-frame light trail.
bool SSGI_LoadTemporalHistory(uint2 tp, uint2 px, float2 uv,
                              float3 P, float3 N, float3 origin,
                              out float3 direction, out float hitT,
                              out float confidence, out float3 radiance,
                              out float weight, out uint representedM,
                              out float meanHitT, out float meanConfidence,
                              out uint age)
{
    direction = float3(0.0, 0.0, 1.0);
    hitT = maxDistance;
    confidence = 0.0;
    radiance = 0.0;
    weight = 0.0;
    representedM = 0u;
    meanHitT = maxDistance;
    meanConfidence = 0.0;
    age = 0u;
    if (reservoirHistoryValid == 0u) return false;

    const float2 mv = gVelocity.Load(int3(px, 0)).xy;
    const float2 prevUV = SSGI_PreviousUV(uv,mv);
    if (or(any(prevUV < 0.0), any(prevUV > 1.0))) return false;
    const int2 prevTp = int2(round(float2(RTTraceCoordinateFromFullUV(prevUV.x,screenW,2),
                                        RTTraceCoordinateFromFullUV(prevUV.y,screenH,2))));
    if(or(any(prevTp<0), any(prevTp>=int2(traceW,traceH)))) return false;

    float3 prevReceiverN; uint prevM; uint prevAge;
    SSGI_UnpackReservoirMeta(gReservoirHistoryMeta.Load(int3(prevTp, 0)),
                             prevReceiverN, prevM, prevAge);
    if (or(or(prevM == 0u, prevAge >= clamp(reservoirMaxAge, 1u, 63u)),
           dot(prevReceiverN, N) < reservoirNormalThreshold))
        return false;

    const int2 prevPx = min(prevTp*2,int2(screenW,screenH)-1);
    const float2 storedReceiverUV=(float2(prevPx)+.5)/float2(screenW,screenH);
    const float prevDepth = gPrevDepth.Load(int3(prevPx, 0)).r;
    if (prevDepth <= 0.0) return false;

    // For a static receiver, matrix reprojection and motion agree. In that case
    // previous depth is a strong disocclusion oracle. For a moving receiver the
    // matrix describes the wrong world point, so follow the engine-wide rule
    // and trust the velocity lookup plus stored-normal test instead.
    const float4 prevClip = mul(float4(P, 1.0), prevViewProj);
    if (prevClip.w > 1e-4)
    {
        const float3 prevNdc = prevClip.xyz / prevClip.w;
        const float2 matrixPrevUV = float2(prevNdc.x * 0.5 + 0.5,
                                           0.5 - prevNdc.y * 0.5);
        const float2 deltaPx = abs(matrixPrevUV - prevUV) * float2(screenW, screenH);
        if (max(deltaPx.x, deltaPx.y) < 1.5)
        {
            if (prevNdc.z <= 0.0) return false;
            const float expected = SSGI_LinearDepth(prevNdc.z);
            const float actual = SSGI_LinearDepth(prevDepth);
            const float tol = max(thickness, actual * reservoirDepthThreshold);
            if (abs(expected - actual) > tol) return false;
        }
    }

    const float4 oldDirection = gReservoirHistoryDirection.Load(int3(prevTp, 0));
    const float4 oldRadiance  = gReservoirHistoryRadiance.Load(int3(prevTp, 0));
    const float2 oldAux       = gReservoirHistoryAux.Load(int3(prevTp, 0));
    representedM = min(prevM, max(clamp(reservoirMaxM, 1u, 63u) - 1u, 1u));
    const float countScale = float(representedM) / float(prevM);
    meanHitT = clamp(oldAux.x, 0.0, maxDistance);
    meanConfidence = saturate(oldAux.y);
    age = min(prevAge + 1u, 63u);

    // An all-miss reservoir is still useful: its M zero-confidence samples
    // contribute to the coverage denominator. It has no selected direction.
    if (oldRadiance.w <= 1e-5 || oldDirection.w <= 1e-5)
    {
        weight = 0.0;
        meanHitT = maxDistance;
        return true;
    }

    const float3 previousReceiver = SSGI_WorldFromPrevUVDepth(storedReceiverUV, prevDepth);
    const float3 previousOrigin = previousReceiver + prevReceiverN * normalBias;
    const float3 oldDir = OctDecode(oldDirection.xy);
    const float3 oldHitWS = previousOrigin + oldDir * oldDirection.z;

    float2 currentHitUV; float currentHitLinear; float3 hitNormal; float geoConfidence;
    if (!SSGI_ValidateReusedSample(origin, N, oldHitWS, direction, hitT,
                                   currentHitUV, currentHitLinear, hitNormal,
                                   geoConfidence))
        return false;

    float historyConfidence;
    const float3 currentHitWS = origin + direction * hitT;
    if (!SSGI_FetchPrevRadiance(currentHitUV, currentHitWS, hitT,
                                currentHitLinear, radiance, historyConfidence))
        return false;

    confidence = saturate(geoConfidence * historyConfidence);
    if (confidence <= 1e-3) return false;
    const float oldTarget = SSGI_RadianceTarget(oldRadiance.rgb, oldDirection.w);
    const float newTarget = SSGI_RadianceTarget(radiance, confidence);
    const float targetRatio = clamp(newTarget / max(oldTarget, 1e-5), 0.0, 4.0);
    // Usually this is the same static receiver in two frames and the ratio is
    // one. Keeping the area-measure shift makes temporal reuse well-defined for
    // moving receivers instead of silently treating the reconnected direction
    // as if it had been sampled from the current cosine distribution.
    const float sourceCosR = max(dot(prevReceiverN, oldDir), 0.02);
    const float targetCosR = max(dot(N, direction), 0.02);
    const float sourceCosH = max(-dot(hitNormal, oldDir), 0.02);
    const float targetCosH = max(-dot(hitNormal, direction), 0.02);
    const float sourceAreaPdf = sourceCosR * sourceCosH /
                                max(oldDirection.z * oldDirection.z, 1e-4);
    const float targetAreaPdf = targetCosR * targetCosH /
                                max(hitT * hitT, 1e-4);
    const float jacobian = clamp(targetAreaPdf / max(sourceAreaPdf, 1e-5),
                                 0.25, 4.0);
    // Do not let two individually clamped approximations multiply into a 16x
    // history amplification in one frame. SSGI_CapReservoir applies the final
    // normalization guard after this aggregate is merged with the fresh ray.
    const float shiftCorrection = clamp(targetRatio * jacobian, 0.0, 4.0);
    weight = oldRadiance.w * countScale * shiftCorrection;
    // Reconnection changes the represented path distances. Blend the previous
    // all-path mean toward the selected reconnected path instead of pretending
    // every historical ray travelled the newly selected distance.
    meanHitT = lerp(meanHitT, lerp(maxDistance, hitT, confidence), 0.25);
    return true;
}

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    // Half-resolution dispatch: tp indexes this pass's targets, px is the full-res G-buffer pixel it
    // stands for. Everything below works in FULL-res pixels (uv, Hi-Z march, step count, tolerances):
    // the depth hierarchy is full-res, and half-res steps would skip thin geometry. Half res changes
    // how many rays are cast, not the precision of any one of them.
    const uint2 tp = dtid.xy;
    if (tp.x >= traceW || tp.y >= traceH) return;

    // Top-left of the 2x2 block, not a per-frame rotating offset: rotating would raise coverage over
    // time but moves the viewZ/normal guides under REBLUR every frame, which its reprojection assumes
    // are stable. Ray directions already vary with frameIndex. Revisit only with a measurement.
    const uint2 px = min(tp * 2u, uint2(screenW - 1u, screenH - 1u));

    const float2 uv    = (float2(px) + 0.5) * float2(invScreenW, invScreenH);
    const float  ndcZ  = gDepth.Load(int3(px, 0)).r;

    // ---- NRD guide channels, written for EVERY pixel ----
    // Sky included: viewZ beyond CommonSettings::denoisingRange tells NRD to skip a pixel; an unwritten
    // one would feed NRD whatever the texture held last frame.
    float3 N = normalize(gNormal.Load(int3(px, 0)).rgb * 2.0 - 1.0);
    if (ndcZ <= 0.0)
    {
        OutNrdViewZ[tp]       = 1e7;                 // >> denoisingRange -> skip
        OutNrdNormalRough[tp] = NRD_FrontEnd_PackNormalAndRoughness(float3(0, 0, 1), 1.0, 0);
        OutNrdMV[tp]          = float2(0, 0);
        if (reservoirEnabled != 0u)
        {
            SSGIReservoirState empty;
            SSGI_ResetReservoir(empty);
            SSGI_WriteTemporalReservoir(tp, float3(0.0, 0.0, 1.0), empty);
        }
        else
        {
            OutRawCoverage[tp] = 0.0;
            OutNrdDiffIn[tp] = 0.0;
        }
        return;
    }

    const float3 P      = SSGI_WorldFromUVDepth(uv, ndcZ);
    const float  linZ   = SSGI_LinearDepth(ndcZ);
    const float3 V      = normalize(cameraPos - P);

    // Roughness is fixed at 1.0: this is a DIFFUSE lobe. REBLUR uses the
    // roughness guide for its specular lobe reasoning only, and handing it the
    // surface's real roughness would make it treat a smooth floor's diffuse GI
    // as a near-deterministic signal and stop filtering it.
    OutNrdNormalRough[tp] = NRD_FrontEnd_PackNormalAndRoughness(N, 1.0, 0);
    OutNrdViewZ[tp]       = linZ;

    // IN_MV: NRD wants a screen-space UV delta (motionVectorScale {1,1,0}, not world space;
    // prevUv = uv + mv). gVelocity is a raw NDC delta (cur - prev), so it is halved and re-signed
    // for UV (prev - cur, Y down) - not a passthrough.
    const float2 vel = gVelocity.Load(int3(px, 0)).xy;
    OutNrdMV[tp] = float2(-vel.x * 0.5, vel.y * 0.5);

    // ---- Far field ----
    // Used only by the calibration path. Normally Lighting.ps owns the far field and applies it through
    // (1 - resolvedFraction) with its full expression (DDGI + sky + AO), not the DDGI-only
    // approximation here. Keeping it out of the traced signal keeps the partition's halves independent.
    const float3 farIrr = (debugMode == 1u) ? SSGI_FarFieldIrradiance(P, N, V)
                                            : float3(0, 0, 0);

    if (debugMode == 1u)
    {
        // CALIBRATION: claim the whole hemisphere and return exactly the far-field contribution.
        // Lighting.ps computes rgb + far * (1 - 1) = rgb, so the frame must be unchanged; if it is not, the
        // bug is in the units, the partition algebra or the binding chain, before any trace code runs.
        OutRawCoverage[tp] = 1.0;
        OutNrdDiffIn[tp] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(
                               farIrr, 0.0, false);
        return;
    }

    // ---- One-new-ray spatiotemporal reservoir path ----
    // One ray per HALF-res pixel is 0.25 full-res rpp. Temporal reuse happens here because the
    // expensive trace / reprojection helpers are already resident; the following spatial pass
    // consumes this scratch reservoir.
    if (reservoirEnabled != 0u)
    {
        const float3 origin = P + N * normalBias;
        SSGIReservoirState reservoir;
        SSGI_ResetReservoir(reservoir);

        float3 oldDir; float oldHitT; float oldConfidence; float3 oldRadiance;
        float oldWeight; uint oldM; float oldMeanHitT;
        float oldMeanConfidence; uint oldAge;
        if (SSGI_LoadTemporalHistory(tp, px, uv, P, N, origin,
                                     oldDir, oldHitT, oldConfidence, oldRadiance,
                                     oldWeight, oldM, oldMeanHitT,
                                     oldMeanConfidence, oldAge))
        {
            SSGI_MergeReservoir(reservoir, oldDir, oldHitT, oldConfidence,
                                oldRadiance, oldWeight, oldM, oldMeanHitT,
                                oldMeanConfidence, oldAge,
                                SSGI_ReservoirRandom(tp, 0x17a31u));
        }

        const float3 freshDir = SSGI_CosineHemisphere(N, SSGI_Rand2(tp, 0u));
        float2 freshUV; float freshHitT; float3 freshHitWS;
        float freshLinearZ; float freshGeoConfidence;
        float3 freshRadiance = 0.0;
        float freshConfidence = 0.0;
        float3 storedFreshDir = freshDir;
        if (SSGI_ScreenTrace(origin, freshDir, freshUV, freshHitT, freshHitWS,
                             freshLinearZ, freshGeoConfidence))
        {
            float historyConfidence;
            if (SSGI_FetchPrevRadiance(freshUV, freshHitWS, freshHitT,
                                       freshLinearZ, freshRadiance,
                                       historyConfidence))
            {
                freshConfidence = saturate(freshGeoConfidence * historyConfidence);
                storedFreshDir = normalize(freshHitWS - origin);
            }
        }
        const float freshMeanHitT = lerp(maxDistance, freshHitT, freshConfidence);
        const float freshTarget =
            SSGI_RadianceTarget(freshRadiance, freshConfidence);
        SSGI_MergeReservoir(reservoir, storedFreshDir, freshHitT,
                            freshConfidence, freshRadiance, freshTarget,
                            1u, freshMeanHitT, freshConfidence, 0u,
                            SSGI_ReservoirRandom(tp, 0x9d2c5u));
        SSGI_CapReservoir(reservoir);
        SSGI_WriteTemporalReservoir(tp, N, reservoir);

        // Spatial writes the actual NRD signal and coverage; this dispatch does
        // not touch those UAVs, avoiding a write-after-write UAV hazard.
        return;
    }

    // ---- The gather ----
    const uint  N_RAYS = max(rayCount, 1u);
    const float3 origin = P + N * normalBias;

    float3 sumResolved = float3(0, 0, 0);
    float  resolvedWeight = 0.0;
    float  sumHitT     = 0.0;

    [loop] for (uint r = 0; r < N_RAYS; ++r)
    {
        const float2 u2  = SSGI_Rand2(tp, r);
        const float3 dir = SSGI_CosineHemisphere(N, u2);

        float2 hitUV; float hitT; float3 hitWS; float hitLinZ; float hitConf;
        if (!SSGI_ScreenTrace(origin, dir, hitUV, hitT, hitWS, hitLinZ, hitConf))
        {
            sumHitT += maxDistance;
            continue;
        }

        float3 L; float historyConf;
        if (!SSGI_FetchPrevRadiance(hitUV, hitWS, hitT, hitLinZ,
                                    L, historyConf))
        {
            sumHitT += maxDistance;
            continue;
        }
        // A hit whose previous-frame colour could not be fetched (off-screen last frame, or no pyramid)
        // is an UNRESOLVED ray, not a black sample, hence the continue: counting it as resolved-with-zero
        // would darken exactly the pixels around moving objects and screen edges.
        const float confidence = hitConf * historyConf;
        sumResolved   += L * confidence;
        resolvedWeight += confidence;
        // NRD's diffuse hit-distance is the average over ALL paths. A partially
        // trusted hit is a mixture of its real distance and the unresolved
        // max-distance sentinel, matching the same confidence partition used
        // for radiance and coverage.
        sumHitT += lerp(maxDistance, hitT, confidence);
    }

    const float invN     = 1.0 / float(N_RAYS);
    const float covFrac  = saturate(resolvedWeight * invN);

    // ---- Units: plain mean, NOT mean * PI ----
    // The output is E/PI, the quantity DDGI_SampleVolumeRaw returns, because Lighting.ps multiplies
    // both tiers by one ddgiDiffuseScale (default 1.0 = physical). With cosine sampling
    // pdf = cos(theta)/PI, so
    //     E    ~= (1/N) SUM L_i cos_i / pdf_i = (PI/N) SUM L_i
    //     E/PI ~= (1/N) SUM L_i                               <- plain mean
    // Do not return E: Lighting.ps would scale by PI again (a bounce ~3.14x stronger than DDGI). The
    // calibration mode cannot catch a units bug: it never runs this estimator.
    // Divide by N (all rays), not by resolvedWeight: unresolved directions get their share through
    // Lighting.ps's (1 - covFrac); dividing by the resolved weight would give the resolved directions
    // the whole hemisphere's solid angle.
    const float3 nearIrr = sumResolved * invN;

    OutRawCoverage[tp] = covFrac;

    // ---- NRD input ----
    // Denoise exactly the near term this pass produces. Do not denoise the reconstructed hemisphere
    // (near + far * (1 - cov)): the resolve would then subtract DDGI raw back out, which is not the far
    // field Lighting.ps adds (DDGI + sky + AO), leaving a seam along the coverage gradient.
    // Hit distance is the mean over ALL paths (REBLUR diffuse contract); unresolved rays contributed
    // maxDistance.
    const float  hitAvg  = sumHitT * invN;
    const float  normHit = REBLUR_FrontEnd_GetNormHitDist(
                               hitAvg, linZ, float3(hitDistParamA, 0.0, 1.0), 1.0);
    OutNrdDiffIn[tp] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(nearIrr, normHit, false);
}
