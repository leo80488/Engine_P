// Opt-in readback probe using exactly the production PT/GI hit decoder.
// Reuse PT bindings; its rendering entry is not called.
#define main PathTraceUnusedMain
#include "PathTrace.cs.hlsl"
#undef main
void Store3(uint base,float3 value)
{ Counters[base]=asuint(value.x); Counters[base+1]=asuint(value.y); Counters[base+2]=asuint(value.z); }
[numthreads(16,1,1)]
void main(uint3 tid:SV_DispatchThreadID)
{
    uint i=tid.x,base=7+2*7*128+i*32;
    for(uint j=0;j<32;++j) Counters[base+j]=0;
    uint2 px=uint2((float2(i%4,i/4)-1.5)*float2(width,height)*.035+float2(width,height)*.5);
    Texture2D<float> depths=ResourceDescriptorHeap[depthIdx];
    Texture2D<float4> normals=ResourceDescriptorHeap[normalIdx];
    Texture2D<float4> albedos=ResourceDescriptorHeap[albedoIdx];
    Texture2D<float4> surfaces=ResourceDescriptorHeap[surfaceIdx];
    float2 uv=(float2(px)+.5)/float2(width,height);
    const float3 P=WorldPosFromDepth(uv,depths.Load(int3(px,0)),invViewProj);
    Store3(base+18,P); Store3(base+21,normalize(normals.Load(int3(px,0)).xyz*2-1));
    Store3(base+24,albedos.Load(int3(px,0)).rgb);
    float4 surface=surfaces.Load(int3(px,0));
    Counters[base+27]=asuint(surface.r); Counters[base+28]=asuint(surface.g);
    RayDesc ray; ray.Origin=camera; ray.Direction=normalize(P-camera); ray.TMin=.001; ray.TMax=1000;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(g_TLAS,RAY_FLAG_NONE,0x01,ray);
    while(q.Proceed())
        if(q.CandidateType()==CANDIDATE_NON_OPAQUE_TRIANGLE && HR_AlphaAccept(
            q.CandidateInstanceID(),q.CandidatePrimitiveIndex(),q.CandidateTriangleBarycentrics(),false))
            q.CommitNonOpaqueTriangleHit();
    Counters[base]=1;
    if(q.CommittedStatus()!=COMMITTED_TRIANGLE_HIT) return;
    uint id=q.CommittedInstanceID(),primitive=q.CommittedPrimitiveIndex();
    float2 bary=q.CommittedTriangleBarycentrics();
    Counters[base+1]=id; Counters[base+2]=primitive;
    Counters[base+3]=asuint(bary.x); Counters[base+4]=asuint(bary.y);
    if(flags&256u) id=(id+1)%max(alphaInstanceCount,1u);
    RTHitSurface s;
    if(!RTLoadHitSurface(id,primitive,bary,q.CommittedObjectToWorld3x4(),q.CommittedWorldToObject3x4(),
        camera+ray.Direction*q.CommittedRayT(),true,0,s)) { Counters[base]=3; return; }
    Counters[base]=2|(g_Instances[id].indexBytes<<8);
    Store3(base+5,s.P); Store3(base+8,s.Nshade);
    Counters[base+11]=asuint(s.uv.x); Counters[base+12]=asuint(s.uv.y);
    Store3(base+13,s.albedo); Counters[base+16]=asuint(s.rough); Counters[base+17]=asuint(s.metal);
    float3 p0,p1,p2,n0,n1,n2; float2 uv0,uv1,uv2;
    if(HR_LoadHitTriangleAttributes(id,primitive,p0,p1,p2,n0,n1,n2,uv0,uv1,uv2))
        Store3(base+29,mul(q.CommittedObjectToWorld3x4(),float4(p0*(1-bary.x-bary.y)+p1*bary.x+p2*bary.y,1)));
}
