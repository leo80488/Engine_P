#pragma once

// PostProcess::PostProcessProfile - a shared, authored look made entirely of
// Overridable<T> properties. It is a Resource (referenced by ProfileHandle,
// serialized to a .ppprofile asset), NOT embedded per volume: many volumes can
// reference one profile, and the engine default is one too.
//
// Each *Settings group is mirrored 1:1 by the flat ResolvedPostProcessSettings,
// and the X-macro in PostProcessProperties.inl cross-checks both at compile time
// (BlendProfileInto / Flatten / MakeEngineDefaultProfile name every member).
// cas, autoExposure, lensFlare, underwater and dof configure the pass of their
// name; bloom, colorGrading and ueColorGrading feed ToneMapPass; the stylize
// groups (pixelate ... crosshatch, sourceOutline) feed StylizePass.

#include "PostProcess/Overridable.h"
#include "Resource/ResourceHandle.h"

#include <DirectXMath.h>

namespace PostProcess
{

// Generational handle to a PostProcessProfile resource (see ProfileSystem).
using ProfileHandle = Resource::Handle;

struct CASSettings
{
    Overridable<bool>  enabled;
    Overridable<float> sharpness;
};

struct AutoExposureSettings
{
    Overridable<bool>  enabled;
    Overridable<float> manualExposure;
    // Time constants in seconds. `adaptationTau` governs adapting to a BRIGHTER
    // scene (exposure falling), `adaptationTauDark` to a darker one. Adaptation is
    // done in stops, so equal values are truly symmetric; a larger dark one gives the
    // slow dark adaptation of the eye.
    Overridable<float> adaptationTau;
    Overridable<float> adaptationTauDark;
    Overridable<float> minLogLuma;
    Overridable<float> maxLogLuma;
    Overridable<float> lowPercent;
    Overridable<float> highPercent;
    Overridable<float> minExposure;
    Overridable<float> maxExposure;
    Overridable<float> evBias;
    Overridable<float> keyValue;
    Overridable<float> centerWeight;
    Overridable<float> skyWeight;
    Overridable<float> nprWeight;
    Overridable<float> highlightWeight;
};

struct BloomSettings
{
    Overridable<float> strength;
};

struct UEColorGradingSettings
{
    Overridable<bool> enabled;
    Overridable<DirectX::XMFLOAT3> shadowsSaturation;
    Overridable<DirectX::XMFLOAT3> shadowsContrast;
    Overridable<DirectX::XMFLOAT3> shadowsGamma;
    Overridable<DirectX::XMFLOAT3> shadowsGain;
    Overridable<DirectX::XMFLOAT3> shadowsOffset;
    Overridable<DirectX::XMFLOAT3> midtonesSaturation;
    Overridable<DirectX::XMFLOAT3> midtonesContrast;
    Overridable<DirectX::XMFLOAT3> midtonesGamma;
    Overridable<DirectX::XMFLOAT3> midtonesGain;
    Overridable<DirectX::XMFLOAT3> midtonesOffset;
    Overridable<DirectX::XMFLOAT3> highlightsSaturation;
    Overridable<DirectX::XMFLOAT3> highlightsContrast;
    Overridable<DirectX::XMFLOAT3> highlightsGamma;
    Overridable<DirectX::XMFLOAT3> highlightsGain;
    Overridable<DirectX::XMFLOAT3> highlightsOffset;
    Overridable<float> shadowsMax;
    Overridable<float> highlightsMin;
    Overridable<float> highlightsMax;
    Overridable<float> filmSlope;
    Overridable<float> filmToe;
    Overridable<float> filmShoulder;
    Overridable<float> filmBlackClip;
    Overridable<float> filmWhiteClip;
};

struct ColorGradingSettings
{
    Overridable<bool>              enabled;
    Overridable<float>             exposure;
    Overridable<float>             contrast;
    Overridable<float>             brightness;
    Overridable<DirectX::XMFLOAT3> lift;
    Overridable<DirectX::XMFLOAT3> gamma;
    Overridable<DirectX::XMFLOAT3> gain;
    Overridable<float>             hueShift;
    Overridable<float>             saturation;
    Overridable<float>             vibrance;
    Overridable<float>             temperature;
    Overridable<float>             tint;
    Overridable<float>             vignetteStrength;
    Overridable<float>             filmGrain;
};

struct LensFlareSettings
{
    Overridable<bool>     enabled;
    Overridable<float>    intensity;
    Overridable<uint32_t> ghostCount;
    Overridable<float>    ghostDispersal;
    Overridable<float>    haloWidth;
    Overridable<float>    streakLength;
    Overridable<float>    chromaticOffset;
};

struct UnderwaterSettings
{
    Overridable<bool>              enabled;
    Overridable<float>             strength;
    Overridable<float>             scale;
    Overridable<float>             speed;
    Overridable<DirectX::XMFLOAT3> tint;
    Overridable<float>             tintAmount;
};

struct DepthOfFieldSettings
{
    Overridable<bool>  enabled;
    Overridable<float> focusDistance;
    Overridable<float> focusRange;
    Overridable<float> transitionRange;
    Overridable<float> maxRadius;
};

struct PixelateSettings
{
    Overridable<bool>     enabled;
    Overridable<uint32_t> size;
};

struct KuwaharaSettings
{
    Overridable<bool>     enabled;
    Overridable<uint32_t> radius;
};

struct PosterizeSettings
{
    Overridable<bool>     enabled;
    Overridable<uint32_t> levels;
};

struct HalftoneSettings
{
    Overridable<bool>  enabled;
    Overridable<float> cellSize;
    Overridable<float> angle;
};

struct DitherSettings
{
    Overridable<bool>     enabled;
    Overridable<uint32_t> levels;
};

struct SourceOutlineSettings
{
    Overridable<bool> enabled;
    Overridable<float> depthWidth, normalWidth, depthExponent, normalExponent;
    Overridable<float> distancePower, depthMax, highlightU, highlightV;
    Overridable<DirectX::XMFLOAT3> outlineColor, highlightColor;
};

struct CrosshatchSettings
{
    Overridable<bool>  enabled;
    Overridable<float> density;
    Overridable<float> thickness;
};

// The shared resource. Defaulted members override nothing (overrideState =
// false) - what a freshly created bounded volume wants. The engine default comes
// from MakeEngineDefaultProfile() (every property overriding).
struct PostProcessProfile
{
    CASSettings          cas;
    AutoExposureSettings autoExposure;
    BloomSettings        bloom;
    ColorGradingSettings colorGrading;
    UEColorGradingSettings ueColorGrading;
    LensFlareSettings    lensFlare;
    UnderwaterSettings   underwater;
    DepthOfFieldSettings dof;
    PixelateSettings     pixelate;
    KuwaharaSettings     kuwahara;
    PosterizeSettings    posterize;
    HalftoneSettings     halftone;
    DitherSettings       dither;
    CrosshatchSettings   crosshatch;
    SourceOutlineSettings sourceOutline;

