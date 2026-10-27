#ifndef HYBRID_REFL_COMMON_HLSLI
#define HYBRID_REFL_COMMON_HLSLI

// HybridReflCommon.hlsli - shared CB, register map and helpers for the hybrid reflection
// pipeline (AMD FidelityFX Hybrid-Reflections architecture, NRD REBLUR_SPECULAR denoiser).
//
// Pipeline (all on ONE dedicated compute root signature built in RTReflectionPass.cpp;
// this map is the single source of truth, keep the C++ TableDef in sync):
//   ClassifyTiles -> PrepareArgsSW -> IntersectSW (indirect) -> PrepareArgsHW ->
//   IntersectHW (indirect) -> NRDPack -> [NRD REBLUR] -> NRDResolve
//
//   [0]  ROOT_CBV        b0  space0  HybridReflConstants (this file)
//   [1]  ROOT_SRV        t0  space0  TLAS (RaytracingAccelerationStructure)
//   [2]  ROOT_SRV        t1  space0  g_Instances (DDGIInstanceData mirror, 128B)
//   [3]  TABLE 1 SRV     t2  space0  gDepth        (main depth - ray ORIGINS only)
//   [4]  TABLE 1 SRV     t3  space0  gNormal       (GBuffer normal, *2-1)
//   [5]  TABLE 1 SRV     t4  space0  gSurface      (.r = perceptual roughness)
//   [6]  TABLE 1 SRV     t5  space0  gVelocity     (raw NDC delta curNDC-prevNDC)
//   [7]  TABLE 1 SRV     t6  space0  gLights       (cluster GPULight buffer)
//   [8]  TABLE 1 SRV     t7  space0  gDDGIVolumes
//   [9]  TABLE N SRV     t0  space4  gDDGIIrradiance[DDGI_MAX_VOLUMES]
//   [10] TABLE N SRV     t0  space5  gDDGIDepth[DDGI_MAX_VOLUMES]
//   [11] TABLE N SRV     t0  space6  gDDGIProbeData[DDGI_MAX_VOLUMES] (one private
//                                    space each: no width cap)
//   [12] TABLE 1 SRV     t20 space0  gPrevHDR      (prev-frame scene-color pyramid)
//   [13] TABLE 1 SRV     t21 space0  gPrevDepth    (prev-frame NDC depth ping-pong)
//   [14] TABLE 1 SRV     t22 space0  gSkySH        (SkyIBL 9 x float4 SH)
//   [15] TABLE 1 SRV     t23 space0  gSkyCube      (TextureCube, prefiltered)
//   [16] TABLE 1 SRV     t24 space0  gSkyIrradiance(TextureCube, cosine-convolved)
//   [17] TABLE 1 SRV     t27 space0  gDepthHier    (2-ch Hi-Z, .r = max = NEAREST
//                                    under reverse-Z; grass-free SSRTraceDepth)
//   [18] TABLE 1 SRV     t28 space0  gRawColor     (intersect output as SRV - pack/resolve)
//   [19] TABLE 1 SRV     t29 space0  gHitDist      (intersect hitT as SRV - pack)
//   [20] TABLE 1 SRV     t30 space0  gNrdSpecOut   (NRD OUT_SPEC_RADIANCE_HITDIST - resolve)
//   [21] TABLE 1 SRV     t31 space0  gHitCounterHist (R32_UINT tile hit-counter, prev frame)
//   [22] TABLE 1 UAV     u0  space0  OutRawColor   (rgb radiance, a = relia)
//   [23] TABLE 1 UAV     u1  space0  OutHitDist    (world-space hit t, R16F)
//   [24] TABLE 1 UAV     u2  space0  g_SWRayList   (uint packed pixel per SW ray)
//   [25] TABLE 1 UAV     u3  space0  g_HWRayList   (uint packed pixel per HW ray)
//   [26] TABLE 1 UAV     u4  space0  g_RayCounter  (8 uints, see HR_CTR_*)
//   [27] TABLE 1 UAV     u5  space0  g_IndirectArgs(8 uints: SW args @0, HW args @16B)
//   [28] TABLE 1 UAV     u6  space0  OutHitCounter (R32_UINT tile hit-counter, this frame)
//   [29] TABLE 1 UAV     u7  space0  OutDenoised   (-> Lighting.ps t44: rgb + a conf)
//   [30] TABLE 1 UAV     u8  space0  OutDepthHistory (this frame NDC depth -> next gPrevDepth)
//   [31] TABLE 1 UAV     u9  space0  OutNrdNormalRough (R10G10B10A2)
//   [32] TABLE 1 UAV     u10 space0  OutNrdViewZ       (R32F, +linear view z, sky=1e7)
//   [33] TABLE 1 UAV     u11 space0  OutNrdMV          (RG16F, UV-space prevUV-uv)
//   [34] TABLE 1 UAV     u12 space0  OutNrdSpecIn      (RGBA16F packed radiance+normHitDist)
//   [35] TABLE N SRV     t0.. space1 bindless ByteAddressBuffer (VB/IB)
//   [36] TABLE N SRV     t0.. space2 bindless Texture2D (albedo/surface/emissive)
//   [37] ROOT_SRV        t32 space0  g_AlphaRecords (RTAlphaRecord, 40B) -
//                                    per-CANDIDATE alpha-cutout test in
//                                    IntersectHW; see rt_alpha_record.hlsli
//   s0 space0 - static linear CLAMP (pyramid / history sampling)
//   s1 space0 - static linear WRAP  (DDGI atlas / material textures)
//
// Each stage declares only the registers it touches; this is the union. REVERSE-Z
// EVERYWHERE: bigger NDC z = nearer; sky/cleared depth is EXACTLY 0.0 - always test
// `depth <= 0.0`, never an epsilon.

