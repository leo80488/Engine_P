#pragma once

// ShadingModel - UE-style "what kind of surface is this?" enum. Every material picks exactly
// one; its shader fills SurfaceData and the deferred lighting pass applies the model's BRDF,
// finding the model through the pixel's material index (GBuffer RT1.a) and routing on the
// GBuffer stencil code (ShadingStencil::RefFor). The per-model decisions (route, ReSTIR, RT,
// menus) are rows of ShadingModelTable.h.
// The set is FIXED: a new model is a value appended here, a row in
// shaders/shading_model_table.hlsli and the lighting shaders' branches. Models are not
// asset-defined: the GBuffer layout is fixed and the lighting shader must know each model.

#include <cstdint>

enum class ShadingModel : uint8_t
{
    // Metallic / roughness microfacet PBR on the plain GBuffer (shaders/core/gbuffer_codec.hlsli):
    //   RT0 baseColor.rgb
    //   RT1 worldNormal.xyz / packed material index .a
    //   RT2 (roughness, metalness, AO, reflectance)
    //   RT3 motion vector
    //   RT4 per-model scratch (unused here)
    //   RT5 emissive seed in HdrSceneColor
    Standard = 0,

    // No lighting: baseColor goes straight to the scene color, emissive passes through. GBuffer
    // normals / surface are still written (TAA, motion) but the lighting pass early-outs.
    Unlit,

    // Two-layer specular: the base lobe plus a thin dielectric film on top - car paint, lacquered
    // wood, varnished plastic. It gives a SHARP environment reflection over a base that keeps its
    // own, usually blurrier, highlight, which one GGX lobe cannot express. Per light a
    // Kelemen-visibility GGX lobe at the coat's roughness, plus a second split-sum environment
    // fetch at the coat's mip; the base (diffuse and specular, direct and indirect) is attenuated
    // by the coat's Fresnel.
    // The coat's F0 is fixed at 0.04 (IOR 1.5) on purpose: a coat whose F0 varies is a different
    // material.
    // Parameters: `clearCoat` strength [0,1] and `clearCoatRoughness` [0,1], per pixel in GBuffer
    // RT4 .r/.g from SurfaceData::clearCoat / ::clearCoatRoughness. Strength 0 removes the lobe and
    // the attenuation EXACTLY, so an unauthored ClearCoat material is bit-identical to Standard.
    // No second coat normal (Unreal and Filament have one): RT4 has no channels left, so the coat
    // follows the bumps of a strongly normal-mapped base; author it over a smooth normal.
    // RT reflections and the DDGI cache shade a hit from rt_instance_data, which has no coat
    // channels: a coated surface reflects as a plain single-lobe material.
    ClearCoat,

    // Subsurface scattering - skin, wax, jade, thick leaves. Two terms, both PER LIGHT (the sun
    // and every clustered point / spot):
    //   * the diffuse terminator is WRAPPED past N.L = 0 by `subsurfaceWrap`, standing in for the
    //     screen-space diffusion blur the engine does not have; specular keeps the true N.L.
    //   * a transmission lobe (Unreal's SubsurfaceBxDF) lands in DirectLighting::transmission, so
    //     AccumulateDirect gates it with that light's own shadow.
    // Parameters are named MaterialComponent / MaterialGPUData fields with MaterialSchema rows
    // (subsurfaceColor / Strength / Wrap / Opacity), so a plain material authors them in the
    // Inspector. Per pixel in GBuffer RT4: .r = opacity (1 = solid), .g = a [0,1] scatter mask,
    // from SurfaceData::subsurfaceOpacity / ::subsurfaceMask so a custom hook can drive them from a
    // thickness map. The Toon model has no wrap or transmission term.
    Subsurface,

