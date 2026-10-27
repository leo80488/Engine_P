// Shadow.vs.hlsl - depth-only vertex shader for shadow map rendering.
// Fetches positions through PVF (the GBuffer.vs.hlsl ByteAddressBuffer layout) and projects them
// with the shadow view-projection at b1 space0: PerViewCB's root slot in GBuffer.vs.hlsl, with a
// buffer ShadowPass binds per cascade.

// Bindings, VSOut and the per-vertex function live in shadow_vertex.hlsli, shared with
// ShadowMeshlet.ms.hlsl.
#include "shadow_vertex.hlsli"

VSOut main(uint rawID : SV_VertexID, uint instID : SV_InstanceID)
{
    uint instSlot = ResolveInstanceSlot(gInstanceRemapIdx, instanceOffset + instID);
#ifdef SPOT_MIXED_SHADOW
    bool dynamic = IsDynamicShadowInstance(InstanceBuffer[instSlot]);
    if ((_svShadowFilter == 1 && dynamic) || (_svShadowFilter == 2 && !dynamic))
    {
        VSOut excluded = (VSOut)0;
        // Collapse every vertex outside the XY clip range: no depth or PS work, even with depth clipping
        // disabled. Cache hits already remove static instances in InstanceCull; this branch partitions
        // the full args on rebuild frames.
        excluded.sv = float4(2, 2, 0, 1);
        excluded.instanceSlot = instSlot;
        return excluded;
    }
#endif
    MeshDescriptor md = MeshDescriptors[meshDescIdx];
    uint vid = FetchIndex(md, rawID);
    VSOut o = ShadowVertex(md, vid, instSlot);
    return o;
}
