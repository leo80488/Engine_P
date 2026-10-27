// SceneMeshlet.as.hlsl - amplification stage of the scene's meshlet GBuffer path.
//
// One group = kMeshletASGroupSize (32) clusters of ONE instance. The CPU
// dispatches DispatchMesh(blocks, instanceCount, 1) per draw packet, with
// blocks = ceil(the entry's clusters over ALL levels / 32): SV_GroupID.x is the
// cluster block, SV_GroupID.y the draw-local instance (the VS's SV_InstanceID).
//
// Which clusters survive, in order (each stage gated by MeshletCullCB.cullMask):
//   shaded   only the entry's leading clusters whose levels pass the LOD shading
//            check (sidecar .w, Resource/MeshletData.h): a coarser level whose
//            triangles take other faces' normals shades black; the last level
//            kept is this view's root. The shadow views draw every level.
//   LOD      MeshletLodSelected: the cluster's group is fine enough for this view
//            and its parent is not; tau <= 0 = level 0 only
//   frustum  the cluster's sphere through the instance (meshlet_cull.hlsli)
//   cone     the normal cone, in object space; off for a DOUBLE_SIDED draw and a
//            cluster the builder flagged NO_CONE
//   Hi-Z     the sphere's world AABB united with last frame's, against last
//            frame's depth pyramid (hiz_cull.hlsli - InstanceCull's own test);
//            off while the pyramid is unusable (MeshletCullCB.hiZValid: none
//            yet, a camera cut or jump) and for NO_OCCLUSION / NO_GEOM instances
// Culling never changes the picture - the meshlet_cull_identity pack proves it
// with the stages switched on mid-run; tests/unit/meshlet_cull_test.py proves the
// tests themselves against the D3D rasterizer.
//
// The survivors are compacted with an ORDERED group-scope prefix sum (not wave
// intrinsics: wave width varies and a 32-thread group may span waves), so the
// mesh groups keep cluster order and the raster order is deterministic.
//
// KEEP THIS FILE PURE ASCII (docs: build-and-dxc-gotchas).

#include "gbuffer_vertex.hlsli"     // b0 push constants, t0 InstanceBuffer, t1 MeshDescriptors, g_Buffers
#include "meshlet_cull.hlsli"
#include "hiz_cull.hlsli"

ConstantBuffer<MeshletCullCB> g_MeshletCull : register(b4, space0);

// The 4th root constant is GBuffer.vs's vestigial prevPosInfo; the meshlet draw
// uses it for per-draw flags (GBufferPass::RecordChunk).
#define g_MeshletDrawFlags prevPosInfo

