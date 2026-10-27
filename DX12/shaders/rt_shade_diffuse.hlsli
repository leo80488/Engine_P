#ifndef RT_SHADE_DIFFUSE_HLSLI
#define RT_SHADE_DIFFUSE_HLSLI
#include "core/color.hlsli"   // Luminance
// Caller supplies Visibility and IndirectAtHit; this helper never reads screen colour.
// View-independent diffuse transport only. The receiver's albedo is applied
// once by Lighting; the hit's reflectance belongs here, before reconnection.

// UNSHADOWED irradiance/PI from one local light at a secondary hit (no ray).
// The cone test takes the NORMALISED surface->light vector (light_eval.hlsli
// contract). l/distance are returned for the caller's shadow ray.
float3 RT_LocalLightUnshadowed(RTHitSurface s,GPULight light,out float3 l,out float distance)
{
    float3 delta=light.position-s.P;
    distance=length(delta); l=0;
    if(light.type==0 || !(light.intensity>0) || !any(light.color>0) || distance<=.002 || distance>=light.radius) return 0;
    l=delta/distance;
    float cosine=saturate(dot(s.Nshade,l));
    if(cosine<=0) return 0;
    float attenuation=EvalLocalLightAttenuation(light,l,distance);
    if(!(attenuation>0)) return 0;
    return light.color*light.intensity*attenuation*(cosine/PI);
}
float3 RT_LocalLightIrradiance(RTHitSurface s,GPULight light,uint qualityTier)
{
    float3 l; float distance;
    float3 c=RT_LocalLightUnshadowed(s,light,l,distance);
    if(!any(c>0)) return 0;
    // HLSL 2018 ?: evaluates both operands. Keep the ray query in a real
    // branch so Tier 0 does not trace a discarded shadow.
    float localVisibility=1;
    [branch] if(qualityTier!=0)
        localVisibility=Visibility(s.P,s.Ngeo,l,distance-.002);
    return c*localVisibility;
}
// A hash of the hit position (1/256 m cells): re-shading the SAME secondary
// point (temporal validation) makes the same light choice, so its radiance is
// deterministic, while different points decorrelate.
uint RT_HitHash(float3 p)
{
    int3 q=int3(floor(p*256));
    return HashLowbias32(asuint(q.x)^HashLowbias32(asuint(q.y)^HashLowbias32(asuint(q.z)^0x68e31da4u)))|1u;
}
// Local lights at a secondary hit.
//  * Up to exactLimit listed lights (Tier 0: 64, Tier 1: hitExactLights): the exact sum
//    (Tier 1 traces one shadow ray per light, Tier 0 none).
//  * More lights (many-light cells, or an overflowed grid that lists them all): a hashed,
//    evenly strided subset of at most kMaxStream lights (each included with probability
//    n/count, rescaled by count/n). Tier 0 sums it; Tier 1 draws hitLightSamples lights in
//    proportion to their unshadowed contribution (exact normaliser) and traces one shadow
//    ray each. A uniform pick scaled by the count would leave most hits black and a few
//    count-times too bright, which ReSTIR reuses as sparkles.
float3 RT_LocalLightsAtHit(RTHitSurface s,uint qualityTier)
{
    StructuredBuffer<GPULight> lights=ResourceDescriptorHeap[lightsIdx];
    uint offset; uint count=WorldLightRange(worldGrid,s.P,lightCount,offset);
    if(count==0) return 0;
    const uint exactLimit=qualityTier==0?64u:clamp(hitExactLights,1u,64u);
    float3 irradiance=0;
    if(count<=exactLimit)
    {
        [loop] for(uint i=0;i<count;++i)
            irradiance+=RT_LocalLightIrradiance(s,lights[WorldLightIndex(worldGrid,offset,i)],qualityTier);
        return irradiance;
    }
    RT_CountRandomLightPick();
    uint hash=RT_HitHash(s.P);
    const uint kMaxStream=64u;
    const uint n=min(count,kMaxStream);
    const uint start=count>n?hash%count:0;
    const float scale=float(count)/float(n);
    const uint picks=max(hitLightSamples,1u);
    float wsum=0,wsel=0,dsel=0; float3 csel=0,lsel=0;
    [loop] for(uint i=0;i<n;++i)
    {
        const uint k=(start+(i*count)/n)%count;
        float3 l; float d;
        float3 c=RT_LocalLightUnshadowed(s,lights[WorldLightIndex(worldGrid,offset,k)],l,d);
        if(qualityTier==0) { irradiance+=c; continue; }
        const float w=Luminance(c);
        if(!(w>0)) continue;
        wsum+=w;
        // One pick: streaming RIS in this same pass.
        if(picks==1 && RTRandom(hash)*wsum<w) { wsel=w; csel=c; lsel=l; dsel=d; }
    }
    if(qualityTier==0) return irradiance*scale;
    if(!(wsum>0)) return 0;
    if(picks==1)
        return wsel>0?csel*(wsum/wsel)*scale*Visibility(s.P,s.Ngeo,lsel,dsel-.002):0;
    // hitLightSamples picks with replacement, each in proportion to its
    // unshadowed contribution (the exact normaliser), one shadow ray each.
    [loop] for(uint p=0;p<picks;++p)
    {
        const float target=RTRandom(hash)*wsum;
        float acc=0,wpick=0,dpick=0; float3 cpick=0,lpick=0;
        [loop] for(uint i=0;i<n;++i)
        {
            const uint k=(start+(i*count)/n)%count;
            float3 l; float d;
            float3 c=RT_LocalLightUnshadowed(s,lights[WorldLightIndex(worldGrid,offset,k)],l,d);
            const float w=Luminance(c);
            if(!(w>0)) continue;
            acc+=w; wpick=w; cpick=c; lpick=l; dpick=d; // the last positive light if rounding runs past the end
            if(acc>=target) break;
        }
        if(wpick>0) irradiance+=cpick*(wsum/wpick)*Visibility(s.P,s.Ngeo,lpick,dpick-.002);
    }
    return irradiance*(scale/float(picks));
}

float3 RT_ShadeDiffuseHalf(RTHitSurface s,uint qualityTier,float ddgiScale,inout uint rng)
{
    if(s.unlit) return s.albedo+s.emissive;
    float3 irradiance=0;
    float3 l=normalize(-sunDirection);
    float cosine=saturate(dot(s.Nshade,l));
    if(cosine>0 && any(sunColor>0)) irradiance+=sunColor*cosine*Visibility(s.P,s.Ngeo,l,100)/PI;
    if(lightCount>0 && lightsIdx!=0) irradiance+=RT_LocalLightsAtHit(s,qualityTier);
    const float3 emission=(flags&kGIFlagNoHitEmission)?0:s.emissive; // diagnostic: emitter share of GI
    return emission+s.albedo*(1-s.metal)*(irradiance+IndirectAtHit(s)*ddgiScale);
}
#endif
