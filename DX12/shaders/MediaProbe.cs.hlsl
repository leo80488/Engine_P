// Harness-only GPU test of core/participating_media.hlsli (-bsdf_probe): the histograms of the
// Henyey-Greenstein directions, the free-flight distances (and the source each collision
// picks) and the equi-angular distances the header draws, the densities at the bin centres,
// the optical depths and the ratio-tracking transmittance, which tools/verify_bsdf_probe.py
// holds against an independent CPU oracle (tools/media_oracle.py). The case tables are
// mirrored there.
#include "core/math.hlsli"
#include "core/sampling.hlsli"
#include "core/participating_media.hlsli"

RWStructuredBuffer<uint> Histogram : register(u2);

// The layout below is mirrored by PathTracePass (kMediaProbe*) and the verifier.
static const uint kMediaBase        = 14336u;
static const uint kMediaHgCases     = 5u;
static const uint kMediaHgBins      = 256u;   // 16 cos bands x 16 azimuth sectors about d
static const uint kMediaHgCaseWords = 2u * kMediaHgBins;
static const uint kMediaFfBase      = kMediaBase + kMediaHgCases * kMediaHgCaseWords;
static const uint kMediaFfCases     = 8u;
static const uint kMediaFfBins      = 64u;
static const uint kMediaFfCaseWords = 80u;
static const uint kMediaEaBase      = kMediaFfBase + kMediaFfCases * kMediaFfCaseWords;
static const uint kMediaEaCases     = 2u;
static const uint kMediaEaBins      = 64u;
static const uint kMediaEaCaseWords = 2u * kMediaEaBins;
static const uint kMediaPartial     = kMediaEaBase + kMediaEaCases * kMediaEaCaseWords;
static const uint kMediaGroups      = 64u;
static const uint kMediaPerThread   = 64u;    // 64 groups x 256 threads x 64 = 1M draws a case

// A free-flight case's words after its 64 bins.
static const uint kFfNone    = 64u;   // no collision on the segment
static const uint kFfOver    = 65u;   // a collision past the histogram's range
static const uint kFfSources = 66u;   // 4 counts: global, height, local volume 0, local volume 1
static const uint kFfTau     = 70u;   // MediumAnalyticDepth
static const uint kFfTauDet  = 71u;   // MediumDepthDeterministic
static const uint kFfAbsorb  = 72u;   // MediumAbsorptionTransmittance, rgb
static const uint kFfRatio   = 75u;   // the ratio-tracking sum and sum of squares

// ==== The probe's media ====
// Each free-flight case reads a window of these two local volumes.
static uint g_probeLocalFirst = 0u;

MediumLocal MediumLoadLocal(uint index)
{
    MediumLocal v;
    v._pad = 0.0;
    if (g_probeLocalFirst + index == 0u)
    {
        // A box: centre (2, 0.5, 0), half extents (1.5, 1, 1), soft edges, falling with height.
        v.row0 = float4(1.0 / 1.5, 0.0, 0.0, -2.0 / 1.5);
        v.row1 = float4(0.0, 1.0, 0.0, -0.5);
        v.row2 = float4(0.0, 0.0, 1.0, 0.0);
        v.albedo = float3(0.8, 0.7, 0.6);     v.density  = 0.6;
        v.emission = float3(0.0, 0.0, 0.0);   v.edgeFade = 0.3;
        v.shape = kMediumLocalBox;   v.heightFalloff = 0.5;   v.halfHeight = 1.0;
    }
    else
    {
        // A sphere: centre (6, 0.3, 0.2), radius 1.2.
        v.row0 = float4(1.0 / 1.2, 0.0, 0.0, -6.0 / 1.2);
        v.row1 = float4(0.0, 1.0 / 1.2, 0.0, -0.3 / 1.2);
        v.row2 = float4(0.0, 0.0, 1.0 / 1.2, -0.2 / 1.2);
        v.albedo = float3(0.5, 0.9, 0.4);     v.density  = 0.8;
        v.emission = float3(0.0, 0.0, 0.0);   v.edgeFade = 0.6;
        v.shape = kMediumLocalSphere;   v.heightFalloff = 0.0;   v.halfHeight = 1.2;
    }
    return v;
}

