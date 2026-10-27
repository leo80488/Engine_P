// Skybox pixel shader: the environment cube plus the baked starfield, an analytic
// sun disk and the textured moon disk. The sun disk is tinted orange-red near the
// horizon by a wavelength-dependent Beer-Lambert transmittance approximation.

TextureCube gEnvironment : register(t6, space0);
// Moon disk texture (square 2D), in gEmissive's slot. Sampled only when
// MoonVisible > 0, which SkyboxPass sets only while a moon texture is bound;
// otherwise the slot may hold an unrelated texture.
Texture2D<float4> gMoonTex   : register(t17, space0);
// Starfield cube (RGBA16F, alpha = twinkle phase), baked once by StarsBake.cs.hlsl;
// this PS only samples it. t18 is Lighting's SSAO slot: SkyboxPass keeps
// StarIntensity at 0 until a cube is bound there, and RenderStars samples only
// when it is > 0.
TextureCube gStarsTex        : register(t18, space0);
#include "sampler_heap.hlsli"   // samplers from the sampler heap
#define gSampler SmpFixed(SMP_LINEAR_WRAP)

// Sky / sun / moon constants (world space).
// SunDir: unit direction toward the sun that drives the atmosphere.
// MoonDir / MoonDiskCos / MoonColor: the moon disk, drawn when MoonVisible > 0.
cbuffer SkyCB : register(b2, space0)
{
    float3 SunDir;         float SunDiskSize;     // cos(half-angle of the sharp core)
    float3 SunColor;       float SunDiskIntensity;
    float3 MoonDir;        float MoonDiskCos;
    float3 MoonColor;      float MoonVisible;     // 1 = draw moon disk, 0 = skip
    // Starfield params, pushed by the Renderer every frame.
    float  StarIntensity;  // [0,1] day/night blend (0 = invisible, 1 = full night)
    float  StarTime;       // seconds, drives twinkle phase
    float  StarDensity;    // star grid resolution (~250 -> a few thousand stars); read by the bake, not here
    float  StarBrightness; // overall multiplier
};

struct PSIn
{
    float4 pos : SV_POSITION;
    float3 dir : TEXCOORD0;
};

// Starfield, pre-baked into a TextureCube by StarsBake.cs.hlsl:
//   .rgb = star colour x size x magnitude (pre-multiplied)
//   .a   = per-star twinkle phase in (0, 1], 0 where there is no star
// Every texel of one star has the same phase (the hash is per cell), so after
// bilinear sampling all its pixels twinkle together: one object, not pixel noise.
float3 RenderStars(float3 rd)
{
    if (StarIntensity <= 0.0) return float3(0, 0, 0);

    float horizonFade = smoothstep(-0.05, 0.20, rd.y);
    if (horizonFade <= 0.0) return float3(0, 0, 0);

    float4 baked = gStarsTex.SampleLevel(gSampler, rd, 0);
    if (baked.a <= 0.0) return float3(0, 0, 0);  // empty sky

    // Per-star twinkle, phase from baked.a. Brightness swings over [0.60, 1.20];
    // 3 rad/s (~0.5 Hz) is slow enough not to feel nervous.
    const float kTwinkleSpeed = 3.0;
    float phase  = baked.a * 6.28318530718;
    float tw     = 0.90 + 0.30 * sin(StarTime * kTwinkleSpeed + phase);

    // Day/night power curve.
    float fade = StarIntensity * StarIntensity;

    return baked.rgb * tw * horizonFade * fade * StarBrightness;
}

// Approximate atmospheric transmittance toward the sun; sinElev = SunDir.y.
// The longer path near the horizon filters blue and green more than red, giving
// the orange-red sunset tint. ~(0.91, 0.71, 0.42) at the zenith.
float3 SunAtmosphericTint(float sinElev)
{
    // Path length ~ 1 / sin(elev), offset and clamped so it stays finite (<= ~33)
    // at and below the horizon.
    float pathLen = 1.0 / max(sinElev + 0.03, 0.03);
    // Per-channel Rayleigh-like optical depth (lambda^-4 falloff), hand-tuned so no
    // LUT lookup is needed.
    float3 od = float3(0.10, 0.36, 0.90) * pathLen;
    return exp(-od);
}

float4 main(PSIn i) : SV_TARGET
{
    float3 rd  = normalize(i.dir);
    float3 sunDirN = normalize(SunDir);
    float3 sky = gEnvironment.SampleLevel(gSampler, rd, 0.0f).rgb;

    // Stars: additive over the sky cube, gated by StarIntensity (the Renderer's
    // day/night blend). Drawn before the moon disk so the moon covers the stars
    // behind it. The twinkle phase is per star (baked alpha, see RenderStars): a
    // per-pixel hash phase makes each star shimmer like noise.
    sky += RenderStars(rd);

    // Analytic sun disk: pixel-sharp, no cubemap aliasing.
    // SunDiskSize = cos(core half-angle). The Renderer passes a half-angle a little
    // wider than the real sun's ~0.27 deg so the disk survives low resolutions.
    float cosT = dot(rd, sunDirN);
    float core = smoothstep(SunDiskSize - 2e-4, SunDiskSize, cosT);

    // Glow: a fixed 0.0005-wide band in cosine space just outside the core; its
    // start follows SunDiskSize.
    float glowStart = SunDiskSize - 0.0005; // ~1.8 deg of glow around the disk
    float glow      = pow(saturate((cosT - glowStart) / 0.0005), 4.0);

    // Horizon fade: hides the disk smoothly below the horizon so it never shows
    // beneath the ground plane.
    float horizonMask = smoothstep(-0.02, 0.02, sunDirN.y);

    // Atmospheric tint on the disk itself (horizon sun is orange/red).
    float3 tint = SunAtmosphericTint(sunDirN.y);

    // The sun disk is always evaluated: horizonMask hides it below the horizon,
    // and the Renderer sets SunDiskIntensity to 0 outside the procedural atmosphere.
    float  coreAmt = (core + glow * 0.25) * SunDiskIntensity * horizonMask;
    float3 color   = sky + SunColor * tint * coreAmt;

    // Moon disk: drawn from MoonDir, independent of which body drives the lighting.
    if (MoonVisible > 0.0)
    {
        float3 moonDirN = normalize(MoonDir);
        float  moonCosT = dot(rd, moonDirN);

        // Fade the disk while it is still a few degrees above the horizon, so the moon
        // dims gradually instead of staying bright until it clips: fully visible above
        // y = 0.12 (~7 deg), gone at 0.01.
        float moonHorizon = smoothstep(0.01, 0.12, moonDirN.y);

        if (moonHorizon > 0.0 && moonCosT > 0.0)
        {
            float3 upHint    = (abs(moonDirN.y) > 0.99) ? float3(0, 0, 1) : float3(0, 1, 0);
            float3 tangent   = normalize(cross(upHint, moonDirN));
            float3 bitangent = cross(moonDirN, tangent);

            float sinHalf = sqrt(saturate(1.0 - MoonDiskCos * MoonDiskCos));
            float u = dot(rd, tangent)   / max(sinHalf, 1e-4);
            float v = dot(rd, bitangent) / max(sinHalf, 1e-4);

            float r2     = u * u + v * v;
            float inside = saturate(1.0 - r2);
            float mask   = smoothstep(0.0, 0.01, inside) * moonHorizon;

            if (mask > 0.0)
            {
                float2 uv  = float2(u, v) * 0.5 + 0.5;
                float4 tex = gMoonTex.SampleLevel(gSampler, uv, 0.0);
                color += tex.rgb * tex.a *  mask;
            }
        }
    }

    return float4(color, 1.0f);
}

