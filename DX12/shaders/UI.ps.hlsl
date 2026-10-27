// UI pixel shader. Two materials, picked by the b0 root constant UIPass sets per draw:
//   materialID 0 -- plain: the bound texture * per-vertex tint (images, sprites, solid fills via the
//                  1x1 white texture).
//   materialID 1 -- SDF text: the bound R8 font page's RED channel is a signed distance field
//                  (0.5 == glyph edge). smoothstep over the screen-space derivative keeps the edge
//                  crisp at ANY scale; optional outline / glow come from the effects table (b2) at
//                  the b0 effectIndex.
// Bindings (engine default root signature):
//   b0 space0 -- UIDrawConsts (materialID, effectIndex), set per draw command.
//   b2 space0 -- UIEffects table (uploaded from UIDrawList::Effects()).
//   t3 space0 -- UI texture (atlas / sprite / SDF font page).
//   sampler   -- UIPass's per-draw heap sampler; SDF AA needs a linear-clamp one.

Texture2D    g_UITex : register(t3, space0);
#include "sampler_heap.hlsli"   // samplers from the sampler heap
#define g_UISmp SmpDynamic()   // UIPass's per-draw choice

cbuffer UIDrawConsts : register(b0, space0)
{
    uint g_materialID;   // 0 = plain, 1 = SDF text
    uint g_effectIndex;  // index into g_fx[]
    uint g_uiPad0;
    uint g_uiPad1;
};

struct UITextEffect
{
    float4 outlineColor;
    float4 glowColor;
    float  outlineWidthN; // normalised (px / sdfPixelRange); 0 = no outline
    float  glowWidthN;    // normalised;                      0 = no glow
    float  softness;
    float  _pad;
    float4 customA;       // free-form params for custom text shaders
    float4 customB;       // (unused by this built-in shader)
    float4 runRect;       // whole-run rect for custom shaders (unused here)
};
cbuffer UIEffects : register(b2, space0)
{
    UITextEffect g_fx[64];
};

struct PSIn
{
    float4 pos   : SV_POSITION;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
};

float4 main(PSIn i) : SV_TARGET
{
    float4 tex = g_UITex.Sample(g_UISmp, i.uv);

    if (g_materialID == 0u)
        return tex * i.color;            // plain image / solid fill

    // SDF text.
    float dist = tex.r;                  // R8 atlas page; 0.5 == glyph edge
    float aa   = max(fwidth(dist), 1e-4); // screen-space AA half-width
    float fill = smoothstep(0.5 - aa, 0.5 + aa, dist);

    UITextEffect fx = g_fx[min(g_effectIndex, 63u)];

    float3 rgb = i.color.rgb;
    float  a   = i.color.a * fill;

    // Outline -- a band just outside the glyph body.
    if (fx.outlineWidthN > 0.0)
    {
        float outlineA = smoothstep(0.5 - fx.outlineWidthN - aa,
                                    0.5 - fx.outlineWidthN + aa, dist) * fx.outlineColor.a;
        rgb = lerp(fx.outlineColor.rgb, i.color.rgb, fill);
        a   = max(i.color.a * fill, outlineA);
    }

    // Glow -- soft halo fading outward from the edge, composited under the glyph.
    if (fx.glowWidthN > 0.0)
    {
        float glowA = smoothstep(0.5 - fx.glowWidthN, 0.5, dist) * fx.glowColor.a;
        rgb = lerp(fx.glowColor.rgb, rgb, a);
        a   = max(a, glowA);
    }

    return float4(rgb, a);
}
