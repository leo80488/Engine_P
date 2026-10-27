#ifndef TAA_COMMON_HLSLI
#define TAA_COMMON_HLSLI

// Shared TAACB and pure helpers for the TAA resolve. The helpers take values, not
// textures or samplers, so this header claims no register slots.
// The CB layout must match TAAPass::TAACB field for field: change both together.

// TAA_USE_TONEMAP_BLEND, a build-time switch:
//   1 (default) - Karis tonemap blend: currT = ToneMapLuma(curr), hist =
//                 ToneMapLuma(hist), blend in tonemapped space, output =
//                 InvToneMapLuma(resolved). Robust against HDR fireflies.
//   0           - Falcor-style linear blend, no tonemap around it. Lighter, but
//                 bright peaks enter the lerp with their full HDR luma (the HDR
//                 ratio detectors still fire). For A/B against the Falcor reference.
#ifndef TAA_USE_TONEMAP_BLEND
#define TAA_USE_TONEMAP_BLEND 1
#endif

cbuffer TAACB : register(b0, space2)
{
    float4x4 invViewProjNoJitter;    // inverse(current UNJITTERED VP), row-vector convention
                                     //   used for the jitter-free sky / cloud motion vectors in TAA.cs.hlsl
    float4x4 prevViewProj;           // previous frame unjittered VP
    uint     width;
    uint     height;
    float    historyWeight;          // [0, 0.99] direct history blend weight.
                                     //   alpha_diffuse  = 1 - historyWeight
                                     //   alpha_specular = alpha_diffuse x 0.125 (a longer specular history)
                                     // A direct weight rather than exp(-dt / tau): the time-constant slider was
                                     // visually flat over most of its range. The cost is frame-rate dependence (at
                                     // w = 0.9, 144 fps integrates 2.4x faster in wall time than 60 fps), mild across
                                     // the usual 60-144 fps.
    float    hasHistory;             // 0 = first frame / after resize, 1 = valid
    float    deltaTime;              // seconds elapsed this frame (TAA.cs.hlsl does not read it)
    float    jitterX;                // current frame jitter X in pixels [-0.5, +0.5)
    float    jitterY;                // current frame jitter Y in pixels [-0.5, +0.5)
    float    colorBoxSigma;          // base AABB gamma (default 1.5)
    float    colorBoxSigmaSpecular;  // AABB gamma for specular pixels (default 2.0)
    float    specularRoughnessMax;   // roughness threshold for "specular" (default 0.5)
    uint     antiFlicker;            // 0/1 - Falcor distance-to-clamp anti-flicker
    float    velocityWiden;          // motion-proportional AABB widening factor
    float    sharpenStrength;        // Karis 5-tap unsharp blend factor (default 0.1; 0 disables)
    float    outlineMinAlpha;        // alpha floor where outlineStencilBit is set (TAAPass default 0: off)
    uint     outlineStencilBit;      // bit mask AND'd against stencil; 0 disables the lookup
    float    particleVfxMinAlpha;    // alpha floor for particle-VFX stencil-tagged pixels (sharp, no smear)
    uint     particleVfxBit;         // stencil bit for TAA-excluded particles; 0 disables the lookup
    // Responsive motion path: velFactor ramps over [motionVelocityStartPx,
    // motionVelocityEndPx] px per frame; motionAlphaFloor x velFactor floors alpha and
    // motionRawBlend x velFactor blends in the raw centre tap.
    float    motionVelocityStartPx;
    float    motionVelocityEndPx;
    float    motionAlphaFloor;
    float    motionRawBlend;
    // Diagnostic. 1 = paint every pixel carrying outlineStencilBit magenta and
    // return. Answers the one question code reading cannot: does OutlinePass
    // actually tag this material's rim, and does TAA actually see the tag?
    // Everything outline-related in this shader is gated on that bit, so if it
    // is never set, every outline mitigation here is dead code.
    uint     outlineStencilDebug;
    // Alpha floor for a texel that was outline last frame and is not now (the band the
    // rim just vacated). The rim writes neither depth nor velocity, so that texel
    // reprojects onto itself and pulls the dark rim out of history, and neither the
    // depth disocclusion detector nor the 3x3 variance clamp rejects it (the rim's new
    // position, 1-2 texels away, drags the box minimum down to the outline colour).
    // This is the outline-shaped trail behind a moving character. 0 disables.
    float    outlineVacatedAlpha;
    uint     hasOutlineHist;         // 0 when the coverage ping-pong isn't ready yet
};

// ==== Luma / tonemap ====

#include "core/color.hlsli"   // Luminance

// Karis-style Reinhard tonemap weighted by the max channel, not BT.709 luma. Luma
// underweights blue (.0722) and overweights green (.7152): float3(50, 0, 0) has
// luma ~10.6 -> weight 1/(1+10.6) ~ 0.087, float3(0, 50, 0) gets 1/(1+35.8) ~ 0.027,
// so a red firefly is compressed 3x less and leaks into the AABB stats and the
// blend. The max channel compresses any single-channel spike the same. As in the
// Karis 2014 reference.
float3 ToneMapLuma(float3 c)
{
    return c / (1.0 + max(max(c.r, c.g), c.b));
}

float3 InvToneMapLuma(float3 c)
{
    return c / max(1.0 - max(max(c.r, c.g), c.b), 1e-4);
}

// ==== YCoCg (no scale) for perceptual neighbourhood clamping ====

float3 RGBToYCoCg(float3 c)
{
    return float3(
         0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
         0.5  * c.r              - 0.5  * c.b,
        -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

float3 YCoCgToRGB(float3 y)
{
    float tmp = y.x - y.z;
    return float3(tmp + y.y, y.x + y.z, tmp - y.y);
}

// ==== Variance AABB clip ====
// Project history q onto the AABB [mn, mx] along the centre -> q ray. A
// per-component clamp would bias along the principal channel; the ray clip keeps
// the chrominance direction.

float3 ClipAABB(float3 mn, float3 mx, float3 q)
{
    float3 center  = 0.5 * (mx + mn);
    float3 extents = 0.5 * (mx - mn) + 1e-5;
    float3 v       = q - center;
    float3 vabs    = abs(v) / extents;
    float  ma      = max(vabs.x, max(vabs.y, vabs.z));
    return (ma > 1.0) ? (center + v / ma) : q;
}

// ==== Screen UV <-> NDC ====

float2 UVToNDC(float2 uv)
{
    return float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
}

float2 NDCToUV(float2 ndc)
{
    return ndc * float2(0.5, -0.5) + 0.5;
}

#endif // TAA_COMMON_HLSLI
