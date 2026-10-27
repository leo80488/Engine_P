#pragma once

// PostProcess::ResolvedPostProcessSettings - the single, flat, override-free
// CONTRACT between the volume / profile layer and the post chain:
//
//   volume / profile layer  -->  ResolvedPostProcessSettings  -->  PostProcess::PostChain
//   (knows nothing about passes)          (POD)                    (knows nothing about volumes)
//
// Produced each frame by PostProcessResolveSystem (PostProcessResolve.h):
//   result = Flatten(engine default profile)
//   for each volume (priority ascending): BlendProfileInto(result, profile, weight)
//   then the live overrides, the same way
// The group / member layout is mirrored 1:1 by PostProcessProfile; the X-macro
// in PostProcessProperties.inl cross-checks both at compile time.

#include "PostProcess/PostProcessProfile.h"
#include "RenderGraph/RenderPass/ColorGradingParams.h"

#include <DirectXMath.h>

namespace PostProcess
{

struct ResolvedPostProcessSettings
{
    struct CAS
    {
        bool  enabled;
        float sharpness;
    } cas;

    struct AutoExposure
    {
        bool  enabled;
        float manualExposure;
        float adaptationTau;      // seconds, adapting to a brighter scene
        float adaptationTauDark;  // seconds, adapting to a darker scene
        float minLogLuma;
        float maxLogLuma;
        float lowPercent;
        float highPercent;
        float minExposure;
        float maxExposure;
        float evBias;
        float keyValue;
        float centerWeight;
        float skyWeight;
        float nprWeight;
        float highlightWeight;
    } autoExposure;

    struct Bloom
    {
        float strength;
    } bloom;

    struct UEColorGrading
    {
        bool enabled;
        DirectX::XMFLOAT3 shadowsSaturation;
        DirectX::XMFLOAT3 shadowsContrast;
        DirectX::XMFLOAT3 shadowsGamma;
        DirectX::XMFLOAT3 shadowsGain;
        DirectX::XMFLOAT3 shadowsOffset;
        DirectX::XMFLOAT3 midtonesSaturation;
        DirectX::XMFLOAT3 midtonesContrast;
        DirectX::XMFLOAT3 midtonesGamma;
        DirectX::XMFLOAT3 midtonesGain;
        DirectX::XMFLOAT3 midtonesOffset;
        DirectX::XMFLOAT3 highlightsSaturation;
        DirectX::XMFLOAT3 highlightsContrast;
        DirectX::XMFLOAT3 highlightsGamma;
        DirectX::XMFLOAT3 highlightsGain;
        DirectX::XMFLOAT3 highlightsOffset;
        float shadowsMax;
        float highlightsMin;
        float highlightsMax;
        float filmSlope;
        float filmToe;
        float filmShoulder;
        float filmBlackClip;
        float filmWhiteClip;
    } ueColorGrading;

    struct ColorGrading
    {
        bool              enabled;
        float             exposure;
        float             contrast;
        float             brightness;
        DirectX::XMFLOAT3 lift;
        DirectX::XMFLOAT3 gamma;
        DirectX::XMFLOAT3 gain;
        float             hueShift;
        float             saturation;
        float             vibrance;
        float             temperature;
        float             tint;
        float             vignetteStrength;
        float             filmGrain;
    } colorGrading;

    struct LensFlare
    {
        bool     enabled;
        float    intensity;
        uint32_t ghostCount;
        float    ghostDispersal;
        float    haloWidth;
        float    streakLength;
        float    chromaticOffset;
    } lensFlare;

    struct Underwater
    {
        bool              enabled;
        float             strength;
        float             scale;
        float             speed;
        DirectX::XMFLOAT3 tint;
        float             tintAmount;
    } underwater;

    struct DepthOfField
    {
        bool  enabled;
        float focusDistance;
        float focusRange;
        float transitionRange;
        float maxRadius;
    } dof;

    struct Pixelate   { bool enabled; uint32_t size;   } pixelate;
    struct Kuwahara   { bool enabled; uint32_t radius; } kuwahara;
    struct Posterize  { bool enabled; uint32_t levels; } posterize;
    struct Halftone   { bool enabled; float cellSize; float angle; } halftone;
    struct Dither     { bool enabled; uint32_t levels; } dither;
    struct Crosshatch { bool enabled; float density; float thickness; } crosshatch;
    struct SourceOutline {
        bool enabled;
        float depthWidth, normalWidth, depthExponent, normalExponent;
        float distancePower, depthMax, highlightU, highlightV;
        DirectX::XMFLOAT3 outlineColor, highlightColor;
    } sourceOutline;

