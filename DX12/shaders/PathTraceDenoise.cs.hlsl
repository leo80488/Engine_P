// Denoises the path tracer's image while it is young (PathTracePass::RecordDenoise): a frame
// that restarted the progressive mean (the scene or the camera moves) shows one sample per pixel,
// and the first frames after it a handful. An SVGF-style filter over the GBuffer primary:
//
//   Temporal  the image is demodulated by the GBuffer albedo (texture detail survives the blur:
//             it is multiplied back at the end). A restarted frame reprojects the denoiser's
//             own history through the motion vectors (2x2 bilinear, each tap rejected on a view
//             depth or normal mismatch), a running mean capped at kMaxHistory frames, and a
//             sample far above a settled history is clamped. An accumulating frame (the image
//             is already the mean of n samples) takes the mean as it is. Either way the
//             history is written: the first moving frame after a still one starts from the
//             converged image.
//   Variance  the luminance variance the edge-stopping scales with: of the temporal history
//             (restarted frames, 4+ frames long), of the path tracer's samples / n
//             (accumulating frames: PathTrace.cs.hlsl keeps the mean of luminance squared), or
//             of a 3x3 neighbourhood (short histories, the rtxdi core).
//   Atrous    four a-trous passes (steps 1, 2, 4, 8) of a 5x5 B3-spline kernel, stopped by
//             view depth, normal and luminance against the variance; the variance is filtered
//             with the squared weights. The last pass remodulates and writes the image,
//             faded to the plain mean from fadeStart to fadeEnd samples (PathTracePass stops
//             dispatching at fadeEnd, so a converged image is the running mean bit for bit).
//
// FogTemporal / FogAtrous: the same filter over the path tracer's camera fog (FogCamera: rgb the
// light the fog scatters toward the camera, a the segment's transmittance; a running mean that
// restarts whenever the camera moves), changed where the fog differs (PathTracePass::
// RecordFogDenoise):
//   - no albedo, normal or luminance stop: the fog lies in front of the surface and has no detail
//     of its own to keep but its depth edges (FogAtrous says why not luminance);
//   - the sky's pixels are filtered too (the fog over the sky is part of the picture), as their own
//     depth class: view depth 0 stops at every surface;
//   - a restarted frame reprojects its history by the end point's world position (the sky's: its
//     far-plane point, a direction) instead of the motion vectors, which the sky does not have;
//   - no clamp against the history and no variance: the fog keeps its mean, a bright path spreads
//     over the kernel and fades over the history;
//   - the transmittance is filtered with the light.
//
// MotionClear / MotionProbe: how many GBuffer pixels moved on their own this frame - a motion vector
// with the camera still (PathTracePass::RecordMotionProbe): WPO, or anything else the path
// tracer's fingerprints do not see - and how many show a material whose shader reads the time
// (MAT_FLAG_READS_TIME: it animates without moving). The counts reach the CPU a few frames later.
//
// Sky pixels (reverse-Z depth 0) carry variance -1 in the image's filter and are never written. The
// pass's own textures and the image stay in UNORDERED_ACCESS and are read through their UAVs (the
// passes are ordered by UAV barriers); the GBuffer guides and the fog's running mean are SRVs.
// PathTracePass's root signature (b0 + a directly indexed heap).

cbuffer PtDenoiseParams : register(b0)
{
    uint  width, height, depthIdx, normalIdx;
    uint  albedoIdx, velocityIdx, resultUav, sampleMomentUav;
    uint  historyUav, historyMomentsUav, historyMetaUav, historyOutUav;
    uint  historyMomentsOutUav, historyMetaOutUav, filterInUav, filterOutUav;
    uint  step, samples, flags, motionUav;
    float nearZ, farZ, fadeStart, fadeEnd;
    uint  materialsIdx, fogIdx, dnPad0, dnPad1;
    // The fog's reprojection: this frame's inverse view-projection, the history's view-projection.
    float4x4 fogInvViewProj, fogPrevViewProj;
};

// flags
#define PTDN_HISTORY_VALID 1u   // the history textures hold last denoised frame's (no cut, no resize)
#define PTDN_RESTARTED     2u   // this frame's image is one sample: reproject the history
#define PTDN_LAST          4u   // the last a-trous pass: remodulate into the image

