#ifndef SSGI_RESERVOIR_HLSLI
#define SSGI_RESERVOIR_HLSLI

// Compact reservoir layout (24 bytes / half-resolution pixel):
//   direction RGBA16F : octahedral hit direction, hit distance, confidence
//   radiance  RGBA16F : selected radiance, cumulative reservoir weight W
//   aux       RG16F   : mean all-path hit distance, mean confidence coverage
//   meta      R32_UINT: receiver normal (2x10b), M (6b), selected age (6b)
// Three sets exist at runtime: temporal scratch + two final-history ping-pongs.

struct SSGIReservoirState
{
    float3 direction;
    float  hitT;
    float  selectedConfidence;
    float3 radiance;
    float  weightSum;
    float  hitDistanceSum;
    float  confidenceSum;
    uint   M;
    uint   age;
    bool   hasSelected;
};

#include "core/hash.hlsli"

float SSGI_ReservoirRandom(uint2 pixel, uint salt)
{
    const uint h = HashLowbias32(pixel.x ^ (pixel.y * 0x9e3779b9u) ^
                             (frameIndex * 0x85ebca6bu) ^ salt);
    return float(h & 0x00ffffffu) * (1.0 / 16777216.0);
}

#include "core/packing.hlsli"   // OctEncode / OctDecode

uint SSGI_PackReservoirMeta(float3 receiverNormal, uint M, uint age)
{
    const uint2 q = (uint2)round(saturate(OctEncode(receiverNormal) * 0.5 + 0.5) * 1023.0);
    return q.x | (q.y << 10u) | (min(M, 63u) << 20u) | (min(age, 63u) << 26u);
}

void SSGI_UnpackReservoirMeta(uint packed, out float3 receiverNormal,
                              out uint M, out uint age)
{
    const uint2 q = uint2(packed & 1023u, (packed >> 10u) & 1023u);
    receiverNormal = OctDecode(float2(q) * (2.0 / 1023.0) - 1.0);
    M = (packed >> 20u) & 63u;
    age = (packed >> 26u) & 63u;
}

void SSGI_ResetReservoir(out SSGIReservoirState r)
{
    r.direction = float3(0.0, 0.0, 1.0);
    r.hitT = maxDistance;
    r.selectedConfidence = 0.0;
    r.radiance = 0.0;
    r.weightSum = 0.0;
    r.hitDistanceSum = 0.0;
    r.confidenceSum = 0.0;
    r.M = 0u;
    r.age = 0u;
    r.hasSelected = false;
}

// Scalar RIS target for a vector radiance contribution. max(R,G,B) is used
// instead of luminance so the reconstruction factor below is bounded per
// channel; on a neutral wall it makes the selected sample's brightness cancel
// completely and the output becomes the accumulated multi-sample mean.
float SSGI_RadianceTarget(float3 radiance, float confidence)
{
    if (confidence <= 1e-5) return 0.0;
    const float3 L = max(radiance, 0.0);
    const float importance = max(max(L.r, L.g), L.b);
    // Coverage has its own accumulator, so a black radiance sample does not
    // need an artificial non-zero RIS target. Giving black samples weight
    // only raises variance and can make W disagree with the represented
    // radiance after several reuse steps.
    return confidence * importance;
}

// Standard reservoir/RIS reconstruction of mean(confidence * radiance).
// The selected sample is distributed proportional to its scalar target, while
// W/M carries the mean target over every represented candidate.
float3 SSGI_EstimateRadiance(SSGIReservoirState r)
{
    if (!r.hasSelected || r.M == 0u || r.weightSum <= 1e-5) return 0.0;
    const float selectedTarget =
        SSGI_RadianceTarget(r.radiance, r.selectedConfidence);
    if (selectedTarget <= 1e-6) return 0.0;
    const float normalization = r.weightSum /
                                (float(r.M) * selectedTarget);
    return max(r.radiance, 0.0) * r.selectedConfidence * normalization;
}

// Merge one reservoir aggregate. `candidateWeight` is its transformed radiance
// importance W, `meanConfidence` independently preserves hemisphere coverage,
// `candidateM` is the number of represented ray candidates, and `meanHitT`
// includes maxDistance for misses so NRD receives an all-path mean.
void SSGI_MergeReservoir(inout SSGIReservoirState r,
                         float3 direction, float hitT, float confidence,
                         float3 radiance, float candidateWeight,
                         uint candidateM, float meanHitT,
                         float meanConfidence, uint age,
                         float randomValue)
{
    if (candidateM == 0u) return;

    const float w = max(candidateWeight, 0.0);
    const float newWeight = r.weightSum + w;
    if (and(w > 0.0, or(!r.hasSelected, randomValue * newWeight < w)))
    {
        r.direction = direction;
        r.hitT = hitT;
        r.selectedConfidence = saturate(confidence);
        r.radiance = max(radiance, 0.0);
        r.age = age;
        r.hasSelected = true;
    }
    else if (!r.hasSelected)
    {
        // All-miss reservoirs carry useful zero-coverage samples too. Preserve
        // their age so they eventually expire instead of suppressing the first
        // newly visible hit for reservoirMaxM frames after a disocclusion.
        r.age = max(r.age, age);
    }
    r.weightSum = newWeight;
    r.hitDistanceSum += clamp(meanHitT, 0.0, maxDistance) * float(candidateM);
    r.confidenceSum += saturate(meanConfidence) * float(candidateM);
    r.M += candidateM;
}

