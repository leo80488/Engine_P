#ifndef SSGI_COMMON_HLSLI
#define SSGI_COMMON_HLSLI

// SSGICommon.hlsli - shared CB + register map for the hybrid diffuse-GI pass.
// Denoiser: NRD REBLUR_DIFFUSE.
//
// One dedicated compute root signature (SSGIPass.cpp). This file is the single
// source of truth for the layout; keep the C++ TableDef in sync:
//   Plain:     SSGITrace -> [NRD REBLUR_DIFFUSE] -> SSGIResolve
//   Reservoir: SSGITrace(candidate + temporal reuse)
//               -> SSGIReservoirSpatial -> [NRD] -> SSGIResolve
//
//   [0]  ROOT_CBV     b0  space0  SSGIConstants (this file)
//   [1]  TABLE 1 SRV  t1  space0  gDepth        (main depth, reverse-Z)
//   [2]  TABLE 1 SRV  t2  space0  gNormal       (post-decal GBuffer normal, *2-1)
//   [3]  TABLE 1 SRV  t3  space0  gVelocity     (raw NDC delta curNDC-prevNDC)
//   [4]  TABLE 1 SRV  t4  space0  gDepthHier    (2-ch Hi-Z, .r = max = nearest
//                                 under reverse-Z; grass-free SSRTraceDepth,
//                                 owned/built by SSGIPass; RT reflections may
//                                 borrow it later in the same graph)
//   [5]  TABLE 1 SRV  t5  space0  gPrevHDR      (prev-frame scene-colour pyramid)
//   [6]  TABLE 1 SRV  t6  space0  gDiffDenoised (NRD OUT_DIFF_RADIANCE_HITDIST)
//   [7]  TABLE 1 SRV  t7  space0  gRawCoverage  (R16F confidence coverage)
//   [8]  TABLE 1 SRV  t8  space0  gDDGIVolumes  (StructuredBuffer<DDGIVolumeGPU>)
//   [9]  TABLE N SRV  t0  space4  gDDGIIrradiance[DDGI_MAX_VOLUMES]
//   [10] TABLE N SRV  t0  space5  gDDGIDepth     [DDGI_MAX_VOLUMES]
//   [11] TABLE N SRV  t0  space6  gDDGIProbeData [DDGI_MAX_VOLUMES]
//   [12] TABLE 1 UAV  u0  space0  OutRawCoverage (R16F confidence coverage)
//   [13] TABLE 1 UAV  u1  space0  OutNrdNormalRough (R10G10B10A2)
//   [14] TABLE 1 UAV  u2  space0  OutNrdViewZ       (R32F, +linear, sky = 1e7)
//   [15] TABLE 1 UAV  u3  space0  OutNrdMV          (RG16F, UV-space prevUV-uv)
//   [16] TABLE 1 UAV  u4  space0  OutNrdDiffIn      (RGBA16F radiance+normHitDist)
//   [17] TABLE 1 UAV  u5  space0  OutResult     (-> LightCB.ssgiResultIdx)
//   [18] TABLE 1 SRV  t9  space0  gCovHistory   (previous filtered coverage)
//   [19] TABLE 1 UAV  u6  space0  OutCovHistory (this frame's coverage)
//   [20] TABLE 1 SRV  t10 space0  gPrevDepth    (previous full-res reverse-Z)
//   [21] TABLE 1 UAV  u7  space0  OutDepthHistory (current full-res depth)
//   [22..25] SRV t11..t14  previous reservoir direction/radiance/aux/meta
//   [26..29] SRV t15..t18  current temporal reservoir
//   [30..33] UAV u8..u11   current temporal reservoir
//   [34..37] UAV u12..u15  current spatial/final reservoir history
//   s0 space0 - static linear CLAMP (pyramid / history)
//   s1 space0 - static linear WRAP  (DDGI atlases)
//
// The DDGI arrays live in their own spaces (4/5/6) at t0, DDGI_MAX_VOLUMES
// wide, so their width is unconstrained. Do not fold them back into consecutive
// space0 registers: that caps kMaxVolumes at whatever gap is free and dxc
// hard-errors past it.
//
// Reverse-Z everywhere: bigger NDC z = nearer; sky/cleared depth is exactly
// 0.0 - always test `depth <= 0.0`, never an epsilon.

#include "DDGICommon.hlsli"
#include "DDGISampling.hlsli"