#include "core/depth.hlsli"   // LinearizeReverseZ
#include "core/color.hlsli"   // Luminance
#include "core/gbuffer_codec.hlsli"   // GBufferDecodeMatIdx
#include "material.hlsli"   // MaterialGPUData, MAT_FLAG_READS_TIME

static const float kMaxHistory  = 16.0;
static const float kMinAlbedo   = 0.02;
static const float kMaxIllum    = 30000.0;   // fp16 history headroom
static const float kPhiLuminance = 4.0;
static const float kPhiNormal    = 64.0;
static const float kPhiDepth     = 0.01;

float DnViewZ(uint2 px)
{
    Texture2D<float> depthTex = ResourceDescriptorHeap[depthIdx];
    const float d = depthTex.Load(int3(px, 0));
    return d > 0.0 ? LinearizeReverseZ(d, nearZ, farZ) : 0.0;
}
float3 DnNormal(uint2 px)
{
    Texture2D<float4> normalTex = ResourceDescriptorHeap[normalIdx];
    return normalize(normalTex.Load(int3(px, 0)).xyz * 2.0 - 1.0);
}
float3 DnAlbedo(uint2 px)
{
    Texture2D<float4> albedoTex = ResourceDescriptorHeap[albedoIdx];
    return max(albedoTex.Load(int3(px, 0)).rgb, kMinAlbedo);
}
float3 DnIllum(uint2 px)
{
    RWTexture2D<float4> image = ResourceDescriptorHeap[resultUav];
    return min(max(image[px].rgb, 0.0) / DnAlbedo(px), kMaxIllum);
}
float DnLum(float3 c) { return Luminance(max(c, 0.0)); }

[numthreads(8,8,1)]
void Temporal(uint3 tid : SV_DispatchThreadID)
{
    const uint2 px = tid.xy;
    if (any(px >= uint2(width, height))) return;
    RWTexture2D<float4> histOut    = ResourceDescriptorHeap[historyOutUav];
    RWTexture2D<float2> momentsOut = ResourceDescriptorHeap[historyMomentsOutUav];
    RWTexture2D<float4> metaOut    = ResourceDescriptorHeap[historyMetaOutUav];
    RWTexture2D<float4> filterOut  = ResourceDescriptorHeap[filterOutUav];
    const float viewZ = DnViewZ(px);
    if (viewZ <= 0.0)
    {
        histOut[px] = 0; momentsOut[px] = 0; metaOut[px] = 0;
        filterOut[px] = float4(0, 0, 0, -1);
        return;
    }
    const float3 n = DnNormal(px);
    float3 illum = DnIllum(px);
    const float lum = DnLum(illum);

    float3 acc = illum;
    float2 moments = float2(lum, lum * lum);
    float len = 1.0;
    if (flags & PTDN_RESTARTED)
    {
        float3 h = 0;
        float2 hm = 0;
        float hl = 0, wsum = 0;
        if (flags & PTDN_HISTORY_VALID)
        {
            Texture2D<float2> velocityTex = ResourceDescriptorHeap[velocityIdx];
            RWTexture2D<float4> histTex   = ResourceDescriptorHeap[historyUav];
            RWTexture2D<float2> momTex    = ResourceDescriptorHeap[historyMomentsUav];
            RWTexture2D<float4> metaTex   = ResourceDescriptorHeap[historyMetaUav];
            // The engine's motion is the NDC delta (cur - prev).
            const float2 ndcDelta = velocityTex.Load(int3(px, 0));
            const float2 prevPos = (float2(px) + 0.5) + float2(-0.5 * ndcDelta.x, 0.5 * ndcDelta.y) * float2(width, height) - 0.5;
            const int2 base = int2(floor(prevPos));
            const float2 f = prevPos - float2(base);
            [unroll]
            for (int i = 0; i < 4; ++i)
            {
                const int2 q = base + int2(i & 1, i >> 1);
                if (any(q < 0) || any(q >= int2(width, height))) continue;
                const float4 meta = metaTex[q];
                if (meta.w <= 0.0 || abs(meta.w - viewZ) > 0.05 * viewZ + 0.02) continue;
                if (dot(meta.xyz, n) < 0.9) continue;
                const float w = ((i & 1) ? f.x : 1.0 - f.x) * ((i >> 1) ? f.y : 1.0 - f.y);
                const float4 hq = histTex[q];
                h += hq.rgb * w; hl += hq.a * w; hm += momTex[q] * w; wsum += w;
            }
        }
        if (wsum > 1e-3)
        {
            h /= wsum; hm /= wsum; hl /= wsum;
            if (hl >= 4.0)
            {
                // One bright path (an emitter seen through a gap) must not stain kMaxHistory frames.
                const float settled = 8.0 * DnLum(h) + 0.05;
                if (lum > settled) illum *= settled / lum;
            }
            len = min(hl + 1.0, kMaxHistory);
            acc = lerp(h, illum, 1.0 / len);
            const float l = DnLum(illum);
            moments = lerp(hm, float2(l, l * l), max(1.0 / len, 0.2));
        }
    }
    else
    {
        // The running mean of `samples` paths: its history length is the sample count.
        len = min(float(samples), 255.0);
    }

    float variance;
    if (!(flags & PTDN_RESTARTED) && sampleMomentUav != 0)
    {
        // Accumulating: the paths' luminance variance over the samples, demodulated.
        RWTexture2D<float>  sampleMoment = ResourceDescriptorHeap[sampleMomentUav];
        RWTexture2D<float4> image        = ResourceDescriptorHeap[resultUav];
        const float meanLum = DnLum(image[px].rgb);
        const float albedoLum = max(DnLum(DnAlbedo(px)), kMinAlbedo);
        variance = max(sampleMoment[px] - meanLum * meanLum, 0.0) /
                   (max(float(samples), 1.0) * albedoLum * albedoLum);
    }
    else if ((flags & PTDN_RESTARTED) && len >= 4.0)
    {
        variance = max(moments.y - moments.x * moments.x, 0.0);
    }
    else
    {
        // A short history: the 3x3 neighbourhood's spread stands in, at least as large as one
        // sample's (it also holds the image's own detail).
        float s1 = 0, s2 = 0, cnt = 0;
        [unroll]
        for (int y = -1; y <= 1; ++y)
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const int2 q = int2(px) + int2(x, y);
            if (any(q < 0) || any(q >= int2(width, height))) continue;
            if (DnViewZ(q) <= 0.0) continue;
            const float l = DnLum(DnIllum(q));
            s1 += l; s2 += l * l; cnt += 1.0;
        }
        s1 /= max(cnt, 1.0); s2 /= max(cnt, 1.0);
        variance = max(s2 - s1 * s1, 0.0) * 4.0 / len;
    }

    histOut[px]    = float4(acc, len);
    momentsOut[px] = moments;
    metaOut[px]    = float4(n, viewZ);
    filterOut[px]  = float4(acc, min(variance, kMaxIllum));
}

