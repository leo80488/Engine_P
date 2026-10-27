// custom_material.hlsli - the single-file material entry. One material source defines optional
// hooks and the engine compiles that same file for both stages:
//
//     #define MATERIAL_HAS_SURFACE          // provides EvaluateMaterial (pixel)
//     #define MATERIAL_HAS_VERTEX_OFFSET    // provides MaterialGetWorldPositionOffset (vertex)
//     #include "custom_material.hlsli"
//
//     // [optional] world-position offset (wind, animation, ...)
//     float3 MaterialGetWorldPositionOffset(MaterialVertexParams p) { return ...; }
//     // [optional, MATERIAL_HAS_VERTEX_MODIFY] edit normal/uv/vertex color before shading
//     void   ModifyVertexAttributes(inout MaterialVertexParams p)   { ... }
//     // [pixel] fill the SurfaceData the engine encodes into the GBuffer
//     void   EvaluateMaterial(MaterialPixelParams p, inout SurfaceData s) { ... }
//
// The engine injects MATERIAL_STAGE_VS / MATERIAL_STAGE_PS per compile. The pixel stage runs
// custom_surface.hlsli's main (calls EvaluateMaterial); the vertex stage runs custom_vertex.hlsli's
// main (calls the vertex hooks through CustomizeVertex below). The hook the running stage does not
// use is unreachable and DXC dead-strips it, so a pixel hook's Sample()/ddx() never breaks the
// vertex compile, and vice versa.
//
// Without MATERIAL_HAS_VERTEX_OFFSET the file is compiled as a pixel shader only (so
// ModifyVertexAttributes needs it too) and the material shares the engine's generic vertex and
// depth/shadow shaders (no per-material VS permutation). Every hook has a weak default: no offset,
// no attribute change, standard PBR (the GBuffer.ps default), so a material writes only what it
// needs.

#ifndef CUSTOM_MATERIAL_HLSLI
#define CUSTOM_MATERIAL_HLSLI

// Default to the pixel stage when the engine didn't inject one (manual dxc / safety).
#if !defined(MATERIAL_STAGE_VS) && !defined(MATERIAL_STAGE_PS)
#define MATERIAL_STAGE_PS 1
#endif

#include "material_bindings.hlsli"   // all engine bindings (declared once)
#include "material_types.hlsli"      // MaterialVertexParams / MaterialPixelParams / SurfaceData

// ==== Vertex hooks: forward declarations + weak defaults ====
float3 MaterialGetWorldPositionOffset(MaterialVertexParams p);
void   ModifyVertexAttributes(inout MaterialVertexParams p);

#ifndef MATERIAL_HAS_VERTEX_OFFSET
float3 MaterialGetWorldPositionOffset(MaterialVertexParams p) { return float3(0.0, 0.0, 0.0); }
#endif
#ifndef MATERIAL_HAS_VERTEX_MODIFY
void   ModifyVertexAttributes(inout MaterialVertexParams p) {}
#endif

#if defined(MATERIAL_STAGE_VS) && MATERIAL_STAGE_VS

// ==== Vertex stage ====
// Reuse the engine vertex template (fetch + TBN + velocity + billboard + CUSTOM_VS_SHADOW).
// Its main() calls CustomizeVertex, which bridges to the material hooks. The user's
// EvaluateMaterial (if present) must still parse here, so material_surface.hlsli supplies its
// types, bindings and helpers; it is unreachable from the VS main and dead-stripped.
#include "material_surface.hlsli"   // helpers + weak EvaluateMaterial (bindings guarded)
#include "custom_vertex.hlsli"      // VtxContext + main(CustomizeVertex) (bindings guarded)

void CustomizeVertex(inout VtxContext v)
{
    MaterialVertexParams p;
    p.worldPos      = v.worldPos;
    p.normalWS      = v.worldNormal;
    p.tangentWS     = v.worldTangent;
    p.uv0           = v.uv;
    p.uv1           = v.uv1;
    p.uv2           = v.uv2;
    p.uv3           = v.uv3;
    p.vertexColor   = v.color;
    p.localPos      = v.localPos;
    p.worldMatrix   = v.worldMatrix;
    p.time          = v.time;
    p.deltaTime     = v.deltaTime;
    p.cameraPos     = v.cameraPos;
    p.vertexID      = v.vertexID;
    p.instanceID    = v.instanceID;
    p.materialIndex = v.materialIndex;

    // Optional attribute edits (normal/uv/color); writes back the writable fields.
    ModifyVertexAttributes(p);
    v.worldNormal  = p.normalWS;
    v.worldTangent = p.tangentWS;
    v.uv           = p.uv0;
    v.uv1          = p.uv1;
    v.uv2          = p.uv2;
    v.uv3          = p.uv3;
    v.color        = p.vertexColor;

    // World-position offset.
    v.worldPos += MaterialGetWorldPositionOffset(p);
}

#else

// ==== Pixel stage ====
// The full pixel surface template (main + EvaluateMaterial weak default + the GBuffer /
// forward variants + the GBuffer encode). The user's MaterialGetWorldPositionOffset (if
// present) parses here against the types above and is dead-stripped.
#include "custom_surface.hlsli"

#endif // stage

// ==== Water service layer: both stages, every permutation ====
// A water material's hook calls WaterSceneDepthBelow() / WaterFresnel() / WaterRippleSlope()
// etc., and so does a standard material graph that uses the SceneDepth / SceneColor / Fresnel
// nodes (the graph emits these helpers whatever its domain). The hook must parse where the real
// implementations (WATER_SURFACE only) do not exist:
//
//   1. every other pixel variant: the deferred GBuffer one (surface materials, and the material
//      preview's deferred route for a water material), the forward / translucent ones, the
//      SHADOW_HOOK depth-only one;
//   2. the vertex stage, when the material also declares MATERIAL_HAS_VERTEX_OFFSET (Gerstner
//      waves) - the VS branch never includes custom_surface.hlsli;
//   3. the CUSTOM_VS_SHADOW depth-only vertex variant.
//
// water_common.hlsli supplies neutral stubs for all of them. Without them that variant fails to
// compile, lands in the negative cache and the material does not draw there, while its Inspector
// rows (read from the declarations) remain: "the rows are there, the picture is not".
// #define MATERIAL_DOMAIN_WATER is recorded and shown, not enforced; the stubs stay because
// surface graphs call the helpers too.
//
// The include guard makes this a no-op where custom_surface.hlsli already included it for the
// real (MS_IS_WATER) path.
#include "water_common.hlsli"

#endif // CUSTOM_MATERIAL_HLSLI
