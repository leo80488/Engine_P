// Built-in random colour-mix trail: fbm-turbulent hue cycling, time-scrolled along the ribbon, with
// a second offset hue noise-mixed in so neighbouring pixels blend random colours instead of a clean
// gradient; alpha shimmers with the noise.
// COLOUR IS ENTIRELY THIS SHADER'S: for a custom trail shader Trail.vs.hlsl outputs white scaled by
// hdrIntensity, so the component's Start/End Color are IGNORED and i.color.a is always 1. Every
// knob lives in the b8 cbuffer below and reaches the material Inspector through shader reflection
// (//@default / @min / @max / @label / @group).
// TEXTURES
//   Tex_Mask  OPTIONAL shape mask: RGB tints, A gates opacity. Engine texture slots (Base Color...)
//             are HIDDEN for custom-shader materials, so a Tex_ slot is the ONLY way to author a
//             mask; empty -> the i.texIdx header texture -> white.
// MATERIAL SETUP (Inspector)
//   Use Custom Shader = ON
//   Pixel Shader      = asset/Shaders/Trials/TrailFX_RainbowNoise.ps.hlsl
//                       (a BARE filename resolves under shaders/ instead: the two copies of this
//                        file must stay byte-identical)
//   Shading Model     = Unlit
// b8 LAYOUT: members are declared in @group order (Hue -> Mix -> Textures). The Inspector emits a
// section header whenever the group string CHANGES, so keep a group's members contiguous or its
// header is drawn twice. Appending is safe; reordering rewrites every authored .imat.

#include "TrailFX.hlsli"

cbuffer TrailRainbowParams : register(b8, space0)
{
    // One named scalar per knob: four consecutive floats pack into one 16-byte
    // row exactly like a float4, and each gets its own @min/@max.
    float HueAlongTrail;   //@default 0.6 @min 0.0 @max 4.0 @label "Hue Along Trail" @group "Hue"
    float HueSpeed;        //@default 0.25 @min 0.0 @max 4.0 @label "Hue Scroll Speed" @group "Hue"
    float Turbulence;      //@default 0.55 @min 0.0 @max 1.0 @label "Turbulence (noise → hue)" @group "Hue"
    float SecondHueOffset; //@default 0.33 @min 0.0 @max 1.0 @label "Second Hue Offset" @group "Hue"

    float MixAmount;       //@default 0.5 @min 0.0 @max 1.0 @label "Second Hue Mix Amount" @group "Mix"
    float Saturation;      //@default 1.0 @min 0.0 @max 1.0 @label "Saturation (0 = white)" @group "Mix"
    float AlphaShimmer;    //@default 1.0 @min 0.0 @max 1.0 @label "Alpha Shimmer" @group "Mix"
    float Intensity;       //@default 1.0 @min 0.0 @max 8.0 @label "Intensity" @group "Mix"
    // Texture slot: the bound texture's bindless index as a float (-1 = nothing bound; TrailFX.hlsli).
    // Tex_* slots are listed apart ("Custom Textures") with their OWN group-header tracker, so the
    // @group must not reuse a value-param group name ("Hue"/"Mix") or the same header prints in both
    // lists. They are labelled by variable name and ignore @label, so:
    //   Tex_Mask  shape mask: RGB tints, A gates opacity. Empty -> header texture -> white.
    float Tex_Mask;        //@group "Textures"
};

float4 main(TrailPSIn i) : SV_TARGET
{
    const float t = TrailTime();

    const float n  = TrailFbm(float2(i.uv.x * 4.0 + t * 1.2,
                                     i.uv.y * 3.0 - t * 0.8));
    const float n2 = TrailNoise(float2(i.uv.x * 11.0 - t * 2.4,
                                       i.uv.y * 6.0 + t * 1.1));

    // Hue drifts along U, scrolls with time, and is churned by the noise.
    const float hue = frac(i.uv.x * HueAlongTrail + t * HueSpeed + n * Turbulence);
    float3 rgb = TrailHueToRGB(hue);
    // Random second colour mixed in.
    rgb = lerp(rgb, TrailHueToRGB(frac(hue + SecondHueOffset)), n2 * saturate(MixAmount));
    rgb = lerp(float3(1.0, 1.0, 1.0), rgb, saturate(Saturation));

    // Shape mask: the Tex_ slot wins, then the header texture, then TrailSampleTexture's white.
    const float4 tex = TrailSlotBound(Tex_Mask)
                     ? TrailSampleSlot(Tex_Mask, i.uv)
                     : TrailSampleTexture(i.texIdx, i.uv);

    // i.color.rgb = flat hdrIntensity for custom shaders (no tint).
    rgb *= i.color.rgb * tex.rgb * Intensity * (0.7 + 0.6 * n2);

    // i.color.a is always 1 here, so it is deliberately NOT multiplied in.
    const float a = i.alpha * tex.a * lerp(1.0, 0.55 + 0.45 * n, saturate(AlphaShimmer));
    if (a < 0.004) discard;
    return TrailOutput(i, rgb, a);
}
