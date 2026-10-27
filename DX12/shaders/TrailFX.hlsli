// Shared plumbing for trail PIXEL shaders: the engine default, the built-in FX and user custom
// shaders. A custom trail PS is a complete pixel shader with entry point main:
//
//     #include "TrailFX.hlsli"
//     float4 main(TrailPSIn i) : SV_TARGET
//     {
//         float3 rgb = ...;
//         float  a   = ...;
//         return TrailOutput(i, rgb, a);
//     }
//
// Assign it through the entity's MATERIAL (Use Custom Shader = ON, this file as the Pixel Shader
// Path): the shape stays on TrailComponent, the material owns shading. A bare filename resolves
// under shaders/ (built-ins: TrailFX_Fire.ps.hlsl, TrailFX_RainbowNoise.ps.hlsl); a dir-qualified
// path (e.g. "asset/FX/my_trail.hlsl") is used as-is.
// TEXTURES: declare each as a "Tex_*" b8 slot (below); engine texture slots (Base Color...) are
// hidden for custom-shader materials, so Tex_ slots are the ONLY authoring surface. The header
// texture (i.texIdx = the material's Base Color / the component texture) still arrives as a
// fallback input.
// CONTRACT: the trail blend state is PREMULTIPLIED (ONE, INV_SRC_ALPHA) for EVERY trail. Always
// return through TrailOutput(), which premultiplies rgb by a and makes additive a = 0; never output
// straight (rgb, a).

#ifndef TRAIL_FX_HLSLI
#define TRAIL_FX_HLSLI

// Interpolants from Trail.vs.hlsl (must match its VSOut byte-for-byte).
//   uv.x  - U along the trail, 0 at the newest head; in arc-length mode it exceeds 1 when
//           uvTileLength tiles (wrap sampler).
//   uv.y  - V across the blade: 0 = base row, 1 = tip row.
//   alpha - age fade: 1 = newest, 0 = fully aged; use it as "life".
//   color - startColor -> endColor lerp x hdrIntensity (HDR); white x hdrIntensity for a custom
//           shader.
//   flags - header renderFlags: bit0 arc-UV, bit1 additive, bit2 TAA-exclude, bit3 world-anchored
//           UV, bits 8-15 shader bucket id.
struct TrailPSIn
{
    float4 pos   : SV_POSITION;
    float4 color : COLOR0;
    float  alpha : TEXCOORD0;
    float2 uv    : TEXCOORD1;
    nointerpolation uint texIdx : TEXCOORD2;   // bindless; 0xFFFFFFFF = none
    nointerpolation uint flags  : TEXCOORD3;
};

// Per-draw root constants (b0 space0, engine-shared slot, set per bucket).
cbuffer TrailDrawCB : register(b0, space0)
{
    uint g_trailVariantFilter;   // TAA bucket filter (VS-side concern)
    uint g_trailShaderBucket;    // shader bucket filter (VS-side concern)
    uint g_trailTimeBits;        // asfloat() = engine global time, seconds
    uint g_trailSlotFilter;      // per-trail draw filter (VS-side concern)
};

// Engine global time in seconds: drives UV scrolling and animated FX.
float TrailTime() { return asfloat(g_trailTimeBits); }

Texture2D    g_TrailTex[] : register(t0, space2);   // bindless (PIXEL-only visibility)
#include "sampler_heap.hlsli"   // samplers from the sampler heap
#define g_TrailSmp SmpFixed(SMP_LINEAR_WRAP)
#define g_TrailClampSmp SmpFixed(SMP_LINEAR_CLAMP)   // one-shot masks / curves

// The component's optional texture; none -> white (safe to multiply).
float4 TrailSampleTexture(uint texIdx, float2 uv)
{
    if (texIdx == 0xFFFFFFFFu) return float4(1.0, 1.0, 1.0, 1.0);
    return g_TrailTex[NonUniformResourceIndex(texIdx)].Sample(g_TrailSmp, uv);
}

// ==== Extra texture slots (b8 param convention) ====
// A b8 param named "Tex_<Name>" (float) is a TEXTURE SLOT: the inspector shows a drag-drop field
// instead of a float, and the value arriving here is the bound texture's bindless index as a float
// (-1 = nothing bound). Sample it with TrailSampleSlot() and always provide a fallback for the
// unbound case (procedural noise, white, ...):
//   float Tex_Distortion;  //@label "Distortion noise (empty = procedural)"
//   ...
//   float d = TrailSlotBound(Tex_Distortion)
//           ? TrailSampleSlot(Tex_Distortion, uv).r : TrailFbm(uv);
bool TrailSlotBound(float slot) { return slot >= 0.0; }

float4 TrailSampleSlot(float slot, float2 uv)
{
    return g_TrailTex[NonUniformResourceIndex((uint)(slot + 0.5))].Sample(g_TrailSmp, uv);
}

float4 TrailSampleSlotClamp(float slot, float2 uv)
{
    return g_TrailTex[NonUniformResourceIndex((uint)(slot + 0.5))].Sample(g_TrailClampSmp, uv);
}

// The premultiplied output packing alone, with no fog and no soft fade; prefer
// TrailOutput(i, rgb, a) below.
float4 TrailOutput(float3 rgb, float a, uint flags)
{
    a = saturate(a);
    if (flags & 2u) return float4(rgb * a, 0.0);   // additive: dest preserved
    return float4(rgb * a, a);                     // alpha blend
}

// Trails draw after the fog composites, so they get the fog the opaque scene got at their own
// depth, and fade out where the ribbon meets the opaque surface behind it instead of cutting a hard
// line into the ground. The blend is premultiplied ONE / INV_SRC_ALPHA (additive is alpha 0), so
// one rule covers both.
#include "fog_apply.hlsli"   // FogApplyCB b4 + s3, bound by TrailRenderPass

float4 TrailOutput(TrailPSIn i, float3 rgb, float a)
{
    const float4 c = TrailOutput(rgb, a, i.flags) * Fog_SoftFade(i.pos);
    return Fog_ApplyPremultiplied(c, Fog_Evaluate(i.pos));
}

// ==== Noise toolbox ====

#include "core/hash.hlsli"

// Value noise, smooth interpolation, ~0..1.
float TrailNoise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    const float a = HashHoskins12(i);
    const float b = HashHoskins12(i + float2(1, 0));
    const float c = HashHoskins12(i + float2(0, 1));
    const float d = HashHoskins12(i + float2(1, 1));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

// 3-octave fbm, ~0..1.
float TrailFbm(float2 p)
{
    float v = 0.0;
    float amp = 0.5;
    [unroll]
    for (int i = 0; i < 3; ++i)
    {
        v += amp * TrailNoise(p);
        p = p * 2.03 + 17.13;
        amp *= 0.5;
    }
    return v;
}

// Hue (0..1, wraps) -> saturated RGB.
float3 TrailHueToRGB(float hue)
{
    const float3 p = abs(frac(hue + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    return saturate(p - 1.0);
}

#endif
