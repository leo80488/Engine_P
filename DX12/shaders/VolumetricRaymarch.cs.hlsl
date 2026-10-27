#include "spot_shadow_view.hlsli"
// Half-resolution per-pixel raymarch for the high-quality volumetric lighting tier:
// the directional sun (CSM-shadowed) and shadow-casting spot lights (SpotShadowPass
// atlas). FroxelLightInject.cs.hlsl handles many small lights on the 3D grid, which
// cannot resolve sharp cascade / atlas shadow edges; this pass reads them per pixel
// for crisp shafts and occluder silhouettes.
// The medium dims each step's light from the camera's side only, except the sun's when
// FroxelParams.rmMediumIdx names the medium set: then also on its way from the sun.
// Output, half-res RGBA16F: .rgb = in-scatter radiance along the view ray; .a = the
// depth key of the surface this texel marched to (vol_raymarch_common.hlsli), used by
// the apply's bilateral weights.

#include "FroxelCommon.hlsli"
#include "vol_raymarch_common.hlsli"
#include "cluster_common.hlsli"
#include "light_eval.hlsli"   // the spot cone (AngularAttenuation), same as surfaces
// The frame's LightCB, opened from the heap (FroxelParams.lightCbIdx): the cloud
// shadow's map index and basis are the ones the lit surfaces read.
#define LIGHT_CB_AS_STRUCT
#include "light_cb.hlsli"
#include "cloud_shadow.hlsli"

cbuffer FroxelCB : register(b0, space2)
{
    FroxelParams P;
};

Texture3D<float4>            FroxelDensityTex : register(t0, space2);
Texture2DArray<float>        ShadowCascades   : register(t1, space2);
StructuredBuffer<GPULight>   Lights           : register(t2, space2);
Texture2D<float>             SceneDepth       : register(t3, space2);
Texture2DArray<float>        SpotShadowAtlas  : register(t6, space2);
StructuredBuffer<SpotShadowView>   SpotShadowVPs    : register(t7, space2);

SamplerState                 LinearSampler    : register(s0, space2);
SamplerComparisonState       ShadowSampler    : register(s1, space2);

RWTexture2D<float4>          InScatterOut     : register(u0, space2);

// The medium set the sun crosses on its way to each step, opened from the heap.
#include "core/sampling.hlsli"   // OrthonormalBasis, for core/participating_media.hlsli
#include "core/participating_media.hlsli"
#define FROXEL_MEDIUM_IDX P.rmMediumIdx
#include "froxel_rt_medium.hlsli"

// ==== Tunable budget ====
// 32 steps is ~1 ms at 960x540 on a mid-range GPU. Each step takes one froxel density
// sample, one CSM tap for the sun, and loops the shadow-casting spots. IGN jitter turns
// banding into noise that the temporal pass averages out.
#define RM_STEPS          32
// Caps the shadowed spots tapped per step, so many overlapping casters (SpotShadowPass
// allows 256) cannot blow up the worst-case cost.
#define RM_MAX_SPOTS      8

// CSM tap: 2x2 PCF in one cascade. Returns visibility in [0, 1], or -1 when worldPos
// is outside the cascade (ComputeSunShadow takes the first in-bounds one).
float SampleCascade(int cascade, float4x4 m, float3 worldPos, float texelSize, float bias)
{
    float4 sp = mul(float4(worldPos, 1.0), m);
    sp.xyz /= sp.w;
    float2 uv = sp.xy * float2(0.5, -0.5) + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 ||
        sp.z < 0.0 || sp.z > 1.0) return -1.0;

    float depth = sp.z + bias;
    float shadow = 0.0;
    [unroll] for (int dy = 0; dy <= 1; ++dy)
    [unroll] for (int dx = 0; dx <= 1; ++dx)
    {
        float2 o = (float2(dx, dy) - 0.5) * texelSize;
        shadow += ShadowCascades.SampleCmpLevelZero(
            ShadowSampler, float3(uv + o, float(cascade)), depth);
    }
    return shadow * 0.25;
}

float ComputeSunShadow(float3 worldPos)
{
    float4x4 mats[3] = { P.shadowMatrix0, P.shadowMatrix1, P.shadowMatrix2 };
    [unroll] for (int c = 0; c < 3; ++c)
    {
        float s = SampleCascade(c, mats[c], worldPos,
                                P.shadowParams.x, P.shadowParams.z);
        if (s >= 0.0) return s;
    }
    return 1.0;
}

