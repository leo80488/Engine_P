#ifndef SPOT_SHADOW_FILTER_HLSLI
#define SPOT_SHADOW_FILTER_HLSLI

// Exact expansion of the 3x3 bilinear comparison tent (1,2,1 weights) into
// its 4x4 texel footprint. Hardware SampleCmp compares four texel centers to
// ONE reference. Even a receiver-plane correction at the sample UV leaves
// those four comparisons biased on a slope. Correct at each actual center.
float FilterSpotShadowPlane(Texture2DArray<float> atlas, uint slice, uint width,
                            float2 uv, float depth, float2 depthGradient)
{
    float2 texelPos = uv * float(width) - 0.5;
    int2 base = int2(floor(texelPos));
    float2 f = frac(texelPos);
    float4 wx = float4(1.0-f.x, 2.0-f.x, 1.0+f.x, f.x);
    float4 wy = float4(1.0-f.y, 2.0-f.y, 1.0+f.y, f.y);
    float visibility = 0.0;
    [unroll] for (int y = 0; y < 4; ++y)
    [unroll] for (int x = 0; x < 4; ++x)
    {
        int2 coord = clamp(base + int2(x-1, y-1), int2(0,0), int2(width-1, width-1));
        float2 centerUV = (float2(coord) + 0.5) / float(width);
        float reference = saturate(depth + dot(depthGradient, centerUV - uv));
        float stored = atlas.Load(int4(coord, int(slice), 0));
        // D32 reversed Z, including clear depth == 0: GREATER_EQUAL.
        visibility += wx[x] * wy[y] * (reference >= stored ? 1.0 : 0.0);
    }
    return visibility * (1.0 / 16.0);
}

#endif
