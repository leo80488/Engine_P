// The ReSTIR GI shell's SHaRC radiance cache (shaders/SharcEngine.hlsli,
// Graphics/SharcCache.h).
//
// One TU with the shell: it includes RTGI.cs.hlsl with RTGI_SHARC, so the shell's constants
// (with their SHaRC tail), receivers (LoadReceiver), hit loading and Tier shading
// (RT_LocalLightsAtHit, IndirectAtHit, GISunAtHit) are exactly Trace's. RTGIPass also builds
// Trace and Temporal from this file: with RTGI_SHARC they are the shell's kernels with the
// cache query (RTGI_HitRadiance) at their hit shading.
//
//   RTGISharcClear    zero the cache (the first frame, a new world, a new mode)
//   RTGISharcUpdate   SHARC_UPDATE: one G-buffer-rooted path per
//                     sharcUpdateTile^2 render pixels deposits into it
//   RTGISharcResolve  this frame's deposits into each entry's history
// RTGIPass runs them in that order after the counter Clear and before Trace, which reads the
// resolved cache.
//
// HLSL 2018 + -enable-16bit-types (SharcTypes.h stores fp16 radiance;
// ShaderDiskCache::CompileFlags{false, true}, so the hot-reload recipe records the flag); the
// update's hash insert needs 64-bit atomics on descriptor-heap resources
// (RHI::DeviceCapabilities::int64HeapAtomics).
#define RTGI_SHARC 1
#include "RTGI.cs.hlsl"

[numthreads(256,1,1)]
void RTGISharcClear(uint3 tid:SV_DispatchThreadID)
{
    SHARC_ENGINE_CLEAR(tid.x,sharcHashUav,sharcAccumulationUav,sharcResolvedUav,sharcCapacity);
}

[numthreads(256,1,1)]
void RTGISharcResolve(uint3 tid:SV_DispatchThreadID)
{
    SHARC_ENGINE_RESOLVE(tid.x,GISharcParameters(),sharcPrevCamera,sharcAccumulationFrames,sharcStaleFrames,sharcFrame);
}

