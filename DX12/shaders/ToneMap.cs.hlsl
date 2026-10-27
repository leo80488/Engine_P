// Tone map: HDR scene + bloom + lens flare x exposure -> the COMPOSITION BUFFER. ACES fitted in SDR,
// a peak-parameterised GT curve in HDR.
// The output is extended gamma (1.0 == paper white, > 1.0 == HDR headroom), NOT a display signal:
// the present pixel shader does the final PQ / scRGB encode. Read Graphics/HDROutput.h before
// changing the tail of CSMain.
// Bindings (space2): b0 PerDispatch; t0 HDR scene; t1 bloom (mip 0, from BloomPass); t2 exposure
// (StructuredBuffer<float>); t3 lens flare (half-res RGBA16F, additive); t5 grading LUT (3D);
// u0 the composition output (RGBA8 UNORM in SDR, RGBA16F in HDR).

#include "UEToneMap.hlsli"
#include "dither.hlsli"
cbuffer PerDispatch : register(b0, space2)
{
    uint  g_width;
    uint  g_height;
    float g_bloomStrength;   // additive bloom intensity (0..1)
    uint  g_enableLUT;       // 1 = apply color grading LUT, 0 = bypass
    float g_lensFlareStrength; // additive lens flare multiplier (0 = disabled)
    uint  g_hdrMode;         // HDROutputMode: 0 = SDR, 1 = HDR10, 2 = scRGB
    float g_hdrPeakRatio;    // peakNits / paperWhiteNits: display headroom in
                             // paper-white units. 1.0 == no headroom == SDR.
    uint  g_reserved;        // unused; keeps the CB layout (the colour-vision assist runs in Present.ps.hlsl)
    UEToneMapData g_ueGrading;
    uint  g_debugInjectColumns; // harness: the first n columns of the input read NaN, the next n +Inf
    uint  g_debugFlags;         // harness reverse arms: bit 0 skips the non-finite guard, bit 1 the dither
}

Texture2D<float4>        g_hdr       : register(t0, space2);
Texture2D<float4>        g_bloom     : register(t1, space2);
StructuredBuffer<float>  g_exposure  : register(t2, space2);
Texture2D<float4>        g_lensFlare : register(t3, space2);
Texture3D<float4>        g_lut       : register(t5, space2);
RWTexture2D<float4>      g_output    : register(u0, space2);

SamplerState g_linear : register(s0, space2);

// Input c is fetched at the centre of texel c*(N-1): GenerateLUT.cs.hlsl bakes
// texel i from input i/(N-1). Change one side and neutral grading stops being
// an identity.
static const float LUT_SIZE   = 32.0;
static const float LUT_SCALE  = (LUT_SIZE - 1.0) / LUT_SIZE;
static const float LUT_OFFSET = 0.5 / LUT_SIZE;

// ACES Fitted (Stephen Hill, "s2014"): hue-preserving RRT+ODT fit in ACEScg, the curve UE's "ACES"
// output transform uses. Highlights desaturate toward white (saturated sky blues roll off instead
// of staying pure blue).
//   sRGB linear -> ACEScg   via ACESInputMat
//   S-curve in ACEScg       via RRTAndODTFit
//   ACEScg -> sRGB linear   via ACESOutputMat
// The matrices fold sRGB -> ACES2065-1 -> ACEScg and ACEScg -> sRGB into single 3x3s.
static const float3x3 ACESInputMat = {
    { 0.59719f, 0.35458f, 0.04823f },
    { 0.07600f, 0.90834f, 0.01566f },
    { 0.02840f, 0.13383f, 0.83777f }
};
static const float3x3 ACESOutputMat = {
    {  1.60475f, -0.53108f, -0.07367f },
    { -0.10208f,  1.10813f, -0.00605f },
    { -0.00327f, -0.07276f,  1.07602f }
};

float3 RRTAndODTFit(float3 v)
{
    float3 a = v * (v + 0.0245786f) - 0.000090537f;
    float3 b = v * (0.983729f * v + 0.4329510f) + 0.238081f;
    return a / b;
}