cbuffer HybridReflConstants : register(b0, space0)
{
    float4x4 invViewProj;        // inverse of CURRENT-frame jittered viewProj
    float4x4 prevViewProj;       // PREVIOUS-frame jittered viewProj
    float4x4 viewProj;           // current jittered viewProj

    float3   cameraPos;          float rayTMax;              // HW TMax base (default 150)
    float3   sunDirection;       float roughnessThreshold;   // from-light; trace band gate (0.55)
    float3   sunColor;           float mirrorRoughness;      // radiance x intensity; mirror-band roughness (0.1)
    float3   flatAmbient;        float iblStrength;          // None/FlatColor ambient; sky master gate

    uint     screenW;            uint  screenH;
    float    invScreenW;         float invScreenH;

    float    nearZ;              float farZ;
    float    normalBias;         // HW ray-origin offset along N (0.05)
    float    ssrConfidenceThreshold; // SW ValidateHit conf >= this = confident (0.9)

    float    depthThicknessBase; // ValidateHit thickness floor, wu (0.2, AMD default)
    float    thicknessRoughScale;// + alpha(=r^2) x this (10.0, HSR dynamic thickness)
    float    thicknessLenFactor; // + worldRayLen x this (0.01)
    uint     maxTraversalIters;  // Hi-Z loop cap (128, AMD default)

    float    hybridSpawnRate;    // HW->SW random probe rate (0.02)
    float    hybridMissWeight;   // miss penalty in IsSW (0.5)
    uint     tileCountX;         // ceil(screenW/8) - hit-counter dims
    uint     tileCountY;

    uint     frameIndex;
    uint     resetHistory;       // 1 = drop history: screen tier off, hit-counter history = 0
    uint     unifiedFlags;       // bit0 = SW (screen) tier live, bit1 = HW (RT) tier live,
                                 // bit2 = denoiser BYPASSED: NRDPack/REBLUR did not run,
                                 //        NRDResolve passes rawColor through (front-layer instance)
                                 // bit3 = local lights at HW hits UNSHADOWED (A/B arm,
                                 //        -rtr_unshadowed_hit_lights)
    uint     hizMipCount;        // gDepthHier mip count (0 = no Hi-Z)

    uint     ddgiVolumeCount;
    uint     lightCount;
    float    ddgiDiffuseScale;   // LightCB mirror (default 1.0 = physical)
    float    _padSkyDiffuse;     // padding; keeps the layout RTReflectionPass.h's CB mirrors

    uint     hasSkySH;           // 1 = gSkySH bound AND iblUseSH
    uint     hasPrevHdr;         // 1 = gPrevHDR bound and content valid
    float    prevHdrMaxMip;      // pyramid mipCount - 1
    uint     hasSkyCube;         // 1 = gSkyCube holds a real sky (else black fallback)

    uint     skyCubeMips;
    float    ddgiAONearFieldStrength;
    uint     probeCount;         // 0 = probe tier off at HW hits
    uint     missEnvFallback;    // 1 (default) = SSR-only outcomes fall back to the probe -> sky env,
                                 // 0 = conservative zero / conf-0 baseline for A/B

    // Alpha-cutout occluders (rt_alpha_record.hlsli). 0 = table not live this frame -> never
    // touch g_AlphaRecords (its root SRV may be VA 0) and accept every candidate as solid.
    uint     alphaTestEnabled;
    uint     alphaInstanceCount; // bound-check for InstanceID()
    // REBLUR hitDistanceParameters.A = rayTMax * kHitDistAScale (0.075). Mirrored on the C++
    // side by NrdDenoiser::Params::specHitDistScaleA: HybridReflNRDPack NORMALIZES hit distance
    // with this number and REBLUR DENORMALIZES with that one, so they MUST be equal. Scaling
    // with rayTMax keeps every miss (hitT == rayTMax) from saturating normHitDist to 1.0 when
    // the ray budget grows.
    float    hitDistParamA;
    // Bindless g_Buffers[] slot of the persistent MaterialGPUData table
    // (MATERIAL_GPU_DATA_STRIDE, material_layout.generated.hlsli; kInvalidBufferIndex/-1 = not
    // bound). IntersectHW's NPR hit shading reads the per-material toon knobs through it; the
    // HIT's material slot rides rtFlags bits 16..31 (slot+1, 0 = none).
    uint     materialBufIdx;
    // NPRStabilize primary-view parity for reflected NPR hits: the bindless g_Buffers[] slot of
    // the 1-float AutoExposure buffer (>= 16384 = not bound -> exposure 1.0; it holds the same
    // prev-frame value the stabilizer reads, which runs before AutoExposure in the post chain),
    // and the scene-wide NPR exposed-space max-RGB soft ceiling (0 = none). Without them the
    // mirror image misses the EV bias / shoulder / scene ceiling the primary view gets and
    // reads BRIGHTER than the character under strong light.
    uint     exposureBufIdx;
    float    sceneNprMaxBrightness;
    // InstanceInclusionMask of the REFLECTION ray (the secondary sun-shadow ray keeps 0x02).
    // Both instances trace 0x08, which DDGISceneAS sets on every main-pass-visible instance
    // EXCEPT the forward translucent ones. HR_AlphaAccept's blend branch commits glass for a
    // reflection ray and the hit shading has no opacity, so with 0x01 the opaque instance
    // would reflect windows / alpha-blend decals as solid, and the FRONT-LAYER instance, whose
    // rays start ON a pane, would hit the pane's own BLAS at t ~ 0.
    uint     rayMask;
    uint     worldLightGridIndex;
    // ReSTIR GI as the PREVIOUS frame left it: the bindless SRV of RestirGIResult (RTGI runs
    // after this pass in the same frame), or 0 - the opaque instance only, and only while
    // ReSTIR GI ran last frame. Its coverage is the environment stand-in's sky visibility and
    // its radiance the rough band (HR_MissEnvRadiance), as Lighting.ps folds this frame's.
    uint     restirGiPrevIdx;
    // Heap index of the reflection-probe StructuredBuffer, valid while probeCount > 0.
    uint     probeBufferIdx;
    uint     _giPad1;
    uint     _giPad2;
};

