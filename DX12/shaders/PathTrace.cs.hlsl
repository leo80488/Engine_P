// GBuffer-primary path tracing with an opt-in perspective camera/thin-lens arm.
// maxBounces=0 uses Russian roulette only.
// Finite-cone sun MIS and point/spot NEE; environment and emissive surfaces use BSDF sampling.
// PT_MEDIA (PathTracePass's media pipelines): the fog as a participating medium - collisions on
// every segment, medium vertices, the fog on the shadow rays, and the camera segment's estimator
// (FogCamera). Without it the kernels are the medium-free ones.
#ifndef PT_MEDIA
#define PT_MEDIA 0
#endif
#define PT_REFERENCE_GGX 1
#include "rt_instance_data.hlsli"
#include "rt_alpha_record.hlsli"
#include "bsdf_sample.hlsli"
#include "light_eval.hlsli"
#include "material.hlsli"
#include "core/depth.hlsli"   // LinearizeReverseZ, WorldPosFromDepth
#include "core/gbuffer_codec.hlsli"   // GBufferDecodeMatIdx
#include "core/color.hlsli"   // Luminance
#include "core/bsdf_dielectric.hlsli"   // a Transmissive vertex's interface
cbuffer Params : register(b0)
{
    float4x4 invViewProj;
    float3 camera; uint spp;
    float3 sunDirection; uint width;
    float3 sunColor; uint height;
    uint albedoIdx, normalIdx, surfaceIdx, depthIdx;
    uint alphaTestEnabled, alphaInstanceCount, seed, skyIdx;
    uint lightsIdx, lightCount, maxBounces; float environmentScale;
    float sunCosRadius; uint flags; float constantEnvironment; uint materialsIdx;
    float3 cameraRight; float lensRadius;
    float3 cameraUp; float focusDistance;
    float3 cameraForward; uint pathMaterialsIdx;
    uint rrAlbedoUav,rrSpecularUav,rrNormalUav,rrDistanceUav;
    uint activeLocalLightCount, firstActiveLocalLightIndex;
    uint areaPreviousIdx, areaCurrentIdx;
    uint areaHistoryCap, areaHistoryValid;
    uint areaPreviousFilmIdx, areaCurrentFilmIdx;
    float4x4 areaPreviousViewProj;
    uint areaPreviousPrimaryIdx, areaCurrentPrimaryIdx;
    uint areaPreviousGeometryIdx, areaCurrentGeometryIdx;
    float areaReconnectWeight; uint areaRadianceIdx; uint areaFlags; float areaSpatialRadius;
    // Area v2 (the AreaV2* kernels): the previous frame's camera, for shifts into the previous
    // domain (pairwise temporal MIS needs the reverse shift of the canonical).
    float4x4 areaPrevInvViewProj;
    float3 areaPrevCamera; float areaPrevLensRadius;
    float3 areaPrevRight; float areaPrevFocus;
    float3 areaPrevUp; uint areaPreviousLensIdx;
    float3 areaPrevForward; uint areaCurrentLensIdx;
    // ptRRStart: first bounce Russian roulette may end (2 while the frame does not accumulate,
    // else 3). lightAliasIdx: the power alias table over the active local lights
    // (activeLocalLightCount rows); 0 = uniform.
    uint areaSpatialK; float areaSpatialConfidence; uint ptRRStart, lightAliasIdx;
    // -pt_last_nee_off: no next-event estimate at the vertex the bounce limit ends the path at,
    // so -pt_bounces=B counts exactly the paths of length <= B+1 that RTXDI's
    // maxBounceDepth=B+1 does (for the RtxdiPT A/B).
    // -pt_gi_shell_model: the ReSTIR GI shell's hit model at every bounce vertex (see the bounce
    // loop).
    // auxOutUav: the FrontReflection / NprAmbient / FogCamera entries' output (those dispatches'
    // own constants); on the main entry the preview denoiser's sample moment (RTStoreSample), 0
    // when it is off. Area's entries never get it.
    uint ptLastNeeOff, ptGiShellModel, auxOutUav, ptFrontSamples;
#if PT_MEDIA
    // The fog as a participating medium: the medium set's heap index (0: none), mediaFlags (1:
    // single scattering, pass.pathTrace.mediaSingleScatter; 2: equi-angular sampling on the camera
    // segment, pass.pathTrace.mediaEquiAngular), the local lights' fog scales (0: all 1) and the
    // sun's. The medium-free kernels leave the row undeclared: the bound constants are the same.
    uint mediumIdx, mediaFlags, lightMediumScaleIdx; float sunMediumScale;
#endif
};
RaytracingAccelerationStructure g_TLAS : register(t0);
StructuredBuffer<RTInstanceData> g_Instances : register(t1);
StructuredBuffer<RTAlphaRecord> g_AlphaRecords : register(t2);
ByteAddressBuffer g_Buffers[16384] : register(t0, space1);
Texture2D g_AllTextures[16384] : register(t0, space2);
SamplerState gSampWrap : register(s1);
RWTexture2D<float4> Result : register(u0);
RWTexture2D<float4> Accumulation : register(u1);
RWStructuredBuffer<uint> Counters : register(u2);
void RTLensConeProbe();
void RTAreaReconnectProbe();
[numthreads(1,1,1)]
void ClearCounters()
{
    for (uint i=0;i<7;++i) Counters[i]=0;
    if(flags&16384u) for(uint j=0;j<((flags&4194304u)?15u:((flags&262144u)?12u:((flags&65536u)?9u:6u)));++j) Counters[7+2*7*128+16*32+j]=0;
    if(flags&16777216u) RTLensConeProbe();
    if(flags&268435456u) RTAreaReconnectProbe();
    if(flags&134217728u) for(uint k=24;k<28;++k) Counters[7+2*7*128+16*32+k]=0;
    if(areaFlags&1u) for(uint m=0;m<16;++m) Counters[7+2*7*128+16*32+64+m]=0; // Area v2 shift accounting (kAreaV2Base)
}
void CountRay(uint index)
{
    if (flags & 8u)
    {
        uint count = WaveActiveCountBits(true);
        if (WaveIsFirstLane()) InterlockedAdd(Counters[index],count);
    }
}
typedef RTInstanceData HRInstanceData;
#define HR_MaterialUV RTInstanceUV
#include "rt_hit_surface.hlsli"

// Candidate alpha is evaluated before commitment. Each candidate needs its own
// distance, triangle area and incidence; a rejected hit must not advance width.
bool RTAlphaAcceptCone(uint id,uint primitive,float2 bary,float3x4 objectToWorld,
    float3 direction,float candidateWidth)
{
    if(!(flags&67108864u) || !(HR_AlphaFlags(id)&RT_ALPHA_FLAG_ALPHA_CLIP))
        return HR_AlphaAccept(id,primitive,bary,true);
    RTAlphaRecord a=g_AlphaRecords[id];
    if(a.albedoTexIdx<0 || a.albedoTexIdx>=int(HR_BINDLESS_LIMIT))
        return HR_AlphaAccept(id,primitive,bary,true);
    RTInstanceData inst=g_Instances[id];
    // The surface-cache atlas does not have independently padded mip pages.
    if(inst.rtFlags&RT_INST_FLAG_SURFACE_CACHE) return HR_AlphaAccept(id,primitive,bary,true);
    float3 p0,p1,p2,n0,n1,n2; float2 uv0,uv1,uv2;
    if(!HR_LoadHitTriangleAttributes(id,primitive,p0,p1,p2,n0,n1,n2,uv0,uv1,uv2))
        return HR_AlphaAccept(id,primitive,bary,true);
    float3 e0=mul((float3x3)objectToWorld,p1-p0),e1=mul((float3x3)objectToWorld,p2-p0);
    float3 normal=cross(e0,e1);
    float worldArea=length(normal);
    if(worldArea<1e-20) return HR_AlphaAccept(id,primitive,bary,true);
    float2 u=uv1-uv0,v=uv2-uv0;
    float uvArea=abs((u.x*v.y-u.y*v.x)*inst.uvTilingOffset.x*inst.uvTilingOffset.y);
    float lod=.5*log2(max(uvArea/worldArea,1e-20))
        +log2(max(candidateWidth,1e-20)/max(abs(dot(direction,normal/worldArea)),1e-4));
    return HR_AlphaAcceptMip(id,primitive,bary,true,RTHitTextureLod(a.albedoTexIdx,0,true,lod));
}

// ==== Transmissive vertices (BlendMode::Transmissive): rt_glass.hlsli ====
#include "rt_glass.hlsli"
// The current vertex's glass (inactive at an opaque one): RTEvaluate, RTPdf and the next-event
// estimates read it.
static RTGlass g_glass=(RTGlass)0;
bool PTIsGlass(uint id) { return pathMaterialsIdx!=0 && RTGlassInstance(id); }

// ==== Diffuse transmission (TwoSidedFoliage, Subsurface): rt_diffuse_transmission.hlsli ====
#include "rt_diffuse_transmission.hlsli"
// The current vertex's lobe behind the surface (inactive at any other, and under -pt_diffuse):
// RTEvaluate, RTPdf and the next-event estimates read it.
static RTThin g_thin=(RTThin)0;
RTThin PTThinAt(MaterialGPUData m, float3 albedo, float metal)
{
    if (flags & 1u) return (RTThin)0;
    return RTThinAt(m, albedo, metal);
}

// ==== The fog as a participating medium (PT_MEDIA entries) ====
// core/participating_media.hlsli over this frame's Media::Set (Graphics/MediumSet.h): six float4
// for the set, then six for each local volume. The trackers' variable draws run on a stream of their
// own, seeded from each sample's seed: the path's stream keeps its dimensions, and a replay of the
// seed (Area) traces the same fog.
#if PT_MEDIA
#include "core/participating_media.hlsli"
MediumLocal MediumLoadLocal(uint index)
{
    StructuredBuffer<float4> words=ResourceDescriptorHeap[mediumIdx];
    const uint base=6u*(index+1u);
    const float4 r3=words[base+3],r4=words[base+4],r5=words[base+5];
    MediumLocal v;
    v.row0=words[base]; v.row1=words[base+1]; v.row2=words[base+2];
    v.albedo=r3.xyz; v.density=r3.w;
    v.emission=r4.xyz; v.edgeFade=r4.w;
    v.shape=asuint(r5.x); v.heightFalloff=r5.y; v.halfHeight=r5.z; v._pad=r5.w;
    return v;
}
MediumSet PTMediumSet()
{
    StructuredBuffer<float4> words=ResourceDescriptorHeap[mediumIdx];
    const float4 r0=words[0],r1=words[1],r2=words[2],r3=words[3],r4=words[4],r5=words[5];
    MediumSet s;
    s.global.albedo=r0.xyz; s.global.density=r0.w;
    s.global.emission=r1.xyz; s.global.falloff=r1.w;
    s.global.baseHeight=r2.x; s.global.anisotropy=r2.y; s.global.reach=r2.z; s.global._pad=r2.w;
    s.height.albedo=r3.xyz; s.height.density=r3.w;
    s.height.falloff=r4.x; s.height.height=r4.y; s.height.anisotropy=r4.z; s.height._pad=r4.w;
    s.localCount=asuint(r5.x); s.camera=r5.yzw; s.global.reachCenter=r5.yzw;
    return s;
}
static uint g_mediumRng=1;
// The fog's transmittance along a shadow ray over [0, tMax] (kMediumFar: to the sky).
float PTMediumTransmittance(float3 o,float3 d,float tMax)
{
    return mediumIdx!=0?MediumTransmittance(PTMediumSet(),o,d,0,tMax,g_mediumRng):1;
}
// How strongly light @p index lights the fog (VolumetricLightComponent's intensityScale, 0 when it
// opts out): 1 for every light without the table.
float PTLightMediumScale(uint index)
{
    if(lightMediumScaleIdx==0) return 1;
    StructuredBuffer<float> scales=ResourceDescriptorHeap[lightMediumScaleIdx];
    return scales[index];
}
#endif

