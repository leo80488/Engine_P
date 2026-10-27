// FroxelLightInjectRT.cs.hlsl - the froxel fog's ray-traced lighting (VolumetricFogPass's RT tier).
// The output is FroxelLightInject.cs.hlsl's:
//   .rgb = sigma_s x L_in + emission
//   .a   = sigma_t (the density volume's)
// L_in at a froxel:
//   - the sun (kFroxelRtSun): its colour x its fog scale x HG x the medium's transmittance to the
//     sky x a visibility ray (the shadow casters, alpha-tested; glass filters it)
//   - one local light of the froxel's cluster list, resampled: `candidates` uniform picks (every
//     light of a shorter list) weighed by their unshadowed light, merged with last frame's choice
//     at the reprojected froxel (through ClusterPass's key map, re-evaluated here, its M capped at
//     20 frames), then one visibility ray to the chosen light. The falloff is the surfaces' over
//     the froxel's volume (light_eval.hlsli's FroxelDistanceAttenuation); the selection's target
//     takes the analytic media only, the contribution the local volumes too
//   - the indirect light: FroxelIndirectRT.cs.hlsl's coarse grid, trilinear, when it ran (DDGI or
//     the indirect rays); else the constant ambient, as FroxelLightInject's
// The emission is the global medium's and the medium set's local volumes'. The grid is radial:
// FroxelToWorld places a slice at its distance along the pixel's ray.
// The Shafts entry traces the sun's single scattering per half-resolution pixel instead
// (pass.volumetricFog.rtShafts; it is described where it starts).
//
// On the ReSTIR shell layout (ReSTIR::CreateShellLayout): b0 the froxel constants, t0 the TLAS,
// t1 / t2 the instance and alpha tables, the bindless buffers and textures, u0 the lighting volume.
// The rest is read through the descriptor heap (FroxelParams' FROXEL_RT block).
#define FROXEL_RT 1
#include "FroxelCommon.hlsli"
#include "light_eval.hlsli"
#include "brdf.hlsli"
#include "bsdf_sample.hlsli"
#include "material.hlsli"
#include "rt_instance_data.hlsli"
#include "rt_alpha_record.hlsli"
#include "core/hash.hlsli"
#include "core/color.hlsli"   // Luminance

cbuffer FroxelCB : register(b0)
{
    FroxelParams P;
};

RaytracingAccelerationStructure g_TLAS : register(t0);
StructuredBuffer<RTInstanceData> g_Instances : register(t1);
StructuredBuffer<RTAlphaRecord> g_AlphaRecords : register(t2);
ByteAddressBuffer g_Buffers[16384] : register(t0, space1);
Texture2D g_AllTextures[16384] : register(t0, space2);
SamplerState gSampWrap : register(s1);
RWTexture3D<float4> DstLighting : register(u0);

typedef RTInstanceData HRInstanceData;
#define HR_MaterialUV RTInstanceUV
// rt_hit_fetch.hlsli reads the alpha-record gate as cbuffer globals; here they are the froxel
// constants'.
#define alphaTestEnabled (P.alphaTestEnabled)
#define alphaInstanceCount (P.alphaInstanceCount)
#include "rt_hit_fetch.hlsli"
#include "core/bsdf_dielectric.hlsli"
#include "rt_glass.hlsli"
#undef alphaTestEnabled
#undef alphaInstanceCount

#include "core/participating_media.hlsli"
#include "froxel_rt_medium.hlsli"

// ==== Random numbers ====
struct FroxelRng { uint state; };
float FroxelRandom(inout FroxelRng r)
{
    r.state = HashPcg(r.state);
    return float(r.state >> 8) * (1.0 / 16777216.0);
}

