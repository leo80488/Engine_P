// Splits the image into an 8x8 grid of tiles, one 8x8 thread group per tile:
// tileMeans[0..63] = tile means, [64..127] = the mean across-frames variance of the
// tile's 64 probe pixels (0 when varianceEnabled is off). Each texel participates
// exactly once, including odd image sizes. No exposure or tonemapping.
#include "core/color.hlsli"   // Luminance

cbuffer Constants : register(b0, space2)
{
    uint sourceIndex;
    uint width;
    uint height;
    uint varianceUav;
    uint samples; uint varianceEnabled; float alternation; uint measureAlpha;
};
RWStructuredBuffer<float> tileMeans : register(u0, space2);
groupshared float sums[64];
groupshared float variances[64];

float Measure(float4 value)
{
    return measureAlpha != 0 ? value.a : Luminance(value.rgb);
}

[numthreads(8, 8, 1)]
void main(uint3 group : SV_GroupID, uint3 tid : SV_GroupThreadID, uint lane : SV_GroupIndex)
{
    Texture2D<float4> source = ResourceDescriptorHeap[sourceIndex];
    uint2 lo = group.xy * uint2(width, height) / 8;
    uint2 hi = (group.xy + 1) * uint2(width, height) / 8;
    float value = 0;
    for (uint y = lo.y + tid.y; y < hi.y; y += 8)
        for (uint x = lo.x + tid.x; x < hi.x; x += 8)
            value += Measure(source.Load(int3(x, y, 0)));
    float variance=0;
    if(varianceEnabled)
    {
        // Fixed stratified 64x64 pixel probes, NOT variance of tile means.
        uint2 pixel=min(lo+(2*tid.xy+1)*(hi-lo)/16,uint2(width,height)-1);
        float luminance=Measure(source.Load(int3(pixel,0)));
        luminance*=1+((samples&1)?alternation:-alternation);
        RWStructuredBuffer<float2> moments=ResourceDescriptorHeap[varianceUav];
        uint index=(group.y*8+group.x)*64+lane;
        float2 state=samples==1?float2(0,0):moments[index];
        float delta=luminance-state.x;
        state.x+=delta/samples;
        state.y+=delta*(luminance-state.x);
        moments[index]=state;
        variance=max(state.y/samples,0);
    }
    variances[lane]=variance;
    sums[lane] = value;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 32; stride > 0; stride >>= 1)
    {
        if (lane < stride) { sums[lane] += sums[lane + stride]; variances[lane]+=variances[lane+stride]; }
        GroupMemoryBarrierWithGroupSync();
    }
    if (lane == 0)
    {
        tileMeans[group.y * 8 + group.x] = sums[0] / max(1u, (hi.x - lo.x) * (hi.y - lo.y));
        tileMeans[64+group.y*8+group.x]=variances[0]/64;
    }
}
