// core/color.hlsli - colour arithmetic shared by every pass. The rules every core header
// follows are at the top of core/gbuffer_codec.hlsli.

#ifndef CORE_COLOR_HLSLI
#define CORE_COLOR_HLSLI

// Relative luminance of LINEAR Rec.709 / sRGB-primary RGB: the engine's one
// luminance (exposure metering, bloom, TAA, SSR, ReSTIR targets, NPR knobs).
// Clamp first where a value can be negative (an unbiased estimate): the caller's
// Luminance(max(c, 0)).
float Luminance(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// Rec.601 luma weights: FXAA's edge metric, kept on purpose (FXAA is tuned on
// it); not radiometry.
float LuminanceRec601(float3 c)
{
    return dot(c, float3(0.299, 0.587, 0.114));
}

#endif // CORE_COLOR_HLSLI
