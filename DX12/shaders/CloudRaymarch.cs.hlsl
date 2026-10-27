// Quarter-resolution volumetric cloud raymarch: Nubis density model (Schneider 2015/2017) +
// Frostbite lighting (Hillaire 2016), after UE5's VolumetricCloud.usf.
//   1. World-space ray from the texel UV via invViewProj.
//   2. Intersect a spherical cloud shell (clouds dip below the horizon, not a flat fog band).
//   3. Clamp the exit to the opaque scene depth and the max trace distance.
//   4. Adaptive march: coarse steps on the cheap base shape; on a hit back up and switch to
//      fine steps with detail erosion; revert to coarse after several empty fine samples.
//   5. Density (CloudDensity.hlsli): Perlin-Worley base, remap-eroded by weather coverage,
//      per-type height gradient, then Worley edge erosion (wispy base -> billowy top).
//   6. Lighting: 5-tap cone + far-tap optical depth toward the sun, 4 Wrenninge multi-scatter
//      octaves (a = b = c = 0.5), dual-lobe HG + silver-lining lobe, Nubis in-scatter
//      probability, height-gradient ambient, Frostbite energy-conserving integration.
// Output: RGBA16F (rgb = premultiplied in-scatter radiance, a = transmittance) and R32F
// CloudDistOut, the per-texel march distance for the depth-aware composite.

// Shared CB layout + RaySphere / ShellInterval / HeightInLayer.
#define CLOUD_CB_REGISTER register(b0, space2)
#include "CloudCommon.hlsli"
// Noise / weather SRVs (t0..t2 space2), samplers, Remap / HeightGradient / SampleDensity /
// gDetailFade - shared verbatim with CloudShadowBake so the shadow always matches the cloud.
#include "CloudDensity.hlsli"

Texture2D<float>          SceneDepth  : register(t3, space2);

RWTexture2D<float4>       CloudOut    : register(u0, space2);
RWTexture2D<float>        CloudDistOut : register(u1, space2); // scene distance marched against

#include "core/math.hlsli"   // PI

// Step count from a fixed world-space step, clamped to [kMinSteps, maxSteps]: uniform step
// size across the sky, and maxSteps bounds the cost of long horizon rays. Do not scale the
// count by march length: zenith rays then hit the floor and horizon rays the ceiling, and
// both are undersampled.
static const float kTargetStepLen    = 40.0;   // m per fine step
static const float kMinSteps         = 48.0;
// Horizon fade: shell-entry distances in [start, end] blend the result away (the geometric
// horizon for a 1.5 km layer bottom is ~138 km).
static const float kHorizonFadeStart = 90000.0;
static const float kHorizonFadeEnd   = 140000.0;
// Fine->coarse hysteresis: empty fine samples before reverting to coarse.
static const int   kEmptyToCoarse    = 8;
static const float kCoarseMul        = 3.0;   // coarse step = fine step * this

// Multi-scattering octaves (Wrenninge): scatter a^n, extinction b^n, phase eccentricity c^n;
// a <= b or energy is created. 4, not 3: a dense core's sun optical depth reaches 4-11, and
// 3 octaves only thin extinction to b^2 = 0.25x (exp(-8 x 0.25) ~ 0.14), leaving interiors
// black. The 4th is cheap: SunOpticalDepth runs once per sample and every octave shares it.
static const int   kMSOctaves = 4;
static const float kMSScatter = 0.5;   // a
static const float kMSExtinct = 0.5;   // b
static const float kMSPhase   = 0.5;   // c

// Cone kernel for the sun-light march: decorrelated, roughly unit offsets.
static const float3 kCone[5] = {
    float3( 0.30,  0.45, -0.25),
    float3(-0.35,  0.20,  0.40),
    float3( 0.45, -0.10,  0.30),
    float3(-0.20,  0.60, -0.20),
    float3( 0.15, -0.35,  0.45),
};

