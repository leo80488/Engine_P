// SwordTrailVFX M_Trail_Color / M_Trail_Refraction, exported from puji.
// Parameters retain the source material graph's meaning. Curve textures hold
// Niagara dynamic inputs (age x normalized ribbon index), not color images.
#include "TrailFX.hlsli"
#include "TrailScene.hlsli"

cbuffer SwordTrailParams : register(b8, space0)
{
    float4 ParticleTint;  //@default 1 1 1 1 @group "Color"
    float4 Color1;        //@default 1 1 1 1 @group "Color"
    float4 Color2;        //@default 1 1 1 1 @group "Color"
    float4 TrailControl;  //@default 0.95 0.75 1 1 @group "Trail"
    float4 Distortion;    //@default 1 1 1 -1 @group "Distortion"
    float4 Surface;       //@default 0 1 0 0 @group "Surface"
    float4 Refraction;    //@default 1.03 1 0 1 @group "Refraction"
    float4 Dynamic;       //@default 0.9 0 0 0 @group "Animation"
    float Tex_Trail;      //@group "Textures"
    float Tex_Distortion; //@group "Textures"
    float Tex_Noise;      //@group "Textures"
    float Tex_Normal;     //@group "Textures"
    float Tex_Dynamic;    //@group "Curves"
    float Tex_Color;      //@group "Curves"
    float Engine_TrailAge; //@label "Emitter age (runtime)" @group "Animation"
    float EmitterLifetime; //@default 0.6 @group "Animation"
    // x = continuous sheet opacity, y = continuous emission (0 = source pulse).
    float4 RibbonBody; //@default 0 0 0 0 @group "Continuity"
    // strength, longitudinal exponent, minimum width, soft edge fraction.
    float4 TailShape; //@default 0 0.8 0.02 0.12 @group "Tail"
};

