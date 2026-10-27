#include "bsdf_sample.hlsli"
#include "material.hlsli"
#include "core/depth.hlsli"   // LinearizeReverseZ, WorldPosFromDepth
#include "core/gbuffer_codec.hlsli"   // GBufferDecodeMatIdx
#include "shading_model_table.hlsli"   // which models are DI receivers
#include "rt_instance_data.hlsli"
#include "rt_alpha_record.hlsli"
#include "light_eval.hlsli"
#include "world_light_grid.hlsli"
#include "restir_reservoir.hlsli"
#include "restir_reprojection.hlsli"
#define NRD_NORMAL_ENCODING 2
#define NRD_ROUGHNESS_ENCODING 1
#include "NRD.hlsli"
cbuffer Params : register(b0)
{
    float4x4 invViewProj,worldToView;
    float3 camera; float rayTMax;
    uint width,height,traceW,traceH;
    uint depthIdx,normalIdx,albedoIdx,surfaceIdx;
    uint extraIdx,velocityIdx,materialsIdx,lightsIdx;
    uint worldGrid,lightCount,alphaInstanceCount,frame;
    uint diffUav,specUav,normalUav,viewZUav;
    uint mvUav,diffSrv,specSrv,flags;
    uint listUav,listSrv,counterUav,argsUav;
    uint candidates,alphaTestEnabled,stageIndex,confidenceUav;
    uint initialUav,initialSrv,previousSrv,temporalUav;
    uint temporalSrv,previousSurfaceSrv,surfaceUav,historyReset;
    uint maxM,spatialRadius,historyWidth,historyHeight;
    uint rawSpecSrv; float mirrorRoughness; uint spatialCandidates,maxAge;
    float2 historyJitterDelta; uint confidenceSrv,areaLite;
    // This frame's 2x2 trace phase (x | y<<1), rotating through four phases so temporal reuse
    // and TAA see every full-resolution pixel.
    uint tracePhase, flags2;
    // The previous -> current light index map (ClusterPass) and the previous list's length.
    // lightMapIdx 0 = the list kept its keys and order: nothing to translate.
    uint lightMapIdx, prevLightCount;
};
// flags2 (all 32 bits of flags are taken):
//   bit 0 = low-weight Resolve fallback (alpha 0 -> clustered loop),
//   bit 1 = geometric-normal ray origin + horizon (depth-reconstructed Ng).
static const uint kDIFlags2ResolveFallback=1u<<0;
static const uint kDIFlags2GeometricOrigin=1u<<1;
// bit 2 = A/B only (-di_confidence_require_valid): the stricter confidence rules - retest
// and cooldown only for a valid history sample, no canonical carry in Spatial.
static const uint kDIFlags2ConfidenceRequireValid=1u<<2;
// bit 3: under ray reconstruction (no NRD) Resolve writes the selected light's distance
// into slice 1's alpha - RRGuidePass's specular hit distance; at half resolution the pixel's
// 2x2 cell's. Lighting reads only slice 1's rgb.
static const uint kDIFlags2RRDistance=1u<<3;
// flags2 bits 8-15 = the DI specular AA scale x4 (DISpecularAAScale; 0 = off).
// The bits below are in flags. Bit 21 samples diffuse lighting without albedo and applies
// albedo at resolve; bit 20 is a floor-removal diagnostic. Both stay opt-in until the energy
// and temporal-quality gates qualify them.
// Reuse-chain bits (set by RTDIPass): 25 = a spatial pass follows and owns the
// selected-sample visibility ray, 26 = discard occluded selections from the reservoir that
// is carried forward (RTXDI discardIfInvisible), 28 = RTXDI boiling filter on the temporal
// output.
static const uint kDIFlagSpatialTests=1u<<25;
static const uint kDIFlagDiscardInvisible=1u<<26;
static const uint kDIFlagBoilingFilter=1u<<28;
// 29 = RTXDI enableInitialVisibility: the Initial winner is shadow-tested and,
// when occluded, removed while its M=1 is kept. Temporal reuse then resamples
// an effectively visibility-aware target instead of carrying occluded lights.
static const uint kDIFlagInitialVisibility=1u<<29;
// 30 = RTXDI permutation sampling: the first temporal tap is XOR-permuted
// inside a 4x4 block by a per-frame constant, so a pixel's history comes from
// a different (compatible) neighbour each frame.
static const uint kDIFlagPermutation=1u<<30;
// 31 = visibility-aware reuse. Every reservoir only ever holds a sample that is
// visible from its OWN receiver (Initial visibility; each merge selects only
// visible candidates), so the target of all resampling is p_hat*V. Pairwise
// MIS then needs the cross-evaluations p_c(y_i)*V_c(y_i) and p_i(y_c)*V_i(y_c)
// by ray; the selected sample is visible by construction, so no final ray.
// This is what removes multi-light shadow noise: unshadowed-target reuse keeps
// picking occluded lights and shades 0 / 2x no matter how much it reuses.
// 15 = also trace V_i(y_c) for the spatial inverse half (unbiased canonical
// weight at shadow boundaries; off = assume visible, K fewer rays).
static const uint kDIFlagVisibilityMIS=1u<<31;
static const uint kDIFlagInverseVisibility=1u<<15;
// 27 = the denoiser is RELAX (world hit distance, identity pack) not REBLUR.
static const uint kDIFlagRelax=1u<<27;
// ExecuteIndirect args are 2-D so 4K full resolution stays under 65535 groups.
static const uint kDIGroupsPerRow=65535u;
uint CompactLane(uint3 tid,uint3 gid) { return gid.y*(kDIGroupsPerRow*64u)+tid.x; }
void DIMaterialFactors(float3 N,float3 V,float3 albedo,float3 F0,float rough,
    out float3 df,out float3 sf)
{
    if(flags&(1u<<21))
    {
        // The sampled diffuse integrand excludes albedo, including zero
        // channels. Keep NRD's stable lobe floor; apply albedo only at resolve.
        NRD_MaterialFactors(N,V,1,F0,rough,df,sf);
        return;
    }
    NRD_MaterialFactors(N,V,albedo,F0,rough,df,sf);
    if(flags&(1u<<20))
        df=max((df-NRD_MATERIAL_FACTOR_MIN_SCALE)/(1-NRD_MATERIAL_FACTOR_MIN_SCALE),1e-6);
}
RaytracingAccelerationStructure g_TLAS : register(t0);
StructuredBuffer<RTInstanceData> g_Instances : register(t1);
StructuredBuffer<RTAlphaRecord> g_AlphaRecords : register(t2);
ByteAddressBuffer g_Buffers[16384] : register(t0,space1);
Texture2D g_AllTextures[16384] : register(t0,space2);
SamplerState gSampWrap : register(s1);
RWTexture2DArray<float4> Result : register(u0);
typedef RTInstanceData HRInstanceData;
#define HR_MaterialUV RTInstanceUV
#include "rt_hit_fetch.hlsli"
// flags & 256: a diagnostic luminance-weighted alternative to the L1 target (DITarget). Both
// are positive scalar targets for the same vector integrand; proposal and reconstruction
// stay paired.
float LightTarget(float3 diffuse,float3 specular)
{
    float3 value=max(diffuse+specular,0);
    return (flags&256u)?dot(value,float3(.25,.5,.25)):DITarget(diffuse,specular);
}
struct Receiver
{
    float3 P,N,V,albedo,F0;
    float rough,metal,viewZ;
    uint material;
    bool eligible;
    // Full-resolution pixel of a current-frame receiver (hasPixel); a history
    // receiver rebuilt from a stored position has none and keeps N as Ng.
    uint2 pixel;
    bool hasPixel;
    // Geometric normal, reconstructed once per receiver (hasNg) by the passes that trace from
    // it; TraceVisibility falls back to DIGeometricNormal.
    float3 Ng;
    bool hasNg;
};
// The GBuffer receiver as stored. Resolve's neighbour gather reads only N, P
// and eligibility from it, so it skips the specular AA below.
Receiver LoadReceiverBase(uint2 px)
{
    Receiver r=(Receiver)0;
    Texture2D<float> depths=ResourceDescriptorHeap[depthIdx];
    float depth=depths.Load(int3(px,0));
    if (depth<=0) return r;
    Texture2D<float4> normals=ResourceDescriptorHeap[normalIdx];
    Texture2D<float4> albedos=ResourceDescriptorHeap[albedoIdx];
    Texture2D<float4> surfaces=ResourceDescriptorHeap[surfaceIdx];
    Texture2D<float4> extras=ResourceDescriptorHeap[extraIdx];
    StructuredBuffer<MaterialGPUData> materials=ResourceDescriptorHeap[materialsIdx];
    float4 normal=normals.Load(int3(px,0)), extra=extras.Load(int3(px,0));
    uint mat=GBufferDecodeMatIdx(normal.a);
    uint model=materials[mat].customShadingModel; r.material=mat;
    // The models: the shading-model table's `di` column (Lighting.ps folds by
    // the same rule). Isotropic here: a face pixel (RT4.a marker; its .r/.g
    // are not model data) or a zero anisotropy in RT4.g.
    r.eligible=ShadingModelDiReceives(model, extra.a>0.5 || extra.g==0);
    float2 uv=(float2(px)+0.5)/float2(width,height);
    r.P=WorldPosFromDepth(uv,depth,invViewProj); r.N=normalize(normal.xyz*2-1); r.V=normalize(camera-r.P);
    r.viewZ=abs(mul(float4(r.P,1),worldToView).z);
    float4 surface=surfaces.Load(int3(px,0));
    r.albedo=albedos.Load(int3(px,0)).rgb;
    r.rough=max(surface.r,0.045); r.metal=surface.g;
    r.F0=lerp((0.16*surface.a*surface.a).xxx,r.albedo,r.metal);
    r.pixel=px; r.hasPixel=true;
    return r;
}
// DI specular AA (flags2 bits 8-15 = scale x4; 0 = off). The GBuffer roughness
// already carries the raster pass's one-pixel geometric specular AA
// (MS_GeometricSpecularAA). A half-resolution DI texel shades a whole 2x2
// block, and under TAA/DLSS jitter a sub-pixel lamp highlight on a thin,
// curved, glossy surface (chair tubes, tile bevels) swings 10-1500x between
// frames; RELAX keeps each spike for 10-25 frames, which is the highlight
// flicker. Widen the lobe by the normal variance across the aligned 2x2 block
// with the raster pass's constants, scaled by the extra footprint. Only
// same-material, non-sky block pixels count, so a silhouette against another
// object does not roughen the floor beside it. All four pixels of a block get
// the same kernel, so Shade's demodulation (at the trace pixel) and Resolve's
// full-resolution remodulation agree.
float DISpecularAAScale() { return float((flags2>>8)&255u)*0.25; }
void DIApplySpecularAA(inout Receiver r,uint2 px)
{
    const float scale=DISpecularAAScale();
    if(!(scale>0) || !r.hasPixel) return;
    Texture2D<float> depths=ResourceDescriptorHeap[depthIdx];
    Texture2D<float4> normals=ResourceDescriptorHeap[normalIdx];
    const uint2 block=min(px&~1u,uint2(max(width,2u),max(height,2u))-2u);
    float3 n[4];
    [unroll] for(uint i=0;i<4;++i)
    {
        const uint2 q=block+uint2(i&1u,i>>1);
        const float4 v=normals.Load(int3(q,0));
        const bool same=depths.Load(int3(q,0))>0 && GBufferDecodeMatIdx(v.a)==r.material;
        n[i]=same?normalize(v.xyz*2-1):r.N;
    }
    const float3 dndu=.5*((n[1]-n[0])+(n[3]-n[2]));
    const float3 dndv=.5*((n[2]-n[0])+(n[3]-n[1]));
    const float kSigma2=.25, kKappa=.18;
    const float kernelR2=min(2*kSigma2*scale*(dot(dndu,dndu)+dot(dndv,dndv)),kKappa);
    r.rough=sqrt(saturate(r.rough*r.rough+kernelR2));
}
Receiver LoadReceiver(uint2 px)
{
    Receiver r=LoadReceiverBase(px);
    DIApplySpecularAA(r,px);
    return r;
}
// At half resolution the trace texel's receiver rotates through the four pixels of its 2x2
// block (tracePhase, set per frame by RTDIPass); the CPU folds the phase change into
// historyJitterDelta and NRD's jitter.
uint2 TracePhase() { return (flags&4u)?uint2(0,0):uint2(tracePhase&1u,(tracePhase>>1)&1u); }
uint2 FullPixel(uint2 tracePixel) { return min(tracePixel*((flags&4u)?1u:2u)+TracePhase(),uint2(width,height)-1); }
// Geometric normal from the depth buffer (DIGeometricNormal). Per axis the one-sided
// difference toward the neighbour nearer in view depth, so it never spans a silhouette;
// degenerate ones fall back to the shading normal.
float3 DIWorldPosition(int2 q,out float depth)
{
    Texture2D<float> depths=ResourceDescriptorHeap[depthIdx];
    depth=depths.Load(int3(q,0));
    float2 uv=(float2(q)+.5)/float2(width,height);
    return WorldPosFromDepth(uv,depth,invViewProj);
}
float DIViewDepth(float3 p) { return mul(float4(p,1),worldToView).z; }
float3 DIGeometricNormal(Receiver r)
{
    if(!(flags2&kDIFlags2GeometricOrigin) || !r.hasPixel) return r.N;
    const int2 px=int2(r.pixel);
    float d0,d1,d2,d3;
    const int2 hi=int2(width,height)-1;
    const float3 xp=DIWorldPosition(min(px+int2(1,0),hi),d0), xm=DIWorldPosition(max(px-int2(1,0),0),d1);
    const float3 yp=DIWorldPosition(min(px+int2(0,1),hi),d2), ym=DIWorldPosition(max(px-int2(0,1),0),d3);
    const bool bxp=d0>0 && px.x<hi.x, bxm=d1>0 && px.x>0, byp=d2>0 && px.y<hi.y, bym=d3>0 && px.y>0;
    if(!(bxp||bxm) || !(byp||bym)) return r.N;
    const float z=DIViewDepth(r.P);
    const bool useXp=bxp && (!bxm || abs(DIViewDepth(xp)-z)<=abs(DIViewDepth(xm)-z));
    const bool useYp=byp && (!bym || abs(DIViewDepth(yp)-z)<=abs(DIViewDepth(ym)-z));
    float3 g=cross(useYp?yp-r.P:r.P-ym,useXp?xp-r.P:r.P-xm);
    if(!(dot(g,g)>1e-24)) return r.N;
    g=normalize(g);
    if(dot(g,camera-r.P)<0) g=-g;
    return dot(g,r.N)>.05?g:r.N;
}
// A receiver the pass will trace visibility rays from: Ng once, here.
Receiver LoadTracingReceiver(uint2 px)
{
    Receiver r=LoadReceiver(px);
    if(r.eligible && (flags2&kDIFlags2GeometricOrigin)) { r.Ng=DIGeometricNormal(r); r.hasNg=true; }
    return r;
}
// Denoiser front/back ends. The raw (-di_no_nrd) path unpacks the front-end
// packing with the same pair, so both denoisers stay energy-identical raw.
float4 DIPackSignal(float3 radiance,float distance,float viewZ,float rough)
{
    if(flags&kDIFlagRelax) return RELAX_FrontEnd_PackRadianceAndHitDist(radiance,distance,true);
    float hit=REBLUR_FrontEnd_GetNormHitDist(distance,viewZ,float3(rayTMax,0,1),rough);
    return REBLUR_FrontEnd_PackRadianceAndNormHitDist(radiance,hit,false);
}
float3 DIUnpackSignal(float4 value)
{
    return (flags&kDIFlagRelax)?RELAX_BackEnd_UnpackRadiance(value).rgb:REBLUR_BackEnd_UnpackRadianceAndNormHitDist(value).rgb;
}
void Count(uint index)
{
    RWStructuredBuffer<uint> counter=ResourceDescriptorHeap[counterUav];
    uint n=WaveActiveCountBits(true);
    if (WaveIsFirstLane()) InterlockedAdd(counter[index],n);
}
float TraceVisibility(Receiver r,float3 l,float distance,uint counterIndex);
void ApplyLightPoint(GPULight light,float3 P,uint seed,inout float3 l,inout float distance);
bool TranslateHistory(inout DIReservoir r,bool count);
bool LightExistedPreviously(uint index);
bool ReachedByRemovedLight(float3 P);
[numthreads(1,1,1)]
void Clear()
{
    RWStructuredBuffer<uint> counter=ResourceDescriptorHeap[counterUav];
    for (uint i=0;i<31;++i) counter[i]=0; // mirror: RTDIPass::CounterCount
}
[numthreads(8,8,1)]
void Classify(uint3 tid:SV_DispatchThreadID)
{
    uint2 tp=tid.xy;
    if (any(tp>=uint2(traceW,traceH))) return;
    uint2 px=FullPixel(tp);
    Receiver r=LoadReceiver(px);
    if(flags&512u)
    {
        // NRD consumes confidence in PREVIOUS-frame coordinates and reprojects it
        // itself. Read the previous temporal reservoir at this coordinate, never
        // the spatial output or a second motion-vector reprojection.
        // Without reuse there is no reservoir history to describe; report the
        // NRD history as trustworthy instead of silently disabling accumulation.
        float confidence=(flags&8u)?0:1;
        if(!historyReset && (flags&8u))
        {
            StructuredBuffer<DIPacked> previous=ResourceDescriptorHeap[previousSrv];
            float3 previousNormal;
            DIReservoir history=DIUnpack(previous[tp.y*traceW+tp.x],previousNormal);
            // Translate last frame's light index into today's list.
            const bool lightGone=TranslateHistory(history,false);
            // A discarded (occluded) selection still represents its M frames.
            confidence=saturate(float(history.M)/max(maxM,1u));
            if((flags&(1u<<24)) && history.reactiveFrames>0) confidence=0;
            // The light this texel's history was lit by is gone: NRD's
            // history of it is wrong now (a removed lamp must not fade out).
            if(lightGone) confidence=0;
            // Retest whenever the texel holds a tested light, valid or not: visibility-aware reuse
            // stores an occluded selection as invalid (Initial visibility, the merges), so requiring
            // `valid` would never see a shadow move off a surface.
            if((flags&(1u<<24)) && history.visibilityTested && history.lightIndex<lightCount
                && (history.valid || !(flags2&kDIFlags2ConfidenceRequireValid)))
            {
                // This texel describes the PREVIOUS receiver, not today's
                // surface at tp. Retest that receiver's selected light against
                // today's TLAS so moving shadows invalidate NRD history even
                // when depth, normal and motion all remain unchanged.
                StructuredBuffer<uint4> previousSurfaces=ResourceDescriptorHeap[previousSurfaceSrv];
                StructuredBuffer<GPULight> lights=ResourceDescriptorHeap[lightsIdx];
                Receiver oldReceiver=(Receiver)0;
                oldReceiver.P=asfloat(previousSurfaces[tp.y*traceW+tp.x].xyz);
                oldReceiver.N=previousNormal;
                float3 toLight=lights[history.lightIndex].position-oldReceiver.P;
                float distance=length(toLight);
                if(distance>.002 && all(isfinite(oldReceiver.P)))
                {
                    float3 l=toLight/distance;
                    // The same point on the emitter the sample was tested at.
                    ApplyLightPoint(lights[history.lightIndex],oldReceiver.P,history.pathSeed,l,distance);
                    bool visible=TraceVisibility(oldReceiver,l,distance,24)>0;
                    if(visible!=history.visible) { confidence=0; Count(25); }
                }
                else confidence=0;
            }
        }
        if(flags&2048u) confidence=1;
        if(flags&1024u) confidence=0;
        RWTexture2D<float> confidences=ResourceDescriptorHeap[confidenceUav];
        confidences[tp]=historyReset?0:confidence;
    }
    if(flags&8u)
    {
        uint pixel=tp.y*traceW+tp.x;
        RWStructuredBuffer<DIPacked> initial=ResourceDescriptorHeap[initialUav];
        RWStructuredBuffer<DIPacked> temporal=ResourceDescriptorHeap[temporalUav];
        RWStructuredBuffer<uint4> receiver=ResourceDescriptorHeap[surfaceUav];
        initial[pixel]=(DIPacked)0; temporal[pixel]=(DIPacked)0;
        receiver[pixel]=uint4(asuint(r.P),r.material);
    }
    RWTexture2D<float4> diffuse=ResourceDescriptorHeap[diffUav];
    RWTexture2D<float4> specular=ResourceDescriptorHeap[specUav];
    RWTexture2D<float4> normal=ResourceDescriptorHeap[normalUav];
    RWTexture2D<float> viewZ=ResourceDescriptorHeap[viewZUav];
    RWTexture2D<float2> mv=ResourceDescriptorHeap[mvUav];
    Texture2D<float2> velocity=ResourceDescriptorHeap[velocityIdx];
    diffuse[tp]=0; specular[tp]=0;
    normal[tp]=NRD_FrontEnd_PackNormalAndRoughness(r.eligible?r.N:float3(0,0,1),r.eligible?r.rough:1,0);
    viewZ[tp]=r.eligible?r.viewZ:1e7;
    mv[tp]=r.eligible?velocity.Load(int3(px,0))*float2(-0.5,0.5):0;
    if (r.eligible)
    {
        RWStructuredBuffer<uint> list=ResourceDescriptorHeap[listUav];
        RWStructuredBuffer<uint> counter=ResourceDescriptorHeap[counterUav];
        uint n=WaveActiveCountBits(true), base=0;
        if (WaveIsFirstLane()) InterlockedAdd(counter[0],n,base);
        base=WaveReadLaneFirst(base);
        list[base+WavePrefixCountBits(true)]=tp.y*traceW+tp.x;
    }
}
[numthreads(1,1,1)]
void Prepare()
{
    RWStructuredBuffer<uint> counter=ResourceDescriptorHeap[counterUav];
    RWStructuredBuffer<uint> args=ResourceDescriptorHeap[argsUav];
    uint groups=(counter[0]+63)/64;
    args[0]=min(groups,kDIGroupsPerRow); args[1]=max((groups+kDIGroupsPerRow-1)/kDIGroupsPerRow,1u); args[2]=1;
}
void EvaluateLight(Receiver r, GPULight light, out float3 diffuse, out float3 specular,
                   out float3 l, out float distance)
{
    diffuse=specular=0; l=float3(0,0,1); distance=0;
    if (light.type==0) return;
    float3 toLight=light.position-r.P;
    distance=length(toLight);
    if (distance<=0.002 || distance>=light.radius) return;
    l=toLight/distance;
    float ndl=saturate(dot(r.N,l));
    if (ndl<=0) return;
    float3 spec,kd;
    EvalCookTorrance(r.N,r.V,l,r.F0,r.rough,r.metal,r.albedo,spec,kd);
    // Pass the normalised vector: EvalLocalLightAttenuation's cone test (light_eval.hlsli)
    // widens spot cones with distance when given the raw offset.
    float3 energy=light.color*light.intensity*EvalLocalLightAttenuation(light,l,distance)*ndl;
    // Use this same integrand for candidate targets, every shifted target and
    // shading. Dividing colored radiance after selection loses support at zero
    // albedo; reconstruct the physical diffuse only after spatial filtering.
    float3 diffuseAlbedo=(flags&(1u<<21))?1:r.albedo;
    diffuse=kd*diffuseAlbedo/PI*energy; specular=spec*energy;
}
float TraceVisibility(Receiver r, float3 l, float distance,uint counterIndex)
{
    if (flags&2u) return 1;
    RayDesc ray; ray.Direction=l;
    if(flags2&kDIFlags2GeometricOrigin)
    {
        // The origin is the receiver lifted along its geometric normal by a view-scaled offset
        // (raster uses a slope-scaled, texel-sized bias); a light below the geometric horizon is
        // shadowed by the surface itself instead of tracing into the neighbouring triangles
        // (terminator acne).
        const float3 Ng=r.hasNg?r.Ng:DIGeometricNormal(r);
        if(dot(Ng,l)<=0) return 0;
        const float lift=1e-3+1e-4*r.viewZ;
        ray.Origin=r.P+Ng*lift; ray.TMin=1e-4+1e-5*r.viewZ;
        ray.TMax=min(rayTMax,max(distance-lift-.002,ray.TMin+1e-4));
    }
    else
    {
        ray.Origin=r.P+r.N*.001; ray.TMin=.001;
        ray.TMax=min(rayTMax,max(distance-.002,.0011));
    }
    Count(counterIndex);
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(g_TLAS,RAY_FLAG_NONE,0x02,ray);
    while(q.Proceed())
        if (q.CandidateType()==CANDIDATE_NON_OPAQUE_TRIANGLE &&
            HR_AlphaAccept(q.CandidateInstanceID(),q.CandidatePrimitiveIndex(),q.CandidateTriangleBarycentrics(),false))
            q.CommitNonOpaqueTriangleHit();
    return q.CommittedStatus()==COMMITTED_NOTHING?1.0:0.0;
}
float Visibility(Receiver r,float3 l,float distance) { return TraceVisibility(r,l,distance,1); }
// The point a visibility ray aims at. A punctual light (sourceRadius 0) keeps the centre
// direction and distance the caller passes in; an emitter of radius R gets a point on its
// disk facing the receiver, fixed by the sample's seed, so every test of one sample
// (Initial, the merges, the confidence retest next frame) aims at the same point.
void ApplyLightPoint(GPULight light,float3 P,uint seed,inout float3 l,inout float distance)
{
    const float R=asfloat(light.sourceRadiusBits);
    if(!(R>0) || !(distance>R)) return;
    // The seed's two 16-bit halves are the point: RTXDI's packed uv, so a sample converts
    // between the engine and RTXDI reservoirs exactly. The seeds are hashes already (Initial),
    // uniform in both halves.
    const float2 xi=float2(seed&0xffffu,seed>>16)*(1.0/65536.0);
    const float3 w=l;
    const float3 t=normalize(abs(w.y)<.999?cross(w,float3(0,1,0)):cross(w,float3(1,0,0)));
    const float3 b=cross(w,t);
    const float radius=R*sqrt(xi.x), phi=6.28318530718*xi.y;
    const float3 toPoint=light.position+radius*(cos(phi)*t+sin(phi)*b)-P;
    distance=length(toPoint);
    l=toPoint/distance;
}
float SampleVisibility(Receiver r,GPULight light,uint seed,float3 l,float distance,uint counterIndex)
{
    ApplyLightPoint(light,r.P,seed,l,distance);
    return TraceVisibility(r,l,distance,counterIndex);
}
// TranslateHistory: last frame's light index in today's list (lightMapIdx 0 = the identity).
// Returns true when the light is gone; then the whole history goes, M too. Kept as M
// zero-contribution samples it would claim under pairwise MIS that it could have produced
// every fresh sample: the fresh one would get 1/(M+1) of its weight and the receiver would go
// dark, recovering at (M/(M+1))^n (tools/verify_restir_light_swap.py).
static const uint kLightMapCurrentOffset=4096u; // mirror: ClusterPass::kMaxLights
static const uint kLightMapRemovedOffset=8192u; // mirror: ClusterPass::kRemovedOffset
bool TranslateHistory(inout DIReservoir r,bool count)
{
    if(lightMapIdx==0) return false;
    uint translated=~0u;
    if(r.lightIndex<prevLightCount)
    {
        StructuredBuffer<uint> map=ResourceDescriptorHeap[lightMapIdx];
        translated=map[r.lightIndex];
    }
    if(translated<lightCount) { r.lightIndex=translated; return false; }
    if(count && r.valid) Count(30);
    r.valid=false; r.weightSum=0; r.lightIndex=0xffffu; r.M=0;
    return true;
}
// Whether today's light `index` existed last frame. A light that did not cannot have been
// produced by the history: its fresh samples take their whole MIS weight (a new lamp does
// not fade in over M frames).
bool LightExistedPreviously(uint index)
{
    if(lightMapIdx==0) return true;
    StructuredBuffer<uint> map=ResourceDescriptorHeap[lightMapIdx];
    return map[kLightMapCurrentOffset+index]!=~0u;
}
// Whether a light removed this frame could have lit a receiver at P (its attenuation is
// exactly 0 at its radius). A history there, even one holding a light that still exists, was
// normalized over a target that included the removed light: kept, it overestimates (~+25%),
// so it is dropped like one of a removed light.
bool ReachedByRemovedLight(float3 P)
{
    if(lightMapIdx==0) return false;
    StructuredBuffer<uint> map=ResourceDescriptorHeap[lightMapIdx];
    const uint count=map[kLightMapRemovedOffset];
    if(count==~0u) return true;
    [loop] for(uint i=0;i<count;++i)
    {
        const uint b=kLightMapRemovedOffset+4+4*i;
        const float3 d=P-asfloat(uint3(map[b],map[b+1],map[b+2]));
        const float radius=asfloat(map[b+3]);
        if(dot(d,d)<radius*radius) return true;
    }
    return false;
}
[numthreads(64,1,1)]
void Initial(uint3 tid:SV_DispatchThreadID,uint3 gid:SV_GroupID)
{
    RWStructuredBuffer<uint> counter=ResourceDescriptorHeap[counterUav];
    uint lane=CompactLane(tid,gid);
    if (lane>=counter[0]) return;
    StructuredBuffer<uint> list=ResourceDescriptorHeap[listSrv];
    uint pixel=list[lane]; uint2 tp=uint2(pixel%traceW,pixel/traceW);
    Receiver r=LoadTracingReceiver(FullPixel(tp));
    StructuredBuffer<GPULight> lights=ResourceDescriptorHeap[lightsIdx];
    uint offset; uint count=WorldLightRange(worldGrid,r.P,lightCount,offset);
    if (count==0) return;
    // The grid keeps the proposal view-independent. Compute its normalizer
    // once, then use the same discrete PDF for all eight RIS candidates.
    float powerSum=0;
    for (uint i=0;i<count;++i)
        powerSum+=max(0,asfloat(lights[WorldLightIndex(worldGrid,offset,i)].powerBits));
    uint rng=HashLowbias32(pixel^HashLowbias32(frame)^0x164b39d5u);
    if (rng==0) rng=1;
    DIReservoir reservoir; DIReset(reservoir);
    float2 proposalRotation=RTReservoirR2(tp,frame,0x41u);
    for (uint k=0;k<candidates;++k)
    {
        Count(4);
        uint lightIndex;
        float proposal;
        // One candidate per CDF stratum; the mixture PDF remains the full
        // power distribution. Reservoir selection uses an independent stream.
        float u=(k+proposalRotation.x)/candidates;
        if (powerSum>0)
        {
            float remaining=u*powerSum;
            lightIndex=WorldLightIndex(worldGrid,offset,count-1);
            for (uint j=0;j<count;++j)
            {
                uint index=WorldLightIndex(worldGrid,offset,j);
                float power=max(0,asfloat(lights[index].powerBits));
                if (power>0) lightIndex=index;
                remaining-=power;
                if (remaining<0) break;
            }
            proposal=max(0,asfloat(lights[lightIndex].powerBits))/powerSum;
        }
        else
        {
            lightIndex=WorldLightIndex(worldGrid,offset,min(uint(u*count),count-1));
            proposal=1.0/count;
        }
        float3 d,s,l; float distance;
        EvaluateLight(r,lights[lightIndex],d,s,l,distance);
        // The candidate's light-point seed: a hash, so no stream shifts.
        const uint seed=HashLowbias32(pixel*0x9e3779b9u^frame*0x85ebca6bu^k*0xc2b2ae35u);
        DIStreamSeeded(reservoir,lightIndex,seed,LightTarget(d,s),proposal,RTRandom(rng));
    }
    if(flags&8u)
    {
        // RTXDI InitialSampling: finalize the candidate RIS, then let the result stand for one
        // sample. M counts frames from here on, so an M cap of 20 means twenty frames of history.
        float W=DIWeight(reservoir);
        reservoir.M=1;
        reservoir.weightSum=reservoir.valid?W*reservoir.target:0;
        // pathSeed: the selected candidate's light-point seed (DIStreamSeeded).
        if((flags&kDIFlagInitialVisibility) && reservoir.valid)
        {
            float3 d,s,l; float distance;
            EvaluateLight(r,lights[reservoir.lightIndex],d,s,l,distance);
            reservoir.visible=SampleVisibility(r,lights[reservoir.lightIndex],reservoir.pathSeed,l,distance,1)>0;
            reservoir.visibilityTested=true;
            if(!reservoir.visible) { reservoir.valid=false; reservoir.weightSum=0; Count(7); }
        }
        RWStructuredBuffer<DIPacked> initial=ResourceDescriptorHeap[initialUav];
        initial[pixel]=DIPack(reservoir,r.N);
        return;
    }
    if (!reservoir.valid) return;
    Count(2);
    float3 d,s,l; float distance;
    EvaluateLight(r,lights[reservoir.lightIndex],d,s,l,distance);
    float rtDiVisibility=SampleVisibility(r,lights[reservoir.lightIndex],reservoir.pathSeed,l,distance,1);
    float weight=DIWeight(reservoir)*rtDiVisibility;
    float3 df,sf;
    DIMaterialFactors(r.N,r.V,r.albedo,r.F0,r.rough,df,sf);
    d=d*weight/df; s=s*weight/sf;
    if (!all(isfinite(d)) || !all(isfinite(s))) { Count(3); d=s=0; }
    RWTexture2D<float4> diffuse=ResourceDescriptorHeap[diffUav];
    RWTexture2D<float4> specular=ResourceDescriptorHeap[specUav];
    diffuse[tp]=DIPackSignal(d,distance,r.viewZ,r.rough);
    specular[tp]=DIPackSignal(s,distance,r.viewZ,r.rough);
}
float TargetAt(Receiver r,DIReservoir sample)
{
    if(!sample.valid || sample.lightIndex>=lightCount) return 0;
    StructuredBuffer<GPULight> lights=ResourceDescriptorHeap[lightsIdx];
    float3 d,s,l; float distance;
    EvaluateLight(r,lights[sample.lightIndex],d,s,l,distance);
    return LightTarget(d,s);
}
// p_hat*V at a receiver: the unshadowed target times one shadow ray.
float VisibleTarget(Receiver r,DIReservoir sample,uint counterIndex)
{
    if(!sample.valid || sample.lightIndex>=lightCount) return 0;
    StructuredBuffer<GPULight> lights=ResourceDescriptorHeap[lightsIdx];
    float3 d,s,l; float distance;
    EvaluateLight(r,lights[sample.lightIndex],d,s,l,distance);
    float t=LightTarget(d,s);
    if(!(t>0)) return 0;
    return SampleVisibility(r,lights[sample.lightIndex],sample.pathSeed,l,distance,counterIndex)>0?t:0;
}
void TestSelected(Receiver r,inout DIReservoir sample)
{
    if(!sample.valid) return;
    StructuredBuffer<GPULight> lights=ResourceDescriptorHeap[lightsIdx];
    float3 d,s,l; float distance;
    EvaluateLight(r,lights[sample.lightIndex],d,s,l,distance);
    sample.visible=SampleVisibility(r,lights[sample.lightIndex],sample.pathSeed,l,distance,1)>0;
    sample.visibilityTested=true;
    if(!sample.visible) Count(7);
}
// The reservoir that is shaded is also the one carried into the next frame.
// Optionally discard an occluded selection from it (RTXDI
// StoreVisibilityInDIReservoir(...,discardIfInvisible=true)): M is kept for
// correct resampling, only the sample is removed. The light index and the
// tested/visible bits survive so Shade can still emit the light distance.
void FinishSelected(Receiver r,inout DIReservoir sample)
{
    TestSelected(r,sample);
    if((flags&kDIFlagDiscardInvisible) && sample.valid && sample.visibilityTested && !sample.visible)
    { sample.valid=false; sample.weightSum=0; }
}
// RTXDI BoilingFilter: a sample whose contribution weight is far above the
// wave's average non-zero weight is a lucky outlier that spatiotemporal reuse
// would otherwise spread into a growing blotch. Strength 0.2 = RTXDI default.
void BoilingFilter(inout DIReservoir sample)
{
    float w=DIWeight(sample);
    float sum=WaveActiveSum(w>0?w:0);
    uint count=WaveActiveCountBits(w>0);
    const float multiplier=10.0/0.2-9.0;
    if(w>0 && count>1 && w>multiplier*sum/float(count))
    { DIReset(sample); Count(26); }
}
bool CompactPixel(uint lane,out uint pixel,out uint2 tp)
{
    RWStructuredBuffer<uint> counters=ResourceDescriptorHeap[counterUav];
    pixel=0; tp=0;
    if(lane>=counters[0]) return false;
    StructuredBuffer<uint> list=ResourceDescriptorHeap[listSrv];
    pixel=list[lane]; tp=uint2(pixel%traceW,pixel/traceW);
    return true;
}
[numthreads(64,1,1)]
void Temporal(uint3 tid:SV_DispatchThreadID,uint3 gid:SV_GroupID)
{
    uint pixel; uint2 tp;
    if(!CompactPixel(CompactLane(tid,gid),pixel,tp)) return;
    Receiver r=LoadTracingReceiver(FullPixel(tp));
    StructuredBuffer<DIPacked> initial=ResourceDescriptorHeap[initialSrv];
    float3 ignored;
    DIReservoir result=DIUnpack(initial[pixel],ignored);
    uint reactiveFrames=0;
    // Do not restart Initial's random stream: selection must not reuse a
    // random variate that already chose one of its candidates.
    uint rng=HashLowbias32(pixel^HashLowbias32(frame)^0xad90777du);
    if(rng==0) rng=1;
    if(historyReset==0)
    {
        Texture2D<float2> velocity=ResourceDescriptorHeap[velocityIdx];
        float2 mv=velocity.Load(int3(FullPixel(tp),0))*float2(-.5,.5);
        float stride=(flags&4u)?1:2;
        const float2 previousCoordinate=float2(
            RTPreviousTraceCoordinate(tp.x,mv.x,width,stride,historyJitterDelta.x),
            RTPreviousTraceCoordinate(tp.y,mv.y,height,stride,historyJitterDelta.y));
        int2 reprojected=int2(round(previousCoordinate));
        if(areaLite!=0)
        {
            // Area-lite fractional film motion: the jitter-corrected previous film position picks one
            // of its four source pixels with bilinear probability instead of the nearest one, so
            // sub-pixel jitter and motion resample history instead of snapping to it.
            const float2 cell=floor(previousCoordinate), fraction=previousCoordinate-cell;
            reprojected=int2(cell)+int2(RTRandom(rng)<fraction.x,RTRandom(rng)<fraction.y);
        }
        StructuredBuffer<DIPacked> previous=ResourceDescriptorHeap[previousSrv];
        StructuredBuffer<uint4> surface=ResourceDescriptorHeap[previousSurfaceSrv];
        // RTXDI TemporalResampling: the reprojected tap first, then a few
        // random taps nearby, so silhouettes and sub-pixel motion do not
        // throw a whole history away on one failed receiver test.
        bool found=false; int2 previousPixel=0; DIReservoir old; DIReset(old);
        float3 previousP=0,previousN=r.N;
        [loop] for(uint attempt=0;attempt<5 && !found;++attempt)
        {
            int2 q=reprojected;
            if(attempt==0 && (flags&kDIFlagPermutation))
            {
                // RTXDI_ApplyPermutationSampling with one frame-uniform number.
                uint permutation=HashLowbias32(frame*0x2c1b3c6du+0x297a2d39u);
                int2 offset=int2(permutation&3u,(permutation>>2)&3u);
                q=((q+offset)^3)-offset;
            }
            else if(attempt>0) q+=int2(round((float2(RTRandom(rng),RTRandom(rng))*2-1)*(stride>1?1.5:2.5)));
            if(any(q<0) || any(q>=int2(historyWidth,historyHeight))) continue;
            uint index=q.y*historyWidth+q.x;
            float3 previousNormal;
            DIReservoir candidate=DIUnpack(previous[index],previousNormal);
            TranslateHistory(candidate,true); // into today's light list
            uint4 previousSurface=surface[index];
            found=previousSurface.w==r.material && dot(previousNormal,r.N)>.9
                && abs(dot(asfloat(previousSurface.xyz)-r.P,r.N))<max(.01,r.viewZ*.01);
            if(found) { old=candidate; previousPixel=q; previousP=asfloat(previousSurface.xyz); previousN=previousNormal; }
        }
        // A removed light reached this history's receiver.
        if(found && ReachedByRemovedLight(previousP)) { found=false; DIReset(old); Count(30); }
        if(found)
        {
            bool visibilityChanged=false;
            if((flags&(1u<<24)) && (old.valid || !(flags2&kDIFlags2ConfidenceRequireValid)))
            {
                // The cooldown belongs to the receiver, valid sample or not.
                Texture2D<float> confidence=ResourceDescriptorHeap[confidenceSrv];
                // Receiver history outlives any particular selected light.
                visibilityChanged=old.reactiveFrames==0 && confidence.Load(int3(previousPixel,0))==0;
                reactiveFrames=old.reactiveFrames>0 ? old.reactiveFrames-1 : (visibilityChanged ? 2u : 0u);
            }
            // Classify's retest saw this receiver's tested light change visibility. Its M frames
            // describe the old visibility: merged under pairwise MIS, a fresh visible sample would get
            // weight 1/(M+1) and the shadow would fade out over ~45 frames whatever NRD does. Drop the
            // reservoir history there (the fresh sample alone is unbiased), as NRD drops its own through
            // the same confidence.
            if(visibilityChanged && !(flags2&kDIFlags2ConfidenceRequireValid)) found=false;
        }
        if(found)
        {
            // RTXDI DI has no age rejection; age only counts persistence. A cap
            // of 63 (the 6-bit ceiling) therefore means "never discard".
            bool ageOk=maxAge>=63u || old.age<maxAge;
            if(ageOk && (flags&kDIFlagVisibilityMIS))
            {
                // Two-technique balance heuristic (pairwise, non-defensive, as
                // the Area-ReSTIR author's temporal pass) over p_hat*V.
                DICap(old,maxM);
                const float Mc=float(result.M),Mh=float(old.M);
                const float pcc=result.valid?result.target:0; // fresh: tested visible
                const float phh=old.valid?old.target:0;       // history: visible at its receiver
                const float pch=VisibleTarget(r,old,28);
                float phc=0;
                if(result.valid)
                {
                    // The fresh light seen from the previous receiver. The
                    // previous material is the same one (receiver test), so
                    // only position, normal and view differ.
                    bool same=length(previousP-r.P)<=max(.001,r.viewZ*1e-4) && dot(previousN,r.N)>.999;
                    if(!LightExistedPreviously(result.lightIndex)) phc=0;
                    else if(same) phc=pcc;
                    else
                    {
                        Receiver previousReceiver=r;
                        previousReceiver.P=previousP; previousReceiver.N=previousN;
                        previousReceiver.V=normalize(camera-previousP);
                        // A stored receiver: no pixel to rebuild Ng from.
                        previousReceiver.hasPixel=false; previousReceiver.hasNg=false;
                        phc=VisibleTarget(previousReceiver,result,28);
                    }
                }
                float mh=Mh*phh+Mc*pch>0?Mh*phh/(Mh*phh+Mc*pch):0;
                float mc=Mc*pcc+Mh*phc>0?Mc*pcc/(Mc*pcc+Mh*phc):0;
                DIReservoir merged; DIReset(merged);
                DISelect(merged,result,pcc,mc*pcc*DIWeight(result),RTRandom(rng));
                ++old.age;
                DISelect(merged,old,pch,mh*pch*DIWeight(old),RTRandom(rng));
                if(!merged.valid) { merged=result; merged.valid=false; merged.weightSum=0; }
                // MIS weights already partition unity; convert to stream ABI.
                merged.M=result.M+old.M;
                merged.weightSum*=merged.M;
                if(merged.valid) { merged.visibilityTested=true; merged.visible=true; }
                result=merged;
                Count(5);
            }
            else if(ageOk)
            {
                // historyLimit = maxHistoryLength * current M (= 1 frame).
                DICap(old,maxM);
                // An empty/discarded history still represents M zero samples.
                result.M+=old.M;
                if(old.valid && old.lightIndex<lightCount)
                {
                    // RTXDI evaluates the unoccluded target at the receiving
                    // surface; visibility is retraced after selection.
                    float target=TargetAt(r,old);
                    float weight=target*DIWeight(old)*old.M;
                    ++old.age;
                    DISelect(result,old,target,weight,RTRandom(rng));
                }
                Count(5);
            }
        }
    }
    if(flags&kDIFlagBoilingFilter) BoilingFilter(result);
    result.reactiveFrames=reactiveFrames;
    // result.pathSeed stays the selected sample's light-point seed.
    // When a spatial pass follows, its final selection owns the one
    // visibility ray (and the discard); tracing here would be wasted.
    // Visibility-aware reuse selects only visible samples: no final ray.
    if(!(flags&kDIFlagSpatialTests) && !(flags&kDIFlagVisibilityMIS)) FinishSelected(r,result);
    RWStructuredBuffer<DIPacked> output=ResourceDescriptorHeap[temporalUav];
    output[pixel]=DIPack(result,r.N);
}
// Conditional expectation of the final reservoir selection. This diagnostic
// spends a visibility ray per contributing sample to isolate resampling noise.
void IntegrateCandidate(Receiver r,DIReservoir sample,float coefficient,
                        inout float3 diffuse,inout float3 specular,
                        inout float hitDistance,inout float hitWeight)
{
    if(!(coefficient>0) || !sample.valid) return;
    StructuredBuffer<GPULight> lights=ResourceDescriptorHeap[lightsIdx];
    float3 d,s,l; float distance;
    EvaluateLight(r,lights[sample.lightIndex],d,s,l,distance);
    float rtDiVisibility=SampleVisibility(r,lights[sample.lightIndex],sample.pathSeed,l,distance,1);
    diffuse+=coefficient*d*rtDiVisibility;
    specular+=coefficient*s*rtDiVisibility;
    float luminanceWeight=LightTarget(d,s)*coefficient*rtDiVisibility;
    hitDistance+=distance*luminanceWeight; hitWeight+=luminanceWeight;
}
[numthreads(64,1,1)]
void Spatial(uint3 tid:SV_DispatchThreadID,uint3 gid:SV_GroupID)
{
    uint pixel; uint2 tp;
    uint lane=CompactLane(tid,gid);
    if(!CompactPixel(lane,pixel,tp)) return;
    RWStructuredBuffer<uint> stageCounters=ResourceDescriptorHeap[counterUav];
    if(lane==0) InterlockedAdd(stageCounters[14+min(stageIndex,1u)],1);
    StructuredBuffer<DIPacked> input=ResourceDescriptorHeap[temporalSrv];
    RWStructuredBuffer<DIPacked> output=ResourceDescriptorHeap[initialUav];
    Receiver r=LoadTracingReceiver(FullPixel(tp));
    float3 ignored;
    // Exact copy is also the all-rejected positive control: no new RNG. When
    // Temporal left the visibility ray to this pass, the copy still owes it.
    if(flags&16u)
    {
        if(!(flags&kDIFlagSpatialTests) || (flags&kDIFlagVisibilityMIS)) { output[pixel]=input[pixel]; return; }
        DIReservoir copied=DIUnpack(input[pixel],ignored);
        FinishSelected(r,copied);
        output[pixel]=DIPack(copied,r.N); return;
    }
    DIReservoir canonical=DIUnpack(input[pixel],ignored);
    float canonicalTarget=TargetAt(r,canonical);
    uint rng=HashLowbias32(pixel^HashLowbias32(frame)^0x695dcfe5u^HashLowbias32(stageIndex));
    if(rng==0) rng=1;
    DIReservoir result; DIReset(result);
    float canonicalMIS=0;
    float3 integratedDiffuse=0,integratedSpecular=0;
    float integratedHit=0,integratedHitWeight=0;
    const uint K=(flags&8192u) ? 1u : clamp(spatialCandidates,1u,8u);
    uint accepted=0,paired=0,represented=canonical.M;
    float2 rotation=RTReservoirR2(tp,frame,0x83u+stageIndex*0x100u);
    for(uint k=0;k<K;++k)
    {
        // Hilbert/R2 rotation and radial strata cover the disk with K attempts.
        float angle=frac(rotation.x+k*0.61803398875)*2*PI;
        float radius=sqrt((k+rotation.y)/K)*spatialRadius;
        int2 q=int2(tp)+int2(round(float2(cos(angle),sin(angle))*radius));
        // Pair validity is GEOMETRIC only. A neighbour whose reservoir holds no
        // sample still owns M candidates, so its canonical half must use
        // Mi*p_i(y_c) rather than the rejected-pair 1/K (RTXDI pairwise).
        bool geometric=(flags&32u)==0 && all(q>=0) && all(q<int2(traceW,traceH)) && any(q!=int2(tp));
        DIReservoir neighbour; DIReset(neighbour);
        Receiver nr=(Receiver)0;
        float shifted=0, inverse=0, neighbourTarget=0;
        if(geometric)
        {
            neighbour=DIUnpack(input[q.y*traceW+q.x],ignored);
            nr=LoadTracingReceiver(FullPixel(q));
            geometric=nr.eligible && dot(nr.N,r.N)>.9
                && abs(dot(nr.P-r.P,r.N))<max(.01,r.viewZ*.01);
        }
        bool usable=geometric && neighbour.valid && neighbour.lightIndex<lightCount;
        if(geometric)
        {
            ++paired; represented+=neighbour.M;
            inverse=(flags&kDIFlagVisibilityMIS) && (flags&kDIFlagInverseVisibility)
                ? VisibleTarget(nr,canonical,27) : TargetAt(nr,canonical);
        }
        if(usable)
        {
            // Under visibility-aware reuse both own targets already include
            // V=1 (each reservoir holds a sample visible from its receiver).
            shifted=(flags&kDIFlagVisibilityMIS)?VisibleTarget(r,neighbour,27):TargetAt(r,neighbour);
            neighbourTarget=TargetAt(nr,neighbour);
        }
        canonicalMIS+=RTPairCanonical(canonical.M,canonicalTarget,geometric?neighbour.M:0,inverse,1,K,geometric);
        float mi=RTPairNeighbour(neighbour.M,neighbourTarget,
            canonical.M,shifted,1,K,usable);
        if(usable)
        {
            ++accepted; Count(stageIndex==0?6:13);
            if(flags&4096u)
                IntegrateCandidate(r,neighbour,mi*DIWeight(neighbour),
                    integratedDiffuse,integratedSpecular,integratedHit,integratedHitWeight);
            DISelect(result,neighbour,shifted,mi*DIWeight(neighbour)*shifted,RTRandom(rng));
        }
    }
    if(paired==0)
    {
        // Every pair rejected: the canonical halves are exactly 1/K each, so
        // the result is the input itself (the bit-identical positive control).
        if(abs(canonicalMIS-1)>1e-6) Count(8);
        if(!(flags&kDIFlagSpatialTests) || (flags&kDIFlagVisibilityMIS)) { output[pixel]=input[pixel]; return; }
        FinishSelected(r,canonical);
        output[pixel]=DIPack(canonical,r.N); return;
    }
    if(flags&4096u)
    {
        IntegrateCandidate(r,canonical,canonicalMIS*DIWeight(canonical),
            integratedDiffuse,integratedSpecular,integratedHit,integratedHitWeight);
        float3 df,sf;
        DIMaterialFactors(r.N,r.V,r.albedo,r.F0,r.rough,df,sf);
        integratedDiffuse/=df; integratedSpecular/=sf;
        if(!all(isfinite(integratedDiffuse)) || !all(isfinite(integratedSpecular)))
        { Count(3); integratedDiffuse=integratedSpecular=0; }
        float distance=integratedHitWeight>0?integratedHit/integratedHitWeight:0;
        RWTexture2D<float4> diffuse=ResourceDescriptorHeap[diffUav];
        RWTexture2D<float4> specular=ResourceDescriptorHeap[specUav];
        diffuse[tp]=DIPackSignal(integratedDiffuse,distance,r.viewZ,r.rough);
        specular[tp]=DIPackSignal(integratedSpecular,distance,r.viewZ,r.rough);
        Count(2);
        uint represented=WaveActiveSum(canonical.M);
        if(WaveIsFirstLane()) InterlockedAdd(stageCounters[9],represented);
        // Scratch-only marker: Shade must not overwrite the integrated signal.
        // This reservoir never becomes temporal history.
        DIPacked packed=input[pixel]; packed.reference|=1u<<23;
        output[pixel]=packed; return;
    }
    DISelect(result,canonical,canonicalTarget,canonicalMIS*DIWeight(canonical)*canonicalTarget,RTRandom(rng));
    // MIS already contains population weighting; convert back to streaming
    // ABI. The pooled population (canonical + paired neighbours) is what this
    // reservoir represents when it becomes the next frame's history; the
    // temporal pass caps it back to maxM.
    result.M=max(min(represented,63u),1u);
    result.weightSum*=result.M;
    if(!isfinite(result.weightSum)) { Count(8); result=canonical; }
    if(flags&kDIFlagVisibilityMIS)
    {
        if(result.valid) { result.visibilityTested=true; result.visible=true; }
        else if(canonical.visibilityTested && !(flags2&kDIFlags2ConfidenceRequireValid))
        {
            // Nothing visible was selected. Carry the canonical's tested light (weightless) so next
            // frame's confidence retest can see the occluder move away; Shade treats it as a discarded
            // selection.
            result.lightIndex=canonical.lightIndex; result.pathSeed=canonical.pathSeed;
            result.visibilityTested=true; result.visible=canonical.visible;
        }
    }
    else FinishSelected(r,result);
    output[pixel]=DIPack(result,r.N);
}
[numthreads(64,1,1)]
void Shade(uint3 tid:SV_DispatchThreadID,uint3 gid:SV_GroupID)
{
    uint pixel; uint2 tp;
    if(!CompactPixel(CompactLane(tid,gid),pixel,tp)) return;
    StructuredBuffer<DIPacked> input=ResourceDescriptorHeap[initialSrv];
    float3 ignored;
    DIPacked packed=input[pixel];
    if((flags&4096u) && (packed.reference&(1u<<23))) return;
    DIReservoir sample=DIUnpack(packed,ignored);
    // A discarded occluded selection shades to zero exactly like an occluded one, and keeps
    // that light's distance as the NRD hit distance (a zero distance on black samples biases
    // REBLUR badly).
    bool discarded=!sample.valid && sample.visibilityTested && !sample.visible && sample.lightIndex<lightCount;
    if(discarded) { sample.valid=true; sample.weightSum=0; }
    if(!sample.valid) return;
    Count(2);
    RWStructuredBuffer<uint> counters=ResourceDescriptorHeap[counterUav];
    uint represented=WaveActiveSum(sample.M);
    if(WaveIsFirstLane()) InterlockedAdd(counters[9],represented);
    Receiver r=LoadReceiver(FullPixel(tp));
    StructuredBuffer<GPULight> lights=ResourceDescriptorHeap[lightsIdx];
    float3 d,s,l; float distance;
    EvaluateLight(r,lights[sample.lightIndex],d,s,l,distance);
    float rtDiVisibility=sample.visibilityTested?(sample.visible?1.0:0.0):1.0;
    float weight=DIWeight(sample)*rtDiVisibility;
    float3 df,sf;
    DIMaterialFactors(r.N,r.V,r.albedo,r.F0,r.rough,df,sf);
    d=d*weight/df; s=s*weight/sf;
    if(!all(isfinite(d)) || !all(isfinite(s))) { Count(3); d=s=0; }
    RWTexture2D<float4> diffuse=ResourceDescriptorHeap[diffUav];
    RWTexture2D<float4> specular=ResourceDescriptorHeap[specUav];
    diffuse[tp]=DIPackSignal(d,distance,r.viewZ,r.rough);
    specular[tp]=DIPackSignal(s,distance,r.viewZ,r.rough);
}
[numthreads(8,8,1)]
void Resolve(uint3 tid:SV_DispatchThreadID)
{
    uint2 px=tid.xy;
    if (any(px>=uint2(width,height))) return;
    Receiver r=LoadReceiver(px);
    float3 d=0,s=0; float sum=0;
    float3 fallbackD=0,fallbackS=0; float fallbackSum=0;
    Texture2D<float4> diffuse=ResourceDescriptorHeap[diffSrv];
    // Optional near-mirror escape: only specular bypasses NRD. It still uses
    // the same demodulation, bilateral upsample and full-resolution remodulation.
    bool rawMirror=r.eligible && mirrorRoughness>0 && r.rough<=mirrorRoughness;
    Texture2D<float4> specular=ResourceDescriptorHeap[NonUniformResourceIndex(rawMirror?rawSpecSrv:specSrv)];
    if(rawMirror) Count(16);
    if (r.eligible)
    {
        if (flags&4u)
        {
            d=DIUnpackSignal(diffuse.Load(int3(px,0)));
            s=DIUnpackSignal(specular.Load(int3(px,0)));
            sum=1;
        }
        else for (int y=-1;y<=1;++y) for (int x=-1;x<=1;++x)
        {
            int2 q=clamp(int2(px/2)+int2(x,y),0,int2(traceW,traceH)-1);
            Receiver neighbour=LoadReceiverBase(FullPixel(q));
            if (!neighbour.eligible) continue;
            float w=pow(saturate(dot(r.N,neighbour.N)),32)*
                exp(-abs(dot(neighbour.P-r.P,r.N))/max(.01,r.viewZ*.01));
            float bilateralWeight=w;
            if(flags&(1u<<23))
            {
                // Stored receivers are at full pixel 2*q+phase, not 2*q+0.5.
                // Center the separable reconstruction kernel on (px-phase)/2
                // in float. Radius 1.5 preserves constants and linear ramps at
                // integer and half-integer phases while smoothing even pixels.
                float2 fraction=(float2(px&1u)-float2(TracePhase()))*.5;
                float2 kernel=max(1.5-abs(float2(x,y)-fraction),0);
                w*=kernel.x*kernel.y;
            }
            float3 nd=DIUnpackSignal(diffuse.Load(int3(q,0)));
            float3 ns=DIUnpackSignal(specular.Load(int3(q,0)));
            if(flags&(1u<<19))
            {
                // Deliberately wrong ordering for the material-boundary test.
                float3 ndf,nsf;
                DIMaterialFactors(neighbour.N,neighbour.V,neighbour.albedo,neighbour.F0,neighbour.rough,ndf,nsf);
                if(flags&(1u<<21)) ndf*=neighbour.albedo;
                nd*=ndf; ns*=nsf;
            }
            if(flags&(1u<<23))
            {
                fallbackD+=nd*bilateralWeight; fallbackS+=ns*bilateralWeight;
                fallbackSum+=bilateralWeight;
            }
            d+=nd*w; s+=ns*w;
            sum+=w;
        }
    }
    // A thin surface can leave only a guide-valid neighbour outside the tent: use the
    // guide-only (bilateral) weights then rather than leave a black hole.
    if(sum<=0 && fallbackSum>0) { d=fallbackD; s=fallbackS; sum=fallbackSum; }
    // Weights this small mean no trace texel describes this surface (a thin object, an odd
    // pixel in front of another surface). Normalizing them would give the pixel a full-weight
    // copy of an unrelated neighbour's light and shadow; alpha 0 hands it to the clustered loop
    // instead.
    if((flags2&kDIFlags2ResolveFallback) && r.eligible && !(flags&4u) && sum<1e-3) { sum=0; Count(29); }
    if(r.eligible && (flags&128u) && (flags&1u)==0)
    {
        float history=diffuse.Load(int3(px/((flags&4u)?1u:2u),0)).w;
        uint maximum=WaveActiveMax(asuint(max(history,0)));
        RWStructuredBuffer<uint> counters=ResourceDescriptorHeap[counterUav];
        if(WaveIsFirstLane()) InterlockedMax(counters[10],maximum);
        if(all(px>=uint2(width,height)*3/8) && all(px<uint2(width,height)*5/8))
        {
            uint total=WaveActiveSum(uint(round(max(history,0))));
            uint population=WaveActiveCountBits(true);
            if(WaveIsFirstLane())
            { InterlockedAdd(counters[11],total); InterlockedAdd(counters[12],population); }
        }
    }
    float3 df,sf;
    DIMaterialFactors(r.N,r.V,r.albedo,r.F0,r.rough,df,sf);
    if(flags&(1u<<21)) df*=r.albedo;
    // Remodulate AFTER the gather using the full-resolution receiver.
    if((flags&(1u<<19)) && (flags&4u)==0) { df=1; sf=1; }
    float3 outputD=sum>0?d/sum*df:0,outputS=sum>0?s/sum*sf:0;
    if((flags&(1u<<18)) && r.eligible && abs(int(px.x)-int(width/2))<=8
        && abs(int(px.y)-int(height/2))<=64)
    {
        // White-light red/green dielectric fixture. Equal minor albedo channels
        // must retain equal diffuse radiance; the dielectric specular is white.
        bool red=r.albedo.r>r.albedo.g*4,green=r.albedo.g>r.albedo.r*4;
        RWStructuredBuffer<uint> counters=ResourceDescriptorHeap[counterUav];
        if(red || green)
        {
            float error=red?abs(outputD.g-outputD.b)/max(abs(outputD.r),1e-20)
                           :abs(outputD.r-outputD.b)/max(abs(outputD.g),1e-20);
            uint redMaximum=WaveActiveMax(red?asuint(error):0),greenMaximum=WaveActiveMax(green?asuint(error):0);
            uint redPopulation=WaveActiveCountBits(red),greenPopulation=WaveActiveCountBits(green);
            if(WaveIsFirstLane())
            {
                InterlockedMax(counters[17],redMaximum); InterlockedMax(counters[18],greenMaximum);
                InterlockedAdd(counters[19],redPopulation); InterlockedAdd(counters[20],greenPopulation);
            }
            float chroma=max(abs(outputS.r-outputS.g),abs(outputS.g-outputS.b))/max(max(outputS.r,max(outputS.g,outputS.b)),.001);
            uint maximum=WaveActiveMax(asuint(chroma));
            uint nv=WaveActiveMax(asuint(saturate(dot(r.N,r.V))));
            if(WaveIsFirstLane()) { InterlockedMax(counters[21],maximum); InterlockedMax(counters[22],nv); }
            // Dedicated unoccluded punctual-light fixture oracle. A chromatic
            // predicate alone misses energy loss when an albedo channel is zero.
            StructuredBuffer<GPULight> probeLights=ResourceDescriptorHeap[lightsIdx];
            float3 expected=0;
            for(uint light=0;light<lightCount;++light)
            {
                float3 ed,es,el; float distance;
                EvaluateLight(r,probeLights[light],ed,es,el,distance);
                expected+=ed*((flags&(1u<<21))?r.albedo:1);
            }
            float energyError=max(max(abs(outputD.x-expected.x),abs(outputD.y-expected.y)),abs(outputD.z-expected.z))
                /max(max(expected.x,max(expected.y,expected.z)),1e-20);
            uint maxEnergy=WaveActiveMax(asuint(energyError));
            if(WaveIsFirstLane()) InterlockedMax(counters[23],maxEnergy);
        }
    }
    uint debugMode=(flags>>16)&3u;
    // Alpha is a diagnostic tag consumed only by the deferred DI fold: in normal rendering 1 on
    // a pixel with an estimate (0 without one, below); fallback materials are untagged.
    if(r.eligible && debugMode==2)
    {
        Result[uint3(px,0)]=float4(1,0,1,2);
        Result[uint3(px,1)]=float4(0,0,0,1);
        return;
    }
    // Alpha 0 = this pixel has no DI estimate: the producer judged it ineligible, or no
    // half-resolution sample supported it (a thin or odd-pixel surface in front of an
    // ineligible background). The deferred fold then falls back to the clustered loop instead
    // of drawing it unlit. The isolated signal (debug 3) tags only pixels with an estimate: a
    // fallback pixel tagged 3 would count as a DI zero in every energy measurement, though
    // rendering lights it.
    float tag=r.eligible && sum>0?(debugMode==3?3:1):0;
    // Shade packed the light distance with the signal (RELAX raw, REBLUR normalized by
    // rayTMax) - the pixel's own trace texel at full resolution, its 2x2 cell's at half; 0
    // where there is no estimate.
    float lightDistance=1;
    if(flags2&kDIFlags2RRDistance)
    {
        lightDistance=0;
        if(r.eligible && sum>0)
        {
            int2 q=clamp(int2(px/((flags&4u)?1u:2u)),0,int2(traceW,traceH)-1);
            float w=specular.Load(int3(q,0)).w;
            lightDistance=(flags&kDIFlagRelax)?w:w*rayTMax;
        }
    }
    Result[uint3(px,0)]=float4(outputD,tag);
    Result[uint3(px,1)]=float4(outputS,lightDistance);
}