float HenyeyGreenstein(float cosT, float g)
{
    const float g2 = g * g;
    const float denom = 1.0 + g2 - 2.0 * g * cosT;
    return (1.0 - g2) / (4.0 * PI * pow(max(denom, 1e-4), 1.5));
}

// Interleaved gradient noise (Jimenez), advanced per frame so CloudTemporal averages the
// dither away. With temporal off (temporalOn == 0) the frame is 0 and the pattern is static:
// clouds write no velocity, so an animated jitter without accumulation would boil.
float IGN(float2 pixel, float frame)
{
    float2 p = pixel + 5.588238 * frame;
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

// Halton(2,3) sub-texel offsets: successive frames march different directions inside the
// same quarter-res texel, which CloudTemporal accumulates into real supersampling.
static const float2 kSubPixel[8] = {
    float2( 0.000, -0.166), float2(-0.250,  0.166), float2( 0.250, -0.389),
    float2(-0.375,  0.055), float2( 0.125, -0.055), float2(-0.125,  0.278),
    float2( 0.375, -0.278), float2(-0.062,  0.389),
};

// Optical depth toward the sun: 5 cone taps over half the layer thickness (full detail for
// the first two, cheap beyond) plus one long-range tap for shadows from distant cloud towers
// (Horizon Zero Dawn).
float SunOpticalDepth(float3 wp)
{
    const float layerThick = max(topAltitude - bottomAltitude, 1.0);
    const float stepLen    = (layerThick * 0.5) / 5.0;

    float od = 0.0;
    [unroll] for (int i = 0; i < 5; ++i)
    {
        float dist = stepLen * (float(i) + 0.5);
        float3 sp  = wp + sunDir * dist + kCone[i] * (dist * 0.3);
        float  hh  = HeightInLayer(sp);
        if (hh <= 0.0 || hh >= 1.0) continue;
        float dummy;
        od += SampleDensity(sp, hh, i >= 2, dummy) * stepLen;
    }

    float3 fp = wp + sunDir * (layerThick * 1.5);
    float  fh = HeightInLayer(fp);
    if (fh > 0.0 && fh < 1.0)
    {
        float dummy;
        od += SampleDensity(fp, fh, true, dummy) * (layerThick * 0.5);
    }
    return od * extinction;
}

[numthreads(8, 8, 1)]
void main(uint2 dt : SV_DispatchThreadID)
{
    if (dt.x >= (uint)halfResW || dt.y >= (uint)halfResH) return;

    // World-space ray: UV -> NDC -> world via invViewProj. The sub-texel offset cycles over 8
    // frames for the temporal accumulator; it is zero with temporal off.
    const uint  frm = (uint)frameIndex;
    const float2 sub = (temporalOn > 0.5) ? kSubPixel[frm & 7u] : float2(0, 0);
    float2 uv  = (float2(dt) + 0.5 + sub) / float2(halfResW, halfResH);
    float2 ndc = uv * 2.0 - 1.0;
    ndc.y = -ndc.y;

    // Reversed-Z: near plane is NDC z=1, far is NDC z=0.
    float4 nearW = mul(float4(ndc, 1.0, 1.0), invViewProj);
    float4 farW  = mul(float4(ndc, 0.0, 1.0), invViewProj);
    float3 rayP  = nearW.xyz / nearW.w;
    float3 rayQ  = farW.xyz  / farW.w;
    float3 rayD  = normalize(rayQ - rayP);

    // Farthest opaque depth in this texel's 4x4 full-res footprint (reversed-Z: the min raw
    // value; sky 0 is the farthest), from four corner gathers. Marching to the farthest surface
    // gives a texel straddling a silhouette valid cloud for its sky pixels; the depth-aware
    // composite then picks per full-res pixel. A single centre load would make the 4x4 block an
    // all-or-nothing decision (a halo along terrain outlines).
    float sceneNdcZ;
    {
        float2 invFull = 1.0 / float2(fullResW, fullResH);
        float2 b  = float2(dt * 4u);
        float4 g0 = SceneDepth.GatherRed(LinearClamp, (b + float2(1.0, 1.0)) * invFull);
        float4 g1 = SceneDepth.GatherRed(LinearClamp, (b + float2(3.0, 1.0)) * invFull);
        float4 g2 = SceneDepth.GatherRed(LinearClamp, (b + float2(1.0, 3.0)) * invFull);
        float4 g3 = SceneDepth.GatherRed(LinearClamp, (b + float2(3.0, 3.0)) * invFull);
        float4 m  = min(min(g0, g1), min(g2, g3));
        sceneNdcZ = min(min(m.x, m.y), min(m.z, m.w));
    }

    // Scene distance this texel marches against, written for the composite on every exit path.
    // Reprojecting (ndc, sceneNdcZ) through invViewProj is reversed-Z safe and already a distance
    // along this ray.
    float sceneDist = kCloudSkyDist;
    if (sceneNdcZ > 0.0)   // reversed-Z: 0 = far clear (sky), >0 = geometry
    {
        float4 surfW = mul(float4(ndc, sceneNdcZ, 1.0), invViewProj);
        sceneDist = length(surfW.xyz / surfW.w - cameraPos);
    }
    CloudDistOut[dt] = sceneDist;

    float tEntry, tExit;
    if (!ShellInterval(cameraPos, rayD, tEntry, tExit) || tEntry > kHorizonFadeEnd)
    {
        CloudOut[dt] = float4(0, 0, 0, 1);
        return;
    }

    tExit = min(tExit, sceneDist);

    // Cap the march distance from the shell entry (UE mode 0) and remember whether the cap,
    // not geometry or the shell, ended it, so the tail can be feathered.
    bool clampedByMax = (tEntry + maxTraceDist) < tExit;
    tExit = min(tExit, tEntry + maxTraceDist);
    if (tExit <= tEntry)
    {
        CloudOut[dt] = float4(0, 0, 0, 1);
        return;
    }

    const float marchLen  = tExit - tEntry;
    // min(kMinSteps, maxSteps): Max Ray Steps below the floor must still give a well-ordered
    // clamp (lo > hi is undefined).
    const float stepCount = clamp(marchLen / kTargetStepLen,
                                  min(kMinSteps, maxSteps), maxSteps);
    const float stepFine   = marchLen / stepCount;
    const float stepCoarse = stepFine * kCoarseMul;
    const float fadeStart  = tExit - 0.25 * marchLen;   // only applied when clampedByMax

    // Detail-erosion LOD: full strength while a step spans <= 1/4 of the detail period, off at
    // one whole period. detailNoiseScale is 1/period, so stepFine * detailNoiseScale is periods
    // per step.
    gDetailFade = saturate(Remap(stepFine * detailNoiseScale, 1.0, 0.25, 0.0, 1.0));

    // Per-octave phase: dual-lobe HG blended forward / back, max() with a silver-lining lobe
    // (Nubis); eccentricity attenuates by c^n.
    float cosT = dot(rayD, sunDir);
    float phaseOct[kMSOctaves];
    {
        float cN = 1.0;
        [unroll] for (int n = 0; n < kMSOctaves; ++n)
        {
            float ph = lerp(HenyeyGreenstein(cosT, phaseFwdG * cN),
                            HenyeyGreenstein(cosT, phaseBackG * cN),
                            phaseBlend);
            float silver = silverIntensity
                         * HenyeyGreenstein(cosT, (0.99 - silverSpread) * cN);
            phaseOct[n] = max(ph, silver);
            cN *= kMSPhase;
        }
    }

    // Ambient skylight: authored tint x the luminance of the light on the sky dome
    // (ambientLightLum: the sun at the layer, zero at night when the sky is dark, though the moon
    // still lights the deck through sunColor) x a bottom-occlusion height gradient (UE: 0.5).
    const float3 ambientBase = ambientTint * (ambientStrength * ambientLightLum);

    // Jittered adaptive march. Coarse steps sample only the conservative base shape; a hit backs
    // up one coarse step and switches to fine. The jitter spans one fine step: enough to break
    // banding, small enough to stay subtle in world space.
    float jitter = IGN(float2(dt), (temporalOn > 0.5) ? float(frm) : 0.0);
    float t = tEntry + jitter * stepFine;

    float3 scatter       = 0;
    float  transmittance = 1.0;
    bool   fineMode      = false;
    int    sinceHit      = 0;

    const int maxIter = (int)stepCount * 3;   // room for coarse / fine switches and backups
    [loop] for (int i = 0; i < maxIter; ++i)
    {
        if (t >= tExit) break;
        float3 p = cameraPos + rayD * t;
        float  h = HeightInLayer(p);

        if (!fineMode)
        {
            float dummy;
            float dc = SampleDensity(p, h, true, dummy);
            if (dc > 1e-4)
            {
                fineMode = true;
                sinceHit = 0;
                t = max(t - stepCoarse, tEntry);   // re-cover the skipped span
                continue;
            }
            t += stepCoarse;
            continue;
        }

        float baseShape;
        float d = SampleDensity(p, h, false, baseShape);

        // Feather the tail when the max-trace cap (not geometry) ends the march, so distant decks
        // fade instead of slicing off.
        if (clampedByMax)
            d *= 1.0 - smoothstep(fadeStart, tExit, t);

        if (d > 1e-4)
        {
            sinceHit = 0;

            float od = SunOpticalDepth(p);

            // Nubis in-scatter probability (instead of the 2015 powder term). Depth: how much material
            // surrounds the sample (base shape as the low-mip proxy); vertical: bases are dark, nothing
            // scatters up from below.
            float depthProb = 0.05 + pow(saturate(baseShape),
                                         clamp(Remap(h, 0.3, 0.85, 0.5, 2.0), 0.5, 2.0));
            float vertProb  = pow(clamp(Remap(h, 0.07, 0.14, 0.1, 1.0), 0.1, 1.0), 0.8);
            float inscatter = depthProb * vertProb;

            float3 ambientL = ambientBase * saturate(0.5 + h);

            // Frostbite energy-conserving integration, one term per multi-scatter octave; only octave 0
            // advances the view transmittance.
            float sigmaT = max(d * extinction, 1e-7);
            float aN = 1.0, bN = 1.0;
            float stepTr0 = 1.0;
            [unroll] for (int n = 0; n < kMSOctaves; ++n)
            {
                float sigmaS_n = sigmaT * aN;            // albedo ~ 1 (water)
                float sigmaT_n = max(sigmaT * bN, 1e-7);
                float trN = exp(-sigmaT_n * stepFine);
                float tlN = exp(-od * bN);

                float3 L = sunColor * (tlN * phaseOct[n] * inscatter);
                if (n == 0) L += ambientL;
                L *= cloudColor;

                float3 Lscat = L * sigmaS_n;
                scatter += transmittance * (Lscat - Lscat * trN) / sigmaT_n;
                if (n == 0) stepTr0 = trN;

                aN *= kMSScatter;
                bN *= kMSExtinct;
            }
            transmittance *= stepTr0;

            if (transmittance < 0.005) break;
        }
        else if (++sinceHit >= kEmptyToCoarse)
        {
            fineMode = false;
        }
        t += stepFine;
    }

    // Horizon fade on the entry distance, not the sample distance, so a whole distant deck fades
    // as one.
    float horizonFade = 1.0 - smoothstep(kHorizonFadeStart, kHorizonFadeEnd, tEntry);
    scatter       *= horizonFade;
    transmittance  = lerp(1.0, transmittance, horizonFade);

    CloudOut[dt] = float4(scatter, transmittance);
}
