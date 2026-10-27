// Example custom UI text shader: a rainbow whose hue sweeps across the WHOLE string (not per glyph,
// via TextRunT()), plus a soft brightness wave.
// Assign on a Canvas UIText: customShaderPath = "text_fx/TextFX_RainbowWave.ps.hlsl".
// Params (Inspector "Custom Params A" = customA, "B" = customB; a 0 in customA picks the default):
//   customA.x  hue scroll speed                    (default 0.35)
//   customA.y  rainbow repeats across the run      (default 1.0 = one spectrum, first to last char)
//   customA.z  wave strength 0..1                  (default 0.25)
//   customA.w  wave speed                          (default 3.0)
//   customB.x  gradient direction: 0 = horizontal (left -> right), 1 = vertical (top -> bottom);
//              fractions blend the two.
// The UIText colour still tints the result (white for a pure rainbow) and its alpha fades it.
// Works with fxShadow / fxJitter: the shadow copy renders behind with the stock shader.

#include "../TextFX.hlsli"

float4 main(TextPSIn i) : SV_TARGET
{
    const float dist = TextSdfDist(i.uv);
    const float fill = TextSdfFill(dist);
    if (fill <= 0.0) discard;

    const UITextEffect fx = TextEffectData();
    const float hueSpeed = (fx.customA.x != 0.0) ? fx.customA.x : 0.35;
    const float hueFreq  = (fx.customA.y != 0.0) ? fx.customA.y : 1.0;
    const float waveAmp  = (fx.customA.z != 0.0) ? saturate(fx.customA.z) : 0.25;
    const float waveSpd  = (fx.customA.w != 0.0) ? fx.customA.w : 3.0;
    const float vertical = saturate(fx.customB.x);

    // Position across the WHOLE string (0 at the first glyph's left edge, 1 at the last glyph's right
    // edge); Font::DrawText supplies the run rect, so the gradient is seamless across glyphs.
    const float2 runT = TextRunT(fx, i.pos);
    const float  t    = lerp(runT.x, runT.y, vertical);

    const float3 rainbow = TextHueToRgb(frac(t * hueFreq + g_uiTimeSec * hueSpeed));

    // Soft brightness wave sweeping along the run.
    const float wave = 1.0 - waveAmp * (0.5 + 0.5 *
        sin(g_uiTimeSec * waveSpd + t * 6.2831853));

    const float3 rgb = rainbow * wave * i.color.rgb;
    return float4(rgb, i.color.a * fill);
}