float3 ACESFitted(float3 color)
{
    color = mul(ACESInputMat, color);
    color = RRTAndODTFit(color);
    color = mul(ACESOutputMat, color);
    return saturate(color);
}

// ==== HDR tone curve: peak-parameterised, mid-gray anchored ====
// ACESFitted cannot simply drop its saturate(): RRTAndODTFit is fitted for a ~100-nit display and
// ASYMPTOTES at 1/0.983729 = 1.0165, so the curve itself destroys the highlights.
// This is the Uchimura "GT" curve (toe / linear / exponential shoulder), per channel in ACEScg so it
// shares the SDR path's hue and desaturation. Its shoulder asymptotes at exactly P (the headroom in
// paper-white units); toe and mid-tones do not depend on P.
// The four constants are not the published GT defaults: they are fitted numerically to this file's
// ACESFitted() at P = 1 (RMS 1.94/255, max 3.47/255 over display output 0.02..0.85), so HDR matches
// the SDR look below the highlight roll-off. Mid-gray (scene 0.18) maps to 0.10110 for EVERY P; the
// curve is monotone for P in [1, 50] over scene 1e-6..1e5, and with the input cap below finite for
// every input, Inf and negative included. Re-fit the constants if ACESFitted changes.
static const float kGT_InputScale = 0.680722f;  // scene pre-scale into the curve
static const float kGT_Contrast   = 0.490461f;  // 'a': slope of the linear part
static const float kGT_LinearStart= 0.532571f;  // 'm': toe/linear crossover
static const float kGT_ToePower   = 1.428136f;  // 'c': black tightness

float3 GTToneCurve(float3 x, float P)
{
    // P < 1 (a display peaking BELOW paper white) is degenerate: C2 blows up as P approaches m. The
    // C++ setter already enforces peakNits >= paperWhiteNits; this is defence in depth.
    P = max(P, 1.0f);

    const float m = kGT_LinearStart;
    const float a = kGT_Contrast;
    const float c = kGT_ToePower;

    // Shoulder is C1-continuous with the linear segment at x == m.
    const float C2 = (a * P) / (P - m);
    const float CP = -C2 / P;

    // Capped at the largest half: the shoulder already sits at P there, and the toe's pow() of a larger
    // x overflows float32 to Inf, which its zero weight then turns into NaN.
    x = min(max(x, 0.0f), 65504.0f) * kGT_InputScale;

    // Segment weights: smooth toe->linear blend, hard linear->shoulder switch
    // at x == m (where the two are already tangent).
    float3 t  = saturate(x / m);
    float3 w0 = 1.0f - t * t * (3.0f - 2.0f * t);   // toe
    float3 w2 = step(m, x);                          // shoulder
    float3 w1 = 1.0f - w0 - w2;                      // linear

    // max() before pow(): pow(0, c) is undefined in HLSL.
    float3 T = m * pow(max(x, 1e-9f) / m, c);
    // clamp() the exponent: x is unbounded scene luminance; CP < 0 makes large x underflow, but an inf
    // here would turn into a NaN when multiplied by a zero weight.
    float3 S = P - (P - m) * exp(clamp(CP * (x - m), -60.0f, 60.0f));
    float3 L = m + a * (x - m);

    return T * w0 + L * w1 + S * w2;
}