    // Default-constructs to the engine shipping values (straight from the X-macro),
    // so a never-resolved instance is still a sane "no volumes" frame.
    ResolvedPostProcessSettings()
    {
#define PP_BOOL(g, m, l, d)            g.m = (d);
#define PP_FLOAT(g, m, l, d, a, b)     g.m = (d);
#define PP_FLOAT3(g, m, l, dx, dy, dz) g.m = DirectX::XMFLOAT3{dx, dy, dz};
#define PP_COLOR(g, m, l, dr, dg, db)  g.m = DirectX::XMFLOAT3{dr, dg, db};
#define PP_UINT(g, m, l, d, a, b)      g.m = static_cast<uint32_t>(d);
#include "PostProcess/PostProcessProperties.inl"
    }
};

// Copy every property value out of a profile, ignoring overrideState. Only
// meaningful for the engine default profile (all properties overriding).
inline ResolvedPostProcessSettings Flatten(const PostProcessProfile& p)
{
    ResolvedPostProcessSettings r;
#define PP_BOOL(g, m, l, d)            r.g.m = p.g.m.value;
#define PP_FLOAT(g, m, l, d, a, b)     r.g.m = p.g.m.value;
#define PP_FLOAT3(g, m, l, dx, dy, dz) r.g.m = p.g.m.value;
#define PP_COLOR(g, m, l, dr, dg, db)  r.g.m = p.g.m.value;
#define PP_UINT(g, m, l, d, a, b)      r.g.m = p.g.m.value;
#include "PostProcess/PostProcessProperties.inl"
    return r;
}

// Sequential lerp of one profile's OVERRIDDEN properties into the running
// result, pulled toward the profile by @p weight. Non-overriding properties
// pass the lower layer through untouched (the Overridable rule). The exposure
// multipliers blend in stops and the hue shift around the circle; @p linearOnly
// (a harness reverse arm) blends every float linearly.
inline void BlendProfileInto(ResolvedPostProcessSettings& dst,
                             const PostProcessProfile& p, float weight, bool linearOnly = false)
{
    const float manual0 = dst.autoExposure.manualExposure;
    const float minExp0 = dst.autoExposure.minExposure;
    const float maxExp0 = dst.autoExposure.maxExposure;
    const float hue0    = dst.colorGrading.hueShift;
#define PP_BOOL(g, m, l, d)            if (p.g.m.overrideState) dst.g.m = PostProcess::BlendValue(dst.g.m, p.g.m.value, weight);
#define PP_FLOAT(g, m, l, d, a, b)     if (p.g.m.overrideState) dst.g.m = PostProcess::BlendValue(dst.g.m, p.g.m.value, weight);
#define PP_FLOAT3(g, m, l, dx, dy, dz) if (p.g.m.overrideState) dst.g.m = PostProcess::BlendValue(dst.g.m, p.g.m.value, weight);
#define PP_COLOR(g, m, l, dr, dg, db)  if (p.g.m.overrideState) dst.g.m = PostProcess::BlendValue(dst.g.m, p.g.m.value, weight);
#define PP_UINT(g, m, l, d, a, b)      if (p.g.m.overrideState) dst.g.m = PostProcess::BlendValue(dst.g.m, p.g.m.value, weight);
#include "PostProcess/PostProcessProperties.inl"
    if (linearOnly) return;
    if (p.autoExposure.manualExposure.overrideState)
        dst.autoExposure.manualExposure = PostProcess::BlendLog2(manual0, p.autoExposure.manualExposure.value, weight);
    if (p.autoExposure.minExposure.overrideState)
        dst.autoExposure.minExposure = PostProcess::BlendLog2(minExp0, p.autoExposure.minExposure.value, weight);
    if (p.autoExposure.maxExposure.overrideState)
        dst.autoExposure.maxExposure = PostProcess::BlendLog2(maxExp0, p.autoExposure.maxExposure.value, weight);
    if (p.colorGrading.hueShift.overrideState)
        dst.colorGrading.hueShift = PostProcess::BlendAngleDeg(hue0, p.colorGrading.hueShift.value, weight);
}

// Pack the resolved (flat) color-grading block into the 16-byte-aligned GPU
// struct consumed by ToneMapPass's LUT bake.
inline ColorGradingParams ToColorGradingParams(const ResolvedPostProcessSettings::ColorGrading& c)
{
    ColorGradingParams p;
    p.exposure        = c.exposure;
    p.contrast        = c.contrast;
    p.brightness      = c.brightness;
    p.lift            = c.lift;
    p.gamma           = c.gamma;
    p.gain            = c.gain;
    p.hueShift        = c.hueShift;
    p.saturation      = c.saturation;
    p.vibrance        = c.vibrance;
    p.temperature     = c.temperature;
    p.tint            = c.tint;
    p.vignetteStrength = c.vignetteStrength;
    p.filmGrain       = c.filmGrain;
    return p;
}

} // namespace PostProcess
