// DDGIFinalizeIndirect.cs.hlsl - writes the indirect args for the trace and for the
// per-probe kernels.
// Reads g_RayAlloc[0] (total rays, from DDGIRayAllocation.cs) and writes the trace's
// group dimensions to g_DispatchArgs[0..2]. The args buffer is separate so it can sit in
// D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT for ExecuteIndirect without conflicting with
// the UAV state of the other buffers.

#include "DDGICommon.hlsli"

ConstantBuffer<DDGIVolumeGPU> g_Vol     : register(b0, space0);
RWStructuredBuffer<uint> g_RayAlloc     : register(u3, space0);
RWStructuredBuffer<uint> g_DispatchArgs : register(u4, space0);
// The ray-count buffer; this kernel reads the active-probe count DDGIRayAllocation wrote
// into it (layout in DDGICommon.hlsli). DDGIPass binds root param 12 (u2) once per
// Execute, ahead of every dispatch.
RWStructuredBuffer<uint> g_RayCount     : register(u2, space0);

[numthreads(1, 1, 1)]
void main()
{
    const uint total  = g_RayAlloc[0];
    const uint groups = (total + (DDGI_TRACE_GROUP_SIZE - 1u)) / DDGI_TRACE_GROUP_SIZE;

    // Fold into 2D once the group count would exceed the per-dimension limit; it must
    // not be a clamp (see DDGI_TRACE_GROUP_SIZE in DDGICommon.hlsli).
    // The <= branch is exact (gy == 1). The > branch over-dispatches up to
    // DDGI_MAX_GROUPS_X-1 groups; DDGIRayTrace early-outs on linearIdx >= totalRays, so the
    // tail costs one buffer load per idle thread.
    uint gx, gy;
    if (groups <= DDGI_MAX_GROUPS_X)
    {
        gx = max(groups, 1u);
        gy = 1u;
    }
    else
    {
        gx = DDGI_MAX_GROUPS_X;
        gy = (groups + (DDGI_MAX_GROUPS_X - 1u)) / DDGI_MAX_GROUPS_X;
    }

    g_DispatchArgs[0] = gx;
    g_DispatchArgs[1] = gy;
    g_DispatchArgs[2] = 1u;

    // ---- Args for the three "one group per probe" kernels ----
    // Relight.Irr / Relight.Dep / Border run over the compacted active-probe list, so the
    // group count is that list's length. This second D3D12_DISPATCH_ARGUMENTS sits at byte
    // offset 16 (uint index 4); the caller passes that offset to ExecuteIndirect. Zero
    // groups is legal (every probe skipped this frame) and those kernels are no-ops then,
    // so no clamp to 1. Component reflection caps probe counts at 32 per axis (32768
    // probes), far below the 65535 per-dimension limit the trace folds around.
    const uint activeProbes = g_RayCount[DDGI_ActiveCountSlot(g_Vol)];
    g_DispatchArgs[4] = activeProbes;
    g_DispatchArgs[5] = 1u;
    g_DispatchArgs[6] = 1u;
}
