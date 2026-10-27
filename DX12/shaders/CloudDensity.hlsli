#ifndef CLOUD_DENSITY_HLSLI
#define CLOUD_DENSITY_HLSLI

// The Nubis cloud density model, shared verbatim by CloudRaymarch.cs.hlsl (the visible cloud)
// and CloudShadowBake.cs.hlsl (its shadow). They must agree exactly: a shadow from a different
// density field lands where the cloud is not and looks like a projection bug. Change shaping,
// coverage, height gradient or erosion here, never in a caller.
// The includer defines CLOUD_CB_REGISTER and includes CloudCommon.hlsli first (this reads
// coverage / density / *Scale / anvilBias / windOffset / cloudTypeBias from that CB).

Texture3D<float4>  BaseNoise   : register(t0, space2);  // 128^3: R = Perlin-Worley, GBA = Worley FBM
Texture3D<float4>  DetailNoise : register(t1, space2);  // 32^3 worley erosion
Texture2D<float4>  WeatherTex  : register(t2, space2);  // 512^2 coverage / fill / type

SamplerState       LinearClamp : register(s0, space2);
SamplerState       LinearWrap  : register(s2, space2);  // tileable noise / weather

float Remap(float v, float lo, float hi, float newLo, float newHi)
{
    return newLo + (v - lo) * (newHi - newLo) / max(1e-5, hi - lo);
}

// Density-over-height profile per cloud type; gradient = (start0, full0, full1, end1),
// the usual Nubis constants.
float HeightGradient(float h, float type)
{
    const float4 kStratus       = float4(0.00, 0.07, 0.08, 0.15);
    const float4 kStratocumulus = float4(0.00, 0.20, 0.42, 0.60);
    const float4 kCumulus       = float4(0.00, 0.08, 0.75, 0.98);
    float4 g = (type < 0.5)
        ? lerp(kStratus,       kStratocumulus, saturate(type * 2.0))
        : lerp(kStratocumulus, kCumulus,       saturate(type * 2.0 - 1.0));
    return smoothstep(g.x, g.y, h) * (1.0 - smoothstep(g.z, g.w, h));
}

// Detail-erosion fade in [0, 1]. The detail texture is 32^3 over a ~500 m tile (finest band
// ~31 m); an 80-300 m march step samples it far below Nyquist and the volumes have no mips.
// Callers set it from their step length before marching; the shadow bake sets 0.
static float gDetailFade = 1.0;

// Cloud density at a world position. `cheap` skips detail erosion (coarse marching and far
// light taps); erosion only removes density, so the cheap sample is a conservative bound.
// `baseShape` returns the pre-erosion shaped density in [0, 1], the Nubis "lodded density"
// in-scatter proxy.
float SampleDensity(float3 wp, float h, bool cheap, out float baseShape)
{
    baseShape = 0.0;

    // Weather map: planar world XZ, scrolled at 1/4 wind speed.
    float2 wuv = (wp.xz + windOffset.xz * 0.25) * weatherScale;
    float3 weather = WeatherTex.SampleLevel(LinearWrap, wuv, 0).rgb;

    // Dual coverage (Haggstrom): R = distinct formations; G floods toward overcast as the
    // coverage slider passes 0.5.
    float wCov = max(weather.r, saturate(2.0 * coverage - 1.0) * weather.g);
    float cov  = saturate(wCov * saturate(2.0 * coverage));

    // Anvil: inflate coverage toward the layer top (pow < 1 raises).
    float anvilExp = Remap(saturate(h), 0.7, 0.8, 1.0, lerp(1.0, 0.5, anvilBias));
    cov = pow(cov, clamp(anvilExp, 0.5, 1.0));
    if (cov <= 1e-4) return 0.0;

    float type = saturate(weather.b + cloudTypeBias);

    // Base shape: Perlin-Worley dilated by the Worley FBM (remap erosion, not multiplication,
    // keeps cloud cores opaque).
    float3 sp = wp + windOffset;
    float4 lf = BaseNoise.SampleLevel(LinearWrap, sp * baseNoiseScale, 0);
    float lfFbm = lf.g * 0.625 + lf.b * 0.25 + lf.a * 0.125;
    float base  = saturate(Remap(lf.r, lfFbm - 1.0, 1.0, 0.0, 1.0));
    base *= HeightGradient(h, type);

    // Coverage as the erosion threshold; the trailing * cov softens low-coverage bottoms
    // (GPU Pro 7).
    float shaped = saturate(Remap(base, 1.0 - cov, 1.0, 0.0, 1.0)) * cov;
    baseShape = shaped;
    if (cheap || shaped <= 1e-4 || gDetailFade <= 1e-3) return shaped * density;

    // Detail erosion: wispy (inverted worley) near the base, billowy above.
    float3 hf = DetailNoise.SampleLevel(LinearWrap, sp * detailNoiseScale, 0).rgb;
    float hfFbm = hf.r * 0.625 + hf.g * 0.25 + hf.b * 0.125;
    float hfMod = lerp(hfFbm, 1.0 - hfFbm, saturate(h * 10.0));
    float eroded = saturate(Remap(shaped,
                                  hfMod * detailStrength * gDetailFade,
                                  1.0, 0.0, 1.0));
    return eroded * density;
}

#endif // CLOUD_DENSITY_HLSLI
