// Samples the NV12 Y (luma) + UV (chroma) planes of a decoded video frame and emits
// linear RGB for the HDR scene buffer. The PSO alpha-blends, so the video is either a
// fullscreen replacement (alpha = 1) or an overlay (alpha from the CB).
// NV12: Y plane R8_UNORM at frame size; UV plane R8G8_UNORM at half width x half
// height (interleaved U, V). Limited-range BT.709 (HD, default) or BT.601 (SD).
//
// Caller binding:
//   t6 space0 - Y plane SRV  (R8_UNORM,   PlaneSlice = 0)
//   t7 space0 - UV plane SRV (R8G8_UNORM, PlaneSlice = 1)
//   b2 space0 - VideoCompositeCB: rectUV.xy = uvMin, rectUV.zw = uvMax (crop / letterbox);
//               colorSpace 0 = BT.709, 1 = BT.601
//   LinearClamp comes from the sampler heap.

Texture2D<float>   VideoY    : register(t6, space0);
Texture2D<float2>  VideoUV   : register(t7, space0);
#include "sampler_heap.hlsli"   // samplers from the sampler heap
#define LinearClamp SmpFixed(SMP_LINEAR_CLAMP)

cbuffer VideoCompositeCB : register(b2, space0)
{
    float4 rectUV;        // xy = min, zw = max
    float  alpha;
    uint   colorSpace;    // 0 = BT.709, 1 = BT.601
    float2 _pad;
};

struct PSIn
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float3 YuvLimitedToRgb(float3 yuv, uint cs)
{
    // Limited-range YUV (Y: 16..235, UV: 16..240 mapped to [0,1] by NV12 storage)
    // brought into "code value range" via Y -= 16/255, UV -= 128/255, then scaled.
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
    else         // BT.709 (default, HD content)
    {
        rgb.r = y + 1.5748 * cr;
        rgb.g = y - 0.1873 * cb - 0.4681 * cr;
        rgb.b = y + 1.8556 * cb;
    }
    return saturate(rgb);
}

float4 main(PSIn i) : SV_Target
{
    // Map screen-space UV to the target sub-rect (default 0..1).
    float2 vuv = lerp(rectUV.xy, rectUV.zw, i.uv);

    float  y  = VideoY .SampleLevel(LinearClamp, vuv, 0).r;
    float2 uv = VideoUV.SampleLevel(LinearClamp, vuv, 0).rg;

    float3 rgb = YuvLimitedToRgb(float3(y, uv.x, uv.y), colorSpace);

    // Transfer decode: BT.709 camera / encoder output is ~sRGB, so a plain 2.2 power stands
    // in for the piecewise BT.1886 curve; switch to the curve if HDR output shows banding.
    rgb = pow(rgb, 2.2);

    return float4(rgb, alpha);
}
