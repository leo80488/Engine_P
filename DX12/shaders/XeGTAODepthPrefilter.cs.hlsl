// VERBATIM port of XeGTAO_PrefilterDepths16x16 from Intel XeGTAO.hlsli (SPDX MIT).
// Writes all 5 mips of the linear depth pyramid in ONE dispatch: 8x8 threads (as the
// reference), each loading a 2x2 quad of hardware depth into mip 0, then groupshared
// reductions through g_scratchDepths produce mips 1..4.
// Bindings:
//   b0 space2 - PrefilterCB (sizes, DepthUnpackConsts, falloff knobs)
//   t0 space2 - hardware depth (source)
//   u0..u4    - mips 0..4 of the linear depth pyramid (R32F)

cbuffer PrefilterCB : register(b0, space2)
{
    uint2  SrcSize;              // hardware depth dims (gather UV)
    uint2  DstSize;              // unused
    float2 DepthUnpackConsts;    // XeGTAO DepthUnpackConsts (mul, add)
    float  EffectRadius;
    float  EffectFalloffRange;
    float  RadiusMultiplier;
    uint   SrcMip;               // unused in this variant
};

Texture2D<float>    gSrcDepth : register(t0, space2);

RWTexture2D<float>  gOutMip0 : register(u0, space2);
RWTexture2D<float>  gOutMip1 : register(u1, space2);
RWTexture2D<float>  gOutMip2 : register(u2, space2);
RWTexture2D<float>  gOutMip3 : register(u3, space2);
RWTexture2D<float>  gOutMip4 : register(u4, space2);

// Despite the name the sampler is LINEAR; GatherRed ignores the filter mode.
SamplerState        gPointClamp : register(s0, space2);

// ==== Helpers, verbatim from XeGTAO.hlsli ====
float XeGTAO_ScreenSpaceToViewSpaceDepth(float screenDepth)
{
    return DepthUnpackConsts.x / (DepthUnpackConsts.y - screenDepth);
}

float XeGTAO_ClampDepth(float d)
{
    // FP32 path (the reference also supports FP16 via min16float).
    return clamp(d, 0.0, 3.402823466e+38);
}

// Weighted average of 4 depths biased to the farthest (reference XeGTAO_DepthMIPFilter):
// a sample within falloffFrom of maxDepth gets full weight, fading to 0 at effectRadius
// (0.75 x the AO radius) nearer.
float XeGTAO_DepthMIPFilter(float depth0, float depth1, float depth2, float depth3)
{
    float maxDepth = max(max(depth0, depth1), max(depth2, depth3));

    const float depthRangeScaleFactor = 0.75;
    const float effectRadius = depthRangeScaleFactor * EffectRadius * RadiusMultiplier;
    const float falloffRange = EffectFalloffRange * effectRadius;
    const float falloffFrom  = effectRadius * (1.0 - EffectFalloffRange);
    const float falloffMul   = -1.0 / falloffRange;
    const float falloffAdd   = falloffFrom / falloffRange + 1.0;

    float w0 = saturate((maxDepth - depth0) * falloffMul + falloffAdd);
    float w1 = saturate((maxDepth - depth1) * falloffMul + falloffAdd);
    float w2 = saturate((maxDepth - depth2) * falloffMul + falloffAdd);
    float w3 = saturate((maxDepth - depth3) * falloffMul + falloffAdd);

    float wSum = w0 + w1 + w2 + w3;
    return (wSum > 1e-6)
        ? (w0*depth0 + w1*depth1 + w2*depth2 + w3*depth3) / wSum
        : maxDepth;
}

// ==== Main entry: XeGTAO_PrefilterDepths16x16, verbatim ====
// 8x8 threads; the groupshared depth cache is reused for mips 1..4.
groupshared float g_scratchDepths[8][8];