MediumSet ProbeEmptySet()
{
    MediumSet s;
    s.global.albedo = float3(1.0, 1.0, 1.0);    s.global.density = 0.0;
    s.global.emission = float3(0.0, 0.0, 0.0);  s.global.falloff = 0.0;
    s.global.baseHeight = 0.0;  s.global.anisotropy = 0.0;  s.global.reach = 0.0;  s.global._pad = 0.0;
    s.global.reachCenter = float3(0.0, 0.0, 0.0);
    s.height.albedo = float3(1.0, 1.0, 1.0);    s.height.density = 0.0;
    s.height.falloff = 0.0;  s.height.height = 0.0;  s.height.anisotropy = 0.0;  s.height._pad = 0.0;
    s.localCount = 0u;
    s.camera = float3(0.0, 0.0, 0.0);
    return s;
}

// Free-flight case c: the medium, the ray and its segment, and the range the histogram covers
// (the segment, or its first 20 m when it runs to the sky).
void ProbeFreeFlightCase(uint c, out MediumSet s, out float3 o, out float3 d, out float t0, out float t1,
                         out float histEnd)
{
    s  = ProbeEmptySet();
    t0 = 0.0;
    g_probeLocalFirst = 0u;
    if (c == 0u)
    {
        s.global.density = 0.3;
        o = float3(0.0, 1.0, 0.0);  d = float3(1.0, 0.0, 0.0);  t1 = 8.0;
    }
    else if (c == 1u)
    {
        s.global.density = 0.25;  s.global.falloff = 0.4;  s.global.albedo = float3(0.9, 0.5, 0.2);
        o = float3(0.0, -2.0, 0.0);  d = normalize(float3(0.8, 0.6, 0.0));  t1 = 12.0;
    }
    else if (c == 2u)
    {
        s.global.density = 0.2;  s.global.falloff = 0.3;
        o = float3(0.0, 3.0, 0.0);  d = normalize(float3(0.866, -0.5, 0.0));  t1 = 15.0;
    }
    else if (c == 3u)
    {
        s.height.density = 0.15;  s.height.falloff = 0.2;  s.height.albedo = float3(0.3, 0.6, 0.9);
        o = float3(0.0, -1.0, 0.0);  d = normalize(float3(0.714, 0.7, 0.0));  t1 = kMediumFar;
    }
    else if (c == 4u)
    {
        s.height.density = 0.05;  s.height.falloff = 0.25;
        o = float3(0.0, 4.0, 0.0);  d = normalize(float3(0.9165, -0.4, 0.0));  t1 = 20.0;
    }
    else if (c == 5u)
    {
        s.localCount = 1u;
        o = float3(-1.0, 0.5, 0.0);  d = float3(1.0, 0.0, 0.0);  t1 = 8.0;
    }
    else if (c == 6u)
    {
        s.localCount = 1u;  g_probeLocalFirst = 1u;
        o = float3(3.0, 0.5, 0.1);  d = float3(1.0, 0.0, 0.0);  t1 = 8.0;
    }
    else
    {
        s.global.density = 0.05;  s.global.falloff = 0.2;  s.global.baseHeight = 0.5;
        s.height.density = 0.03;  s.height.falloff = 0.3;
        s.localCount = 2u;
        o = float3(-1.0, 0.5, 0.0);  d = normalize(float3(1.0, 0.05, 0.0));  t1 = 10.0;
    }
    histEnd = min(t1, 20.0);
}

float ProbeHgAsymmetry(uint c)
{
    static const float kG[5] = { 0.0, 0.3, -0.3, 0.7, 0.9 };
    return kG[c];
}

static const float3 kProbeHgAxis = float3(0.3, 0.5, 0.8);