// The light a shadow ray carries to p: 0 behind an opaque occluder, else the product of the
// glass crossings' transmittances (no glass under -pt_diffuse, whose paths never meet it), and
// under PT_MEDIA the fog's.
float3 RTVisibility(float3 p, float3 n, float3 l, float distance)
{
    CountRay(3);
    // From the side l leaves by: a light behind a leaf (g_thin) is reached from behind it.
    RayDesc ray; ray.Origin = p + (dot(n,l) >= 0 ? n : -n) * 0.001; ray.Direction = l; ray.TMin = 0.001; ray.TMax = distance;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(g_TLAS, RAY_FLAG_NONE, 0x02, ray);
    float3 transmittance=1;
    while (q.Proceed())
    {
        if (q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        const uint id=q.CandidateInstanceID();
        // Glass filters the light; nothing commits it.
        if (PTIsGlass(id))
        {
            if (!(flags & 1u))
                transmittance*=RTGlassCrossing(pathMaterialsIdx,id,q.CandidatePrimitiveIndex(),
                    q.CandidateTriangleBarycentrics(),(float3x3)q.CandidateWorldToObject3x4(),l);
            continue;
        }
        if (HR_AlphaAccept(id, q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics(), false))
            q.CommitNonOpaqueTriangleHit();
    }
#if PT_MEDIA
    // A ray toward the sun (distance 10000) runs to the sky.
    if (q.CommittedStatus() != COMMITTED_NOTHING) return 0;
    return transmittance * PTMediumTransmittance(ray.Origin, l, distance >= 10000 ? kMediumFar : distance);
#else
    return q.CommittedStatus() == COMMITTED_NOTHING ? transmittance : 0;
#endif
}
float RTMis(float a, float b) { return a*a / max(a*a+b*b,1e-20); }
float RTPdf(float3 n, float3 v, float3 l, float rough)
{
    if (flags & 1u) return saturate(dot(n,l))/PI;
    return g_glass.active ? RTGlassMixturePdf(g_glass,n,v,l,rough)
         : g_thin.active  ? RTThinPdf(g_thin,n,v,l,rough) : RTBsdfPdf(n,v,l,rough);
}
float3 RTEvaluate(float3 n, float3 v, float3 l, float3 albedo, float rough, float metal, float reflectance)
{
    if (flags & 1u) return albedo * (saturate(dot(n,l))/PI);
    return g_glass.active ? RTGlassLightEval(g_glass,n,v,l,albedo,rough,metal,reflectance)
         : g_thin.active  ? RTThinEval(g_thin,n,v,l,albedo,rough,metal,reflectance)
                          : RTEvalBSDF(n,v,l,albedo,rough,metal,reflectance);
}
float3 RRSpecular(float3 n,float3 v,float3 l,float3 albedo,float rough,float metal,float reflectance)
{
    if(flags&1u) return 0;
    float3 spec,kd;
    EvalCookTorrance(n,v,l,lerp((.16*reflectance*reflectance).xxx,albedo,metal),rough,metal,albedo,spec,kd);
    return spec*saturate(dot(n,l));
}
float RRLuminance(float3 value) { return Luminance(max(value,0)); }
float3 RTSun(float3 p, float3 n, float3 v, float3 albedo, float rough, float metal, float reflectance,
    inout uint rng,bool recordSpec,out float specEnergy)
{
    specEnergy=0;
    if ((flags & 4u) || !any(sunColor > 0)) return 0;
    float c = lerp(sunCosRadius,1.0,RTRandom(rng));
    float phi = 2*PI*RTRandom(rng), st = sqrt(saturate(1-c*c));
    float3 l = RTBasis(normalize(-sunDirection),float3(st*cos(phi),st*sin(phi),c));
    if ((dot(n,l) <= 0 && !g_thin.active) || g_glass.inside) return 0;
    float pdf = 1.0 / (2*PI*(1-sunCosRadius));
    float3 radiance = sunColor / (PI*(1-sunCosRadius*sunCosRadius));
    const float3 visibility=RTVisibility(p,n,l,10000);
    const float mis=RTMis(pdf,RTPdf(n,v,l,rough));
    if(recordSpec) specEnergy=RRLuminance(RRSpecular(n,v,l,albedo,rough,metal,reflectance)*radiance*visibility*mis/pdf);
    return RTEvaluate(n,v,l,albedo,rough,metal,reflectance) * radiance * visibility * mis / pdf;
}
// The clustered array also holds directional / disabled entries: sampling
// those adds zero-valued trials and avoidable variance.
bool RTActiveLocalLight(GPULight light)
{
    return light.type!=0u && light.intensity>0 && light.radius>0 && any(light.color>0);
}
// Next-event lights in proportion to power through ClusterPass's alias table (32-byte rows;
// one uniform picks the row and its coin, so a vertex still draws one number for the choice
// and Area replays stay aligned). Without the table (-pt_uniform_lights, or before its ring
// exists) a uniform choice that walks up to every light to find the k-th active one.
struct PTLightAlias { uint light, aliasLight; float threshold, pdfLight, pdfAlias; uint pad0, pad1, pad2; };
bool RTSelectLocalLight(float u,out uint index,out float pdf)
{
    index=0; pdf=0;
    uint activeCount=activeLocalLightCount;
    if(activeCount==0) return false;
    if(lightAliasIdx!=0)
    {
        StructuredBuffer<PTLightAlias> table=ResourceDescriptorHeap[lightAliasIdx];
        float x=u*activeCount;
        uint row=min(uint(x),activeCount-1);
        PTLightAlias entry=table[row];
        bool primary=(x-float(row))<entry.threshold;
        index=primary?entry.light:entry.aliasLight;
        pdf=primary?entry.pdfLight:entry.pdfAlias;
        return pdf>0;
    }
    StructuredBuffer<GPULight> lights = ResourceDescriptorHeap[lightsIdx];
    uint selected=min(uint(u*activeCount),activeCount-1);
    index=firstActiveLocalLightIndex;
    [branch] if(activeCount>1)
    [loop] for(uint i=0;i<lightCount;++i)
        if(RTActiveLocalLight(lights[i]))
        {
            if(selected==0) { index=i; break; }
            --selected;
        }
    pdf=1.0/activeCount;
    return true;
}
// A visibility ray to an emitter of radius R aims at a point on its disk facing the vertex.
// The point comes from a hash of the path's stream state, so no extra number is drawn (Area
// replays stay aligned); a punctual light keeps the centre ray.
void RTApplyLightPoint(GPULight light,float3 p,uint rng,inout float3 l,inout float distance)
{
    const float R=asfloat(light.sourceRadiusBits);
    if(!(R>0) || !(distance>R)) return;
    uint h=rng*747796405u+2891336453u;
    h=((h>>((h>>28u)+4u))^h)*277803737u; h=(h>>22u)^h;
    const float2 xi=float2(h&0xffffu,h>>16)*(1.0/65536.0);
    const float3 w=l;
    const float3 t=normalize(abs(w.y)<.999?cross(w,float3(0,1,0)):cross(w,float3(1,0,0)));
    const float3 b=cross(w,t);
    const float radius=R*sqrt(xi.x), phi=6.28318530718*xi.y;
    const float3 toPoint=light.position+radius*(cos(phi)*t+sin(phi)*b)-p;
    distance=length(toPoint);
    l=toPoint/distance;
}
float3 RTLocalLights(float3 p, float3 n, float3 v, float3 albedo, float rough, float metal, float reflectance,
    inout uint rng,bool recordSpec,out float specEnergy,out float lightDistance)
{
    specEnergy=lightDistance=0;
    if (lightCount == 0 || lightsIdx == 0) return 0;
    StructuredBuffer<GPULight> lights = ResourceDescriptorHeap[lightsIdx];
    // Preserve the random dimension even if all entries are inactive.
    float select=RTRandom(rng);
    uint index; float pdf;
    if(!RTSelectLocalLight(select,index,pdf)) return 0;
    GPULight light = lights[index];
    float3 toLight = light.position-p;
    float distance = length(toLight);
    if (distance <= 0.002 || distance >= light.radius) return 0;
    float3 l = toLight/distance;
    if ((dot(n,l) <= 0 && !g_thin.active) || g_glass.inside) return 0;
    // The cone test takes the NORMALISED surface->light vector (light_eval.hlsli).
    float3 contribution = RTEvaluate(n,v,l,albedo,rough,metal,reflectance) *
        light.color * light.intensity * EvalLocalLightAttenuation(light,l,distance);
    // Nothing to shadow (outside a spot cone, a black lobe): no ray.
    if(!any(contribution>0)) return 0;
    float3 visibilityL=l; float visibilityDistance=distance;
    RTApplyLightPoint(light,p,rng,visibilityL,visibilityDistance);
    const float3 visibility=RTVisibility(p,n,visibilityL,visibilityDistance-0.002);
    if(recordSpec)
    {
        specEnergy=RRLuminance(RRSpecular(n,v,l,albedo,rough,metal,reflectance)*
            light.color*light.intensity*EvalLocalLightAttenuation(light,l,distance)*visibility/pdf);
        lightDistance=distance;
    }
    return contribution * visibility / pdf;
}
// The continuation's next-event estimate: `candidates` lights by power
// (RTSelectLocalLight), resampled by the luminance of their unshadowed contribution; the
// survivor gets the one shadow ray, weighted sum(w) / (K target). Unbiased - every light that
// can light the vertex has a positive target. With ~200 lamps one pick by power is mostly a
// lamp out of range or behind the surface: on the glass reflection that variance is the
// grain REBLUR cannot blur off a mirror.
float3 RTLocalLightsRIS(float3 p, float3 n, float3 v, float3 albedo, float rough, float metal, float reflectance,
    inout uint rng, uint candidates)
{
    if (lightCount == 0 || lightsIdx == 0) return 0;
    StructuredBuffer<GPULight> lights = ResourceDescriptorHeap[lightsIdx];
    float wSum=0, chosenTarget=0, chosenDistance=0;
    float3 chosenF=0, chosenL=0;
    uint chosen=0;
    [loop] for(uint k=0;k<candidates;++k)
    {
        const float select=RTRandom(rng), pick=RTRandom(rng);
        uint index; float pdf;
        if(!RTSelectLocalLight(select,index,pdf)) continue;
        const GPULight light=lights[index];
        const float3 toLight=light.position-p;
        const float distance=length(toLight);
        if(distance<=0.002 || distance>=light.radius) continue;
        const float3 l=toLight/distance;
        if((dot(n,l)<=0 && !g_thin.active) || g_glass.inside) continue;
        const float3 f=RTEvaluate(n,v,l,albedo,rough,metal,reflectance)*
            light.color*light.intensity*EvalLocalLightAttenuation(light,l,distance);
        const float target=Luminance(f);
        if(!(target>0)) continue;
        const float w=target/pdf;
        wSum+=w;
        if(pick*wSum<w) { chosen=index; chosenF=f; chosenTarget=target; chosenL=l; chosenDistance=distance; }
    }
    if(!(wSum>0)) return 0;
    float3 visibilityL=chosenL; float visibilityDistance=chosenDistance;
    RTApplyLightPoint(lights[chosen],p,rng,visibilityL,visibilityDistance);
    const float3 visibility=RTVisibility(p,n,visibilityL,visibilityDistance-0.002);
    return chosenF*visibility*wSum/(float(candidates)*chosenTarget);
}

#if PT_MEDIA
// ==== Medium vertices ====
// A vertex in the fog scatters through the Henyey-Greenstein phase function of the source it
// collided with (asymmetry g); d is the way the path arrives (core/participating_media.hlsli's
// convention). Its next-event estimates are a surface's (RTSun, RTLocalLights) with the phase
// function for the BSDF and the light's fog scale; the shadow rays carry the glass and the fog.
float3 PTMediumSun(float3 x,float3 d,float g,inout uint rng)
{
    if((flags&4u) || !any(sunColor>0)) return 0;
    const float c=lerp(sunCosRadius,1.0,RTRandom(rng));
    const float phi=2*PI*RTRandom(rng), st=sqrt(saturate(1-c*c));
    if(!(sunMediumScale>0)) return 0;
    const float3 l=RTBasis(normalize(-sunDirection),float3(st*cos(phi),st*sin(phi),c));
    const float pdf=1.0/(2*PI*(1-sunCosRadius));
    const float phase=MediumPhaseHG(dot(d,l),g);
    const float3 radiance=sunColor*sunMediumScale/(PI*(1-sunCosRadius*sunCosRadius));
    return phase*radiance*RTVisibility(x,l,l,10000)*RTMis(pdf,phase)/pdf;
}
float3 PTMediumLocal(float3 x,float3 d,float g,inout uint rng)
{
    if(lightCount==0 || lightsIdx==0) return 0;
    StructuredBuffer<GPULight> lights=ResourceDescriptorHeap[lightsIdx];
    const float select=RTRandom(rng);
    uint index; float pdf;
    if(!RTSelectLocalLight(select,index,pdf)) return 0;
    const GPULight light=lights[index];
    const float3 toLight=light.position-x;
    const float distance=length(toLight);
    if(distance<=0.002 || distance>=light.radius) return 0;
    const float3 l=toLight/distance;
    const float3 contribution=MediumPhaseHG(dot(d,l),g)*light.color*light.intensity*PTLightMediumScale(index)*
        EvalLocalLightAttenuation(light,l,distance);
    if(!any(contribution>0)) return 0;
    float3 visibilityL=l; float visibilityDistance=distance;
    RTApplyLightPoint(light,x,rng,visibilityL,visibilityDistance);
    return contribution*RTVisibility(x,visibilityL,visibilityL,visibilityDistance-0.002)/pdf;
}
// The fog on the segment o + t l, t in [0, tEnd) (kMediumFar: to the sky), outside every solid:
// its first collision by decomposition tracking. There the path's next vertex is a medium vertex
// at p with its source's g; the source's emission is added and its albedo weighs the throughput.
bool PTMediumCollide(float3 o,float3 l,float tEnd,inout float3 p,inout float g,inout float3 throughput,
    inout float3 value)
{
    if(mediumIdx==0) return false;
    const MediumSet ms=PTMediumSet();
    const MediumCollision c=MediumSampleCollision(ms,o,l,0,tEnd,g_mediumRng);
    if(c.source==kMediumSourceNone) return false;
    float3 albedo,emission;
    MediumSourceProps(ms,c.source,albedo,emission,g);
    value+=throughput*emission;
    throughput*=albedo;
    p=o+l*c.t;
    return true;
}

// ==== The camera segment's fog (FogCamera) ====
// The fog between the camera and a GBuffer surface at tS along d (kMediumFar: the sky):
// PTCameraSegment returns its transmittance - the analytic sources exactly, the local volumes by
// ratio tracking - and one vertex in it, where a path starts that carries the light the fog
// scatters toward the camera. The vertex's distance is drawn from a mixture, weighed by the
// mixture's density (one-sample MIS, the balance heuristic):
//   the global medium's and the height fog's free flights, each conditioned on a collision before
//   tS, in proportion to their opacities on the segment;
//   a uniform distance in one local volume, by its marched opacity;
//   equi-angular sampling toward one local light by power (Kulla and Fajardo 2012), half the
//   samples when there is one (not with pass.pathTrace.mediaEquiAngular off).
// The vertex then takes a source in proportion to its sigma_t there.
struct PTCameraVertex
{
    float3 x; float g;
    float3 weight;     // the path's throughput at the vertex
    float3 emission;   // its source's emission toward the camera, weighted alike
    bool valid;
};
static bool g_cameraFog=false;   // FogCamera: RTTraceSample traces the camera segment's fog
static float g_cameraT=1;        // ... and leaves its transmittance here
float PTLocalOpacity(MediumLocal v,float3 o,float3 d,float ta,float tb)
{
    return 1-exp(-min(MediumLocalDepthMarch(v,o,d,ta,tb),80.0));
}
float PTCameraSegment(float3 o,float3 d,float tS,inout uint rng,out PTCameraVertex vtx)
{
    vtx=(PTCameraVertex)0;
    const MediumSet ms=PTMediumSet();
    const float T=MediumTransmittance(ms,o,d,0,tS,g_mediumRng);
    // A fixed count of draws, so the stream's dimensions do not depend on the fog.
    const float uLight=RTRandom(rng), uStrategy=RTRandom(rng), uDistance=RTRandom(rng), uSource=RTRandom(rng);
    const float opGlobal=1-exp(-min(MediumGlobalDepth(ms.global,o,d,0,tS),80.0));
    const float opHeight=1-exp(-min(MediumHeightDepth(ms.height,o,d,0,tS),80.0));
    float opLocals=0;
    for(uint i=0;i<ms.localCount;++i)
    {
        const MediumLocal v=MediumLoadLocal(i);
        float ta,tb;
        if(MediumLocalInterval(v,o,d,0,tS,ta,tb)) opLocals+=PTLocalOpacity(v,o,d,ta,tb);
    }
    const float opSum=opGlobal+opHeight+opLocals;
    if(!(opSum>0)) return T;
    float3 lightPos=0;
    bool equiAngular=false;
    if((mediaFlags&2u) && lightCount!=0 && lightsIdx!=0)
    {
        uint index; float pdfLight;
        if(RTSelectLocalLight(uLight,index,pdfLight))
        {
            StructuredBuffer<GPULight> lights=ResourceDescriptorHeap[lightsIdx];
            lightPos=lights[index].position;
            equiAngular=PTLightMediumScale(index)>0;
        }
    }
    const float cEqui=equiAngular?.5:0;
    const float cGlobal=(1-cEqui)*opGlobal/opSum, cHeight=(1-cEqui)*opHeight/opSum, cLocals=(1-cEqui)*opLocals/opSum;
    float t=kMediumFar;
    if(uStrategy<cGlobal)
        t=MediumGlobalFreeFlight(ms.global,o,d,0,tS,-log(1-uDistance*opGlobal));
    else if(uStrategy<cGlobal+cHeight)
        t=MediumHeightFreeFlight(ms.height,o,d,0,tS,-log(1-uDistance*opHeight));
    else if(uStrategy<cGlobal+cHeight+cLocals)
    {
        float u=uDistance*opLocals;
        for(uint i=0;i<ms.localCount;++i)
        {
            const MediumLocal v=MediumLoadLocal(i);
            float ta,tb;
            if(!MediumLocalInterval(v,o,d,0,tS,ta,tb)) continue;
            const float op=PTLocalOpacity(v,o,d,ta,tb);
            if(u<op) { t=lerp(ta,tb,u/op); break; }
            u-=op;
        }
    }
    else
    {
        float pdfUnused;
        t=MediumSampleEquiAngular(o,d,0,tS,lightPos,uDistance,pdfUnused);
    }
    if(!(t<tS)) return T;
    // The mixture's density at t.
    float pdf=0;
    if(cGlobal>0)
        pdf+=cGlobal*MediumGlobalSigmaTAt(ms.global,o+d*t)*exp(-min(MediumGlobalDepth(ms.global,o,d,0,t),80.0))/opGlobal;
    if(cHeight>0)
        pdf+=cHeight*MediumHeightSigmaT(ms.height,o.y+d.y*t)*exp(-min(MediumHeightDepth(ms.height,o,d,0,t),80.0))/opHeight;
    if(cLocals>0)
        for(uint i=0;i<ms.localCount;++i)
        {
            const MediumLocal v=MediumLoadLocal(i);
            float ta,tb;
            if(MediumLocalInterval(v,o,d,0,tS,ta,tb) && t>=ta && t<=tb)
                pdf+=cLocals*PTLocalOpacity(v,o,d,ta,tb)/(opLocals*(tb-ta));
        }
    if(cEqui>0) pdf+=cEqui*MediumPdfEquiAngular(o,d,0,tS,lightPos,t);
    if(!(pdf>0)) return T;
    // The vertex: its source by sigma_t, an independent estimate of the transmittance to it.
    const float3 x=o+d*t;
    const float sigmaGlobal=MediumGlobalSigmaTAt(ms.global,x), sigmaHeight=MediumHeightSigmaT(ms.height,x.y);
    float sigmaT=sigmaGlobal+sigmaHeight;
    for(uint i=0;i<ms.localCount;++i) sigmaT+=MediumLocalSigmaT(MediumLoadLocal(i),x);
    if(!(sigmaT>0)) return T;
    float pick=uSource*sigmaT;
    uint source=kMediumSourceGlobal;
    if(pick>=sigmaGlobal)
    {
        pick-=sigmaGlobal;
        source=kMediumSourceHeight;
        if(pick>=sigmaHeight)
        {
            pick-=sigmaHeight;
            for(uint i=0;i<ms.localCount;++i)
            {
                source=kMediumSourceLocal0+i;
                const float sigmaLocal=MediumLocalSigmaT(MediumLoadLocal(i),x);
                if(pick<sigmaLocal) break;
                pick-=sigmaLocal;
            }
        }
    }
    float3 albedo,emission; float g;
    MediumSourceProps(ms,source,albedo,emission,g);
    const float weight=MediumTransmittance(ms,o,d,0,t,g_mediumRng)*sigmaT/pdf;
    vtx.x=x; vtx.g=g; vtx.weight=weight*albedo; vtx.emission=weight*emission; vtx.valid=true;
    return T;
}
#endif

float3 RTCameraEnvironment(float3 direction)
{
    float3 value=0;
    if(flags&2u) value=constantEnvironment;
    else if(skyIdx!=0)
    {
        TextureCube<float4> sky=ResourceDescriptorHeap[skyIdx];
        value=sky.SampleLevel(gSampWrap,direction,0).rgb*environmentScale;
    }
    if(any(sunColor>0) && dot(direction,normalize(-sunDirection))>=sunCosRadius)
        value+=sunColor/(PI*(1-sunCosRadius*sunCosRadius));
    return value;
}
void RTStoreSample(uint2 px,float3 value)
{
    if (!all(isfinite(value))) CountRay(4);
    float3 mean = value;
    const bool accumulate = spp != 0 && !(flags & (32u|4096u|8192u));
    if (accumulate) mean = Accumulation[px].rgb + (value-Accumulation[px].rgb) / (float(spp)+1);
    Accumulation[px] = float4(mean,1);
    Result[px] = float4(mean,1);
    // The main entry's auxOutUav: the preview denoiser's sample moment (PathTraceDenoise.cs.hlsl),
    // the running mean of the samples' luminance squared beside the image's.
    if (auxOutUav != 0)
    {
        RWTexture2D<float> moment = ResourceDescriptorHeap[auxOutUav];
        const float l = Luminance(max(value, 0));
        moment[px] = accumulate ? moment[px] + (l*l - moment[px]) / (float(spp)+1) : l*l;
    }
}

// A thin-lens camera. The path evaluator takes one so the same sample can be evaluated
// under the previous frame's camera (Area v2 reverse shifts).
struct RTCam
{
    float4x4 invVP;
    float3 pos; float lens;
    float3 right; float focus;
    float3 up; float3 forward;
};
RTCam RTCamCurrent()
{
    RTCam c; c.invVP=invViewProj; c.pos=camera; c.lens=lensRadius; c.right=cameraRight;
    c.focus=focusDistance; c.up=cameraUp; c.forward=cameraForward; return c;
}
RTCam RTCamPrevious()
{
    RTCam c; c.invVP=areaPrevInvViewProj; c.pos=areaPrevCamera; c.lens=areaPrevLensRadius; c.right=areaPrevRight;
    c.focus=areaPrevFocus; c.up=areaPrevUp; c.forward=areaPrevForward; return c;
}
float RTPixelConeSpread(float2 uv,float3 direction,float3 lensOrigin,RTCam cam)
{
    float4 nx=mul(float4(uv.x*2-1+2.0/width,1-uv.y*2,1,1),cam.invVP);
    float4 ny=mul(float4(uv.x*2-1,1-uv.y*2+2.0/height,1,1),cam.invVP);
    float3 dx=normalize(nx.xyz/nx.w-cam.pos),dy=normalize(ny.xyz/ny.w-cam.pos);
    if(cam.lens<=0 || (flags&33554432u))
        return max(length(dx-direction),length(dy-direction));
    // Hold the actual lens sample fixed while differentiating film position.
    // Work relative to the camera, avoiding subtraction of world-space foci.
    float3 focus=direction*(cam.focus/max(dot(direction,cam.forward),1e-6));
    float3 fx=dx*(cam.focus/max(dot(dx,cam.forward),1e-6));
    float3 fy=dy*(cam.focus/max(dot(dy,cam.forward),1e-6));
    float3 toFocus=focus-(lensOrigin-cam.pos);
    float distance=length(toFocus);
    float3 rayDirection=toFocus/max(distance,1e-8);
    float3 deltaX=fx-focus,deltaY=fy-focus;
    // Derivative of normalization, rather than subtracting nearly equal unit
    // vectors. This remains usable for a large lens and very short focus.
    return max(length(deltaX-rayDirection*dot(rayDirection,deltaX)),
               length(deltaY-rayDirection*dot(rayDirection,deltaY)))/max(distance,1e-8);
}
float RTPixelConeSpread(float2 uv,float3 direction,float3 lensOrigin)
{ return RTPixelConeSpread(uv,direction,lensOrigin,RTCamCurrent()); }
void RTLensConeProbe()
{
    const float2 films[3]={float2(.5,.5),float2(.25,.25),float2(.75,.75)};
    const float2 lenses[3]={float2(0,0),float2(.8,.3),float2(-.4,.7)};
    for(uint i=0;i<9;++i)
    {
        float2 uv=films[i/3],lens=lenses[i%3];
        float3 direction=normalize(WorldPosFromDepth(uv,1,invViewProj)-camera);
        float3 origin=camera+lensRadius*(lens.x*cameraRight+lens.y*cameraUp);
        Counters[7+2*7*128+16*32+15+i]=asuint(RTPixelConeSpread(uv,direction,origin));
    }
}

struct RTAreaPrimary
{
    uint4 hit; // InstanceID+1, primitive/front-face bit, float barycentrics.
    uint4 geometry; // Float world position and oct16 geometric normal.
};
#include "core/packing.hlsli"   // OctEncode, OctDecode
uint RTAreaPackNormal(float3 n)
{
    uint2 q=uint2(round(saturate(OctEncode(n)*.5+.5)*65535));
    return q.x|(q.y<<16);
}
// Audit only identity mappings into the same sample domain. Spatial canonical
// reads use the current temporal bank; temporal reads require unchanged camera.
void RTAreaAuditPrimary(uint2 px,RTAreaPrimary replay)
{
    if(!(flags&4194304u)) return;
    Texture2D<uint4> hits=ResourceDescriptorHeap[areaPreviousPrimaryIdx];
    Texture2D<uint4> geometry=ResourceDescriptorHeap[areaPreviousGeometryIdx];
    uint4 h=hits.Load(int3(px,0)),g=geometry.Load(int3(px,0));
    bool mismatch=any(h.xy!=replay.hit.xy);
    if(h.x!=0 && replay.hit.x!=0)
    {
        float3 a=asfloat(g.xyz),b=asfloat(replay.geometry.xyz);
        mismatch=mismatch || any(abs(asfloat(h.zw)-asfloat(replay.hit.zw))>1e-5)
            || any(abs(a-b)>1e-4*max(1.0,length(b))) || g.w!=replay.geometry.w;
    }
    else mismatch=mismatch || any(h!=replay.hit) || any(g!=replay.geometry);
    CountRay(7+2*7*128+16*32+12);
    if(mismatch) CountRay(7+2*7*128+16*32+13);
}

// A complete deterministic path evaluation, including film and lens sampling.
// Replaying a seed must trace the primary again; GBuffer shading is not replay.
float3 RTTraceSample(uint2 px,uint rng,out bool directStore,uint filmRotation,out RTAreaPrimary context,
    bool overrideLens,float2 lensOffset,RTCam cam)
{
    context=(RTAreaPrimary)0;
    directStore=false;
    g_thin=(RTThin)0;
    if(flags&8192u)
    {
        RWTexture2D<float4> da=ResourceDescriptorHeap[rrAlbedoUav];
        RWTexture2D<float4> sa=ResourceDescriptorHeap[rrSpecularUav];
        RWTexture2D<float4> nr=ResourceDescriptorHeap[rrNormalUav];
        RWTexture2D<float> hd=ResourceDescriptorHeap[rrDistanceUav];
        da[px]=0; sa[px]=0; nr[px]=float4(0,0,1,1); hd[px]=0;
    }
    if (rng == 0) rng = 1;
#if PT_MEDIA
    g_mediumRng=MediumRngSeed(rng);
#endif
    float3 p,n,v,albedo;
    // Geometric normal of the current vertex (flag 256, camera primary): the
    // continuation leaves along it and directions below it are the vertex's
    // own surface, a zero. Without the flag it is the shading normal, which
    // lets a normal-mapped or smooth-shaded vertex hit its own surface.
    float3 ng=0;
    float rough,metal,reflectance;
    float3 value=0,throughput=1;
#if PT_MEDIA
    // The current vertex is in the fog (PTMediumCollide, PTCameraSegment): it has no surface and
    // scatters through its source's phase function, asymmetry mediumG. insideSolid: the path is
    // inside a Transmissive solid, whose own medium replaces the fog.
    bool mediumVertex=false, insideSolid=false;
    float mediumG=0;
#endif
    // Geometric transport of the camera footprint. Planar reflection preserves
    // spread; curvature, refraction and BSDF-lobe broadening are not modeled.
    float coneWidth=0,coneSpread=0;
    if(flags&512u)
    {
        CountRay(0);
        float2 film=float2(px)+.5;
        if(flags&1024u)
        {
            float2 sampleUV=float2(RTRandom(rng),RTRandom(rng));
            if(filmRotation!=0)
            {
                // Exact permutation of the RNG's 24-bit film grid. Integer
                // wrapping avoids drift when many fractional shifts compose.
                uint2 rotation=uint2(filmRotation&65535u,filmRotation>>16);
                uint2 grid=(uint2(sampleUV*16777216.0)+(rotation<<8))&16777215u;
                sampleUV=float2(grid)*(1.0/16777216.0);
            }
            film+=sampleUV-.5;
        }
        float2 uv=film/float2(width,height);
        float4 nearPoint=mul(float4(uv.x*2-1,1-uv.y*2,1,1),cam.invVP);
        float3 direction=normalize(nearPoint.xyz/nearPoint.w-cam.pos);
        RayDesc ray; ray.Origin=cam.pos; ray.Direction=direction;
        if(cam.lens>0)
        {
            float radius=cam.lens*sqrt(RTRandom(rng)),phi=2*PI*RTRandom(rng);
            float3 focus=cam.pos+direction*(cam.focus/max(dot(direction,cam.forward),1e-6));
            ray.Origin+=radius*(cos(phi)*cam.right+sin(phi)*cam.up);
            if(overrideLens) ray.Origin=cam.pos+lensOffset.x*cam.right+lensOffset.y*cam.up;
            ray.Direction=normalize(focus-ray.Origin);
        }
        ray.TMin=.001; ray.TMax=10000;
        if(flags&67108864u) coneSpread=RTPixelConeSpread(uv,direction,ray.Origin,cam);
        // Opaque instances commit in fixed function, back faces culled (double-sided instances
        // carry TRIANGLE_CULL_DISABLE); only the alpha-tested / blend instances (instance-level
        // non-opaque) reach the candidate loop, whose facing test keeps their single-sided ones.
        // Mask 0x08, the GBuffer's surfaces (DDGISceneAS instanceMask: every main-pass instance but
        // the forward-translucent ones; Dithered keeps it): the camera primary sees what the GBuffer
        // primary sees, and TransparentPass composites glass over the result once, as it does over
        // the GBuffer primary. Do not use 0x01: it shades a pane here as an opaque surface (a mirror
        // hiding the room behind it, alpha-blend poster margins black) that is then drawn a second
        // time on top.
        RayQuery<RAY_FLAG_CULL_BACK_FACING_TRIANGLES> primary;
        primary.TraceRayInline(g_TLAS,RAY_FLAG_NONE,0x08,ray);
        while(primary.Proceed())
            if(primary.CandidateType()==CANDIDATE_NON_OPAQUE_TRIANGLE &&
                (primary.CandidateTriangleFrontFace() || (g_Instances[primary.CandidateInstanceID()].rtFlags&RT_INST_FLAG_DOUBLE_SIDED)) &&
                RTAlphaAcceptCone(primary.CandidateInstanceID(),primary.CandidatePrimitiveIndex(),primary.CandidateTriangleBarycentrics(),
                    primary.CandidateObjectToWorld3x4(),ray.Direction,primary.CandidateTriangleRayT()*coneSpread))
                primary.CommitNonOpaqueTriangleHit();
#if PT_MEDIA
        // The fog along the camera ray: a collision before what it hits (or the sky) is the path's
        // first vertex.
        if(PTMediumCollide(ray.Origin,ray.Direction,
            primary.CommittedStatus()==COMMITTED_TRIANGLE_HIT?primary.CommittedRayT():kMediumFar,
            p,mediumG,throughput,value))
        {
            mediumVertex=true; v=-ray.Direction; n=ray.Direction; albedo=0; rough=1; metal=0; reflectance=0;
        }
        else
        {
#endif
        if(primary.CommittedStatus()!=COMMITTED_TRIANGLE_HIT)
        { return RTCameraEnvironment(ray.Direction); }
        CountRay(5);
        // The table uses committed InstanceID, independent of raster slot epochs.
        // Allocation failure must not index a stale descriptor.
        if(pathMaterialsIdx==0)
        { return 0; }
        StructuredBuffer<MaterialGPUData> pathMaterials=ResourceDescriptorHeap[pathMaterialsIdx];
        MaterialGPUData primaryMaterial=pathMaterials[primary.CommittedInstanceID()];
        CountRay(6);
        RTHitSurface hit;
        bool loaded;
        if(flags&2048u)
        {
            float spread=RTPixelConeSpread(uv,direction,ray.Origin,cam);
            coneSpread=spread;
            coneWidth=primary.CommittedRayT()*spread;
            loaded=RTLoadHitSurfaceCone(primary.CommittedInstanceID(),primary.CommittedPrimitiveIndex(),
                primary.CommittedTriangleBarycentrics(),primary.CommittedObjectToWorld3x4(),primary.CommittedWorldToObject3x4(),
                ray.Origin+ray.Direction*primary.CommittedRayT(),primary.CommittedTriangleFrontFace(),
                primary.CommittedRayT()*spread,ray.Direction,hit,primaryMaterial.normalStrength);
        }
        else loaded=RTLoadHitSurface(primary.CommittedInstanceID(),primary.CommittedPrimitiveIndex(),
            primary.CommittedTriangleBarycentrics(),primary.CommittedObjectToWorld3x4(),primary.CommittedWorldToObject3x4(),
            ray.Origin+ray.Direction*primary.CommittedRayT(),primary.CommittedTriangleFrontFace(),0,hit,primaryMaterial.normalStrength);
        // Unloaded hits and unresolved custom proxies fall back to the environment. Such pixels
        // are not a material-reference oracle; that needs the material fully published.
        if(!loaded || !hit.trusted)
        { return RTCameraEnvironment(ray.Direction); }
        if(flags&4194304u)
        {
            context.hit=uint4(primary.CommittedInstanceID()+1,
                primary.CommittedPrimitiveIndex()|(primary.CommittedTriangleFrontFace()?0u:0x80000000u),
                asuint(primary.CommittedTriangleBarycentrics()));
            context.geometry=uint4(asuint(hit.P),RTAreaPackNormal(hit.Ngeo));
        }
        p=hit.P; n=hit.Nshade; ng=hit.Ngeo; v=-ray.Direction; albedo=hit.albedo;
        rough=hit.rough; metal=hit.metal; reflectance=hit.reflectance;
        value=hit.emissive;
        if(hit.unlit) return value+albedo;
        g_thin=PTThinAt(primaryMaterial,albedo,metal);
#if PT_MEDIA
        }
#endif
    }
    else
    {
#if PT_MEDIA
    if(g_cameraFog)
    {
        // FogCamera: the camera segment to this pixel's GBuffer surface, or to the sky; the path
        // starts at a vertex in its fog, or there is none.
        Texture2D<float> fogDepthTex=ResourceDescriptorHeap[depthIdx];
        const float fogDepth=fogDepthTex.Load(int3(px,0));
        const float3 fogEnd=WorldPosFromDepth((float2(px)+.5)/float2(width,height),fogDepth>0?fogDepth:1,invViewProj);
        const float3 fogDir=normalize(fogEnd-camera);
        PTCameraVertex cv;
        g_cameraT=PTCameraSegment(camera,fogDir,fogDepth>0?length(fogEnd-camera):kMediumFar,rng,cv);
        if(!cv.valid) return 0;
        p=cv.x; v=-fogDir; n=fogDir; albedo=0; rough=1; metal=0; reflectance=0;
        mediumVertex=true; mediumG=cv.g; throughput=cv.weight; value=cv.emission;
    }
    else
    {
#endif
    Texture2D<float> depthTex = ResourceDescriptorHeap[depthIdx];
    float depth = depthTex.Load(int3(px,0));
    if (depth <= 0) { directStore=true; return 0; }
    Texture2D<float4> albedoTex = ResourceDescriptorHeap[albedoIdx];
    Texture2D<float4> normalTex = ResourceDescriptorHeap[normalIdx];
    Texture2D<float4> surfaceTex = ResourceDescriptorHeap[surfaceIdx];
    float2 uv = (float2(px)+0.5) / float2(width,height);
    p = WorldPosFromDepth(uv, depth, invViewProj);
    if(flags&2097152u)
    {
        float4 nx=mul(float4(uv.x*2-1+2.0/width,1-uv.y*2,1,1),invViewProj);
        float4 ny=mul(float4(uv.x*2-1,1-uv.y*2+2.0/height,1,1),invViewProj);
        float3 direction=normalize(p-camera);
        coneSpread=max(length(normalize(nx.xyz/nx.w-camera)-direction),length(normalize(ny.xyz/ny.w-camera)-direction));
        coneWidth=length(p-camera)*coneSpread;
    }
    n = normalize(normalTex.Load(int3(px,0)).xyz*2-1);
    v = normalize(camera-p);
    albedo = albedoTex.Load(int3(px,0)).rgb;
    if (materialsIdx!=0)
    {
        StructuredBuffer<MaterialGPUData> materials=ResourceDescriptorHeap[materialsIdx];
        uint material=GBufferDecodeMatIdx(normalTex.Load(int3(px,0)).a);
        if (materials[material].customShadingModel==SHADING_MODEL_UNLIT)
        {
            // Primary emission is already in the GBuffer HDR seed. Unlit adds
            // base color exactly once and has no scattering continuation.
            directStore=true; return albedo;
        }
    }
    CountRay(0);
    float4 surf = surfaceTex.Load(int3(px,0));
    rough = max(surf.r,RT_ROUGHNESS_FLOOR); metal = surf.g; reflectance = surf.a;
    if (materialsIdx!=0)
    {
        StructuredBuffer<MaterialGPUData> materials=ResourceDescriptorHeap[materialsIdx];
        g_thin=PTThinAt(materials[GBufferDecodeMatIdx(normalTex.Load(int3(px,0)).a)],albedo,metal);
    }
#if PT_MEDIA
    }
#endif
    }
    if(flags&8192u)
    {
        RWTexture2D<float4> da=ResourceDescriptorHeap[rrAlbedoUav];
        RWTexture2D<float4> sa=ResourceDescriptorHeap[rrSpecularUav];
        RWTexture2D<float4> nr=ResourceDescriptorHeap[rrNormalUav];
        float3 f0=lerp((.16*reflectance*reflectance).xxx,albedo,metal);
        float2 ab=EnvBRDFApprox(rough,saturate(dot(n,v)));
        // Engine split-sum convention, linear reflectance; no NRD demodulation. Reflectances, so
        // clamp to [0,1]: a base colour above 1 (some lamp materials reach 32) makes DLSS-RR's
        // model D write non-finite texels at 3x upscaling.
        da[px]=float4(saturate(albedo*(1-metal)),1);
        sa[px]=float4((flags&1u)?float3(0,0,0):saturate(f0*ab.x+ab.y),1);
        nr[px]=float4(n,rough);
    }
    float rrDirectEnergy=0,rrDirectMoment=0,rrContinuationDistance=0;
    float3 rrPrimaryValue=0,rrSpecularFraction=0;
    // What glass adds to a segment (RTGlassSample): bsdfPdf, the density of its direction at the
    // last vertex a light sample could also reach it from; sunMode, how a miss adds the sun disk
    // (0 weighed against that light sample, 1 whole: a smooth glass reflection, which no light
    // sample reaches, 2 not: refracted light, which the shadow rays carry); mediumSigma, the
    // absorption of the solid the path is inside.
    float bsdfPdf=0;
    uint sunMode=0;
    float3 mediumSigma=0;
    g_glass=(RTGlass)0;
    uint bounce = 0;
    [loop] while (true)
    {
        bool recordSpec=(flags&8192u) && bounce==0;
        float sunSpec=0,localSpec=0,localDistance=0;
        float3 sun=0,local=0;
        if (!(ptLastNeeOff!=0 && maxBounces!=0 && bounce>=maxBounces))
        {
#if PT_MEDIA
            if (mediumVertex)
            {
                sun=PTMediumSun(p,-v,mediumG,rng);
                local=PTMediumLocal(p,-v,mediumG,rng);
            }
            else
            {
#endif
            sun=RTSun(p,n,v,albedo,rough,metal,reflectance,rng,recordSpec,sunSpec);
            local=RTLocalLights(p,n,v,albedo,rough,metal,reflectance,rng,recordSpec,localSpec,localDistance);
#if PT_MEDIA
            }
#endif
        }
        value += throughput * (sun+local);
        if(recordSpec)
        {
            rrDirectEnergy=sunSpec+localSpec;
            rrDirectMoment=sunSpec*10000+localSpec*localDistance;
            rrPrimaryValue=value;
        }
        // Direct-light oracle: excludes all secondary NEE/emission/environment.
        if ((flags & 64u) != 0) break;
#if PT_MEDIA
        // Single scattering: a medium vertex ends its path after its estimates.
        if (mediumVertex && (mediaFlags & 1u)) break;
#endif
        if (maxBounces != 0 && bounce >= maxBounces) break;
        if (bounce >= max(ptRRStart,1u))
        {
            float survival = clamp(max(throughput.x,max(throughput.y,throughput.z)),0.05,0.95);
            if (RTRandom(rng) >= survival) break;
            throughput /= survival;
        }
        float3 l, weight;
        const bool glassVertex=g_glass.active;
#if PT_MEDIA
        // A medium vertex samples its phase function exactly: the weight is 1.
        if (mediumVertex)
        {
            l=MediumSampleHG(-v,mediumG,float2(RTRandom(rng),RTRandom(rng)));
            weight=1;
        }
        else
#endif
        if ((flags & 1u) != 0)
        {
            float u = RTRandom(rng), phi = 2*PI*RTRandom(rng);
            l = RTBasis(n,float3(sqrt(u)*cos(phi),sqrt(u)*sin(phi),sqrt(1-u)));
            weight = albedo;
        }
        else if (glassVertex)
        {
            RTGlassStep step;
            const float3 u=float3(RTRandom(rng),RTRandom(rng),RTRandom(rng));
            if (!RTGlassSample(g_glass,n,v,albedo,rough,metal,reflectance,u,step)) break;
            l=step.l; weight=step.weight;
            if (!step.passThrough) { bsdfPdf=step.pdf; sunMode=step.sunMode; }
            // Into a solid or out of it.
            if (step.transmitted && !g_glass.lobe.thinWalled) mediumSigma=g_glass.inside?0:g_glass.sigma;
#if PT_MEDIA
            if (step.transmitted && !g_glass.lobe.thinWalled) insideSolid=!g_glass.inside;
#endif
        }
        else if (g_thin.active)
        {
            // RTSampleBSDF's three numbers in its order; the third picks the lobe behind as well.
            const float2 uDir=float2(RTRandom(rng),RTRandom(rng));
            float pdf;
            if (!RTThinSample(g_thin,n,v,albedo,rough,metal,reflectance,float3(uDir,RTRandom(rng)),l,weight,pdf)) break;
        }
        else if (!RTSampleBSDF(n,v,albedo,rough,metal,reflectance,rng,l,weight)) break;
        if(recordSpec)
            rrSpecularFraction=saturate(RRSpecular(n,v,l,albedo,rough,metal,reflectance)/
                max(RTEvaluate(n,v,l,albedo,rough,metal,reflectance),1e-30));
#if PT_MEDIA
        if (mediumVertex) { bsdfPdf=MediumPhaseHG(dot(-v,l),mediumG); sunMode=0; }
        else
#endif
        if (!glassVertex) { bsdfPdf=RTPdf(n,v,l,rough); sunMode=0; }
        // The next vertex sets its own (RTGlassAtHit, PTThinAt) or none.
        const bool thinBehind=g_thin.active && dot(l,n)<0;
        g_glass=(RTGlass)0;
        g_thin=(RTThin)0;
        throughput *= weight;
        if (!any(throughput > 0)) break;
        CountRay(bounce == 0 ? 1 : 2);
        const bool geoHorizon=(flags&256u) && dot(ng,ng)>0;
        if (!glassVertex && !thinBehind && geoHorizon && dot(l,ng) <= 0) break;
        // A glass vertex leaves on the side l goes to; light through a leaf by its back.
        const float3 sideN=geoHorizon?ng:n;
        const float3 offsetN=glassVertex?(dot(l,ng)>=0?ng:-ng):(thinBehind?-sideN:sideN);
        RayDesc ray; ray.Origin = p+offsetN*0.001; ray.Direction = l; ray.TMin = 0.001; ray.TMax = 10000;
#if PT_MEDIA
        if (mediumVertex) ray.Origin = p;   // no surface to leave
#endif
        // -pt_gi_shell_model: the ReSTIR GI shell's GI ray (RTGI.cs.hlsl) - mask 0x01, no culling
        // (the facing rule below judges a back face), and alpha-blend (translucent) instances pass
        // through, as HR_AlphaAccept(..,false). Stock: mask 0x08, the GBuffer's surfaces, as the
        // camera primary - forward-translucent instances (glass, Additive / Multiply VFX) are not
        // path vertices, the way this path's own shadow rays pass through them. Do not give the stock
        // ray mask 0x01: its candidate test keeps blend instances, so it commits a glass pane as an
        // opaque, mostly metallic mirror and light from a lit room reaches the corridor behind a
        // glass partition directly but never as a bounce. A Transmissive instance (0x10) is a
        // vertex of its own, both faces (the glass section above); -pt_diffuse keeps 0x08.
        const bool shellModel=ptGiShellModel!=0;
        RayQuery<RAY_FLAG_NONE> q;   // back-face cull as for the primary ray; here it is the run-time flag
        q.TraceRayInline(g_TLAS, shellModel?RAY_FLAG_NONE:RAY_FLAG_CULL_BACK_FACING_TRIANGLES,
            shellModel?0x01:((flags&1u)?0x08:0x18), ray);
        while(q.Proceed())
            if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
                (shellModel?!(HR_AlphaFlags(q.CandidateInstanceID())&RT_ALPHA_FLAG_BLEND):
                    (q.CandidateTriangleFrontFace() ||
                     (g_Instances[q.CandidateInstanceID()].rtFlags & (RT_INST_FLAG_DOUBLE_SIDED|RT_INST_FLAG_TRANSMISSIVE)))) &&
                RTAlphaAcceptCone(q.CandidateInstanceID(),q.CandidatePrimitiveIndex(),q.CandidateTriangleBarycentrics(),
                    q.CandidateObjectToWorld3x4(),l,coneWidth+q.CandidateTriangleRayT()*coneSpread))
                q.CommitNonOpaqueTriangleHit();
        if((flags&8192u) && bounce==0)
        {
            rrContinuationDistance=q.CommittedStatus()==COMMITTED_TRIANGLE_HIT?q.CommittedRayT():ray.TMax;
        }
#if PT_MEDIA
        // The fog along the segment, outside every solid: a collision is the next vertex.
        if (!insideSolid && PTMediumCollide(ray.Origin,l,
                q.CommittedStatus()==COMMITTED_TRIANGLE_HIT?q.CommittedRayT():kMediumFar,p,mediumG,throughput,value))
        {
            mediumVertex=true; v=-l; n=l; ng=0;
            ++bounce;
            if (!any(throughput > 0)) break;
            continue;
        }
#endif
        if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        {
            if (sunMode != 2u && any(sunColor > 0) && dot(l,normalize(-sunDirection)) >= sunCosRadius)
            {
                float sunPdf = 1.0 / (2*PI*(1-sunCosRadius));
                float mis = ((flags & 4u) || sunMode == 1u) ? 1.0 : RTMis(bsdfPdf,sunPdf);
#if PT_MEDIA
                value += throughput * sunColor * mis * (mediumVertex ? sunMediumScale : 1.0) /
                         (PI*(1-sunCosRadius*sunCosRadius));
#else
                value += throughput * sunColor * mis / (PI*(1-sunCosRadius*sunCosRadius));
#endif
            }
            if (flags & 2u) value += throughput * constantEnvironment;
            else if (skyIdx != 0)
            {
                TextureCube<float4> sky = ResourceDescriptorHeap[skyIdx];
                value += throughput * sky.SampleLevel(gSampWrap,l,0).rgb * environmentScale;
            }
            break;
        }
        uint id = q.CommittedInstanceID();
        // Inside a solid: what it absorbed on the way.
        throughput*=DielectricTransmittance(mediumSigma,q.CommittedRayT());
        // A solid's surface keeps its outward normal from either side (RTGlassAtHit tells them apart).
        const bool glassHit=!shellModel && !(flags&1u) && PTIsGlass(id);
        const bool glassSolid=glassHit && RTGlassSolid(pathMaterialsIdx,id);
        // Reflection vertices can be outside the primary raster frustum.
        // Use the same InstanceID-indexed snapshot as camera-primary hits.
        float normalStrength=1;
        if(pathMaterialsIdx!=0)
        {
            StructuredBuffer<MaterialGPUData> pathMaterials=ResourceDescriptorHeap[pathMaterialsIdx];
            normalStrength=pathMaterials[id].normalStrength;
        }
        RTHitSurface hit;
        bool loaded;
        if(flags&2097152u)
        {
            coneWidth+=q.CommittedRayT()*coneSpread;
            loaded=RTLoadHitSurfaceCone(id,q.CommittedPrimitiveIndex(),q.CommittedTriangleBarycentrics(),
                q.CommittedObjectToWorld3x4(),q.CommittedWorldToObject3x4(),
                ray.Origin+l*q.CommittedRayT(),shellModel || glassSolid || q.CommittedTriangleFrontFace(),coneWidth,l,hit,normalStrength);
        }
        else loaded=RTLoadHitSurface(id,q.CommittedPrimitiveIndex(),q.CommittedTriangleBarycentrics(),
                              q.CommittedObjectToWorld3x4(),q.CommittedWorldToObject3x4(),
                              ray.Origin+l*q.CommittedRayT(),shellModel || glassSolid || q.CommittedTriangleFrontFace(),0,hit,normalStrength);
        if(!loaded) break;
        // -pt_gi_shell_model: the shell's facing rule on the unflipped
        // geometric normal - a single-sided surface seen from behind ends the
        // path (a zero of the shell's near field; its far field covers the
        // ray), a double-sided one turns to the ray.
        if (shellModel && dot(hit.Ngeo,-l) <= 0)
        {
            if ((hit.flags & RT_INST_FLAG_DOUBLE_SIDED) == 0) break;
            hit.Ngeo=-hit.Ngeo; hit.Nshade=-hit.Nshade;
        }
        if (!hit.trusted)
        {
            // A custom shader without a resolved proxy cannot be traced exactly: terminate with an
            // environment estimate, as the other RT passes do.
            if (flags & 2u) value+=throughput*constantEnvironment;
            else if (skyIdx!=0)
            {
                TextureCube<float4> sky=ResourceDescriptorHeap[skyIdx];
                value+=throughput*sky.SampleLevel(gSampWrap,l,0).rgb*environmentScale;
            }
            break;
        }
        p=hit.P; n=hit.Nshade; ng=hit.Ngeo; v=-l; albedo=hit.albedo;
        value+=throughput*hit.emissive;
#if PT_MEDIA
        mediumVertex=false;
#endif
        if (hit.unlit) { value+=throughput*albedo; break; }
        rough=hit.rough; metal=hit.metal; reflectance=hit.reflectance;
        if (glassHit) g_glass=RTGlassAtHit(pathMaterialsIdx,id,albedo,rough,metal,ng,l);
        else if (pathMaterialsIdx!=0 && !shellModel)
        {
            StructuredBuffer<MaterialGPUData> pathMaterials=ResourceDescriptorHeap[pathMaterialsIdx];
            g_thin=PTThinAt(pathMaterials[id],albedo,metal);
        }
        // -pt_gi_shell_model under -pt_diffuse: the shell's hit reflects the
        // diffuse lobe, albedo*(1-metal) (rt_shade_diffuse.hlsli).
        if (shellModel && (flags & 1u)) albedo*=1-metal;
        ++bounce;
    }
    if(flags&8192u)
    {
        // Include primary NEE paths: a dark continuation miss must not assign
        // the far-plane distance to a bright punctual specular highlight.
        // Moments use only the specular share of each radiance contribution.
        float indirectSpec=RRLuminance(max(value-rrPrimaryValue,0)*rrSpecularFraction);
        float energy=rrDirectEnergy+indirectSpec;
        RWTexture2D<float> hd=ResourceDescriptorHeap[rrDistanceUav];
        hd[px]=energy>1e-20?(rrDirectMoment+indirectSpec*rrContinuationDistance)/energy:0;
    }
    g_glass=(RTGlass)0;
    g_thin=(RTThin)0;
    return value;
}

