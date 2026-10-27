// FroxelIndirectRT.cs.hlsl - the froxel fog's indirect light (VolumetricFogPass's RT tier): the
// light the medium scatters from everything but the lights, on a coarse grid over the froxels (one
// cell per kFroxelIndirectCell froxels) that FroxelLightInjectRT.cs.hlsl reads trilinearly in place
// of the constant ambient. A cell holds:
//   .rgb = the in-scattered radiance per unit sigma_s, the phase-weighted mean of L_in
//   .a   = the samples behind it (0: none yet, and .rgb is the constant ambient)
// Its source (FroxelParams::indirectMode, VolumetricFogPass's resolution of
// pass.volumetricFog.rtIndirect):
//   Ddgi  DDGI's probes where they cover the cell centre: their sphere-mean irradiance / pi, the
//         isotropic phase's mean radiance (the fog's g is not applied); the constant ambient for
//         the rest of the coverage
//   Rays  kFroxelIndirectRayCount rays a frame from points of the cell, their directions drawn from the
//         phase about the view ray, each cell's points and directions on its own shifted rank-1
//         lattice (R3, R2) over time so they cover the cell and the phase evenly; the radiance a ray
//         brings is weighed by the fog to where it comes from: a miss takes the sky, an unlit surface
//         its colour and emission, a lit one its emission and - in the FROXEL_INDIRECT_SHARC build -
//         the path tracer's SHaRC cache, whose one value per voxel stands for every direction (a
//         glossy surface's too: it is the only answer one bounce has). A cache miss, a back face or
//         an untrusted proxy adds no sample. The cell's mean holds its last 1 / indirectAlpha samples
//         at its place in last frame's grid (radial, as the injection's reprojection); Filter then
//         smooths the grid into the one the injection reads (indirectReadIdx)
// The rays see the shaded surfaces (mask 0x08) as the path tracer's continuations do; glass is not
// one of them.
//
// On the ReSTIR shell layout, as FroxelLightInjectRT.cs.hlsl: b0 the froxel constants, t0 the TLAS,
// t1 / t2 the instance and alpha tables, the bindless buffers and textures. The grids, the medium,
// the sky, the cache and the frame's LightCB (DDGI) are read through the descriptor heap.
// FROXEL_INDIRECT_SHARC compiles with -enable-16bit-types (SharcEngine.hlsli).
#define FROXEL_RT 1
#include "FroxelCommon.hlsli"
#include "core/sampling.hlsli"
#include "core/hash.hlsli"
#include "material.hlsli"
#include "rt_instance_data.hlsli"
#include "rt_alpha_record.hlsli"
#include "bindless.hlsli"
#define LIGHT_CB_AS_STRUCT
#include "light_cb.hlsli"
#include "DDGISampling.hlsli"

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

typedef RTInstanceData HRInstanceData;
#define HR_MaterialUV RTInstanceUV
// rt_hit_fetch.hlsli reads the alpha-record gate as cbuffer globals; here they are the froxel
// constants'.
#define alphaTestEnabled (P.alphaTestEnabled)
#define alphaInstanceCount (P.alphaInstanceCount)
#include "rt_hit_surface.hlsli"
#undef alphaTestEnabled
#undef alphaInstanceCount

#include "core/participating_media.hlsli"
#include "froxel_rt_medium.hlsli"

#if FROXEL_INDIRECT_SHARC
#include "SharcEngine.hlsli"
#endif

// Rays a cell traces each frame.
static const uint kFroxelIndirectRayCount = 4u;
// Rank-1 lattices in 32-bit fixed point (Roberts' R2 and R3 sequences): sample i of a cell is
// shift + i x k mod 1, exact however large i grows.
static const uint2 kFroxelR2 = uint2(3242174889u, 2447445413u);
static const uint3 kFroxelR3 = uint3(3518319154u, 2882110344u, 2360945575u);
float FroxelUnit(uint x)
{
    return float(x >> 8) * (1.0 / 16777216.0);
}

// A point of the grid at continuous froxel coordinates (FroxelToWorldJittered measures its jitter
// from a froxel's centre).
float3 FroxelGridToWorld(float3 g)
{
    return FroxelToWorldJittered(uint3(0, 0, 0), P, g - 0.5);
}

// The fog's transmittance from p along w over [0, t] (kMediumFar: to the sky).
float FroxelIndirectTransmittance(MediumSet media, float3 p, float3 w, float t)
{
    return P.mediumIdx ? MediumTransmittanceDeterministic(media, p, w, 0, t) : 1.0;
}

