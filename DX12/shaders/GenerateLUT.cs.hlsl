// GenerateLUT.cs.hlsl - bakes a 32x32x32 procedural 3D color grading LUT; each
// texel maps an RGB input -> graded RGB output.
//
// Root signature (shared compute, space2):
//   [0] CBV  b0 space2  - ColorGradingParams
//   [5] UAV  u1 space2  - RWTexture3D output LUT

cbuffer ColorGradingCB : register(b0, space2)
{
    // Exposure & Contrast
    float cb_exposure;
    float cb_contrast;
    float cb_brightness;
    uint  cb_debugFlags;   // harness: bit 0 = the old tint sign and exposure maths (a reverse arm)

    // Lift / Gamma / Gain
    float3 cb_lift;   float _pad1;
    float3 cb_gamma;  float _pad2;
    float3 cb_gain;   float _pad3;

    // HSL
    float cb_hueShift;
    float cb_saturation;
    float cb_vibrance;
    float _pad4;

    // White Balance
    float cb_temperature;
    float cb_tint;
    float _pad5a, _pad5b;

    // Vignette & Grain (unused by the LUT bake; present for the layout match)
    float cb_vignetteStrength;
    float cb_filmGrain;
    float _pad6a, _pad6b;
};

RWTexture3D<float4> g_lutOut : register(u1, space2);

static const uint LUT_SIZE = 32;

// ==== Grading operators ====

// +temperature warms (red up, blue down); +tint goes magenta (green down), as ColorGradingParams says.
float3 ApplyWhiteBalance(float3 c, float temp, float tint)
{
    float t = temp * 0.1;
    float g = (cb_debugFlags & 1u) ? 1.0 + tint * 0.05 : 1.0 - tint * 0.05;
    float3 wb = float3(1.0 + t, g, 1.0 - t);
    return c * wb;
}

// The LUT's input is the gamma-encoded display value (ToneMap.cs.hlsl encodes with pow(1/2.2) before
// the fetch), so ev stops of light are x 2^(ev / 2.2) on it. The result clips at 1 in ApplyContrast.
float3 ApplyExposure(float3 c, float ev)
{
    return c * exp2((cb_debugFlags & 1u) ? ev : ev / 2.2);
}

float3 ApplyLiftGammaGain(float3 c, float3 lift, float3 gamma, float3 gain)
{
    c = c * gain + lift;
    // No floor above 0: it lifted black. pow(0, y) is 0 for the y > 0 the gamma clamp guarantees.
    c = pow(max(c, 0.0), 1.0 / max(gamma, 0.001));
    return c;
}

float3 ApplyContrast(float3 c, float contrast)
{
    return saturate((c - 0.5) * contrast + 0.5);
}

float3 RGBtoHSV(float3 c)
{
    float4 K = float4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    float4 p = lerp(float4(c.bg, K.wz), float4(c.gb, K.xy), step(c.b, c.g));
    float4 q = lerp(float4(p.xyw, c.r), float4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return float3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

float3 HSVtoRGB(float3 c)
{
    float4 K = float4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    float3 p = abs(frac(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * lerp(K.xxx, saturate(p - K.xxx), c.y);
}

float3 ApplyHueSatVibrance(float3 c, float hueShift, float sat, float vib)
{
    float3 hsv = RGBtoHSV(c);
    hsv.x = frac(hsv.x + hueShift / 360.0);
    float vibranceAdj = (1.0 - hsv.y) * vib;
    hsv.y = saturate(hsv.y * sat + vibranceAdj);
    return HSVtoRGB(hsv);
}

[numthreads(4, 4, 4)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= LUT_SIZE)) return;

    // Neutral color: texel i holds the input i/(N-1), so the lattice spans exactly
    // [0,1]. ToneMap.cs.hlsl samples input c at c*(N-1)/N + 0.5/N (LUT_SCALE /
    // LUT_OFFSET), the centre of texel c*(N-1), and trilinear filtering reproduces a
    // linear map exactly - so neutral parameters bake a true identity. Do not bake
    // (i+0.5)/N: neutral grading would map c -> c*31/32 + 1/64 (white 255 -> 251,
    // black 0 -> 4).
    float3 color = float3(id) / float(LUT_SIZE - 1);

    color = ApplyWhiteBalance(color, cb_temperature, cb_tint);

    color = ApplyExposure(color, cb_exposure);

    color = ApplyLiftGammaGain(color, cb_lift, cb_gamma, cb_gain);

    color = ApplyContrast(color, cb_contrast);

    color = saturate(color + cb_brightness);

    color = ApplyHueSatVibrance(color, cb_hueShift, cb_saturation, cb_vibrance);

    g_lutOut[id] = float4(saturate(color), 1.0);
}