// The replay domain is the random-number stream, with unit Jacobian. Film and lens
// coordinates are the stream's first dimensions. Scene/camera changes invalidate history on
// the CPU; this overload does no hit reconnection.
float3 RTTraceSample(uint2 px,uint rng,out bool directStore,uint filmRotation,out RTAreaPrimary context,
    bool overrideLens,float2 lensOffset)
{
    return RTTraceSample(px,rng,directStore,filmRotation,context,overrideLens,lensOffset,RTCamCurrent());
}
float3 RTTraceSample(uint2 px,uint rng,out bool directStore,uint filmRotation,out RTAreaPrimary context)
{
    return RTTraceSample(px,rng,directStore,filmRotation,context,false,0);
}
float3 RTTraceSample(uint2 px,uint rng,out bool directStore,uint filmRotation=0)
{
    RTAreaPrimary ignored;
    return RTTraceSample(px,rng,directStore,filmRotation,ignored);
}

float RTAreaTarget(float3 value) { return max(RRLuminance(value),1e-6); }

uint RTAreaFilm(uint2 px)
{
    if(!(flags&65536u)) return 0;
    Texture2D<uint> rotations=ResourceDescriptorHeap[areaPreviousFilmIdx];
    return rotations.Load(int3(px,0));
}

uint2 RTAreaHistoryPixel(uint2 px,uint rng,out uint2 rotation)
{
    rotation=0;
    if(!(flags&131072u)) return px;
    // Center depth only chooses a proposal; it is not the selected path's hit.
    // Each chosen source maps its ENTIRE local film domain by a torus rotation,
    // so bilinear probabilities need no overlap rejection or PDF division.
    Texture2D<float> depth=ResourceDescriptorHeap[depthIdx];
    float d=depth.Load(int3(px,0));
    float2 uv=(float2(px)+.5)/float2(width,height);
    float4 world=mul(float4(uv.x*2-1,1-uv.y*2,d,1),invViewProj);
    float4 clip=mul(float4(world.xyz/world.w,1),areaPreviousViewProj);
    if(d<=0 || abs(world.w)<1e-8 || clip.w<=1e-8 || !all(isfinite(clip)))
    { CountRay(7+2*7*128+16*32+8); return px; }
    float2 minimum=(clip.xy/clip.w*float2(.5,-.5)+.5)*float2(width,height)-.5;
    if(!all(isfinite(minimum)) || any(minimum < -1.0) || any(minimum > float2(width,height)))
    { CountRay(7+2*7*128+16*32+8); return px; }
    float2 fraction=frac(minimum);
    rng^=0xb5297a4du; rng^=rng>>16; rng*=0x7feb352du;
    rng^=rng>>15; rng*=0x846ca68bu; rng^=rng>>16;
    int2 source=int2(floor(minimum))+int2(RTRandom(rng)<fraction.x,RTRandom(rng)<fraction.y);
    if(any(source<0) || any(source>=int2(width,height)))
    { CountRay(7+2*7*128+16*32+8); return px; }
    rotation=(65536u-uint2(round(fraction*65536.0)))&65535u;
    if(any(source!=int2(px))) CountRay(7+2*7*128+16*32+7);
    return uint2(source);
}