// ACEScg working space + GT curve. Output is LINEAR, 1.0 == paper white, highlights up to ~P. The
// ACEScg -> sRGB matrix can push a single very saturated channel somewhat past P; the display's
// roll-off handles that, and clamping here would desaturate it.
float3 ACESToneMapHDR(float3 color, float peakRatio)
{
    color = mul(ACESInputMat, color);
    color = GTToneCurve(color, peakRatio);
    color = mul(ACESOutputMat, color);
    return max(color, 0.0f);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_width || id.y >= g_height) return;

    float2 uv = (float2(id.xy) + 0.5f) / float2(g_width, g_height);

    float3 hdr   = g_hdr.SampleLevel(g_linear, uv, 0).rgb;
    float3 bloom = g_bloom.SampleLevel(g_linear, uv, 0).rgb;
    float3 flare = g_lensFlare.SampleLevel(g_linear, uv, 0).rgb;

    // The guard NPRStabilize.cs.hlsl applies too: a NaN/Inf that slipped past the meter, or a
    // manualExposure of 0 (a legal authored value), would otherwise turn the whole frame black with
    // no in-editor control to undo it.
    float exposure = g_exposure[0];
    if (!isfinite(exposure) || exposure < 1e-4) exposure = 1.0;

    float3 color = hdr + bloom * g_bloomStrength + flare * g_lensFlareStrength;

    if (id.x < 2u * g_debugInjectColumns)
        color = asfloat(id.x < g_debugInjectColumns ? 0x7FC00000u : 0x7F800000u).xxx;
    // A non-finite input would come out black, Inf included (ACESFitted and the GT toe make it NaN,
    // saturate() makes that 0): an fp16 overflow in the brightest highlight would show as a black dot.
    // NaN reads as black; the rest is clamped to [0, the largest half], so Inf tone maps to white.
    if ((g_debugFlags & 1u) == 0u)
    {
        if (any(isnan(color))) color = 0.0f;
        color = min(max(color, 0.0f), 65504.0f);
    }

    color *= exposure;
    // colorGrading.vignetteStrength (controls.z): a cos^4 falloff on the exposed scene colour, ahead
    // of the tone curve, whether or not the UE film grading is on. Zero skips it.
    if (g_ueGrading.controls.z > 0.0)
    {
        float aspect=float(g_height)/max(float(g_width),1);
        float2 v=(uv*2-1)*float2(1,aspect)*sqrt(2/(1+aspect*aspect))*g_ueGrading.controls.z;
        float cos2=rcp(1+dot(v,v));
        color*=cos2*cos2;
    }

    // Tone map, then encode into the COMPOSITION SPACE: pow(linear / paperWhite, 1/2.2), 1.0 = paper
    // white, above 1.0 = HDR headroom. Everything downstream (the overlay passes, GlassShatter, the
    // editor's ImGui) alpha-blends in that space; the present pixel shader decodes and does the PQ /
    // scRGB encode (Graphics/HDROutput.h).
    if (g_hdrMode == 0)
    {
        // SDR: ACESFitted / the saturated UE curve stays in [0,1], so the encode below never exceeds 1.0.
        color = g_ueGrading.controls.x > .5 ? saturate(UEToneMap(color,g_ueGrading)) : ACESFitted(color);
    }
    else
    {
        // HDR: the same ACEScg working space, peak-parameterised curve, no clamp.
        if (g_ueGrading.controls.x > .5)
            color = max(0,mul(UE_AP1_SRGB,UEGrade(mul(UE_SRGB_AP1,color),g_ueGrading)));
        color = ACESToneMapHDR(color, g_hdrPeakRatio);
    }

    // SDR stays in [0,1]. The colour-vision assist is Present.ps.hlsl's: it must see the HUD too.
    if (g_hdrMode == 0) color = saturate(color);

    // Linear -> gamma with the same exponent in both modes: in SDR the sRGB approximation, in HDR the
    // composition-space encode whose exact inverse is the present shader's pow(x, 2.2).
    color = pow(max(color, 0.0f), 1.0f / 2.2f);

    // Color grading LUT. It is baked over a neutral [0,1] domain, so only the SDR band is graded:
    // headroom above 1.0 passes through untouched and the transition stays continuous. In SDR hi is
    // zero, so this is a single LUT fetch.
    if (g_enableLUT)
    {
        float3 lo  = min(color, 1.0f);
        float3 hi  = color - lo;
        float3 uvw = lo * LUT_SCALE + LUT_OFFSET;
        color = g_lut.SampleLevel(g_linear, uvw, 0).rgb + hi;
    }

    // SDR quantises to 8 bits on this write: a static +-0.5 LSB dither keeps a smooth gradient from
    // banding. Static (no frame term), so a still frame stays bit-stable; just under half a level, so
    // an exact level - black and white included - still rounds to itself.
    if (g_hdrMode == 0 && (g_debugFlags & 2u) == 0u)
        color = saturate(color + (StaticDither(id.xy) - 0.5f) * (0.999f / 255.0f));

    g_output[id.xy] = float4(color, 1.0f);
}
