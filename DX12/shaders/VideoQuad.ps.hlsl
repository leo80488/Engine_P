// Samples the NV12 Y / UV planes for a world-space video quad. Same YUV -> RGB math
// as VideoComposite.ps.hlsl without the sub-rect (the quad supplies 0..1 UVs). Writes
// the HDR scene buffer with alpha blending, so the scene shows through when alpha < 1.

#include "fog_apply.hlsli"   // FogApplyCB b4 + s3, bound by VideoQuadPass

Texture2D<float>  VideoY      : register(t6, space0);
Texture2D<float2> VideoUV     : register(t7, space0);
#include "sampler_heap.hlsli"   // samplers from the sampler heap
#define LinearClamp SmpFixed(SMP_LINEAR_CLAMP)

cbuffer VideoQuadCB : register(b2, space0)
{
    row_major float4x4 worldMatrix;
    row_major float4x4 viewProjMatrix;
    float    quadWidth;
    float    quadHeight;
    float    alpha;
    uint     colorSpace;
};

struct PSIn
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float3 YuvLimitedToRgb(float3 yuv, uint cs)
{
    float y  = (yuv.x - 16.0 / 255.0) * (255.0 / 219.0);
    float cb = (yuv.y - 128.0 / 255.0) * (255.0 / 224.0);
    float cr = (yuv.z - 128.0 / 255.0) * (255.0 / 224.0);

    float3 rgb;
    if (cs == 1) // BT.601
    {
        rgb.r = y + 1.402    * cr;
        rgb.g = y - 0.344136 * cb - 0.714136 * cr;
        rgb.b = y + 1.772    * cb;
    }
    else         // BT.709
    {
        rgb.r = y + 1.5748 * cr;
        rgb.g = y - 0.1873 * cb - 0.4681 * cr;
        rgb.b = y + 1.8556 * cb;
    }
    return saturate(rgb);
}

float4 main(PSIn i) : SV_Target
{
    float  y  = VideoY .SampleLevel(LinearClamp, i.uv, 0).r;
    float2 uv = VideoUV.SampleLevel(LinearClamp, i.uv, 0).rg;

    float3 rgb = YuvLimitedToRgb(float3(y, uv.x, uv.y), colorSpace);
    rgb = pow(rgb, 2.2);   // sRGB -> linear

    // A world-space screen in fog gets the fog the opaque scene got at its depth;
    // blend is SRC_ALPHA / INV_SRC_ALPHA.
    return Fog_ApplyAlpha(float4(rgb, alpha), Fog_Evaluate(i.pos));
}
