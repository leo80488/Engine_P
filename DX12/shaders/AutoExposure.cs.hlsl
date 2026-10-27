// Histogram auto exposure, two kernels:
//   CSHistogramBuild   - weighted 256-bin log-luminance histogram
//   CSHistogramAverage - percentile-clipped average log-luma -> adapted exposure
// Meter the RAW lit scene, upstream of NPRStabilize, never its output: NPRStabilize scales
// exposure-protected NPR pixels by rcp(exposure), so metering it is a feedback loop
// (see PostProcess::Context::meterSrv).
// Root signature (space2):
//   [0] CBV b0 PerDispatch
//   [1] SRV t0 HDR scene    [2] SRV t1 GBuffer normal + packed material index
//   [3] SRV t2 MaterialGPUData    [7] SRV t3 reverse-Z scene depth
//   [4] UAV u0 histogram (uint: 256 bins + the non-finite pixel count at NONFINITE_SLOT)
//   [5] UAV u1 exposure (float: [0] exposure, [1] the frame's non-finite count for CPU readback)
//   [14] UAV u2 exposure texture (1x1 R32_FLOAT, the upscalers' Exposure input; bound only when it exists)

cbuffer PerDispatch : register(b0, space2)
{
    uint  g_width;
    uint  g_height;
    float g_minLogLuma;     // log2 luma floor (e.g. -8)
    float g_invLogLumaRange;// 1 / (maxLogLuma - minLogLuma)
    float g_adaptationRate; // per-frame lerp factor when the scene BRIGHTENS
    float g_lowPercent;     // lower percentile to clip (e.g. 0.5)
    float g_highPercent;    // upper percentile to clip (e.g. 0.95)
    float g_minExposure;    // minimum exposure floor (e.g. 0.1)
    float g_maxExposure;    // maximum exposure ceiling
    float g_evBias;         // exposure compensation in EV stops (+1 = 2x brighter)
    float g_keyValue;       // target middle-grey key (default 0.18)
    float g_centerWeight;   // relative weight at image centre
    float g_skyWeight;      // multiplier for reverse-Z clear pixels
    float g_nprWeight;      // multiplier for NPR material pixels
    float g_highlightWeight;// multiplier for extreme HDR highlights
    float g_adaptationRateDark; // per-frame lerp factor when the scene DARKENS

    // Dispatch-uniform values computed on the CPU so the per-pixel path skips them.
    float g_hiStart;        // exp2(maxLogLuma - 2)
    float g_hiEnd;          // exp2(maxLogLuma)
    uint  g_metaWidth;      // depth / normal metadata dimensions
    uint  g_metaHeight;
    uint  g_materialCount;  // element count of g_materials
    uint  g_yStride;        // row stride between sampled rows (>= 1)
    // Harness only (-ae_inject_nan=<n>): the first n columns of every sampled row meter as
    // NaN, so a pack can prove the non-finite count is live. 0 in every other run.
    uint  g_debugNonFinite;
    uint  g_debugFlags;     // bit 0: harness, the bias goes on before the bounds (a reverse arm)
                            // bit 1: also write the exposure texture; bit 2: only that, no metering
}

#include "material.hlsli"
#include "core/gbuffer_codec.hlsli"   // GBufferDecodeMatIdx

Texture2D<float4>         g_hdr       : register(t0, space2);
Texture2D<float4>         g_normal    : register(t1, space2);
StructuredBuffer<MaterialGPUData> g_materials : register(t2, space2);
Texture2D<float>          g_depth     : register(t3, space2);
RWStructuredBuffer<uint>  g_histogram : register(u0, space2);
RWStructuredBuffer<float> g_exposure  : register(u1, space2);
RWTexture2D<float>        g_exposureTex : register(u2, space2);

static const uint HISTOGRAM_BINS = 256;
// One past the bins: metered pixels whose luma was NaN / Inf. Counted, never binned.
static const uint NONFINITE_SLOT = HISTOGRAM_BINS;

// Fixed-point scale for the per-pixel metering weight. The weight is a product
// (centre x sky/NPR x highlight), so the cap covers the authored maxima: centre 4 x NPR 2 = 8,
// x WEIGHT_SCALE 8 = 64. Headroom: 8K pixels x 64 = 2.12e9 < 2^32 even without subsampling.
static const float WEIGHT_SCALE   = 8.0;
static const uint  WEIGHT_MAX     = 64;

#include "core/color.hlsli"   // Luminance (BT.709)

uint LumaToBin(float luma)
{
    if (luma < 1e-5f) return 0;
    float logL = (log2(luma) - g_minLogLuma) * g_invLogLumaRange;
    return (uint)(saturate(logL) * float(HISTOGRAM_BINS - 1) + 0.5f);
}

