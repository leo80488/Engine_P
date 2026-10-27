// ShadowHook.vs.hlsl - the shadow-pass VS of a masked custom material that has NO custom vertex
// shader: custom_vertex.hlsli with a hook that changes nothing, compiled by the shadow passes with
// CUSTOM_VS_SHADOW + SHADOW_HOOK. It outputs the full MaterialSurfaceVaryings the material's
// SHADOW_HOOK pixel shader reads, projected by the shadow view (a material WITH a custom VS
// compiles its own file the same way); the position is Shadow.vs.hlsl's world transform.
//
// KEEP THIS FILE PURE ASCII.

#include "custom_vertex.hlsli"

void CustomizeVertex(inout VtxContext v)
{
}
