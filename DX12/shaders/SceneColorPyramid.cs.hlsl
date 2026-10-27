// SceneColorPyramid.cs.hlsl - pre-filtered HDR mip pyramid for screen-space radiance fetches.
// Built each frame from the HDR snapshot the resolve pass owns. SSRTrace samples it at a mip from
// roughness x ray length, so glossy reflections read pre-integrated radiance instead of one mip-0
// texel: the core variance reduction for rough SSR. The next frame's unified reflection trace, SSGI
// and forward reflections read it as their previous-frame HDR. The reduce uses Karis firefly
// weighting (w = 1/(1 + lum)) so blown-out texels do not survive the 2x2 average as sparkles.

cbuffer HierCB : register(b0, space2)
{
    uint srcWidth;
    uint srcHeight;
    uint dstWidth;
    uint dstHeight;
};

Texture2D<float4>    g_Src : register(t0, space2);   // HDR snapshot (unused: mip 0 is a copy, see CSMain_Mip0)
RWTexture2D<float4>  g_Mip : register(u0, space2);   // reduce source: the previous mip
RWTexture2D<float4>  g_Dst : register(u1, space2);   // reduce dst

#include "core/color.hlsli"   // Luminance

// CSMain_Mip0 stays as a no-op so ShaderID::SceneColorPyramidMip0_CS still resolves at Init.
// Mip 0 is filled by CopyTextureSubresource in SceneColorPyramidPass::Execute: CS writes to this
// pyramid were silently dropped (see that method).
[numthreads(8, 8, 1)]
void CSMain_Mip0(uint3 dtid : SV_DispatchThreadID)
{
    // Intentionally empty.
}

[numthreads(8, 8, 1)]
void CSMain_Reduce(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= dstWidth || dtid.y >= dstHeight) return;

    uint2 s0 = uint2(min(dtid.x * 2u,      srcWidth - 1u), min(dtid.y * 2u,      srcHeight - 1u));
    uint2 s1 = uint2(min(dtid.x * 2u + 1u, srcWidth - 1u), min(dtid.y * 2u,      srcHeight - 1u));
    uint2 s2 = uint2(min(dtid.x * 2u,      srcWidth - 1u), min(dtid.y * 2u + 1u, srcHeight - 1u));
    uint2 s3 = uint2(min(dtid.x * 2u + 1u, srcWidth - 1u), min(dtid.y * 2u + 1u, srcHeight - 1u));

    float4 c0 = g_Mip[s0];
    float4 c1 = g_Mip[s1];
    float4 c2 = g_Mip[s2];
    float4 c3 = g_Mip[s3];

    float w0 = 1.0 / (1.0 + Luminance(c0.rgb));
    float w1 = 1.0 / (1.0 + Luminance(c1.rgb));
    float w2 = 1.0 / (1.0 + Luminance(c2.rgb));
    float w3 = 1.0 / (1.0 + Luminance(c3.rgb));
    float wSum = w0 + w1 + w2 + w3;

    g_Dst[dtid.xy] = (c0 * w0 + c1 * w1 + c2 * w2 + c3 * w3) / wSum;
}
