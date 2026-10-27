// ShadowMeshlet.ms.hlsl - mesh stage of the CSM meshlet path.
//
// One group = one cluster from the amplification payload, as SceneMeshlet.ms:
// the first vertexCount (<= 64) threads each produce one vertex through
// ShadowVertex - the SAME function Shadow.vs runs (shadow_vertex.hlsli) - with
// the cluster's vertex ref as the fetched index, the first triCount (<= 124)
// threads one triangle each. The output is Shadow.vs's VSOut, so Shadow.ps
// (alpha test, dissolve) links unchanged and a depth-only PSO needs no PS.
//
// KEEP THIS FILE PURE ASCII (docs: build-and-dxc-gotchas).

#include "shadow_vertex.hlsli"
#include "meshlet_shared.hlsli"

[numthreads(kMeshletMSGroupSize, 1, 1)]
[outputtopology("triangle")]
void main(uint gtid : SV_GroupThreadID,
          uint gid  : SV_GroupID,
          in payload MeshletPayload p,
          out vertices VSOut verts[kMeshletMaxVerts],
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
        // The draw-local instance the AS kept, through this view's remap (the
        // cascade's WORKING remap on the retained path) - Shadow.vs's slot.
        const uint instSlot = ResolveInstanceSlot(gInstanceRemapIdx, instanceOffset + p.instance);
        verts[gtid] = ShadowVertex(md, vid, instSlot);
    }
    if (gtid < triCount)
    {
        const uint t = g_Buffers[p.blobIdx].Load(MeshletDwordByte(d.z + gtid));
        tris[gtid] = uint3(t & 0xFF, (t >> 8) & 0xFF, (t >> 16) & 0xFF);
    }
}
