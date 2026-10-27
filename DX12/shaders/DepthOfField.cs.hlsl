// DepthOfField.cs.hlsl - focus-distance depth of field (HDR, pre-tonemap).
//
// Per pixel: linearize the reverse-Z scene depth, derive a circle of confusion
// (CoC) from the distance to the focus plane, then gather the HDR colour over a
// disk whose radius ~ CoC. Taps are weighted by their OWN CoC (scatter-as-
// gather), so a sharp foreground does not bleed into blurred neighbours.
//
// Compute root signature (space2):
//   b0 - DoFCB
//   t0 - HDR colour (R16G16B16A16_FLOAT)
//   t1 - hardware depth (reverse-Z: 0 = far/sky, 1 = near)
//   u0 - HDR output
//   s0 - linear clamp sampler (static)

cbuffer DoFCB : register(b0, space2)
{
    uint  width;
    uint  height;
    float nearZ;
    float farZ;

    float focusDistance;    // world units - the distance that stays perfectly sharp
    float focusRange;       // half-width (world units) of the in-focus band
    float transitionRange;  // world units over which CoC ramps 0 -> 1 past the band
    float maxRadius;        // max blur radius in pixels (aperture proxy)
};

Texture2D<float4>   gColor  : register(t0, space2);
Texture2D<float>    gDepth  : register(t1, space2);
RWTexture2D<float4> gOutput : register(u0, space2);
SamplerState        gLinear : register(s0, space2);

// Reverse-Z hardware depth -> linear view-space distance (world units):
// d = 0 -> farZ, d = 1 -> nearZ.
#include "core/depth.hlsli"   // LinearizeReverseZ
float LinearizeZ(float d)
{
    return LinearizeReverseZ(d, nearZ, farZ);
}

// 0 in the focus band, ramps to 1 over transitionRange beyond it.
float CoC(float z)
{
    float dist = abs(z - focusDistance);
    return saturate((dist - focusRange) / max(transitionRange, 1e-4));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height) return;

    const float2 dims  = float2(width, height);
    const float2 uv    = (float2(id.xy) + 0.5) / dims;

    // gColor is display-res (the dispatch / UV domain), but gDepth may be a smaller
    // render-res buffer while an upscaler (FSR) is active. Sample depth by
    // normalized UV, not display texel coords, so it stays aligned whatever its real
    // size (and never reads out of bounds). At native res the UV lands on the texel
    // center, identical to a point Load.
    const float4 centerColor  = gColor.SampleLevel(gLinear, uv, 0);
    const float  centerZ      = LinearizeZ(gDepth.SampleLevel(gLinear, uv, 0));
    const float  centerCoC    = CoC(centerZ);
    const float  centerRadius = centerCoC * maxRadius;

    // In focus -> passthrough (cheap early-out for the sharp band).
    if (centerRadius < 0.5)
    {
        gOutput[id.xy] = centerColor;
        return;
    }

    float3 accum = centerColor.rgb;
    float  wsum  = 1.0;

    // Two concentric rings (8 + 16 = 24 taps) scaled by the center CoC radius.
    const int kRings = 2;
    [unroll]
    for (int r = 0; r < kRings; ++r)
    {
        const float ringScale  = float(r + 1) / float(kRings);  // 0.5, 1.0
        const int   taps       = 8 * (r + 1);                   // 8, 16
        const float ringRadius = centerRadius * ringScale;

        for (int s = 0; s < taps; ++s)
        {
            const float  ang  = 6.2831853 * (float(s) + 0.5) / float(taps);
            const float2 dir  = float2(cos(ang), sin(ang));
            const float2 tapP = float2(id.xy) + 0.5 + dir * ringRadius;
            const float2 tuv  = tapP / dims;

            const float3 tcol = gColor.SampleLevel(gLinear, tuv, 0).rgb;
            const float  tz   = LinearizeZ(gDepth.SampleLevel(gLinear, tuv, 0));
            const float  tCoCRadius = CoC(tz) * maxRadius;

            // A tap contributes only if its own blur disk reaches the center pixel.
            const float w = saturate(tCoCRadius - ringRadius + 1.0);
            accum += tcol * w;
            wsum  += w;
        }
    }

    gOutput[id.xy] = float4(accum / wsum, centerColor.a);
}