[numthreads(8,8,1)]
void Atrous(uint3 tid : SV_DispatchThreadID)
{
    const uint2 px = tid.xy;
    if (any(px >= uint2(width, height))) return;
    RWTexture2D<float4> inTex = ResourceDescriptorHeap[filterInUav];
    const float4 c = inTex[px];
    if (c.a < 0.0)
    {
        if (!(flags & PTDN_LAST)) { RWTexture2D<float4> outTex = ResourceDescriptorHeap[filterOutUav]; outTex[px] = c; }
        return;
    }
    const float viewZ = DnViewZ(px);
    const float3 n = DnNormal(px);
    const float lum = DnLum(c.rgb);

    // The variance this pass stops on: a 3x3 Gaussian of it (one noisy estimate must not decide).
    float gaussVar = 0, gw = 0;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    [unroll]
    for (int x = -1; x <= 1; ++x)
    {
        const int2 q = int2(px) + int2(x, y);
        if (any(q < 0) || any(q >= int2(width, height))) continue;
        const float v = inTex[q].a;
        if (v < 0.0) continue;
        const float w = (x == 0 ? 0.5 : 0.25) * (y == 0 ? 0.5 : 0.25);
        gaussVar += v * w; gw += w;
    }
    gaussVar /= max(gw, 1e-6);
    const float phiL = kPhiLuminance * sqrt(max(gaussVar, 0.0)) + 1e-4;
    const float phiZ = kPhiDepth * viewZ * float(step) + 1e-4;

    static const float kKernel[3] = { 3.0 / 8.0, 1.0 / 4.0, 1.0 / 16.0 };
    float3 sum = 0;
    float sumVar = 0, wsum = 0;
    [unroll]
    for (int j = -2; j <= 2; ++j)
    [unroll]
    for (int i = -2; i <= 2; ++i)
    {
        const int2 q = int2(px) + int2(i, j) * int(step);
        if (any(q < 0) || any(q >= int2(width, height))) continue;
        const float4 s = inTex[q];
        if (s.a < 0.0) continue;
        float w = kKernel[abs(i)] * kKernel[abs(j)];
        if (i != 0 || j != 0)
        {
            const float zq = DnViewZ(q);
            w *= exp(-abs(zq - viewZ) / phiZ);
            w *= pow(saturate(dot(n, DnNormal(q))), kPhiNormal);
            w *= exp(-abs(DnLum(s.rgb) - lum) / phiL);
        }
        sum += s.rgb * w; sumVar += s.a * w * w; wsum += w;
    }
    const float3 filtered = sum / wsum;
    const float variance = sumVar / (wsum * wsum);
    if (!(flags & PTDN_LAST))
    {
        RWTexture2D<float4> outTex = ResourceDescriptorHeap[filterOutUav];
        outTex[px] = float4(filtered, variance);
        return;
    }
    // Remodulate, fade to the plain mean as the samples gather, write the image (alpha kept).
    RWTexture2D<float4> result = ResourceDescriptorHeap[resultUav];
    const float4 mean = result[px];
    float3 outColor = filtered * DnAlbedo(px);
    if (fadeEnd > fadeStart)
        outColor = lerp(outColor, mean.rgb, saturate((float(samples) - fadeStart) / (fadeEnd - fadeStart)));
    result[px] = float4(outColor, mean.a);
}