uint2 RTAreaFilmGrid(uint pathSeed,uint rotation)
{
    if(pathSeed==0) pathSeed=1;
    uint2 grid=uint2(float2(RTRandom(pathSeed),RTRandom(pathSeed))*16777216.0);
    return (grid+(uint2(rotation&65535u,rotation>>16)<<8))&16777215u;
}

void RTAreaOverlap(uint2 px,uint pathSeed,inout float3 value,inout float target,inout RTAreaPrimary context,
    out float sum,out uint count,out uint selectedSeed,out uint selectedFilm)
{
    int2 base=int2(px); uint2 fraction=0;
    if(flags&131072u)
    {
        Texture2D<float> depth=ResourceDescriptorHeap[depthIdx];
        float d=depth.Load(int3(px,0));
        float2 uv=(float2(px)+.5)/float2(width,height);
        float4 world=mul(float4(uv.x*2-1,1-uv.y*2,d,1),invViewProj);
        float4 clip=mul(float4(world.xyz/world.w,1),areaPreviousViewProj);
        float2 minimum=(clip.xy/clip.w*float2(.5,-.5)+.5)*float2(width,height)-.5;
        if(d>0 && abs(world.w)>=1e-8 && clip.w>1e-8 && all(isfinite(minimum)) &&
            all(minimum>=-1.0) && all(minimum<float2(width,height)))
        {
            base=int2(floor(minimum));
            // Carry rounding into the base: domain tests and film mapping must
            // agree at the quantized pixel boundary, including negative motion.
            uint2 q=uint2(round(frac(minimum)*65536.0));
            base+=int2(q>>16); fraction=q&65535u;
        }
        else CountRay(7+2*7*128+16*32+8);
    }
    Texture2D<uint4> previous=ResourceDescriptorHeap[areaPreviousIdx];
    uint2 freshGrid=RTAreaFilmGrid(pathSeed,0);
    uint2 freshSource=(freshGrid+(fraction<<8))>>24;
    uint freshM=0, maxM=0, historySeed=0, historyFilm=0;
    float historySum=0, historyTarget=1; float3 historyValue=0;
    RTAreaPrimary historyContext=(RTAreaPrimary)0;
    uint rng=pathSeed^0x68e31da4u;
    rng^=rng>>16; rng*=0x7feb352du; rng^=rng>>15; rng*=0x846ca68bu; rng^=rng>>16;
    // First RIS: disjoint source overlap domains cover the reprojected square.
    // Each source estimator integrates its entire unit pixel, so no bilinear
    // alpha or /4 factor belongs in these weights.
    [loop] for(uint i=0;i<4;++i)
    {
        uint2 offset=uint2(i&1u,i>>1); int2 source=base+int2(offset);
        if(any(source<0) || any(source>=int2(width,height))) continue;
        if((flags&524288u) && (source.x&1)==0) continue; // Missing-source validation arm.
        uint4 h=previous.Load(int3(source,0));
        uint m=min(h.y,areaHistoryCap-1); float w=asfloat(h.z);
        if(m==0 || w<=0 || !isfinite(w)) continue;
        maxM=max(maxM,m);
        if(all(offset==freshSource)) freshM=m;
        CountRay(7+2*7*128+16*32+9);
        uint oldFilm=RTAreaFilm(uint2(source));
        int2 shifted=int2(RTAreaFilmGrid(h.x,oldFilm))+int2(offset<<24)-int2(fraction<<8);
        if(any(shifted<0) || any(shifted>=16777216))
        { CountRay(7+2*7*128+16*32+10); continue; }
        uint2 rotation=(uint2(oldFilm&65535u,oldFilm>>16)+65536u-fraction)&65535u;
        uint film=rotation.x|(rotation.y<<16);
        bool ignored; RTAreaPrimary incomingContext;
        float3 incoming=RTTraceSample(px,h.x,ignored,film,incomingContext);
        if(!(flags&131072u) && all(source==int2(px)) && film==oldFilm)
            RTAreaAuditPrimary(px,incomingContext);
        CountRay(7+2*7*128+16*32);
        if(film!=0) CountRay(7+2*7*128+16*32+6);
        if(any(source!=int2(px))) CountRay(7+2*7*128+16*32+7);
        if(!all(isfinite(incoming))) CountRay(4);
        float t=RTAreaTarget(incoming), weight=t*w*(float(m)/(1.0+float(m)));
        historySum+=weight;
        if(RTRandom(rng)*historySum<weight)
        { historyValue=incoming; historyTarget=t; historySeed=h.x; historyFilm=film; historyContext=incomingContext; }
    }
    if(freshM>0) CountRay(7+2*7*128+16*32+11);
    if(flags&1048576u) freshM=maxM; // Intentionally wrong; validates uncovered-support energy gate.
    // Second RIS: canonical owns all uncovered support. Within source i's
    // region its 1/(1+Mi) complements history's Mi/(1+Mi), pointwise.
    sum=target/(1.0+float(freshM))+historySum;
    count=1+maxM; selectedSeed=pathSeed; selectedFilm=0;
    if(RTRandom(rng)*sum<historySum)
    {
        value=historyValue; target=historyTarget;
        selectedSeed=historySeed; selectedFilm=historyFilm; context=historyContext;
        CountRay(7+2*7*128+16*32+1);
    }
}

