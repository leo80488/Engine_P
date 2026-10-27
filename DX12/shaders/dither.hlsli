#ifndef DITHER_HLSLI
#define DITHER_HLSLI

// ==== Screen-door thresholds for BlendMode::Dithered ====
// Interleaved gradient noise (Jimenez 2014): a per-pixel threshold with no visible structure.
// With the R2-style frame offset it decorrelates from frame to frame, so a temporal AA integrates
// `clip(coverage - threshold)` into coverage-weighted transparency. 5.588238 is the offset of the
// original TAA-dithering recipe; 64 frames is far more than any history length here.
//
// The static variant is for the shadow maps: they are not temporally filtered, so a per-frame
// threshold would flicker there; a fixed pattern gives a stable PCF-averaged partial shadow whose
// density is the coverage.
float DitherIGN(float2 p)
{
    return frac(52.9829189 * frac(0.06711056 * p.x + 0.00583715 * p.y));
}

float TemporalDither(uint2 px, uint frameIndex)
{
    const float f = float(frameIndex % 64u);
    return DitherIGN(float2(px) + f * 5.588238);
}

float StaticDither(uint2 px)
{
    return DitherIGN(float2(px));
}

#endif // DITHER_HLSLI