// ==== The camera fog ====
static const float kPhiFogDepth = 0.05;   // the fog changes slowly with the segment's length

[numthreads(8,8,1)]
void FogTemporal(uint3 tid : SV_DispatchThreadID)
{
    const uint2 px = tid.xy;
    if (any(px >= uint2(width, height))) return;
    Texture2D<float4> fogTex  = ResourceDescriptorHeap[fogIdx];
    Texture2D<float> depthTex = ResourceDescriptorHeap[depthIdx];
    const float4 fog = fogTex.Load(int3(px, 0));
    const float depth = depthTex.Load(int3(px, 0));
    const float viewZ = depth > 0.0 ? LinearizeReverseZ(depth, nearZ, farZ) : 0.0;

    float4 acc = fog;
    float len;
    if (flags & PTDN_RESTARTED)
    {
        len = 1.0;
        float4 h = 0;
        float hl = 0, wsum = 0;
        if (flags & PTDN_HISTORY_VALID)
        {
            const float2 uv = (float2(px) + 0.5) / float2(width, height);
            const float4 clip = mul(float4(WorldPosFromDepth(uv, depth, fogInvViewProj), 1.0), fogPrevViewProj);
            if (clip.w > 1e-6)
            {
                RWTexture2D<float4> histTex = ResourceDescriptorHeap[historyUav];
                RWTexture2D<float2> metaTex = ResourceDescriptorHeap[historyMetaUav];
                const float2 prevPos = (clip.xy / clip.w * float2(0.5, -0.5) + 0.5) * float2(width, height) - 0.5;
                const int2 base = int2(floor(prevPos));
                const float2 f = prevPos - float2(base);
                [unroll]
                for (int i = 0; i < 4; ++i)
                {
                    const int2 q = base + int2(i & 1, i >> 1);
                    if (any(q < 0) || any(q >= int2(width, height))) continue;
                    const float2 meta = metaTex[q];   // history length, view depth (0: the sky)
                    if ((meta.y > 0.0) != (viewZ > 0.0)) continue;
                    if (viewZ > 0.0 && abs(meta.y - clip.w) > 0.05 * clip.w + 0.02) continue;
                    const float w = ((i & 1) ? f.x : 1.0 - f.x) * ((i >> 1) ? f.y : 1.0 - f.y);
                    h += histTex[q] * w; hl += meta.x * w; wsum += w;
                }
            }
        }
        if (wsum > 1e-3)
        {
            h /= wsum; hl /= wsum;
            len = min(hl + 1.0, kMaxHistory);
            acc = lerp(h, fog, 1.0 / len);
        }
    }
    else
    {
        // The running mean of `samples` paths: its history length is the sample count.
        len = min(float(samples), 255.0);
    }
    RWTexture2D<float4> histOut = ResourceDescriptorHeap[historyOutUav];
    RWTexture2D<float2> metaOut = ResourceDescriptorHeap[historyMetaOutUav];
    histOut[px] = acc;   // also the first a-trous pass's input
    metaOut[px] = float2(len, viewZ);
}

