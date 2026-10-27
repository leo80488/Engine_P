// custom_vertex.hlsli - the custom vertex template of the GBuffer / Transparent passes.
//
// PURPOSE
//   Lets a material customize the vertex stage (world-position offset, UV scroll, vertex-color
//   tweaks, normal bending, ...) without re-implementing the engine's programmable vertex fetch
//   (PVF), tangent basis, TAA velocity and billboard code. A custom pixel shader may replace the
//   whole PS (or hook EvaluateMaterial through custom_surface.hlsli); the vertex side only hooks a
//   small function, like Unreal's "World Position Offset" node and Unity's surface-shader
//   "vertex:" modifier.
//
// HOW TO AUTHOR A CUSTOM VERTEX SHADER
//   Create a `.vs.hlsl` file that includes this header and defines exactly one function,
//   CustomizeVertex, then assign it to the material's "Vertex Shader Path" slot
//   (Inspector -> Use Custom Shader).
//
//       #include "custom_vertex.hlsli"
//
//       void CustomizeVertex(inout VtxContext v)
//       {
//           // World Position Offset (wind sway)
//           v.worldPos.x += sin(v.time + v.worldPos.z) * 0.1;
//           // Scrolling UV
//           v.uv.y       += v.time * 0.2;
//           // v.worldNormal / v.worldTangent / v.uv1 / v.color are writable too.
//       }
//
//   The engine supplies `main` (below): it fetches the vertex, transforms it to world space,
//   fills VtxContext, calls the hook, then finalizes clip-space position + motion vectors. The
//   output PSIn is byte-for-byte what GBuffer.vs.hlsl produces, so any pixel shader (engine
//   default or custom) consumes it unchanged.
//
// OPTIONAL TUNABLES
//   Declare a `cbuffer Foo : register(b8, space0)` and/or unbound `Texture2D`s exactly like a
//   custom pixel shader: the material system reflects them into Inspector sliders / texture
//   slots, shared with the custom PS bag (b8 = the per-material custom CBV; t* space3 = custom
//   textures).
//
// SHADOWS
//   The offset reaches the shadow maps: the depth-only variant (#if CUSTOM_VS_SHADOW) runs the
//   same fetch + hook, so the cast shadow tracks the deformed silhouette in the CSM (sun), spot
//   and point shadow passes. gTimeParams sits at a fixed CB offset (192) in both the GBuffer
//   PerViewCB and the ShadowPerViewCB, so the hook reads identical time in either pass. (The far
//   terrain-only CSM cascade is skipped: dynamic casters are sub-pixel there.)
//
// VELOCITY
//   The motion vector's previous position runs the hook a second time, on last frame's
//   undeformed vertex at last frame's time (gTimeParams.x - .y): a time-animated offset (foliage
//   wind) moves its pixels by exactly what the hook moved them. One extra hook evaluation per
//   vertex in the GBuffer / forward variants; the shadow variant has no velocity.

#ifndef CUSTOM_VERTEX_HLSLI
#define CUSTOM_VERTEX_HLSLI

// All engine bindings (PushConstants, InstanceBuffer, MeshDescriptors, PerViewCB /
// ShadowPerViewCB) come from the shared header so the vertex template, the pixel
// template, and the unified custom_material.hlsli agree on the layout. The shadow
// variant (CUSTOM_VS_SHADOW) swaps PerViewCB for ShadowPerViewCB there too.
#include "material_bindings.hlsli"

// ==== Output structs ====