    // Anisotropic GGX - a specular STREAK along a tangent frame rather than a disc. Hair, brushed
    // metal, satin, vinyl.
    // Parameters: `anisotropy` [-1,1] and `anisotropyRotation` (turns about the shading normal).
    // Per pixel in GBuffer RT4: .r = the direction as an ANGLE in the shading normal's tangent
    // plane (one channel; shaders/shading_models.hlsli says why that is exact), .g = the signed
    // anisotropy. A custom hook overrides the direction through SurfaceData::anisotropyDirWS, which
    // is seeded with the mesh tangent rotated by the material knob.
    // SIGN follows Unreal: POSITIVE widens the lobe ALONG the tangent; hair and brushed metal, whose
    // tangent runs along the strand, want NEGATIVE.
    // Toon is a model of its own, so a material is anisotropic OR toon.
    // RT hits and the path tracer shade it isotropically: HybridReflIntersectHW shades a hit from
    // rt_instance_data, which has no tangent frame and no RT4, so a mirror shows a plain GGX disc.
    Anisotropic,

    // Water surface - Unreal's "Single Layer Water". A TRANSLUCENT SURFACE material that this model
    // redirects from the translucency pass into WaterSurfacePass, because water must write depth
    // and stamp the GBuffer for the screen-space reflection chain to trace off it.
    // A shading model, not a material domain: a domain says which pipeline compiles the material;
    // "this surface is water" says how it is shaded (UE has no Water domain either).
    // Never deferred-lit: the water permutation of custom_surface.hlsli emits a 4-MRT forward main;
    // the look comes from the material's hook, with scene depth / colour, the sky-probe reflection,
    // CSM, clustered lights and ripples from water_common.hlsli. Selected by
    // MaterialComponent::IsWaterShadingModel().
    Water,

    // Two-sided foliage - Unreal's TwoSidedFoliage. Light arriving on the FAR side of a thin leaf
    // or blade passes through and glows toward the viewer, so backlit foliage reads bright green,
    // not black.
    // Unlike Subsurface (wrap around the FRONT normal, per-pixel thickness in RT4) it lights the
    // back with -N and needs no thickness - a leaf is uniformly thin - so it costs no GBuffer
    // channel. The transmitted lobe is view-dependent (strongest looking into the light through the
    // leaf).
    // Set the material DOUBLE-SIDED too, or the back faces are culled; the Inspector turns it on with
    // the selection.
    // The lighting paths read only the named fields subsurfaceColor / subsurfaceStrength /
    // subsurfaceWrap shared with Subsurface. WriteMaterialSlot fills them:
    //   * custom-shader material with reflected b8 params -> the members it names
    //     `@engine foliageTint / foliageWrap / foliageStrength`, or, in a shader with no `@engine`
    //     annotation, the positional convention
    //       customParams[0].rgb = transmission tint (black = no transmission)
    //       customParams[1].x   = wrap    (0 = hard terminator, default 0.5)
    //       customParams[2].x   = strength          (default 1)
    //     An unset (<= 0) wrap is 0.5 and an unset strength 1. Two Tokyo foliage materials author
    //     their tint this way.
    //   * plain material -> the authored fields. Such a material has no customParams
    //     (customParamCount 0; the Inspector's Custom Parameters block needs shader reflection).
    TwoSidedFoliage,

    // Cel / anime shading - the ramp-lit stylized model the character pipeline is built on
    // (DanbaidongRP's PBRToonBase, ported). A shading model, not a second axis: it routes to the
    // NPR lighting permutation through ShadingStencil::RefFor, as Unreal's ShadingModelID does.
    // Ramp vs two colours is a parameter, not two models: with a per-material toon ramp
    // (MaterialGPUData::toonRampTex - the custom texture the shader names `@engine toonRamp`, else
    // its t3) the diffuse ramp is sampled, without one it lerps between two authored colours;
    // `toonCtx.active` and the RT reflection hit shader key on the same binding.
    // Do not key on the engine texture slot RAMPMAP: MaterialComponent's constructor seeds a default
    // ramp path into every material, so it carries no signal.
    Toon,

    Count
};

// The value is serialized raw (MaterialSerializer writes `customShadingModel = <uint>`): append
// new models before Count. Inserting one in the middle silently repoints every saved material.

// The model's display name for the Inspector and logs; never nullptr.
const char* ShadingModelName(ShadingModel m);