// Spot-shadow atlas visibility for an in-scatter sample.
// The s1 space2 comparison sampler (CreateComputeRootSignature) is BORDER + OPAQUE_WHITE
// + GREATER_EQUAL: under reverse-Z an out-of-UV tap reads 0 (shadowed), so no manual
// UV clamp is needed (one would bleed past the cone edge).
// Do not add the surface receiver bias: a sample in the medium has no acne to cancel,
// and the bias leaks light through thin walls.
// Do not add manual PCF taps: the LINEAR comparison filter already does a 2x2 bilinear
// PCF per SampleCmp; offset taps make a 3x3 footprint that amplifies edge noise.
float SpotShadowVisibility(float3 worldPos, uint sliceIdx)
{
    const SpotShadowView shadowView = SpotShadowVPs[sliceIdx];
    if (shadowView.atlasIndex == 0xFFFFFFFFu) return 1.0;
    Texture2DArray<float> atlas = ResourceDescriptorHeap[NonUniformResourceIndex(shadowView.atlasIndex)];
    float4 sc = mul(float4(worldPos, 1.0), shadowView.vp);
    // Behind the light's apex plane: no path from the light.
    if (sc.w <= 0.0) return 0.0;
    sc.xyz /= sc.w;

    // Closer than the shadow frustum's near plane: nothing in the map can occlude it, so
    // fully lit. Do not treat it as shadowed: that kills the light near the spot's apex.
    if (sc.z > 1.0) return 1.0;

    float2 uv = sc.xy * float2(0.5, -0.5) + 0.5;
    // One SampleCmp with the LINEAR comparison sampler. BORDER + WHITE returns 0
    // (shadowed) for UV outside [0,1] and for sc.z < 0 (past the far plane).
    return atlas.SampleCmpLevelZero(
        ShadowSampler, float3(uv, (float)shadowView.slice), sc.z);
}

