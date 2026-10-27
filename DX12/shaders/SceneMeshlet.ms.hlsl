// SceneMeshlet.ms.hlsl - mesh stage of the scene's meshlet GBuffer path.
//
// One group = one cluster from the amplification payload. 128 threads: the
// first vertexCount (<= 64) each produce one vertex through GBufferVertex - the
// SAME function GBuffer.vs runs (gbuffer_vertex.hlsli), with the cluster's
// vertex ref as the fetched index - and the first triCount (<= 124) each emit
// one triangle. Whatever level the cluster belongs to, the refs index the
// library's original VB, so this stage never changes with LOD.
//
// The output struct is GBuffer.vs's PSIn, so GBuffer.ps links unchanged. In the
// Meshlets view mode (MeshletCullCB.debugMode) COLOR carries the debug colour.
//
// KEEP THIS FILE PURE ASCII (docs: build-and-dxc-gotchas).

#include "gbuffer_vertex.hlsli"
#include "meshlet_shared.hlsli"

// Read for the Meshlets view mode only (debugMode); the cull fields are the AS's.
ConstantBuffer<MeshletCullCB> g_MeshletCull : register(b4, space0);

[numthreads(kMeshletMSGroupSize, 1, 1)]
[outputtopology("triangle")]
void main(uint gtid : SV_GroupThreadID,
          uint gid  : SV_GroupID,
          in payload MeshletPayload p,
          out vertices PSIn verts[kMeshletMaxVerts],
          out indices uint3 tris[kMeshletMaxTris])
{
    const uint cluster = p.clusterIds[gid];
    // coneAxisCutoff | vertexRefOffset | triangleOffset | counts
    const uint4 d = g_Buffers[p.blobIdx].Load4(MeshletDescByte(cluster) + kMeshletDescConeOffset);
    const uint vertexCount = d.w & 0xFF;
    const uint triCount    = (d.w >> 8) & 0xFF;

    SetMeshOutputCounts(vertexCount, triCount);

    if (gtid < vertexCount)
    {
        const uint vid = g_Buffers[p.blobIdx].Load(MeshletDwordByte(d.y + gtid));
        MeshDescriptor md = MeshDescriptors[meshDescIdx];
        PSIn v = GBufferVertex(md, vid, p.instance);
        // The Meshlets view mode (MeshletDebug.ps shows COLOR raw). A uniform
        // branch, off in every normal frame.
        if (g_MeshletCull.debugMode != kMeshletDebugOff)
            v.col = MeshletDebugColor(g_MeshletCull.debugMode, cluster, (d.w >> 16) & 0xFF);
        verts[gtid] = v;
    }
    if (gtid < triCount)
    {
        const uint t = g_Buffers[p.blobIdx].Load(MeshletDwordByte(d.z + gtid));
        tris[gtid] = uint3(t & 0xFF, (t >> 8) & 0xFF, (t >> 16) & 0xFF);
    }
}
