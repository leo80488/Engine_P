#ifndef RESTIR_GI_RESERVOIR_HLSLI
#define RESTIR_GI_RESERVOIR_HLSLI
#include "restir_reservoir.hlsli"
#include "core/color.hlsli"   // Luminance
#include "restir_gi_math.hlsli"
// Engine-local 40-byte ABI: store the off-screen secondary position and normal.
// W is finalized inverse density, never an overloaded streaming sum on disk.
struct GIReservoir
{
    float3 P; uint normal;
    float3 radiance; float W;
    float target; uint metadata;
};
// Receiver record: P is the lifted receiver point (= the ray origin), normal
// the shading normal, geoNormal the depth-reconstructed geometric normal
// (the horizon of every connection), coverageCount = f16 coverage | count<<16.
struct GISurface
{
    float3 P; uint normal;
    uint geoNormal,material,coverageCount,albedo;
};
float GISurfaceCoverage(GISurface s) { return f16tof32(s.coverageCount&0xffffu); }
uint GISurfaceCount(GISurface s) { return s.coverageCount>>16; }
void GISetCoverageCount(inout GISurface s,float coverage,uint count)
{ s.coverageCount=(f32tof16(saturate(coverage))&0xffffu)|(min(count,65535u)<<16); }
uint GIM(GIReservoir r) { return r.metadata&63u; }
uint GIAge(GIReservoir r) { return (r.metadata>>6)&63u; }
uint GIPackNormal(float3 n)
{
    uint2 e=uint2(round(saturate(OctEncode(n)*.5+.5)*65535));
    return e.x|(e.y<<16);
}
float3 GIUnpackNormal(uint n)
{ return OctDecode(float2(n&65535u,n>>16)*(2.0/65535)-1); }
float GIGeometry(float3 receiver,GIReservoir sample)
{
    float3 delta=receiver-sample.P; float distance2=dot(delta,delta);
    return distance2>1e-8?saturate(dot(GIUnpackNormal(sample.normal),delta*rsqrt(distance2)))/distance2:0;
}
// Solid-angle change at the SECONDARY surface, with no receiver cosine ratio.
// See pinned RTXDI-Library Utils/Math.hlsli RTXDI_CalculateJacobian.
float GIJacobian(float3 receiver,float3 source,GIReservoir sample)
{
    float3 oldDelta=source-sample.P,newDelta=receiver-sample.P;
    float oldD2=dot(oldDelta,oldDelta),newD2=dot(newDelta,newDelta);
    float3 n=GIUnpackNormal(sample.normal);
    float oldCos=dot(n,oldDelta*rsqrt(max(oldD2,1e-20)));
    float newCos=dot(n,newDelta*rsqrt(max(newD2,1e-20)));
    return GIReconnectionJacobian(oldD2,newD2,oldCos,newCos);
}
float3 GIIntegrand(float3 p,float3 n,GIReservoir sample)
{
    float3 delta=sample.P-p; float d2=dot(delta,delta);
    return d2>1e-8?sample.radiance*(saturate(dot(n,delta*rsqrt(d2)))/PI):0;
}
float GITarget(float3 p,float3 n,GIReservoir sample,float3 rho)
{ return Luminance(max(rho*GIIntegrand(p,n,sample),0)); }
uint GIPackAlbedo(float3 rho) { uint3 c=uint3(round(saturate(rho)*1023)); return c.x|(c.y<<10)|(c.z<<20); }
float3 GIUnpackAlbedo(uint c) { return float3(c&1023u,(c>>10)&1023u,(c>>20)&1023u)/1023; }
void GISelect(inout GIReservoir result,inout float sum,GIReservoir candidate,float target,float weight,float random)
{
    if(!(weight>0) || !isfinite(weight)) return;
    sum+=weight;
    if(random*sum<weight) { result=candidate; result.target=target; }
}
#endif
