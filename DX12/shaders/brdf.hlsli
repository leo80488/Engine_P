#ifndef BRDF_HLSLI
#define BRDF_HLSLI

// brdf.hlsli - Cook-Torrance microfacet BRDF functions and the shading-model lobes.
// Shared by Lighting.ps.hlsl and Transparent.ps.hlsl.
// NO RESOURCE ACCESS IN THIS FILE: graphics, compute (HybridReflIntersectHW.cs.hlsl) and
// ray-query units include it under different root signatures, so it stays pure
// arithmetic and every caller opens its own resources (the anisotropic lobe has no LUT,
// the subsurface lobe no diffusion-profile texture). A design rule, not a hard limit:
// every root signature sets CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED; see light_eval.hlsli's
// header.

#include "core/math.hlsli"   // PI

// Fresnel-Schlick approximation.
float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

// Fresnel with roughness bias, for IBL specular at grazing angles.
float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
{
    float3 r = float3(1.0 - roughness, 1.0 - roughness, 1.0 - roughness);
    return F0 + (max(r, F0) - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

// GGX normal distribution function.
float DistributionGGX(float3 N, float3 H, float roughness)
{
    float a  = roughness * roughness;
    float a2 = a * a;
    float NdotH  = saturate(dot(N, H));
    float NdotH2 = NdotH * NdotH;
#if defined(PT_REFERENCE_GGX)
    // PT must integrate the same narrow NDF its VNDF proposal uses; the raster path's
    // 1e-7 floor clips almost all of the lobe at roughness 0.045. sin^2(theta) comes from
    // vectors: even 1-NoH^2 loses precision for narrow lobes (Filament's NDF /
    // Lagrange-identity note).
    float3 NxH = cross(N, H);
    float denom = dot(NxH, NxH) + a2 * NdotH2;
    return a2 / max(PI * denom * denom, 1e-30);
#else
    float denom  = NdotH2 * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denom * denom, 1e-7);
#endif
}

// Schlick-GGX geometry term (single direction).
float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / max(NdotV * (1.0 - k) + k, 1e-7);
}

// Smith geometry term (both light and view directions).
float GeometrySmith(float NdotV, float NdotL, float roughness)
{
    return GeometrySchlickGGX(NdotV, roughness)
         * GeometrySchlickGGX(NdotL, roughness);
}

// Karis 2013 analytical BRDF approximation (split-sum without LUT texture).
// EnvBRDF(F0, roughness, NdotV) ~ F0 * A + B
float2 EnvBRDFApprox(float roughness, float NdotV)
{
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4( 1.0,  0.0425,  1.04, -0.04);
    float4 r = roughness * c0 + c1;
    float  a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    return float2(-1.04, 1.04) * a004 + r.zw;
}

// Full Cook-Torrance specular (outSpecular) + energy-conserving diffuse weight (outKD).
void EvalCookTorrance(float3 N, float3 V, float3 L, float3 F0,
                      float roughness, float metalness, float3 albedo,
                      out float3 outSpecular, out float3 outKD)
{
    float3 H     = normalize(V + L);
    float  NdotL = saturate(dot(N, L));
    float  NdotV = saturate(dot(N, V));

    float3 F = FresnelSchlick(saturate(dot(H, V)), F0);
    float  D = DistributionGGX(N, H, roughness);
    float  G = GeometrySmith(NdotV, NdotL, roughness);

    outSpecular = (D * G * F) / max(4.0 * NdotV * NdotL, 1e-4);
    outKD       = (1.0 - F) * (1.0 - metalness);
}

// ==== ShadingModel::Anisotropic: directional GGX ====
// Brushed metal, hair, satin: the microfacet distribution is not rotationally symmetric,
// so the highlight is a STREAK, not a disc.
// Parameterisation matches Unreal's `Anisotropy` and Filament's `anisotropy`:
//     alpha = roughness^2               (DistributionGGX's `a`)
//     at    = alpha * (1 + anisotropy)  roughness ALONG the tangent
//     ab    = alpha * (1 - anisotropy)  roughness ACROSS it
// anisotropy > 0 widens the lobe along T, so the streak stretches along T. Hair and
// brushed metal whose tangent runs along the strand / brush want the streak ACROSS it:
// a NEGATIVE anisotropy or a 90 deg Anisotropy Rotation. Do not flip the sign here: it
// is UE's convention and flipping it inverts every value copied from a UE material.
// anisotropy == 0 does NOT reproduce EvalCookTorrance bit-for-bit: this pair uses
// height-correlated Smith visibility (the right partner for the anisotropic D), the
// isotropic path UE4's Schlick-k. The difference is a few percent of lobe energy at
// grazing angles, and the model is per MATERIAL, so nothing pops between the two.
static const float kAnisoMinAlpha = 1e-3;