void ProbeEquiAngularCase(uint c, out float3 o, out float3 d, out float t0, out float t1, out float3 light)
{
    o = float3(0.0, 0.0, 0.0);
    if (c == 0u)
    {
        d = float3(1.0, 0.0, 0.0);  t0 = 0.0;  t1 = 10.0;  light = float3(4.0, 0.5, 0.0);
    }
    else
    {
        d = normalize(float3(1.0, 0.2, 0.0));  t0 = 1.0;  t1 = 30.0;  light = float3(2.0, 2.0, 1.0);
    }
}

// ==== Binning ====
uint HgBin(float3 axis, float3 l)
{
    float3 t, b;
    OrthonormalBasis(axis, t, b);
    float phi = atan2(dot(l, b), dot(l, t));
    if (phi < 0.0)
        phi += 2.0 * PI;
    const uint band   = min(uint((dot(l, axis) + 1.0) * 8.0), 15u);
    const uint sector = min(uint(phi * 16.0 / (2.0 * PI)), 15u);
    return band * 16u + sector;
}

float HgBinCentreCos(uint bin)
{
    return ((bin / 16u) + 0.5) / 8.0 - 1.0;
}

// ==== Entries ====
// One group per case (y: 5 HG, then 8 free-flight, then 2 equi-angular): zero the counts, write
// the densities at the bin centres and the free-flight cases' depths.
[numthreads(256, 1, 1)]
void MediaClear(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID)
{
    const uint c = gid.y, i = gtid.x;
    if (c < kMediaHgCases)
    {
        const uint base = kMediaBase + c * kMediaHgCaseWords;
        Histogram[base + i] = 0u;
        Histogram[base + kMediaHgBins + i] = asuint(MediumPhaseHG(HgBinCentreCos(i), ProbeHgAsymmetry(c)));
        return;
    }
    if (c < kMediaHgCases + kMediaFfCases)
    {
        const uint ff   = c - kMediaHgCases;
        const uint base = kMediaFfBase + ff * kMediaFfCaseWords;
        if (i >= kMediaFfCaseWords)
            return;
        Histogram[base + i] = 0u;
        if (i == 0u)
        {
            MediumSet s;
            float3 o, d;
            float t0, t1, histEnd;
            ProbeFreeFlightCase(ff, s, o, d, t0, t1, histEnd);
            Histogram[base + kFfTau]    = asuint(MediumAnalyticDepth(s, o, d, t0, t1));
            Histogram[base + kFfTauDet] = asuint(MediumDepthDeterministic(s, o, d, t0, t1));
            const float3 absorbed = MediumAbsorptionTransmittance(s, o, d, t0, t1);
            Histogram[base + kFfAbsorb + 0u] = asuint(absorbed.x);
            Histogram[base + kFfAbsorb + 1u] = asuint(absorbed.y);
            Histogram[base + kFfAbsorb + 2u] = asuint(absorbed.z);
        }
        return;
    }
    const uint ea   = c - kMediaHgCases - kMediaFfCases;
    const uint base = kMediaEaBase + ea * kMediaEaCaseWords;
    if (i >= kMediaEaBins)
        return;
    float3 o, d, light;
    float t0, t1;
    ProbeEquiAngularCase(ea, o, d, t0, t1, light);
    Histogram[base + i] = 0u;
    const float centre = t0 + (i + 0.5) * (t1 - t0) / float(kMediaEaBins);
    Histogram[base + kMediaEaBins + i] = asuint(MediumPdfEquiAngular(o, d, t0, t1, light, centre));
}

uint ProbeRng(uint3 gid, uint3 gtid, uint salt)
{
    const uint rng = (gid.x * 256u + gtid.x + 1u) * 747796405u ^ (gid.y * 7u + salt) * 2891336453u;
    return (rng == 0u) ? 1u : rng;
}

