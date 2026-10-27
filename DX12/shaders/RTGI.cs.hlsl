#include "bsdf_sample.hlsli"
#include "material.hlsli"
#include "core/depth.hlsli"   // LinearizeReverseZ, WorldPosFromDepth
#include "core/gbuffer_codec.hlsli"   // GBufferDecodeMatIdx
#include "shading_model_table.hlsli"   // which models are GI receivers
#include "rt_instance_data.hlsli"
#include "rt_alpha_record.hlsli"
#include "light_eval.hlsli"
#include "world_light_grid.hlsli"
#include "restir_gi_reservoir.hlsli"
#include "restir_reprojection.hlsli"
#include "DDGISampling.hlsli"
#define NRD_NORMAL_ENCODING 2
#define NRD_ROUGHNESS_ENCODING 1
#include "NRD.hlsli"
cbuffer Params : register(b0)
{
    float4x4 invViewProj,worldToView;
    float3 camera; float rayTMax;
    float3 sunDirection; uint frame;
    float3 sunColor; uint flags;
    uint width,height,traceW,traceH;
    uint depthIdx,normalIdx,albedoIdx,surfaceIdx;
    uint velocityIdx,materialsIdx,lightsIdx,lightCount;
    uint alphaTestEnabled,alphaInstanceCount,counterUav,seed;
    uint diffUav,normalUav,viewZUav,mvUav;
    uint diffSrv,coverageUav,coverageSrv,volumeIdx;
    uint irradianceIdx,ddgiDepthIdx,probeIdx,volumeCount;
    float constantRadiance; uint worldGrid,tier,rrDistanceUav;   // rrDistanceUav: RR's diffuse hit distance (Resolve)
    uint initialUav,initialSrv,previousSrv,temporalUav;
    uint temporalSrv,previousSurfaceSrv,surfaceUav,surfaceSrv;
    uint historyReset,maxM,maxAge,spatialRadius;
    float2 historyJitterDelta,reprojectionPadding;
    uint emitterIdx,emitterTableIdx,emitterCount,emitterCandidates;
    uint initialCandidates,hitExactLights,hitLightSamples,localEmitterLimit;
    uint spatialSamples,boostSamples; float freshBlend; uint cbPadding2;
#if RTGI_SHARC
    // SHaRC radiance cache (mirrored by RTGICore.h). Declared only in the RTGI_SHARC builds, so
    // the non-SHaRC kernels' cbuffer and DXIL carry nothing of it.
    uint sharcHashUav,sharcAccumulationUav,sharcResolvedUav,sharcCapacity;
    float sharcSceneScale,sharcRadianceScale,sharcMaxVoxel; uint sharcFlags;
    float3 sharcPrevCamera; uint sharcFrame;
    uint sharcUpdateTile,sharcUpdateBounces,sharcAccumulationFrames,sharcStaleFrames;
    uint skyIdx; float environmentScale,sharcDepositMax; uint sharcPad0;
#endif
};
RaytracingAccelerationStructure g_TLAS : register(t0);
StructuredBuffer<RTInstanceData> g_Instances : register(t1);
StructuredBuffer<RTAlphaRecord> g_AlphaRecords : register(t2);
ByteAddressBuffer g_Buffers[16384] : register(t0,space1);
Texture2D g_AllTextures[16384] : register(t0,space2);
SamplerState gSampWrap : register(s1);
RWTexture2D<float4> Result : register(u0);
typedef RTInstanceData HRInstanceData;
#define HR_MaterialUV RTInstanceUV
#include "rt_hit_surface.hlsli"

static const uint kGIFlagNoInverseVisibility=128u, kGIFlagGeometricHitDistance=256u, kGIFlagRelax=512u,
    kGIFlagBoilingFilter=1024u, kGIFlagJacobianClamp=2048u, kGIFlagNoHitEmission=4096u, kGIFlagRandomAgeCap=32768u,
    kGIFlagEmitterSampling=65536u, kGIFlagShadingHorizon=131072u,
    kGIFlagNoDisocclusionBoost=262144u, kGIFlagTemporalOneOverM=524288u, kGIFlagNoCoverageFilter=1048576u;
// Opt-in RTXDI clamp of a reconnection Jacobian that passed the (0.1,10) leash to [1/3,3]:
// bounded shifted weights for a small bias. Off by default: it gave no measurable noise
// change under camera motion.
float GIClampJacobian(float j) { return (flags&kGIFlagJacobianClamp)?clamp(j,1.0/3,3.0):j; }
// NRD front/back ends: RELAX takes world-space hit distance, REBLUR a
// normalized one (hitDistanceParameters {rayTMax,0,1}).
float4 GIPackSignal(float3 radiance,float distance,float viewZ)
{
    if(flags&kGIFlagRelax) return RELAX_FrontEnd_PackRadianceAndHitDist(radiance,distance,false);
    return REBLUR_FrontEnd_PackRadianceAndNormHitDist(radiance,REBLUR_FrontEnd_GetNormHitDist(distance,viewZ,float3(rayTMax,0,1),1),false);
}
float3 GIUnpackSignal(float4 value)
{ return (flags&kGIFlagRelax)?RELAX_BackEnd_UnpackRadiance(value).rgb:REBLUR_BackEnd_UnpackRadianceAndNormHitDist(value).rgb; }
void Count(uint index)
{
    RWStructuredBuffer<uint> counts=ResourceDescriptorHeap[counterUav];
    uint n=WaveActiveCountBits(true);
    if(WaveIsFirstLane()) InterlockedAdd(counts[index],n);
}
// P is the GI receiver point and every connection's origin: the surface point lifted along
// Ng. N is the shading normal (the cosine), Ng the geometric normal (the horizon). Do not
// lift the origin along N: it then differs from the point the weights are evaluated at, and
// directions below the geometric surface hit the receiver's own surface on bevels, smoothed
// hard edges and normal maps, returning its own radiance to itself (a coloured outline).
struct Receiver { float3 P,N,Ng,albedo; float viewZ; uint material; bool eligible; };
float3 GIWorldPosition(int2 q,out float depth)
{
    Texture2D<float> depths=ResourceDescriptorHeap[depthIdx];
    depth=depths.Load(int3(q,0));
    float2 uv=(float2(q)+.5)/float2(width,height);
    return WorldPosFromDepth(uv,depth,invViewProj);
}
float GIViewDepth(float3 p) { return mul(float4(p,1),worldToView).z; }
// Geometric normal from the depth buffer. Per axis, the one-sided difference
// toward the neighbour nearer in view depth, so it never spans a silhouette;
// oriented to the camera. Degenerate reconstructions, and ones more than ~87
// degrees from the shading normal, fall back to the shading normal.
float3 GIGeometricNormal(int2 px,float3 P,float3 N)
{
    if(flags&kGIFlagShadingHorizon) return N;
    float d0,d1,d2,d3;
    const int2 hi=int2(width,height)-1;
    const float3 xp=GIWorldPosition(min(px+int2(1,0),hi),d0), xm=GIWorldPosition(max(px-int2(1,0),0),d1);
    const float3 yp=GIWorldPosition(min(px+int2(0,1),hi),d2), ym=GIWorldPosition(max(px-int2(0,1),0),d3);
    const bool bxp=d0>0 && px.x<hi.x, bxm=d1>0 && px.x>0, byp=d2>0 && px.y<hi.y, bym=d3>0 && px.y>0;
    if(!(bxp||bxm) || !(byp||bym)) return N;
    const float z=GIViewDepth(P);
    const bool useXp=bxp && (!bxm || abs(GIViewDepth(xp)-z)<=abs(GIViewDepth(xm)-z));
    const bool useYp=byp && (!bym || abs(GIViewDepth(yp)-z)<=abs(GIViewDepth(ym)-z));
    float3 g=cross(useYp?yp-P:P-ym,useXp?xp-P:P-xm);
    if(!(dot(g,g)>1e-24)) return N;
    g=normalize(g);
    if(dot(g,camera-P)<0) g=-g;
    return dot(g,N)>.05?g:N;
}
Receiver LoadReceiver(uint2 px)
{
    Receiver r=(Receiver)0;
    Texture2D<float> depths=ResourceDescriptorHeap[depthIdx];
    float depth=depths.Load(int3(px,0));
    if(depth<=0) return r;
    Texture2D<float4> normals=ResourceDescriptorHeap[normalIdx];
    float4 normal=normals.Load(int3(px,0));
    StructuredBuffer<MaterialGPUData> materials=ResourceDescriptorHeap[materialsIdx];
    uint material=GBufferDecodeMatIdx(normal.a);
    uint model=materials[material].customShadingModel; r.material=material;
    Texture2D<float4> albedos=ResourceDescriptorHeap[albedoIdx];
    r.albedo=saturate(albedos.Load(int3(px,0)).rgb);
    r.eligible=ShadingModelGiReceives(model);   // the shading-model table's `gi` column
    float2 uv=(float2(px)+.5)/float2(width,height);
    const float3 P=WorldPosFromDepth(uv,depth,invViewProj); r.N=normalize(normal.xyz*2-1);
    r.viewZ=abs(GIViewDepth(P));
    r.Ng=GIGeometricNormal(int2(px),P,r.N);
    r.P=P+r.Ng*(1e-3+1e-4*r.viewZ);
    return r;
}
// The receiver is already lifted off its surface; TMin only guards precision.
float GIRayTMin(Receiver r) { return 1e-4+1e-5*r.viewZ; }
uint2 FullPixel(uint2 p) { return min(p*((flags&4u)?1u:2u),uint2(width,height)-1); }
float Visibility(float3 p,float3 n,float3 l,float distance)
{
    Count(3);
    RayDesc ray; ray.Origin=p+n*.001; ray.Direction=l; ray.TMin=.001; ray.TMax=distance;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(g_TLAS,RAY_FLAG_NONE,0x02,ray);
    while(q.Proceed())
        if(q.CandidateType()==CANDIDATE_NON_OPAQUE_TRIANGLE && HR_AlphaAccept(
            q.CandidateInstanceID(),q.CandidatePrimitiveIndex(),q.CandidateTriangleBarycentrics(),false))
            q.CommitNonOpaqueTriangleHit();
    return q.CommittedStatus()==COMMITTED_NOTHING?1:0;
}
float3 IndirectAtHit(RTHitSurface s)
{
    if(volumeCount==0 || volumeIdx==0) return 0;
    StructuredBuffer<DDGIVolumeGPU> volumes=ResourceDescriptorHeap[volumeIdx];
    DDGIVolumePick pick=DDGI_PickVolumes(s.P,volumes);
    float4 best=0,second=0;
    [unroll] for(uint i=0;i<DDGI_MAX_VOLUMES;++i)
    {
        if(!DDGI_IsPicked(pick,i)) continue;
        Texture2D<float4> irradiance=ResourceDescriptorHeap[irradianceIdx+i];
        Texture2D<float2> depth=ResourceDescriptorHeap[ddgiDepthIdx+i];
        StructuredBuffer<DDGIProbeData> probes=ResourceDescriptorHeap[probeIdx+i];
        float4 value=DDGI_SampleVolumeRaw(s.P,s.Nshade,s.Nshade,volumes[i],irradiance,depth,probes,gSampWrap);
        if(i==(uint)pick.bestSlot) best=value; else second=value;
    }
    float4 value=DDGI_CombineWinners(best,second,pick);
    return value.rgb*value.a;
}
void RT_CountRandomLightPick() { Count(12); }
#include "rt_shade_diffuse.hlsli"
[numthreads(1,1,1)]
void Clear()
{
    RWStructuredBuffer<uint> counts=ResourceDescriptorHeap[counterUav];
    // 10 spatial visibility rays, 11 luminance M resets, 12 many-light hits, 13 boiled,
    // 14 visible emitter samples, 15 emitter samples selected, 16 rays below the
    // geometric horizon, 17 single-sided back-face hits (unresolved), 18 no
    // compatible history tap, 19 Jacobian leash, 20 age cap, 21 occluded
    // history, 22-24 vanished history (miss, wrong distance, other).
    for(uint i=0;i<32;++i) counts[i]=0;
}

