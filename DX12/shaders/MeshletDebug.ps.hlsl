// The Meshlets view mode's GBuffer pixel shader. While the view mode is on,
// GBufferPass draws every opaque packet with this PS and LightingPass shows the raw
// albedo (the BaseColor view): a packet the mesh pipeline drew shows the colour its
// MS wrote into COLOR (a hash per cluster, or a colour per LOD level -
// MeshletCullCB.debugMode, MeshletDebugColor); a packet GBuffer.vs drew shows flat
// grey. Which one drew it is the per-draw flag in the 4th root constant
// (kMeshletDrawMeshlet), set by GBufferPass for this view only.
//
// Writes all six GBuffer targets so nothing stale survives: an unlit, material-0
// surface with a real motion vector (TAA keeps working).
//
// Keep this file pure ASCII (docs: build-and-dxc-gotchas).

#include "gbuffer_vertex.hlsli"
#include "meshlet_shared.hlsli"

struct MeshletDebugOut
{
    float4 albedo   : SV_TARGET0;
    float4 normal   : SV_TARGET1;
    float4 surface  : SV_TARGET2;
    float2 velocity : SV_TARGET3;
    float4 extra    : SV_TARGET4;
    float4 sceneCol : SV_TARGET5;
};

MeshletDebugOut main(PSIn i)
{
    // A meshlet draw's flags have bit 31; a VS draw carries 0 (CPU path) or its
    // pristine prevPosInfo (indirect path: 0xFFFFFFFF, or a skinned base index).
    const bool meshlet = (prevPosInfo & kMeshletDrawMeshlet) != 0 && prevPosInfo != 0xFFFFFFFFu;
    MeshletDebugOut o;
    o.albedo   = float4(meshlet ? i.col : float3(0.35, 0.35, 0.35), 1.0);
    o.normal   = float4(normalize(i.wn) * 0.5 + 0.5, 0.0);
    o.surface  = float4(1.0, 0.0, 1.0, 0.5);
    o.velocity = i.curClip.xy / i.curClip.w - i.prevClip.xy / i.prevClip.w;
    o.extra    = float4(0.0, 0.0, 0.0, 0.0);
    o.sceneCol = float4(0.0, 0.0, 0.0, 0.0);
    return o;
}