// ==== Ddgi ====
// The probes' isotropic radiance at a point of the medium, their coverage's share; @p fallback for
// the rest.
float3 FroxelIndirectDdgi(float3 p, float3 fallback)
{
    if (BindlessNone(P.lightCbIdx)) return fallback;
    ConstantBuffer<LightCB> lcb = BindlessCBuf<LightCB>(P.lightCbIdx);
    if (lcb.ddgiEnabled == 0u || lcb.ddgiVolumeCount == 0u) return fallback;
    StructuredBuffer<DDGIVolumeGPU> vols = BindlessSBuf<DDGIVolumeGPU>(lcb.ddgiVolumeBufIdx);
    const DDGIVolumePick pick = DDGI_PickVolumes(p, vols);
    float4 best = float4(0, 0, 0, 0), second = float4(0, 0, 0, 0);
    [unroll] for (uint vi = 0; vi < DDGI_MAX_VOLUMES; ++vi)
    {
        if (!DDGI_IsPicked(pick, vi)) continue;
        const DDGIVolumeGPU vol = vols[vi];
        Texture2D<float4> irr = ResourceDescriptorHeap[NonUniformResourceIndex(vol.irradianceBaseIdx + vi)];
        Texture2D<float2> dep = ResourceDescriptorHeap[NonUniformResourceIndex(vol.depthBaseIdx + vi)];
        StructuredBuffer<DDGIProbeData> pd = ResourceDescriptorHeap[NonUniformResourceIndex(vol.probeDataBaseIdx + vi)];
        // A point of the medium faces nowhere: no normal or view bias, every probe weighed alike
        // before its Chebyshev visibility.
        const float4 s = DDGI_SampleVolumeAmbientIso(p, float3(0, 0, 0), float3(0, 0, 0), vol, irr, dep, pd, gSampWrap);
        if (vi == (uint)pick.bestSlot) best = s; else second = s;
    }
    const float4 r = DDGI_CombineWinners(best, second, pick);
    const float3 L = r.rgb * lcb.ddgiDiffuseScale * r.a + fallback * saturate(1.0 - r.a);
    return all(isfinite(L)) ? L : fallback;
}

// ==== Rays ====
float3 FroxelSky(float3 w)
{
    if (P.skyIdx == 0u) return 0;
    TextureCube<float4> sky = ResourceDescriptorHeap[P.skyIdx];
    return sky.SampleLevel(gSampWrap, w, 0).rgb * P.environmentScale;
}

#if FROXEL_INDIRECT_SHARC
// The cache's outgoing radiance at a hit, the hit's emission added (SHARC_SEPARATE_EMISSIVE), as
// the path tracer's own query (RtxdiBridgeSharc.hlsli's RAB_SharcQuery).
bool FroxelSharcQuery(RTHitSurface s, out float3 radiance)
{
    SharcParameters p;
    SHARC_ENGINE_PARAMETERS(p, P.cameraPos, P.sharcHashUav, P.sharcAccumulationUav, P.sharcResolvedUav,
                            P.sharcCapacity, P.sharcSceneScale, P.sharcRadianceScale);
    SharcHitData h;
    h.positionWorld = s.P;
    h.normalWorld = s.Ngeo;
    h.emissive = s.emissive;
    radiance = 0;
    return SharcGetCachedRadiance(p, h, radiance, false) && all(isfinite(radiance));
}
#endif

// The radiance arriving at p along w, weighed by the fog on the way: false when the ray finds
// nothing it can answer for.
bool FroxelIndirectSample(float3 p, float3 w, MediumSet media, out float3 L)
{
    L = 0;
    RayDesc ray;
    ray.Origin = p;
    ray.Direction = w;
    ray.TMin = 1e-3;
    ray.TMax = 10000.0;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(g_TLAS, RAY_FLAG_NONE, 0x08, ray);
    while (q.Proceed())
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            HR_AlphaAccept(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics(), true))
            q.CommitNonOpaqueTriangleHit();
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
    {
        L = FroxelSky(w) * FroxelIndirectTransmittance(media, p, w, kMediumFar);
        return true;
    }
    // A single-sided surface seen from behind: p lies inside or behind it, where no light is seen.
    const uint id = q.CommittedInstanceID();
    const bool front = q.CommittedTriangleFrontFace();
    if (!front && !(g_Instances[id].rtFlags & RT_INST_FLAG_DOUBLE_SIDED)) return false;
    const float t = q.CommittedRayT();
    RTHitSurface s;
    if (!RTLoadHitSurface(id, q.CommittedPrimitiveIndex(), q.CommittedTriangleBarycentrics(), q.CommittedObjectToWorld3x4(),
                          q.CommittedWorldToObject3x4(), p + w * t, front, 0, s) || !s.trusted)
        return false;
    float3 Lo = s.emissive;
    if (s.unlit)
        Lo += s.albedo;
#if FROXEL_INDIRECT_SHARC
    else if (!FroxelSharcQuery(s, Lo))
        return false;
#endif
    L = Lo * FroxelIndirectTransmittance(media, p, w, t);
    return all(isfinite(L));
}