float4 CurveAt(float slot, float age, float index, float4 fallback)
{
    if (!TrailSlotBound(slot)) return fallback;
    uint w, h;
    g_TrailTex[NonUniformResourceIndex((uint)(slot + 0.5))].GetDimensions(w, h);
    float2 uv = (saturate(float2(age, index)) * (float2(w, h) - 1) + 0.5) / float2(w, h);
    return TrailSampleSlotClamp(slot, uv);
}
float3 DesaturateUE(float3 color, float amount)
{
    return lerp(color, dot(color, float3(0.212639, 0.715168655, 0.072192319)).xxx, amount);
}
float3 HueUE(float3 color, float turns)
{
    float3 axis = normalize(float3(1, 1, 1));
    float angle = turns * 6.28318530718;
    return color * cos(angle) + cross(axis, color) * sin(angle)
         + axis * dot(axis, color) * (1 - cos(angle));
}
float4 SampleOrWhite(float slot, float2 uv)
{
    return TrailSlotBound(slot) ? TrailSampleSlot(slot, uv) : float4(1, 1, 1, 1);
}
float4 SampleTrailMask(float2 uv)
{
    // Source trail silhouettes use TA_Clamp. Wrapping after ScaleUVsByCenter
    // or Fade resurrects the opposite edge as detached islands.
    return TrailSlotBound(Tex_Trail) ? TrailSampleSlotClamp(Tex_Trail, uv) : float4(1,1,1,1);
}
float4 main(TrailPSIn i) : SV_TARGET
{
    float age = saturate(1 - i.alpha);
    // Continuity belongs to the mesh, not a flat-opacity fan. Contract the
    // original texture toward the blade centre as U approaches the old tail.
    float tailWidth = lerp(1, max(TailShape.z, pow(saturate(1-i.uv.x), max(TailShape.y,0.01))), saturate(TailShape.x));
    // Niagara CustomSide sets Right=-Facing; its slice V=0 is +axis (tip).
    // Engine ribbon V=0 is -axis (base), so preserve the source convention.
    float2 sourceUV = float2(i.uv.x, 0.5 - (i.uv.y-0.5) / max(tailWidth,0.001));
    float edge = abs(sourceUV.y-0.5)*2;
    float coverage = lerp(1, 1-smoothstep(1-max(TailShape.w,0.001),1,edge), saturate(TailShape.x));
    // RibbonLinkOrder is 1-System.NormalizedLoopAge in the source: index
    // increases from newest to oldest, just like the engine's stretched U.
    float4 dyn = CurveAt(Tex_Dynamic, age, i.uv.x, Dynamic);
    // The original Fade channel uses Emitter.Age / Particles.Initial.Lifetime,
    // not Particles.NormalizedAge. Read the same baked curve with that clock.
    float emitterFade = CurveAt(Tex_Dynamic, Engine_TrailAge / max(EmitterLifetime, 0.001), i.uv.x, Dynamic).w;
    dyn.w = RibbonBody.y > 0.5 ? 0 : emitterFade;
    float4 tint = ParticleTint * CurveAt(Tex_Color, age, i.uv.x, float4(1,1,1,i.alpha));
    float2 normal = SampleOrWhite(Tex_Normal, sourceUV + float2(-TrailTime(), 0)).xy * 2 - 1;
    float refMask = DesaturateUE(SampleTrailMask(sourceUV).rgb, 1).r;
    float2 offset = normal * (Refraction.x - 1) * Refraction.y * refMask * 32.0;
    if (Refraction.z > 0.5)
        return TrailRefractedOutput(i, 0, 0, offset * tint.a, tint.a * coverage);

    // Source graph: two independent panners, dynamic Fade offsets U BEFORE
    // ScaleUVsByCenter (division around 0.5 on BOTH axes).
    float dist = SampleOrWhite(Tex_Distortion, sourceUV + float2(0, TrailTime())).r - 0.5;
    float2 uv = (sourceUV + dist * dyn.y + float2(dyn.w, 0) - 0.5)
              / max(abs(TrailControl.xy), 0.0001) + 0.5;
    float3 trail = SampleTrailMask(uv).rgb;
    float2 noiseUV = sourceUV;
    if (Refraction.w > 0.5)
        noiseUV += pow(max(SampleOrWhite(Tex_Distortion,
                     sourceUV + TrailTime() * Distortion.zw).r, 1e-6), Distortion.y) * Distortion.x;
    float noise = SampleOrWhite(Tex_Noise, noiseUV + float2(TrailTime(), 0)).r;
    float3 powered = pow(max(trail, 1e-6), TrailControl.z) * TrailControl.w;
    float opacity = noise * DesaturateUE(powered, Surface.w).r - (1 - dyn.x);
    float mixColor = pow(max(saturate(sourceUV.y) + Surface.x, 1e-6), Surface.y);
    float3 rgb = lerp(tint.rgb * Color1.rgb, HueUE(tint.rgb, dyn.z) * Color2.rgb, mixColor);
    float3 bodyRGB = DesaturateUE(rgb, Surface.z);
    rgb = DesaturateUE(rgb * saturate(trail), Surface.z);
    // Noise/dissolve is a DETAIL layer. A smoothly feathered sheet beneath it
    // prevents its zeroes from cutting the blade into disconnected islands.
    // Fade by each segment's age so a moving weapon emits without pulse gaps.
    float across = smoothstep(0, 0.08, i.uv.y) * smoothstep(0, 0.08, 1-i.uv.y);
    float bodyAlpha = saturate(RibbonBody.x) * across * saturate(tint.a)
                    * (1-age) * smoothstep(0, 0.08, 1-i.uv.x);
    if (RibbonBody.y <= 0.5) bodyAlpha *= saturate(1-emitterFade);
    float detailAlpha = saturate(opacity);
    opacity = detailAlpha + bodyAlpha * (1-detailAlpha);
    rgb = (rgb * detailAlpha + bodyRGB * bodyAlpha * (1-detailAlpha)) / max(opacity, 1e-6);
    return TrailRefractedOutput(i, rgb, opacity * coverage, offset, tint.a * coverage);
}
