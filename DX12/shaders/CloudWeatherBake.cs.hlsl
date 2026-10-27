// One-shot bake of the 512^2 tileable weather map (the Nubis 2017 "cloud map"):
//   R = primary coverage: Perlin FBM remapped into connected formations with real gaps
//   G = secondary coverage: denser Perlin-Worley field the raymarch blends in as global
//       coverage -> 1, to reach overcast
//   B = cloud type in [0, 1]: 0 = stratus, 0.5 = stratocumulus, 1 = cumulus; low frequency,
//       so types form regional bands
//   A = 1 (reserved)
// Sampled planar (world XZ x weatherScale) with hardware WRAP, scrolled at a fraction of the
// wind offset.

RWTexture2D<float4> WeatherOut : register(u0, space2);

static const float kDim = 512.0;

float2 Hash2(int2 c, int2 gridDim)
{
    c = ((c % gridDim) + gridDim) % gridDim;
    float2 p = float2(c) + 0.5;
    p = frac(p * float2(443.897, 441.423));
    p += dot(p, p.yx + 19.19);
    return frac(float2((p.x + p.y) * p.y, (p.x - p.y) * p.x));
}

float2 GradDir2(int2 c, int2 gridDim)
{
    float a = Hash2(c, gridDim).x * 6.28318530;
    return float2(cos(a), sin(a));
}

// Tileable 2D gradient (Perlin) noise in [-1, 1].
float Perlin2(float2 p, int gridDim)
{
    p *= float(gridDim);
    int2 ip = (int2)floor(p);
    float2 fp = frac(p);
    float2 u = fp * fp * fp * (fp * (fp * 6.0 - 15.0) + 10.0);

    int2 gd = int2(gridDim, gridDim);
    float v00 = dot(GradDir2(ip + int2(0,0), gd), fp - float2(0,0));
    float v10 = dot(GradDir2(ip + int2(1,0), gd), fp - float2(1,0));
    float v01 = dot(GradDir2(ip + int2(0,1), gd), fp - float2(0,1));
    float v11 = dot(GradDir2(ip + int2(1,1), gd), fp - float2(1,1));

    return lerp(lerp(v00, v10, u.x), lerp(v01, v11, u.x), u.y);
}

// The amplitude-normalised octave sum |n / norm| peaks at ~0.40 for the octave counts in
// main() (independent octaves almost never align), so a plain * 0.5 + 0.5 spans only
// ~[0.30, 0.70]. The Remap() constants in main() assume a full [0, 1] input; without the gain,
// coverage cannot reach overcast, density caps at ~0.5 and cumulus is unreachable.
// kFbmGain = 1 / (2 x 0.40) fills [0, 1]; saturate() clips the rare overshoot.
static const float kFbmGain = 1.25;

float PerlinFBM2(float2 p, int baseFreq, int octaves)
{
    float n = 0.0, amp = 0.5, norm = 0.0;
    int freq = baseFreq;
    [loop] for (int i = 0; i < octaves; ++i)
    {
        n    += Perlin2(p, freq) * amp;
        norm += amp;
        amp  *= 0.5;
        freq *= 2;
    }
    return saturate(n / max(norm, 1e-4) * kFbmGain + 0.5);
}

// Tileable 2D Worley distance in [0, 1].
float Worley2(float2 p, int gridDim)
{
    p *= float(gridDim);
    int2 ip = (int2)floor(p);
    float2 fp = frac(p);

    float minDist = 1e9;
    [unroll] for (int x = -1; x <= 1; ++x)
    [unroll] for (int y = -1; y <= 1; ++y)
    {
        int2 c = ip + int2(x, y);
        float2 feature = float2(x, y) + Hash2(c, int2(gridDim, gridDim));
        float2 r = feature - fp;
        minDist = min(minDist, dot(r, r));
    }
    return saturate(sqrt(minDist));
}

float Remap(float v, float lo, float hi, float newLo, float newHi)
{
    return newLo + (v - lo) * (newHi - newLo) / max(1e-5, hi - lo);
}

[numthreads(8, 8, 1)]
void main(uint2 dt : SV_DispatchThreadID)
{
    const float2 uv = (float2(dt) + 0.5) / kDim;

    // R: connected formations with gaps. Threshold-remap of a mid-frequency FBM; values below
    // the cut vanish -> discrete cloud objects.
    float pf = PerlinFBM2(uv, 4, 5);
    float coverage = saturate(Remap(pf, 0.42, 1.0, 0.0, 1.0));

    // G: denser fill field (Perlin gated by inverted Worley -> cellular islands with connective
    // tissue), used to flood the sky toward overcast.
    float pf2 = PerlinFBM2(uv + 0.37, 6, 4);          // decorrelated offset
    float cells = 1.0 - Worley2(uv, 6);
    float fill = saturate(Remap(pf2 * (0.55 + 0.45 * cells), 0.20, 1.0, 0.0, 1.0));

    // B: cloud type, very low frequency so types come in regional bands.
    float type = PerlinFBM2(uv + 0.71, 2, 3);
    type = saturate(Remap(type, 0.25, 0.75, 0.0, 1.0)); // widen to the full range

    WeatherOut[dt] = float4(coverage, fill, type, 1.0);
}
