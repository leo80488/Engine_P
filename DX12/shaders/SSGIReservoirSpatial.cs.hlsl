// SSGIReservoirSpatial.cs.hlsl - half-resolution spatial reservoir reuse.
//
// Trace has already produced one fresh candidate and merged temporal history.
// This pass reconnects up to eight neighbouring selected samples to the current
// receiver, applies receiver/endpoint/short-visibility validation, performs
// weighted reservoir resampling, and writes the coverage + NRD input contract.
// REBLUR stays the final reconstruction denoiser, not the sampler.

#include "SSGICommon.hlsli"

#define NRD_NORMAL_ENCODING    2
#define NRD_ROUGHNESS_ENCODING 1
#include "NRD.hlsli"

Texture2D<float>  gDepth     : register(t1, space0);
Texture2D<float4> gNormal    : register(t2, space0);
Texture2D<float2> gDepthHier : register(t4, space0);

Texture2D<float4> gReservoirTemporalDirection : register(t15, space0);
Texture2D<float4> gReservoirTemporalRadiance  : register(t16, space0);
Texture2D<float2> gReservoirTemporalAux       : register(t17, space0);
Texture2D<uint>   gReservoirTemporalMeta      : register(t18, space0);

RWTexture2D<float>  OutRawCoverage : register(u0, space0);
RWTexture2D<float4> OutNrdDiffIn   : register(u4, space0);
RWTexture2D<float4> OutReservoirHistoryDirection : register(u12, space0);
RWTexture2D<float4> OutReservoirHistoryRadiance  : register(u13, space0);
RWTexture2D<float2> OutReservoirHistoryAux       : register(u14, space0);
RWTexture2D<uint>   OutReservoirHistoryMeta      : register(u15, space0);

#include "SSGIReservoir.hlsli"

void SSGI_LoadTemporalReservoir(int2 p, out SSGIReservoirState r,
                                out float3 receiverNormal)
{
    SSGI_ResetReservoir(r);
    const float4 direction = gReservoirTemporalDirection.Load(int3(p, 0));
    const float4 radiance = gReservoirTemporalRadiance.Load(int3(p, 0));
    const float2 aux = gReservoirTemporalAux.Load(int3(p, 0));
    SSGI_UnpackReservoirMeta(gReservoirTemporalMeta.Load(int3(p, 0)),
                             receiverNormal, r.M, r.age);
    r.weightSum = max(radiance.w, 0.0);
    r.hitDistanceSum = clamp(aux.x, 0.0, maxDistance) * float(r.M);
    r.confidenceSum = saturate(aux.y) * float(r.M);
    r.hasSelected = r.M > 0u && r.weightSum > 1e-5 && direction.w > 1e-5;
    if (r.hasSelected)
    {
        r.direction = OctDecode(direction.xy);
        r.hitT = clamp(direction.z, 0.0, maxDistance);
        r.selectedConfidence = saturate(direction.w);
        r.radiance = max(radiance.rgb, 0.0);
    }
}

void SSGI_WriteFinalReservoir(uint2 tp, float3 receiverNormal,
                              SSGIReservoirState r)
{
    const float meanHitT = r.M > 0u
        ? clamp(r.hitDistanceSum / float(r.M), 0.0, maxDistance)
        : maxDistance;
    const float meanConfidence = r.M > 0u
        ? saturate(r.confidenceSum / float(r.M)) : 0.0;
    if (!r.hasSelected || r.weightSum <= 1e-5)
    {
        OutReservoirHistoryDirection[tp] = 0.0;
        OutReservoirHistoryRadiance[tp] = 0.0;
    }
    else
    {
        OutReservoirHistoryDirection[tp] =
            float4(OctEncode(r.direction), r.hitT, r.selectedConfidence);
        OutReservoirHistoryRadiance[tp] = float4(r.radiance, r.weightSum);
    }
    OutReservoirHistoryAux[tp] = float2(meanHitT, meanConfidence);
    OutReservoirHistoryMeta[tp] = SSGI_PackReservoirMeta(receiverNormal, r.M, r.age);
}

