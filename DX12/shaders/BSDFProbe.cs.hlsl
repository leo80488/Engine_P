// Harness-only GPU distribution test. Shares the production sampler and PDF.
#include "bsdf_sample.hlsli"
RWStructuredBuffer<uint> Histogram : register(u2);
static const uint kBins=128;
void ProbeCase(uint c, out float3 v, out float rough)
{
    float z=c<=3 ? 1.0 : 0.15;
    v=float3(sqrt(1-z*z),0,z);
    uint r=(max(c,1u)-1)%3;
    rough=r==0 ? 0.3 : (r==1 ? 0.65 : 1.0);
}
[numthreads(128,1,1)]
void Clear(uint3 tid:SV_DispatchThreadID)
{
    uint c=tid.y, bin=tid.x;
    Histogram[7+c*kBins+bin]=0;
    float z=(bin/16+0.5)/8.0, phi=(bin%16+0.5)*2*PI/16.0;
    float3 h=float3(sqrt(1-z*z)*cos(phi),sqrt(1-z*z)*sin(phi),z);
    float3 v; float rough; ProbeCase(c,v,rough);
    float pdf=c==0 ? h.z/PI : RTGGXVisibleNormalPdf(v,h,rough);
    Histogram[7+7*kBins+c*kBins+bin]=asuint(pdf);
}
[numthreads(256,1,1)]
void main(uint3 tid:SV_DispatchThreadID)
{
    uint c=tid.y;
    uint rng=(tid.x+1)*747796405u ^ (c+19)*2891336453u;
    if (rng==0) rng=1;
    float2 u=float2(RTRandom(rng),RTRandom(rng));
    float3 v; float rough; ProbeCase(c,v,rough);
    float3 h=c==0 ? RTSampleCosine(u) : SampleGGXVNDF(v,rough*rough,u.x,u.y);
    float phi=atan2(h.y,h.x); if (phi<0) phi+=2*PI;
    uint bin=min(uint(h.z*8),7u)*16+min(uint(phi*16/(2*PI)),15u);
    InterlockedAdd(Histogram[7+c*kBins+bin],1);
}