#if CUSTOM_VS_SHADOW && !SHADOW_HOOK
// Depth-only output for shadow rasterization (uv kept for the alpha-test clip in the PS;
// instanceSlot feeds the per-instance dissolve clip). Must match Shadow.ps.hlsl PSIn exactly.
// The SHADOW_HOOK variant takes the full struct below instead: the material's own shadow PS
// runs its pixel hook and reads every interpolant the GBuffer PS does.
struct CV_Out
{
    float4 sv : SV_POSITION;
    float2 uv : TEXCOORD0;
    nointerpolation uint instanceSlot : INSTSLOT;
};
#else
// GBuffer output - must match GBuffer.vs.hlsl PSIn exactly.
struct CV_Out
{
    float4 sv        : SV_POSITION;
    float3 worldPos  : POSITIONWS;
    float2 uv        : TEXCOORD0;
    float3 wn        : NORMAL;
    float3 wt        : TANGENT;
    float3 wbt       : BINORMAL;
    float3 col       : COLOR;
    float2 uv1       : TEXCOORD3;
    float2 uv2       : TEXCOORD4;
    float2 uv3       : TEXCOORD5;
    float4 curClip   : TEXCOORD1;
    float4 prevClip  : TEXCOORD2;
    nointerpolation uint instanceSlot : INSTSLOT;
};
#endif

// ==== VtxContext: the customization surface ====
// Writable fields are applied to the final vertex; read-only fields give the hook the data it
// needs. Modify the writable fields in CustomizeVertex().

struct VtxContext
{
    // ---- Writable (your hook edits these) ----
    float3 worldPos;      // world-space position - add WPO offsets here
    float3 worldNormal;   // world-space normal
    float3 worldTangent;  // world-space tangent
    float2 uv;            // primary UV set
    float2 uv1;           // second UV set
    float2 uv2;           // additional UV set
    float2 uv3;           // additional UV set
    float3 color;         // per-vertex color RGB

    // ---- Read-only context ----
    float3   localPos;       // mesh-local position (pre-transform)
    float4x4 worldMatrix;    // per-instance world matrix (row-vector convention)
    float    time;           // scene time in seconds
    float    deltaTime;      // seconds since previous frame
    float3   cameraPos;      // camera world position
    uint     vertexID;       // resolved vertex index
    uint     instanceID;     // SV_InstanceID
    uint     materialIndex;  // material slot (index into g_Materials)
};

// User-provided hook, defined in the material's custom VS file after this include. Declared
// here so `main` can call it.
void CustomizeVertex(inout VtxContext v);

// ==== Main (engine-owned) ====
// One entry point for both passes; the fetch + hook are identical, only the output (full GBuffer
// attributes vs. depth-only) and the projection differ.

