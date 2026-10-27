// DXRParity.cs.hlsl - the inline RayQuery half of the DXR parity self-test
// (DXRParity.hlsli has what both halves share). The same rays, TLAS and effective
// flags as DXRParity.lib.hlsl (FORCE_OPAQUE in the query's template, nothing added
// at the call).

#include "DXRParity.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint i = id.x;
    if (i >= g_Count) return;

    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(g_TLAS, RAY_FLAG_NONE, 0xFF, ParityRayDesc(g_Rays[i]));
    while (q.Proceed()) {}

    ParityHit h = (ParityHit)0;
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        h.t              = q.CommittedRayT();
        h.instanceID     = q.CommittedInstanceID();
        h.primitiveIndex = q.CommittedPrimitiveIndex();
        h.hit            = 1;
    }
    g_Hits[i] = h;
}
