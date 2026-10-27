// WorldUI.ps.hlsl -- bindless world-space UI sampler.
//
// Per-vertex texIdx encodes the binding + a kind:
//   0xFFFFFFFF   -- bar / border: the vertex colour.
//   bit31 set    -- SDF font glyph: the R8 page's RED channel is a signed distance
//                   field; a reconstructed edge stays sharp at any distance / scale.
//   bits 29..24  -- effects-table slot (b2, g_fx[64]): outline / glow per text.
//   bits 23..0   -- bindless index into the engine's space2 table.
//   bit31 clear  -- plain image: sample * tint.

Texture2D    g_BindlessTex[] : register(t0, space2);
#include "sampler_heap.hlsli"   // samplers from the sampler heap
#define g_UISmp SmpFixed(SMP_LINEAR_CLAMP)   // DebugIcon / WorldUIBillboard only ever bound linear clamp

// Mirrors UI::GpuTextEffect (UIDrawList.h, 96 bytes) / UI.ps.hlsl.
struct UITextEffect
{
    float4 outlineColor;
    float4 glowColor;
    float  outlineWidthN;
    float  glowWidthN;
    float  softness;
    float  _pad;
    float4 customA;
    float4 customB;
    float4 runRect;
};
cbuffer WorldUIEffects : register(b2, space0)
{
    UITextEffect g_fx[64];
};

struct PSIn
{
    float4 pos                          : SV_POSITION;
    float2 uv                           : TEXCOORD0;
    nointerpolation uint texIdx         : TEXCOORD1;
    float4 color                        : COLOR0;
};

float4 main(PSIn i) : SV_TARGET
{
    if (i.texIdx == 0xFFFFFFFFu)
        return i.color;                       // bar / border -- flat colour

    const uint idx   = i.texIdx & 0x00FFFFFFu;          // bindless page/texture
    const uint eff   = (i.texIdx >> 24) & 0x3Fu;        // effects-table slot
    const bool isSDF = (i.texIdx & 0x80000000u) != 0u;
    float4 tex = g_BindlessTex[NonUniformResourceIndex(idx)].Sample(g_UISmp, i.uv);

    if (isSDF)
    {
        float dist = tex.r;                   // R8 atlas page; 0.5 == glyph edge
        float aa   = max(fwidth(dist), 1e-4);
        float fill = smoothstep(0.5 - aa, 0.5 + aa, dist);

        UITextEffect fx = g_fx[eff];
        float3 rgb = i.color.rgb;
        float  a   = i.color.a * fill;

        // Outline -- band just outside the glyph body (UI.ps.hlsl parity).
        if (fx.outlineWidthN > 0.0)
        {
            float outlineA = smoothstep(0.5 - fx.outlineWidthN - aa,
                                        0.5 - fx.outlineWidthN + aa, dist) * fx.outlineColor.a;
            rgb = lerp(fx.outlineColor.rgb, i.color.rgb, fill);
            a   = max(i.color.a * fill, outlineA * i.color.a);
        }

        // Glow -- soft halo fading outward from the edge.
        if (fx.glowWidthN > 0.0)
        {
            float glowA = smoothstep(0.5 - fx.glowWidthN, 0.5, dist) * fx.glowColor.a;
            rgb = lerp(fx.glowColor.rgb, rgb, a);
            a   = max(a, glowA * i.color.a);
        }

        return float4(rgb, a);
    }
    return tex * i.color;                      // plain image
}