void SSGI_CapReservoir(inout SSGIReservoirState r)
{
    const uint capM = clamp(reservoirMaxM, 1u, 63u);
    if (r.M > capM)
    {
        const float scale = float(capM) / float(r.M);
        r.weightSum *= scale;
        r.hitDistanceSum *= scale;
        r.confidenceSum *= scale;
        r.M = capM;
    }
    // `weightSum` alone has no useful absolute bound: reconstruction uses
    // W / (M * selectedTarget), so an absolute ceiling still lets a dim selected
    // sample receive hundreds of times its radiance, and repeated shift Jacobians
    // make such samples sticky and visibly white. Clamp the reservoir
    // normalization instead; well-behaved reservoirs sit near one and are
    // unaffected by this firefly guard.
    if (r.hasSelected && r.M > 0u)
    {
        const float selectedTarget =
            SSGI_RadianceTarget(r.radiance, r.selectedConfidence);
        if (selectedTarget > 1e-6)
        {
            static const float kMaxReservoirNormalization = 4.0;
            const float maxWeight = float(r.M) * selectedTarget *
                                    kMaxReservoirNormalization;
            r.weightSum = min(max(r.weightSum, 0.0), maxWeight);
        }
        else
        {
            r.weightSum = 0.0;
            r.hasSelected = false;
        }
    }
    else
    {
        r.weightSum = 0.0;
    }
    r.confidenceSum = min(max(r.confidenceSum, 0.0), float(r.M));
}

// Short screen-space reconnection test. Endpoints are excluded so the source
// and selected hit surfaces do not self-occlude. This is deliberately capped
// at 4..6 mip-0 taps: reuse must stay cheaper than casting another full ray.
float SSGI_ConnectionVisibility(float3 originWS, float3 hitWS)
{
    const uint steps = clamp(reservoirValidationSteps, 4u, 6u);
    float visibility = 1.0;
    [loop] for (uint i = 0u; i < steps; ++i)
    {
        const float t = lerp(0.12, 0.88, (float(i) + 0.5) / float(steps));
        const float3 q = lerp(originWS, hitWS, t);
        const float4 clip = mul(float4(q, 1.0), viewProj);
        if (clip.w <= 1e-5) return 0.0;
        const float3 ndc = clip.xyz / clip.w;
        const float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
        if (or(or(ndc.z <= 0.0, any(uv < 0.0)), any(uv > 1.0))) return 0.0;

        const int2 p = clamp(int2(uv * float2(screenW, screenH)), int2(0, 0),
                             int2((int)screenW - 1, (int)screenH - 1));
        const float sceneZ = gDepthHier.Load(int3(p, 0)).r;
        if (sceneZ <= 0.0) continue;

        const float rayLinear = SSGI_LinearDepth(ndc.z);
        const float sceneLinear = SSGI_LinearDepth(sceneZ);
        const float tol = max(thickness, rayLinear * reservoirDepthThreshold);
        // Smaller linear depth is nearer. Fade across the tolerance band to
        // prevent a one-ULP depth change from flipping a whole old reservoir.
        const float clearance = rayLinear - sceneLinear;
        visibility = min(visibility, 1.0 - smoothstep(tol * 0.5, tol, clearance));
        if (visibility <= 1e-3) return 0.0;
    }
    return visibility;
}

// Reconnect a stored hit point to a new receiver, validate the endpoint against
// current depth/normal, then verify the segment with the short test above.
bool SSGI_ValidateReusedSample(float3 originWS, float3 receiverNormal,
                               float3 oldHitWS,
                               out float3 direction, out float hitT,
                               out float2 hitUV, out float hitLinearZ,
                               out float3 hitNormal, out float confidence)
{
    direction = float3(0.0, 0.0, 1.0);
    hitT = maxDistance;
    hitUV = 0.0;
    hitLinearZ = farZ;
    hitNormal = float3(0.0, 0.0, 1.0);
    confidence = 0.0;

    const float4 clip = mul(float4(oldHitWS, 1.0), viewProj);
    if (clip.w <= 1e-5) return false;
    const float3 ndc = clip.xyz / clip.w;
    hitUV = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (or(or(ndc.z <= 0.0, any(hitUV < 0.0)), any(hitUV > 1.0))) return false;

    const int2 p = clamp(int2(hitUV * float2(screenW, screenH)), int2(0, 0),
                         int2((int)screenW - 1, (int)screenH - 1));
    const float surfaceZ = gDepthHier.Load(int3(p, 0)).r;
    if (surfaceZ <= 0.0) return false;

    const float expectedLinear = SSGI_LinearDepth(ndc.z);
    hitLinearZ = SSGI_LinearDepth(surfaceZ);
    const float tol = max(thickness, hitLinearZ * reservoirDepthThreshold);
    const float depthError = abs(expectedLinear - hitLinearZ);
    const float depthConfidence = 1.0 - smoothstep(tol * 0.35, tol, depthError);
    if (depthConfidence <= 1e-3) return false;

    const float3 currentHitWS = SSGI_WorldFromUVDepth(hitUV, surfaceZ);
    const float3 delta = currentHitWS - originWS;
    hitT = length(delta);
    if (hitT <= normalBias || hitT > maxDistance * 1.01) return false;
    direction = delta / max(hitT, 1e-6);
    if (dot(receiverNormal, direction) <= 0.01) return false;

    hitNormal = normalize(gNormal.Load(int3(p, 0)).rgb * 2.0 - 1.0);
    const float facingConfidence = smoothstep(0.0, 0.2, -dot(hitNormal, direction));
    if (facingConfidence <= 1e-3) return false;

    const float visibility = SSGI_ConnectionVisibility(originWS, currentHitWS);
    confidence = depthConfidence * facingConfidence * visibility;
    return confidence > 1e-3;
}

#endif // SSGI_RESERVOIR_HLSLI
