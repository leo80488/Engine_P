// DXRParity.lib.hlsl - the DXR half of the parity self-test (DXRParity.hlsli has
// what both halves share). A DXIL library: one ray-generation shader, one miss
// shader, one triangle hit group whose closest-hit shader records the hit. Every
// TLAS instance's hit-group contribution is 0 (DDGISceneAS) and TraceRay adds
// none, so the one hit-group record serves all.

#include "DXRParity.hlsli"

struct ParityPayload
{
    ParityHit hit;
};

[shader("raygeneration")]
void ParityRayGen()
{
    const uint2 idx = DispatchRaysIndex().xy;
    const uint  i   = idx.y * g_Width + idx.x;
    if (i >= g_Count) return;

    ParityPayload payload;
    payload.hit = (ParityHit)0;
    // RayContributionToHitGroupIndex 0, MultiplierForGeometryContributionToHitGroupIndex 0,
    // MissShaderIndex 0.
    TraceRay(g_TLAS, RAY_FLAG_FORCE_OPAQUE, 0xFF, 0, 0, 0, ParityRayDesc(g_Rays[i]), payload);
    g_Hits[i] = payload.hit;
}

[shader("miss")]
void ParityMiss(inout ParityPayload payload)
{
    payload.hit = (ParityHit)0;
}

[shader("closesthit")]
void ParityClosestHit(inout ParityPayload payload, in BuiltInTriangleIntersectionAttributes attribs)
{
    payload.hit.t              = RayTCurrent();
    payload.hit.instanceID     = InstanceID();
    payload.hit.primitiveIndex = PrimitiveIndex();
    payload.hit.hit            = 1;
}