// ==== Ray-list / counter / args conventions ====

// g_RayCounter slots
#define HR_CTR_SW_COUNT   0   // classify writes; PrepareArgsSW zeroes after saving
#define HR_CTR_SW_HIST    1   // saved SW ray count - IntersectSW guard
#define HR_CTR_HW_COUNT   2   // classify + IntersectSW appends; PrepareArgsHW zeroes
#define HR_CTR_HW_HIST    3   // saved HW ray count - IntersectHW guard

// g_IndirectArgs layout (uints): [0..2] SW dispatch args, [3] pad,
//                                [4..6] HW dispatch args, [7] pad
#define HR_ARGS_SW_OFFSET 0
#define HR_ARGS_HW_OFFSET 4   // x4 bytes = byte offset 16 for ExecuteIndirect

// Indirect dispatches are 32 threads/group, split 2D to dodge the 65535
// per-dimension group cap: gx = min(groups, HR_GROUPS_PER_ROW),
// gy = ceil(groups / HR_GROUPS_PER_ROW). Shaders linearize:
//   flatGroup = Gid.y * HR_GROUPS_PER_ROW + Gid.x
//   rayIndex  = flatGroup * 32 + GTid.x
#define HR_GROUPS_PER_ROW 4096u
#define HR_INTERSECT_GROUP_SIZE 32u

