// GBuffer.vs.hlsl - PVF vertex shader.
// No Input Assembler: all vertex data is fetched manually via SV_VertexID.
// Outputs per-pixel velocity for the TAA motion vector buffer.
//
// The vertex math lives in gbuffer_vertex.hlsli (GBufferVertex), shared with the
// mesh-shader entry SceneMeshlet.ms.hlsl; this entry only turns SV_VertexID into
// the vertex's index.

#include "gbuffer_vertex.hlsli"

PSIn main(uint rawID : SV_VertexID, uint instID : SV_InstanceID)
{
    MeshDescriptor md = MeshDescriptors[meshDescIdx];

    // Resolve index (handles both uint16/uint32 index buffers and non-indexed draws).
    uint vid = FetchIndex(md, rawID);

    // Through a named local, not `return GBufferVertex(...)`: returning the call
    // directly lets the output stores move above the prevWorld branch, which changes
    // the DXIL. Keep this spelling.
    PSIn o = GBufferVertex(md, vid, instID);
    return o;
}
