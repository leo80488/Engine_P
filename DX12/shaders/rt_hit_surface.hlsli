#ifndef RT_HIT_SURFACE_HLSLI
#define RT_HIT_SURFACE_HLSLI
// Hit-surface decode (RTHitSurface) for PathTrace, RTGI and RtxdiBridge; the reflection
// shader keeps its own shading path. Uses rt_hit_fetch.hlsli's bindings and triangle decoder.
// TODO(RenderPipeline_Architecture_Review_Plan SH4(f)): move RTR (HybridReflIntersectHW)
// onto this header so it shares the GI / PT normal decode.
#include "rt_hit_fetch.hlsli"
struct RTHitSurface
{
    float3 P, Ngeo, Nshade;
    float2 uv;
    float3 albedo, emissive;
    float rough, metal, ao, reflectance;
    uint flags;
    bool unlit, trusted;
};
float RTHitTextureLod(int textureIndex,float explicitMip,bool coneEnabled,float coneLod)
{
    if(!coneEnabled) return explicitMip;
    uint w,h,levels;
    g_AllTextures[NonUniformResourceIndex(textureIndex)].GetDimensions(0,w,h,levels);
    return clamp(coneLod+.5*log2(max(float(w)*float(h),1)),0,float(max(levels,1u)-1));
}
bool RTLoadHitSurfaceImpl(uint id, uint primitive, float2 bary,
                     float3x4 objectToWorld, float3x4 worldToObject,
                     float3 hitPosition, bool frontFace, float mip,
                     out RTHitSurface s, bool coneEnabled, float coneWidth, float3 coneDirection, float normalStrength)
{
    s=(RTHitSurface)0;
    RTInstanceData inst=g_Instances[id];
    float3 p0,p1,p2,n0,n1,n2; float2 uv0,uv1,uv2;
    if (!HR_LoadHitTriangleAttributes(id,primitive,p0,p1,p2,n0,n1,n2,uv0,uv1,uv2)) return false;
    float w=1-bary.x-bary.y;
    float3 ng=cross(p1-p0,p2-p0), ns=n0*w+n1*bary.x+n2*bary.y;
    if (dot(ng,ng)<1e-20) return false;
    if (!all(isfinite(ns)) || dot(ns,ns)<1e-12) ns=ng;
    // Normals use inverse transpose, including nonuniform instance scales.
    s.Ngeo=normalize(mul(ng,(float3x3)worldToObject));
    s.Nshade=normalize(mul(ns,(float3x3)worldToObject));
    float3 unflipped=s.Nshade;
    if (!frontFace) { s.Ngeo=-s.Ngeo; s.Nshade=-s.Nshade; }
    float2 rawUV=uv0*w+uv1*bary.x+uv2*bary.y;
    s.uv=RTInstanceUV(inst,rawUV);
    bool cached=(inst.rtFlags & RT_INST_FLAG_SURFACE_CACHE)!=0;
    // Isotropic cone footprint from the world / UV triangle areas (Falcor
    // TexLODHelpers::computeRayConeTriangleLODValue, RayCone::computeLOD). Atlas pages have
    // no independently padded mip chain, so they keep the explicit mip.
    coneEnabled=coneEnabled && !cached;
    float rawLod=0,scaledLod=0;
    if(coneEnabled)
    {
        float3 a=mul((float3x3)objectToWorld,p1-p0),b=mul((float3x3)objectToWorld,p2-p0);
        float worldArea=max(length(cross(a,b)),1e-20);
        float2 u=uv1-uv0,v=uv2-uv0;
        float uvArea=abs(u.x*v.y-u.y*v.x);
        rawLod=.5*log2(max(uvArea/worldArea,1e-20))
            +log2(max(coneWidth,1e-20)/max(abs(dot(coneDirection,s.Ngeo)),1e-4));
        scaledLod=rawLod+.5*log2(max(abs(inst.uvTilingOffset.x*inst.uvTilingOffset.y),1e-20));
    }
    if (inst.normalTexIdx>=0)
    {
        bool authored=false;
        float3 authoredT=0; float authoredSign=1;
        if((inst.rtFlags&RT_INST_FLAG_INTERLEAVED_TANGENT)!=0 && inst.vertexStride>=48
            && inst.vbBindlessIdx<16384 && inst.ibBindlessIdx<16384)
        {
            uint vb=NonUniformResourceIndex(inst.vbBindlessIdx),ib=NonUniformResourceIndex(inst.ibBindlessIdx);
            uint i0=HR_LoadIndex(ib,inst.indexStart+primitive*3,inst.indexBytes);
            uint i1=HR_LoadIndex(ib,inst.indexStart+primitive*3+1,inst.indexBytes);
            uint i2=HR_LoadIndex(ib,inst.indexStart+primitive*3+2,inst.indexBytes);
            float4 tangent=asfloat(g_Buffers[vb].Load4(i0*inst.vertexStride+32))*w
                +asfloat(g_Buffers[vb].Load4(i1*inst.vertexStride+32))*bary.x
                +asfloat(g_Buffers[vb].Load4(i2*inst.vertexStride+32))*bary.y;
            authoredT=mul((float3x3)objectToWorld,tangent.xyz);
            authoredT-=s.Nshade*dot(s.Nshade,authoredT);
            authored=all(isfinite(tangent)) && dot(authoredT,authoredT)>1e-12;
            authoredSign=tangent.w<0?-1:1;
        }
        float3 e1=p1-p0,e2=p2-p0;
        float2 d1=uv1-uv0,d2=uv2-uv0;
        float det=d1.x*d2.y-d2.x*d1.y;
        if (authored || abs(det)>1e-12)
        {
            float3 t=authored?authoredT:mul((float3x3)objectToWorld,(e1*d2.y-e2*d1.y)/det);
            t-=s.Nshade*dot(s.Nshade,t);
            if (dot(t,t)>1e-12)
            {
                t=normalize(t);
                float3 bw=mul((float3x3)objectToWorld,(e2*d1.x-e1*d2.x)/(abs(det)>1e-12?det:1));
                float signB=authored?authoredSign:(dot(cross(unflipped,t),bw)<0 ? -1.0 : 1.0);
                float3 b=cross(s.Nshade,t)*signB;
                float2 normalUV=cached ? rawUV : s.uv;
                float2 xy=g_AllTextures[NonUniformResourceIndex(inst.normalTexIdx)].SampleLevel(gSampWrap,normalUV,RTHitTextureLod(inst.normalTexIdx,mip,coneEnabled,cached?rawLod:scaledLod)).rg*2-1;
                float nz=sqrt(saturate(1-dot(xy,xy)));
                xy *= cached ? 1 : normalStrength;
                s.Nshade=normalize(t*xy.x+b*xy.y+s.Nshade*nz);
            }
        }
    }
    s.P=hitPosition; s.flags=inst.rtFlags;
    s.albedo=inst.baseColor.rgb;
    float2 albedoUV=(inst.rtFlags & RT_INST_FLAG_ALBEDO_RAW) ? rawUV : s.uv;
    if (inst.albedoTexIdx>=0)
        s.albedo*=g_AllTextures[NonUniformResourceIndex(inst.albedoTexIdx)].SampleLevel(gSampWrap,albedoUV,RTHitTextureLod(inst.albedoTexIdx,mip,coneEnabled,(inst.rtFlags&RT_INST_FLAG_ALBEDO_RAW)?rawLod:scaledLod)).rgb;
    float4 sm=1;
    if (inst.surfaceTexIdx>=0)
        sm=g_AllTextures[NonUniformResourceIndex(inst.surfaceTexIdx)].SampleLevel(gSampWrap,s.uv,RTHitTextureLod(inst.surfaceTexIdx,mip,coneEnabled,scaledLod));
    s.emissive=inst.emissive.rgb;
    if (cached) s.emissive*=inst.surfaceTexIdx>=0 ? sm.a : 0;
    else if (inst.emissiveTexIdx>=0)
        s.emissive*=g_AllTextures[NonUniformResourceIndex(inst.emissiveTexIdx)].SampleLevel(gSampWrap,s.uv,RTHitTextureLod(inst.emissiveTexIdx,mip,coneEnabled,scaledLod)).rgb;
    s.rough=max(lerp(inst.roughnessMin,inst.roughnessMax,sm.g),RT_ROUGHNESS_FLOOR);
    s.metal=lerp(inst.metalnessMin,inst.metalnessMax,sm.b);
    s.ao=sm.r; s.reflectance=inst.reflectance;
    s.unlit=inst.emissive.a==1;
    s.trusted=(inst.rtFlags & RT_INST_FLAG_PROXY_UNTRUSTED)==0;
    return all(isfinite(s.Nshade));
}
bool RTLoadHitSurface(uint id,uint primitive,float2 bary,float3x4 objectToWorld,float3x4 worldToObject,
    float3 hitPosition,bool frontFace,float mip,out RTHitSurface s,float normalStrength=1)
{
    return RTLoadHitSurfaceImpl(id,primitive,bary,objectToWorld,worldToObject,hitPosition,frontFace,mip,s,false,0,0,normalStrength);
}
bool RTLoadHitSurfaceCone(uint id,uint primitive,float2 bary,float3x4 objectToWorld,float3x4 worldToObject,
    float3 hitPosition,bool frontFace,float coneWidth,float3 direction,out RTHitSurface s,float normalStrength=1)
{
    return RTLoadHitSurfaceImpl(id,primitive,bary,objectToWorld,worldToObject,hitPosition,frontFace,0,s,true,coneWidth,direction,normalStrength);
}
#endif