// Interleaved-gradient noise: a cheap hash with a low-discrepancy pattern across a
// screen tile. Jitters the first ray step per frame (frameIdx) so the temporal pass
// converges on a smooth integral instead of quantised plateaus.
float IGN(float2 pixel, uint frameIdx)
{
    pixel += float(frameIdx & 63u) * 5.588238;  // decorrelate across frames
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// World-space view ray for a full-res uv and the world distance maxT to the pixel's
// surface (ndcZ = its own depth, see VolRmMarchPixel), clamped to froxelFar. The
// surface path uses invViewProj only, so it holds for any depth convention.
void BuildViewRay(float2 uv, float ndcZ, out float3 rayOrigin, out float3 rayDir,
                  out float maxT)
{
    rayOrigin = P.cameraPos;

    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    // Direction through any point of this pixel's NDC column; ndc.z = 0.5 is an
    // arbitrary non-degenerate choice.
    float4 pH = mul(float4(ndc, 0.5, 1.0), P.invViewProj);
    float3 pW = pH.xyz / pH.w;
    rayDir = normalize(pW - rayOrigin);

    // Sky early-out: reverse-Z sky is ndc.z = 0. This is the only convention-specific line.
    if (ndcZ <= 1e-6)
    {
        maxT = P.froxelFar;
        return;
    }

    float4 surfH = mul(float4(ndc, ndcZ, 1.0), P.invViewProj);
    float3 surfW = surfH.xyz / surfH.w;
    maxT = min(length(surfW - rayOrigin), P.froxelFar);
}

// Froxel density at worldPos: (scatteringRGB, extinction), or zero outside the froxel
// grid so the step contributes nothing.
float4 SampleFroxelDensity(float3 worldPos)
{
    float3 uvw = WorldToFroxelUVW(worldPos, P);
    if (any(uvw < 0.0) || any(uvw > 1.0))
        return float4(0, 0, 0, 0);
    return FroxelDensityTex.SampleLevel(LinearSampler, uvw, 0);
}

// ==== The medium on the sun's way ====
// The local volumes the sun's way from some step of o + t d, t in [tMin, tMax], can cross, as bits of
// the medium set's list: a volume's bounding sphere has to come within its radius of the strip the
// rays to the sun sweep from the segment, and reach the sun's side of it. Conservative; the steps then
// test only these instead of every volume each.
uint SunMediumLocalMask(MediumSet media, float3 o, float3 d, float tMin, float tMax, float3 toSun)
{
    // Across the sun's direction the strip is the segment's projection.
    const float  dSun     = dot(d, toSun);
    const float3 dAcross  = d - dSun * toSun;
    const float  dAcross2 = dot(dAcross, dAcross);
    uint mask = 0u;
    for (uint i = 0u; i < media.localCount; ++i)
    {
        // The bounds from the world -> unit-volume rows: the inverse's columns are the rows' cross
        // products over the determinant; the unit box's corners lie within the columns' summed
        // lengths of the centre, the unit sphere within their root sum of squares.
        const MediumLocal v = MediumLoadLocal(i);
        const float3 ca  = cross(v.row1.xyz, v.row2.xyz);
        const float3 cb  = cross(v.row2.xyz, v.row0.xyz);
        const float3 cc  = cross(v.row0.xyz, v.row1.xyz);
        const float  det = dot(v.row0.xyz, ca);
        if (!(abs(det) > 1e-20))
        {
            mask |= 1u << i;
            continue;
        }
        const float3 centre = -(v.row0.w * ca + v.row1.w * cb + v.row2.w * cc) / det;
        const float3 axes   = float3(length(ca), length(cb), length(cc)) / abs(det);
        const float  radius = v.shape == kMediumLocalSphere ? length(axes) : axes.x + axes.y + axes.z;

        const float3 u       = centre - o;
        const float3 uAcross = u - dot(u, toSun) * toSun;
        const float  t       = dAcross2 > 1e-12 ? clamp(dot(uAcross, dAcross) / dAcross2, tMin, tMax) : tMin;
        const float3 gap     = uAcross - t * dAcross;
        const float  ahead   = dot(u, toSun) - (dSun >= 0.0 ? tMin : tMax) * dSun + radius;
        if (dot(gap, gap) <= radius * radius && ahead >= 0.0)
            mask |= 1u << i;
    }
    return mask;
}

// The medium's transmittance from p toward the sun: MediumTransmittanceDeterministic with only the
// local volumes in @p localMask.
float SunMediumTransmittance(MediumSet media, uint localMask, float3 p, float3 toSun)
{
    float tau = MediumAnalyticDepth(media, p, toSun, 0.0, kMediumFar);
    for (uint m = localMask; m != 0u; m &= m - 1u)
    {
        const MediumLocal v = MediumLoadLocal(firstbitlow(m));
        float ta, tb;
        if (MediumLocalInterval(v, p, toSun, 0.0, kMediumFar, ta, tb))
            tau += MediumLocalDepthMarch(v, p, toSun, ta, tb);
    }
    return exp(-min(tau, 80.0));
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint halfW, halfH;
    InScatterOut.GetDimensions(halfW, halfH);
    if (DTid.x >= halfW || DTid.y >= halfH) return;

    // One full-res pixel of this texel's 2x2 block, marched along its own ray to its own
    // depth (vol_raymarch_common.hlsli: nearest / farthest surface in a checkerboard). The
    // texel's key rides in .a for the apply.
    float ndcZ;
    const int2 px = VolRmMarchPixel(SceneDepth, DTid.xy, ndcZ);
    uint fullW, fullH;
    SceneDepth.GetDimensions(fullW, fullH);
    float2 uv = (float2(px) + 0.5) / float2(fullW, fullH);
    const float key = VolRmKey(ndcZ, P.nearPlane, P.farPlane, P.froxelFar);

    float3 rayOrigin, rayDir;
    float  maxT;
    BuildViewRay(uv, ndcZ, rayOrigin, rayDir, maxT);

    // Start / end of the march, both world distance along rayDir. Do not mix in view-space
    // Z: off-axis samples land at the wrong world position.
    float tMin = P.froxelNear;
    float tMax = maxT;
    if (tMax <= tMin) { InScatterOut[DTid.xy] = float4(0, 0, 0, key); return; }

    float stepLen  = (tMax - tMin) / float(RM_STEPS);
    float jitter   = IGN(float2(DTid.xy), P.frameIndex);  // [0, 1)
    float t        = tMin + jitter * stepLen;

    // View direction for the HG phase; constant along the ray, so hoisted.
    float3 V        = rayDir;
    float3 toSun    = -normalize(P.sunDir);
    float  sunCos   = dot(V, toSun);
    float  sunPhase = PhaseHG(sunCos, P.anisotropy);
    // The medium between each step and the sun, as the ray-traced froxels take it: the global
    // and height fog in closed form, the local volumes this ray's sun rays can reach marched.
    // Empty without rmMediumIdx.
    const MediumSet sunMedium = FroxelMediumSet();
    const uint      sunLocals = SunMediumLocalMask(sunMedium, rayOrigin, rayDir, tMin, tMax, toSun);

    // Cloud shadow: the map index and basis rows the lit surfaces use, read once from the
    // frame's LightCB (lightCbIdx is a typed CBV handle; null when there is no view of it,
    // then no cloud term).
    uint   csIdx = BINDLESS_NULL_TEX2D_FLOAT;   // the handle of "no map"
    float4 csR   = float4(0, 0, 0, 0);
    float4 csU   = float4(0, 0, 0, 0);
    if (!BindlessNone(P.lightCbIdx))
    {
        ConstantBuffer<LightCB> lcb = BindlessCBuf<LightCB>(P.lightCbIdx);
        csIdx = lcb.gCloudShadowIdx;
        csR   = lcb.gCloudShadowRight;
        csU   = lcb.gCloudShadowUp;
    }

    uint   spotCount = (uint)(P.shadowParams.y);
    float3 inScatter = float3(0, 0, 0);
    float  trans     = 1.0;

    [loop] for (int i = 0; i < RM_STEPS; ++i, t += stepLen)
    {
        float3 p = rayOrigin + rayDir * t;

        float4 dens = SampleFroxelDensity(p);
        float  sigmaT = dens.a;
        if (sigmaT <= 0.0) continue;

        // ---- Sun contribution ----
        float3 stepL = float3(0, 0, 0);
        if (P.sunStrength > 0.0)
        {
            // Cloud shadow on the shaft itself turns a uniform sun beam into god rays through the
            // gaps in the deck. Sampled per step, not per pixel, so a shaft can be lit near the
            // ground and cut off higher up.
            float sunVis = ComputeSunShadow(p)
                         * CloudShadowSample(p, csIdx, csR, csU, LinearSampler);
            if (P.rmMediumIdx != 0u && sunVis > 0.0)
                sunVis *= SunMediumTransmittance(sunMedium, sunLocals, p, toSun);
            stepL += P.sunColor * P.sunStrength * sunVis * sunPhase;
        }

        // ---- Shadow-casting spot lights ----
        // Walks the opted-in volumetric light list. Lights without an atlas slice belong to
        // FroxelLightInject.
        uint processed = 0;
        [loop] for (uint li = 0; li < spotCount && processed < RM_MAX_SPOTS; ++li)
        {
            GPULight L = Lights[li];
            if (L.type != 2u)                       continue;  // spots only: the atlas holds spot shadows
            if (L.shadowSliceIdx == 0xFFFFFFFFu)    continue;  // no shadow -> froxel tier

            float3 toL   = L.position - p;
            float  dist  = length(toL);
            if (dist > L.radius) continue;
            float3 Ldir  = toL / max(dist, 1e-4);

            // Same windowed distance falloff as the froxel path (no 1/d^2 blow-up).
            float distNorm  = dist / max(L.radius, 1e-4);
            float distNorm2 = distNorm * distNorm;
            float atten     = saturate(1.0 - distNorm2 * distNorm2);
            atten *= atten;

            // Spot cone: light_eval's AngularAttenuation, the one surfaces use (inner = 0.75 x outer).
            const float coneFall = AngularAttenuation(Ldir, L);
            if (coneFall <= 0.0) continue;

            float  spotCos   = dot(V, Ldir);
            float  spotPhase = PhaseHG(spotCos, P.anisotropy);
            float  shadowVis = SpotShadowVisibility(p, L.shadowSliceIdx);

            stepL += L.color * L.intensity *
                     atten * coneFall * spotPhase * shadowVis;
            ++processed;
        }

        // Beer's-law transmittance over the step plus Frostbite-style analytic in-scatter for
        // constant sigma over the step (stable under varying step length, no double counting).
        float stepTrans = exp(-sigmaT * stepLen);
        float3 sInt     = (stepL - stepL * stepTrans) / max(sigmaT, 1e-4);
        inScatter      += trans * dens.rgb * sInt;
        trans          *= stepTrans;

        // The ray is opaque: later steps cannot add meaningful in-scatter.
        if (trans < 0.01) break;
    }

    InScatterOut[DTid.xy] = float4(inScatter, key);
}