// Exact inverse of LumaToBin: the forward map rounds t*(BINS-1), so bin b represents
// t = b/(BINS-1). Do not add +0.5: it biases every reading by half a bin.
float BinToLogLuma(uint bin)
{
    return g_minLogLuma + (float(bin) / float(HISTOGRAM_BINS - 1)) / g_invLogLumaRange;
}

// ==== CSHistogramBuild: one thread per sampled pixel, groupshared atomics ====
// Rows are subsampled by g_yStride: a 256-bin histogram is coarse, and 1/4 of the rows gives
// the same result for 1/4 of the bandwidth. x is not strided: a 32-lane wave reads 16
// contiguous texels (two whole cache lines), so striding x would save nothing.
groupshared uint gs_histogram[HISTOGRAM_BINS];

[numthreads(16, 16, 1)]
void CSHistogramBuild(uint3 groupID : SV_GroupID,
                      uint3 localID : SV_GroupThreadID,
                      uint3 id      : SV_DispatchThreadID)
{
    uint flatLocal = localID.y * 16 + localID.x;
    if (flatLocal < HISTOGRAM_BINS)
        gs_histogram[flatLocal] = 0;
    GroupMemoryBarrierWithGroupSync();

    uint2 px = uint2(id.x, id.y * max(g_yStride, 1u));
    bool  inRange    = (px.x < g_width && px.y < g_height);
    float4 hdrSample = inRange ? g_hdr.Load(int3(px, 0)) : float4(0, 0, 0, 0);
    float  luma      = Luminance(hdrSample.rgb);
    if (px.x < g_debugNonFinite) luma = asfloat(0x7FC00000u);   // -ae_inject_nan (harness)

    // Reject NaN here: the ordered compare in LumaToBin would bin it as the darkest pixel at
    // full weight. Upstream producers scrub NaN, but a custom material can still emit one.
    if (inRange && isfinite(luma))
    {
        float2 uv = (float2(px) + 0.5) / float2(g_width, g_height);
        uint   bin = LumaToBin(luma);

        // Composition-aware weight. Metadata stays at render resolution when FSR has already
        // upscaled the HDR input, so sample it by normalized UV.
        float2 p = uv * 2.0 - 1.0;
        float centerMask = saturate(1.0 - dot(p, p) * 0.5);
        centerMask *= centerMask;
        float weight = lerp(1.0, max(g_centerWeight, 0.0), centerMask);

        int2 metaPx = min(int2(uv * float2(g_metaWidth, g_metaHeight)),
                          int2(g_metaWidth - 1u, g_metaHeight - 1u));
        float depth = g_depth.Load(int3(metaPx, 0));
        bool isSky = depth <= 1e-7;
        if (isSky)
        {
            weight *= max(g_skyWeight, 0.0);
        }
        else
        {
            float4 normalRaw = g_normal.Load(int3(metaPx, 0));
            uint matIdx = GBufferDecodeMatIdx(normalRaw.a);
            matIdx = min(matIdx, max(g_materialCount, 1u) - 1u);
            // Toon pixels (keyed on the shading model) get their own metering weight.
            if (g_materials[matIdx].customShadingModel == SHADING_MODEL_TOON)
                weight *= max(g_nprWeight, 0.0);
        }

        // Percentile clipping catches isolated fireflies; this taper also keeps a large emissive
        // sign or sun disk from dominating before it reaches the clipped tail.
        float highlight = smoothstep(g_hiStart, g_hiEnd, luma);
        weight *= lerp(1.0, saturate(g_highlightWeight), highlight);

        // Fixed-point weights for the uint histogram atomics. A non-zero weight floors at 1, not 0:
        // a small uniform scale (skyWeight 0.05 on an all-sky frame) must not look like an explicit
        // exclusion, which would empty the histogram and freeze adaptation.
        uint quantizedWeight = (weight > 0.0)
            ? max(1u, (uint)min(floor(weight * WEIGHT_SCALE + 0.5), float(WEIGHT_MAX)))
            : 0u;
        if (quantizedWeight != 0u)
            InterlockedAdd(gs_histogram[bin], quantizedWeight);
    }
    else if (inRange)
    {
        // Count non-finite pixels: one device atomic each, so a clean frame pays nothing;
        // CSHistogramAverage hands the count to the CPU.
        InterlockedAdd(g_histogram[NONFINITE_SLOT], 1u);
    }
    GroupMemoryBarrierWithGroupSync();

    // Flush groupshared -> global, one bin per thread (16x16 == BINS). Empty bins are skipped:
    // most tiles touch only a handful of the 256.
    if (flatLocal < HISTOGRAM_BINS)
    {
        uint v = gs_histogram[flatLocal];
        if (v != 0u)
            InterlockedAdd(g_histogram[flatLocal], v);
    }
}

