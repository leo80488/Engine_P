// Ray count -> indirect dispatch args (port of AMD FFX
// hsr_prepare_indirect_args_hybrid/hw.hlsl). Three 1-thread entries on the shared
// hybrid-reflection root signature:
//   CSMainClear - runs first each frame: zeroes every counter slot so nothing
//                 depends on cross-frame or first-frame garbage
//   CSMainSW    - after ClassifyTiles, before IntersectSW (indirect)
//   CSMainHW    - after IntersectSW (which may append fallback rays), before
//                 IntersectHW (indirect)
// Each turns its ray count into a 2D dispatch (32 threads/group, split across rows
// of HR_GROUPS_PER_ROW groups to stay under the 65535 per-dimension cap), saves the
// count to the HIST slot (the intersect passes' guard) and zeroes the live counter
// for the next producer. rays == 0 yields (0, 0, 1), a legal no-op dispatch.

#include "HybridReflCommon.hlsli"

RWStructuredBuffer<uint> g_RayCounter   : register(u4, space0);
RWStructuredBuffer<uint> g_IndirectArgs : register(u5, space0);

[numthreads(1, 1, 1)]
void CSMainClear()
{
    [unroll]
    for (uint i = 0; i < 8u; ++i)
        g_RayCounter[i] = 0u;
}

[numthreads(1, 1, 1)]
void CSMainSW()
{
    const uint rays   = g_RayCounter[HR_CTR_SW_COUNT];
    const uint groups = (rays + HR_INTERSECT_GROUP_SIZE - 1u) / HR_INTERSECT_GROUP_SIZE;

    g_IndirectArgs[HR_ARGS_SW_OFFSET + 0] = min(groups, HR_GROUPS_PER_ROW);
    g_IndirectArgs[HR_ARGS_SW_OFFSET + 1] = (groups + HR_GROUPS_PER_ROW - 1u) / HR_GROUPS_PER_ROW;
    g_IndirectArgs[HR_ARGS_SW_OFFSET + 2] = 1u;

    g_RayCounter[HR_CTR_SW_HIST]  = rays;
    g_RayCounter[HR_CTR_SW_COUNT] = 0u;
}

[numthreads(1, 1, 1)]
void CSMainHW()
{
    const uint rays   = g_RayCounter[HR_CTR_HW_COUNT];
    const uint groups = (rays + HR_INTERSECT_GROUP_SIZE - 1u) / HR_INTERSECT_GROUP_SIZE;

    g_IndirectArgs[HR_ARGS_HW_OFFSET + 0] = min(groups, HR_GROUPS_PER_ROW);
    g_IndirectArgs[HR_ARGS_HW_OFFSET + 1] = (groups + HR_GROUPS_PER_ROW - 1u) / HR_GROUPS_PER_ROW;
    g_IndirectArgs[HR_ARGS_HW_OFFSET + 2] = 1u;

    g_RayCounter[HR_CTR_HW_HIST]  = rays;
    g_RayCounter[HR_CTR_HW_COUNT] = 0u;
}