// ==== Visibility ====
// What reaches p from distance along l (10000: the sky): 0 behind an opaque shadow caster (mask
// 0x02, alpha-tested), else the glass crossings' product.
float3 FroxelVisibility(float3 p, float3 l, float distance)
{
    RayDesc ray;
    ray.Origin = p;
    ray.Direction = l;
    ray.TMin = 1e-3;
    ray.TMax = max(distance - 1e-2, 2e-3);
    if (!(P.rtFlags & kFroxelRtGlass))
    {
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
        q.TraceRayInline(g_TLAS, RAY_FLAG_NONE, 0x02, ray);
        while (q.Proceed())
            if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
                HR_AlphaAccept(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics(), false))
                q.CommitNonOpaqueTriangleHit();
        return q.CommittedStatus() == COMMITTED_NOTHING ? 1.0 : 0.0;
    }
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(g_TLAS, RAY_FLAG_NONE, 0x02, ray);
    float3 t = 1;
    while (q.Proceed())
    {
        if (q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        const uint id = q.CandidateInstanceID();
        if (RTGlassInstance(id))
        {
            t *= RTGlassCrossing(P.pathMaterialsIdx, id, q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics(),
                                 (float3x3)q.CandidateWorldToObject3x4(), l);
            continue;
        }
        if (HR_AlphaAccept(id, q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics(), false))
            q.CommitNonOpaqueTriangleHit();
    }
    return q.CommittedStatus() == COMMITTED_NOTHING ? t : 0.0;
}

// ==== Local lights ====
GPULight FroxelLoadLight(uint index)
{
    StructuredBuffer<GPULight> lights = ResourceDescriptorHeap[P.lightsIdx];
    return lights[index];
}
// A light's scale in the fog (VolumetricLightComponent; 0 opts out): 1 without the table.
float FroxelLightScale(uint index)
{
    if (P.lightMediumScaleIdx == 0u) return 1.0;
    StructuredBuffer<float> scales = ResourceDescriptorHeap[P.lightMediumScaleIdx];
    return scales[index];
}
// The raymarched shafts take this light (kFroxelRtSkipShadow): one of the volumetric list's
// shadow casters, matched by its slice, which no other light holds.
bool FroxelRaymarched(uint shadowSliceIdx)
{
    if (!(P.rtFlags & kFroxelRtSkipShadow) || shadowSliceIdx == 0xFFFFFFFFu || P.volLightsIdx == 0u) return false;
    StructuredBuffer<GPULight> volumetric = ResourceDescriptorHeap[P.volLightsIdx];
    const uint count = (P.rtFlags >> 8) & 0xFFu;
    for (uint i = 0; i < count; ++i)
        if (volumetric[i].shadowSliceIdx == shadowSliceIdx) return true;
    return false;
}
// The light a local light sends the froxel at p, before the fog between them and any occluder:
// colour x intensity x the froxel-averaged falloff x the cone x HG x its fog scale. l, distance:
// toward the light.
float3 FroxelLocalLight(GPULight light, uint index, float3 p, float3 V, float radius, out float3 l, out float distance)
{
    const float3 toLight = light.position - p;
    distance = length(toLight);
    l = toLight / max(distance, 1e-4);
    if (light.type == 0u || !(distance < light.radius)) return 0;
    if (FroxelRaymarched(light.shadowSliceIdx)) return 0;
    const float soft = max(radius, asfloat(light.sourceRadiusBits));
    return light.color * light.intensity * FroxelDistanceAttenuation(distance, light.radius, soft) *
           AngularAttenuation(l, light) * PhaseHG(dot(V, l), P.anisotropy) * FroxelLightScale(index);
}
// The selection's target: the light's luminance through the analytic media (the global and the
// height fog), no local volumes and no occluder.
float FroxelTarget(GPULight light, uint index, float3 p, float3 V, float radius, MediumSet media)
{
    float3 l;
    float distance;
    const float3 L = FroxelLocalLight(light, index, p, V, radius, l, distance);
    if (!any(L > 0)) return 0;
    return Luminance(L) * exp(-min(MediumAnalyticDepth(media, p, l, 0, distance), 80.0));
}

struct FroxelReservoir
{
    uint  light;   // kFroxelRtNoLight = none
    float W;       // the contribution weight: sum of weights / (M x target)
    uint  M;
    float target;  // the chosen light's target, this frame
    float weightSum;
};
FroxelReservoir FroxelEmptyReservoir()
{
    FroxelReservoir r;
    r.light = kFroxelRtNoLight; r.W = 0; r.M = 0; r.target = 0; r.weightSum = 0;
    return r;
}
bool FroxelStream(inout FroxelReservoir r, uint light, float target, float weight, uint M, inout FroxelRng rng)
{
    r.weightSum += weight;
    r.M += M;
    if (weight > 0 && FroxelRandom(rng) * r.weightSum < weight)
    {
        r.light = light;
        r.target = target;
        return true;
    }
    return false;
}

