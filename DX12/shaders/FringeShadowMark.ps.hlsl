// FringeShadowMark.ps.hlsl - bangs -> face screen-space shadow-mark PS.
//
// Writes the "hair shadow here" flag into the GBuffer extra target (RT4 /
// gExtraGBuffer, t17 in Lighting.ps). The PSO restricts the write to the GREEN
// channel (render_target_write_mask = ENABLE_GREEN), so the face's own RT4 payload
// (extra.r / .b = the SDF mirror halves, extra.a = face marker) survives - the
// DanbaidongRP equivalent is its ColorMask R "avoid fighting with hair".
//
// The stencil test (EQUAL, ref 2 = NPR, read-only) restricts the mark to NPR
// character pixels, like the reference's SHADINGMODELS_CHARACTER stencil gate:
// PBR pixels (whose shading models may repurpose RT4, e.g. SSS thickness) are
// never touched.
//
// Consumed by Lighting.ps's toon face branch: extra.g > 0.5 -> the bangs shadow
// term goes to 0 and min()s into the SDF face terminator (ramp shadow color).
//
// Runs the per-instance dissolve clip, so dissolving bangs stop stamping face marks
// in sync with their GBuffer holes (sampler-free noise fetch).

#include "gpu_instance.hlsli"
#include "dissolve.hlsli"

cbuffer PushConstants : register(b0, space0)
{
    uint meshDescIdx;
    uint instanceOffset;
    uint materialIndex;
};

// The buffer the VS binds at b1 (this stage declares the tail it needs).
cbuffer FringeMarkCB : register(b1, space0)
{
    float4x4 _fmViewProj;         // 0
    float4   _fmRow0;             // 64  (camRight, offsetX)
    float4   _fmRow1;             // 80  (camUp, offsetY)
    float4   _fmRow2;             // 96  (camPos, depthPush)
    float3   _fmCamFwd;           // 112
    uint     dissolveNoiseIdx;    // 124 (~0u = none)
};

StructuredBuffer<GPUInstanceData> InstanceBuffer : register(t0, space0);
Texture2D g_AllTextures[] : register(t0, space2);

// MUST match FringeShadowMark.vs.hlsl VSOut.
struct PSIn
{
    float4 sv : SV_POSITION;
    float2 uv : TEXCOORD0;
    nointerpolation uint instanceSlot : INSTSLOT;
};

float4 main(PSIn i) : SV_Target0
{
    const float4 dparams = InstanceBuffer[i.instanceSlot].dissolveParams;
    [branch]
    if (dparams.x > 0.0 && dissolveNoiseIdx != 0xFFFFFFFFu)
        DissolveClipLoad(g_AllTextures[dissolveNoiseIdx], dparams, i.uv);

    return float4(0.0, 1.0, 0.0, 0.0);   // G = hair-shadow mark (write-masked to G)
}
