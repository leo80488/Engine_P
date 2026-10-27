// DecalClusterCull.cs.hlsl - assigns decals to view-space clusters.
//
// Dispatch: ((CLUSTER_COUNT + 63) / 64, 1, 1); one thread per cluster.
// Fixed-layout indexing: cluster i writes its decal index list to
// [i * MAX_DECALS_PER_CLUSTER .. (i+1) * MAX_DECALS_PER_CLUSTER), so no atomic
// counter is needed.
//
// Input:  g_ClusterAABBs (built by ClusterBuild.cs.hlsl, shared with light cull)
//         g_Decals       (uploaded each frame by DecalPass::SetDecals)
// Output: per-cluster DecalGridEntry {offset, count} and the packed index list.

#include "decal_common.hlsli"

cbuffer DecalCB : register(b0, space2)
{
    float4x4 invProj;        // unused here, kept for CB layout parity with ClusterCB
    float    nearZ;
    float    farZ;
    uint     screenW;
    uint     screenH;
    uint     decalCount;
    uint3    _pad;
    float4x4 viewMatrix;     // world -> view (bounds into cluster space)
};

StructuredBuffer<ClusterAABB>     g_ClusterAABBs  : register(t0, space2);
StructuredBuffer<GPUDecal>        g_Decals        : register(t1, space2);

RWStructuredBuffer<uint>          g_DecalIndexList : register(u0, space2);
RWStructuredBuffer<DecalGridEntry> g_DecalGrid     : register(u1, space2);

[numthreads(64, 1, 1)]
void CSCullDecals(uint3 id : SV_DispatchThreadID)
{
    uint clusterIdx = id.x;
    if (clusterIdx >= CLUSTER_COUNT) return;

    ClusterAABB aabb = g_ClusterAABBs[clusterIdx];

    // Gather into registers first so the list can be sorted by sortLayer before
    // it is written. MAX_DECALS_PER_CLUSTER (16) is small enough that insertion
    // sort is the cheapest option.
    uint  localIdx [MAX_DECALS_PER_CLUSTER];
    float localSort[MAX_DECALS_PER_CLUSTER];
    uint  count = 0;

    for (uint i = 0; i < decalCount && count < MAX_DECALS_PER_CLUSTER; ++i)
    {
        GPUDecal d = g_Decals[i];

        // Bounds sphere center into view space. The radius is scale-invariant (the
        // CPU side already uses the decal OBB's worst-case radius).
        float3 centerVS = mul(float4(d.boundsCenter, 1.0), viewMatrix).xyz;

        if (!DecalSphereVsAABB(centerVS, d.boundsRadius, aabb))
            continue;

        // Insertion sort by ascending sortLayer. Ties keep insertion order (stable),
        // so same-layer decals blend in CPU upload order - predictable for artists.
        const float s = d.sortLayer;
        uint pos = count;
        [loop]
        while (pos > 0 && localSort[pos - 1] > s)
        {
            localIdx [pos] = localIdx [pos - 1];
            localSort[pos] = localSort[pos - 1];
            --pos;
        }
        localIdx [pos] = i;
        localSort[pos] = s;
        ++count;
    }

    const uint baseOffset = clusterIdx * MAX_DECALS_PER_CLUSTER;
    for (uint j = 0; j < count; ++j)
        g_DecalIndexList[baseOffset + j] = localIdx[j];

    g_DecalGrid[clusterIdx].offset = baseOffset;
    g_DecalGrid[clusterIdx].count  = count;
}