// This froxel's place in last frame's grid (radial, as FroxelTemporal's FROXEL_RT build): false
// outside it.
bool FroxelReproject(float3 worldPos, out uint3 prev)
{
    prev = 0;
    const float4 clip = mul(float4(worldPos, 1.0), P.prevViewProj);
    if (clip.w <= 0.0) return false;
    const float2 uv = float2(clip.x / clip.w * 0.5 + 0.5, -clip.y / clip.w * 0.5 + 0.5);
    const float slice = ViewDepthToFroxelSlice(length(worldPos - P.prevCameraPos), P.froxelNear, P.froxelFar, P.froxelD);
    const float3 g = float3(uv * float2(P.froxelW, P.froxelH), slice);
    if (any(g < 0.0) || any(g >= float3(P.froxelW, P.froxelH, P.froxelD))) return false;
    prev = uint3(g);
    return true;
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= P.froxelW || DTid.y >= P.froxelH || DTid.z >= P.froxelD)
        return;
    const uint froxelIndex = DTid.x + P.froxelW * (DTid.y + P.froxelH * DTid.z);

    // XY-only sub-voxel jitter, as FroxelLightInject's.
    float3 jitter = GetFroxelJitter(P.frameIndex);
    jitter.z = 0.0;
    const float3 worldPos = FroxelToWorldJittered(DTid, P, jitter);
    Texture3D<float4> density = ResourceDescriptorHeap[P.densityIdx];
    const float4 dens = density[DTid];
    const float3 V = normalize(worldPos - P.cameraPos);
    RWStructuredBuffer<uint2> reservoirs = ResourceDescriptorHeap[P.reservoirIdx];

    // ---- Emission (Le x sigma_t), as FroxelLightInject's, the local volumes from the medium set ----
    const MediumSet media = FroxelMediumSet();
    float3 emission = P.fogEmissive * GlobalFogExtinction(P, worldPos);
    for (uint i = 0; i < media.localCount; ++i)
    {
        const MediumLocal v = MediumLoadLocal(i);
        if (any(v.emission > 0)) emission += v.emission * MediumLocalSigmaT(v, worldPos);
    }

    // Air without a scattering medium: nothing to light, no ray.
    if (!(max(dens.r, max(dens.g, dens.b)) > 1e-7))
    {
        reservoirs[froxelIndex] = uint2(kFroxelRtNoLight, 0u);
        DstLighting[DTid] = float4(emission, dens.a);
        return;
    }

    FroxelRng rng;
    rng.state = HashPcg(froxelIndex ^ HashPcg(P.frameIndex * 0x9E3779B9u + 0x7F4A7C15u));

    // The froxel's radius: half the larger of its slice's depth and its width at that distance.
    const float r0 = FroxelSliceToViewDepth(float(DTid.z), P.froxelNear, P.froxelFar, P.froxelD);
    const float r1 = FroxelSliceToViewDepth(float(DTid.z + 1u), P.froxelNear, P.froxelFar, P.froxelD);
    const float across = length(FroxelToWorld(uint3(min(DTid.x + 1u, P.froxelW - 1u), DTid.y, DTid.z), P) -
                                FroxelToWorld(uint3(max(DTid.x, 1u) - 1u, DTid.y, DTid.z), P)) * 0.5;
    const float radius = 0.5 * max(r1 - r0, across);

    // ---- The sun ----
    float3 sun = 0;
    if ((P.rtFlags & kFroxelRtSun) && any(P.sunColor > 0) && P.sunMediumScale > 0)
    {
        const float3 l = -normalize(P.sunDir);
        const float3 E = P.sunColor * P.sunMediumScale * PhaseHG(dot(V, l), P.anisotropy) *
                         (P.mediumIdx ? MediumTransmittanceDeterministic(media, worldPos, l, 0, kMediumFar) : 1.0);
        if (Luminance(E) * max(dens.r, max(dens.g, dens.b)) > 1e-7)
            sun = E * FroxelVisibility(worldPos, l, 10000.0);
    }

    // ---- One local light, resampled ----
    float3 local = 0;
    FroxelReservoir r = FroxelEmptyReservoir();
    if (P.lightsIdx != 0u && P.lightCount != 0u && P.lightGridIdx != 0u)
    {
        // The froxel's cluster: its screen position and planar view depth.
        const float4 clip = mul(float4(worldPos, 1.0), P.viewProj);
        const float2 uv = saturate(float2(clip.x / clip.w * 0.5 + 0.5, -clip.y / clip.w * 0.5 + 0.5));
        const uint3 cc = GetClusterCoord(uv, max(clip.w, P.clusterNear), P.clusterNear, P.clusterFar);
        StructuredBuffer<LightGridEntry> grid = ResourceDescriptorHeap[P.lightGridIdx];
        // An overflowed cluster (offset ~0) lists every light: [0, count).
        const LightGridEntry e = grid[ClusterIndex(cc)];
        const bool all = e.offset == 0xFFFFFFFFu;
        const uint n = e.count;
        StructuredBuffer<uint> list = ResourceDescriptorHeap[P.lightIndexIdx];
        if (n != 0u)
        {
            // Every light of a short list, else `candidates` uniform picks: each weight is the
            // target over its pick's probability (1 / n), and M counts every pick.
            const bool every = n <= P.candidates;
            const uint picks = every ? n : P.candidates;
            for (uint k = 0; k < picks; ++k)
            {
                const uint j = every ? k : min(uint(FroxelRandom(rng) * float(n)), n - 1u);
                const uint index = all ? j : list[e.offset + j];
                if (index >= P.lightCount) continue;
                const float target = FroxelTarget(FroxelLoadLight(index), index, worldPos, V, radius, media);
                FroxelStream(r, index, target, target * float(n), 0u, rng);
            }
            r.M = picks;
        }

        // Last frame's choice at the reprojected froxel: its light under this frame's index (the
        // key map), its target re-evaluated here, its M capped at 20 frames of this one's.
        uint3 prev;
        if ((P.rtFlags & kFroxelRtHistory) && P.prevReservoirIdx != 0u && FroxelReproject(worldPos, prev))
        {
            RWStructuredBuffer<uint2> history = ResourceDescriptorHeap[P.prevReservoirIdx];
            const uint2 h = history[prev.x + P.froxelW * (prev.y + P.froxelH * prev.z)];
            uint light = h.x & 0xFFFFu;
            if (light != kFroxelRtNoLight && P.lightMapIdx != 0u)
            {
                StructuredBuffer<uint> map = ResourceDescriptorHeap[P.lightMapIdx];
                light = light < P.prevLightCount ? map[light] : 0xFFFFFFFFu;
            }
            else if (P.lightMapIdx == 0u)
                light = kFroxelRtNoLight;   // no key map: an index of last frame's list means nothing now
            if (light < P.lightCount)
            {
                const uint M = min(h.x >> 16, 20u * max(r.M, 1u));
                const float target = FroxelTarget(FroxelLoadLight(light), light, worldPos, V, radius, media);
                FroxelStream(r, light, target, target * asfloat(h.y) * float(M), M, rng);
            }
        }

        if (r.light != kFroxelRtNoLight && r.target > 0)
        {
            r.W = r.weightSum / (float(max(r.M, 1u)) * r.target);
            const GPULight light = FroxelLoadLight(r.light);
            float3 l;
            float distance;
            const float3 L = FroxelLocalLight(light, r.light, worldPos, V, radius, l, distance);
            const float T = P.mediumIdx ? MediumTransmittanceDeterministic(media, worldPos, l, 0, distance) : 1.0;
            if (any(L > 0) && T > 0)
                local = L * T * r.W * FroxelVisibility(worldPos, l, distance);
        }
        else
            r = FroxelEmptyReservoir();
    }
    reservoirs[froxelIndex] = uint2((r.light & 0xFFFFu) | (min(r.M, 0xFFFFu) << 16), asuint(r.W));

    // ---- The indirect light: FroxelIndirectRT.cs.hlsl's grid at the froxel, else FroxelLightInject's
    // constant stand-in for it ----
    float3 ambient = P.ambientColor * P.ambientContribution / (4.0 * PI);
    if (P.indirectMode != kFroxelIndirectConstant && P.indirectReadIdx != 0u)
    {
        RWTexture3D<float4> grid = ResourceDescriptorHeap[P.indirectReadIdx];
        ambient = FroxelIndirectFetch(grid, (float3(DTid) + 0.5 + jitter) / float3(kFroxelIndirectCell),
                                      FroxelIndirectDims(P)).rgb;
    }

    DstLighting[DTid] = float4(dens.rgb * (sun + local + ambient) + emission, dens.a);
}

