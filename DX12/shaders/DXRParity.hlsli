// DXRParity.hlsli - shared declarations of the DXR / inline RayQuery parity
// self-test (host: DXRParityTest).
//
// The engine traces every ray with an inline RayQuery; the DXR state-object path
// is kept unused and proved by this test. The CPU writes one grid of camera rays
// (g_Rays) that BOTH kernels read - a ray built in two separately compiled shaders
// could differ in its last bits - and each kernel writes the closest hit of ray i
// to its own g_Hits[i]: DXRParity.lib.hlsl through TraceRay, DXRParity.cs.hlsl
// through RayQuery, same TLAS, same flags (FORCE_OPAQUE: no any-hit, no candidate
// loop, so both report the closest triangle).
//
// One pipeline layout serves both (DXRParityTest::kLayout): t0 the TLAS (root
// SRV), t1 the rays (root SRV), u0 the hits (table), b0 four constants.

#ifndef DXR_PARITY_HLSLI
#define DXR_PARITY_HLSLI

struct ParityRay
{
    float3 origin;
    float  tMin;
    float3 direction;
    float  tMax;
};

struct ParityHit
{
    float t;                // 0 for a miss
    uint  instanceID;       // InstanceID() / CommittedInstanceID()
    uint  primitiveIndex;   // PrimitiveIndex() / CommittedPrimitiveIndex()
    uint  hit;              // 1 = a triangle was hit
};

RaytracingAccelerationStructure g_TLAS  : register(t0);
StructuredBuffer<ParityRay>     g_Rays  : register(t1);
RWStructuredBuffer<ParityHit>   g_Hits  : register(u0);

cbuffer ParityParams : register(b0)
{
    uint g_Width;    // the ray grid: ray i = y * g_Width + x
    uint g_Height;
    uint g_Count;    // g_Width * g_Height
    uint g_Pad;
};

RayDesc ParityRayDesc(ParityRay r)
{
    RayDesc d;
    d.Origin    = r.origin;
    d.TMin      = r.tMin;
    d.Direction = r.direction;
    d.TMax      = r.tMax;
    return d;
}

#endif // DXR_PARITY_HLSLI
