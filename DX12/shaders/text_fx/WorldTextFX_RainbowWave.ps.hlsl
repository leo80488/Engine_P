// WorldTextFX_RainbowWave.ps.hlsl -- example custom WORLD-SPACE text shader.
//
// Same effect as text_fx/TextFX_RainbowWave.ps.hlsl but for WorldUIText /
// DamageNumber (bindless path). Assign:
//   customShaderPath = "text_fx/WorldTextFX_RainbowWave.ps.hlsl"
// Params: customA.x hue speed, customA.y rainbow repeats across the run,
// customA.z wave strength, customA.w wave speed, customB.x 0=horizontal /
// 1=vertical gradient. Component colour tints; alpha carries distance fade.

#include "../WorldTextFX.hlsli"

float4 main(WorldTextPSIn i) : SV_TARGET
{
    const float dist = WorldTextSdfDist(i);
    const float fill = WorldTextSdfFill(dist);
    if (fill <= 0.0) discard;

    const UITextEffect fx = WorldTextEffectData(i);
    const float hueSpeed = (fx.customA.x != 0.0) ? fx.customA.x : 0.35;
    const float hueFreq  = (fx.customA.y != 0.0) ? fx.customA.y : 1.0;
    const float waveAmp  = (fx.customA.z != 0.0) ? saturate(fx.customA.z) : 0.25;
    const float waveSpd  = (fx.customA.w != 0.0) ? fx.customA.w : 3.0;
    const float vertical = saturate(fx.customB.x);

    const float2 runT = WorldTextRunT(fx, i.pos);
    const float  t    = lerp(runT.x, runT.y, vertical);

    const float3 rainbow = WorldTextHueToRgb(frac(t * hueFreq + g_wuTimeSec * hueSpeed));
    const float wave = 1.0 - waveAmp * (0.5 + 0.5 *
        sin(g_wuTimeSec * waveSpd + t * 6.2831853));

    const float3 rgb = rainbow * wave * i.color.rgb;
    return float4(rgb, i.color.a * fill);
}
