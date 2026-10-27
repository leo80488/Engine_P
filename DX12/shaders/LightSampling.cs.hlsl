// Light sampling tables (LightSamplingPass): the local-light PDF texture (light i
// at its Z-curve texel, value = power; each coarser mip texel = the sum of its 2x2
// children) and the RIS buffer of power-proportional draws made by descending that
// mip chain.
#include "cluster_common.hlsli"
cbuffer Params : register(b0)
{
    uint lightsIdx, lightCount, frame, pdfSize;
    uint mipCount, tileSize, tileCount, risUav;
    uint histogramUav, stage, entries, tableFlags; // tableFlags bit 0: -light_tables_corrupt
    uint4 pdfUav[2];   // mips 0..7
};
uint PdfUav(uint mip) { return pdfUav[mip>>2][mip&3]; }

// Z-curve (Morton) order: light i <-> texel (its even bits, its odd bits).
uint Part1By1(uint x)
{
    x&=0x0000ffffu;
    x=(x^(x<<8))&0x00ff00ffu; x=(x^(x<<4))&0x0f0f0f0fu;
    x=(x^(x<<2))&0x33333333u; x=(x^(x<<1))&0x55555555u;
    return x;
}
uint TexelToLight(uint2 t) { return Part1By1(t.x)|(Part1By1(t.y)<<1); }
uint Compact1By1(uint x)
{
    x&=0x55555555u;
    x=(x^(x>>1))&0x33333333u; x=(x^(x>>2))&0x0f0f0f0fu;
    x=(x^(x>>4))&0x00ff00ffu; x=(x^(x>>8))&0x0000ffffu;
    return x;
}
uint2 LightToTexel(uint i) { return uint2(Compact1By1(i),Compact1By1(i>>1)); }

// The power the proposal follows: local lights that can light anything.
float LightPower(GPULight l)
{
    if(l.type==0u || !(l.intensity>0) || !(l.radius>0)) return 0;
    const float p=asfloat(l.powerBits);
    return isfinite(p) && p>0 ? p : 0;
}

[numthreads(8,8,1)]
void BuildPdf(uint3 id:SV_DispatchThreadID)
{
    if(any(id.xy>=pdfSize)) return;
    const uint i=TexelToLight(id.xy);
    float p=0;
    if(i<lightCount)
    {
        StructuredBuffer<GPULight> lights=ResourceDescriptorHeap[lightsIdx];
        p=LightPower(lights[i]);
    }
    RWTexture2D<float> mip0=ResourceDescriptorHeap[PdfUav(0)];
    mip0[id.xy]=p;
}

[numthreads(8,8,1)]
void BuildMip(uint3 id:SV_DispatchThreadID)
{
    const uint size=max(pdfSize>>stage,1u);
    if(any(id.xy>=size)) return;
    RWTexture2D<float> source=ResourceDescriptorHeap[PdfUav(stage-1)];
    RWTexture2D<float> target=ResourceDescriptorHeap[PdfUav(stage)];
    const uint2 c=id.xy*2;
    target[id.xy]=source[c]+source[c+uint2(1,0)]+source[c+uint2(0,1)]+source[c+uint2(1,1)];
}

#include "core/hash.hlsli"
float Random(inout uint s)
{
    s=s*747796405u+2891336453u;
    uint w=((s>>((s>>28u)+4u))^s)*277803737u;
    w=(w>>22u)^w;
    return float(w>>8)*(1.0/16777216.0);
}

// One draw per entry: from the 1x1 top, step into a child in proportion to
// its weight, down to the light's mip-0 texel; pdf = its power / the total.
[numthreads(64,1,1)]
void Presample(uint3 id:SV_DispatchThreadID)
{
    const uint entry=id.x;
    if(entry>=entries) return;
    RWStructuredBuffer<uint2> ris=ResourceDescriptorHeap[risUav];
    RWTexture2D<float> top=ResourceDescriptorHeap[PdfUav(mipCount-1)];
    const float total=top[uint2(0,0)];
    if(!(total>0)) { ris[entry]=uint2(0xffffffffu,0); return; }
    uint s=HashLowbias32(entry^HashLowbias32(frame*0x9e3779b9u+0x632be5abu));
    uint2 texel=0;
    for(int mip=int(mipCount)-2;mip>=0;--mip)
    {
        RWTexture2D<float> level=ResourceDescriptorHeap[PdfUav(uint(mip))];
        const uint2 c=texel*2;
        const uint2 offsets[4]={uint2(0,0),uint2(1,0),uint2(0,1),uint2(1,1)};
        float w[4];
        [unroll] for(uint a=0;a<4;++a) w[a]=level[c+offsets[a]];
        // Negative control (-light_tables_corrupt): the leaf level ignores the
        // lights' powers, so the draws stop following the PDF and the
        // self-check must fail.
        if((tableFlags&1u) && mip==0) [unroll] for(uint b=0;b<4;++b) w[b]=w[b]>0?1:0;
        float u=Random(s)*(w[0]+w[1]+w[2]+w[3]);
        // The child holding u; a zero-weight child is never entered, and the
        // rounding at the end of the range falls to the last non-zero one.
        uint chosen=4u;
        [unroll] for(uint k=0;k<4;++k) { if(chosen==4u && w[k]>0 && u<w[k]) chosen=k; u-=w[k]; }
        if(chosen==4u) [unroll] for(uint d=0;d<4;++d) if(w[d]>0) chosen=d;
        texel=c+offsets[min(chosen,3u)];
    }
    RWTexture2D<float> mip0=ResourceDescriptorHeap[PdfUav(0)];
    const float p=mip0[texel];
    const uint light=TexelToLight(texel);
    ris[entry]=p>0 && light<lightCount ? uint2(light,asuint(total/p)) : uint2(0xffffffffu,0);
}

// -light_tables_log self-check: counts[i] of this frame's draws, then pdf[i].
[numthreads(64,1,1)]
void HistogramClear(uint3 id:SV_DispatchThreadID)
{
    const uint i=id.x;
    if(i>=pdfSize*pdfSize) return;
    RWStructuredBuffer<uint> histogram=ResourceDescriptorHeap[histogramUav];
    RWTexture2D<float> mip0=ResourceDescriptorHeap[PdfUav(0)];
    RWTexture2D<float> top=ResourceDescriptorHeap[PdfUav(mipCount-1)];
    const float total=top[uint2(0,0)];
    histogram[i]=0;
    const float p=i<lightCount && total>0 ? mip0[LightToTexel(i)]/total : 0;
    histogram[pdfSize*pdfSize+i]=asuint(p);
}
[numthreads(64,1,1)]
void HistogramCount(uint3 id:SV_DispatchThreadID)
{
    if(id.x>=entries) return;
    RWStructuredBuffer<uint2> ris=ResourceDescriptorHeap[risUav];
    RWStructuredBuffer<uint> histogram=ResourceDescriptorHeap[histogramUav];
    const uint light=ris[id.x].x;
    if(light<pdfSize*pdfSize) InterlockedAdd(histogram[light],1u);
}