float3 RTAreaUnpackNormal(uint packed)
{
    return OctDecode(float2(packed&65535u,packed>>16)*(2.0/65535.0)-1);
}
// Reconstruct the same film/lens random dimensions as the path evaluator.
// F and L are camera-relative; the continuation keeps the original RNG state.
void RTAreaCameraSample(uint2 px,uint rng,uint filmRotation,out float3 F,out float3 L)
{
    if(rng==0) rng=1;
    float2 film=float2(px)+.5;
    if(flags&1024u)
    {
        float2 sampleUV=float2(RTRandom(rng),RTRandom(rng));
        if(filmRotation!=0)
        {
            uint2 rotation=uint2(filmRotation&65535u,filmRotation>>16);
            sampleUV=float2((uint2(sampleUV*16777216.0)+(rotation<<8))&16777215u)*(1.0/16777216.0);
        }
        film+=sampleUV-.5;
    }
    float2 uv=film/float2(width,height);
    float3 direction=normalize(WorldPosFromDepth(uv,1,invViewProj)-camera);
    F=direction*(focusDistance/max(dot(direction,cameraForward),1e-6));
    float radius=lensRadius*sqrt(RTRandom(rng)),phi=2*PI*RTRandom(rng);
    L=radius*(cos(phi)*cameraRight+sin(phi)*cameraUp);
}
bool RTAreaReconnectLens(float3 P,float3 n,float3 Fs,float3 Ls,float3 Ft,out float2 lens,out float jacobian)
{
    lens=0; jacobian=1;
    if(lensRadius<=0) return false;
    float z=dot(P,cameraForward),f=focusDistance;
    if(z<=.001 || abs(z-f)<=1e-4*max(z,f)) return false;
    float3 Lt=(z*Ft-f*P)/(z-f);
    lens=float2(dot(Lt,cameraRight),dot(Lt,cameraUp));
    if(!all(isfinite(lens)) || dot(lens,lens)>lensRadius*lensRadius) return false;
    Lt=lens.x*cameraRight+lens.y*cameraUp;
    float3 ds=normalize(P-Ls),dt=normalize(P-Lt);
    float cs=abs(dot(ds,cameraForward)),ct=abs(dot(dt,cameraForward));
    float ns=abs(dot(ds,n)),nt=abs(dot(dt,n));
    if(min(min(cs,ct),min(ns,nt))<1e-4) return false;
    float d0s=length(Fs-Ls),d1s=length(P-Fs),d0t=length(Ft-Lt),d1t=length(P-Ft);
    // Ratio of lens-to-surface area differentials, source divided by target.
    jacobian=(d1s*d1s*cs/(d0s*d0s*ns))*(d0t*d0t*nt/(d1t*d1t*ct));
    if(!isfinite(jacobian) || jacobian<1e-6 || jacobian>1e6) { jacobian=1; return false; }
    if(flags&536870912u) jacobian=1; // Diagnostic falsifier only.
    return true;
}
void RTAreaReconnectProbe()
{
    const float zs[8]={.1,1,1.303,2.6,4,1.3,2.6,1.303};
    const float slopes[8]={0,.7,-.7,.7,-.7,0,0,.7};
    for(uint i=0;i<8;++i)
    {
        float3 n=normalize(slopes[i]*cameraRight+cameraForward);
        float3 Fs=focusDistance*cameraForward;
        float3 Ls=lensRadius*((i==7?-.8:.1)*cameraRight+.05*cameraUp);
        float3 P=Ls+(Fs-Ls)*(dot(n,zs[i]*cameraForward-Ls)/dot(n,Fs-Ls));
        float3 Ft=Fs+focusDistance*((i==6?10:.001)*cameraRight+.0007*cameraUp);
        float2 lens; float jacobian;
        bool valid=RTAreaReconnectLens(P,n,Fs,Ls,Ft,lens,jacobian);
        uint index=7+2*7*128+16*32+28+i*4;
        Counters[index]=valid?1u:0u; Counters[index+1]=asuint(lens.x);
        Counters[index+2]=asuint(lens.y); Counters[index+3]=asuint(jacobian);
    }
}
// Same-frame mapping; temporal history remains random replay. Spatial results
// are not fed back. Unsupported/occluded mappings have zero proposal support.
float3 RTAreaReconnect(uint2 from,uint2 to,uint pathSeed,uint film,RTAreaPrimary source,
    out float target,out float jacobian)
{
    target=0; jacobian=1;
    CountRay(7+2*7*128+16*32+24);
    if(source.hit.x==0 || lensRadius<=0) return 0;
    float3 P=asfloat(source.geometry.xyz)-camera;
    float3 Fs,Ls,Ft,unused;
    RTAreaCameraSample(from,pathSeed,film,Fs,Ls);
    RTAreaCameraSample(to,pathSeed,film,Ft,unused);
    float2 lens;
    if(!RTAreaReconnectLens(P,RTAreaUnpackNormal(source.geometry.w),Fs,Ls,Ft,lens,jacobian)) return 0;
    CountRay(7+2*7*128+16*32+25);
    bool ignored; RTAreaPrimary hit;
    float3 value=RTTraceSample(to,pathSeed,ignored,film,hit,true,lens);
    if(any(hit.hit.xy!=source.hit.xy) || any(abs(asfloat(hit.hit.zw)-asfloat(source.hit.zw))>1e-4)
        || any(abs(asfloat(hit.geometry.xyz)-asfloat(source.geometry.xyz))>1e-4*max(1.0,length(P)))) return 0;
    CountRay(7+2*7*128+16*32+26);
    target=RTAreaTarget(value);
    return value;
}

float3 RTAreaReconnectSpatial(uint2 px,bool cached)
{
    Texture2D<uint4> temporal=ResourceDescriptorHeap[areaPreviousIdx];
    uint4 canonical=temporal.Load(int3(px,0));
    bool ignored;
    uint canonicalFilm=RTAreaFilm(px);
    RTAreaPrimary canonicalContext;
    float3 value;
    if(cached)
    {
        Texture2D<float4> radiance=ResourceDescriptorHeap[areaRadianceIdx];
        Texture2D<uint4> cachedHits=ResourceDescriptorHeap[areaPreviousPrimaryIdx];
        Texture2D<uint4> cachedGeometry=ResourceDescriptorHeap[areaPreviousGeometryIdx];
        value=radiance.Load(int3(px,0)).rgb;
        canonicalContext.hit=cachedHits.Load(int3(px,0));
        canonicalContext.geometry=cachedGeometry.Load(int3(px,0));
    }
    else
    {
        value=RTTraceSample(px,canonical.x,ignored,canonicalFilm,canonicalContext);
        RTAreaAuditPrimary(px,canonicalContext);
    }
    float tc=RTAreaTarget(value), wc=asfloat(canonical.z);
    if(!all(isfinite(value))) CountRay(4);
    // Pixel/frame-dependent, never selected-sample-dependent neighbor selection.
    uint direction=(px.x+3*px.y+seed)&3u;
    int2 offset=direction==0?int2(1,0):direction==1?int2(0,1):direction==2?int2(-1,0):int2(0,-1);
    int2 neighbor=int2(px)+offset;
    if(any(neighbor<0) || any(neighbor>=int2(width,height))) return value*wc;
    uint4 source=temporal.Load(int3(neighbor,0));
    if(source.y==0) return value*wc;
    CountRay(7+2*7*128+16*32+3);
    float3 incoming,reverse;
    float tci,tic,Jin=1,Jout=1;
    Texture2D<uint4> hits=ResourceDescriptorHeap[areaPreviousPrimaryIdx];
    Texture2D<uint4> geometry=ResourceDescriptorHeap[areaPreviousGeometryIdx];
    RTAreaPrimary sourceContext;
    sourceContext.hit=hits.Load(int3(neighbor,0)); sourceContext.geometry=geometry.Load(int3(neighbor,0));
    incoming=RTAreaReconnect(uint2(neighbor),px,source.x,RTAreaFilm(uint2(neighbor)),sourceContext,tci,Jin);
    reverse=RTAreaReconnect(px,uint2(neighbor),canonical.x,canonicalFilm,canonicalContext,tic,Jout);
    CountRay(7+2*7*128+16*32+4);
    if(!all(isfinite(incoming)) || !all(isfinite(reverse))) CountRay(4);
    float ti=asfloat(source.w);
    // Scale the source proposal in BOTH MIS halves. This is a fixed proposal
    // allocation, not a radiance clamp or sample-dependent confidence.
    float sourceM=areaReconnectWeight*float(source.y);
    float mc=float(canonical.y)*tc/(float(canonical.y)*tc+sourceM*tic*Jout);
    float mi=sourceM*ti/(float(canonical.y)*tci*Jin+sourceM*ti);
    // Both balance-MIS halves. W already includes each source's RIS normalization.
    float a=tc*wc*mc, b=tci*asfloat(source.z)*mi*Jin;
    // No spatial history/selected guides are published. Integrate out the
    // final RIS choice instead of adding selection variance to the image.
    if(b>0) CountRay(7+2*7*128+16*32+27);
    return value*(a/tc)+(tci>0?incoming*(b/tci):float3(0,0,0));
}

float3 RTAreaSpatial(uint2 px,uint selection)
{
    Texture2D<uint4> temporal=ResourceDescriptorHeap[areaPreviousIdx];
    uint4 canonical=temporal.Load(int3(px,0));
    bool ignored;
    uint canonicalFilm=RTAreaFilm(px);
    RTAreaPrimary canonicalContext;
    float3 value=RTTraceSample(px,canonical.x,ignored,canonicalFilm,canonicalContext);
    RTAreaAuditPrimary(px,canonicalContext);
    float tc=RTAreaTarget(value), wc=asfloat(canonical.z);
    if(!all(isfinite(value))) CountRay(4);
    // Pixel/frame-dependent, never selected-sample-dependent neighbor selection.
    uint direction=(px.x+3*px.y+seed)&3u;
    int2 offset=direction==0?int2(1,0):direction==1?int2(0,1):direction==2?int2(-1,0):int2(0,-1);
    int2 neighbor=int2(px)+offset;
    if(any(neighbor<0) || any(neighbor>=int2(width,height))) return value*wc;
    uint4 source=temporal.Load(int3(neighbor,0));
    if(source.y==0) return value*wc;
    CountRay(7+2*7*128+16*32+3);
    float3 incoming=RTTraceSample(px,source.x,ignored,RTAreaFilm(uint2(neighbor)));
    CountRay(7+2*7*128+16*32+4);
    float3 reverse=RTTraceSample(uint2(neighbor),canonical.x,ignored,canonicalFilm);
    if(!all(isfinite(incoming)) || !all(isfinite(reverse))) CountRay(4);
    float ti=asfloat(source.w), tci=RTAreaTarget(incoming), tic=RTAreaTarget(reverse);
    float mc=float(canonical.y)*tc/(float(canonical.y)*tc+float(source.y)*tic);
    float mi=float(source.y)*ti/(float(canonical.y)*tci+float(source.y)*ti);
    // Both balance-MIS halves. W already includes each source's RIS normalization.
    float a=tc*wc*mc, b=tci*asfloat(source.z)*mi, sum=a+b;
    selection^=0x63d83595u;
    selection^=selection>>16; selection*=0x7feb352du;
    selection^=selection>>15; selection*=0x846ca68bu;
    selection^=selection>>16;
    if(RTRandom(selection)*sum<b)
    {
        value=incoming; tc=tci;
        CountRay(7+2*7*128+16*32+5);
    }
    return value*(sum/tc);
}

// ==== Area ReSTIR v2 ====
// Zhang, Lin, Kettunen, Yuksel, Wyman 2024 (author reference Source/Modules/AreaReSTIR). A
// sample is x = (continuation seed, film subpixel uv via a torus rotation of the seed's first
// two dimensions, lens uv: the seed's next two dimensions or an explicit override). Two shift
// mappings are MIS-combined with a circle-of-confusion confidence split, as the author's
// ShiftMappingModeInReusing::MIS:
//   RR  random replay: the same x at the destination pixel/camera, J = 1.
//   RC  primary-hit reconnection: keep the primary hit and the subpixel uv,
//       solve the destination lens point that sees the hit through the
//       destination film point, replay the continuation. J is the ratio of
//       lens-to-surface area differentials (Resampling.slang) times the
//       lens-radius ratio of the primary sample space.
// Temporal uses pairwise non-defensive MIS with reverse shifts of the canonical sample into
// the previous camera; spatial uses defensive pairwise weights over K neighbours with a
// confidence that grows as the canonical history shrinks (both as Resampling.slang).
static const uint kAreaV2RC=2u, kAreaV2CameraStatic=4u, kAreaV2Spatial=8u, kAreaV2Blend=16u, kAreaV2NoIdentity=64u;
// Spatial reuse only where the temporal history is short (M < cap/2): at a full history the
// spatial pass buys no measurable noise reduction for its full replays.
static const uint kAreaV2SpatialGate=32u;
bool AreaSpatialSkips(uint M) { return (areaFlags&kAreaV2SpatialGate) && M*2u>=max(areaHistoryCap,1u); }
static const uint kAreaV2Base=7+2*7*128+16*32+64;
// Lens coordinates are unit-disk points stored at full precision: a sample
// must replay exactly the ray its target and contribution weight belong to.
static const float2 kAreaNoLens=float2(4,4);
bool AreaHasLens(float2 disk) { return disk.x<2; }