void AnisotropicRoughness(float roughness, float anisotropy, out float at, out float ab)
{
    const float alpha = roughness * roughness;
    const float a     = clamp(anisotropy, -1.0, 1.0);
    at = max(alpha * (1.0 + a), kAnisoMinAlpha);
    ab = max(alpha * (1.0 - a), kAnisoMinAlpha);
}

// Anisotropic GGX NDF (Burley 2012) in Filament's stable form: one division on a single
// dot product instead of squaring three terms that overflow at low roughness.
float DistributionGGXAniso(float NdotH, float3 H, float3 T, float3 B,
                           float at, float ab)
{
    const float  ToH = dot(T, H);
    const float  BoH = dot(B, H);
    const float  a2  = at * ab;
    const float3 v   = float3(ab * ToH, at * BoH, a2 * NdotH);
    const float  v2  = max(dot(v, v), 1e-12);
    const float  w2  = a2 / v2;
    return a2 * w2 * w2 * (1.0 / PI);
}

// Height-correlated Smith VISIBILITY (Heitz 2014). It already includes the
// 1/(4 NdotL NdotV) denominator: do not divide again. EvalCookTorrance returns
// (D*G*F)/(4 NoV NoL); this path returns D*Vis*F.
float VisibilitySmithGGXAniso(float at, float ab,
                              float ToV, float BoV, float ToL, float BoL,
                              float NdotV, float NdotL)
{
    const float lambdaV = NdotL * length(float3(at * ToV, ab * BoV, NdotV));
    const float lambdaL = NdotV * length(float3(at * ToL, ab * BoL, NdotL));
    return 0.5 / max(lambdaV + lambdaL, 1e-6);
}

// Drop-in sibling of EvalCookTorrance for ShadingModel::Anisotropic, with the same
// out-parameter contract (specular already divided by 4 NoV NoL, kD the energy-conserving
// diffuse weight).
void EvalCookTorranceAniso(float3 N, float3 V, float3 L, float3 T, float3 B,
                           float3 F0, float roughness, float anisotropy,
                           float metalness, float3 albedo,
                           out float3 outSpecular, out float3 outKD)
{
    const float3 H     = normalize(V + L);
    const float  NdotL = saturate(dot(N, L));
    // abs: keeps the Smith term finite on back-facing shading normals instead of 0/0.
    const float  NdotV = abs(dot(N, V)) + 1e-5;
    const float  NdotH = saturate(dot(N, H));

    float at, ab;
    AnisotropicRoughness(roughness, anisotropy, at, ab);

    const float3 F = FresnelSchlick(saturate(dot(H, V)), F0);
    const float  D = DistributionGGXAniso(NdotH, H, T, B, at, ab);
    const float  Vis = VisibilitySmithGGXAniso(at, ab,
                                               dot(T, V), dot(B, V),
                                               dot(T, L), dot(B, L),
                                               NdotV, NdotL);

    outSpecular = D * Vis * F;
    outKD       = (1.0 - F) * (1.0 - metalness);
}

// ==== ShadingModel::ClearCoat: a second, thin specular layer on top ====
// Car paint, lacquered wood, varnished plastic, wet enamel: a smooth dielectric film over
// a rougher (or metallic) base. A SHARP reflection on top of a base that keeps its own
// blurrier highlight, which one GGX lobe cannot express.
// The model (Filament's clear-coat lobe):
//   * the coat is a fixed dielectric, IOR 1.5 -> F0 = 0.04, not authored (a coat whose F0
//     varies is a second material);
//   * D is GGX at the COAT's own roughness, so it can be mirror-smooth over a rough base;
//   * V is Kelemen's 0.25/(L.H)^2, the standard partner for a thin smooth layer;
//   * the base (diffuse AND specular) is attenuated once by (1 - Fc). A stricter model
//     squares it (the light crosses the coat twice), but Fc is evaluated at the half
//     vector, as in Filament and most real-time implementations, so squaring would only
//     darken grazing angles a few percent more for no gain in correctness.
// No second normal: the coat shares the shading normal, because RT4 has exactly two free
// channels and they hold strength + roughness. On a strongly normal-mapped base the
// coat's reflection follows the bumps: wrong but stable (also noted in ShadingModel.h).

// Kelemen's visibility term for a thin smooth layer. Already includes the
// 1/(4 NoL NoV) denominator, like the anisotropic Smith: do not divide again.
float VisibilityKelemen(float LdotH)
{
    return 0.25 / max(LdotH * LdotH, 1e-4);
}