// Light and transmittance alike, stopped by view depth alone: a luminance stop as the image's (the
// centre's variance against each neighbour) holds a bright path in place and drops its light from
// the neighbours instead of spreading it - the fog's mean fell 5-9 % that way.
[numthreads(8,8,1)]
void FogAtrous(uint3 tid : SV_DispatchThreadID)
{
    const uint2 px = tid.xy;
    if (any(px >= uint2(width, height))) return;
    RWTexture2D<float4> inTex = ResourceDescriptorHeap[filterInUav];
    const float viewZ = DnViewZ(px);
    const float phiZ = kPhiFogDepth * viewZ * float(step) + 1e-4;

    static const float kKernel[3] = { 3.0 / 8.0, 1.0 / 4.0, 1.0 / 16.0 };
    float4 sum = 0;
    float wsum = 0;
    [unroll]
    for (int j = -2; j <= 2; ++j)
    [unroll]
    for (int i = -2; i <= 2; ++i)
    {
        const int2 q = int2(px) + int2(i, j) * int(step);
        if (any(q < 0) || any(q >= int2(width, height))) continue;
        const float w = kKernel[abs(i)] * kKernel[abs(j)] * exp(-abs(DnViewZ(q) - viewZ) / phiZ);
        sum += inTex[q] * w; wsum += w;
    }
    float4 filtered = sum / wsum;
    if (!(flags & PTDN_LAST))
    {
        RWTexture2D<float4> outTex = ResourceDescriptorHeap[filterOutUav];
        outTex[px] = filtered;
        return;
    }
    // Fade to the running mean as the samples gather, write the target.
    Texture2D<float4> fogTex = ResourceDescriptorHeap[fogIdx];
    if (fadeEnd > fadeStart)
        filtered = lerp(filtered, fogTex.Load(int3(px, 0)), saturate((float(samples) - fadeStart) / (fadeEnd - fadeStart)));
    RWTexture2D<float4> result = ResourceDescriptorHeap[resultUav];
    result[px] = filtered;
}

// ==== Object motion on screen ====
static const float kMotionPx = 1.0e-3;   // pixels; a still pixel's motion vector is exactly 0

[numthreads(1,1,1)]
void MotionClear(uint3 tid : SV_DispatchThreadID)
{
    RWStructuredBuffer<uint> counter = ResourceDescriptorHeap[motionUav];
    counter[0] = 0;
    counter[1] = 0;
}

[numthreads(8,8,1)]
void MotionProbe(uint3 tid : SV_DispatchThreadID)
{
    const uint2 px = tid.xy;
    bool moved = false, timed = false;
    if (all(px < uint2(width, height)) && DnViewZ(px) > 0.0)
    {
        Texture2D<float2> velocityTex = ResourceDescriptorHeap[velocityIdx];
        // The engine's motion is the NDC delta (cur - prev): half the viewport per unit.
        const float2 deltaPx = velocityTex.Load(int3(px, 0)) * float2(width, height) * 0.5;
        moved = any(abs(deltaPx) > kMotionPx);
        if (materialsIdx != 0)
        {
            Texture2D<float4> normalTex = ResourceDescriptorHeap[normalIdx];
            StructuredBuffer<MaterialGPUData> materials = ResourceDescriptorHeap[materialsIdx];
            uint count, stride;
            materials.GetDimensions(count, stride);
            const uint material = GBufferDecodeMatIdx(normalTex.Load(int3(px, 0)).a);
            timed = material < count && (materials[material].materialFlags & MAT_FLAG_READS_TIME) != 0u;
        }
    }
    const uint movedCount = WaveActiveCountBits(moved);
    const uint timedCount = WaveActiveCountBits(timed);
    if ((movedCount | timedCount) != 0 && WaveIsFirstLane())
    {
        RWStructuredBuffer<uint> counter = ResourceDescriptorHeap[motionUav];
        if (movedCount != 0) InterlockedAdd(counter[0], movedCount);
        if (timedCount != 0) InterlockedAdd(counter[1], timedCount);
    }
}