// Hit-counter bit layout (FFX classifier verbatim):
//   bits 0..7   SW hits (this frame, atomically accumulated by IntersectSW)
//   bits 8..15  old SW hits   (classify moves last frame's low bytes up)
//   bits 16..23 SW misses (this frame)
//   bits 24..31 old SW misses
#define HR_HITCOUNTER_SW_HIT_FLAG  (1u)
#define HR_HITCOUNTER_SW_MISS_FLAG (1u << 16u)
uint HR_Hitcounter_GetSWHits(uint c)      { return (c >> 8u)  & 0xFFu; }
uint HR_Hitcounter_GetSWMisses(uint c)    { return (c >> 24u) & 0xFFu; }

// Tile classes (FFX classifier)
#define HR_TILE_CLASS_FULL_SW 0u
#define HR_TILE_CLASS_HALF_SW 1u   // checkerboard transition frame
#define HR_TILE_CLASS_FULL_HW 2u

// Ray encoding - 16 bits per axis.
uint  HR_PackRay(uint2 px)      { return (px.x & 0xFFFFu) | (px.y << 16u); }
uint2 HR_UnpackRay(uint packed) { return uint2(packed & 0xFFFFu, packed >> 16u); }

// ==== Shared helpers ====

// Normalized position inside the traced roughness band [0..1].
float HR_RoughFactor(float roughness)
{
    return saturate(roughness / max(roughnessThreshold, 1e-4));
}

// Reverse-Z depth reconstruction (engine convention: mul(vector, matrix)).
#include "core/depth.hlsli"   // LinearizeReverseZ, WorldPosFromDepth
#include "core/color.hlsli"   // Luminance
float3 HR_ReconstructWorldPos(float2 uv, float depth)
{
    return WorldPosFromDepth(uv, depth, invViewProj);
}

float HR_LinearizeReverseZ(float zNdc)
{
    return LinearizeReverseZ(zNdc, nearZ, farZ);
}

// Project a world position to (uv, ndcZ). Returns w for validity checks.
float4 HR_ProjectToScreen(float3 worldPos)   // xy = uv, z = ndc z, w = clip w
{
    float4 clip = mul(float4(worldPos, 1.0), viewProj);
    float2 uv = float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
    return float4(uv, clip.z / clip.w, clip.w);
}