// ==== The sun's shafts, per pixel (pass.volumetricFog.rtShafts) ====
// The sun's single scattering along each half-resolution texel's ray instead of in the froxels, as
// sharp as its visibility rays: kFroxelShaftSamples distances a frame on [froxelNear, the texel's
// surface or froxelFar] - the froxels' range, the raymarch's - drawn as the reference path tracer
// draws its camera segment's vertex (PathTrace.cs.hlsl's PTCameraSegment, without the lights):
// the global medium's and the height fog's free flights conditioned on the segment, or a uniform
// distance in one local volume, in proportion to their opacities, weighed by the mixture's density
// (one-sample MIS, the balance heuristic). Each takes the sun's transmittance to the sky and one
// visibility ray. VolumetricRaymarchTemporal and VolumetricRaymarchApply take the output as the
// raymarch's: .rgb the in-scatter, .a the texel's surface key (vol_raymarch_common.hlsli).
#include "vol_raymarch_common.hlsli"

static const uint kFroxelShaftSamples = 2u;

float FroxelShaftOpacity(float depth)
{
    return 1.0 - exp(-min(depth, 80.0));
}

float FroxelShaftLocalOpacity(MediumLocal v, float3 o, float3 d, float ta, float tb)
{
    return FroxelShaftOpacity(MediumLocalDepthMarch(v, o, d, ta, tb));
}

