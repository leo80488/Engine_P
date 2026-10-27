// The present composite: the display encode (SurfaceDX12::EnsurePresentPSO), the one place
// the engine converts from its composition space (Graphics/HDROutput.h) to the signal the
// display expects. Its inverse is the tail of ToneMap.cs.hlsl: keep the two exact inverses
// (the same 2.2 exponent, no clamp between them). Loaded through
// ShaderDiskCache::LoadOrCompile: cached, recorded as a variant (a package carries it
// cooked), compiled by the offline gates and hot-reloaded.
//
// accessibility.colorVision runs here, the last stage of the picture: after the grading LUT, the
// HUD, the world UI and the editor's panels, so the correction reaches every pixel the player
// sees. With it off (mode 0) SDR is a pure passthrough.

#include "dither.hlsli"

cbuffer PresentCB : register(b0)
{
    uint  g_mode;            // 0 = SDR, 1 = HDR10, 2 = scRGB
    float g_paperWhiteNits;  // nit value that composition 1.0 maps to
    float g_peakNits;        // panel peak; informational here
    uint  g_debugFlags;      // harness reverse arms: bit 0 drops the HDR10 dither
    uint  g_colorVision;     // 0 off, 1 protanopia, 2 deuteranopia, 3 tritanopia
    float g_colorVisionSeverity;   // 0..1: how much of the deficiency the correction assumes
};
Texture2D    g_tex : register(t0);
SamplerState g_smp : register(s0);

// Rec.709 -> Rec.2020 primaries (row-major; mul(M, v) treats v as a column).
static const float3x3 kRec709ToRec2020 = {
    0.627403896, 0.329283039, 0.043313063,
    0.069097289, 0.919540395, 0.011362316,
    0.016391439, 0.088013308, 0.895595253 };

// SMPTE ST.2084 inverse EOTF. Input is luminance normalized so that
// 1.0 == 10000 nits; output is the PQ code value the display decodes.
float3 PQEncode(float3 L)
{
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    float3 Lm = pow(max(L, 0.0), m1);
    return pow((c1 + c2 * Lm) / (1.0 + c3 * Lm), m2);
}

// Daltonization on display-linear RGB. What the deficient eye loses (its LMS response simulated
// after Vienot et al., blended toward the full dichromat by severity) is shifted into the channels
// it still tells apart. Linear in both directions, so the scale of the input does not matter and
// HDR headroom passes through.
float3 CorrectColorVision(float3 rgb, uint mode, float severity)
{
    const float3x3 kRgbToLms = float3x3(17.8824,   43.5161,  4.11935,
                                        3.45565,   27.1554,  3.86714,
                                        0.0299566, 0.184309, 1.46709);
    const float3x3 kLmsToRgb = float3x3( 0.0809444479,   -0.130504409,   0.116721066,
                                        -0.0102485335,    0.0540193266, -0.113614708,
                                        -0.000365296938, -0.00412161469, 0.693511405);
    const float3 lms = mul(kRgbToLms, rgb);
    float3 seen = lms;
    if (mode == 1)      seen.x = 2.02344 * lms.y - 2.52581 * lms.z;
    else if (mode == 2) seen.y = 0.494207 * lms.x + 1.24827 * lms.z;
    else                seen.z = -0.395913 * lms.x + 0.801109 * lms.y;
    seen = lerp(lms, seen, saturate(severity));
    const float3 lost = rgb - mul(kLmsToRgb, seen);
    const float3x3 kShift = float3x3(0.0, 0.0, 0.0,
                                     0.7, 1.0, 0.0,
                                     0.7, 0.0, 1.0);
    return max(rgb + mul(kShift, lost), 0.0);
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float4 c = g_tex.Sample(g_smp, uv);
    const bool assist = g_colorVision != 0u && g_colorVisionSeverity > 0.0;

    // SDR: plain passthrough; the composition buffer already holds sRGB-ish gamma.
    if (g_mode == 0 && !assist) return c;

    // Extended gamma -> linear, normalized so 1.0 == paper white.
    // Exact inverse of the pow(1/2.2) in ToneMap.cs.hlsl. Values above
    // 1.0 are HDR headroom and MUST NOT be clamped here.
    float3 lin = pow(max(c.rgb, 0.0), 2.2);

    if (assist) lin = CorrectColorVision(lin, g_colorVision, g_colorVisionSeverity);

    // SDR re-encodes what it decoded, re-dithered: the 8-bit write below would otherwise band a
    // smooth gradient the correction stretched.
    if (g_mode == 0)
    {
        const float3 enc = pow(saturate(lin), 1.0 / 2.2);
        return float4(saturate(enc + (StaticDither(uint2(pos.xy)) - 0.5) * (0.999 / 255.0)), c.a);
    }

    // scRGB: linear Rec.709 with 1.0 == 80 nits, by definition.
    if (g_mode == 2) return float4(lin * (g_paperWhiteNits / 80.0), c.a);

    // HDR10: absolute nits in Rec.2020, then PQ.
    float3 nits  = lin * g_paperWhiteNits;
    float3 r2020 = mul(kRec709ToRec2020, nits);
    // Rec.709 -> Rec.2020 is gamut-EXPANDING, so components stay >= 0
    // for in-gamut input; the max() guards against negatives arriving
    // from a wide-gamut composition value, which pow() would NaN on.
    float3 pq = PQEncode(max(r2020, 0.0) / 10000.0);
    // The PQ signal is 10 bits: the same static +-0.5 LSB dither ToneMap.cs.hlsl gives SDR's 8 bits.
    if ((g_debugFlags & 1u) == 0u)
        pq += (StaticDither(uint2(pos.xy)) - 0.5) * (0.999 / 1023.0);
    return float4(pq, c.a);
}