cbuffer SSGIConstants : register(b0, space0)
{
    float4x4 invViewProj;        // inverse of the current-frame jittered viewProj
    float4x4 viewProj;           // current jittered viewProj

    float3   cameraPos;          float  maxDistance;   // screen-march world range (m)
    float3   _ssgiPad0;          float  thickness;     // depth-buffer thickness floor (wu)

    uint     screenW;            uint   screenH;
    float    invScreenW;         float  invScreenH;

    float    nearZ;              float  farZ;
    uint     frameIndex;         uint   rayCount;      // cosine rays per pixel per frame

    uint     hizMipCount;        // gDepthHier mip count (0 = no Hi-Z -> pass inert)
    uint     maxTraversalIters;  // Hi-Z loop cap
    uint     hasPrevHdr;         // 1 = gPrevHDR bound AND its content is valid
    float    prevHdrMaxMip;      // pyramid mipCount - 1

    uint     ddgiVolumeCount;
    uint     ddgiEnabled;
    float    normalBias;         // ray-origin offset along N (wu)
    // REBLUR hitDistanceParameters.A. SSGINRDPack normalizes with this and
    // REBLUR denormalizes with nrd::ReblurSettings::hitDistanceParameters.A, fed
    // from NrdDenoiser::Params::diffHitDistScaleA. They must be the same number: a
    // mismatch produces no error and no log line, only a globally wrong blur
    // radius and wrong temporal reprojection.
    float    hitDistParamA;

    // Debug / bring-up.
    //   0 = normal operation
    //   1 = calibration: skip the march entirely, output the far-field DDGI
    //       irradiance with resolvedFraction = 1. Lighting.ps must then render
    //       a frame identical (to float error) to SSGI-off, which proves the
    //       units, the partition algebra and the whole binding chain in one test.
    uint     debugMode;
    // 0 on the first frame after enable / resize / camera cut: SSGIResolve must
    // then take this frame's spatial estimate whole instead of blending into a
    // history that describes a different image.
    uint     covHistoryValid;
    float2   historyJitterDelta; // previous minus current, in full-resolution pixels

    // ---- Half-resolution tracing ----
    // The trace, the NRD input quartet and the denoiser all run at half the
    // render resolution; only SSGIResolve runs full-res, bilaterally upsampling
    // as it writes. screenW/H above stay the full render size: the Hi-Z the march
    // walks is full-res, so every screen-space step, tolerance and self-hit
    // distance must keep counting in full-res pixels, or the march silently halves
    // its step density and steps over thin geometry.
    //
    // A half-res thread maps to full-res pixel (traceCoord * 2): the trace reads
    // the G-buffer there, and SSGIResolve uses the same relation to find each
    // half-res sample's depth/normal guide without a second set of half-res guide
    // textures.
    uint     traceW;             uint  traceH;
    float    invTraceW;          float invTraceH;

    // A screen hit is accepted as a previous-frame radiance sample only when its
    // current world position projects onto compatible previous-frame depth, so
    // disocclusions and camera cuts do not turn stale HDR into bounce light.
    float4x4 prevViewProj;        // previous jittered viewProj

    // The previous inverse reconstructs the old receiver position for temporal
    // reservoir samples. The selected hit is stored compactly as an octahedral
    // direction + distance relative to that receiver, avoiding a 16-byte FP32
    // world-position texture per reservoir set.
    float4x4 prevInvViewProj;

    // ---- Half-resolution spatiotemporal reservoir ----
    // When enabled, Trace emits exactly one new candidate per half-res pixel
    // (0.25 full-res rpp), merges previous history, and Spatial reconnects up
    // to eight neighbouring selected samples before NRD sees the result.
    uint reservoirEnabled;
    uint reservoirHistoryValid;
    uint reservoirSpatialCandidates;
    uint reservoirMaxM;

    uint reservoirMaxAge;
    uint reservoirValidationSteps;
    float reservoirNormalThreshold;
    float reservoirDepthThreshold;
};

// ==== Shared helpers ====

#include "restir_reprojection.hlsli"
float2 SSGI_PreviousUV(float2 uv,float2 ndcMotion)
{
    return uv-float2(ndcMotion.x*.5,-ndcMotion.y*.5)
        +historyJitterDelta/float2(screenW,screenH);
}

// Reverse-Z linearisation. Sky (depth == 0) would divide by zero, so callers
// must gate on `depth <= 0.0` before calling this.
float SSGI_LinearDepth(float ndcZ)
{
    return (nearZ * farZ) / max(ndcZ * (farZ - nearZ) + nearZ, 1e-8);
}

float3 SSGI_WorldFromUVDepth(float2 uv, float ndcZ)
{
    const float4 ndc = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, ndcZ, 1.0);
    const float4 ws  = mul(ndc, invViewProj);
    return ws.xyz / ws.w;
}

// Hilbert-curve + R2 low-discrepancy sequence, pure ALU (the XeGTAO pattern,
// the same one HybridRefl uses). NRD wants a blue-noise-like spatial
// distribution; white noise here shows up as clumping that REBLUR smears into
// paisley swirls.
#include "core/hash.hlsli"

float2 SSGI_Rand2(uint2 pixel, uint sampleIdx)
{
    const uint hilbert = HilbertIndex64(pixel & 63u);
    // Cranley-Patterson rotation shared by all samples of this pixel/frame.
    // Hilbert ordering keeps neighbouring pixels blue-noise-like; the explicit
    // per-ray strata below are the important low-spp correction: with every ray
    // advancing R2 independently, a two-ray pixel can put both samples in the same
    // radial half of the cosine hemisphere.
    const uint n = hilbert + 288u * frameIndex;
    const uint kA = 2654435769u;   // 2^32 / phi
    const uint kB = 1779033703u;   // 2^32 / phi^2
    const float2 rotation =
        float2((n * kA) & 0xFFFFFFFFu, (n * kB) & 0xFFFFFFFFu) * 2.3283064e-10;

    const float invN = rcp(float(max(rayCount, 1u)));
    return float2((float(sampleIdx) + rotation.x) * invN,
                  frac(rotation.y + float(sampleIdx) * 0.61803398875));
}

float3 SSGI_WorldFromPrevUVDepth(float2 uv, float ndcZ)
{
    const float4 ndc = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, ndcZ, 1.0);
    const float4 ws  = mul(ndc, prevInvViewProj);
    return ws.xyz / ws.w;
}

// Cosine-weighted hemisphere sample around N (Malley's method). Returns the
// direction; the pdf cancels against the cosine in the estimator, which is why
// the integrator is a plain mean x PI.
float3 SSGI_CosineHemisphere(float3 N, float2 u)
{
    const float r   = sqrt(u.x);
    const float phi = 6.28318530718 * u.y;
    float3 t = abs(N.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    const float3 tx = normalize(cross(t, N));
    const float3 ty = cross(N, tx);
    return normalize(tx * (r * cos(phi)) + ty * (r * sin(phi))
                     + N * sqrt(max(1.0 - u.x, 0.0)));
}

#endif // SSGI_COMMON_HLSLI
