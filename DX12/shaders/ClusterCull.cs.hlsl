// Assigns lights to clusters, one thread per cluster (dispatch CLUSTER_COUNT / 64 groups).
// Cluster i owns the fixed slice [i * MAX_PER_CLUSTER, (i+1) * MAX_PER_CLUSTER) of the
// index list, so no atomic counter is needed.

#include "cluster_common.hlsli"

#define MAX_PER_CLUSTER 32  // max lights stored per cluster in the index list

cbuffer ClusterCB : register(b0, space2)
{
    float4x4 invProj;
    float    nearZ;
    float    farZ;
    uint     screenW;
    uint     screenH;
    uint     lightCount;
    uint3    _pad;
    float4x4 viewMatrix;
};

StructuredBuffer<ClusterAABB>      g_ClusterAABBs   : register(t0, space2);
StructuredBuffer<GPULight>         g_Lights         : register(t1, space2);

RWStructuredBuffer<uint>           g_LightIndexList : register(u0, space2);
RWStructuredBuffer<LightGridEntry> g_LightGrid      : register(u1, space2);

[numthreads(64, 1, 1)]
void CSCullLights(uint3 id : SV_DispatchThreadID)
{
    uint clusterIdx = id.x;
    if (clusterIdx >= CLUSTER_COUNT) return;

    ClusterAABB aabb = g_ClusterAABBs[clusterIdx];

    uint baseOffset = clusterIdx * MAX_PER_CLUSTER;
    uint count = 0;

    for (uint i = 0; i < lightCount; i++)
    {
        GPULight light = g_Lights[i];
        if (light.type == 0) continue; // skip directional

        float3 viewPos = mul(float4(light.position, 1.0), viewMatrix).xyz;

        if (SphereVsAABB(viewPos, light.radius, aabb))
        {
            // Overflow must not drop lights: offset 0xFFFFFFFF + count = lightCount tells consumers to
            // walk the whole light buffer for this cluster.
            if (count == MAX_PER_CLUSTER)
            {
                g_LightGrid[clusterIdx].offset = 0xFFFFFFFFu;
                g_LightGrid[clusterIdx].count = lightCount;
                return;
            }
            g_LightIndexList[baseOffset + count] = i;
            count++;
        }
    }

    g_LightGrid[clusterIdx].offset = baseOffset;
    g_LightGrid[clusterIdx].count  = count;
}
