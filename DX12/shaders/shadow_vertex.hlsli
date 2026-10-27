// The shadow-map vertex: its bindings and one function, shared by Shadow.vs.hlsl (every
// shadow family's depth VS: CSM, spot, point) and ShadowMeshlet.ms.hlsl (the CSM meshlet
// path) so the two cannot drift. tests/baselines/dxil_manifest.json pins Shadow.vs's DXIL.
//
// Keep this file pure ASCII: the offline SDK dxc reads the system code page.
#ifndef SHADOW_VERTEX_HLSLI
#define SHADOW_VERTEX_HLSLI

#include "pvf_fetch.hlsli"

cbuffer PushConstants : register(b0, space0)
{
    uint meshDescIdx;
    uint instanceOffset;
    uint materialIndex;
};

#include "gpu_instance.hlsli"
StructuredBuffer<GPUInstanceData> InstanceBuffer  : register(t0, space0);
StructuredBuffer<MeshDescriptor> MeshDescriptors : register(t1, space0);

// Prefix view of the 256 B per-cascade / caster / face CB slot. The tail rows are declared
// so gInstanceRemapIdx lands at its fixed byte offset 228 (as in material_bindings.hlsli's
// ShadowPerViewCB; every shadow CB writer stores the remap heap index there).
cbuffer ShadowPerViewCB : register(b1, space0)
{
    float4x4 shadowViewProj;
    float4   _svPad[8];            //  64..191 (CSM frustum planes; unused here)
    float4   _svTimeParams;        // 192 (custom-VS only; unused here)
    float4   _svCameraPosWS;       // 208 (custom-VS only; unused here)
    uint     _svDissolveNoiseIdx;  // 224 (Shadow.ps reads this; unused here)
    uint     gInstanceRemapIdx;    // 228 stable-slot remap heap index (0 = identity)
    uint     _svShadowFilter;      // 232: spot-only, 0=all, 1=static, 2=dynamic
    uint     _svPad2;              // 236
};

// Must match Shadow.ps.hlsl's PSIn (and the CUSTOM_VS_SHADOW CV_Out in
// custom_vertex.hlsli): instanceSlot feeds the per-instance dissolve clip.
struct VSOut
{
    float4 sv : SV_POSITION;
    float2 uv : TEXCOORD0;
    nointerpolation uint instanceSlot : INSTSLOT;
};

// One shadow vertex: library vertex @p vid of mesh @p md, through instance
// @p instSlot (already remapped), into this view's shadow clip space.
VSOut ShadowVertex(MeshDescriptor md, uint vid, uint instSlot)
{
    float3   localPos = FETCH_POS(md, vid);
    float2   uv0      = FETCH_UV0(md, vid);
    float4x4 world    = InstanceBuffer[instSlot].world;
    float4   wPos     = mul(float4(localPos, 1.0f), world);

    // GPU-TDR safety net (see GBuffer.vs.hlsl): collapse a non-finite or absurd vertex (stale
    // streaming mesh descriptor, degenerate skin) to the instance origin, so a shadow draw
    // cannot rasterize a screen-spanning triangle and hang the GPU.
    if (!(dot(wPos.xyz, wPos.xyz) < 1e10f))  // NaN/inf/|pos|>1e5 - fast-math-proof
        wPos = float4(world[3][0], world[3][1], world[3][2], 1.0f);

    VSOut o;
    o.sv = mul(wPos, shadowViewProj);
    o.uv = uv0;
    o.instanceSlot = instSlot;   // remapped - Shadow.ps dissolve clip reads it
    return o;
}

#endif // SHADOW_VERTEX_HLSLI