// This cell's place in last frame's grid, in continuous cell coordinates: false outside it.
bool FroxelIndirectReproject(float3 worldPos, uint3 dims, out float3 c)
{
    c = 0;
    const float4 clip = mul(float4(worldPos, 1.0), P.prevViewProj);
    if (clip.w <= 0.0) return false;
    const float2 uv = float2(clip.x / clip.w * 0.5 + 0.5, -clip.y / clip.w * 0.5 + 0.5);
    const float slice = ViewDepthToFroxelSlice(length(worldPos - P.prevCameraPos), P.froxelNear, P.froxelFar, P.froxelD);
    c = float3(uv * float2(P.froxelW, P.froxelH), slice) / float3(kFroxelIndirectCell);
    return all(c >= 0.0) && all(c < float3(dims));
}

[numthreads(4, 4, 4)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    const uint3 dims = FroxelIndirectDims(P);
    if (any(DTid >= dims) || P.indirectIdx == 0u)
        return;
    RWTexture3D<float4> grid = ResourceDescriptorHeap[P.indirectIdx];
    const float3 fallback = P.ambientColor * P.ambientContribution / (4.0 * PI);
    const float N = 1.0 / max(P.indirectAlpha, 1e-3);   // the samples a cell averages at most
    const float3 centre = FroxelGridToWorld((float3(DTid) + 0.5) * float3(kFroxelIndirectCell));

    if (P.indirectMode == kFroxelIndirectDdgi)
    {
        grid[DTid] = float4(FroxelIndirectDdgi(centre, fallback), N);
        return;
    }

    // ---- Rays: last frame's value at the cell's centre, then this frame's sample ----
    float4 value = float4(fallback, 0);
    float3 c;
    if ((P.rtFlags & kFroxelRtIndirectHistory) && P.prevIndirectIdx != 0u && FroxelIndirectReproject(centre, dims, c))
    {
        RWTexture3D<float4> history = ResourceDescriptorHeap[P.prevIndirectIdx];
        value = FroxelIndirectFetch(history, c, dims);
        if (!all(isfinite(value))) value = float4(fallback, 0);
    }

    // The cell's lattice shifts: fixed per cell, so its samples over time stay stratified.
    const uint cell = DTid.x + dims.x * (DTid.y + dims.y * DTid.z);
    const uint shiftW = HashPcg(cell * 0x9E3779B9u + 0x2545F491u);
    const uint shiftP = HashPcg(shiftW);
    const MediumSet media = FroxelMediumSet();
    float3 sum = 0;
    uint valid = 0u;
    [loop] for (uint k = 0u; k < kFroxelIndirectRayCount; ++k)
    {
        const uint i = P.frameIndex * kFroxelIndirectRayCount + k;
        const float3 u = float3(FroxelUnit(shiftP + i * kFroxelR3.x), FroxelUnit(HashPcg(shiftP) + i * kFroxelR3.y),
                                FroxelUnit(HashPcg(shiftP + 1u) + i * kFroxelR3.z));
        const float3 p = FroxelGridToWorld((float3(DTid) + u) * float3(kFroxelIndirectCell));
        const float3 V = normalize(p - P.cameraPos);
        const float2 uw = float2(FroxelUnit(shiftW + i * kFroxelR2.x), FroxelUnit(HashPcg(shiftW) + i * kFroxelR2.y));
        float3 L;
        if (FroxelIndirectSample(p, MediumSampleHG(V, P.anisotropy, uw), media, L))
        {
            sum += L;
            ++valid;
        }
    }
    if (valid != 0u)
    {
        // A running mean until the cell holds N samples, then an exponential average over N.
        const float n = min(value.a + float(valid), N);
        value = float4(value.rgb + (sum - float(valid) * value.rgb) / n, n);
    }
    grid[DTid] = value;
}

// ==== Filter ====
// The rays' grid smoothed for the injection: a 3 x 3 x 3 tent over the cells that hold samples (a
// cell's noise falls about fourfold; the indirect light changes slowly across cells). The history
// keeps the raw grid, so the smoothing does not compound from frame to frame.
[numthreads(4, 4, 4)]
void Filter(uint3 DTid : SV_DispatchThreadID)
{
    const uint3 dims = FroxelIndirectDims(P);
    if (any(DTid >= dims) || P.indirectIdx == 0u || P.indirectReadIdx == 0u)
        return;
    RWTexture3D<float4> grid = ResourceDescriptorHeap[P.indirectIdx];
    RWTexture3D<float4> smoothed = ResourceDescriptorHeap[P.indirectReadIdx];
    float3 sum = 0;
    float weight = 0;
    [unroll] for (int z = -1; z <= 1; ++z)
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        const int3 c = int3(DTid) + int3(x, y, z);
        if (any(c < 0) || any(c >= int3(dims)))
            continue;
        // A cell without samples holds the constant ambient as a stand-in, not a measurement.
        const float4 v = grid[uint3(c)];
        if (!(v.a > 0.0))
            continue;
        const float w = float((2 - abs(x)) * (2 - abs(y)) * (2 - abs(z)));
        sum += v.rgb * w;
        weight += w;
    }
    const float4 self = grid[DTid];
    smoothed[DTid] = weight > 0.0 ? float4(sum / weight, self.a) : self;
}