groupshared MeshletPayload s_payload;
// Exclusive prefix sum of the keep flags; [kMeshletASGroupSize] = the total.
groupshared uint s_scan[kMeshletASGroupSize + 1];
// The instance, computed once per group by thread 0.
groupshared float3 s_r0, s_r1, s_r2, s_t, s_camOS;
groupshared float  s_maxScale;
groupshared uint   s_camValid;
// The level that ends this view's chain when the shading check cut it short
// (its parent error is then treated as the root's); 0xFFFFFFFF = not cut.
groupshared uint   s_rootLevel;
// Hi-Z: whether this instance takes the test, and where it was last frame.
groupshared uint   s_hiZ;
groupshared float3 s_p0, s_p1, s_p2, s_pt;
groupshared float  s_prevMaxScale;
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
    const uint  shadedCount  = min(info.w, clusterCount);   // MeshDescriptorHeap::MeshletInfo
    const uint  cullMask     = g_MeshletCull.cullMask;

    if (gtid == 0)
    {
        s_rootLevel = 0xFFFFFFFFu;
        if (blobIdx != kMeshletNoData && shadedCount > 0 && shadedCount < clusterCount)
            s_rootLevel = (g_Buffers[blobIdx].Load(MeshletDescByte(firstCluster + shadedCount - 1) +
                                                   kMeshletDescCountsOffset) >> 16) & 0xFF;
        const uint instSlot = ResolveInstanceSlot(gInstanceRemapIdx, instanceOffset + gid.y);
        const float4x4 world = InstanceBuffer[instSlot].world;
        s_r0 = world[0].xyz;
        s_r1 = world[1].xyz;
        s_r2 = world[2].xyz;
        s_t  = world[3].xyz;
        s_maxScale = MeshletMaxScale(s_r0, s_r1, s_r2);
        float3 camOS = float3(0, 0, 0);
        s_camValid = MeshletWorldToObject(g_MeshletCull.cameraPosWS, s_r0, s_r1, s_r2, s_t, camOS) ? 1u : 0u;
        s_camOS = camOS;
        // The pyramid and its VP are one frame old: test a moving instance
        // where it WAS as well as where it is (InstanceCull's union; settled
        // instances have prevWorld == world). Skinned (NO_OCCLUSION) and WPO
        // (NO_GEOM) instances never take the mesh path - honoured anyway.
        s_hiZ = ((cullMask & kMeshletCullHiZ) != 0 && g_MeshletCull.hiZValid != 0 &&
                 (InstanceBuffer[instSlot].instFlags &
                  (INSTANCE_FLAG_NO_OCCLUSION_CULL | INSTANCE_FLAG_NO_GEOM_CULL)) == 0) ? 1u : 0u;
        if (s_hiZ != 0)
        {
            const float4x4 prevWorld = InstanceBuffer[instSlot].prevWorld;
            s_p0 = prevWorld[0].xyz;
            s_p1 = prevWorld[1].xyz;
            s_p2 = prevWorld[2].xyz;
            s_pt = prevWorld[3].xyz;
            s_prevMaxScale = MeshletMaxScale(s_p0, s_p1, s_p2);
        }
        [unroll] for (uint s = 0; s < kMeshletStatsCount; ++s) s_stats[s] = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    const uint local   = gid.x * kMeshletASGroupSize + gtid;
    const uint cluster = firstCluster + local;
    bool keep = (blobIdx != kMeshletNoData) && (local < shadedCount);
    if (keep)
    {
        const uint base = MeshletDescByte(cluster);
        const float4 sphere = asfloat(g_Buffers[blobIdx].Load4(base));            // cullCenter.xyz, cullRadius
        const uint   cone   = g_Buffers[blobIdx].Load(base + kMeshletDescConeOffset);    // int8 axis.xyz | cutoff
        const uint   counts = g_Buffers[blobIdx].Load(base + kMeshletDescCountsOffset);
        const uint   level  = (counts >> 16) & 0xFF;
        const uint   flags  = counts >> 24;
        InterlockedAdd(s_stats[kMeshletStatTested], 1u);

        // LOD. The group spheres and errors through the instance: centre transformed,
        // radius and error x the largest axis scale (the conservative side for
        // non-uniform scale). Every cluster of one group carries the same values, so the
        // whole group takes one answer; when a group is a whole entry level (discrete
        // LOD), one instance draws ONE level. tau <= 0 (or the stage off) = level 0 only.
        const float tau = (cullMask & kMeshletCullLod) != 0 ? g_MeshletCull.lodThreshold : 0.0f;
        if (tau > 0.0f)
        {
            const float4 selfS   = asfloat(g_Buffers[blobIdx].Load4(base + kMeshletDescLodSelfOffset));
            const float4 parentS = asfloat(g_Buffers[blobIdx].Load4(base + kMeshletDescLodParentOffset));
            const float2 errs    = asfloat(g_Buffers[blobIdx].Load2(base + kMeshletDescLodErrorOffset));
            keep = MeshletLodSelected(level,
                       MeshletXformPoint(selfS.xyz, s_r0, s_r1, s_r2, s_t), selfS.w * s_maxScale, errs.x * s_maxScale,
                       MeshletXformPoint(parentS.xyz, s_r0, s_r1, s_r2, s_t), parentS.w * s_maxScale,
                       level == s_rootLevel ? kMeshletRootErrorMin : errs.y * s_maxScale,
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
        if (keep && (cullMask & kMeshletCullCone) != 0 && s_camValid != 0 &&
            (flags & kMeshletFlagNoCone) == 0 && (g_MeshletDrawFlags & kMeshletDrawNoCone) == 0)
        {
            if (MeshletConeBackfacing(sphere.xyz, sphere.w, cone, s_camOS))
            {
                keep = false;
                InterlockedAdd(s_stats[kMeshletStatCullCone], 1u);
            }
        }
        if (keep && s_hiZ != 0)
        {
            // The sphere's world AABB now and last frame (radius x the largest
            // axis scale, as the frustum stage), against the pyramid with the
            // VP it was captured with.
            const float3 cWS   = MeshletXformPoint(sphere.xyz, s_r0, s_r1, s_r2, s_t);
            const float  rWS   = sphere.w * s_maxScale;
            const float3 cPrev = MeshletXformPoint(sphere.xyz, s_p0, s_p1, s_p2, s_pt);
            const float  rPrev = sphere.w * s_prevMaxScale;
            Texture2D<float> hiZ = ResourceDescriptorHeap[g_MeshletCull.hiZSrvIdx];
            if (!HiZOcclusionTest(min(cWS - rWS, cPrev - rPrev), max(cWS + rWS, cPrev + rPrev),
                                  g_MeshletCull.hiZViewProj, hiZ, g_MeshletCull.hiZWidth,
                                  g_MeshletCull.hiZHeight, g_MeshletCull.hiZMipCount))
            {
                keep = false;
                InterlockedAdd(s_stats[kMeshletStatCullHiZ], 1u);
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

        if ((cullMask & kMeshletCullStats) != 0 && g_MeshletCull.statsUavIdx != kMeshletNoData)
        {
            RWStructuredBuffer<uint> stats = ResourceDescriptorHeap[g_MeshletCull.statsUavIdx];
            [unroll] for (uint s = 0; s < kMeshletStatsCount; ++s)
                if (s_stats[s] != 0) InterlockedAdd(stats[s], s_stats[s]);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (keep)
        s_payload.clusterIds[s_scan[gtid]] = cluster;

    DispatchMesh(s_scan[kMeshletASGroupSize], 1, 1, s_payload);
}
