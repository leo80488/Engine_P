// debug_visualize.hlsli - the debug views' colour maps. DebugTargetView.cs.hlsl paints with
// them for both the debug views and the pass capture's picture (the render-graph panel's
// preview), so a picture means the same thing in both. Pure functions of their arguments: no
// resources, no constant buffers.
//
// Every result is a gamma-space colour in [0, 1] (the debug view writes it straight into the
// composition buffer, HDROutput.h).

#ifndef DEBUG_VISUALIZE_HLSLI
#define DEBUG_VISUALIZE_HLSLI

// NaN / Inf: magenta, as the attachment preview paints it. No colour map
// below produces it.
static const float3 DV_kBad = float3(1.0, 0.0, 1.0);

bool DV_Bad(float v)   { return isnan(v) || isinf(v); }
bool DV_Bad3(float3 v) { return DV_Bad(v.x) || DV_Bad(v.y) || DV_Bad(v.z); }

// A fully saturated hue, h in [0, 1).
float3 DV_Hue(float h)
{
    const float3 k = float3(1.0, 2.0 / 3.0, 1.0 / 3.0);
    return saturate(abs(frac(h + k) * 6.0 - 3.0) - 1.0);
}

// Reverse-Z device depth as grey: 1 - log(viewZ / near) / log(far / near) -
// white at the near plane, black at the far plane and where nothing was drawn
// (depth exactly 0). An orthographic camera's depth is linear: shown as is.
float3 DV_Depth(float d, float nearZ, float farZ, bool orthographic)
{
    if (DV_Bad(d)) return DV_kBad;
    if (orthographic) return saturate(d).xxx;   // the attachment preview's mapping
    if (d <= 0.0) return float3(0.0, 0.0, 0.0);   // reverse-Z 0: far plane / sky
    // Reverse-Z: d = 1 at near, 0 at far. viewZ = n*f / (n + d*(f-n)).
    const float viewZ = nearZ * farZ / (nearZ + d * (farZ - nearZ));
    const float t = log(max(viewZ, nearZ) / nearZ) / max(log(farZ / nearZ), 1e-6);
    return (1.0 - saturate(t)).xxx;
}

// A heat ramp, t in [0, 1]: black, blue, cyan, green, yellow, red, white in six
// equal steps. Below 0 is black, above 1 white.
float3 DV_Heat(float t)
{
    static const float3 kStops[7] = { float3(0.0, 0.0, 0.0), float3(0.0, 0.0, 1.0), float3(0.0, 1.0, 1.0),
                               float3(0.0, 1.0, 0.0), float3(1.0, 1.0, 0.0), float3(1.0, 0.0, 0.0),
                               float3(1.0, 1.0, 1.0) };
    const float x = saturate(t) * 6.0;
    const uint  i = min((uint)x, 5u);
    return lerp(kStops[i], kStops[i + 1], x - (float)i);
}

// An integer category: sixteen fixed, mutually distinct colours, then a
// golden-ratio walk around the hue circle at two brightnesses. Never black and
// never DV_kBad's magenta, so "nothing" (black) and "not a number" stay
// distinguishable from every category.
float3 DV_Categorical(uint id)
{
    static const float3 kPalette[16] = {
        float3(0.90, 0.10, 0.29), float3(0.24, 0.71, 0.29), float3(1.00, 0.88, 0.10), float3(0.26, 0.39, 0.85),
        float3(0.96, 0.51, 0.19), float3(0.57, 0.12, 0.71), float3(0.27, 0.94, 0.94), float3(0.74, 0.96, 0.05),
        float3(0.98, 0.75, 0.83), float3(0.00, 0.50, 0.50), float3(0.86, 0.75, 1.00), float3(0.60, 0.39, 0.14),
        float3(1.00, 0.98, 0.78), float3(0.50, 0.00, 0.00), float3(0.67, 1.00, 0.76), float3(0.50, 0.50, 0.00) };
    if (id < 16u) return kPalette[id];
    float h = frac((float)id * 0.6180339887);
    if (abs(h - 5.0 / 6.0) < 0.06) h = frac(h + 0.12);   // step off magenta (5/6)
    return DV_Hue(h) * ((id & 16u) != 0u ? 0.62 : 0.95);
}

// The stencil byte's layout (Graphics/ShadingStencil.h): the low six bits are a
// shading route - a category, black where nothing was drawn (0) - and the two
// flag bits are diagonal stripes over it, four pixels wide: 0x40 (particles and
// trails kept out of TAA's history) white one way, 0x80 (the outline rim) red
// the other. @p px is the output pixel, for the stripes.
float3 DV_StencilBits(uint s, uint2 px)
{
    const uint route = s & 0x3Fu;
    float3 c = route == 0u ? float3(0.0, 0.0, 0.0) : DV_Categorical(route);
    const int2 p = int2(px);
    if ((s & 0x40u) != 0u && (((p.x + p.y) >> 2) & 1) != 0)        c = float3(1.0, 1.0, 1.0);
    if ((s & 0x80u) != 0u && (((p.x - p.y + 8192) >> 2) & 1) != 0) c = float3(1.0, 0.0, 0.0);
    return c;
}

#endif // DEBUG_VISUALIZE_HLSLI
