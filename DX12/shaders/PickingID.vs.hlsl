#include "pvf_fetch.hlsli"

cbuffer PushConstants : register(b0, space0) { uint meshDescIdx; uint instanceOffset; uint pad0; };
#include "gpu_instance.hlsli"
StructuredBuffer<GPUInstanceData> InstanceBuffer  : register(t0, space0);
StructuredBuffer<MeshDescriptor> MeshDescriptors : register(t1, space0);
// PickingPass binds the shared "PerView" buffer; the tail rows are declared so
// gInstanceRemapIdx lands at its fixed byte offset 228.
cbuffer PerViewCB : register(b1, space0)
{
    float4x4 viewProj;
    float4x4 _prevViewProj;         //  64 (unused here)
    float4x4 _curViewProjNoJitter;  // 128 (unused here)
    float4   _gTimeParams;          // 192 (unused here)
    float4   _gCameraPosWS;         // 208 (unused here)
    uint     _gDissolveNoiseIdx;    // 224 (unused here)
    uint     gInstanceRemapIdx;     // 228 stable-slot remap heap index (0 = identity)
    uint2    _gPerViewPad;          // 232..239
};

struct PSIn
{
    float4               sv           : SV_Position;
    nointerpolation uint instanceSlot : TEXCOORD0;
};

PSIn main(uint rawID : SV_VertexID, uint instID : SV_InstanceID)
{
    MeshDescriptor md = MeshDescriptors[meshDescIdx];
    uint vid          = FetchIndex(md, rawID);
    float3 localPos   = FETCH_POS(md, vid);

    // Forward the remapped (stable) slot: the picking readback maps it through the persistent
    // m_instanceSlotToEntity table.
    uint instSlot   = ResolveInstanceSlot(gInstanceRemapIdx, instanceOffset + instID);
    float4x4 world  = InstanceBuffer[instSlot].world;
    float4 worldPos = mul(float4(localPos, 1.0f), world);

    // GPU-TDR safety net (see GBuffer.vs.hlsl): collapse a non-finite / absurd
    // vertex to the instance origin so picking can't rasterize a giant triangle.
    if (!(dot(worldPos.xyz, worldPos.xyz) < 1e10f))  // NaN/inf/|pos|>1e5, fast-math-proof
        worldPos = float4(world[3][0], world[3][1], world[3][2], 1.0f);

    PSIn o;
    o.sv           = mul(worldPos, viewProj);
    o.instanceSlot = instSlot;
    return o;
}
