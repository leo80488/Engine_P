// Harness-only GPU test of core/bsdf_dielectric.hlsli (-bsdf_probe). For each case: the
// histogram of the directions SampleDielectric draws, PdfDielectric and EvalDielectric at
// the bin centres, and the sampler's mean weight per lobe with its spread, which
// tools/verify_bsdf_probe.py holds against an independent CPU oracle (tools/bsdf_oracle.py).
#include "bsdf_sample.hlsli"   // RTRandom; core/math.hlsli and core/sampling.hlsli
#include "core/bsdf_dielectric.hlsli"

RWStructuredBuffer<uint> Histogram : register(u2);

// The layout below is mirrored by PathTracePass (kDielectricProbe*) and the verifier.
static const uint kDielBase      = 4096u;
static const uint kDielCases     = 9u;
static const uint kDielBins      = 256u;   // 2 sides x 8 |z| bands x 16 azimuth sectors
static const uint kDielCaseWords = 3u * kDielBins + 8u;
static const uint kDielGroups    = 64u;
static const uint kDielPerThread = 64u;    // 64 groups x 256 threads x 64 = 1M draws a case
static const uint kDielPartial   = kDielBase + kDielCases * kDielCaseWords;

void DielectricProbeCase(uint c, out DielectricLobe lobe, out float3 wo)
{
    // perceptual roughness, wo's angle from +Z in degrees, transmission weight, thin
    static const float4 kCases[9] = {
        float4(0.30,   0.0, 1.0, 0.0),
        float4(0.60,   0.0, 1.0, 0.0),
        float4(1.00,   0.0, 1.0, 0.0),
        float4(0.30,  70.0, 1.0, 0.0),
        float4(0.45, 150.0, 1.0, 0.0),   // inside, 30 degrees from -Z: under the critical angle
        float4(0.45, 120.0, 1.0, 0.0),   // inside, 60 degrees from -Z: past it
        float4(0.40,  45.0, 1.0, 1.0),
        float4(0.50,  45.0, 0.4, 0.0),
        float4(0.00,  50.0, 1.0, 0.0) }; // smooth: two deltas
    const float4 k = kCases[c];
    lobe.alpha        = k.x * k.x;
    lobe.eta          = 1.5;
    lobe.transmission = k.z;
    lobe.thinWalled   = k.w > 0.5;
    // 0.1 rad off the x axis keeps a delta's direction off the azimuth sector edges.
    const float theta = radians(k.y);
    wo = float3(sin(theta) * cos(0.1), sin(theta) * sin(0.1), cos(theta));
}

uint DielectricProbeBin(float3 w)
{
    float phi = atan2(w.y, w.x);
    if (phi < 0.0)
        phi += 2.0 * PI;
    const uint side = (w.z < 0.0) ? 1u : 0u;
    return side * 128u + min(uint(abs(w.z) * 8.0), 7u) * 16u + min(uint(phi * 16.0 / (2.0 * PI)), 15u);
}

float3 DielectricProbeBinCentre(uint bin)
{
    const float side = (bin >= 128u) ? -1.0 : 1.0;
    const uint  b    = bin % 128u;
    const float z    = ((b / 16u) + 0.5) / 8.0;
    const float phi  = ((b % 16u) + 0.5) * 2.0 * PI / 16.0;
    const float s    = sqrt(max(1.0 - z * z, 0.0));
    return float3(s * cos(phi), s * sin(phi), side * z);
}

// One thread a bin: zero the counts, write pdf and f at the bin centre.
[numthreads(256, 1, 1)]
void DielectricClear(uint3 tid : SV_DispatchThreadID)
{
    const uint c = tid.y, bin = tid.x;
    const uint base = kDielBase + c * kDielCaseWords;
    DielectricLobe lobe;
    float3 wo;
    DielectricProbeCase(c, lobe, wo);
    const float3 wi = DielectricProbeBinCentre(bin);
    Histogram[base + bin] = 0;
    Histogram[base + kDielBins + bin]      = asuint(PdfDielectric(lobe, wo, wi));
    Histogram[base + 2u * kDielBins + bin] = asuint(EvalDielectric(lobe, wo, wi));
    if (bin < 8u)
        Histogram[base + 3u * kDielBins + bin] = 0;
}

groupshared float4 gs_sums[256];
groupshared uint   gs_fails[256];

[numthreads(256, 1, 1)]
void Dielectric(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID)
{
    const uint c = gid.y, t = gtid.x;
    const uint base = kDielBase + c * kDielCaseWords;
    DielectricLobe lobe;
    float3 wo;
    DielectricProbeCase(c, lobe, wo);
    uint rng = (gid.x * 256u + t + 1u) * 747796405u ^ (c + 29u) * 2891336453u;
    if (rng == 0u)
        rng = 1u;
    // x / y: the reflected weight and its square; z / w: the transmitted ones.
    float4 sums  = float4(0.0, 0.0, 0.0, 0.0);
    uint   fails = 0u;
    for (uint i = 0u; i < kDielPerThread; ++i)
    {
        const float  uc = RTRandom(rng);
        const float2 u  = float2(RTRandom(rng), RTRandom(rng));
        DielectricSample s;
        if (!SampleDielectric(lobe, wo, uc, u, s))
        {
            ++fails;
            continue;
        }
        InterlockedAdd(Histogram[base + DielectricProbeBin(s.wi)], 1u);
        if (s.transmitted)
            sums.zw += float2(s.weight, s.weight * s.weight);
        else
            sums.xy += float2(s.weight, s.weight * s.weight);
    }
    gs_sums[t]  = sums;
    gs_fails[t] = fails;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 128u; stride > 0u; stride >>= 1u)
    {
        if (t < stride)
        {
            gs_sums[t]  += gs_sums[t + stride];
            gs_fails[t] += gs_fails[t + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (t == 0u)
    {
        const uint p = kDielPartial + (c * kDielGroups + gid.x) * 5u;
        Histogram[p + 0u] = asuint(gs_sums[0].x);
        Histogram[p + 1u] = asuint(gs_sums[0].y);
        Histogram[p + 2u] = asuint(gs_sums[0].z);
        Histogram[p + 3u] = asuint(gs_sums[0].w);
        Histogram[p + 4u] = gs_fails[0];
    }
}

// One thread a case: the partial sums of the 64 groups into the case's misc words.
[numthreads(1, 1, 1)]
void DielectricReduce(uint3 gid : SV_GroupID)
{
    const uint c = gid.y;
    float4 sums  = float4(0.0, 0.0, 0.0, 0.0);
    uint   fails = 0u;
    for (uint g = 0u; g < kDielGroups; ++g)
    {
        const uint p = kDielPartial + (c * kDielGroups + g) * 5u;
        sums  += float4(asfloat(Histogram[p + 0u]), asfloat(Histogram[p + 1u]),
                        asfloat(Histogram[p + 2u]), asfloat(Histogram[p + 3u]));
        fails += Histogram[p + 4u];
    }
    const uint misc = kDielBase + c * kDielCaseWords + 3u * kDielBins;
    Histogram[misc + 0u] = asuint(sums.x);
    Histogram[misc + 1u] = asuint(sums.y);
    Histogram[misc + 2u] = asuint(sums.z);
    Histogram[misc + 3u] = asuint(sums.w);
    Histogram[misc + 4u] = fails;
}
