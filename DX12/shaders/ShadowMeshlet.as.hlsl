// ShadowMeshlet.as.hlsl - amplification stage of the CSM meshlet path.
//
// One group = kMeshletASGroupSize (32) clusters of ONE instance, as
// SceneMeshlet.as: the cull CS wrote the cascade's survivors as
// DISPATCH_MESH(blocks, kept, 1), SV_GroupID.x = cluster block, .y = the
// draw-local instance (Shadow.vs's SV_InstanceID). The cascade's
// MeshletCullCB bank sits at b4 (one per cascade, written with the planes the
// cascade's cull dispatch used - plane 5, the sun-side one, neutralised so
// pancaked casters beyond the light's near plane still draw).
//
// Which clusters survive (each stage gated by MeshletCullCB.cullMask):
//   LOD      MeshletLodSelected, orthographic: errors in shadow texels;
//            tau <= 0 = level 0 only
//   frustum  the cluster's sphere through the instance against the cascade
// No cone test (the buckets draw back faces, OpaqueNone / AlphaTest draw both,
// and the orthographic cone test is a different formula) and no Hi-Z (no shadow
// depth pyramid exists).
//
// KEEP THIS FILE PURE ASCII (docs: build-and-dxc-gotchas).

#include "shadow_vertex.hlsli"
#include "meshlet_cull.hlsli"

ConstantBuffer<MeshletCullCB> g_MeshletCull : register(b4, space0);

groupshared MeshletPayload s_payload;
// Exclusive prefix sum of the keep flags; [kMeshletASGroupSize] = the total.
groupshared uint s_scan[kMeshletASGroupSize + 1];
// The instance, computed once per group by thread 0.
groupshared float3 s_r0, s_r1, s_r2, s_t;
groupshared float  s_maxScale;
// Per-group statistics, flushed with one InterlockedAdd per counter.
groupshared uint s_stats[kMeshletStatsCount];

[numthreads(kMeshletASGroupSize, 1, 1)]
void main(uint gtid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    // The mesh slot's sidecar row (MeshletMeshInfo): where its clusters live.
    const uint4 info         = g_Buffers[g_MeshletCull.sidecarIdx].Load4(meshDescIdx * kMeshletInfoBytes);
    const uint  blobIdx      = info.x;
    const uint  firstCluster = info.y;
    const uint  clusterCount = info.z;
    const uint  cullMask     = g_MeshletCull.cullMask;

    if (gtid == 0)
    {
        const uint instSlot = ResolveInstanceSlot(gInstanceRemapIdx, instanceOffset + gid.y);
        const float4x4 world = InstanceBuffer[instSlot].world;
        s_r0 = world[0].xyz;
        s_r1 = world[1].xyz;
        s_r2 = world[2].xyz;
        s_t  = world[3].xyz;
        s_maxScale = MeshletMaxScale(s_r0, s_r1, s_r2);
        [unroll] for (uint s = 0; s < kMeshletStatsCount; ++s) s_stats[s] = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    const uint local   = gid.x * kMeshletASGroupSize + gtid;
    const uint cluster = firstCluster + local;
    bool keep = (blobIdx != kMeshletNoData) && (local < clusterCount);
    if (keep)
    {
        const uint base = MeshletDescByte(cluster);
        const float4 sphere = asfloat(g_Buffers[blobIdx].Load4(base));            // cullCenter.xyz, cullRadius
        const uint   counts = g_Buffers[blobIdx].Load(base + kMeshletDescCountsOffset);
        const uint   level  = (counts >> 16) & 0xFF;
        InterlockedAdd(s_stats[kMeshletStatTested], 1u);

        // LOD: the main view's rule, orthographic - the projected error is
        // errorWS * lodScale (texels per world unit), no distance. tau <= 0 (or the
        // stage off) = level 0.
        const float tau = (cullMask & kMeshletCullLod) != 0 ? g_MeshletCull.lodThreshold : 0.0f;
        if (tau > 0.0f)
        {
            const float4 selfS   = asfloat(g_Buffers[blobIdx].Load4(base + kMeshletDescLodSelfOffset));
            const float4 parentS = asfloat(g_Buffers[blobIdx].Load4(base + kMeshletDescLodParentOffset));
            const float2 errs    = asfloat(g_Buffers[blobIdx].Load2(base + kMeshletDescLodErrorOffset));
            keep = MeshletLodSelected(level,
                       MeshletXformPoint(selfS.xyz, s_r0, s_r1, s_r2, s_t), selfS.w * s_maxScale, errs.x * s_maxScale,
                       MeshletXformPoint(parentS.xyz, s_r0, s_r1, s_r2, s_t), parentS.w * s_maxScale,
                       errs.y * s_maxScale,
                       g_MeshletCull.cameraPosWS, g_MeshletCull.lodScale, tau, g_MeshletCull.lodOrtho,
                       g_MeshletCull.zNear);
        }
        else
            keep = (level == 0);
        if (!keep) InterlockedAdd(s_stats[kMeshletStatLodRejected], 1u);

        if (keep && (cullMask & kMeshletCullFrustum) != 0)
        {
            const float3 cWS = MeshletXformPoint(sphere.xyz, s_r0, s_r1, s_r2, s_t);
            if (MeshletSphereOutsideFrustum(cWS, sphere.w * s_maxScale, g_MeshletCull.frustumPlanes))
            {
                keep = false;
                InterlockedAdd(s_stats[kMeshletStatCullFrustum], 1u);
            }
        }
        if (keep)
        {
            InterlockedAdd(s_stats[kMeshletStatVisible], 1u);
            InterlockedAdd(s_stats[kMeshletStatVisibleTris], (counts >> 8) & 0xFF);
            InterlockedAdd(s_stats[kMeshletStatLevel0 + min(level, kMeshletStatLevels - 1)], 1u);
        }
    }

    s_scan[gtid] = keep ? 1u : 0u;
    GroupMemoryBarrierWithGroupSync();
    if (gtid == 0)
    {
        uint sum = 0;
        [unroll]
        for (uint i = 0; i < kMeshletASGroupSize; ++i)
        {
            const uint v = s_scan[i];
            s_scan[i] = sum;
            sum += v;
        }
        s_scan[kMeshletASGroupSize] = sum;
        s_payload.instance = gid.y;
        s_payload.blobIdx  = blobIdx;

        // One cascade's slice of the CSM statistics (statsBase = cascade * 16).
        if ((cullMask & kMeshletCullStats) != 0 && g_MeshletCull.statsUavIdx != kMeshletNoData)
        {
            RWStructuredBuffer<uint> stats = ResourceDescriptorHeap[g_MeshletCull.statsUavIdx];
            const uint statsBase = g_MeshletCull.pad0.x;
            [unroll] for (uint s = 0; s < kMeshletStatsCount; ++s)
                if (s_stats[s] != 0) InterlockedAdd(stats[statsBase + s], s_stats[s]);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (keep)
        s_payload.clusterIds[s_scan[gtid]] = cluster;

    DispatchMesh(s_scan[kMeshletASGroupSize], 1, 1, s_payload);
}