// Engine velocity (GBuffer3) is the raw NDC delta curNDC - prevNDC; decoders apply the
// (0.5, -0.5) NDC->UV scale themselves: prevUV = uv - vel * (0.5, -0.5).
float2 HR_PrevUVFromVelocity(float2 uv, float2 velNdc)
{
    return uv - velNdc * float2(0.5, -0.5);
}

// ==== RNG + GGX-VNDF direction generation ====
// IntersectSW and IntersectHW MUST produce bit-identical directions for the same (pixel,
// frame): the HW pass deterministically re-derives the ray a failed SW ray traced (AMD HSR
// deferred-raytracing scheme). Do not fork these functions per pass.

// R2PixelFrame2D: a stable per-pixel Cranley-Patterson base advanced per frame by the R2
// low-discrepancy sequence (plastic constant), the AMD SSSR / Lumen pattern.
#include "core/hash.hlsli"

#include "core/sampling.hlsli"   // OrthonormalBasis, SampleGGXVNDF, Hammersley2D

// Reflection direction: deterministic mirror through the ENTIRE band where NRDResolve
// displays the raw trace (its lerp back to REBLUR output completes at mirrorRoughness * 1.5;
// keep the two in step). A stochastic VNDF sample inside that band would be shown
// unfiltered: per-frame direction jitter at 1 spp is shimmer no denoiser sees. The lobe
// lost is small (alpha ~0.02 at the band top for mirrorRoughness 0.1); the feather's visual
// widening comes from the cone-mip / sky-mip prefilters, which ramp over the same
// smoothstep. Above the band, VNDF-sample the material's OWN lobe. HR_LOBE_BIAS (UE's
// r.Lumen.Reflections.SmoothBias) narrows it; keep it 0: a bias of 0.5 halves alpha and
// makes a 0.3-0.5 floor reflect sharper and streakier than its roughness, where the path
// tracer shows a broad sheen.
static const float HR_LOBE_BIAS = 0.0;
float3 HR_SampleReflectionDir(float3 V, float3 N, float roughness,
                              uint2 pixel, uint frame)
{
    const float3 mirrorR = reflect(-V, N);
    if (roughness <= mirrorRoughness * 1.5)
        return mirrorR;

    float3 T, B;
    OrthonormalBasis(N, T, B);
    const float3 Vt = float3(dot(V, T), dot(V, B), dot(V, N));
    if (Vt.z <= 1e-4)
        return mirrorR;

    const float2 u = R2PixelFrame2D(pixel, frame);
    const float  alpha = max(roughness * roughness * (1.0 - HR_LOBE_BIAS), 1e-3);
    const float3 Ht = SampleGGXVNDF(Vt, alpha, u.x, u.y);
    const float3 H  = T * Ht.x + B * Ht.y + N * Ht.z;
    const float3 R  = reflect(-V, H);
    return (dot(R, N) > 1e-4) ? R : mirrorR;
}

// ==== Environment radiance clamp ====
// Hue-preserving LUMINANCE clamp for environment / sky-miss radiance. The prefiltered sky
// cube contains the SUN DISK, 3-4 orders of magnitude brighter than the rest of the sky.
// Any miss path that samples it (an HW ray escaping through an opening, an SW sky landing
// on an on-screen sky sliver, the SSR-only env fallback) injects a firefly that the write
// site's 64 luma cap barely dents (64 is ~2000x a dark interior's luminance), and REBLUR
// smears one such sample into a soft glowing patch on the receiver. SkyIBL bakes no sun-less
// env map (the usual way: sun-less specular env + analytic sun), so this compresses instead:
// sky/cloud luminance (<= ~2-5) passes untouched, sun texels clamp down to 8.
float3 HR_ClampEnvRadiance(float3 c)
{
    const float kMaxEnvLum = 8.0;
    const float lum = Luminance(c);
    return select(lum > kMaxEnvLum, c * (kMaxEnvLum / lum), c);
}