#include "core/hash.hlsli"
// Hashed per-pixel/frame seed: do not seed the GF(2)-linear xorshift with
// (pixel*A)^(frame*B), or every pixel pair keeps a frame-invariant XOR in every random
// dimension (structured, spatially correlated noise).
uint RTAreaPathSeed(uint2 px)
{
    uint s=HashLowbias32((px.x+px.y*width)^HashLowbias32(seed^0x9e3779b9u));
    return s?s:1u;
}
struct AreaSample { uint seed; uint film; float2 lens; };
uint AreaRotateFilm(uint film,uint2 rotation)
{
    uint2 r=(uint2(film&65535u,film>>16)+rotation)&65535u;
    return r.x|(r.y<<16);
}
uint AreaInverseRotation(uint2 rotation)
{
    uint2 r=(65536u-rotation)&65535u;
    return r.x|(r.y<<16);
}
float2 AreaLensAt(uint idx,uint2 px)
{
    if(idx==0) return kAreaNoLens;
    Texture2D<float2> lenses=ResourceDescriptorHeap[idx];
    return lenses.Load(int3(px,0));
}
float3 AreaTrace(uint2 px,AreaSample x,RTCam cam,out RTAreaPrimary ctx)
{
    bool ignored;
    bool hasLens=AreaHasLens(x.lens) && cam.lens>0;
    float2 offset=hasLens?x.lens*cam.lens:0;
    float3 value=RTTraceSample(px,x.seed,ignored,x.film,ctx,hasLens,offset,cam);
    if(!all(isfinite(value))) { CountRay(4); value=0; }
    return value;
}
bool AreaSamePrimary(RTAreaPrimary a,RTAreaPrimary b)
{
    if(a.hit.x==0 || b.hit.x==0 || any(a.hit.xy!=b.hit.xy)) return false;
    float3 pa=asfloat(a.geometry.xyz), pb=asfloat(b.geometry.xyz);
    return length(pa-pb)<=2e-3*max(1.0,length(pb-camera));
}
// The film focus point and lens point a sample uses under a camera, drawing
// the random dimensions in exactly RTTraceSample's order.
void AreaCameraPoints(uint2 px,AreaSample x,RTCam cam,out float3 Fw,out float3 Lw)
{
    uint rng=x.seed?x.seed:1u;
    float2 film=float2(px)+.5;
    if(flags&1024u)
    {
        float2 sampleUV=float2(RTRandom(rng),RTRandom(rng));
        if(x.film!=0)
        {
            uint2 rotation=uint2(x.film&65535u,x.film>>16);
            sampleUV=float2((uint2(sampleUV*16777216.0)+(rotation<<8))&16777215u)*(1.0/16777216.0);
        }
        film+=sampleUV-.5;
    }
    float2 uv=film/float2(width,height);
    float4 nearPoint=mul(float4(uv.x*2-1,1-uv.y*2,1,1),cam.invVP);
    float3 direction=normalize(nearPoint.xyz/nearPoint.w-cam.pos);
    Fw=cam.pos+direction*(cam.focus/max(dot(direction,cam.forward),1e-6));
    Lw=cam.pos;
    if(cam.lens>0)
    {
        float radius=cam.lens*sqrt(RTRandom(rng)),phi=2*PI*RTRandom(rng);
        float2 offset=radius*float2(cos(phi),sin(phi));
        if(AreaHasLens(x.lens)) offset=x.lens*cam.lens;
        Lw=cam.pos+offset.x*cam.right+offset.y*cam.up;
    }
}
// Lens point on cam's lens plane that sees P through the focus point F.
bool AreaSolveLens(RTCam cam,float3 Pw,float3 Fw,out float2 disk,out float3 Lw)
{
    disk=0; Lw=cam.pos;
    if(cam.lens<=0) return false;
    float3 P=Pw-cam.pos, F=Fw-cam.pos;
    float z=dot(P,cam.forward), f=dot(F,cam.forward);
    if(z<=.001 || abs(z-f)<=1e-4*max(z,f)) return false;
    float3 L=(z*F-f*P)/(z-f);
    float2 lens=float2(dot(L,cam.right),dot(L,cam.up));
    if(!all(isfinite(lens)) || dot(lens,lens)>cam.lens*cam.lens) return false;
    disk=lens/cam.lens; Lw=cam.pos+lens.x*cam.right+lens.y*cam.up;
    return true;
}
// dA(P) / dA(lens) for rays through the fixed focus point F.
float AreaSurfacePerLens(RTCam cam,float3 Pw,float3 n,float3 Fw,float3 Lw)
{
    float3 dir=normalize(Pw-Lw);
    float cl=abs(dot(dir,cam.forward)), cp=abs(dot(dir,n));
    float d0=length(Fw-Lw), d1=length(Pw-Fw);
    return (cl>1e-4 && cp>1e-4 && d0>0) ? d1*d1*cl/(d0*d0*cp) : 0;
}
// Primary-hit reconnection of x (primary hit `source`) from (srcPx, srcCam) to
// (dstPx, dstCam). Target 0 = no image (outside the aperture, degenerate,
// occluded, or the replayed primary lands on another surface).
float3 AreaReconnectShift(uint2 srcPx,RTCam srcCam,uint2 dstPx,RTCam dstCam,AreaSample x,RTAreaPrimary source,
    out float target,out float jacobian,out AreaSample shifted,out RTAreaPrimary ctx)
{
    target=0; jacobian=0; shifted=x; ctx=(RTAreaPrimary)0;
    if(source.hit.x==0 || srcCam.lens<=0 || dstCam.lens<=0) return 0;
    float3 P=asfloat(source.geometry.xyz), n=RTAreaUnpackNormal(source.geometry.w);
    float3 Fs,Ls,Ft,unused;
    AreaCameraPoints(srcPx,x,srcCam,Fs,Ls);
    AreaCameraPoints(dstPx,x,dstCam,Ft,unused);
    float2 disk; float3 Lt;
    if(!AreaSolveLens(dstCam,P,Ft,disk,Lt)) return 0;
    float gs=AreaSurfacePerLens(srcCam,P,n,Fs,Ls), gt=AreaSurfacePerLens(dstCam,P,n,Ft,Lt);
    if(!(gs>0) || !(gt>0)) return 0;
    jacobian=gs/gt*(srcCam.lens*srcCam.lens)/(dstCam.lens*dstCam.lens);
    if(!isfinite(jacobian) || jacobian<1e-6 || jacobian>1e6) { jacobian=0; return 0; }
    shifted.lens=disk;
    float3 value=AreaTrace(dstPx,shifted,dstCam,ctx);
    if(!AreaSamePrimary(ctx,source)) { jacobian=0; return 0; }
    target=RTAreaTarget(value);
    return value;
}
// Author's computeConfidenceWeightForTwoShifts (piecewise branch): the random
// replay share of a candidate's confidence from the circle of confusion at
// the pixel CENTRE (never the sample's own hit, which would bias the MIS).
float AreaReplayConfidence(uint2 px,RTCam cam)
{
    if(!(areaFlags&kAreaV2RC) || cam.lens<=0) return 1;
    Texture2D<float> depthTex=ResourceDescriptorHeap[depthIdx];
    float d=depthTex.Load(int3(px,0));
    if(d<=0) return 1;
    float2 uv=(float2(px)+.5)/float2(width,height);
    float3 P=WorldPosFromDepth(uv,d,invViewProj);
    float z=dot(P-cam.pos,cam.forward);
    if(z<=1e-4) return 1;
    float4 a=mul(float4(uv.x*2-1,1-uv.y*2,1,1),cam.invVP);
    float4 b=mul(float4(uv.x*2-1+2.0/width,1-uv.y*2,1,1),cam.invVP);
    float3 da=normalize(a.xyz/a.w-cam.pos), db=normalize(b.xyz/b.w-cam.pos);
    float3 fa=da*(cam.focus/max(dot(da,cam.forward),1e-6)), fb=db*(cam.focus/max(dot(db,cam.forward),1e-6));
    float pixel=length(fb-fa);
    if(!(pixel>0)) return 1;
    float coc=2*cam.lens*abs(z-cam.focus)/z/pixel;
    if(coc<=2) return 1;
    if(coc<=8) return 1-.3*((coc-2)/6);
    if(coc<=32) return .7-.4*((coc-8)/24);
    return max(.2,.3-.1*((coc-32)/468));
}
// The author's pairwise weights (Resampling.slang), verbatim algebra.
float AreaMisNonDefensiveNonCanonical(float cSum,float c0,float pi,float c1,float pc,float outJ)
{ float p=pi*outJ; return pc==0?0:c0*p/((cSum-c1)*p+c1*pc); }
float AreaMisNonDefensiveCanonical(float cSum,float c0,float pi,float c1,float pc,float outJ)
{ float p=pi*outJ; return pc==0?0:(c0/(cSum-c1))*c1*pc/((cSum-c1)*p+c1*pc); }
float AreaMisDefensiveNonCanonical(float cSum,float c0,float pi,float c1,float pc,float outJ)
{ float w=(cSum-c1)*pi*outJ; return pc==0?0:(c0/cSum)*w/(w+c1*pc); }
float AreaMisDefensiveCanonical(float cSum,float c0,float pi,float c1,float pc,float outJ)
{ float w=c1*pc; return pc==0?0:(c0/cSum)*w/((cSum-c1)*pi*outJ+w); }

struct AreaSelection
{
    float wSum; float3 estimate;
    AreaSample x; float target; float3 value; RTAreaPrimary ctx;
    uint rng;
};
// Stream one candidate. `estimate` integrates the final RIS choice out
// (Rao-Blackwellised conditional mean); the SELECTED sample is what the
// reservoir carries forward.
void AreaStream(inout AreaSelection s,float weight,AreaSample x,float target,float3 value,RTAreaPrimary ctx)
{
    if(!(weight>0) || !isfinite(weight) || !(target>0)) return;
    s.wSum+=weight;
    s.estimate+=value*(weight/target);
    if(RTRandom(s.rng)*s.wSum<weight) { s.x=x; s.target=target; s.value=value; s.ctx=ctx; }
}
// Progressive mode: Accumulation holds the running mean of the canonical
// samples, which are independent every frame, and the displayed value blends
// the ReSTIR estimate into it as the static sample count grows. Both are
// unbiased, and plain accumulation converges faster than averaging correlated
// ReSTIR outputs. Single-sample, NRD-reference and RR modes consume the
// per-frame estimate itself (Accumulation is their input signal).
bool AreaProgressive() { return (areaFlags&kAreaV2Blend) && !(flags&(32u|4096u|8192u)); }
void AreaPublish(uint2 px,float3 restir)
{
    float3 value=restir;
    if(AreaProgressive())
        value=lerp(restir,Accumulation[px].rgb,saturate(float(spp)/float(2*max(areaHistoryCap,1u))));
    if(!all(isfinite(value))) { CountRay(4); value=0; }
    if(!AreaProgressive()) Accumulation[px]=float4(value,1);
    Result[px]=float4(value,1);
}

// px's reprojected centre in the previous film, as the previous pixel whose
// centre is at or up-left of it plus a 16-bit fraction towards the next one.
// No motion, or no usable reprojection (sky, off screen): px itself.
void AreaFootprint(uint2 px,out int2 cell,out uint2 f16)
{
    cell=int2(px); f16=0;
    if(!(flags&131072u)) return;
    Texture2D<float> depth=ResourceDescriptorHeap[depthIdx];
    float d=depth.Load(int3(px,0));
    float2 uv=(float2(px)+.5)/float2(width,height);
    float4 world=mul(float4(uv.x*2-1,1-uv.y*2,d,1),invViewProj);
    float4 clip=mul(float4(world.xyz/world.w,1),areaPreviousViewProj);
    if(d<=0 || abs(world.w)<1e-8 || clip.w<=1e-8 || !all(isfinite(clip))) return;
    float2 centre=(clip.xy/clip.w*float2(.5,-.5)+.5)*float2(width,height)-.5;
    if(!all(isfinite(centre)) || any(centre< -1.0) || any(centre>float2(width,height))) return;
    const float2 base=floor(centre);
    const uint2 f=uint2(round((centre-base)*65536.0));
    cell=int2(base)+int2(f>>16); f16=f&65535u;
}

void AreaV2Temporal(uint2 px)
{
    if(any(px>=uint2(width,height))) return;
    const RTCam cur=RTCamCurrent(), prev=RTCamPrevious();
    const uint pathSeed=RTAreaPathSeed(px);
    AreaSample xc; xc.seed=pathSeed; xc.film=0; xc.lens=kAreaNoLens;
    RTAreaPrimary ctxC;
    const float3 valueC=AreaTrace(px,xc,cur,ctxC);
    const float tc=RTAreaTarget(valueC);
    if(AreaProgressive())
    {
        float3 mean=valueC;
        if(spp!=0) mean=Accumulation[px].rgb+(valueC-Accumulation[px].rgb)/(float(spp)+1);
        Accumulation[px]=float4(mean,1);
    }
    AreaSelection s=(AreaSelection)0;
    s.x=xc; s.target=tc; s.value=valueC; s.ctx=ctxC;
    s.rng=HashLowbias32(pathSeed^0xa511e9b3u)|1u;
    float canonicalWeight=1;
    uint M=1;
    if(areaHistoryValid!=0 && areaHistoryCap>1)
    {
        // One previous pixel under px's reprojected footprint, and the torus
        // translation of its whole film domain onto px's: a bijection with
        // J = 1 that keeps film positions where the pixels overlap. (Taking
        // all four footprint pixels and keeping only samples that land in px
        // is also unbiased, but the random number that land adds more
        // variance than it saves.)
        int2 cell; uint2 f16;
        AreaFootprint(px,cell,f16);
        const float2 f=float2(f16)*(1.0/65536.0);
        // Bilinear source probabilities (the author's proposal; a fixed nearest source gives the
        // same noise).
        uint rng=HashLowbias32(pathSeed^0xb5297a4du)|1u;
        const int2 d=int2(float2(RTRandom(rng),RTRandom(rng))<f);
        const int2 q=cell+d;
        const uint2 rotation=(65536u-f16)&65535u;
        const uint2 src=all(q>=0) && all(q<int2(width,height))?uint2(q):px;
        Texture2D<uint4> previous=ResourceDescriptorHeap[areaPreviousIdx];
        const uint4 h=previous.Load(int3(src,0));
        const float Mh=float(min(h.y,areaHistoryCap-1)), Wh=asfloat(h.z), th=asfloat(h.w);
        if(Mh>0 && Wh>0 && isfinite(Wh) && th>0)
        {
            CountRay(kAreaV2Base+0);
            AreaSample xh; xh.seed=h.x; xh.film=RTAreaFilm(src); xh.lens=AreaLensAt(areaPreviousLensIdx,src);
            RTAreaPrimary ctxH;
            {
                Texture2D<uint4> hits=ResourceDescriptorHeap[areaPreviousPrimaryIdx];
                Texture2D<uint4> geometry=ResourceDescriptorHeap[areaPreviousGeometryIdx];
                ctxH.hit=hits.Load(int3(src,0)); ctxH.geometry=geometry.Load(int3(src,0));
            }
            // Same pixel, same camera, no film offset: both shifts are the
            // identity and every reverse target is the canonical's own.
            const bool identity=(areaFlags&kAreaV2CameraStatic) && !(areaFlags&kAreaV2NoIdentity) && all(src==px) && all(f16==0);
            const float sigma=AreaReplayConfidence(px,cur);
            const float Mc=1, cSum=Mh+Mc;
            canonicalWeight=0;
            // Random replay: film translation into px, J = 1.
            AreaSample xrr=xh; xrr.film=AreaRotateFilm(xh.film,rotation);
            RTAreaPrimary ctxRR;
            const float3 vRR=AreaTrace(px,xrr,cur,ctxRR);
            const float tRR=RTAreaTarget(vRR);
            CountRay(kAreaV2Base+1);
            float tRRrev=tc;
            if(!identity)
            {
                AreaSample xcr=xc; xcr.film=AreaInverseRotation(rotation);
                RTAreaPrimary ignoredCtx;
                tRRrev=RTAreaTarget(AreaTrace(src,xcr,prev,ignoredCtx));
                CountRay(kAreaV2Base+2);
            }
            float c0=sigma*Mh;
            canonicalWeight+=AreaMisNonDefensiveCanonical(cSum,c0,tRRrev,Mc,tc,1);
            float m0=AreaMisNonDefensiveNonCanonical(cSum,c0,th,Mc,tRR,1);
            AreaStream(s,m0*tRR*Wh,xrr,tRR,vRR,ctxRR);
            // Primary-hit reconnection (defocus): keep the film uv, solve the lens point.
            if(sigma<1)
            {
                c0=(1-sigma)*Mh;
                float tRC=0,J=0,tRCrev=0,Jrev=0;
                AreaSample xrc=xh; RTAreaPrimary ctxRC=ctxRR; float3 vRC=0;
                if(identity) { tRC=tRR; J=1; vRC=vRR; tRCrev=tc; Jrev=1; }
                else
                {
                    CountRay(kAreaV2Base+3);
                    vRC=AreaReconnectShift(src,prev,px,cur,xh,ctxH,tRC,J,xrc,ctxRC);
                    if(tRC>0) CountRay(kAreaV2Base+4);
                    CountRay(kAreaV2Base+5);
                    AreaSample ignoredSample; RTAreaPrimary ignoredCtx;
                    AreaReconnectShift(px,cur,src,prev,xc,ctxC,tRCrev,Jrev,ignoredSample,ignoredCtx);
                    if(tRCrev>0) CountRay(kAreaV2Base+6);
                }
                canonicalWeight+=AreaMisNonDefensiveCanonical(cSum,c0,tRCrev,Mc,tc,Jrev);
                m0=tRC>0?AreaMisNonDefensiveNonCanonical(cSum,c0,th,Mc,tRC,1/J):0;
                AreaStream(s,m0*tRC*Wh*J,xrc,tRC,vRC,ctxRC);
            }
            M=uint(min(Mh+Mc,float(areaHistoryCap)));
        }
    }
    const bool historyWon=s.wSum>0;
    AreaStream(s,canonicalWeight*tc,xc,tc,valueC,ctxC);
    if(historyWon && s.x.seed!=xc.seed) CountRay(kAreaV2Base+7);
    const float W=s.target>0?s.wSum/s.target:0;
    RWTexture2D<uint4> current=ResourceDescriptorHeap[areaCurrentIdx];
    current[px]=uint4(s.x.seed,M,asuint(W),asuint(s.target));
    RWTexture2D<uint> films=ResourceDescriptorHeap[areaCurrentFilmIdx];
    films[px]=s.x.film;
    if(areaCurrentLensIdx!=0)
    {
        RWTexture2D<float2> lenses=ResourceDescriptorHeap[areaCurrentLensIdx];
        lenses[px]=s.x.lens;
    }
    {
        RWTexture2D<uint4> hits=ResourceDescriptorHeap[areaCurrentPrimaryIdx];
        RWTexture2D<uint4> geometry=ResourceDescriptorHeap[areaCurrentGeometryIdx];
        hits[px]=s.ctx.hit; geometry[px]=s.ctx.geometry;
    }
    uint total=WaveActiveSum(M);
    if((flags&8u) && WaveIsFirstLane()) InterlockedAdd(Counters[7+2*7*128+16*32+2],total);
    if((areaFlags&kAreaV2Spatial) && !AreaSpatialSkips(M))
    {
        // Same-frame scratch: the selected sample's radiance, so the spatial
        // pass never re-traces the canonical. Never temporal history.
        RWTexture2D<float4> radiance=ResourceDescriptorHeap[areaRadianceIdx];
        radiance[px]=float4(s.value,0);
        return;
    }
    AreaPublish(px,s.estimate);
}

