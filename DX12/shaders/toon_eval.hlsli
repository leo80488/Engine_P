#ifndef TOON_EVAL_HLSLI
#define TOON_EVAL_HLSLI

// The NPR toon model's per-light maths, shared by the deferred pass (Lighting.ps,
// NPR_PASS) and the forward path (custom_surface.hlsli). Pure: no register, no heap;
// sampling the ramp is the caller's (a binding, and the two passes have different
// samplers). The model's provenance - a transcription of the Blender "Arknights:
// Endfield_PBRToonBase" node group, whose constants are not to be cleaned up - and the
// VIS-LINEAR / RAD-GATED invariants: the PROVENANCE note in Lighting.ps.hlsl.
#include "core/color.hlsli"   // Luminance

// Ramp row layout. This project's ramp assets
// (asset/Default_Texture/RampMap_Linear_RGBAHalf*.itex, 256x16) put the authored colored
// ramps in the lower half:
//   band1 V~0.125 / band2 V~0.375 : grayscale placeholders (shadow floor = pure black)
//   band3 V~0.625 : authored spec ramp   (colored)
//   band4 V~0.875 : authored shadow ramp (warm, non-black lifted floor)
// DanbaidongRP's SampleDirectShadowRamp / SpecularRamp default to rampY 0.125 / 0.375
// (band1/2), which here would give a grayscale, pure-black-floored face shadow, so these
// target the authored bands 3/4. A DanbaidongRP-standard ramp with content in band1/2
// wants 0.125 / 0.375 back.
#define TOON_RAMP_V_DIRECT_SPEC    0.625   // band3: authored spec ramp
#define TOON_RAMP_V_DIRECT_SHADOW  0.875   // band4: authored shadow ramp with lifted floor
// Additional (point / spot) light shadow ramp. DanbaidongRP gives additional lights their
// own row (band1, V=0.125), but band1 in this project's ramps is a grayscale placeholder
// with a pure-black floor and would make them too dark, so they reuse the authored shadow
// band. Use 0.125 with a DanbaidongRP-standard ramp that has real band1 content.
#define TOON_RAMP_V_ADD_SHADOW     0.875

// The node graph's sigmoid: base 100000 and factor -3, not e and not -1.
float SigmoidSharp(float x, float center, float sharp)
{
    return saturate(1.0 / (1.0 + pow(100000.0, -3.0 * sharp * (x - center))));
}

// A non-face part's form shadow for one light (1 = lit): the sharpened
// half-lambert DanbaidongRP uses for additional lights too.
//   dirTowardLight  unit vector from the shaded point TOWARD the light.
float ToonHalfLambertTerm(float3 N, float3 dirTowardLight, float threshold, float sharp)
{
    float halfLambert = dot(N, dirTowardLight) * 0.5 + 0.5;
    return SigmoidSharp(halfLambert, threshold, sharp);
}

// The material's nprShadowSmooth as that sigmoid's sharpness.
float ToonNdlSharp(float shadowSmooth)
{
    return max((1.0 - saturate(max(shadowSmooth, 0.001) * 2.0)) * 1.5, 0.06);
}

// Split a physically-authored light into chroma and scalar energy. Artists can
// retain the scene hue while partially decoupling a toon character from extreme
// PBR intensities; defaults (both weights = 1) reproduce the source light.
float3 ToonCoupleSceneLight(float3 sceneLight, float colorWeight, float intensityWeight,
                            float referenceLuminance)
{
    float sceneLum = max(Luminance(max(sceneLight, 0.0)), 1e-5);
    float3 sceneChroma = max(sceneLight, 0.0) / sceneLum;
    float3 toonChroma = lerp(float3(1.0, 1.0, 1.0), sceneChroma,
                             saturate(colorWeight));
    float toonLum = lerp(max(referenceLuminance, 1e-4), sceneLum,
                         saturate(intensityWeight));
    return toonChroma * toonLum;
}

#endif // TOON_EVAL_HLSLI
