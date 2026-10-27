// Assigns baked reflection probes to clusters, shaped like ClusterCull.cs.hlsl: one thread
// per cluster, a fixed slice of the index list each (no atomic counter).
// The cull volume is the influence outer volume as a conservative view-space bounding sphere,
// derived from the rigid world->influence matrix + inflExtents / inflShape of the probe.
// MAX_PROBES_PER_CLUSTER is deliberately small (8): per-pixel probe loops dominate the
// lighting pass cost. Probes past the cap are dropped; raise it if a scene reliably overflows.
// ReflectionProbe comes from shared/reflection_probe_gpu.hlsli, the definition C++ uses too.

#include "cluster_common.hlsli"
#include "shared/reflection_probe_gpu.hlsli"   // struct ReflectionProbe
#include "bindless.hlsli"   // typed bindless handles

#define MAX_PROBES_PER_CLUSTER 8
#define INFL_SPHERE 1u

struct ProbeGridEntry
{
    uint offset;
    uint count;
};

#define REFLECTION_PROBE_FLAG_BAKED 1u

cbuffer ClusterCB : register(b0, space2)
{
    float4x4 invProj;
    float    nearZ;
    float    farZ;
    uint     screenW;
    uint     screenH;
    uint     lightCount;
    uint     probeCount;     // count of valid entries in the probe buffer
    uint     isOrtho;        // ClusterBuild's; unused here
    // Heap index of the reflection-probe StructuredBuffer; valid whenever this kernel runs.
    uint     probeBufferIdx;
    float4x4 viewMatrix;
};

StructuredBuffer<ClusterAABB>      g_ClusterAABBs   : register(t0, space2);

RWStructuredBuffer<uint>           g_ProbeIndexList : register(u0, space2);
RWStructuredBuffer<ProbeGridEntry> g_ProbeGrid      : register(u1, space2);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint clusterIdx = id.x;
    if (clusterIdx >= CLUSTER_COUNT) return;

    ClusterAABB aabb = g_ClusterAABBs[clusterIdx];

    uint baseOffset = clusterIdx * MAX_PROBES_PER_CLUSTER;
    uint count = 0;

    StructuredBuffer<ReflectionProbe> probes = BindlessSBuf<ReflectionProbe>(probeBufferIdx);
    for (uint i = 0; i < probeCount && count < MAX_PROBES_PER_CLUSTER; i++)
    {
        ReflectionProbe p = probes[i];
        if ((p.flags & REFLECTION_PROBE_FLAG_BAKED) == 0u) continue;

        // Influence world centre = -(t . rows) of the rigid world->influence matrix (the world point
        // that maps to the influence origin). Conservative radius: outerR for a sphere, the box
        // half-extent diagonal otherwise.
        float3 tvec  = float3(p.worldToInflR0.w, p.worldToInflR1.w, p.worldToInflR2.w);
        float3 centre = -(tvec.x * p.worldToInflR0.xyz
                        + tvec.y * p.worldToInflR1.xyz
                        + tvec.z * p.worldToInflR2.xyz);
        float radius = (p.inflShape == INFL_SPHERE) ? p.inflExtents.x
                                                    : length(p.inflExtents);
        if (radius <= 0.0) continue;

        float3 viewPos = mul(float4(centre, 1.0), viewMatrix).xyz;
        if (SphereVsAABB(viewPos, radius, aabb))
        {
            g_ProbeIndexList[baseOffset + count] = i;
            count++;
        }
    }

    g_ProbeGrid[clusterIdx].offset = baseOffset;
    g_ProbeGrid[clusterIdx].count  = count;
}