// One distance on o + t d, t in [t0, t1], from the mixture (@p uStrategy picks the strategy,
// @p uDistance the distance in it), and the mixture's density there. False when the segment holds
// no medium or the draw left it.
bool FroxelShaftDistance(MediumSet m, float3 o, float3 d, float t0, float t1, float uStrategy, float uDistance,
                         out float t, out float pdf)
{
    t = kMediumFar;
    pdf = 0.0;
    const float opGlobal = FroxelShaftOpacity(MediumGlobalDepth(m.global, o, d, t0, t1));
    const float opHeight = FroxelShaftOpacity(MediumHeightDepth(m.height, o, d, t0, t1));
    float opLocals = 0.0;
    for (uint i = 0u; i < m.localCount; ++i)
    {
        const MediumLocal v = MediumLoadLocal(i);
        float ta, tb;
        if (MediumLocalInterval(v, o, d, t0, t1, ta, tb)) opLocals += FroxelShaftLocalOpacity(v, o, d, ta, tb);
    }
    const float opSum = opGlobal + opHeight + opLocals;
    if (!(opSum > 0.0)) return false;
    const float cGlobal = opGlobal / opSum, cHeight = opHeight / opSum, cLocals = opLocals / opSum;
    if (uStrategy < cGlobal)
        t = MediumGlobalFreeFlight(m.global, o, d, t0, t1, -log(1.0 - uDistance * opGlobal));
    else if (uStrategy < cGlobal + cHeight)
        t = MediumHeightFreeFlight(m.height, o, d, t0, t1, -log(1.0 - uDistance * opHeight));
    else
    {
        float u = uDistance * opLocals;
        for (uint i = 0u; i < m.localCount; ++i)
        {
            const MediumLocal v = MediumLoadLocal(i);
            float ta, tb;
            if (!MediumLocalInterval(v, o, d, t0, t1, ta, tb)) continue;
            const float op = FroxelShaftLocalOpacity(v, o, d, ta, tb);
            if (u < op) { t = lerp(ta, tb, u / op); break; }
            u -= op;
        }
    }
    if (!(t < t1)) return false;
    if (cGlobal > 0.0)
        pdf += cGlobal * MediumGlobalSigmaTAt(m.global, o + d * t) *
               exp(-min(MediumGlobalDepth(m.global, o, d, t0, t), 80.0)) / opGlobal;
    if (cHeight > 0.0)
        pdf += cHeight * MediumHeightSigmaT(m.height, o.y + d.y * t) *
               exp(-min(MediumHeightDepth(m.height, o, d, t0, t), 80.0)) / opHeight;
    if (cLocals > 0.0)
        for (uint i = 0u; i < m.localCount; ++i)
        {
            const MediumLocal v = MediumLoadLocal(i);
            float ta, tb;
            if (MediumLocalInterval(v, o, d, t0, t1, ta, tb) && t >= ta && t <= tb)
                pdf += cLocals * FroxelShaftLocalOpacity(v, o, d, ta, tb) / (opLocals * (tb - ta));
        }
    return pdf > 0.0;
}