    // True if at least one property overrides; volumes that contribute nothing are
    // skipped.
    bool HasAnyOverride() const;
};

// The base of every resolve: a profile with EVERY property overriding and set
// to the engine's shipping defaults (values come straight from the X-macro).
inline PostProcessProfile MakeEngineDefaultProfile()
{
    PostProcessProfile p;
#define PP_BOOL(g, m, l, d)            p.g.m.value = (d);                              p.g.m.overrideState = true;
#define PP_FLOAT(g, m, l, d, a, b)     p.g.m.value = (d);                              p.g.m.overrideState = true;
#define PP_FLOAT3(g, m, l, dx, dy, dz) p.g.m.value = DirectX::XMFLOAT3{dx, dy, dz};    p.g.m.overrideState = true;
#define PP_COLOR(g, m, l, dr, dg, db)  p.g.m.value = DirectX::XMFLOAT3{dr, dg, db};    p.g.m.overrideState = true;
#define PP_UINT(g, m, l, d, a, b)      p.g.m.value = static_cast<uint32_t>(d);         p.g.m.overrideState = true;
#include "PostProcess/PostProcessProperties.inl"
    return p;
}

inline bool PostProcessProfile::HasAnyOverride() const
{
    const PostProcessProfile& p = *this;
    bool any = false;
#define PP_BOOL(g, m, l, d)            any = any || p.g.m.overrideState;
#define PP_FLOAT(g, m, l, d, a, b)     any = any || p.g.m.overrideState;
#define PP_FLOAT3(g, m, l, dx, dy, dz) any = any || p.g.m.overrideState;
#define PP_COLOR(g, m, l, dr, dg, db)  any = any || p.g.m.overrideState;
#define PP_UINT(g, m, l, d, a, b)      any = any || p.g.m.overrideState;
#include "PostProcess/PostProcessProperties.inl"
    return any;
}

} // namespace PostProcess
