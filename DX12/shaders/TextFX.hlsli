// Author interface for CUSTOM UI TEXT pixel shaders.
// A Canvas UIText's customShaderPath names a .hlsl file relative to shaders/ (e.g.
// "text_fx/TextFX_RainbowWave.ps.hlsl"); UIPass compiles it as a pixel shader (entry main) and uses
// it instead of the built-in SDF shader on that text's glyph quads:
//
//     #include "TextFX.hlsli"
//     float4 main(TextPSIn i) : SV_TARGET
//     {
//         float dist = TextSdfDist(i.uv);        // R8 SDF, 0.5 == glyph edge
//         float fill = TextSdfFill(dist);        // crisp AA coverage
//         UITextEffect fx = TextEffectData();    // customA/customB params
//         float3 rgb = ...;                      // your effect
//         return float4(rgb, i.color.a * fill);
//     }
//
// A custom shader can rely on:
//   * one invocation covers ONE glyph's padded quad; i.uv is its sub-rect of an R8 atlas page
//     (red = signed distance field);
//   * i.color is the UIText tint: multiply its .a in so fades and canvas alpha keep working;
//   * g_uiTimeSec is the UI clock; customA/customB are the component's customParam0/customParam1;
//   * the built-in drop shadow (fxShadow) is drawn BEHIND it with the stock shader; outline/glow
//     widths are in fx.outlineWidthN/glowWidthN (UI.ps.hlsl has the reference bands);
//   * output is straight alpha, composited SrcAlpha/InvSrcAlpha onto the tonemapped LDR target.
// The bindings mirror UI.ps.hlsl / UIPass exactly: do NOT change the registers.

Texture2D    g_UITex : register(t3, space0);   // current font atlas page (R8)
#include "sampler_heap.hlsli"   // samplers from the sampler heap
#define g_UISmp SmpDynamic()   // UIPass's per-draw choice (clamp-linear for SDF text)

cbuffer UIDrawConsts : register(b0, space0)
{
    uint g_materialID;   // 1 for text quads
    uint g_effectIndex;  // this text's slot in g_fx[]
    uint g_uiPad0;
    uint g_uiPad1;
};

cbuffer UICB : register(b1, space0)
{
    float2 g_canvasSizePx; // viewport pixels
    float  g_uiTimeSec;    // UI clock, seconds
    uint   g_uiCbPad;
};

// Mirrors UI::GpuTextEffect (UIDrawList.h, 96 bytes) and UI.ps.hlsl.
struct UITextEffect
{
    float4 outlineColor;
    float4 glowColor;
    float  outlineWidthN;
    float  glowWidthN;
    float  softness;
    float  _pad;
    float4 customA;      // UIText.customParam0
    float4 customB;      // UIText.customParam1
    float4 runRect;      // whole-run rect: {originX, originY, 1/w, 1/h} px
};
cbuffer UIEffects : register(b2, space0)
{
    UITextEffect g_fx[64];
};

struct TextPSIn
{
    float4 pos   : SV_POSITION;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
};

// ==== Helpers ====

// Raw signed-distance sample: 0.5 == glyph edge, >0.5 inside, <0.5 outside.
float TextSdfDist(float2 uv)
{
    return g_UITex.Sample(g_UISmp, uv).r;
}

// Screen-space antialiased coverage of the glyph body.
float TextSdfFill(float dist)
{
    float aa = max(fwidth(dist), 1e-4);
    return smoothstep(0.5 - aa, 0.5 + aa, dist);
}

// Coverage of an EXPANDED body (bandN > 0 grows outward: outlines, glows).
float TextSdfFillExpanded(float dist, float bandN)
{
    float aa = max(fwidth(dist), 1e-4);
    return smoothstep(0.5 - bandN - aa, 0.5 - bandN + aa, dist);
}

// This text's effect-table entry (customA/customB + builtin effect params).
UITextEffect TextEffectData()
{
    return g_fx[min(g_effectIndex, 63u)];
}

// 0..1 position of this pixel across the WHOLE text run: x left -> right over the string's width,
// y top -> bottom over its height (all lines). Font::DrawText fills fx.runRect for custom-shader runs,
// so whole-string gradients need no parameters. Pass i.pos (SV_POSITION) as svPos.
float2 TextRunT(UITextEffect fx, float4 svPos)
{
    return saturate(float2((svPos.x - fx.runRect.x) * fx.runRect.z,
                           (svPos.y - fx.runRect.y) * fx.runRect.w));
}

// Cheap hue -> RGB (for rainbow-style effects).
float3 TextHueToRgb(float h)
{
    float3 p = abs(frac(h + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    return saturate(p - 1.0);
}