// The coat's specular lobe, and its Fresnel in outFc so the caller attenuates the base
// with the SAME value (otherwise the layers do not conserve energy).
//   clearCoat           [0,1] strength; 0 makes the lobe and the attenuation vanish
//                       exactly, so the model degrades to Standard.
//   clearCoatRoughness  [0,1] perceptual, the coat's own.
float ClearCoatLobe(float3 N, float3 V, float3 L,
                    float clearCoat, float clearCoatRoughness,
                    out float outFc)
{
    const float3 H     = normalize(V + L);
    const float  NdotH = saturate(dot(N, H));
    const float  LdotH = saturate(dot(L, H));

    // Floor the coat roughness: a perfectly smooth GGX lobe is a delta that a punctual light
    // samples as 0 or a spike, which reads as temporal sparkle, not as a mirror.
    const float  rc = max(clearCoatRoughness, 0.045);

    // F0 = 0.04: the coat is always a dielectric at IOR 1.5.
    const float  fc = (0.04 + 0.96 * pow(saturate(1.0 - LdotH), 5.0))
                    * saturate(clearCoat);
    outFc = fc;

    return DistributionGGX(N, H, rc) * VisibilityKelemen(LdotH) * fc;
}

// ==== ShadingModel::TwoSidedFoliage ====
// Two-sided foliage transmission, a port of Unreal's TwoSidedBxDF:
//   wrapNoL = saturate((-N.L + wrap) / (1+wrap)^2)   light on the FAR side
//   scatter = D_GGX(0.36, saturate(-V.L))            forward-scatter lobe
//   result  = tint * wrapNoL * scatter
// The -N.L is the model: a leaf glows where light hits the side you are NOT looking at.
// The scatter lobe makes it view-dependent (looking into the sun through a leaf is much
// brighter than side-on), so it does not read as flat ambient fill.
// Returned UNSCALED by radiance: callers multiply by lightColor x attenuation x shadow.
// `tint` already carries the strength multiplier.
float3 FoliageTransmission(float3 N, float3 V, float3 L, float3 tint, float wrap)
{
    float w       = max(wrap, 0.0);
    float denom   = (1.0 + w) * (1.0 + w);
    float wrapNoL = saturate((-dot(N, L) + w) / max(denom, 1e-4));

    // UE's D_GGX(a2, NoH) with a2 = 0.6^2, hardcoded there too: the shape of the lobe, not a
    // surface property.
    const float a2 = 0.36;
    float  x = saturate(-dot(V, L));
    float  d = (x * a2 - x) * x + 1.0;
    float  scatter = a2 / (PI * d * d);

    return tint * (wrapNoL * scatter);
}

// ==== ShadingModel::Subsurface ====
// Two independent pieces, both cheap enough for a deferred uber-shader and both PER-LIGHT
// (a torch next to a face lights it like skin, not plastic):
//   1. WrapDiffuseNoL softens the DIFFUSE terminator so light seeps past it, standing in
//      for a screen-space diffusion blur (the engine has no such pass). Diffuse ONLY:
//      specular keeps the true N.L, or a skin highlight smears onto the shadow side.
//   2. SubsurfaceTransmission: light that entered the far side and exits toward the
//      viewer (Unreal's SubsurfaceBxDF transmission lobe). It goes into
//      DirectLighting::transmission, the bucket two-sided foliage uses, so
//      AccumulateDirect gates it with that light's shadow.

// Energy-normalised Lambert wrap (Penner). w == 0 returns EXACTLY saturate(NdotL), so
// wrap is a true opt-in. Takes the RAW dot, not a saturated one: the back hemisphere must
// stop being identically zero.
float WrapDiffuseNoL(float rawNdotL, float w)
{
    const float k = max(w, 0.0);
    return saturate((rawNdotL + k) / ((1.0 + k) * (1.0 + k)));
}

// Unreal's SubsurfaceBxDF transmission, unscaled by radiance: the caller multiplies by
// the light's colour x attenuation and hands it to AccumulateDirect, like
// FoliageTransmission.
//   opacity  how SOLID the material is: 1 = tight, normal-driven scatter (thick flesh);
//            0 = no normal dependence and a strong see-through in-scatter lobe (a thin
//            leaf, an ear against the sun).
//   ao       baked + SSAO occlusion; gates the back-scatter half, as in UE.
// Deliberate deviation from UE: UE's lerp(BackScatter, 1, InScatter) lets InScatter reach
// 3, so the result can exceed the incoming radiance. Saturating the factor caps it at the
// tint; the difference shows only in the last few degrees of a direct back-light.
float3 SubsurfaceTransmission(float3 N, float3 V, float3 L, float3 tint,
                              float opacity, float ao)
{
    const float3 H = normalize(V + L);
    const float  o = saturate(opacity);
    // See-through lobe: peaks looking straight INTO the light through the surface.
    // pow(..., 12) is UE's: the shape of the lobe, not a surface property.
    const float inScatter = pow(saturate(dot(L, -V)), 12.0) * lerp(3.0, 0.1, o);
    // Wrap-around half: opacity 0 has no normal dependence, opacity 1 full N.H shaping.
    // /(2 pi) keeps it energy-plausible.
    const float normalContribution = saturate(dot(N, H) * o + 1.0 - o);
    const float backScatter        = ao * normalContribution / (2.0 * PI);

    return tint * lerp(backScatter, 1.0, saturate(inScatter));
}

#endif // BRDF_HLSLI