CV_Out main(uint rawID : SV_VertexID, uint instID : SV_InstanceID)
{
    MeshDescriptor md = MeshDescriptors[meshDescIdx];

    uint vid = FetchIndex(md, rawID);

    float3 localPos = FETCH_POS(md, vid);
    float3 localNrm = FETCH_NORMAL(md, vid);
    float4 localTan = FETCH_TANGENT4(md, vid);
    float2 uv0      = FETCH_UV0(md, vid);
    float2 uv1in    = FETCH_UV1(md, vid);
    float2 uv2in    = FETCH_UV2(md, vid);
    float2 uv3in    = FETCH_UV3(md, vid);
    float4 color    = FETCH_COLOR(md, vid);

    // Resolve the draw-local index to the (possibly remapped) stable slot.
    uint instSlot = ResolveInstanceSlot(gInstanceRemapIdx, instanceOffset + instID);
    float4x4 world = InstanceBuffer[instSlot].world;

    float4 wPos;
    float3 wn, wt, wbt;

#if BILLBOARD
    // Billboard: instance matrix carries pre-computed camera right/up + center.
    float3 center  = float3(world[3][0], world[3][1], world[3][2]);
    float  scaleX  = length(float3(world[0][0], world[0][1], world[0][2]));
    float  scaleY  = length(float3(world[1][0], world[1][1], world[1][2]));
    float3 camRight = normalize(float3(world[0][0], world[0][1], world[0][2]));
    float3 camUp    = normalize(float3(world[1][0], world[1][1], world[1][2]));

    wPos = float4(center + localPos.x * camRight * scaleX + localPos.y * camUp * scaleY, 1.0);
    wn   = normalize(cross(camRight, camUp));
    wt   = camRight;
    wbt  = camUp;
#else
    wPos = mul(float4(localPos, 1.0f), world);
    float3x3 w3 = (float3x3)world;
    wn  = normalize(mul(localNrm, w3));

    [branch]
    if (md.tangent.bufferIndex == INVALID_BUFFER)
    {
        // Synthesise an orthonormal basis from N alone (no per-vertex tangent). This is
        // core/sampling.hlsli's TangentFrameRefY written out on purpose: inline, DXC folds this
        // normalize into a later one (wind_vertex.vs.hlsl compiles to a single normalize after the
        // branch); behind the call it keeps both, an ULP-level output change. Do not replace it with
        // the call.
        float3 ref = (abs(wn.y) > 0.99) ? float3(0, 0, 1) : float3(0, 1, 0);
        wt  = normalize(cross(ref, wn));
        wbt = cross(wn, wt);
    }
    else
    {
        wt  = normalize(mul(localTan.xyz, w3));
        wt  = normalize(wt - dot(wt, wn) * wn);  // Gram-Schmidt
        wbt = cross(wn, wt) * localTan.w;
    }
#endif

    // ---- Populate the customization context + invoke the user hook ----
    VtxContext v;
    v.worldPos      = wPos.xyz;
    v.worldNormal   = wn;
    v.worldTangent  = wt;
    v.uv            = uv0;
    v.uv1           = uv1in;
    v.uv2           = uv2in;
    v.uv3           = uv3in;
    v.color         = color.rgb;
    v.localPos      = localPos;
    v.worldMatrix   = world;
    v.time          = gTimeParams.x;
    v.deltaTime     = gTimeParams.y;
    v.cameraPos     = gCameraPosWS.xyz;
    v.vertexID      = vid;
    v.instanceID    = instID;
    v.materialIndex = materialIndex;
#if !CUSTOM_VS_SHADOW && !BILLBOARD
    // The hook's input, kept for the previous-frame evaluation (velocity below).
    const VtxContext vIn = v;
#endif

    CustomizeVertex(v);

    // GPU-TDR safety net for both the depth/shadow and GBuffer paths below, which derive
    // SV_Position from v.worldPos. A non-finite or huge world position (a stale / not-yet-resident
    // bindless mesh descriptor of a streaming mesh, a degenerate skin, a misbehaving WPO hook)
    // projects to screen-spanning triangles that rasterize billions of fragments and exceed
    // Windows' ~2 s GPU timeout -> DXGI_ERROR_DEVICE_HUNG (a pure timeout with a valid draw count and
    // no page fault: the bad input is vertex data). Collapse a bad vertex to the instance origin so
    // the triangle degenerates (zero area -> culled). Valid frames are unaffected.
    // `!(d < T)` is true for NaN, inf and |pos| > 1e5, and unlike isnan/isinf fast-math cannot
    // strip it.
    if (!(dot(v.worldPos, v.worldPos) < 1e10f))
        v.worldPos = world._m30_m31_m32;   // instance world-space translation (row-vector M)

#if CUSTOM_VS_SHADOW && !SHADOW_HOOK
    // ---- Depth-only shadow output ----
    // Same deformed world position as the GBuffer (identical fetch + hook), so the cast shadow
    // matches the lit silhouette. No TBN / velocity needed.
    CV_Out so;
    so.sv = mul(float4(v.worldPos, 1.0f), shadowViewProj);
    so.uv = v.uv;   // alpha-test PS clips on this
    so.instanceSlot = instSlot;   // remapped - the PS dissolve clip reads it
    return so;
#else

    // Re-derive a stable TBN from the (possibly edited) normal/tangent. Falls
    // back to the unedited basis when the hook collapses a vector to zero.
    float3 fwn = (dot(v.worldNormal,  v.worldNormal)  > 1e-12) ? normalize(v.worldNormal)  : wn;
    float3 fwt = (dot(v.worldTangent, v.worldTangent) > 1e-12) ? normalize(v.worldTangent) : wt;
    fwt = fwt - dot(fwt, fwn) * fwn;
    fwt = (dot(fwt, fwt) > 1e-12) ? normalize(fwt) : wt;
#if BILLBOARD
    float handed = 1.0f;   // synthetic camera-facing frame, not a mesh tangent
#else
    // Mirrored instances flip the tangent frame's handedness (see
    // InstanceHandednessSign). Only the authored-tangent branch carries it: the
    // INVALID_BUFFER fallback is an arbitrary frame with no UV relationship.
    float handed = (md.tangent.bufferIndex == INVALID_BUFFER)
                   ? 1.0f
                   : localTan.w * InstanceHandednessSign(world);
#endif
    float3 fwbt = cross(fwn, fwt) * handed;

    float4 wPosCustom = float4(v.worldPos, 1.0f);

    CV_Out o;
#if CUSTOM_VS_SHADOW
    // SHADOW_HOOK: the full interpolants, in the shadow view.
    o.sv       = mul(wPosCustom, shadowViewProj);
#else
    o.sv       = mul(wPosCustom, viewProj);
#endif
    o.worldPos = v.worldPos;
    o.uv       = v.uv;
    o.wn       = fwn;
    o.wt       = fwt;
    o.wbt      = fwbt;
    o.col      = v.color;
    o.uv1      = v.uv1;
    o.uv2      = v.uv2;
    o.uv3      = v.uv3;
    o.instanceSlot = instSlot;   // remapped - the PS reads InstanceBuffer[instanceSlot]

#if CUSTOM_VS_SHADOW
    // A shadow map has no velocity: the pair stays in the signature (the PS
    // declares the GBuffer's struct) and is never read.
    o.curClip  = o.sv;
    o.prevClip = o.sv;
    return o;
#else
    // Velocity: current customized clip-pos (unjittered) minus previous.
    o.curClip = mul(wPosCustom, curViewProjNoJitter);

#if BILLBOARD
    o.prevClip = mul(wPosCustom, prevViewProj);
#else
    float4x4 prevWorld = InstanceBuffer[instSlot].prevWorld;
    float3 prevLocalPos = localPos;
    if (md.prevPosition.bufferIndex != 0xFFFFFFFFu)
    {
        // Last frame's buffer, not this frame's at another base: md.prevPosition names the previous
        // vertex-ring slot, which still holds last frame's skinned output; it is patched beside
        // md.position every frame.
        //
        // Gated on bufferIndex, not on the prevPosInfo root constant: OutlinePass (picking flag /
        // outline width) and FaceOverlayPass alias that DWORD while they run this VS, so it is neither
        // a reliable base nor a reliable sentinel.
        prevLocalPos = FetchAsFloat3(
            md.prevPosition.bufferIndex,
            md.prevPosition.byteOffset + vid * 12u,
            md.prevPosition.format);
    }
    float4 prevWPos = mul(float4(prevLocalPos, 1.0f), prevWorld);
    // The previous frame's deformed position: the hook a second time, on last frame's undeformed
    // vertex at last frame's time (UE5's vertex-deformation velocity; Grass.ms.hlsl evaluates its
    // wind at g_prevTime the same way). Exact for a static offset too. Do not apply this frame's
    // offset to the previous position instead: every time-animated offset (foliage wind) would get
    // zero motion and flicker under DLSS / TAA.
    VtxContext pv  = vIn;
    pv.worldPos    = prevWPos.xyz;
    pv.localPos    = prevLocalPos;
    pv.worldMatrix = prevWorld;
    pv.time        = gTimeParams.x - gTimeParams.y;
    CustomizeVertex(pv);
    prevWPos.xyz = pv.worldPos;
    // Same guard for the prev-frame fetch, so a garbage stale buffer cannot emit runaway motion
    // vectors (TAA): fall back to zero velocity at this vertex.
    if (!(dot(prevWPos.xyz, prevWPos.xyz) < 1e10f))
        prevWPos = float4(v.worldPos, 1.0f);
    o.prevClip = mul(prevWPos, prevViewProj);
#endif // BILLBOARD

    return o;
#endif // CUSTOM_VS_SHADOW (the SHADOW_HOOK output)
#endif // CUSTOM_VS_SHADOW && !SHADOW_HOOK
}

#endif // CUSTOM_VERTEX_HLSLI