void AreaV2Spatial(uint2 px)
{
    if(any(px>=uint2(width,height))) return;
    const RTCam cur=RTCamCurrent();
    Texture2D<uint4> bank=ResourceDescriptorHeap[areaPreviousIdx];
    Texture2D<uint4> hits=ResourceDescriptorHeap[areaPreviousPrimaryIdx];
    Texture2D<uint4> geometry=ResourceDescriptorHeap[areaPreviousGeometryIdx];
    Texture2D<float4> radiance=ResourceDescriptorHeap[areaRadianceIdx];
    const uint4 c=bank.Load(int3(px,0));
    // A long history: the temporal pass already published this pixel.
    if(AreaSpatialSkips(c.y)) return;
    const float Mc=float(max(c.y,1u)), Wc=asfloat(c.z), tc=asfloat(c.w);
    AreaSample xc; xc.seed=c.x; xc.film=RTAreaFilm(px); xc.lens=AreaLensAt(areaPreviousLensIdx,px);
    RTAreaPrimary ctxC; ctxC.hit=hits.Load(int3(px,0)); ctxC.geometry=geometry.Load(int3(px,0));
    const float3 vc=radiance.Load(int3(px,0)).rgb;
    const uint K=clamp(areaSpatialK,1u,4u);
    // Sample-independent neighbour set: a per-pixel/frame rotated golden-angle
    // disk. Confidence grows as the canonical history shrinks (disocclusions,
    // resets), and stays a small defensive share at a full history, where a
    // same-frame neighbour cannot beat the temporal estimate.
    uint rotation=HashLowbias32((px.x*73856093u)^(px.y*19349663u)^HashLowbias32(seed^0x51ed270bu));
    const float angle0=float(rotation&0xffffu)*(2*PI/65536.0), radius0=float(rotation>>16)/65536.0;
    const float adapt=max(.05,1-Mc/float(max(areaHistoryCap,1u)))*areaSpatialConfidence;
    int2 q[4]; float cj[4]; float sigmaJ[4]; bool rcJ[4];
    float cSum=Mc;
    [unroll] for(uint k=0;k<4;++k)
    {
        cj[k]=0; sigmaJ[k]=1; rcJ[k]=false; q[k]=int2(px);
        if(k>=K) continue;
        float angle=angle0+k*2.39996323;
        float radius=max(1.0,sqrt((k+radius0)/K)*areaSpatialRadius);
        int2 p=int2(px)+int2(round(float2(cos(angle),sin(angle))*radius));
        if(any(p<0) || any(p>=int2(width,height)) || all(p==int2(px))) continue;
        uint4 n=bank.Load(int3(p,0));
        if(n.y==0 || !(asfloat(n.z)>0) || !(asfloat(n.w)>0)) continue;
        q[k]=p;
        cj[k]=float(n.y)*adapt;
        // Reconnection only for near neighbours: the lens shift grows with
        // the pixel distance and a wide one is mostly outside the aperture.
        int2 dq=abs(p-int2(px));
        rcJ[k]=(areaFlags&kAreaV2RC) && max(dq.x,dq.y)<=3;
        sigmaJ[k]=rcJ[k]?AreaReplayConfidence(uint2(p),cur):1;
        cSum+=cj[k];
    }
    AreaSelection s=(AreaSelection)0;
    s.x=xc; s.target=tc; s.value=vc; s.ctx=ctxC;
    s.rng=HashLowbias32(RTAreaPathSeed(px)^0x63d83595u)|1u;
    float canonicalWeight=Mc/cSum;
    [loop] for(uint j=0;j<K;++j)
    {
        if(!(cj[j]>0)) continue;
        CountRay(kAreaV2Base+9);
        const uint2 p=uint2(q[j]);
        const uint4 n=bank.Load(int3(p,0));
        const float Wn=asfloat(n.z), tn=asfloat(n.w);
        AreaSample xn; xn.seed=n.x; xn.film=RTAreaFilm(p); xn.lens=AreaLensAt(areaPreviousLensIdx,p);
        RTAreaPrimary ctxN; ctxN.hit=hits.Load(int3(p,0)); ctxN.geometry=geometry.Load(int3(p,0));
        // Random replay, both directions.
        RTAreaPrimary ctxRR, ignoredCtx;
        const float3 vRR=AreaTrace(px,xn,cur,ctxRR);
        const float tRR=RTAreaTarget(vRR);
        const float tRRrev=RTAreaTarget(AreaTrace(p,xc,cur,ignoredCtx));
        CountRay(kAreaV2Base+11);
        float c0=sigmaJ[j]*cj[j];
        canonicalWeight+=AreaMisDefensiveCanonical(cSum,c0,tRRrev,Mc,tc,1);
        AreaStream(s,AreaMisDefensiveNonCanonical(cSum,c0,tn,Mc,tRR,1)*tRR*Wn,xn,tRR,vRR,ctxRR);
        if(rcJ[j] && sigmaJ[j]<1)
        {
            c0=(1-sigmaJ[j])*cj[j];
            float tRC,J,tRCrev,Jrev; AreaSample xrc,ignoredSample; RTAreaPrimary ctxRC;
            const float3 vRC=AreaReconnectShift(p,cur,px,cur,xn,ctxN,tRC,J,xrc,ctxRC);
            AreaReconnectShift(px,cur,p,cur,xc,ctxC,tRCrev,Jrev,ignoredSample,ignoredCtx);
            if(tRC>0) CountRay(kAreaV2Base+10);
            canonicalWeight+=AreaMisDefensiveCanonical(cSum,c0,tRCrev,Mc,tc,Jrev);
            float m0=tRC>0?AreaMisDefensiveNonCanonical(cSum,c0,tn,Mc,tRC,1/J):0;
            AreaStream(s,m0*tRC*Wn*J,xrc,tRC,vRC,ctxRC);
        }
    }
    AreaStream(s,canonicalWeight*tc*Wc,xc,tc,vc,ctxC);
    AreaPublish(px,s.estimate);
}

[numthreads(8,8,1)]
void AreaTemporalV2(uint3 tid : SV_DispatchThreadID) { AreaV2Temporal(tid.xy); }
[numthreads(8,8,1)]
void AreaSpatialV2(uint3 tid : SV_DispatchThreadID) { AreaV2Spatial(tid.xy); }

void RTMain(uint3 tid,bool overlap,bool cacheRadiance)
{
    uint2 px=tid.xy;
    if(any(px>=uint2(width,height))) return;
    uint pathSeed=(px.x+px.y*width+1)*747796405u ^ (seed+1)*2891336453u;
    if(pathSeed==0) pathSeed=1;
    bool directStore;
    RTAreaPrimary context;
    float3 value=RTTraceSample(px,pathSeed,directStore,0,context);
    if(flags&16384u)
    {
        if(!all(isfinite(value))) CountRay(4);
        // Strictly positive target retains support even for black paths.
        // A historical reservoir represents M candidates with normalization W.
        // Its merge weight is target(current replay)*W*M, not just target.
        float target=RTAreaTarget(value), sum=target;
        uint count=1, selectedSeed=pathSeed, selectedFilm=0;
        if(overlap && areaHistoryValid!=0 && areaHistoryCap>1)
            RTAreaOverlap(px,pathSeed,value,target,context,sum,count,selectedSeed,selectedFilm);
        else if(areaHistoryValid!=0 && areaHistoryCap>1)
        {
            Texture2D<uint4> previous=ResourceDescriptorHeap[areaPreviousIdx];
            uint2 rotation=0;
            uint2 sourcePixel=(flags&65536u)?RTAreaHistoryPixel(px,pathSeed,rotation):px;
            uint4 history=previous.Load(int3(sourcePixel,0));
            uint previousFilm=RTAreaFilm(sourcePixel);
            rotation=(rotation+uint2(previousFilm&65535u,previousFilm>>16))&65535u;
            uint replayFilm=rotation.x|(rotation.y<<16);
            uint oldCount=min(history.y,areaHistoryCap-1);
            float oldW=asfloat(history.z);
            if(oldCount>0 && oldW>0 && isfinite(oldW))
            {
                CountRay(7+2*7*128+16*32);
                bool ignored;
                RTAreaPrimary replayContext;
                float3 replay=RTTraceSample(px,history.x,ignored,replayFilm,replayContext);
                if(!(flags&131072u) && all(sourcePixel==px) && replayFilm==previousFilm)
                    RTAreaAuditPrimary(px,replayContext);
                if(replayFilm!=0) CountRay(7+2*7*128+16*32+6);
                if(!all(isfinite(replay))) CountRay(4);
                float replayTarget=RTAreaTarget(replay);
                float weight=replayTarget*oldW*oldCount;
                sum+=weight; count+=oldCount;
                // Avalanche before selection: xorshift(seed^constant) alone
                // is linearly related to the first (film) random dimension.
                uint selection=pathSeed^0xa511e9b3u;
                selection^=selection>>16; selection*=0x7feb352du;
                selection^=selection>>15; selection*=0x846ca68bu;
                selection^=selection>>16;
                if(RTRandom(selection)*sum<weight)
                {
                    value=replay; target=replayTarget; selectedSeed=history.x;
                    selectedFilm=replayFilm; context=replayContext;
                    CountRay(7+2*7*128+16*32+1);
                }
            }
        }
        // Overlap weights already contain both partition-of-unity MIS halves.
        float normalization=sum/(overlap?target:(float(count)*target));
        RWTexture2D<uint4> current=ResourceDescriptorHeap[areaCurrentIdx];
        current[px]=uint4(selectedSeed,count,asuint(normalization),asuint(target));
        if(flags&65536u)
        {
            RWTexture2D<uint> rotations=ResourceDescriptorHeap[areaCurrentFilmIdx];
            rotations[px]=selectedFilm;
        }
        if(flags&4194304u)
        {
            RWTexture2D<uint4> hits=ResourceDescriptorHeap[areaCurrentPrimaryIdx];
            RWTexture2D<uint4> geometry=ResourceDescriptorHeap[areaCurrentGeometryIdx];
            if(context.hit.x!=0)
            {
                CountRay(7+2*7*128+16*32+14);
                if(flags&8388608u) context.hit.y^=1u; // Deliberate context corruption.
            }
            hits[px]=context.hit; geometry[px]=context.geometry;
        }
        // M varies during reset/invalid history: sum lanes rather than count.
        uint total=WaveActiveSum(count);
        if((flags&8u) && WaveIsFirstLane()) InterlockedAdd(Counters[7+2*7*128+16*32+2],total);
        if(cacheRadiance && (flags&32768u))
        {
            RWTexture2D<float4> radiance=ResourceDescriptorHeap[areaRadianceIdx];
            // Raw selected sample, before RIS normalization. This is same-frame
            // scratch only; the spatial result must never overwrite history.
            radiance[px]=float4(value+((flags&2147483648u)?float3(.25,.25,.25):float3(0,0,0)),0);
        }
        value*=normalization;
        if(flags&32768u) return; // Spatial dispatch alone publishes/accumulates.
    }
    if(directStore)
    {
        // Preserve the original GBuffer sky alpha and unlit accumulation bypass.
        Texture2D<float> depth=ResourceDescriptorHeap[depthIdx];
        float alpha=depth.Load(int3(px,0))>0?1:0;
        Result[px]=float4(value,alpha); Accumulation[px]=float4(value,alpha);
        if(auxOutUav!=0)   // noise-free: its moment is its own square
        { RWTexture2D<float> moment=ResourceDescriptorHeap[auxOutUav]; const float l=Luminance(max(value,0)); moment[px]=l*l; }
    }
    else RTStoreSample(px,value);
}

[numthreads(8,8,1)]
void main(uint3 tid : SV_DispatchThreadID) { RTMain(tid,false,false); }

// Compile away the four-source path loop from the ordinary PT entry.
[numthreads(8,8,1)]
void AreaOverlap(uint3 tid : SV_DispatchThreadID) { RTMain(tid,true,false); }

// Separate entry/PSO keeps the three spatial path evaluations out of the
// ordinary PT kernel and its register allocation when spatial reuse is off.
[numthreads(8,8,1)]
void AreaSpatial(uint3 tid : SV_DispatchThreadID)
{
    uint2 px=tid.xy;
    if(any(px>=uint2(width,height))) return;
    uint pathSeed=(px.x+px.y*width+1)*747796405u ^ (seed+1)*2891336453u;
    if(pathSeed==0) pathSeed=1;
    RTStoreSample(px,RTAreaSpatial(px,pathSeed));
}

// Keep reconnection's control flow/register allocation out of the plain replay kernels.
[numthreads(8,8,1)]
void AreaReconnect(uint3 tid : SV_DispatchThreadID)
{
    uint2 px=tid.xy;
    if(any(px>=uint2(width,height))) return;
    RTStoreSample(px,RTAreaReconnectSpatial(px,false));
}

// Dedicated entries compile cache traffic out of the other kernels.
[numthreads(8,8,1)]
void AreaCached(uint3 tid : SV_DispatchThreadID) { RTMain(tid,false,true); }
[numthreads(8,8,1)]
void AreaOverlapCached(uint3 tid : SV_DispatchThreadID) { RTMain(tid,true,true); }
[numthreads(8,8,1)]
void AreaReconnectCached(uint3 tid : SV_DispatchThreadID)
{
    uint2 px=tid.xy;
    if(any(px>=uint2(width,height))) return;
    RTStoreSample(px,RTAreaReconnectSpatial(px,true));
}