[numthreads(8, 8, 1)]
void Shafts(uint3 DTid : SV_DispatchThreadID)
{
    RWTexture2D<float4> output = ResourceDescriptorHeap[P.shaftsIdx];
    uint halfW, halfH;
    output.GetDimensions(halfW, halfH);
    if (DTid.x >= halfW || DTid.y >= halfH) return;

    // The full-res pixel this texel marches and its surface (the raymarch's VolRmMarchPixel).
    Texture2D<float> depth = ResourceDescriptorHeap[P.sceneDepthIdx];
    float ndcZ;
    const int2 px = VolRmMarchPixel(depth, DTid.xy, ndcZ);
    uint fullW, fullH;
    depth.GetDimensions(fullW, fullH);
    const float2 ndc = float2((float(px.x) + 0.5) / float(fullW) * 2.0 - 1.0, 1.0 - (float(px.y) + 0.5) / float(fullH) * 2.0);
    const float4 pH = mul(float4(ndc, 0.5, 1.0), P.invViewProj);
    const float3 d = normalize(pH.xyz / pH.w - P.cameraPos);
    float tEnd = P.froxelFar;
    if (ndcZ > 1e-6)
    {
        const float4 sH = mul(float4(ndc, ndcZ, 1.0), P.invViewProj);
        tEnd = min(length(sH.xyz / sH.w - P.cameraPos), P.froxelFar);
    }

    float3 inScatter = 0;
    if (tEnd > P.froxelNear && P.mediumIdx != 0u && any(P.sunColor > 0) && P.sunMediumScale > 0)
    {
        const MediumSet media = FroxelMediumSet();
        const float3 l = -normalize(P.sunDir);
        const float3 E = P.sunColor * P.sunMediumScale * PhaseHG(dot(d, l), P.anisotropy);
        FroxelRng rng;
        rng.state = HashPcg((DTid.x + DTid.y * halfW) ^ HashPcg(P.frameIndex * 0x9E3779B9u + 0x68E31DA4u));
        const float uStrategy = FroxelRandom(rng);
        [unroll] for (uint k = 0; k < kFroxelShaftSamples; ++k)
        {
            float t, pdf;
            const float uDistance = (float(k) + FroxelRandom(rng)) / float(kFroxelShaftSamples);
            if (!FroxelShaftDistance(media, P.cameraPos, d, P.froxelNear, tEnd, uStrategy, uDistance, t, pdf)) continue;
            const float3 x = P.cameraPos + d * t;
            const float3 sigmaS = MediumAt(media, x).sigmaS;
            const float3 L = E * MediumTransmittanceDeterministic(media, x, l, 0, kMediumFar);
            if (!(Luminance(L * sigmaS) > 0.0)) continue;
            const float T = MediumTransmittanceDeterministic(media, P.cameraPos, d, P.froxelNear, t);
            inScatter += sigmaS * T * L * FroxelVisibility(x, l, 10000.0) / (pdf * float(kFroxelShaftSamples));
        }
    }
    output[DTid.xy] = float4(inScatter, VolRmKey(ndcZ, P.nearPlane, P.farPlane, P.froxelFar));
}
