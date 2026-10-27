// Renders the object ID mask of the outline system: each outlined entity writes
// meshDescIdx + 1 into an R32_UINT target, and the screen-space composite pass
// draws outlines along the edges between different IDs.
//
// Runs the per-instance dissolve clip so no edges are drawn around dissolved-away
// regions (the ID mask gets the GBuffer's holes). Sampler-free noise fetch: this
// pass binds no s0.

#include "gpu_instance.hlsli"
#include "dissolve.hlsli"

cbuffer PushConstants : register(b0, space0)
{
    uint meshDescIdx;
    uint instanceOffset;
    uint materialIndex;
    uint pickingFlag;   // non-zero -> OR bit 31 into the output so sub-pass 3 picks the selection color
};

// PerViewCB: only the dissolve noise bindless index (offset 224) is read here; the
// members before it are placeholders that keep its offset. Stages may declare the
// same CBV with different member names.
cbuffer PerViewCB : register(b1, space0)
{
    float4x4 _oidViewProj;             // 0
    float4x4 _oidPrevViewProj;         // 64
    float4x4 _oidCurViewProjNoJitter;  // 128
    float4   _oidTimeParams;           // 192
    float4   _oidCameraPosWS;          // 208
    uint     gDissolveNoiseIdx;        // 224 (~0u = none)
    uint3    _oidDissolvePad;          // 228..239
};

StructuredBuffer<GPUInstanceData> InstanceBuffer : register(t0, space0);
Texture2D g_AllTextures[] : register(t0, space2);

// Input comes from GBuffer.vs.hlsl. Declare the full PSIn in the same order:
// D3D12 linkage matches semantics by hardware register, so skipping elements before
// instanceSlot would shift its register and fail PSO creation (a trailing-prefix
// subset is fine; a gap is not). DXC strips unread fields but keeps their
// signature slots.
struct PSIn
{
    float4 sv       : SV_POSITION;
    float3 worldPos : POSITIONWS;
    float2 uv       : TEXCOORD0;
    float3 wn       : NORMAL;
    float3 wt       : TANGENT;
    float3 wbt      : BINORMAL;
    float3 col      : COLOR;
    float2 uv1      : TEXCOORD3;
    float2 uv2      : TEXCOORD4;
    float2 uv3      : TEXCOORD5;
    float4 curClip  : TEXCOORD1;
    float4 prevClip : TEXCOORD2;
    nointerpolation uint instanceSlot : INSTSLOT;
};

uint main(PSIn i) : SV_TARGET
{
    // Per-instance dissolve: punch the same holes the GBuffer has.
    const float4 dparams = InstanceBuffer[i.instanceSlot].dissolveParams;
    [branch]
    if (dparams.x > 0.0 && gDissolveNoiseIdx != 0xFFFFFFFFu)
        DissolveClipLoad(g_AllTextures[gDissolveNoiseIdx], dparams, i.uv);

    uint id = meshDescIdx + 1u;
    if (pickingFlag != 0u) id |= 0x80000000u;
    return id;
}
