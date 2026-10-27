// gbuffer_vertex.hlsli - the GBuffer vertex function, shared by its two entry
// points:
//
//   GBuffer.vs.hlsl         one invocation per index (SV_VertexID -> FetchIndex)
//   SceneMeshlet.ms.hlsl    one invocation per meshlet vertex (the vertex ref is
//                           the fetched index: an absolute library VB index)
//
// Both call the same GBufferVertex() with the same (md, vid, instID), so the two
// paths compute the same vertex bit for bit. GBuffer.vs's DXIL is pinned
// (tests/baselines/dxil_manifest.json "GBuffer.vs:*", tools/check_dxil_identity.ps1).
// Do not add `precise`: it brings the paths no closer and changes GBuffer.vs's
// image. Edit the function, not a copy: the depth prepass relies on the VS and the
// MS producing the same depth (GBufferPass.cpp, BuildDepthPrepassPSODesc).
//
// The bindings below are the PVF root signature's (GraphicsDX12.cpp
// CreateDefaultRootSignature); params 9 (t1) and 14 (t0 space1) are
// VISIBILITY_ALL so a mesh shader sees them too.
//
// Keep this file pure ASCII (build-and-dxc-gotchas).

#ifndef GBUFFER_VERTEX_HLSLI
#define GBUFFER_VERTEX_HLSLI

#include "pvf_fetch.hlsli"
#include "core/sampling.hlsli"   // TangentFrameRefY

// ==== Root parameter bindings (must match the PVF root signature) ====

// [0] ROOT_CONSTANTS  b0 space0
cbuffer PushConstants : register(b0, space0)
{
    uint meshDescIdx;
    uint instanceOffset;
    uint materialIndex;
    // Not read for velocity (the prev position comes from md.prevPosition); still set
    // by most passes and aliased by OutlinePass/FaceOverlayPass.
    uint prevPosInfo;
};

// [8] ROOT_SRV  t0 space0
#include "gpu_instance.hlsli"
StructuredBuffer<GPUInstanceData> InstanceBuffer  : register(t0, space0);

// [9] ROOT_SRV  t1 space0
StructuredBuffer<MeshDescriptor> MeshDescriptors : register(t1, space0);

// [1] ROOT_CBV  b1 space0 - prefix view of the shared 256B PerView slot; the
// tail rows (fixed offsets 192/208/224, see material_bindings.hlsli) are
// declared so gInstanceRemapIdx lands at byte 228 in every b1 layout variant.
cbuffer PerViewCB : register(b1, space0)
{
    float4x4 viewProj;             // jittered - SV_POSITION only
    float4x4 prevViewProj;         // previous frame, unjittered (velocity)
    float4x4 curViewProjNoJitter;  // current frame, unjittered (velocity numerator)
    float4   gTimeParams;          // 192 (unused here)
    float4   gCameraPosWS;         // 208 (unused here)
    uint     gDissolveNoiseIdx;    // 224 (unused here)
    uint     gInstanceRemapIdx;    // 228 stable-slot remap heap index (0 = identity)
    uint2    _gPerViewPad;         // 232..239
};

// ==== Output struct ====

struct PSIn
{
    float4 sv        : SV_POSITION;
    float3 worldPos  : POSITIONWS;  // world-space position (dedicated semantic)
    float2 uv        : TEXCOORD0;   // texture coordinates
    float3 wn        : NORMAL;
    float3 wt        : TANGENT;     // world-space tangent
    float3 wbt       : BINORMAL;    // world-space bitangent (cross(N,T) * T.w * handedness sign)
    float3 col       : COLOR;       // per-vertex color (rgb; white when absent)
    float2 uv1       : TEXCOORD3;   // second UV set (0,0 when absent)
    float2 uv2       : TEXCOORD4;   // additional UV set (0,0 when absent)
    float2 uv3       : TEXCOORD5;   // additional UV set (0,0 when absent)
    float4 curClip   : TEXCOORD1;   // current clip-space position (for velocity)
    float4 prevClip  : TEXCOORD2;   // previous clip-space position (for velocity)
    // Appended last: PSes consuming a prefix subset (ProbeCapture, OutlineObjectID)
    // stay signature-compatible. The material PS reads per-instance dissolve
    // params via InstanceBuffer[instanceSlot] (a PS has no SV_InstanceID).
    nointerpolation uint instanceSlot : INSTSLOT;
};

