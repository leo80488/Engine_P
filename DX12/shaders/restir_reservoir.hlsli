#ifndef RESTIR_RESERVOIR_HLSLI
#define RESTIR_RESERVOIR_HLSLI
// Screen-independent reservoir algebra. WeightSum is unnormalised during
// streaming; DIWeight returns the contribution weight W_Y stored in history.
#include "core/hash.hlsli"
float2 RTReservoirR2(uint2 pixel,uint frame,uint salt)
{
    uint index=HilbertIndex64(pixel)+4096u*HashLowbias32((pixel.x>>6)^((pixel.y>>6)<<16)^salt);
    // R2 in fixed point: integer products keep their fractional bits however large
    // index and frame grow.
    uint2 bits=uint2(index*3242174889u,index*2447445413u);
    bits+=uint2(frame*2447445413u,frame*3242174889u);
    return float2(bits>>8)*(1.0/16777216.0);
}
struct DIReservoir
{
    uint lightIndex, M, age, pathSeed;
    uint reactiveFrames; // receiver-local NRD cooldown; temporal output owns it
    float weightSum, target;
    bool valid, visibilityTested, visible;
};
void DIReset(out DIReservoir r) { r=(DIReservoir)0; }
void DIStream(inout DIReservoir r, uint lightIndex, float target,
              float proposalPdf, float randomValue)
{
    ++r.M; // Zero-target candidates are still part of the proposal population.
    float weight=proposalPdf>0 ? target/proposalPdf : 0;
    if (!(weight>0) || !isfinite(weight)) return;
    r.weightSum+=weight;
    if (!r.valid || randomValue*r.weightSum<weight)
    {
        r.lightIndex=lightIndex; r.target=target; r.valid=true;
        r.visibilityTested=false; r.visible=false;
    }
}
// DIStream that also records the candidate's light-point seed: the point on an emitter
// with non-zero sourceRadius that the sample's visibility rays aim at. It travels with
// the sample through every merge.
void DIStreamSeeded(inout DIReservoir r, uint lightIndex, uint seed, float target,
                    float proposalPdf, float randomValue)
{
    ++r.M;
    float weight=proposalPdf>0 ? target/proposalPdf : 0;
    if (!(weight>0) || !isfinite(weight)) return;
    r.weightSum+=weight;
    if (!r.valid || randomValue*r.weightSum<weight)
    {
        r.lightIndex=lightIndex; r.target=target; r.valid=true;
        r.visibilityTested=false; r.visible=false; r.pathSeed=seed;
    }
}
float DIWeight(DIReservoir r)
{ return r.valid && r.M>0 && r.target>0 ? r.weightSum/(float(r.M)*r.target) : 0; }
float DITarget(float3 diffuse, float3 specular)
{ return dot(max(diffuse+specular,0),float3(1,1,1)); }
#include "core/packing.hlsli"   // OctEncode, OctDecode
// Exactly 20 bytes. W is stored independently of tested visibility.
struct DIPacked { uint reference,seed; float W,target; uint meta; };
DIPacked DIPack(DIReservoir r,float3 normal)
{
    DIPacked p;
    p.reference=(r.lightIndex&0xffffu)|(uint(r.valid)<<20)|(uint(r.visibilityTested)<<21)|(uint(r.visible)<<22)
        |(min(r.reactiveFrames,7u)<<24);
    p.seed=r.pathSeed; p.W=DIWeight(r); p.target=r.target;
    uint2 oct=uint2(round(saturate(OctEncode(normal)*.5+.5)*1023));
    p.meta=oct.x|(oct.y<<10)|(min(r.M,63u)<<20)|(min(r.age,63u)<<26);
    return p;
}
DIReservoir DIUnpack(DIPacked p,out float3 normal)
{
    DIReservoir r=(DIReservoir)0;
    r.lightIndex=p.reference&0xffffu; r.valid=(p.reference&(1u<<20))!=0;
    r.visibilityTested=(p.reference&(1u<<21))!=0; r.visible=(p.reference&(1u<<22))!=0;
    r.reactiveFrames=(p.reference>>24)&7u;
    r.pathSeed=p.seed; r.target=p.target; r.M=(p.meta>>20)&63u; r.age=p.meta>>26;
    r.weightSum=p.W*r.M*r.target;
    normal=OctDecode(float2(p.meta&1023u,(p.meta>>10)&1023u)*(2.0/1023)-1);
    return r;
}
void DICap(inout DIReservoir r,uint cap)
{
    uint count=min(r.M,cap);
    r.weightSum*=r.M>0?float(count)/r.M:0;
    r.M=count;
}
// Weight is an unnormalised stream weight; caller owns the population count.
void DISelect(inout DIReservoir outR,DIReservoir candidate,float target,float weight,float randomValue)
{
    if(!(weight>0) || !isfinite(weight) || !candidate.valid) return;
    float sum=outR.weightSum+weight;
    if(!outR.valid || randomValue*sum<weight)
    {
        uint population=outR.M;
        outR=candidate; outR.target=target; outR.M=population;
    }
    outR.weightSum=sum;
}
#include "restir_pairwise.hlsli"
#endif