[numthreads(256, 1, 1)]
void MediaHg(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID)
{
    const uint c = gid.y;
    const uint base = kMediaBase + c * kMediaHgCaseWords;
    const float3 axis = normalize(kProbeHgAxis);
    uint rng = ProbeRng(gid, gtid, 11u);
    for (uint i = 0u; i < kMediaPerThread; ++i)
    {
        const float2 u = float2(MediumRandom(rng), MediumRandom(rng));
        InterlockedAdd(Histogram[base + HgBin(axis, MediumSampleHG(axis, ProbeHgAsymmetry(c), u))], 1u);
    }
}

groupshared float2 gs_ratio[256];

[numthreads(256, 1, 1)]
void MediaFreeFlight(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID)
{
    const uint c = gid.y, t = gtid.x;
    const uint base = kMediaFfBase + c * kMediaFfCaseWords;
    MediumSet s;
    float3 o, d;
    float t0, t1, histEnd;
    ProbeFreeFlightCase(c, s, o, d, t0, t1, histEnd);
    uint rng = ProbeRng(gid, gtid, 23u);
    float2 ratio = float2(0.0, 0.0);
    for (uint i = 0u; i < kMediaPerThread; ++i)
    {
        const MediumCollision hit = MediumSampleCollision(s, o, d, t0, t1, rng);
        if (hit.source == kMediumSourceNone)
        {
            InterlockedAdd(Histogram[base + kFfNone], 1u);
        }
        else
        {
            InterlockedAdd(Histogram[base + kFfSources + min(hit.source, 3u)], 1u);
            if (hit.t >= histEnd)
                InterlockedAdd(Histogram[base + kFfOver], 1u);
            else
                InterlockedAdd(Histogram[base + min(uint((hit.t - t0) / (histEnd - t0) * kMediaFfBins), kMediaFfBins - 1u)], 1u);
        }
        const float T = MediumTransmittance(s, o, d, t0, t1, rng);
        ratio += float2(T, T * T);
    }
    gs_ratio[t] = ratio;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 128u; stride > 0u; stride >>= 1u)
    {
        if (t < stride)
            gs_ratio[t] += gs_ratio[t + stride];
        GroupMemoryBarrierWithGroupSync();
    }
    if (t == 0u)
    {
        const uint p = kMediaPartial + (c * kMediaGroups + gid.x) * 2u;
        Histogram[p + 0u] = asuint(gs_ratio[0].x);
        Histogram[p + 1u] = asuint(gs_ratio[0].y);
    }
}

[numthreads(256, 1, 1)]
void MediaEquiAngular(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID)
{
    const uint c = gid.y;
    const uint base = kMediaEaBase + c * kMediaEaCaseWords;
    float3 o, d, light;
    float t0, t1;
    ProbeEquiAngularCase(c, o, d, t0, t1, light);
    uint rng = ProbeRng(gid, gtid, 37u);
    for (uint i = 0u; i < kMediaPerThread; ++i)
    {
        float pdf;
        const float t = MediumSampleEquiAngular(o, d, t0, t1, light, MediumRandom(rng), pdf);
        InterlockedAdd(Histogram[base + min(uint((t - t0) / (t1 - t0) * kMediaEaBins), kMediaEaBins - 1u)], 1u);
    }
}

// One thread a free-flight case: the 64 groups' ratio-tracking sums into the case's words.
[numthreads(1, 1, 1)]
void MediaReduce(uint3 gid : SV_GroupID)
{
    const uint c = gid.y;
    float2 sums = float2(0.0, 0.0);
    for (uint g = 0u; g < kMediaGroups; ++g)
    {
        const uint p = kMediaPartial + (c * kMediaGroups + g) * 2u;
        sums += float2(asfloat(Histogram[p + 0u]), asfloat(Histogram[p + 1u]));
    }
    const uint base = kMediaFfBase + c * kMediaFfCaseWords;
    Histogram[base + kFfRatio + 0u] = asuint(sums.x);
    Histogram[base + kFfRatio + 1u] = asuint(sums.y);
}
