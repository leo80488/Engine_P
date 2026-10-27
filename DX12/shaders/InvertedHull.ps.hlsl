// Pixel shader of the inverted-hull outline pass. Alpha fades with distance;
// beyond fadeEnd the VS already clips the triangle. Also runs the per-instance
// dissolve clip so a dissolving mesh loses its rim outline in sync with the GBuffer
// holes (sampler-free noise fetch).

#include "gpu_instance.hlsli"
#include "dissolve.hlsli"

cbuffer OutlineCB : register(b2, space0)
{
    float    outlinePixels;
    float    depthThreshold;
    float    normalThreshold;
    float    outlineStrength;
    float3   outlineColor;
    float    outlineFadeStart;
    uint     outlinePxSpaceW;   // DISPLAY extents (unused here; keeps the b2 layout)
    uint     outlinePxSpaceH;
    float    outlineFadeEnd;
    float    nearZ;
    float    farZ;
};

// PerViewCB: only the dissolve noise bindless index (offset 224) is read here; the
// members before it are placeholders that keep its offset. Stages may declare the
// same CBV with different member names.
cbuffer PerViewCB : register(b1, space0)
{
    float4x4 _ohViewProj;             // 0
    float4x4 _ohPrevViewProj;         // 64
    float4x4 _ohCurViewProjNoJitter;  // 128
    float4   _ohTimeParams;           // 192
    float4   _ohCameraPosWS;          // 208
    uint     gDissolveNoiseIdx;       // 224 (~0u = none)
    uint3    _ohDissolvePad;          // 228..239
};

// Per-instance dissolve params (the root SRV has visibility ALL; OutlinePass
// already binds it for the hull VS).
StructuredBuffer<GPUInstanceData> InstanceBuffer : register(t0, space0);
Texture2D g_AllTextures[] : register(t0, space2);

// Must match InvertedHull.vs.hlsl VSOut.
struct PSIn
{
    float4 sv       : SV_POSITION;
    float  distFade : TEXCOORD0;
    float2 uv       : TEXCOORD1;
    nointerpolation uint instanceSlot : INSTSLOT;
};

float4 main(PSIn i) : SV_TARGET
{
    // Per-instance dissolve: the rim must not outline dissolved-away pixels.
    const float4 dparams = InstanceBuffer[i.instanceSlot].dissolveParams;
    [branch]
    if (dparams.x > 0.0 && gDissolveNoiseIdx != 0xFFFFFFFFu)
        DissolveClipLoad(g_AllTextures[gDissolveNoiseIdx], dparams, i.uv);

    clip(i.distFade - 0.001);
    return float4(outlineColor, i.distFade);
}
