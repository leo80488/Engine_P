#ifndef TAA_HISTORY_HLSLI
#define TAA_HISTORY_HLSLI

// TAA history reconstruction: a full 9-tap Catmull-Rom bicubic.
// 9 taps, not the optimised 5-tap form: dropping the four corners (bilinear overlap
// on equal-weight quadrants) loses energy on sharp diagonal edges and on
// sub-pixel-translated history in motion, which the variance clip compounds into
// edge softening over time. The 4 extra SampleLevels are not measurable at TAA
// resolutions.
// Reference: Falcor TAA.ps.slang::bicubicSampleCatmullRom,
//   http://vec3.ca/bicubic-filtering-in-fewer-taps/

float3 SampleHistoryCatmullRom9Tap(Texture2D<float4> tex,
                                    SamplerState     samp,
                                    float2           samplePos, // pixel-space (uv * dim)
                                    float2           texDim)
{
    float2 invTex = 1.0 / texDim;

    float2 tc = floor(samplePos - 0.5) + 0.5;
    float2 f  = samplePos - tc;
    float2 f2 = f * f;
    float2 f3 = f2 * f;

    float2 w0 = f2 - 0.5 * (f3 + f);
    float2 w1 = 1.5 * f3 - 2.5 * f2 + 1.0;
    float2 w3 = 0.5 * (f3 - f2);
    float2 w2 = 1.0 - w0 - w1 - w3;

    float2 w12 = w1 + w2;

    float2 tc0  = (tc - 1.0)         * invTex;
    float2 tc12 = (tc + w2 / w12)    * invTex;
    float2 tc3  = (tc + 2.0)         * invTex;

    // Per-tap max(0, ...) clamp. CR's negative lobe weights (w0, w3 can go
    // negative around f=0.5) can amplify any negative-component sample (hist
    // exposed to a previous negative blend on edges + high contrast) into a
    // negative weighted-sum contribution that survives the OUTER clamp at
    // the call site. Particularly bad for thin high-contrast features over
    // textured backgrounds where contamination chains across frames. Falcor's
    // reference does the same.
    // clang-format off
    float3 r =
        max(tex.SampleLevel(samp, float2(tc0.x,  tc0.y),  0).rgb, 0.0) * (w0.x  * w0.y)  +
        max(tex.SampleLevel(samp, float2(tc12.x, tc0.y),  0).rgb, 0.0) * (w12.x * w0.y)  +
        max(tex.SampleLevel(samp, float2(tc3.x,  tc0.y),  0).rgb, 0.0) * (w3.x  * w0.y)  +
        max(tex.SampleLevel(samp, float2(tc0.x,  tc12.y), 0).rgb, 0.0) * (w0.x  * w12.y) +
        max(tex.SampleLevel(samp, float2(tc12.x, tc12.y), 0).rgb, 0.0) * (w12.x * w12.y) +
        max(tex.SampleLevel(samp, float2(tc3.x,  tc12.y), 0).rgb, 0.0) * (w3.x  * w12.y) +
        max(tex.SampleLevel(samp, float2(tc0.x,  tc3.y),  0).rgb, 0.0) * (w0.x  * w3.y)  +
        max(tex.SampleLevel(samp, float2(tc12.x, tc3.y),  0).rgb, 0.0) * (w12.x * w3.y)  +
        max(tex.SampleLevel(samp, float2(tc3.x,  tc3.y),  0).rgb, 0.0) * (w3.x  * w3.y);
    // clang-format on
    return r;
}

#endif // TAA_HISTORY_HLSLI
