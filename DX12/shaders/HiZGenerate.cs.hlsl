// HiZGenerate.cs.hlsl - writes mip 0 of the Hierarchical-Z buffer from the depth buffer;
// HiZReduce.cs.hlsl builds every mip above it.
//
// Root signature (reuses compute space2):
//   [0] CBV  b0 space2  - HiZCB (source and destination dimensions)
//   [1] SRV  t0 space2  - source depth (Texture2D<float>)
//   [4] UAV  u0 space2  - destination mip (RWTexture2D<float>)

cbuffer HiZCB : register(b0, space2)
{
    uint srcWidth;
    uint srcHeight;
    uint dstWidth;
    uint dstHeight;
};

Texture2D<float>   g_SrcMip : register(t0, space2);
RWTexture2D<float> g_DstMip : register(u0, space2);

SamplerState g_PointClamp : register(s0, space2);

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= dstWidth || dtid.y >= dstHeight) return;

    // Mip 0 is a 1:1 COPY of the depth buffer into the top-left srcWidth x srcHeight region
    // of the Hi-Z texture. Do not downsample here: it halves the valid extent, and the cull
    // shader's uv -> texel mapping then reads occluder depth from the wrong screen location
    // (position-dependent false occlusion culls). Consumers map uv by the VALID extent
    // (CullingCB.hiZWidth/Height = render dims), never the texture size.
    //
    // The texture is the render dims rounded up to a multiple of 32 and grow-only, so a pad
    // band can lie beyond the valid region. It is filled with 0 = far plane under reversed-Z,
    // which keeps the min-reduce chain conservative (anything sampling the pad is judged
    // visible).
    float d = 0.0;
    if (dtid.x < srcWidth && dtid.y < srcHeight)
        d = g_SrcMip.Load(int3(dtid.xy, 0));

    g_DstMip[dtid.xy] = d;
}