#if SHARC_UPDATE
// The sky a continuation segment sees, the path tracer's convention
// (PathTrace.cs.hlsl RTCameraEnvironment) without the sun's disk: every
// vertex's sun is its next-event term (GISunAtHit), never a BSDF hit.
float3 GISharcSky(float3 direction)
{
    if(skyIdx==0) return 0;
    TextureCube<float4> sky=ResourceDescriptorHeap[skyIdx];
    return sky.SampleLevel(gSampWrap,direction,0).rgb*environmentScale;
}
// A deposit the update's fixed-point accumulation can hold: uint(value x
// radianceScale) is added once per path that reaches the entry in a frame
// (SharcAddVoxelData), so a channel stays below sharcDepositMax = 2^32 /
// (radianceScale x 256) - 256 deposits per entry per frame before an overflow.
// A non-finite or clamped value is counted (slot 30).
float3 GISharcDeposit(float3 value,inout bool bad)
{
    if(!all(isfinite(value))) { bad=true; return 0; }
    if(any(value>sharcDepositMax)) { bad=true; value=min(value,sharcDepositMax); }
    return max(value,0);
}
// One path per sharcUpdateTile^2 render pixels, its pixel moving every frame (the PT core's
// update pattern, RtxdiPT.cs.hlsl): from that pixel's G-buffer receiver one cosine ray -
// Trace's distribution, so the cache is filled where the queries go - over the GI rays' mask
// and alpha rule and within rayTMax (a miss is the far field's, never the cache's). Each
// surface the cache may stand for deposits what its query will not add exactly:
//   parity (default): albedo (1 - metal) (local lights + DDGI) - the analytic
//                     Tier shading minus its emission and sun, so the cache's
//                     expectation equals Tier 1 and only the cost changes;
//   multi (kGISharcMulti): albedo (1 - metal) local lights, then up to
//                     sharcUpdateBounces - 1 cosine continuations (the hit's
//                     Lambertian lobe, throughput albedo (1 - metal)), a miss
//                     depositing the sky - no DDGI: the bounces are the cache's.
// The hit's emission and sun ride as SHaRC's separate emissive: carried back to the vertices
// before, never stored in the hit's own entry (its query adds them exactly). Characters
// (skinned), Unlit and untrusted surfaces end the path with no deposit (GISharcEligible). Its
// own random stream. Counters: 29 deposits, 30 bad radiance.
[numthreads(8,8,1)]
void RTGISharcUpdate(uint3 tid:SV_DispatchThreadID)
{
    const uint tile=max(sharcUpdateTile,1u);
    const uint2 tiles=(uint2(width,height)+tile-1)/tile;
    if(any(tid.xy>=tiles)) return;
    uint rng=HashLowbias32((tid.x+tid.y*tiles.x)^HashLowbias32(sharcFrame*0x9e3779b9u+0x2545f491u))|1u;
    const uint pick=min(uint(RTRandom(rng)*float(tile*tile)),tile*tile-1);
    const uint2 px=tid.xy*tile+uint2(pick%tile,pick/tile);
    if(any(px>=uint2(width,height))) return;
    const Receiver r=LoadReceiver(px);
    if(!r.eligible) return;
    float2 u=float2(RTRandom(rng),RTRandom(rng));
    float radius=sqrt(u.x),phi=2*PI*u.y;
    const float3 first=RTBasis(r.N,float3(radius*cos(phi),radius*sin(phi),sqrt(1-u.x)));
    if(dot(first,r.Ng)<=0) return;   // the receiver's own surface: no light reaches a hit there
    RayDesc ray; ray.Origin=r.P; ray.Direction=first; ray.TMin=GIRayTMin(r); ray.TMax=rayTMax;
    const SharcParameters params=GISharcParameters();
    SharcState state;
    SharcInit(state);
    const bool multi=(sharcFlags&kGISharcMulti)!=0;
    const uint segments=multi?max(sharcUpdateBounces,1u):1u;
    [loop] for(uint segment=0;segment<segments;++segment)
    {
        RayQuery<RAY_FLAG_NONE> q;
        q.TraceRayInline(g_TLAS,RAY_FLAG_NONE,0x01,ray);
        while(q.Proceed())
            if(q.CandidateType()==CANDIDATE_NON_OPAQUE_TRIANGLE && HR_AlphaAccept(
                q.CandidateInstanceID(),q.CandidatePrimitiveIndex(),q.CandidateTriangleBarycentrics(),false))
                q.CommitNonOpaqueTriangleHit();
        if(q.CommittedStatus()!=COMMITTED_TRIANGLE_HIT)
        {
            // The first segment's miss is the far field's share (coverage);
            // a continuation's is the sky the vertices before it see.
            if(segment>0)
            {
                bool bad=false;
                const float3 sky=GISharcDeposit(GISharcSky(ray.Direction),bad);
                if(bad) Count(30);
                SharcUpdateMiss(params,state,sky);
            }
            break;
        }
        const float distance=q.CommittedRayT();
        RTHitSurface s;
        if(!RTLoadHitSurface(q.CommittedInstanceID(),q.CommittedPrimitiveIndex(),q.CommittedTriangleBarycentrics(),
            q.CommittedObjectToWorld3x4(),q.CommittedWorldToObject3x4(),ray.Origin+ray.Direction*distance,true,0,s)) break;
        const bool facing=dot(s.Ngeo,-ray.Direction)>0;
        if(!facing && (s.flags&RT_INST_FLAG_DOUBLE_SIDED)==0) break;   // a single-sided back face: Trace's unresolved ray
        if(!facing) { s.Ngeo=-s.Ngeo; s.Nshade=-s.Nshade; }
        if(!GISharcEligible(s)) break;
        float3 irradiance=0;
        if(lightCount>0 && lightsIdx!=0) irradiance=RT_LocalLightsAtHit(s,1);
        if(!multi) irradiance+=IndirectAtHit(s);
        bool bad=false;
        const float3 direct=GISharcDeposit(s.albedo*(1-s.metal)*irradiance,bad);
        SharcHitData h;
        h.positionWorld=s.P; h.normalWorld=s.Ngeo;
        h.emissive=GISharcDeposit(GIHitEmission(s)+GISunAtHit(s),bad);
        if(bad) Count(30);
        Count(29);
        if(!SharcUpdateHit(params,state,h,direct,RTRandom(rng))) break;
        if(segment+1>=segments) break;
        u=float2(RTRandom(rng),RTRandom(rng));
        radius=sqrt(u.x); phi=2*PI*u.y;
        const float3 next=RTBasis(s.Nshade,float3(radius*cos(phi),radius*sin(phi),sqrt(1-u.x)));
        if(dot(next,s.Ngeo)<=0) break;
        SharcSetThroughput(state,s.albedo*(1-s.metal));
        ray.Origin=s.P+s.Ngeo*(1e-3+1e-4*distance);
        ray.Direction=next;
        ray.TMin=1e-3;
        ray.TMax=1000;
    }
}
#endif