// Rays per pixel for the HARDWARE intersect tier (the screen-space tier is a march, not a
// sampler). 1 = one iteration with seed == frameIndex, so the ray matches
// the classifier / SW re-derivation contract. 2 or 4 cut the residual 1-spp variance by
// 1/sqrt(N) at N x traversal on the TDR-budgeted tier, for every pixel the screen tier
// could not resolve; shaders compile from disk at runtime, so change it, restart and
// measure on the heaviest view, one step at a time. It stays 1: the salt-and-pepper root
// cause (a per-pixel binary conf reaching the fold alpha) is fixed in hybrid_refl_env.hlsli,
// so extra samples are polish, and N x traversal on unmeasured hardware is not a default.
#ifndef HR_RAYS_PER_PIXEL
#define HR_RAYS_PER_PIXEL 1u
#endif

// The ONE firefly curve for traced radiance, hue-preserving and roughness-aware: a luma cap
// easing 64 -> 8 with roughFactor (roughness / roughnessThreshold, which both tiers have).
// Every non-mirror source clamps through it. Do not clamp per channel (it desaturates a
// bright sample toward white instead of dimming it) or give estimators different caps: an
// 8:1 range mismatch between per-pixel-selected estimators is the bimodal input that defeats
// REBLUR's sigma clamp and anti-firefly. Callers keep their own `roughness > mirrorRoughness`
// guard: the mirror band is a deterministic, temporally stable ray and stays UNCAPPED, so a
// legitimate bright mirror highlight is never dimmed.
float3 HR_ClampTracedRadiance(float3 c, float roughFactor)
{
    const float roughGate = smoothstep(0.20, 0.65, saturate(roughFactor));
    const float maxLuma   = lerp(64.0, 8.0, roughGate);
    const float lum       = Luminance(c);
    return select(lum > maxLuma, c * (maxLuma / max(lum, 1e-4)), c);
}

// ==== Lobe-matched radiance prefiltering ====
// A 1-spp estimator needs its radiance LOOKUP pre-integrated over the lobe it sampled. In
// the mirror band HR_SampleReflectionDir returns a deterministic reflect() and never
// touches the RNG, so a SHARP point lookup is correct and temporally frozen. Above it the
// direction is a per-pixel, per-frame VNDF sample, and a sharp lookup turns every high
// frequency of the reflected image 1:1 into per-pixel variance for the denoiser.
// Prefiltering removes it at the source; it is purely SPATIAL, so it cannot add ghosting.

// Roughness of the lobe actually TRACED. HR_SampleReflectionDir narrows the sampled GGX
// lobe by HR_LOBE_BIAS (alpha = roughness^2 * (1 - bias)), so one sample represents a cone
// NARROWER than the GBuffer roughness implies; prefiltering at the full roughness would
// double-filter and over-blur glossy reflections. One constant drives both.
float HR_TracedLobeRoughness(float roughness)
{
    return roughness * sqrt(1.0 - HR_LOBE_BIAS);   // alpha_eff = r^2 * (1 - bias)
}

// Prefilter mip for the PRIMARY sky-cube miss. Lives here, not in either intersect shader,
// because the two tiers MUST produce a bit-identical sample: an SW/HW sky-radiance mismatch
// shows as a visible step every time the hit counter flips a sky-reflecting tile between
// FULL_SW and FULL_HW (see the hitT/rayTMax parity note in HybridReflIntersectHW).
// The smoothstep makes this EXACTLY 0.0 at roughness <= mirrorRoughness, so the mirror band
// samples mip 0 and stays a clean A/B control.
float HR_SkyMissMip(float roughness)
{
    const float glossy =
        smoothstep(mirrorRoughness, mirrorRoughness * 1.5, roughness);
    return glossy * HR_TracedLobeRoughness(roughness)
                  * float(max(skyCubeMips, 1u) - 1u);
}

#endif // HYBRID_REFL_COMMON_HLSLI
