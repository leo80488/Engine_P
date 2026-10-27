// Bloom mip chain, two entry points:
//   CSDownsample - 13-tap Sledgehammer kernel, Karis average on the first pass
//   CSUpsample   - 3x3 tent, added onto the destination mip
// Compute root signature (space2): [0] CBV b0 PerDispatch, [1] SRV t0 input, [4] UAV u0 output.

cbuffer PerDispatch : register(b0, space2)
{
    uint  g_srcWidth;
    uint  g_srcHeight;
    uint  g_dstWidth;
    uint  g_dstHeight;
    float g_filterRadius;   // upsample tent radius, in source texels
    uint  g_firstDownsample; // 1 = Karis average (first downsample)
    float _pad0;
    float _pad1;
}

Texture2D<float4>   g_input  : register(t0, space2);
RWTexture2D<float4> g_output : register(u0, space2);

SamplerState g_linear : register(s0, space2);

#include "core/color.hlsli"   // Luminance (BT.709)

// Karis Average: luminance-weighted average that suppresses fireflies.
float4 KarisAverage(float4 a, float4 b, float4 c, float4 d)
{
    float wa = 1.0f / (1.0f + Luminance(a.rgb));
    float wb = 1.0f / (1.0f + Luminance(b.rgb));
    float wc = 1.0f / (1.0f + Luminance(c.rgb));
    float wd = 1.0f / (1.0f + Luminance(d.rgb));
    float sum = wa + wb + wc + wd;
    return (a * wa + b * wb + c * wc + d * wd) / sum;
}

// ==== 13-tap Sledgehammer downsample: 5 overlapping 2x2 quads around the centre ====
float4 DownsampleBox13(float2 uv)
{
    float2 texelSize = float2(1.0f / float(g_srcWidth), 1.0f / float(g_srcHeight));
    float2 e = texelSize;

    // Centre quad weight 0.5, four corner quads 0.125 each -> total 1.0.
    float4 a = g_input.SampleLevel(g_linear, uv + float2(-2.0f,  2.0f) * e, 0);
    float4 b = g_input.SampleLevel(g_linear, uv + float2( 0.0f,  2.0f) * e, 0);
    float4 c = g_input.SampleLevel(g_linear, uv + float2( 2.0f,  2.0f) * e, 0);

    float4 d = g_input.SampleLevel(g_linear, uv + float2(-2.0f,  0.0f) * e, 0);
    float4 eCenter = g_input.SampleLevel(g_linear, uv + float2( 0.0f,  0.0f) * e, 0);
    float4 f = g_input.SampleLevel(g_linear, uv + float2( 2.0f,  0.0f) * e, 0);

    float4 g = g_input.SampleLevel(g_linear, uv + float2(-2.0f, -2.0f) * e, 0);
    float4 h = g_input.SampleLevel(g_linear, uv + float2( 0.0f, -2.0f) * e, 0);
    float4 i = g_input.SampleLevel(g_linear, uv + float2( 2.0f, -2.0f) * e, 0);

    float4 j = g_input.SampleLevel(g_linear, uv + float2(-1.0f,  1.0f) * e, 0);
    float4 k = g_input.SampleLevel(g_linear, uv + float2( 1.0f,  1.0f) * e, 0);
    float4 l = g_input.SampleLevel(g_linear, uv + float2(-1.0f, -1.0f) * e, 0);
    float4 m = g_input.SampleLevel(g_linear, uv + float2( 1.0f, -1.0f) * e, 0);

    float4 downsample = eCenter * 0.125f;
    downsample += (a + c + g + i) * 0.03125f;
    downsample += (b + d + f + h) * 0.0625f;
    downsample += (j + k + l + m) * 0.125f;

    float4 karisGroup0 = KarisAverage(j, k, l, m) * 0.5f; 
    float4 karisGroup1 = KarisAverage(a, b, d, eCenter) * 0.125f;
    float4 karisGroup2 = KarisAverage(b, c, eCenter, f) * 0.125f;
    float4 karisGroup3 = KarisAverage(d, eCenter, g, h) * 0.125f;
    float4 karisGroup4 = KarisAverage(eCenter, f, h, i) * 0.125f;
    
    if (g_firstDownsample)
    {
        downsample = karisGroup0 + karisGroup1 + karisGroup2 + karisGroup3 + karisGroup4;
    }

    return max(downsample, 0.0001f);
}

// ==== 3x3 tent upsample ====
float4 UpsampleTent(float2 uv)
{
    float2 texelSize = float2(1.0f / float(g_srcWidth), 1.0f / float(g_srcHeight));
    float r = g_filterRadius;

    float4 sum = float4(0, 0, 0, 0);
    // Weights: corners 1, edges 2, centre 4 (total 16).
    sum += g_input.SampleLevel(g_linear, uv + float2(-r, -r) * texelSize, 0) * 1.0f;
    sum += g_input.SampleLevel(g_linear, uv + float2( 0, -r) * texelSize, 0) * 2.0f;
    sum += g_input.SampleLevel(g_linear, uv + float2( r, -r) * texelSize, 0) * 1.0f;
    sum += g_input.SampleLevel(g_linear, uv + float2(-r,  0) * texelSize, 0) * 2.0f;
    sum += g_input.SampleLevel(g_linear, uv + float2( 0,  0) * texelSize, 0) * 4.0f;
    sum += g_input.SampleLevel(g_linear, uv + float2( r,  0) * texelSize, 0) * 2.0f;
    sum += g_input.SampleLevel(g_linear, uv + float2(-r,  r) * texelSize, 0) * 1.0f;
    sum += g_input.SampleLevel(g_linear, uv + float2( 0,  r) * texelSize, 0) * 2.0f;
    sum += g_input.SampleLevel(g_linear, uv + float2( r,  r) * texelSize, 0) * 1.0f;
    return sum / 16.0f;
}

// ==== Entry points ====

[numthreads(8, 8, 1)]
void CSDownsample(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_dstWidth || id.y >= g_dstHeight) return;

    float2 uv = (float2(id.xy) + 0.5f) / float2(g_dstWidth, g_dstHeight);
    g_output[id.xy] = DownsampleBox13(uv);
}

[numthreads(8, 8, 1)]
void CSUpsample(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_dstWidth || id.y >= g_dstHeight) return;

    float2 uv = (float2(id.xy) + 0.5f) / float2(g_dstWidth, g_dstHeight);
    float4 upsampled = UpsampleTent(uv);

    // Add onto this mip's own downsample content.
    g_output[id.xy] = g_output[id.xy] + upsampled;
}
