#ifndef BSDF_SAMPLE_HLSLI
#define BSDF_SAMPLE_HLSLI
#include "brdf.hlsli"
#include "core/sampling.hlsli"   // OrthonormalBasis, SampleGGXVNDF, Hammersley2D
uint RTNext(inout uint state) { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state; }
float RTRandom(inout uint state) { return (RTNext(state) >> 8) * (1.0 / 16777216.0); }

float3 RTBasis(float3 n, float3 v)
{
    float3 t,b; OrthonormalBasis(n,t,b);
    return t*v.x+b*v.y+n*v.z;
}
float3 RTSampleCosine(float2 u)
{
    float phi=2*PI*u.y;
    return float3(sqrt(u.x)*cos(phi),sqrt(u.x)*sin(phi),sqrt(1-u.x));
}
// The sampling distribution must not inherit the evaluation BRDF's D clamp.
float RTGGXDistribution(float nh, float rough)
{
    float a=rough*rough, a2=a*a;
    float d=(1-nh*nh)+a2*nh*nh;
    return a2 / max(PI*d*d,1e-30);
}
float RTGGXVisibleNormalPdf(float3 v, float3 h, float rough)
{
    if (v.z<=0 || h.z<=0 || dot(v,h)<=0) return 0;
    float a=rough*rough;
    float g1=2*v.z/(v.z+sqrt(a*a+(1-a*a)*v.z*v.z));
#if defined(PT_REFERENCE_GGX)
    // Match PT evaluation without subtracting nearly equal cosine squares.
    // In this tangent frame, |N x H|^2 is H.x^2 + H.y^2.
    float d=dot(h.xy,h.xy)+a*a*h.z*h.z;
    float distribution=a*a/max(PI*d*d,1e-30);
    return distribution*g1*dot(v,h)/v.z;
#else
    return RTGGXDistribution(h.z,rough)*g1*dot(v,h)/v.z;
#endif
}
float3 RTEvalBSDF(float3 n, float3 v, float3 l, float3 albedo, float rough, float metal, float reflectance)
{
    float3 spec, kd;
    EvalCookTorrance(n, v, l, lerp((0.16 * reflectance * reflectance).xxx, albedo, metal), rough, metal, albedo, spec, kd);
    return (kd * albedo / PI + spec) * saturate(dot(n, l));
}
float RTBsdfPdf(float3 n, float3 v, float3 l, float rough)
{
    if (dot(n,l)<=0 || dot(n,v)<=0) return 0;
    float3 t,b; OrthonormalBasis(n,t,b);
    float3 vt=float3(dot(v,t),dot(v,b),dot(v,n));
    float3 h=normalize(v+l), ht=float3(dot(h,t),dot(h,b),dot(h,n));
    float specPdf=RTGGXVisibleNormalPdf(vt,ht,rough)/max(4*abs(dot(v,h)),1e-20);
    return 0.5*(saturate(dot(n,l))/PI+specPdf);
}
// Mixture PDF includes both lobes regardless of the selected technique.
bool RTSampleBSDF(float3 n, float3 v, float3 albedo, float rough, float metal, float reflectance,
                  inout uint rng, out float3 l, out float3 weight)
{
    float2 u=float2(RTRandom(rng),RTRandom(rng));
    if (RTRandom(rng)<0.5) l=RTBasis(n,RTSampleCosine(u));
    else
    {
        float3 t,b; OrthonormalBasis(n,t,b);
        float3 vt=float3(dot(v,t),dot(v,b),dot(v,n));
        float3 ht=SampleGGXVNDF(vt,rough*rough,u.x,u.y);
        l=reflect(-v,t*ht.x+b*ht.y+n*ht.z);
    }
    weight=0;
    if (dot(n,l)<=0 || dot(n,v)<=0) return false;
    float pdf=RTBsdfPdf(n,v,l,rough);
    if (pdf<=0) return false;
    weight=RTEvalBSDF(n,v,l,albedo,rough,metal,reflectance)/pdf;
    return all(isfinite(weight));
}
#endif
