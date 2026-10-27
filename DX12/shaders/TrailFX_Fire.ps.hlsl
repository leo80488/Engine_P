// Built-in noise flame for weapon trails: time-scrolled fbm erodes the ribbon as it ages (a ragged
// burning tail) and drives a three-stop fire gradient Deep -> Mid -> Core.
// COLOUR IS ENTIRELY THIS SHADER'S: for a custom trail shader Trail.vs.hlsl outputs white scaled by
// hdrIntensity, so the component's Start/End Color are IGNORED and i.color.a is always 1. Every
// knob lives in the b8 cbuffer below and reaches the material Inspector through shader reflection
// (//@default / @min / @max / @label / @group).
// TEXTURES
//   Tex_Fuel  OPTIONAL fuel / shape mask: RGB tints, A modulates heat. Engine texture slots (Base
//             Color...) are HIDDEN for custom-shader materials, so a Tex_ slot is the ONLY way to
//             author a mask; empty -> the i.texIdx header texture -> white.
// MATERIAL SETUP (Inspector)
//   Use Custom Shader = ON
//   Pixel Shader      = asset/Shaders/Trials/TrailFX_Fire.ps.hlsl
//                       (a BARE filename resolves under shaders/ instead: the two copies of this
//                        file must stay byte-identical)
//   Shading Model     = Unlit
// Suggested component settings: blendMode=Additive, excludeFromTAA=on for fast swings,
// uvMode=Arc Length.
// b8 LAYOUT: members are declared in @group order (Color -> Flame -> Textures). The Inspector emits
// a section header whenever the group string CHANGES, so keep a group's members contiguous or its
// header is drawn twice. Appending is safe; reordering rewrites every authored .imat.

#include "TrailFX.hlsli"

cbuffer TrailFireParams : register(b8, space0)
{
    // float4 because these ARE vectors, and a param whose name contains "color" gets an HDR colour
    // swatch instead of four sliders.
    float4 DeepColor;     //@default 0.85 0.08 0.0 1.0 @label "Deep Flame Color (cool end)" @group "Color"
    float4 MidColor;      //@default 1.0 0.45 0.02 1.0 @label "Mid Flame Color" @group "Color"
    float4 CoreColor;     //@default 1.0 0.9 0.55 1.0 @label "White-Hot Core Color" @group "Color"
    // One named scalar per knob: four consecutive floats pack into one 16-byte
    // row exactly like a float4, and each gets its own @min/@max.
    float  ScrollSpeed;   //@default 1.7 @min 0.0 @max 8.0 @label "Scroll Speed" @group "Flame"
    float  NoiseScale;    //@default 3.0 @min 0.5 @max 16.0 @label "Noise Scale" @group "Flame"
    float  FlickerMix;    //@default 0.35 @min 0.0 @max 1.0 @label "Flicker Mix (fine noise)" @group "Flame"
    float  EdgeThreshold; //@default 0.45 @min 0.05 @max 1.0 @label "Edge Threshold" @group "Flame"
    float  Intensity;     //@default 2.4 @min 0.0 @max 8.0 @label "Heat Intensity" @group "Flame"
    // Texture slot: the bound texture's bindless index as a float (-1 = nothing bound; TrailFX.hlsli).
    // Tex_* slots are listed apart ("Custom Textures") with their OWN group-header tracker, so the
    // @group must not reuse a value-param group name ("Color"/"Flame") or the same header prints in
    // both lists. They are labelled by variable name and ignore @label, so:
    //   Tex_Fuel  fuel / shape mask: RGB tints, A modulates heat. Empty -> header texture -> white.
    float  Tex_Fuel;      //@group "Textures"
};

float4 main(TrailPSIn i) : SV_TARGET
{
    const float t    = TrailTime();
    const float life = i.alpha;                 // 1 = newest, 0 = oldest

    // Two noise fields scrolling head -> tail at different speeds and scales: slow fbm = flame body,
    // fast fine noise = flicker. The V-scale and counter-scroll ratios (2.0 : 3.0, 0.6 : 1.7) are fixed
    // relative to the knobs.
    float n = TrailFbm(float2(i.uv.x * NoiseScale         + t * ScrollSpeed,
                              i.uv.y * NoiseScale * (2.0 / 3.0)
                                                          - t * ScrollSpeed * (0.6 / 1.7)));
    n = lerp(n, TrailNoise(float2(i.uv.x * NoiseScale * 3.0 + t * ScrollSpeed * (3.1 / 1.7),
                                  i.uv.y * NoiseScale * (5.0 / 3.0) + 7.7)),
             saturate(FlickerMix));

    // Fuel / shape mask: the Tex_ slot wins, then the header texture, then TrailSampleTexture's white.
    const float4 tex = TrailSlotBound(Tex_Fuel)
                     ? TrailSampleSlot(Tex_Fuel, i.uv)
                     : TrailSampleTexture(i.texIdx, i.uv);

    // Heat: newest = hottest, noise-eroded so the tail burns away ragged.
    const float heat = life * (0.45 + 0.75 * n) * tex.a;

    // Soft threshold -> flame silhouette with a ragged edge.
    const float a = smoothstep(EdgeThreshold * 0.27, EdgeThreshold, heat);
    if (a < 0.004) discard;

    // Three-stop fire gradient.
    float3 fire = lerp(DeepColor.rgb, MidColor.rgb, saturate(heat * 1.6));
    fire = lerp(fire, CoreColor.rgb, saturate(heat * heat * 2.2 - 0.25));

    // i.color.rgb = flat hdrIntensity for custom shaders (no tint); i.color.a is always 1 here, so it is
    // deliberately NOT multiplied in.
    const float3 rgb = fire * (0.6 + Intensity * heat) * i.color.rgb * tex.rgb;
    return TrailOutput(i, rgb, a);
}