// ==== Translucent front-layer reflection ====
// FrontReflection (PathTracePass::RecordFrontReflection): TransparentPass composites glass
// over the path-traced image, so the pane's reflection is traced here, not taken from the
// forward fold's screen tiers (which only mirror what is on screen). It traces the pane's
// specular lobe from TranslucentFrontLayerPass's stamp - the depth, normal and roughness the
// glass's own colour draw shades with - and continues the path as RTTraceSample does from its
// first bounce vertex. It accumulates what custom_surface.hlsli's front tier folds:
// pre-Fresnel reflected radiance (the split-sum fold multiplies it by F0*A+B), confidence 1.
// The pane is no path vertex: no next-event estimate there and no sun disk on its own
// segment, because the forward draw lights its direct specular itself. Its own constants:
// depthIdx / normalIdx / surfaceIdx name the TranslucentFront_* stamp, auxOutUav the
// accumulation - or, under DLSS-RR (flag 8192), the per-frame sample
// PathTraceFrontNRD.cs.hlsl denoises.
//
// RTTraceContinuation: the radiance arriving at a surface along l - the path RTTraceSample
// continues from its first bounce vertex, for a surface that is no path vertex itself (the
// glass pane, a toon receiver's ambient); its first segment adds no sun disk, which the
// caller's surface is lit by elsewhere. hitT: that segment's length (ray TMax on a miss).
// neeCandidates > 1: the local lights' estimate at each vertex resamples that many
// (RTLocalLightsRIS), else one by power.
float3 RTTraceContinuation(float3 origin,float3 l,inout uint rng,out float hitT,uint neeCandidates=1)
{
    hitT=10000;
    float3 p=origin,n=0,ng=0,v=0,value=0,throughput=1,albedo=0;
    float rough=1,metal=0,reflectance=0,bsdfPdf=0;
    // RTTraceSample's glass segment state.
    uint sunMode=0;
    float3 mediumSigma=0;
    g_glass=(RTGlass)0;
    g_thin=(RTThin)0;
    uint bounce=0;
    [loop] while(true)
    {
        // RTTraceSample's bounce ray: mask 0x18 and back faces culled, so
        // neither the caller's surface (a pane is excluded by the mask) nor any
        // other forward-translucent surface is a vertex - but Transmissive glass, both faces.
        const float3 offsetN=g_glass.active?(dot(l,ng)>=0?ng:-ng):(dot(l,n)>=0?n:-n);
        g_glass=(RTGlass)0;
        g_thin=(RTThin)0;
        RayDesc ray; ray.Origin=bounce==0?origin:p+offsetN*0.001; ray.Direction=l; ray.TMin=0.001; ray.TMax=10000;
        RayQuery<RAY_FLAG_NONE> q;
        q.TraceRayInline(g_TLAS,RAY_FLAG_CULL_BACK_FACING_TRIANGLES,(flags&1u)?0x08:0x18,ray);
        while(q.Proceed())
            if(q.CandidateType()==CANDIDATE_NON_OPAQUE_TRIANGLE &&
                (q.CandidateTriangleFrontFace() ||
                 (g_Instances[q.CandidateInstanceID()].rtFlags&(RT_INST_FLAG_DOUBLE_SIDED|RT_INST_FLAG_TRANSMISSIVE))) &&
                HR_AlphaAccept(q.CandidateInstanceID(),q.CandidatePrimitiveIndex(),q.CandidateTriangleBarycentrics(),true))
                q.CommitNonOpaqueTriangleHit();
        if(q.CommittedStatus()!=COMMITTED_TRIANGLE_HIT)
        {
            // The sun disk from a path vertex, MIS-weighted against its
            // next-event estimate; the pane's own segment has none (above).
            if(bounce>0 && sunMode!=2u && any(sunColor>0) && dot(l,normalize(-sunDirection))>=sunCosRadius)
            {
                float sunPdf=1.0/(2*PI*(1-sunCosRadius));
                float mis=((flags&4u) || sunMode==1u)?1.0:RTMis(bsdfPdf,sunPdf);
                value+=throughput*sunColor*mis/(PI*(1-sunCosRadius*sunCosRadius));
            }
            if(flags&2u) value+=throughput*constantEnvironment;
            else if(skyIdx!=0)
            {
                TextureCube<float4> sky=ResourceDescriptorHeap[skyIdx];
                value+=throughput*sky.SampleLevel(gSampWrap,l,0).rgb*environmentScale;
            }
            break;
        }
        if(bounce==0) hitT=q.CommittedRayT();
        uint id=q.CommittedInstanceID();
        throughput*=DielectricTransmittance(mediumSigma,q.CommittedRayT());
        const bool glassHit=!(flags&1u) && PTIsGlass(id);
        const bool glassSolid=glassHit && RTGlassSolid(pathMaterialsIdx,id);
        float normalStrength=1;
        if(pathMaterialsIdx!=0)
        {
            StructuredBuffer<MaterialGPUData> pathMaterials=ResourceDescriptorHeap[pathMaterialsIdx];
            normalStrength=pathMaterials[id].normalStrength;
        }
        RTHitSurface hit;
        if(!RTLoadHitSurface(id,q.CommittedPrimitiveIndex(),q.CommittedTriangleBarycentrics(),
            q.CommittedObjectToWorld3x4(),q.CommittedWorldToObject3x4(),
            ray.Origin+l*q.CommittedRayT(),glassSolid || q.CommittedTriangleFrontFace(),0,hit,normalStrength)) break;
        if(!hit.trusted)
        {
            // An unresolved custom proxy: RTTraceSample's environment estimate.
            if(flags&2u) value+=throughput*constantEnvironment;
            else if(skyIdx!=0)
            {
                TextureCube<float4> sky=ResourceDescriptorHeap[skyIdx];
                value+=throughput*sky.SampleLevel(gSampWrap,l,0).rgb*environmentScale;
            }
            break;
        }
        p=hit.P; n=hit.Nshade; ng=hit.Ngeo; v=-l; albedo=hit.albedo;
        value+=throughput*hit.emissive;
        if(hit.unlit) { value+=throughput*albedo; break; }
        rough=hit.rough; metal=hit.metal; reflectance=hit.reflectance;
        if(glassHit) g_glass=RTGlassAtHit(pathMaterialsIdx,id,albedo,rough,metal,ng,l);
        else if(pathMaterialsIdx!=0)
        {
            StructuredBuffer<MaterialGPUData> pathMaterials=ResourceDescriptorHeap[pathMaterialsIdx];
            g_thin=PTThinAt(pathMaterials[id],albedo,metal);
        }
        ++bounce;
        // RTTraceSample's loop body at this vertex: next-event estimate, the
        // bounce limit, roulette, the BSDF continuation.
        if(!(ptLastNeeOff!=0 && maxBounces!=0 && bounce>=maxBounces))
        {
            float sunSpec,localSpec,localDistance;
            const float3 sun=RTSun(p,n,v,albedo,rough,metal,reflectance,rng,false,sunSpec);
            float3 local;
            if(neeCandidates>1) local=RTLocalLightsRIS(p,n,v,albedo,rough,metal,reflectance,rng,neeCandidates);
            else local=RTLocalLights(p,n,v,albedo,rough,metal,reflectance,rng,false,localSpec,localDistance);
            value+=throughput*(sun+local);
        }
        if(maxBounces!=0 && bounce>=maxBounces) break;
        if(bounce>=max(ptRRStart,1u))
        {
            float survival=clamp(max(throughput.x,max(throughput.y,throughput.z)),0.05,0.95);
            if(RTRandom(rng)>=survival) break;
            throughput/=survival;
        }
        float3 weight;
        if(flags&1u)
        {
            float u=RTRandom(rng),phi=2*PI*RTRandom(rng);
            l=RTBasis(n,float3(sqrt(u)*cos(phi),sqrt(u)*sin(phi),sqrt(1-u)));
            weight=albedo;
        }
        else if(g_glass.active)
        {
            RTGlassStep step;
            const float3 u=float3(RTRandom(rng),RTRandom(rng),RTRandom(rng));
            if(!RTGlassSample(g_glass,n,v,albedo,rough,metal,reflectance,u,step)) break;
            l=step.l; weight=step.weight;
            if(!step.passThrough) { bsdfPdf=step.pdf; sunMode=step.sunMode; }
            if(step.transmitted && !g_glass.lobe.thinWalled) mediumSigma=g_glass.inside?0:g_glass.sigma;
        }
        else if(g_thin.active)
        {
            const float2 uDir=float2(RTRandom(rng),RTRandom(rng));
            float pdf;
            if(!RTThinSample(g_thin,n,v,albedo,rough,metal,reflectance,float3(uDir,RTRandom(rng)),l,weight,pdf)) break;
        }
        else if(!RTSampleBSDF(n,v,albedo,rough,metal,reflectance,rng,l,weight)) break;
        if(!g_glass.active) { bsdfPdf=RTPdf(n,v,l,rough); sunMode=0; }
        throughput*=weight;
        if(!any(throughput>0)) break;
    }
    g_glass=(RTGlass)0;
    g_thin=(RTThin)0;
    return value;
}

// One sample of the pane's specular lobe. The visible-normal pdf cancels the lobe's D and
// G1, so the continuation's radiance is the sample itself; a microfacet below the pane's
// horizon takes the mirror direction (glass lobes are narrow). hitT: the hit distance the
// RR-mode denoiser sizes its specular reprojection with. neeCandidates: the continuation's
// light resampling (ptFrontSamples bits 8-15).
float3 RTFrontIncoming(float3 p,float3 n,float3 v,float rough,inout uint rng,out float hitT,uint neeCandidates)
{
    float3 t,b; OrthonormalBasis(n,t,b);
    const float3 ht=SampleGGXVNDF(float3(dot(v,t),dot(v,b),dot(v,n)),rough*rough,RTRandom(rng),RTRandom(rng));
    float3 l=reflect(-v,t*ht.x+b*ht.y+n*ht.z);
    if(dot(n,l)<=0) l=reflect(-v,n);
    return RTTraceContinuation(p+n*0.001,l,rng,hitT,neeCandidates);
}

[numthreads(8,8,1)]
void FrontReflection(uint3 tid : SV_DispatchThreadID)
{
    uint2 px=tid.xy;
    if(any(px>=uint2(width,height))) return;
    RWTexture2D<float4> output=ResourceDescriptorHeap[auxOutUav];
    Texture2D<float> depthTex=ResourceDescriptorHeap[depthIdx];
    const float depth=depthTex.Load(int3(px,0));
    // Reverse-Z: no pane stamped here is EXACTLY 0 (the prepass clears to sky
    // every frame), and the fold reads nothing at such a texel.
    if(depth<=0) { output[px]=0; return; }
    // -pt_direct_only: the image's direct-light oracle has no secondary light.
    if(flags&64u) { output[px]=float4(0,0,0,1); return; }
    Texture2D<float4> normalTex=ResourceDescriptorHeap[normalIdx];
    Texture2D<float4> surfaceTex=ResourceDescriptorHeap[surfaceIdx];
    const float2 uv=(float2(px)+.5)/float2(width,height);
    const float3 p=WorldPosFromDepth(uv,depth,invViewProj);
    const float3 v=normalize(camera-p);
    float3 n=normalize(normalTex.Load(int3(px,0)).xyz*2-1);
    // A normal-mapped or smooth-shaded texel can face away at a grazing view:
    // tilt it just over the horizon so the lobe stays on the viewer's side.
    const float nv=dot(n,v);
    if(nv<1e-3) n=normalize(n+v*(1e-3-nv));
    // The colour draw's own roughness; the GBuffer path's floor keeps a
    // mirror lobe finite.
    const float rough=max(surfaceTex.Load(int3(px,0)).r,0.045);
    // Decorrelated from RTMain's stream at the same pixel.
    uint rng=((px.x+px.y*width+1)*747796405u ^ (seed+1)*2891336453u)^0x68bc21ebu;
    rng^=rng>>16; rng*=0x7feb352du; rng^=rng>>15; rng*=0x846ca68bu; rng^=rng>>16;
    if(rng==0) rng=1;
    // A denoised frame (flag 8192) traces ptFrontSamples paths per mirror-like texel: REBLUR
    // cannot blur a mirror spatially, so its per-frame variance is what reaches the screen.
    // hitT: the first path's (a mirror's paths leave in one direction).
    const uint samples=((flags&8192u) && rough<0.1)?clamp(ptFrontSamples&0xffu,1u,8u):1u;
    const uint neeCandidates=(ptFrontSamples>>8)&0xffu;
    float hitT;
    float3 value=RTFrontIncoming(p,n,v,rough,rng,hitT,neeCandidates);
    [loop] for(uint si=1;si<samples;++si)
    {
        float hitTi;
        value+=RTFrontIncoming(p,n,v,rough,rng,hitTi,neeCandidates);
    }
    value/=float(samples);
    // One non-finite sample must not poison the running mean for good.
    if(!all(isfinite(value))) value=0;
    // A frame that does not accumulate (flag 8192: RR, or a scene that moves):
    // this frame's sample and its hit distance for PathTraceFrontNRD.cs.hlsl's
    // Pack, no running mean.
    if(flags&8192u) { output[px]=float4(value,hitT); return; }
    // The image's running mean (RTStoreSample). A texel with no pane last
    // frame (alpha 0) starts over.
    const float4 previous=output[px];
    if(spp!=0 && !(flags&4096u) && previous.a>0) value=previous.rgb+(value-previous.rgb)/(float(spp)+1);
    output[px]=float4(value,1);
}

// PT-lit toon receivers (PathTracePass::RecordNprLighting). On the path-trace path an NPR
// pixel keeps its own model (Lighting.ps's NPR variant: ramp, face SDF, rim, brightness
// floor) but takes the two lighting inputs the raster path feeds it from the path tracer:
//   rgb  the ambient: the sphere-mean incoming radiance at the surface - the
//        quantity the toon ambient's isotropic DDGI / SH-DC taps estimate
//        (avg over directions of irradiance/pi), here traced: a uniform
//        sphere direction, the RTTraceContinuation path (bounce light, sky
//        light, emissive surfaces, local lights' indirect; no sun disk - the
//        ramp lights the sun). A direction into the surface starts just below
//        it, so the receiver's own single-sided mesh does not block it (as for
//        free-space probes); its double-sided parts still occlude.
//   a    the sun visibility: one shadow ray toward the sun's cone (the ramp
//        keeps the terminator; faces keep their external-only cascades).
// Non-toon pixels write a = -1, which the denoiser treats as "not a receiver".
[numthreads(8,8,1)]
void NprAmbient(uint3 tid : SV_DispatchThreadID)
{
    uint2 px=tid.xy;
    if(any(px>=uint2(width,height))) return;
    RWTexture2D<float4> output=ResourceDescriptorHeap[auxOutUav];
    Texture2D<float> depthTex=ResourceDescriptorHeap[depthIdx];
    Texture2D<float4> normalTex=ResourceDescriptorHeap[normalIdx];
    const float depth=depthTex.Load(int3(px,0));
    const float4 nraw=normalTex.Load(int3(px,0));
    if(depth<=0 || materialsIdx==0) { output[px]=float4(0,0,0,-1); return; }
    StructuredBuffer<MaterialGPUData> materials=ResourceDescriptorHeap[materialsIdx];
    const uint material=GBufferDecodeMatIdx(nraw.a);
    if(materials[material].customShadingModel!=SHADING_MODEL_TOON) { output[px]=float4(0,0,0,-1); return; }
    const float2 uv=(float2(px)+.5)/float2(width,height);
    const float3 p=WorldPosFromDepth(uv,depth,invViewProj);
    const float3 n=normalize(nraw.xyz*2-1);
    uint rng=((px.x+px.y*width+1)*747796405u ^ (seed+1)*2891336453u)^0x3c6ef372u;
    rng^=rng>>16; rng*=0x7feb352du; rng^=rng>>15; rng*=0x846ca68bu; rng^=rng>>16;
    if(rng==0) rng=1;
    // Uniform sphere direction; the pdf 1/(4 pi) cancels in the mean.
    const float z=1-2*RTRandom(rng), phi=2*PI*RTRandom(rng), r=sqrt(saturate(1-z*z));
    const float3 l=float3(r*cos(phi),r*sin(phi),z);
    float hitT;
    float3 ambient=(flags&64u)?0:RTTraceContinuation(p+n*(dot(l,n)>=0?0.002:-0.002),l,rng,hitT);
    if(!all(isfinite(ambient))) ambient=0;
    float visibility=1;
    if(!(flags&4u) && any(sunColor>0))
    {
        const float c=lerp(sunCosRadius,1.0,RTRandom(rng)), sphi=2*PI*RTRandom(rng), st=sqrt(saturate(1-c*c));
        const float3 ls=RTBasis(normalize(-sunDirection),float3(st*cos(sphi),st*sin(sphi),c));
        visibility=Luminance(RTVisibility(p,n,ls,10000));
    }
    output[px]=float4(ambient,visibility);
}

#if PT_MEDIA
// ==== The camera segment's fog (PathTracePass::RecordCameraFog) ====
// One sample of the fog between the camera and each pixel's GBuffer surface (PTCameraSegment and
// the path from its vertex): rgb the light the fog scatters toward the camera, a the segment's
// transmittance, as a running mean with the image's sample count (RTStoreSample's rule) into
// auxOutUav. PathTraceFogPass composites it: HDR x a + rgb.
[numthreads(8,8,1)]
void FogCamera(uint3 tid : SV_DispatchThreadID)
{
    const uint2 px=tid.xy;
    if(any(px>=uint2(width,height))) return;
    RWTexture2D<float4> output=ResourceDescriptorHeap[auxOutUav];
    // Decorrelated from RTMain's stream at the same pixel.
    uint rng=((px.x+px.y*width+1)*747796405u ^ (seed+1)*2891336453u)^0x2545f491u;
    rng^=rng>>16; rng*=0x7feb352du; rng^=rng>>15; rng*=0x846ca68bu; rng^=rng>>16;
    if(rng==0) rng=1;
    g_cameraFog=true;
    bool ignored;
    float4 value=float4(RTTraceSample(px,rng,ignored),g_cameraT);
    // One non-finite sample must not poison the running mean for good.
    if(!all(isfinite(value))) value=float4(0,0,0,1);
    if(spp!=0 && !(flags&4096u)) value=output[px]+(value-output[px])/(float(spp)+1);
    output[px]=value;
}
#endif
