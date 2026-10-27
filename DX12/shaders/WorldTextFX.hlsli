// WorldTextFX.hlsli -- author interface for CUSTOM WORLD-SPACE text shaders.
//
// The world-space twin of TextFX.hlsli. A WorldUIText / DamageNumber can set
// `customShaderPath` (same path forms; Asset Browser drag & drop works) and its glyph
// quads render with your pixel shader through the bindless table. Write it like:
//
//     #include "WorldTextFX.hlsli"
//     float4 main(WorldTextPSIn i) : SV_TARGET
//     {
//         float dist = WorldTextSdfDist(i);      // R8 SDF, 0.5 == glyph edge
//         float fill = WorldTextSdfFill(dist);
//         UITextEffect fx = WorldTextEffectData(i); // customA/customB params
//         float3 rgb = ...;
//         return float4(rgb, i.color.a * fill);  // .a carries distance fade!
//     }
//
// Facts:
//   * i.texIdx packs [31] = SDF flag, [29..24] = effects slot, [23..0] = bindless page
//     index: use the helpers, do not decode by hand.
//   * i.color is the component tint; its alpha already includes the WorldSpaceUI
//     distance fade: multiply it in or fades break.
//   * g_wuTimeSec animates; fx.runRect is the run's projected SCREEN rect (billboards
//     face the camera): WorldTextRunT(fx, i.pos) gives the 0..1 position across the
//     whole string for run-wide gradients.
//   * The built-in drop shadow (fxShadow) draws BEHIND you with the stock shader;
//     outline / glow apply only when you reproduce them yourself.
//
// Bindings mirror WorldUI.ps.hlsl / WorldUIBillboardPass: do NOT change them.

Texture2D    g_BindlessTex[] : register(t0, space2);
#include "sampler_heap.hlsli"   // samplers from the sampler heap
#define g_UISmp SmpFixed(SMP_LINEAR_CLAMP)   // DebugIcon / WorldUIBillboard only ever bound linear clamp

cbuffer WorldUIDrawConsts : register(b0, space0)
{
    uint g_wuVertexOffset;
    uint g_wuPad0;
    uint g_wuPad1;
    uint g_wuPad2;
};

cbuffer PerViewCB : register(b1, space0)
{
    float4x4 g_viewProj;
    float    g_wuTimeSec;   // UI clock, seconds
    float3   g_wuCbPad;
};

// Mirrors UI::GpuTextEffect (UIDrawList.h, 96 bytes).
struct UITextEffect
{
    float4 outlineColor;
    float4 glowColor;
    float  outlineWidthN;
    float  glowWidthN;
    float  softness;
    float  _pad;
    float4 customA;      // component customParam0
    float4 customB;      // component customParam1
    float4 runRect;      // projected run rect: {originX, originY, 1/w, 1/h} px
};
cbuffer WorldUIEffects : register(b2, space0)
{
    UITextEffect g_fx[64];
};

struct WorldTextPSIn
{
    float4 pos                          : SV_POSITION;
    float2 uv                           : TEXCOORD0;
    nointerpolation uint texIdx         : TEXCOORD1;
    float4 color                        : COLOR0;
};

// ==== Helpers ====

float WorldTextSdfDist(WorldTextPSIn i)
{
    const uint idx = i.texIdx & 0x00FFFFFFu;
    return g_BindlessTex[NonUniformResourceIndex(idx)].Sample(g_UISmp, i.uv).r;
}

float WorldTextSdfFill(float dist)
{
    float aa = max(fwidth(dist), 1e-4);
    return smoothstep(0.5 - aa, 0.5 + aa, dist);
}

float WorldTextSdfFillExpanded(float dist, float bandN)
{
    float aa = max(fwidth(dist), 1e-4);
    return smoothstep(0.5 - bandN - aa, 0.5 - bandN + aa, dist);
}

UITextEffect WorldTextEffectData(WorldTextPSIn i)
{
    return g_fx[(i.texIdx >> 24) & 0x3Fu];
}

// 0..1 position of this pixel across the WHOLE text run (the projected screen rect,
// stable because billboards face the camera). Pass i.pos.
float2 WorldTextRunT(UITextEffect fx, float4 svPos)
{
    return saturate(float2((svPos.x - fx.runRect.x) * fx.runRect.z,
                           (svPos.y - fx.runRect.y) * fx.runRect.w));
}

float3 WorldTextHueToRgb(float h)
{
    float3 p = abs(frac(h + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    return saturate(p - 1.0);
}
