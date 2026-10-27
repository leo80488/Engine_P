// SSRResolve.cs.hlsl - spatial BRDF reweighting of the Hi-Z SSR trace (pass 3). SSRTemporal runs
// next; SSRUpsample then bilateral-blurs rough regions using the variance written here.
// Inputs:
//   t3 hitBuffer   color.rgb + conf.a from the trace
//   t4 rayDirPDF   world-space L + sampled PDF
//   t5 rayLength   world-space ray distance
//   t6 rayClass    geometry / sky / edge miss / thickness miss
// Outputs:
//   u0 resolveColor  RGBA16F  reweighted reflection (rgb) + accumulated confidence (a)
//   u1 variance      R16F     Welford weighted variance of sample luminance
//   u2 reprojDepth   R16F     NDC depth of the CLOSEST neighbour hit (linear depth + closest
//                             rayLength, re-encoded); temporal reprojects along the reflection with it

cbuffer SSRResolveCB : register(b0, space2)
{
    float4x4 invViewProj;
    float3   cameraPos;   float  _pad0;
    // Dispatch and outputs are full-res (traceW/H == renderW/H). Both names stay so a half-res trace
    // can return with a TAA-independent denoiser.
    uint     traceW;      uint   traceH;
    float    invTraceW;   float  invTraceH;
    float    nearZ;       float  farZ;
    uint     frameIndex;  uint   renderW;
    float    fireflyCap;  uint   renderH;
};

Texture2D<float4>    gNormal    : register(t0, space2);
Texture2D<float4>    gSurface   : register(t1, space2);
Texture2D<float>     gDepth     : register(t2, space2);
Texture2D<float4>    gHitBuffer : register(t3, space2);
Texture2D<float4>    gRayDirPDF : register(t4, space2);
Texture2D<float>     gRayLength : register(t5, space2);
Texture2D<uint>      gRayClass  : register(t6, space2);

RWTexture2D<float4>  OutColor        : register(u0, space2);
RWTexture2D<float>   OutVariance     : register(u1, space2);
RWTexture2D<float>   OutReprojDepth  : register(u2, space2);

#include "core/math.hlsli"   // PI

#include "indirect_specular.hlsli"

// ==== Helpers ====
#include "core/depth.hlsli"   // LinearizeReverseZ, WorldPosFromDepth
float3 ReconstructWorldPos(float2 uv, float depth)
{
    return WorldPosFromDepth(uv, depth, invViewProj);
}

float LinearizeReverseZ(float zNdc)
{
    return LinearizeReverseZ(zNdc, nearZ, farZ);
}

// Inverse of LinearizeReverseZ: linear eye-space Z -> reverse-Z NDC z (for reprojDepth).
float InverseLinearDepth(float linZ)
{
    return (nearZ * farZ / max(linZ, 1e-3) - nearZ) / (farZ - nearZ);
}

float D_GGX(float NdotH, float a2)
{
    float d = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-6);
}

float V_SmithGGXCorrelated(float NdotV, float NdotL, float a2)
{
    float GGXV = NdotL * sqrt(NdotV * NdotV * (1.0 - a2) + a2);
    float GGXL = NdotV * sqrt(NdotL * NdotL * (1.0 - a2) + a2);
    return 0.5 / max(GGXV + GGXL, 1e-6);
}

#include "core/color.hlsli"   // Luminance