// ==== The vertex function ====
// md     : MeshDescriptors[meshDescIdx]
// vid    : the vertex's index in md's streams (the fetched index, not SV_VertexID)
// instID : the draw-local instance (SV_InstanceID; the mesh shader passes the
//          dispatch's instance row), resolved below to the stable instance slot.
PSIn GBufferVertex(MeshDescriptor md, uint vid, uint instID)
{
    // Fetch per-vertex attributes.
    float3 localPos = FETCH_POS(md, vid);
    float3 localNrm = FETCH_NORMAL(md, vid);
    float4 localTan = FETCH_TANGENT4(md, vid);  // xyz = tangent, w = handedness
    float2 uv0      = FETCH_UV0(md, vid);
    float2 uv1      = FETCH_UV1(md, vid);   // second UV set (0,0 when absent)
    float2 uv2      = FETCH_UV2(md, vid);   // additional UV set (0,0 when absent)
    float2 uv3      = FETCH_UV3(md, vid);   // additional UV set (0,0 when absent)
    float4 color    = FETCH_COLOR(md, vid); // per-vertex color (white when absent)

    // Resolve the draw-local index to the (possibly remapped) stable slot, then
    // fetch the per-instance world matrix (transposed on CPU).
    uint instSlot = ResolveInstanceSlot(gInstanceRemapIdx, instanceOffset + instID);
    float4x4 world = InstanceBuffer[instSlot].world;

    // Transform position and TBN to world space.
    float4 wPos;
    float3 wn, wt, wbt;

#if BILLBOARD
    // Billboard: strip rotation from the world matrix and face the camera.
    // World matrix row 3 = translation (row-vector convention).
    float3 center = float3(world[3][0], world[3][1], world[3][2]);

    // Extract scale from world matrix (length of each basis row).
    float scaleX = length(float3(world[0][0], world[0][1], world[0][2]));
    float scaleY = length(float3(world[1][0], world[1][1], world[1][2]));

    // The Renderer stores the billboard orientation in the instance matrix:
    // row 0 = right * scaleX, row 1 = up * scaleY, row 3 = position (computed on the
    // CPU: the camera axes cannot be recovered from viewProj here without an inverse).
    float3 camRight = normalize(float3(world[0][0], world[0][1], world[0][2]));
    float3 camUp    = normalize(float3(world[1][0], world[1][1], world[1][2]));

    wPos = float4(center + localPos.x * camRight * scaleX + localPos.y * camUp * scaleY, 1.0);
    wn   = normalize(cross(camRight, camUp));  // face normal = toward camera
    wt   = camRight;
    wbt  = camUp;
#else
    wPos = mul(float4(localPos, 1.0f), world);
    float3x3 w3 = (float3x3)world;

    wn  = normalize(mul(localNrm, w3));

    // Robust TBN. Without a tangent stream FETCH_TANGENT4 returns the (1,0,0,1)
    // fallback; for a normal of exactly +/-X, Gram-Schmidt collapses it to zero and
    // normalize() yields NaN, which interpolation spreads over the whole triangle
    // (NaN GBuffer normals -> black lighting on +/-X-aligned walls). So synthesise the
    // basis from N then. The branch is uniform per draw (descriptor-driven); authored
    // tangents win when the stream exists.
    [branch]
    if (md.tangent.bufferIndex == INVALID_BUFFER)
    {
        // Orthonormal basis from N alone; the |wn.y| > 0.99 guard switches the reference
        // axis for floor/ceiling normals.
        TangentFrameRefY(wn, wt, wbt);
    }
    else
    {
        wt = normalize(mul(localTan.xyz, w3));
        // Gram-Schmidt against N - safe because authored tangents are never parallel
        // to their vertex normal.
        wt  = normalize(wt - dot(wt, wn) * wn);
        // Mirrored instances flip the frame's handedness (InstanceHandednessSign). The
        // synthesised branch needs no sign: its frame has no authored UV relationship.
        wbt = cross(wn, wt) * localTan.w * InstanceHandednessSign(world);
    }
#endif

    // GPU-TDR safety net: a non-finite or huge world position (a stale or
    // not-yet-resident bindless mesh descriptor, a degenerate skin) projects to a
    // screen-spanning triangle whose fragments can exceed the ~2 s GPU timeout
    // (DXGI_ERROR_DEVICE_HUNG with a valid draw count: the bad input is vertex data).
    // Collapse such a vertex to the instance origin so the triangle has zero area.
    // `!(d < T)` is true for NaN, inf and any |pos| > 1e5, and unlike isnan/isinf
    // fast-math cannot strip it; 1e5 units is far beyond any scene coordinate.
    if (!(dot(wPos.xyz, wPos.xyz) < 1e10f))
        wPos = float4(world[3][0], world[3][1], world[3][2], 1.0f);  // instance origin

    PSIn o;
    o.sv       = mul(wPos, viewProj);
    o.worldPos = wPos.xyz;
    o.uv       = uv0;
    o.wn       = wn;
    o.wt       = wt;
    o.wbt      = wbt;
    o.col      = color.rgb;
    o.uv1      = uv1;
    o.uv2      = uv2;
    o.uv3      = uv3;
    o.instanceSlot = instSlot;   // remapped - PS reads InstanceBuffer[instanceSlot]

    // Velocity: current and previous clip positions, both unjittered. o.sv is
    // jittered for the sub-pixel sample pattern; the motion vector must exclude the
    // jitter or TAA sees full-pixel motion on a static camera.
    o.curClip = mul(wPos, curViewProjNoJitter);

    // Prev-frame clip position: both paths use InstanceBuffer[].prevWorld so the
    // entity's motion between frames counts; without it TAA reprojects moving
    // entities (and mesh-local skinned output) to the wrong history pixel, a smear
    // visible against post-TAA overlays such as the outline.
#if BILLBOARD
    // Billboard: a prev wPos would need the previous camera axes, which are not
    // tracked; the current wPos x prevViewProj gives camera-motion velocity only.
    o.prevClip = mul(wPos, prevViewProj);
#else
    float4x4 prevWorld = InstanceBuffer[instSlot].prevWorld;
    if (md.prevPosition.bufferIndex != 0xFFFFFFFFu)
    {
        // Skinned: last frame's skinned position in mesh-local space. md.prevPosition
        // names the previous vertex-ring slot, which still holds last frame's output; it
        // is patched beside md.position every frame.
        // Gate on bufferIndex, not on the prevPosInfo root constant: OutlinePass (picking
        // flag / outline width) and FaceOverlayPass alias that DWORD when they run this VS.
        float3 prevLocalPos = FetchAsFloat3(
            md.prevPosition.bufferIndex,
            md.prevPosition.byteOffset + vid * 12u,
            md.prevPosition.format);
        float4 prevWPos = mul(float4(prevLocalPos, 1.0f), prevWorld);
        o.prevClip = mul(prevWPos, prevViewProj);
    }
    else
    {
        // Static: rest-pose local position x prev world x prev VP.
        float4 prevWPos = mul(float4(localPos, 1.0f), prevWorld);
        o.prevClip = mul(prevWPos, prevViewProj);
    }
#endif

    return o;
}

#endif // GBUFFER_VERTEX_HLSLI