// ==== CSHistogramAverage: single group, weighted average ====
groupshared uint  gs_hist[HISTOGRAM_BINS];

void AdaptExposure(uint bin)
{
    uint count = g_histogram[bin];

    // Clear for the next frame.
    g_histogram[bin] = 0;

    gs_hist[bin] = count;
    GroupMemoryBarrierWithGroupSync();

    // Single-thread reduction
    if (bin == 0)
    {
        // Publish the non-finite count in exposure[1] (AutoExposurePass reads it back with the
        // exposure) and clear it; before the empty-histogram return so an all-NaN frame still reports.
        // Store it as a float value, not asfloat(bits): a small count's bit pattern is a denormal a
        // float store may flush to zero. Exact below 2^24 (a 4K frame is 8.3M pixels).
        g_exposure[1] = float(g_histogram[NONFINITE_SLOT]);
        g_histogram[NONFINITE_SLOT] = 0;

        uint totalWeight = 0;
        for (uint sumBin = 0; sumBin < HISTOGRAM_BINS; ++sumBin)
            totalWeight += gs_hist[sumBin];
        if (totalWeight == 0u)
            return; // every semantic class was explicitly excluded; retain exposure
        uint  lowCut  = (uint)(g_lowPercent  * float(totalWeight));
        uint  highCut = (uint)(g_highPercent * float(totalWeight));

        uint  cumul     = 0;
        float sumLogL   = 0.0f;
        uint  countUsed = 0;

        for (uint histBin = 0; histBin < HISTOGRAM_BINS; ++histBin)
        {
            uint n = gs_hist[histBin];
            uint newCumul = cumul + n;

            // Overlap with [lowCut, highCut].
            uint lo = max(cumul, lowCut);
            uint hi = min(newCumul, highCut);
            if (hi > lo)
            {
                uint used = hi - lo;
                float logL = BinToLogLuma(histBin);
                sumLogL   += logL * float(used);
                countUsed += used;
            }
            cumul = newCumul;
        }

        float targetLogLuma = (countUsed > 0) ? (sumLogL / float(countUsed)) : g_minLogLuma;
        float targetLuma    = exp2(targetLogLuma);

        // Exposure = key / average luma, held to [min, max], then x 2^evBias: the bias is the
        // author's compensation on top of the bounds, so it still moves a scene pinned at one.
        // g_keyValue (default 0.18) is the middle-grey target.
        const bool biasFirst = (g_debugFlags & 1u) != 0u;
        float key = max(g_keyValue, 1e-4f);
        float targetExposure = (targetLuma > 1e-5f) ? (key / targetLuma) : 1.0f;
        if (biasFirst) targetExposure *= exp2(g_evBias);
        targetExposure = clamp(targetExposure, g_minExposure, g_maxExposure);
        if (!biasFirst) targetExposure *= exp2(g_evBias);

        // Adapt in log2(exposure), i.e. in stops, so both directions move at the same rate per stop.
        // Lerping the linear multiplier would make the step depend on the level (rate 0.1: 1 -> 8
        // moves +0.77 stops per frame, 8 -> 1 only -0.13), leaving a walk into daylight blown out ~6x
        // longer than the reverse.
        float prevExposure = g_exposure[0];
        if (isnan(prevExposure) || isinf(prevExposure) || prevExposure < 1e-6f) prevExposure = 1.0f;  // bootstrap

        // Scene brighter -> exposure falls -> g_adaptationRate.
        // Scene darker   -> exposure rises -> g_adaptationRateDark.
        float rate = (targetExposure < prevExposure) ? g_adaptationRate
                                                     : g_adaptationRateDark;
        float adaptedLog = lerp(log2(prevExposure), log2(targetExposure), saturate(rate));
        float adaptedExposure = exp2(adaptedLog);
        if (isnan(adaptedExposure) || isinf(adaptedExposure)) adaptedExposure = targetExposure;
        g_exposure[0] = biasFirst ? clamp(adaptedExposure, g_minExposure, g_maxExposure)
                                  : max(adaptedExposure, 1e-6f);
    }
}

[numthreads(HISTOGRAM_BINS, 1, 1)]
void CSHistogramAverage(uint3 id : SV_DispatchThreadID)
{
    // Export only (a held or manual exposure): no metering, the texture takes the buffer's value. The
    // flag is uniform, so the exit skips the group sync for the whole group.
    if ((g_debugFlags & 4u) == 0u)
        AdaptExposure(id.x);
    // One thread, after its own write to g_exposure[0] (or the frame's held value).
    if (id.x == 0 && (g_debugFlags & 6u) != 0u)
        g_exposureTex[uint2(0, 0)] = g_exposure[0];
}