// Parallax-corrected, lobe-aligned BRDF resolve weight (Stachowiak, SIGGRAPH 2015, "Stochastic
// Screen-Space Reflections"). Instead of reusing the neighbour's traced direction, reconstruct its
// hit point and evaluate THIS pixel's GGX BRDF along the direction to it; that keeps glossy
// reflections on curved / oblique surfaces sharp instead of smearing mismatched neighbours in.
float GetWeight(int2 np, float npDepth, float3 P, float3 V, float3 N,
                float roughness, float NdotV, float3 mirrorDir, float selfRayLen,
                uint centerClass)
{
    const uint npClass = gRayClass.Load(int3(np, 0));
    if (npClass > SSR_HIT_SKY) return 0.0;
    if (centerClass <= SSR_HIT_SKY && npClass != centerClass) return 0.0;

    float4 lpdf = gRayDirPDF.Load(int3(np, 0));
    if (lpdf.w <= 0.0 || dot(lpdf.xyz, lpdf.xyz) < 1e-6) return 0.0;

    float3 npL  = normalize(lpdf.xyz);
    float  PDF  = lpdf.w;
    float  npRL = gRayLength.Load(int3(np, 0));

    // Neighbour surface position -> its reflected hit point in world space.
    float3 L = npL;
    if (npClass == SSR_HIT_GEOMETRY)
    {
        float2 npUV   = (float2(np) + 0.5) / float2(renderW, renderH);
        float3 npPos  = ReconstructWorldPos(npUV, npDepth);
        float3 hitPos = npPos + npL * npRL;
        L = normalize(hitPos - P);
    }
    float  NdotL = saturate(dot(N, L));
    if (NdotL <= 0.0) return 0.0;

    float3 H     = normalize(L + V);
    float  NdotH = saturate(dot(N, H));

    float a  = max(roughness, 0.045);
    float a2 = a * a; a2 *= a2;

    float Vis  = V_SmithGGXCorrelated(NdotV, NdotL, a2);
    float D    = D_GGX(NdotH, a2);
    float brdf = Vis * D * NdotL;
    float w    = brdf / max(PDF, 1e-5);

    // Lobe alignment: a GENTLE linear bias toward this pixel's mirror direction. The GGX D term above
    // already weights the lobe sharply; do not use pow(., 2/a) here: it double-suppresses off-lobe
    // neighbours and collapses weightSum on small / curved glossy surfaces into the noisy 1-spp self
    // fallback.
    w *= saturate(dot(L, mirrorDir));

    // Ray-length similarity: a neighbour whose hit is at a very different distance saw different
    // content; soft-reject it with a relative Gaussian. Skipped when this pixel has no valid self hit
    // (selfRayLen ~ 0), so it can still pull a resolve from its neighbourhood.
    if (selfRayLen > 0.1)
    {
        float rlRel = (npRL - selfRayLen) / max(selfRayLen, 0.1);
        w *= exp(-rlRel * rlRel * 2.0);
    }
    return w;
}

// Vogel golden-angle disk: near-blue-noise points in the unit disk at no precompute cost; the
// caller scales by the radius. r uses i/count (not (i+0.5)/count) so sample 0 is EXACTLY the
// center pixel: a guaranteed self tap keeps weightSum non-zero when every other neighbour is
// BRDF-rejected.
float2 VogelDisk(uint i, uint count, float phi)
{
    const float goldenAngle = 2.39996322973;   // radians, PI * (3 - sqrt(5))
    float r     = sqrt(float(i) / float(count));
    float theta = float(i) * goldenAngle + phi;
    return float2(r * cos(theta), r * sin(theta));
}

#include "core/hash.hlsli"