[numthreads(8, 8, 1)]
void CSPrefilter(uint3 dispatchThreadID : SV_DispatchThreadID,
                 uint3 groupThreadID    : SV_GroupThreadID)
{
    // Each thread writes 4 mip-0 texels: the 2x2 quad at baseCoord*2.
    const uint2 baseCoord = dispatchThreadID.xy;
    const uint2 pixCoord  = baseCoord * 2;

    // GatherRed at the quad's centre returns .w = (0,0), .z = (1,0), .x = (0,1), .y = (1,1);
    // the remapping below is the reference's.
    const float2 pixelSize = 1.0 / float2(SrcSize);
    float2 uv = (float2(pixCoord) + 1.0) * pixelSize;
    float4 depths4 = gSrcDepth.GatherRed(gPointClamp, uv);

    float depth0 = XeGTAO_ClampDepth(XeGTAO_ScreenSpaceToViewSpaceDepth(depths4.w));
    float depth1 = XeGTAO_ClampDepth(XeGTAO_ScreenSpaceToViewSpaceDepth(depths4.z));
    float depth2 = XeGTAO_ClampDepth(XeGTAO_ScreenSpaceToViewSpaceDepth(depths4.x));
    float depth3 = XeGTAO_ClampDepth(XeGTAO_ScreenSpaceToViewSpaceDepth(depths4.y));

    gOutMip0[pixCoord + uint2(0, 0)] = depth0;
    gOutMip0[pixCoord + uint2(1, 0)] = depth1;
    gOutMip0[pixCoord + uint2(0, 1)] = depth2;
    gOutMip0[pixCoord + uint2(1, 1)] = depth3;

    // Mip 1: one sample per thread.
    float dm1 = XeGTAO_DepthMIPFilter(depth0, depth1, depth2, depth3);
    gOutMip1[baseCoord] = dm1;
    g_scratchDepths[groupThreadID.x][groupThreadID.y] = dm1;

    GroupMemoryBarrierWithGroupSync();

    // Mip 2: one thread in every 2x2 block.
    [branch]
    if (all((groupThreadID.xy % 2) == 0))
    {
        float inTL = g_scratchDepths[groupThreadID.x + 0][groupThreadID.y + 0];
        float inTR = g_scratchDepths[groupThreadID.x + 1][groupThreadID.y + 0];
        float inBL = g_scratchDepths[groupThreadID.x + 0][groupThreadID.y + 1];
        float inBR = g_scratchDepths[groupThreadID.x + 1][groupThreadID.y + 1];

        float dm2 = XeGTAO_DepthMIPFilter(inTL, inTR, inBL, inBR);
        gOutMip2[baseCoord / 2] = dm2;
        g_scratchDepths[groupThreadID.x][groupThreadID.y] = dm2;
    }

    GroupMemoryBarrierWithGroupSync();

    // Mip 3: one thread in every 4x4 block.
    [branch]
    if (all((groupThreadID.xy % 4) == 0))
    {
        float inTL = g_scratchDepths[groupThreadID.x + 0][groupThreadID.y + 0];
        float inTR = g_scratchDepths[groupThreadID.x + 2][groupThreadID.y + 0];
        float inBL = g_scratchDepths[groupThreadID.x + 0][groupThreadID.y + 2];
        float inBR = g_scratchDepths[groupThreadID.x + 2][groupThreadID.y + 2];

        float dm3 = XeGTAO_DepthMIPFilter(inTL, inTR, inBL, inBR);
        gOutMip3[baseCoord / 4] = dm3;
        g_scratchDepths[groupThreadID.x][groupThreadID.y] = dm3;
    }

    GroupMemoryBarrierWithGroupSync();

    // Mip 4: one thread per group (8x8 block).
    [branch]
    if (all((groupThreadID.xy % 8) == 0))
    {
        float inTL = g_scratchDepths[groupThreadID.x + 0][groupThreadID.y + 0];
        float inTR = g_scratchDepths[groupThreadID.x + 4][groupThreadID.y + 0];
        float inBL = g_scratchDepths[groupThreadID.x + 0][groupThreadID.y + 4];
        float inBR = g_scratchDepths[groupThreadID.x + 4][groupThreadID.y + 4];

        float dm4 = XeGTAO_DepthMIPFilter(inTL, inTR, inBL, inBR);
        gOutMip4[baseCoord / 8] = dm4;
    }
}