static const int2 kSpatialOffsets[8] = {
    int2( 1, 0), int2(-1, 0), int2(0,  1), int2(0, -1),
    int2( 1, 1), int2(-1, 1), int2(1, -1), int2(-1,-1)
};

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    const uint2 tp = dtid.xy;
    if (tp.x >= traceW || tp.y >= traceH) return;
    const uint2 px = min(tp * 2u, uint2(screenW - 1u, screenH - 1u));
    const float2 uv = (float2(px) + 0.5) * float2(invScreenW, invScreenH);
    const float ndcZ = gDepth.Load(int3(px, 0)).r;

    if (ndcZ <= 0.0)
    {
        OutRawCoverage[tp] = 0.0;
        OutNrdDiffIn[tp] = 0.0;
        SSGIReservoirState empty;
        SSGI_ResetReservoir(empty);
        SSGI_WriteFinalReservoir(tp, float3(0.0, 0.0, 1.0), empty);
        return;
    }

    const float3 P = SSGI_WorldFromUVDepth(uv, ndcZ);
    const float3 N = normalize(gNormal.Load(int3(px, 0)).rgb * 2.0 - 1.0);
    const float3 origin = P + N * normalBias;
    const float linearZ = SSGI_LinearDepth(ndcZ);

    SSGIReservoirState result;
    SSGI_ResetReservoir(result);

    // The centre temporal reservoir is already expressed at this receiver.
    SSGIReservoirState centre; float3 centreStoredNormal;
    SSGI_LoadTemporalReservoir(int2(tp), centre, centreStoredNormal);
    if (centre.M > 0u)
    {
        SSGI_MergeReservoir(result, centre.direction, centre.hitT,
                            centre.selectedConfidence, centre.radiance,
                            centre.weightSum, centre.M,
                            centre.hitDistanceSum / float(centre.M),
                            centre.confidenceSum / float(centre.M), centre.age,
                            SSGI_ReservoirRandom(tp, 0x312fdu));
    }

    const uint candidateCount = min(reservoirSpatialCandidates, 8u);
    const uint rotation = HashLowbias32(frameIndex ^ HilbertIndex64(tp & 63u)) & 7u;
    [loop] for (uint i = 0u; i < candidateCount; ++i)
    {
        const int2 np = int2(tp) + kSpatialOffsets[(i + rotation) & 7u];
        if (any(np < 0) || np.x >= (int)traceW || np.y >= (int)traceH) continue;

        const uint2 npx = min(uint2(np) * 2u, uint2(screenW - 1u, screenH - 1u));
        const float neighborZ = gDepth.Load(int3(npx, 0)).r;
        if (neighborZ <= 0.0) continue;
        const float2 neighborUV = (float2(npx) + 0.5) *
                                  float2(invScreenW, invScreenH);
        const float3 neighborP = SSGI_WorldFromUVDepth(neighborUV, neighborZ);
        const float3 neighborN = normalize(gNormal.Load(int3(npx, 0)).rgb * 2.0 - 1.0);
        if (dot(N, neighborN) < reservoirNormalThreshold) continue;

        // Plane-distance gating keeps a nearby foreground surface from donating
        // its bright reservoir across a depth edge, while allowing slanted
        // planes whose camera-space depths naturally differ across the kernel.
        const float planeDistance = abs(dot(neighborP - P, N));
        const float planeTolerance = max(thickness * 1.5,
                                         linearZ * reservoirDepthThreshold);
        if (planeDistance > planeTolerance) continue;

        SSGIReservoirState source; float3 sourceStoredNormal;
        SSGI_LoadTemporalReservoir(np, source, sourceStoredNormal);
        if (source.M == 0u || dot(sourceStoredNormal, neighborN) < reservoirNormalThreshold)
            continue;

        // Preserve zero-confidence samples in M: they describe unresolved
        // directions and therefore belong in the near/far coverage partition.
        if (!source.hasSelected)
        {
            SSGI_MergeReservoir(result, source.direction, source.hitT, 0.0,
                                0.0, 0.0, source.M,
                                source.hitDistanceSum / float(source.M),
                                source.confidenceSum / float(source.M),
                                source.age,
                                SSGI_ReservoirRandom(tp, 0x5119bu + i));
            continue;
        }

        const float3 sourceOrigin = neighborP + neighborN * normalBias;
        const float3 oldHitWS = sourceOrigin + source.direction * source.hitT;
        float3 targetDirection; float targetHitT; float2 targetHitUV;
        float targetHitLinear; float3 targetHitNormal; float targetConfidence;
        if (!SSGI_ValidateReusedSample(origin, N, oldHitWS,
                                       targetDirection, targetHitT, targetHitUV,
                                       targetHitLinear, targetHitNormal,
                                       targetConfidence))
            continue;

        // ReSTIR-GI-style shift Jacobian for reconnecting the same hit surface
        // from another receiver. Clamping is deliberate: screen-space geometry
        // and FP16 directions are approximate, and an unbounded ratio turns one
        // grazing neighbour into a persistent firefly.
        const float sourceCosR = max(dot(neighborN, source.direction), 0.02);
        const float targetCosR = max(dot(N, targetDirection), 0.02);
        const float sourceCosH = max(-dot(targetHitNormal, source.direction), 0.02);
        const float targetCosH = max(-dot(targetHitNormal, targetDirection), 0.02);
        const float sourceAreaPdf = sourceCosR * sourceCosH /
                                    max(source.hitT * source.hitT, 1e-4);
        const float targetAreaPdf = targetCosR * targetCosH /
                                    max(targetHitT * targetHitT, 1e-4);
        const float jacobian = clamp(targetAreaPdf / max(sourceAreaPdf, 1e-5),
                                     0.25, 4.0);
        const float sourceTarget =
            SSGI_RadianceTarget(source.radiance, source.selectedConfidence);
        const float targetTarget =
            SSGI_RadianceTarget(source.radiance, targetConfidence);
        const float targetRatio = clamp(targetTarget / max(sourceTarget, 1e-5),
                                        0.0, 4.0);

        const uint sourceM = min(source.M, clamp(reservoirMaxM, 1u, 63u));
        const float countScale = float(sourceM) / float(source.M);
        // Target and geometric corrections are both approximate in screen
        // space. Bound their product, otherwise two legal 4x clamps become a
        // 16x amplification before the normalization guard runs.
        const float shiftCorrection = clamp(targetRatio * jacobian, 0.0, 4.0);
        const float transformedWeight = source.weightSum * countScale *
                                        shiftCorrection;
        const float targetMeanHit = lerp(
            source.hitDistanceSum / float(source.M),
            lerp(maxDistance, targetHitT, targetConfidence), 0.5);
        SSGI_MergeReservoir(result, targetDirection, targetHitT,
                            targetConfidence, source.radiance,
                            transformedWeight, sourceM, targetMeanHit,
                            source.confidenceSum / float(source.M), source.age,
                            SSGI_ReservoirRandom(tp, 0xa511eu + i * 0x45d9fu));
    }

    SSGI_CapReservoir(result);
    // Spatial reuse is a current-frame estimator only. Do not persist `result`:
    // every neighbour's already-spatial reservoir would become next frame's
    // temporal source, and a rare bright sample would be counted repeatedly and
    // diffuse one pixel farther every frame. Persist the centre temporal
    // reservoir, so spatial candidates cannot recursively infect the history.
    SSGI_WriteFinalReservoir(tp, N, centre);

    const float coverage = result.M > 0u
        ? saturate(result.confidenceSum / float(result.M)) : 0.0;
    const float3 nearIrr = SSGI_EstimateRadiance(result);
    const float hitAvg = result.M > 0u
        ? clamp(result.hitDistanceSum / float(result.M), 0.0, maxDistance)
        : maxDistance;
    const float normHit = REBLUR_FrontEnd_GetNormHitDist(
        hitAvg, linearZ, float3(hitDistParamA, 0.0, 1.0), 1.0);

    OutRawCoverage[tp] = coverage;
    OutNrdDiffIn[tp] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(
        nearIrr, normHit, false);
}