float2 Hammersley2DRandom(uint i, uint N, uint2 random)
{
    uint bits = i;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    float vdc = float(bits ^ random.y) * 2.3283064365386963e-10;
    return float2(frac(float(i) / float(N) +
                        float(random.x & 0xFFFFu) * (1.0 / 65536.0)),
                  vdc);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    // Full-res dispatch: one thread per G-buffer pixel.
    if (DTid.x >= traceW || DTid.y >= traceH) return;

    const int2   pixel = int2(DTid.xy);
    const float2 uv    = (float2(pixel) + 0.5) / float2(renderW, renderH);

    float depth = gDepth.Load(int3(pixel, 0));
    if (depth <= 0.0)
    {
        OutColor[pixel]       = 0;
        OutVariance[pixel]    = 0;
        OutReprojDepth[pixel] = 0;
        return;
    }

    float4 surf = gSurface.Load(int3(pixel, 0));
    float  roughness = max(surf.r, 0.045);
    float3 N = normalize(gNormal.Load(int3(pixel, 0)).rgb * 2.0 - 1.0);
    float3 worldPos = ReconstructWorldPos(uv, depth);
    float3 V = normalize(cameraPos - worldPos);
    float  NdotV = saturate(dot(N, V));
    float  linDepth = LinearizeReverseZ(depth);
    const uint centerClass = gRayClass.Load(int3(pixel, 0));

    // ---- Mirror / near-mirror fast path ----
    // Below roughness 0.1 the GGX lobe is so tight that GetWeight rejects every neighbour and the
    // BRDF loop would fall back to self. Self alone is not enough: the walker steps whole pixels, so
    // adjacent rays that differ by sub-pixel amounts land on hit pixels 1 apart, which shows as
    // stair-steps at high-contrast edges. Upsample's variance-gated blur skips mirrors (variance 0),
    // so smooth HERE with a depth-aware average over same-surface neighbour hits: silhouettes survive
    // (sky / different-depth taps get 0 weight), single-pixel walker jumps blend out.
    if (roughness < 0.1)
    {
        float4 acc   = 0;
        float  wSum  = 0;
        float  selfRL = gRayLength.Load(int3(pixel, 0));

        // 5x5 Gaussian-weighted box over same-surface neighbours: the walker's staircase / mip-stripe
        // artefacts span 2-4 pixels at high-contrast silhouettes, which 3x3 does not cover. The depth
        // weight is relaxed (32, not 64) so a gentle floor slope does not drop neighbours and re-tighten
        // the staircase. Sky taps (sd <= 0) are skipped: they have no plane to align to.
        [unroll] for (int dy = -2; dy <= 2; ++dy)
        [unroll] for (int dx = -2; dx <= 2; ++dx)
        {
            int2 np = clamp(pixel + int2(dx, dy),
                            int2(0, 0),
                            int2(int(renderW) - 1, int(renderH) - 1));
            float4 s  = gHitBuffer.Load(int3(np, 0));
            float  sd = gDepth.Load(int3(np, 0));
            if (sd <= 0.0) continue;
            const uint tapClass = gRayClass.Load(int3(np, 0));
            if (tapClass > SSR_HIT_SKY) continue;
            if (centerClass <= SSR_HIT_SKY && tapClass != centerClass) continue;
            // Taps outside this mirror surface hold zeros: pixels above the trace roughness cutoff are never
            // traced (e.g. shoreline terrain beside mirror water). Averaging them in dilutes confidence and
            // colour along every contact line, which the water shader back-fills with bright sky - a visible
            // band. Missing data is not a black reflection: skip those taps.
            float sRough = max(gSurface.Load(int3(np, 0)).r, 0.045);
            if (sRough >= 0.1) continue;
            float linSd  = LinearizeReverseZ(sd);
            float relDz  = abs(linSd - linDepth) / max(linDepth, 0.01);
            float wDepth = exp(-relDz * relDz * 32.0);
            // Gaussian spatial weight, sigma ~1.5.
            float r2     = float(dx * dx + dy * dy);
            float wSpat  = exp(-r2 / 4.5);
            float w      = wDepth * wSpat;
            acc  += s * w;
            wSum += w;
        }

        float4 outC = (wSum > 1e-5)
            ? (acc / wSum)
            : gHitBuffer.Load(int3(pixel, 0));
        OutColor[pixel]       = outC;
        OutVariance[pixel]    = 0.0;
        OutReprojDepth[pixel] = InverseLinearDepth(linDepth + selfRL);
        return;
    }

    // Sample radius: 2..8 px, scaled by roughness.
    const float  spatialSize = lerp(2.0, 8.0, saturate(roughness * 5.0));
    // 8 taps: pixels whose reprojected history is rejected every frame (small surfaces) live on these
    // single-frame BRDF samples alone, and 4 left their 1-spp trace noise in the output. Raise toward
    // 16 if small-surface noise persists.
    const uint   kSamples    = 8u;

    // Per-pixel Vogel rotation phase, varied per frame for temporal decorrelation.
    uint  hseed = HashWang(pixel.x + pixel.y * renderW + frameIndex * 0x9E3779B9u);
    float phi   = float(hseed & 0xFFFFu) * (6.28318530718 / 65536.0);

    // References for the parallax-corrected, lobe-aligned neighbour weighting.
    float3 Rmirror    = reflect(-V, N);
    float  selfRayLen = gRayLength.Load(int3(pixel, 0));

    float4 accumColor = 0;
    float  weightSum  = 0;
    float  mean = 0, S = 0;               // Welford weighted variance accumulators
    float  nearestRayLength = 1e9;        // the true closest hit (smallest ray length)

    [unroll]
    for (uint i = 0; i < kSamples; ++i)
    {
        // The Vogel disk covers the radius more evenly than Hammersley-in-square at the same budget, and
        // its residual is higher-frequency (easier for temporal + upsample to remove).
        float2 offset = VogelDisk(i, kSamples, phi) * spatialSize;
        int2 np = clamp(pixel + int2(offset),
                        int2(0, 0),
                        int2(int(renderW) - 1, int(renderH) - 1));

        float npDepth = gDepth.Load(int3(np, 0));
        if (npDepth <= 0.0) continue;

        float w = GetWeight(np, npDepth, worldPos, V, N, roughness, NdotV,
                            Rmirror, selfRayLen, centerClass);
        if (w <= 0.0) continue;

        float4 sampleColor = gHitBuffer.Load(int3(np, 0));

        // Karis tone-map round trip (as Wicked / FidelityFX SSSR): accumulate c' = c / (1 + Y), then expand
        // the mean back with c = c' / (1 - Y'), Y' = Luminance(mean c'). Without it one neighbour that
        // hit the sky (HDR luminance 100+) dominates the weighted mean and the reflection shows only sky
        // colour; y / (1 + y) maps [0, inf) to [0, 1) so all samples count on a comparable scale.
        float Yin = max(Luminance(sampleColor.rgb), 0.0);
        float3 toneMapped = sampleColor.rgb * rcp(1.0 + Yin);

        accumColor.rgb += toneMapped * w;
        accumColor.a   += sampleColor.a * w;
        weightSum      += w;

        float lum = Luminance(sampleColor.rgb);
        float oldMean = mean;
        mean += (w / weightSum) * (lum - oldMean);
        S    += w * (lum - oldMean) * (lum - mean);

        if (w > 0.001)
        {
            float rl = gRayLength.Load(int3(np, 0));
            nearestRayLength = min(nearestRayLength, rl);
        }
    }

    float3 colorOut;
    float  confOut;
    if (weightSum > 1e-5)
    {
        accumColor.rgb /= weightSum;
        accumColor.a   /= weightSum;

        // Karis inverse tone map. Yavg is a mean of Y/(1+Y) values, so < 1 in exact math; the floor on
        // (1 - Yavg) only guards FP rounding to 1.0, which would give +Inf and dark speckles after
        // downstream clamps.
        float Yavg     = saturate(Luminance(accumColor.rgb));
        float invScale = rcp(max(1.0 - Yavg, 1e-3));   // caps the amplification at 1000x
        accumColor.rgb = accumColor.rgb * invScale;

        // Firefly cap: a secondary guard on the expanded HDR for true outliers (specular sun hits); the
        // Karis round trip already absorbs most variance. fireflyCap <= 0 disables it.
        if (fireflyCap > 0.0)
        {
            float y = Luminance(accumColor.rgb);
            if (y > fireflyCap) accumColor.rgb *= fireflyCap / y;
        }
        colorOut = accumColor.rgb;
        confOut  = saturate(accumColor.a);
    }
    else
    {
        // No valid neighbour: fall back to self.
        float4 self = gHitBuffer.Load(int3(pixel, 0));
        colorOut = self.rgb;
        confOut  = self.a;
    }

    OutColor[pixel]    = float4(colorOut, confOut);
    OutVariance[pixel] = (weightSum > 1e-5) ? (S / weightSum) : 0.0;

    // Reprojection depth uses the NEAREST neighbourhood hit, so the reflection-following reprojection
    // lands on the visually dominant hit surface.
    if (nearestRayLength >= 1e9) nearestRayLength = 0.0;
    OutReprojDepth[pixel] = InverseLinearDepth(linDepth + nearestRayLength);
}