// ==== Emitter sampling ====
// GI emitter table row (mirror: GIEmitterGPU in DDGISceneAS.cpp, 112 bytes): every TLAS
// instance flagged RT_INST_FLAG_GI_EMITTER, in InstanceID order.
struct GIEmitter { float4 objectToWorld[3]; float4 worldToObject[3]; uint instanceID,triangleCount; float radiance; uint padding; };
float3x4 GIEmitterObjectToWorld(GIEmitter e) { return float3x4(e.objectToWorld[0],e.objectToWorld[1],e.objectToWorld[2]); }
float3x4 GIEmitterWorldToObject(GIEmitter e) { return float3x4(e.worldToObject[0],e.worldToObject[1],e.worldToObject[2]); }
bool GIEmitterSampling() { return (flags&kGIFlagEmitterSampling) && !(flags&(2u|kGIFlagNoHitEmission)) && emitterCount>0; }
// Area/CDF table, two rows per emitter (filled on the GPU every frame):
//   [2e]   x = world area, y = power (area x radiance estimate), z = inclusive power CDF
//   [2e+1] xyz = world bounding-sphere centre, w = its radius
// One group per emitter sums its triangles' areas and bounds.
groupshared float gGIArea[64];
groupshared float3 gGIMin[64], gGIMax[64];
[numthreads(64,1,1)]
void EmitterArea(uint3 group:SV_GroupID,uint lane:SV_GroupIndex)
{
    const uint e=group.y*65535u+group.x;
    if(e>=emitterCount) return; // uniform per group
    StructuredBuffer<GIEmitter> emitters=ResourceDescriptorHeap[emitterIdx];
    RWStructuredBuffer<float4> table=ResourceDescriptorHeap[emitterTableIdx];
    const GIEmitter emitter=emitters[e];
    const float3x4 toWorld=GIEmitterObjectToWorld(emitter);
    float area=0; float3 lo=1e30, hi=-1e30;
    for(uint t=lane;t<emitter.triangleCount;t+=64)
    {
        float3 p0,p1,p2,n0,n1,n2; float2 uv0,uv1,uv2;
        if(!HR_LoadHitTriangleAttributes(emitter.instanceID,t,p0,p1,p2,n0,n1,n2,uv0,uv1,uv2)) continue;
        const float3 a=mul(toWorld,float4(p0,1)),b=mul(toWorld,float4(p1,1)),c=mul(toWorld,float4(p2,1));
        const float triangleArea=.5*length(cross(b-a,c-a));
        if(!isfinite(triangleArea)) continue;
        area+=triangleArea; lo=min(lo,min(a,min(b,c))); hi=max(hi,max(a,max(b,c)));
    }
    gGIArea[lane]=area; gGIMin[lane]=lo; gGIMax[lane]=hi;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for(uint stride=32;stride>0;stride>>=1)
    {
        if(lane<stride)
        {
            gGIArea[lane]+=gGIArea[lane+stride];
            gGIMin[lane]=min(gGIMin[lane],gGIMin[lane+stride]); gGIMax[lane]=max(gGIMax[lane],gGIMax[lane+stride]);
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if(lane==0)
    {
        const bool bounded=all(gGIMin[0]<=gGIMax[0]);
        table[2*e]=float4(gGIArea[0],bounded?gGIArea[0]*max(emitter.radiance,0):0,0,0);
        table[2*e+1]=bounded?float4((gGIMin[0]+gGIMax[0])*.5,.5*length(gGIMax[0]-gGIMin[0])):0;
    }
}
groupshared float gGIScan[1024];
[numthreads(1024,1,1)]
void EmitterCdf(uint lane:SV_GroupIndex)
{
    RWStructuredBuffer<float4> table=ResourceDescriptorHeap[emitterTableIdx];
    const uint per=(emitterCount+1023)/1024, begin=min(lane*per,emitterCount), end=min(begin+per,emitterCount);
    float local=0;
    for(uint i=begin;i<end;++i) local+=table[2*i].y;
    gGIScan[lane]=local;
    GroupMemoryBarrierWithGroupSync();
    for(uint offset=1;offset<1024;offset<<=1)
    {
        const float before=lane>=offset?gGIScan[lane-offset]:0;
        GroupMemoryBarrierWithGroupSync();
        gGIScan[lane]+=before;
        GroupMemoryBarrierWithGroupSync();
    }
    float running=gGIScan[lane]-local;
    for(uint k=begin;k<end;++k) { float4 row=table[2*k]; running+=row.y; row.z=running; table[2*k]=row; }
}
// Emitted part of a GI hit's outgoing radiance (RT_ShadeDiffuseHalf's own
// split: an Unlit hit is all emission).
float3 GIHitEmission(RTHitSurface s) { return s.unlit?s.albedo+s.emissive:((flags&kGIFlagNoHitEmission)?0:s.emissive); }
// Unoccluded segment receiver -> emitter point (the emitter itself excluded).
bool GIEmitterVisible(float3 origin,float3 target)
{
    float3 delta=target-origin; float distance=length(delta);
    RayDesc ray; ray.Origin=origin; ray.Direction=delta/max(distance,1e-8);
    ray.TMin=.001; ray.TMax=max(distance-max(.004,distance*.0003),.0011);
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(g_TLAS,RAY_FLAG_NONE,0x01,ray);
    while(q.Proceed())
        if(q.CandidateType()==CANDIDATE_NON_OPAQUE_TRIANGLE && HR_AlphaAccept(
            q.CandidateInstanceID(),q.CandidatePrimitiveIndex(),q.CandidateTriangleBarycentrics(),false))
            q.CommitNonOpaqueTriangleHit();
    return q.CommittedStatus()==COMMITTED_NOTHING;
}
#if RTGI_SHARC
// ==== SHaRC radiance cache ====
// A world-space hash grid (SharcEngine.hlsli) of a GI hit's view-independent reflected
// light, filled by a sparse G-buffer-rooted update (RTGISharcUpdate) and resolved every frame
// (RTGISharcResolve) before Trace reads it. A query replaces RT_ShadeDiffuseHalf's light
// loop, shadow rays and DDGI lookup at the hit by one hash probe; the hit's emission and its
// sun (one shadow ray) stay exact - SHaRC's emissive slot (SHARC_SEPARATE_EMISSIVE) - so the
// cosine / emitter balance heuristic's partition of unity is untouched. The cache never
// decides coverage: misses and out-of-range rays stay the far field's.
#ifndef SHARC_ENABLE_CACHE_RESAMPLING
#define SHARC_ENABLE_CACHE_RESAMPLING 0   // RTGIPass defines it from -gi_sharc_resampling (an opt-in multi-bounce arm)
#endif
#include "SharcEngine.hlsli"
// sharcFlags (RTGIPass::Execute)
static const uint kGISharcQuery=1u, kGISharcMulti=2u, kGISharcForceMiss=4u, kGISharcUpdate=8u, kGISharcBound=16u;
// GIReservoir.metadata bit 12 (M is bits 0-5, age 6-11; an age below its cap
// of at most 63 never carries into it): the sample was shaded from the cache.
// The history validation re-shades it the same way, so a receiver's move never
// flips a sample between the cache and the analytic shading.
static const uint kGIMetaCached=4096u;
static const uint kGIShadeAnalytic=0u, kGIShadeAuto=1u, kGIShadeCached=2u;
SharcParameters GISharcParameters()
{
    SharcParameters p;
    SHARC_ENGINE_PARAMETERS(p,camera,sharcHashUav,sharcAccumulationUav,sharcResolvedUav,sharcCapacity,sharcSceneScale,sharcRadianceScale);
    return p;
}
// The sun's share of a hit's reflected light (RT_ShadeDiffuseHalf's sun term
// times the hit's diffuse albedo): one shadow ray, in a real branch - HLSL
// 2018's ?: evaluates both operands.
float3 GISunAtHit(RTHitSurface s)
{
    float3 sun=0;
    const float3 l=normalize(-sunDirection);
    const float cosine=saturate(dot(s.Nshade,l));
    [branch] if(cosine>0 && any(sunColor>0))
        sun=s.albedo*(1-s.metal)*sunColor*cosine*Visibility(s.P,s.Ngeo,l,100)/PI;
    return sun;
}
// Surfaces the cache never stands for: Unlit (all emission), characters
// (skinned BLAS; the update never deposits on them, so their shadows on lit
// surfaces are the cache's delayed part), untrusted proxy materials.
bool GISharcEligible(RTHitSurface s)
{
    return !s.unlit && (s.flags&RT_INST_FLAG_SKINNED)==0 && s.trusted;
}
// Whether the last RTGI_HitRadiance came from the cache.
static bool g_GIShadeCached=false;
// A GI hit's outgoing radiance: the cache where it may stand for the hit,
// otherwise the analytic Tier shading (bit for bit RT_ShadeDiffuseHalf). The
// gate: an eligible surface, a voxel no larger than sharcMaxVoxel and - for a
// sample being created (kGIShadeAuto) - a segment longer than the voxel's
// diagonal (SHaRC's rule against looking up the receiver's own light; a
// validation re-query (kGIShadeCached) skips it, it depends on the receiver),
// and an entry above SHARC_SAMPLE_NUM_THRESHOLD samples. Counters: 25 cache
// hits, 26 no mature entry, 27 gated by the voxel, 28 ineligible surface, 31 a
// cached sample re-shaded analytically.
float3 RTGI_HitRadiance(RTHitSurface s,float hitT,uint mode,inout uint rng)
{
    g_GIShadeCached=false;
    [branch] if(sharcFlags&kGISharcBound) return constantRadiance.xxx;   // the cost-bound arm: no hit shading at all
    [branch] if(mode!=kGIShadeAnalytic && (sharcFlags&kGISharcQuery))
    {
        if(!GISharcEligible(s)) Count(28);
        else
        {
            const SharcParameters p=GISharcParameters();
            const float voxel=SHARC_ENGINE_VOXEL_SIZE(s.P,p);
            if(!(voxel<=sharcMaxVoxel) || (mode==kGIShadeAuto && !(hitT>1.7320508*voxel))) Count(27);
            else
            {
                SharcHitData h;
                h.positionWorld=s.P; h.normalWorld=s.Ngeo; h.emissive=0;
                float3 cached=0;
                [branch] if(!(sharcFlags&kGISharcForceMiss) && SharcGetCachedRadiance(p,h,cached,false) && all(isfinite(cached)))
                {
                    Count(25);
                    g_GIShadeCached=true;
                    return max(cached,0)+GIHitEmission(s)+GISunAtHit(s);
                }
                Count(26);
            }
        }
        if(mode==kGIShadeCached) Count(31);
    }
    return RT_ShadeDiffuseHalf(s,tier,1,rng);
}
#define RTGI_HIT_RADIANCE(s,hitT,mode,rng) RTGI_HitRadiance(s,hitT,mode,rng)
#define GIMetaKeep(value,source) ((value)|((source).metadata&kGIMetaCached))
#define GIValidationMode(sample) ((((sample).metadata&kGIMetaCached)!=0)?kGIShadeCached:kGIShadeAnalytic)
// The validation's luminance-change reset (ValidateHistory), by how the sample was shaded
// before this re-shade. The analytic shading of a stored hit is deterministic, so a 20%
// change there is a real lighting change. A sample the cache shaded moves with the cache's
// own estimate (its voxel averages fresh, noisy deposits every frame), and so does one the
// cache shaded that this re-shade had to shade analytically (its entry fell below the sample
// threshold or was evicted). Resetting M on either is noise (the 20% rule reset ~1000
// histories a frame), so only a gross change (default 60%, a lamp switched off) resets them.
// RTGIPass may pass RTGI_SHARC_CACHED_RESET.
#ifndef RTGI_SHARC_CACHED_RESET
#define RTGI_SHARC_CACHED_RESET .6
#endif
#define GI_LUMINANCE_RESET(cachedBefore) ((cachedBefore)?RTGI_SHARC_CACHED_RESET:.2)
#else
// Without the cache: the analytic shading only (no cache code in the non-SHaRC kernels).
#define RTGI_HIT_RADIANCE(s,hitT,mode,rng) RT_ShadeDiffuseHalf(s,tier,1,rng)
#define GIMetaKeep(value,source) (value)
#define GI_LUMINANCE_RESET(cachedBefore) .2
#endif
// ==== Emitter selection ====
// Up to localEmitterLimit emitters the pmf is per receiver: power over the squared distance
// to the emitter's bounding sphere, zero where the whole sphere is behind the receiver's
// geometric horizon or out of range. A global power pmf sends most candidates to lamps in
// other rooms (only ~2.4% of them visible in a multi-room scene). Above that count the global
// power CDF is used. The same function gives the sampler's pmf and the density a cosine hit
// on an emitter looks up, which the MIS weights need to match exactly.
bool GILocalEmitterPmf() { return emitterCount<=localEmitterLimit; }
float GIEmitterWeight(Receiver r,uint e)
{
    RWStructuredBuffer<float4> table=ResourceDescriptorHeap[emitterTableIdx];
    const float power=table[2*e].y; const float4 sphere=table[2*e+1];
    if(!(power>0)) return 0;
    const float3 d=sphere.xyz-r.P; const float d2=dot(d,d);
    if(dot(d,r.Ng)<-sphere.w) return 0;
    if(d2>(rayTMax+sphere.w)*(rayTMax+sphere.w)) return 0;
    return power/max(d2,max(sphere.w*sphere.w,1e-4));
}
// Normaliser of the receiver's pmf (the global total for large tables).
float GIEmitterTotal(Receiver r)
{
    RWStructuredBuffer<float4> table=ResourceDescriptorHeap[emitterTableIdx];
    if(!GILocalEmitterPmf()) return table[2*(emitterCount-1)].z;
    float total=0;
    [loop] for(uint e=0;e<emitterCount;++e) total+=GIEmitterWeight(r,e);
    return total;
}
float GIEmitterPmf(Receiver r,uint e,float total)
{
    RWStructuredBuffer<float4> table=ResourceDescriptorHeap[emitterTableIdx];
    return total>0?(GILocalEmitterPmf()?GIEmitterWeight(r,e):table[2*e].y)/total:0;
}
// Emitter-table row of an instance (rows are in InstanceID order).
bool GIEmitterRow(uint instanceID,out uint row)
{
    StructuredBuffer<GIEmitter> emitters=ResourceDescriptorHeap[emitterIdx];
    uint lo=0,hi=emitterCount;
    [loop] while(lo<hi) { const uint mid=(lo+hi)>>1; if(emitters[mid].instanceID<instanceID) lo=mid+1; else hi=mid; }
    row=lo;
    return lo<emitterCount && emitters[lo].instanceID==instanceID;
}
// Solid-angle density of the emitter technique at a point P of (row, tri):
// pmf / (triangles x world area) x d^2 / cos_e. Zero wherever the sampler
// rejects (range, horizon, degenerate triangle).
float GIEmitterDensity(Receiver r,float total,uint row,uint tri,float3 P,float3 Ngeo)
{
    StructuredBuffer<GIEmitter> emitters=ResourceDescriptorHeap[emitterIdx];
    const GIEmitter emitter=emitters[row];
    const float pmf=GIEmitterPmf(r,row,total);
    if(!(pmf>0) || tri>=emitter.triangleCount) return 0;
    float3 p0,p1,p2,n0,n1,n2; float2 uv0,uv1,uv2;
    if(!HR_LoadHitTriangleAttributes(emitter.instanceID,tri,p0,p1,p2,n0,n1,n2,uv0,uv1,uv2)) return 0;
    const float3x3 m=(float3x3)GIEmitterObjectToWorld(emitter);
    const float area=.5*length(cross(mul(m,p1-p0),mul(m,p2-p0)));
    if(!(area>1e-12)) return 0;
    const float3 delta=P-r.P; const float d2=dot(delta,delta);
    if(d2<1e-5 || d2>rayTMax*rayTMax) return 0;
    const float3 l=delta*rsqrt(d2);
    const float emitterCosine=dot(Ngeo,-l);
    if(emitterCosine<=0 || dot(r.N,l)<=0 || dot(r.Ng,l)<=0) return 0;
    return pmf/(float(emitter.triangleCount)*area)*d2/emitterCosine;
}
// Systematic selection over the receiver's pmf: candidate k takes the
// emitter whose CDF crosses (k + offset) / K x total. Each candidate is
// distributed by the pmf (GRIS needs no independence between candidates), and
// K picks cost one walk over the table. The walk state is (e, acc, w).
struct GIEmitterWalk { uint e; float acc,w; };
uint GIEmitterPick(Receiver r,inout GIEmitterWalk walk,uint k,uint K,float offset,float total,inout uint rng)
{
    RWStructuredBuffer<float4> table=ResourceDescriptorHeap[emitterTableIdx];
    if(!GILocalEmitterPmf())
    {
        const float u=RTRandom(rng)*total;
        uint lo=0,hi=emitterCount-1;
        [loop] while(lo<hi) { const uint mid=(lo+hi)>>1; if(table[2*mid].z>u) hi=mid; else lo=mid+1; }
        return lo;
    }
    const float threshold=(float(k)+offset)/float(K)*total;
    [loop] while(walk.e+1<emitterCount && (walk.acc+walk.w<=threshold || !(walk.w>0)))
    { walk.acc+=walk.w; ++walk.e; walk.w=GIEmitterWeight(r,walk.e); }
    return walk.e;
}
// ==== Initial candidates ====
// N_c cosine rays and N_e emitter points in one RIS stream. The integrand is split into its
// reflected and emitted parts: the reflected part is the cosine technique's alone, the
// emitted part is shared by both techniques through the balance heuristic. That is a
// partition of unity, so
//   cosine x:  w = p_refl(x) / (N_c p_cos(x)) + p_emit(x) / (N_c p_cos(x) + N_e p_E(x))
//   emitter y: w = p_emit(y) / (N_c p_cos(y) + N_e p_E(y))
// and W = sum(w) / p(Y) with the exact full target. An emitter point needs only its emission
// (no shading) and its visibility (the cosine density applies to visible points only); the
// winner is shaded exactly. A cosine hit on a lamp keeps the share of its emission the
// emitter technique would not sample.
bool GIEmitterPoint(Receiver r,uint row,inout uint rng,out uint tri,out float2 bary,out float3 P)
{
    StructuredBuffer<GIEmitter> emitters=ResourceDescriptorHeap[emitterIdx];
    const GIEmitter emitter=emitters[row];
    const float ut=RTRandom(rng), u1=RTRandom(rng), u2=RTRandom(rng);
    tri=0; bary=0; P=0;
    if(emitter.triangleCount==0) return false;
    tri=min(uint(ut*emitter.triangleCount),emitter.triangleCount-1);
    float3 p0,p1,p2,n0,n1,n2; float2 uv0,uv1,uv2;
    if(!HR_LoadHitTriangleAttributes(emitter.instanceID,tri,p0,p1,p2,n0,n1,n2,uv0,uv1,uv2)) return false;
    const float3x4 toWorld=GIEmitterObjectToWorld(emitter);
    const float3 a=mul(toWorld,float4(p0,1)),b=mul(toWorld,float4(p1,1)),c=mul(toWorld,float4(p2,1));
    const float root=sqrt(u1);
    bary=float2(root*(1-u2),root*u2);
    P=a*(1-bary.x-bary.y)+b*bary.x+c*bary.y;
    const float3 delta=P-r.P; const float d2=dot(delta,delta);
    return d2>=1e-5 && d2<=rayTMax*rayTMax && dot(r.N,delta)>0 && dot(r.Ng,delta)>0;
}
[numthreads(8,8,1)]
void Trace(uint3 tid:SV_DispatchThreadID)
{
    uint2 tp=tid.xy;
    if(any(tp>=uint2(traceW,traceH))) return;
    uint2 px=FullPixel(tp); Receiver r=LoadReceiver(px);
    RWTexture2D<float4> diffuse=ResourceDescriptorHeap[diffUav];
    RWTexture2D<float4> normal=ResourceDescriptorHeap[normalUav];
    RWTexture2D<float> viewZ=ResourceDescriptorHeap[viewZUav];
    RWTexture2D<float2> mv=ResourceDescriptorHeap[mvUav];
    RWTexture2D<float> coverage=ResourceDescriptorHeap[coverageUav];
    Texture2D<float2> velocity=ResourceDescriptorHeap[velocityIdx];
    normal[tp]=NRD_FrontEnd_PackNormalAndRoughness(r.eligible?r.N:float3(0,0,1),1,0);
    viewZ[tp]=r.eligible?r.viewZ:1e7;
    mv[tp]=r.eligible?velocity.Load(int3(px,0))*float2(-.5,.5):0;
    float3 radiance=0; float hitDistance=0,resolved=0;
    GIReservoir reservoir=(GIReservoir)0; reservoir.metadata=1;
    if(r.eligible)
    {
        Count(0); Count(1);
        // Hashed seed: do not seed the GF(2)-linear xorshift with (pixel*A)^(frame*B), or every
        // pixel's stream differs from its neighbours' by a frame-invariant XOR. The cosine
        // directions are Hilbert-R2 points, the same low-discrepancy sequence DI uses, so successive
        // frames stratify.
        uint rng=HashLowbias32((tp.x+tp.y*traceW)^HashLowbias32(frame*0x9e3779b9u+seed))|1u;
        const uint cosineCount=max(initialCandidates,1u);
        const float emitterTotal=GIEmitterSampling()?GIEmitterTotal(r):0;
        const uint emitterPoints=emitterTotal>0?emitterCandidates:0u;
        GIEmitterWalk walk; walk.e=0; walk.acc=0; walk.w=emitterPoints>0 && GILocalEmitterPmf()?GIEmitterWeight(r,0):0;
        const float walkOffset=RTRandom(rng);
        float wsum=0; bool winnerEmitter=false; uint winnerRow=0,winnerTri=0; float2 winnerBary=0;
        float nearest=rayTMax; uint resolvedRays=0;
        [loop] for(uint c=0;c<cosineCount+emitterPoints;++c)
        {
            if(c<cosineCount)
            {
                float2 u=RTReservoirR2(tp,(frame+seed*7919u)*cosineCount+c,0x5bd1u);
                float radius=sqrt(u.x),phi=2*PI*u.y;
                const float3 direction=RTBasis(r.N,float3(radius*cos(phi),radius*sin(phi),sqrt(1-u.x)));
                // Below the geometric horizon the receiver's own surface is in
                // the way: a genuine zero (resolved, no radiance), as RTXDI's
                // BrdfRayTracing.
                if(dot(direction,r.Ng)<=0) { ++resolvedRays; Count(16); continue; }
                RayDesc ray; ray.Origin=r.P; ray.Direction=direction; ray.TMin=GIRayTMin(r); ray.TMax=rayTMax;
                RayQuery<RAY_FLAG_NONE> q;
                q.TraceRayInline(g_TLAS,RAY_FLAG_NONE,0x01,ray);
                while(q.Proceed())
                    if(q.CandidateType()==CANDIDATE_NON_OPAQUE_TRIANGLE && HR_AlphaAccept(
                        q.CandidateInstanceID(),q.CandidatePrimitiveIndex(),q.CandidateTriangleBarycentrics(),false))
                        q.CommitNonOpaqueTriangleHit();
                if(q.CommittedStatus()!=COMMITTED_TRIANGLE_HIT) continue;
                const float distance=q.CommittedRayT();
                RTHitSurface s;
                if(!RTLoadHitSurface(q.CommittedInstanceID(),q.CommittedPrimitiveIndex(),q.CommittedTriangleBarycentrics(),
                    q.CommittedObjectToWorld3x4(),q.CommittedWorldToObject3x4(),ray.Origin+direction*distance,
                    true,tier==0?4:0,s)) continue;
                const bool facing=dot(s.Ngeo,-direction)>0;
                if(!facing && (s.flags&RT_INST_FLAG_DOUBLE_SIDED)==0) { Count(17); continue; }
                if(!facing) { s.Ngeo=-s.Ngeo; s.Nshade=-s.Nshade; }
                ++resolvedRays; nearest=min(nearest,distance); Count(2);
                if(s.flags&RT_INST_FLAG_SKINNED) Count(4);
                GIReservoir candidate=(GIReservoir)0;
                candidate.P=s.P; candidate.normal=GIPackNormal(s.Ngeo);
                candidate.radiance=(flags&2u)?constantRadiance.xxx:RTGI_HIT_RADIANCE(s,distance,kGIShadeAuto,rng);
#if RTGI_SHARC
                candidate.metadata=g_GIShadeCached?kGIMetaCached:0u;
#endif
                if(!all(isfinite(candidate.radiance))) { Count(5); continue; }
                const float target=GITarget(r.P,r.N,candidate,r.albedo);
                float emitted=0,emitterDensity=0; uint row;
                if(emitterPoints>0 && (s.flags&RT_INST_FLAG_GI_EMITTER) && GIEmitterRow(q.CommittedInstanceID(),row))
                {
                    GIReservoir emission=candidate; emission.radiance=max(GIHitEmission(s),0);
                    emitted=min(GITarget(r.P,r.N,emission,r.albedo),target);
                    emitterDensity=GIEmitterDensity(r,emitterTotal,row,q.CommittedPrimitiveIndex(),s.P,s.Ngeo);
                }
                const float cosineDensity=float(cosineCount)*dot(r.N,direction)/PI;
                const float w=(target-emitted)/cosineDensity+emitted/(cosineDensity+float(emitterPoints)*emitterDensity);
                if(!(w>0) || !isfinite(w)) continue;
                wsum+=w;
                if(RTRandom(rng)*wsum<w) { reservoir=candidate; reservoir.target=target; winnerEmitter=false; }
            }
            else
            {
                const uint row=GIEmitterPick(r,walk,c-cosineCount,emitterPoints,walkOffset,emitterTotal,rng);
                uint tri; float2 bary; float3 P;
                if(!GIEmitterPoint(r,row,rng,tri,bary,P)) continue;
                StructuredBuffer<GIEmitter> emitters=ResourceDescriptorHeap[emitterIdx];
                const GIEmitter emitter=emitters[row];
                if(!HR_AlphaAccept(emitter.instanceID,tri,bary,false)) continue;
                RTHitSurface s;
                if(!RTLoadHitSurface(emitter.instanceID,tri,bary,GIEmitterObjectToWorld(emitter),GIEmitterWorldToObject(emitter),
                    P,true,tier==0?4:0,s)) continue;
                const float3 l=normalize(P-r.P);
                if(dot(s.Ngeo,-l)<=0)
                {
                    if((s.flags&RT_INST_FLAG_DOUBLE_SIDED)==0) continue;
                    s.Ngeo=-s.Ngeo; s.Nshade=-s.Nshade;
                }
                GIReservoir emission=(GIReservoir)0; emission.P=P; emission.radiance=max(GIHitEmission(s),0);
                const float emitted=GITarget(r.P,r.N,emission,r.albedo);
                if(!(emitted>0)) continue;
                const float emitterDensity=GIEmitterDensity(r,emitterTotal,row,tri,P,s.Ngeo);
                if(!(emitterDensity>0)) continue; // a pmf that rounded to zero: never sampled
                const float w=emitted/(float(cosineCount)*dot(r.N,l)/PI+float(emitterPoints)*emitterDensity);
                if(!(w>0) || !isfinite(w)) continue;
                // The cosine density applies to visible points only: an occluded
                // point is a zero of the integrand.
                if(!GIEmitterVisible(r.P,P)) continue;
                Count(14);
                wsum+=w;
                if(RTRandom(rng)*wsum<w) { winnerEmitter=true; winnerRow=row; winnerTri=tri; winnerBary=bary; reservoir.P=P; }
            }
        }
        resolved=float(resolvedRays)/float(cosineCount);
        if(winnerEmitter)
        {
            // Shade the emitter winner exactly (its reflected light too).
            Count(15);
            StructuredBuffer<GIEmitter> emitters=ResourceDescriptorHeap[emitterIdx];
            const GIEmitter emitter=emitters[winnerRow];
            RTHitSurface s;
            if(RTLoadHitSurface(emitter.instanceID,winnerTri,winnerBary,GIEmitterObjectToWorld(emitter),GIEmitterWorldToObject(emitter),
                reservoir.P,true,tier==0?4:0,s))
            {
                if(dot(s.Ngeo,r.P-s.P)<=0) { s.Ngeo=-s.Ngeo; s.Nshade=-s.Nshade; }
                reservoir.normal=GIPackNormal(s.Ngeo);
                reservoir.radiance=RTGI_HIT_RADIANCE(s,length(reservoir.P-r.P),kGIShadeAuto,rng);
#if RTGI_SHARC
                reservoir.metadata=g_GIShadeCached?kGIMetaCached:0u;
#endif
                reservoir.target=all(isfinite(reservoir.radiance))?GITarget(r.P,r.N,reservoir,r.albedo):0;
            }
            else reservoir.target=0;
        }
        reservoir.metadata=GIMetaKeep(1,reservoir);
        reservoir.W=reservoir.target>0?wsum/reservoir.target:0;
        if(!(reservoir.target>0)) { reservoir=(GIReservoir)0; reservoir.metadata=1; }
        if(!isfinite(reservoir.W)) { reservoir.W=0; Count(5); }
        // This frame's own estimate f(Y) W (the no-reuse arm denoises it).
        radiance=reservoir.W>0?GIIntegrand(r.P,r.N,reservoir)*reservoir.W:0;
        hitDistance=reservoir.W>0?length(reservoir.P-r.P):(resolvedRays>0?nearest:0);
    }
    if(!all(isfinite(radiance))) { radiance=0; Count(5); }
    // Unresolved rays contribute zero to BOTH fields, reserving their angular
    // share for receiver DDGI/IBL; coverage is the cosine rays' alone.
    diffuse[tp]=GIPackSignal(radiance,hitDistance,r.viewZ);
    coverage[tp]=resolved;
    if(flags&8u)
    {
        uint index=tp.y*traceW+tp.x;
        RWStructuredBuffer<GIReservoir> initial=ResourceDescriptorHeap[initialUav];
        RWStructuredBuffer<GISurface> surfaces=ResourceDescriptorHeap[surfaceUav];
        GISurface surface=(GISurface)0; surface.P=r.P; surface.normal=GIPackNormal(r.N); surface.geoNormal=GIPackNormal(r.Ng);
        surface.material=r.material; surface.albedo=GIPackAlbedo(r.albedo); GISetCoverageCount(surface,resolved,r.eligible?1:0);
        initial[index]=reservoir; surfaces[index]=surface;
    }
}
[numthreads(8,8,1)]
void Resolve(uint3 tid:SV_DispatchThreadID)
{
    uint2 px=tid.xy;
    if(any(px>=uint2(width,height))) return;
    Receiver r=LoadReceiver(px);
    Texture2D<float4> diffuse=ResourceDescriptorHeap[diffSrv];
    Texture2D<float> coverage=ResourceDescriptorHeap[coverageSrv];
    float3 value=0; float cov=0,sum=0;
    if(r.eligible)
    {
        int scale=(flags&4u)?1:2;
        int radius=(flags&4u)?0:1;
        float nearestGap=1e30; int2 nearest=int2(-1,-1);
        for(int y=-radius;y<=radius;++y) for(int x=-radius;x<=radius;++x)
        {
            int2 p=clamp(int2(px/scale)+int2(x,y),0,int2(traceW,traceH)-1);
            Receiver n=LoadReceiver(FullPixel(p));
            if(!n.eligible) continue;
            float w=pow(saturate(dot(r.N,n.N)),32)*exp(-abs(dot(n.P-r.P,r.N))/max(.01,r.viewZ*.01));
            float3 filtered=GIUnpackSignal(diffuse.Load(int3(p,0)));
            value+=filtered*w;
            cov+=coverage.Load(int3(p,0))*w; sum+=w;
            const float gap=abs(n.viewZ-r.viewZ);
            if(gap<nearestGap) { nearestGap=gap; nearest=p; }
        }
        // A pixel the half-resolution taps never land on (a mortar joint, a groove, a thin trim)
        // matches none of them: pow(N.N,32) kills a groove wall's sideways normal. Coverage 0 would
        // hand it to the far field, which without DDGI is the unoccluded sky: sky-coloured seams down
        // a tunnel's walls that the path tracer and -gi_fullres keep dark. So the tap nearest in view
        // depth speaks for it when it sits within a seam's depth (the same object, its occlusion and
        // its light); a real silhouette (a larger depth jump) still goes to the far field.
        if(sum<=1e-3 && nearest.x>=0 && nearestGap<max(.05,r.viewZ*.02))
        {
            value=GIUnpackSignal(diffuse.Load(int3(nearest,0)));
            cov=coverage.Load(int3(nearest,0)); sum=1;
        }
    }
    // Renormalising only cross-surface taps (every weight tiny) would paint
    // another surface's GI here: leave the pixel to the far field instead.
    Result[px]=sum>1e-3?float4(value/sum,saturate(cov/sum)):0;
    // Under ray reconstruction (no NRD): the connection distance of the sample this pixel's
    // trace texel holds (its own at full resolution) is RR's diffuse hit distance (RELAX packed
    // it raw, REBLUR normalized by rayTMax); 0 without an estimate.
    if(rrDistanceUav!=0)
    {
        RWTexture2D<float> rrDistance=ResourceDescriptorHeap[rrDistanceUav];
        float d=0;
        if(r.eligible && sum>1e-3)
        {
            int2 q=clamp(int2(px/((flags&4u)?1u:2u)),0,int2(traceW,traceH)-1);
            float w=diffuse.Load(int3(q,0)).w;
            d=(flags&kGIFlagRelax)?w:w*rayTMax;
        }
        rrDistance[px]=d;
    }
}

bool Compatible(Receiver r,GISurface source)
{
    return GISurfaceCount(source)>0 && source.material==r.material && dot(r.N,GIUnpackNormal(source.normal))>.9
        && abs(dot(source.P-r.P,r.N))<max(.01,r.viewZ*.01);
}
// The re-trace ray of a stored connection from this receiver. False when the connection is
// shorter than 3 mm or beyond rayTMax; TMax overshoots the target slightly so the stored
// surface itself can be hit.
bool Reachable(Receiver r,float3 target,out RayDesc ray,out float distance)
{
    ray.Origin=r.P;
    float3 delta=target-ray.Origin; distance=length(delta);
    ray.Direction=delta/max(distance,1e-8); ray.TMin=GIRayTMin(r); ray.TMax=distance+max(.002,distance*.0001);
    return distance>.003 && distance<=rayTMax;
}
// Validate a history candidate before it is merged: re-trace the connection and, when it
// still lands on the stored surface, re-shade its radiance with the Tier shading.
//   kGIHistoryValid     the same point is still the closest hit (re-shaded)
//   kGIHistoryOccluded  something now sits in front of it: a genuine zero of
//                       the current integrand, so the history keeps its M
//   kGIHistoryVanished  nothing there any more (moved away, missed, back-facing):
//                       the shift has no image, so the whole history is dropped
//                       rather than letting its M dilute the fresh sample (a
//                       W=0-but-keep-M history darkens GI beside anything
//                       that moved).
static const uint kGIHistoryVanished=0u, kGIHistoryOccluded=1u, kGIHistoryValid=2u;
uint ValidateHistory(Receiver r,inout GIReservoir sample,inout uint rng)
{
    if(sample.W<=0 || sample.target<=0) return kGIHistoryVanished;
    RayDesc ray; float distance;
    if(!Reachable(r,sample.P,ray,distance)) { Count(24); return kGIHistoryVanished; }
    // Behind this receiver's own surface: a genuine zero of its integrand.
    if(dot(r.Ng,ray.Direction)<=0) { Count(21); return kGIHistoryOccluded; }
    if(flags&64u) return kGIHistoryValid;
    Count(8);
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(g_TLAS,RAY_FLAG_NONE,0x01,ray);
    while(q.Proceed())
        if(q.CandidateType()==CANDIDATE_NON_OPAQUE_TRIANGLE && HR_AlphaAccept(
            q.CandidateInstanceID(),q.CandidatePrimitiveIndex(),q.CandidateTriangleBarycentrics(),false))
            q.CommitNonOpaqueTriangleHit();
    if(q.CommittedStatus()!=COMMITTED_TRIANGLE_HIT) { Count(22); return kGIHistoryVanished; }
    const float tolerance=max(.003,distance*.0002);
    if(q.CommittedRayT()<distance-tolerance) { Count(21); return kGIHistoryOccluded; }
    if(abs(q.CommittedRayT()-distance)>tolerance) { Count(23); return kGIHistoryVanished; }
    RTHitSurface s;
    if(!RTLoadHitSurface(q.CommittedInstanceID(),q.CommittedPrimitiveIndex(),q.CommittedTriangleBarycentrics(),
        q.CommittedObjectToWorld3x4(),q.CommittedWorldToObject3x4(),ray.Origin+ray.Direction*q.CommittedRayT(),true,tier==0?4:0,s)) { Count(24); return kGIHistoryVanished; }
    if(dot(s.Ngeo,-ray.Direction)<=0)
    {
        if((s.flags&RT_INST_FLAG_DOUBLE_SIDED)==0) { Count(24); return kGIHistoryVanished; }
        s.Ngeo=-s.Ngeo; s.Nshade=-s.Nshade;
    }
    // Re-shaded the way it was shaded (the cached bit): the cache again, or
    // the analytic shading again - never a flip between the two.
    float3 updated=(flags&2u)?constantRadiance.xxx:RTGI_HIT_RADIANCE(s,q.CommittedRayT(),GIValidationMode(sample),rng);
#if RTGI_SHARC
    const bool cachedBefore=(sample.metadata&kGIMetaCached)!=0;
    sample.metadata=(sample.metadata&~kGIMetaCached)|(g_GIShadeCached?kGIMetaCached:0u);
#endif
    if(!all(isfinite(updated))) { Count(5); return kGIHistoryVanished; }
    // The analytic hit shading is deterministic, so a luminance change past GI_LUMINANCE_RESET
    // is a real lighting change: shorten the history instead of trusting stale M.
    float before=Luminance(max(sample.radiance,0)), after=Luminance(max(updated,0));
    if(abs(after-before)>GI_LUMINANCE_RESET(cachedBefore)*max(before,.001))
    { sample.metadata=(sample.metadata&~63u)|1u; Count(11); }
    sample.radiance=updated;
    return kGIHistoryValid;
}
// Visibility of another receiver's secondary point from this receiver: a
// spatial candidate hidden from here has zero target and must never be picked.
bool VisibleFrom(Receiver r,float3 target)
{
    RayDesc ray; float distance;
    if(!Reachable(r,target,ray,distance) || dot(r.Ng,ray.Direction)<=0) return false;
    if(flags&64u) return true;
    Count(10);
    ray.TMax=max(distance-max(.004,distance*.0003),.0011);
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(g_TLAS,RAY_FLAG_NONE,0x01,ray);
    while(q.Proceed())
        if(q.CandidateType()==CANDIDATE_NON_OPAQUE_TRIANGLE && HR_AlphaAccept(
            q.CandidateInstanceID(),q.CandidatePrimitiveIndex(),q.CandidateTriangleBarycentrics(),false))
            q.CommitNonOpaqueTriangleHit();
    return q.CommittedStatus()==COMMITTED_NOTHING;
}
groupshared float gGIBoil[64];
[numthreads(8,8,1)]
void Temporal(uint3 tid:SV_DispatchThreadID,uint groupIndex:SV_GroupIndex)
{
    // Every thread must reach the boiling filter's group barrier.
    uint2 tp=tid.xy; const bool inside=all(tp<uint2(traceW,traceH));
    uint index=inside?tp.y*traceW+tp.x:0; uint2 px=FullPixel(min(tp,uint2(traceW,traceH)-1)); Receiver r=LoadReceiver(px);
    StructuredBuffer<GIReservoir> initial=ResourceDescriptorHeap[initialSrv];
    RWStructuredBuffer<GIReservoir> output=ResourceDescriptorHeap[temporalUav];
    RWStructuredBuffer<GISurface> surfaces=ResourceDescriptorHeap[surfaceUav];
    GIReservoir result=initial[index]; GISurface surface=surfaces[index];
    uint rng=HashLowbias32(index^HashLowbias32(frame)^0x917a52b1u)|1u;
    if(inside && r.eligible && !historyReset)
    {
        Texture2D<float2> velocity=ResourceDescriptorHeap[velocityIdx];
        float2 motion=velocity.Load(int3(px,0))*float2(.5,-.5);
        float stride=(flags&4u)?1:2;
        int2 reprojected=int2(round(float2(
            RTPreviousTraceCoordinate(tp.x,-motion.x,width,stride,historyJitterDelta.x),
            RTPreviousTraceCoordinate(tp.y,-motion.y,height,stride,historyJitterDelta.y))));
        StructuredBuffer<GIReservoir> previous=ResourceDescriptorHeap[previousSrv];
        StructuredBuffer<GISurface> oldSurfaces=ResourceDescriptorHeap[previousSurfaceSrv];
        // RTXDI GI temporal: the reprojected tap first, then a few nearby taps.
        bool found=false; GISurface oldSurface=(GISurface)0; GIReservoir old=(GIReservoir)0;
        [loop] for(uint attempt=0;attempt<5 && !found;++attempt)
        {
            int2 q=reprojected;
            if(attempt>0) q+=int2(round((float2(RTRandom(rng),RTRandom(rng))*2-1)*(stride>1?1.5:2.5)));
            if(any(q<0) || any(q>=int2(traceW,traceH))) continue;
            uint prevIndex=q.y*traceW+q.x;
            oldSurface=oldSurfaces[prevIndex];
            found=Compatible(r,oldSurface);
            if(found) old=previous[prevIndex];
        }
        if(!found) Count(18);
        if(found)
        {
            // Coverage estimates geometry, independently of radiance selection.
            uint n=min(GISurfaceCount(oldSurface),31u);
            GISetCoverageCount(surface,(GISurfaceCoverage(surface)+GISurfaceCoverage(oldSurface)*n)/(n+1),n+1);
            // An empty history (its rays missed or ran out of range) is a population of genuine zeros:
            // it has no sample to shift, so no Jacobian and no ray. Do not feed its stored P (the zero
            // vector) to the Jacobian: that rejects most empty histories and biases sparse GI bright.
            const bool hasSample=old.W>0;
            float j=hasSample?GIJacobian(r.P,oldSurface.P,old):1;
            // Per-pixel age limit in [maxAge/2, maxAge] (RTXDI GI), so a sample that spatial reuse
            // copied into many reservoirs does not expire in all of them on one frame; removes
            // long-lived GI blotches in shadowed areas.
            const float ageJitter=RTRandom(rng);
            const uint ageCap=(flags&kGIFlagRandomAgeCap)?max(1u,uint(float(maxAge)*(.5+.5*ageJitter)+.5)):maxAge;
            bool keep=GIM(old)>0 && GIAge(old)<ageCap && (!hasSample || (j>.1 && j<10 && isfinite(j)));
            if(GIM(old)>0 && GIAge(old)>=ageCap) Count(20);
            else if(GIM(old)>0 && !keep) Count(19);
            j=GIClampJacobian(j);
            uint state=hasSample?kGIHistoryVanished:kGIHistoryOccluded;
            if(keep && hasSample) state=ValidateHistory(r,old,rng);
            if(keep && state==kGIHistoryVanished) { keep=false; Count(9); }
            if(keep && (flags&kGIFlagTemporalOneOverM))
            {
                // A/B: the plain 1/M merge (RTXDI's "Off").
                uint m=min(GIM(old),maxM);
                old.metadata=GIMetaKeep(m|((GIAge(old)+1)<<6),old);
                float target=state==kGIHistoryValid?GITarget(r.P,r.N,old,r.albedo):0;
                float sum=result.target*result.W;
                GISelect(result,sum,old,target,target*old.W*j*m,RTRandom(rng));
                uint total=1+m;
                result.W=result.target>0?sum/(total*result.target):0;
                result.metadata=GIMetaKeep(min(total,maxM)|(GIAge(result)<<6),result);
                Count(6);
            }
            else if(keep)
            {
                // Pairwise MIS between the fresh (canonical) reservoir and the history (RTXDI GI Basic with
                // ray-traced visibility). Plain 1/M gives the fresh sample 1/(1+m) of the weight even where
                // the history's receiver could never have produced it (behind its horizon, occluded from it,
                // outside the Jacobian leash), which darkens contacts and occluder edges while the camera
                // moves.
                uint m=min(GIM(old),maxM);
                old.metadata=GIMetaKeep(m|((GIAge(old)+1)<<6),old);
                const float target=state==kGIHistoryValid?GITarget(r.P,r.N,old,r.albedo):0;
                Receiver prev=r;
                prev.P=oldSurface.P; prev.N=GIUnpackNormal(oldSurface.normal);
                prev.Ng=GIUnpackNormal(oldSurface.geoNormal); prev.albedo=GIUnpackAlbedo(oldSurface.albedo);
                const float pc=result.target;
                float ij=result.W>0?GIJacobian(prev.P,r.P,result):0, inverse=0;
                if(ij>.1 && ij<10 && isfinite(ij))
                {
                    inverse=GITarget(prev.P,prev.N,result,prev.albedo);
                    if(inverse>0 && !VisibleFrom(prev,result.P)) inverse=0;
                }
                else ij=0;
                ij=GIClampJacobian(ij);
                const float own=hasSample?GITarget(prev.P,prev.N,old,prev.albedo):0;
                const float mc=RTPairCanonical(1,pc,m,inverse,ij,1,true);
                const float mh=RTPairNeighbour(m,own,1,target,j,1,true);
                GIReservoir merged=(GIReservoir)0; float sum=0;
                GISelect(merged,sum,old,target,mh*old.W*j*target,RTRandom(rng));
                GISelect(merged,sum,result,pc,mc*result.W*pc,RTRandom(rng));
                merged.W=merged.target>0?sum/merged.target:0;
                merged.metadata=GIMetaKeep(min(1+m,maxM)|(GIAge(merged)<<6),merged);
                result=merged;
                Count(6);
            }
        }
    }
    if(!isfinite(result.W)) { result.W=0; Count(5); }
    // Opt-in boiling filter (RTXDI_GIBoilingFilter, strength 0.2): drop a reservoir whose
    // contribution is over 41x its 8x8 group's non-zero average (empty, M=0). Off by default:
    // with sparse emitters those are legitimate samples (dropping them loses energy), and the
    // sparkles it targets come from the uniform many-light pick.
    const float boilWeight=inside && r.eligible && result.W>0 ? result.W*result.target : 0;
    gGIBoil[groupIndex]=boilWeight;
    GroupMemoryBarrierWithGroupSync();
    if((flags&kGIFlagBoilingFilter) && boilWeight>0)
    {
        float sum=0; uint n=0;
        [unroll] for(uint k=0;k<64;++k) { const float w=gGIBoil[k]; if(w>0) { sum+=w; ++n; } }
        if(n>1 && boilWeight>(sum/n)*41.0) { result=(GIReservoir)0; Count(13); }
    }
    if(inside) { output[index]=result; surfaces[index]=surface; }
}
[numthreads(8,8,1)]
void Spatial(uint3 tid:SV_DispatchThreadID)
{
    uint2 tp=tid.xy; if(any(tp>=uint2(traceW,traceH))) return;
    uint index=tp.y*traceW+tp.x; Receiver r=LoadReceiver(FullPixel(tp));
    StructuredBuffer<GIReservoir> input=ResourceDescriptorHeap[temporalSrv];
    StructuredBuffer<GISurface> surfaces=ResourceDescriptorHeap[surfaceSrv];
    RWStructuredBuffer<GIReservoir> output=ResourceDescriptorHeap[initialUav];
    GIReservoir canonical=input[index];
    if(!r.eligible || (flags&16u) || spatialRadius==0) { output[index]=canonical; return; }
    GIReservoir result=(GIReservoir)0;
    float sum=0,canonicalMIS=0,pc=GITarget(r.P,r.N,canonical,r.albedo);
    uint rng=HashLowbias32(index^HashLowbias32(frame)^0x461bcc91u)|1u;
    // Disocclusion boost (RTXDI): a reservoir with little or no history
    // (Temporal merges give M >= 2) takes boostSamples neighbours from twice
    // the radius, since its fresh neighbours are usually just as young.
    const bool fresh=!(flags&kGIFlagNoDisocclusionBoost) && (GIM(canonical)<=1 || GIM(canonical)*4<maxM);
    uint accepted=0,represented=GIM(canonical);
    const uint K=(flags&8192u) ? 1u : max(fresh?boostSamples:spatialSamples,1u);
    const float searchRadius=fresh?min(2.0*spatialRadius,32.0):float(spatialRadius);
    // Stratified disk with a per-pixel/frame random rotation. Drawn from this pass's hash, not
    // RTReservoirR2: the R2 index differs from the Trace direction's only by a per-tile
    // constant, which would make the neighbour set a function of the pixel's own fresh
    // direction.
    float2 rotation=float2(RTRandom(rng),RTRandom(rng));
    for(uint i=0;i<K;++i)
    {
        float angle=frac(rotation.x+i*0.61803398875)*2*PI;
        float radius=max(sqrt((i+rotation.y)/K)*searchRadius,1.0);
        int2 p=int2(tp)+int2(round(float2(cos(angle),sin(angle))*radius));
        bool valid=(flags&32u)==0 && all(p>=0) && all(p<int2(traceW,traceH)) && any(p!=int2(tp));
        GIReservoir neighbour=(GIReservoir)0; GISurface source=(GISurface)0;
        float j=0,ij=0,shifted=0,own=0,inverse=0;
        if(valid)
        {
            neighbour=input[p.y*traceW+p.x]; source=surfaces[p.y*traceW+p.x];
            // Pair validity is geometric. An empty reservoir on either side
            // still owns its M (genuine zeros): it has no sample to shift, so
            // it contributes no candidate and no Jacobian, but its population
            // must stay in the other side's MIS weight.
            valid=Compatible(r,source) && GIM(neighbour)>0;
            if(valid)
            {
                if(neighbour.W>0)
                {
                    j=GIJacobian(r.P,source.P,neighbour);
                    own=GITarget(source.P,GIUnpackNormal(source.normal),neighbour,GIUnpackAlbedo(source.albedo));
                    // A shift outside the Jacobian leash has no usable image.
                    if(j>.1 && j<10 && isfinite(j))
                        shifted=GITarget(r.P,r.N,neighbour,r.albedo);
                    j=GIClampJacobian(j);
                    // Target includes visibility from this receiver for the shifted sample: one ray per usable
                    // neighbour (a post-hoc validation would zero the whole pixel).
                    if(shifted>0 && !VisibleFrom(r,neighbour.P)) shifted=0;
                }
                if(canonical.W>0)
                {
                    ij=GIJacobian(source.P,r.P,canonical);
                    if(ij>.1 && ij<10 && isfinite(ij))
                        inverse=GITarget(source.P,GIUnpackNormal(source.normal),canonical,GIUnpackAlbedo(source.albedo));
                    else ij=0;
                    ij=GIClampJacobian(ij);
                    // The canonical sample seen from the neighbour's receiver. An occluded connection is a zero
                    // of the neighbour's target: the neighbour could never have produced this sample, so it must
                    // not take the canonical's MIS share. Without this test every pixel beside a shadow boundary
                    // gives away mass its shadowed neighbours cannot return, darkening (and hardening)
                    // penumbrae.
                    if(inverse>0 && !(flags&kGIFlagNoInverseVisibility))
                    {
                        Receiver neighbourReceiver=r;
                        neighbourReceiver.P=source.P; neighbourReceiver.N=GIUnpackNormal(source.normal);
                        neighbourReceiver.Ng=GIUnpackNormal(source.geoNormal);
                        if(!VisibleFrom(neighbourReceiver,canonical.P)) inverse=0;
                    }
                }
            }
        }
        canonicalMIS+=RTPairCanonical(GIM(canonical),pc,GIM(neighbour),inverse,ij,K,valid);
        float mi=RTPairNeighbour(GIM(neighbour),own,GIM(canonical),shifted,j,K,valid);
        if(valid)
        {
            represented+=GIM(neighbour); ++accepted; Count(7);
            GISelect(result,sum,neighbour,shifted,mi*neighbour.W*j*shifted,RTRandom(rng));
        }
    }
    if(accepted==0) { output[index]=canonical; return; }
    GISelect(result,sum,canonical,pc,canonicalMIS*canonical.W*pc,RTRandom(rng));
    result.W=result.target>0?sum/result.target:0;
    // The pooled population becomes the next frame's history (capped there).
    result.metadata=GIMetaKeep(min(represented,63u)|(GIAge(result)<<6),result);
    if(!isfinite(result.W)) { result.W=0; Count(5); }
    output[index]=result;
}
// Coverage is a geometric fraction estimated from initialCandidates Bernoulli rays a frame,
// and NRD never sees it, yet Lighting weights the DDGI + sky far field by (1 - coverage):
// undenoised it is the dominant GI noise on walls, worst under vertical camera motion.
// Cross-bilateral over compatible receivers (5x5 taps, stride 2), weighted by history count
// so a disoccluded receiver borrows its converged neighbours.
float GIFilteredCoverage(uint2 tp,Receiver r,StructuredBuffer<GISurface> surfaces)
{
    float sum=0,weight=0;
    [unroll] for(int y=-2;y<=2;++y) [unroll] for(int x=-2;x<=2;++x)
    {
        const int2 q=clamp(int2(tp)+int2(x,y)*2,0,int2(traceW,traceH)-1);
        const GISurface s=surfaces[q.y*traceW+q.x];
        if(!Compatible(r,s)) continue;
        const float w=float(min(GISurfaceCount(s),32u));
        sum+=GISurfaceCoverage(s)*w; weight+=w;
    }
    return weight>0?sum/weight:GISurfaceCoverage(surfaces[tp.y*traceW+tp.x]);
}
[numthreads(8,8,1)]
void Shade(uint3 tid:SV_DispatchThreadID)
{
    uint2 tp=tid.xy; if(any(tp>=uint2(traceW,traceH))) return;
    uint index=tp.y*traceW+tp.x; Receiver r=LoadReceiver(FullPixel(tp));
    StructuredBuffer<GIReservoir> input=ResourceDescriptorHeap[initialSrv];
    StructuredBuffer<GISurface> surfaces=ResourceDescriptorHeap[surfaceSrv];
    GIReservoir sample=input[index];
    float3 value=r.eligible?GIIntegrand(r.P,r.N,sample)*sample.W:0;
    if(!all(isfinite(value))) { value=0; Count(5); }
    float distance=sample.W>0?length(sample.P-r.P):0;
    RWTexture2D<float4> diffuse=ResourceDescriptorHeap[diffUav];
    // Opt-in, REBLUR only (-gi_nrd_reblur -gi_geometric_hitdist): REBLUR sizes
    // its kernel from the hit distance, and the selected sample is usually a
    // LONG connection (the light it found), which reads as "no nearby
    // occluder" and blurs contact shadows; this frame's own geometric ray
    // (written by Trace) carries the local occlusion instead. RELAX, the
    // default, reads hit distance only in its (disabled) pre-pass.
    // Blend in Trace's own estimate f(Y)W (still in DiffIn): both are unbiased,
    // and the fresh one is independent frame to frame, which NRD can average,
    // while the reservoir's error is correlated through reuse (RTXDI's final
    // shading MIS, 1/8 initial).
    const float4 initialSignal=diffuse[tp];
    if(freshBlend>0 && r.eligible) value=lerp(value,GIUnpackSignal(initialSignal),freshBlend);
    float4 packed=GIPackSignal(value,distance,r.viewZ);
    if(flags&kGIFlagGeometricHitDistance) packed.a=initialSignal.a;
    RWTexture2D<float> coverage=ResourceDescriptorHeap[coverageUav];
    diffuse[tp]=packed;
    coverage[tp]=r.eligible?((flags&kGIFlagNoCoverageFilter)?GISurfaceCoverage(surfaces[index]):GIFilteredCoverage(tp,r,surfaces)):0;
}
