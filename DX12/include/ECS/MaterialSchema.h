#pragma once

// MaterialSchema - declarative description of MaterialComponent's editable fields. Every consumer
// that iterates the material's named params (Inspector widget loop, .imat serialization) reads
// kMaterialPBRSchema instead of hard-coding the X-macro dispatch: the MATERIAL_PROPS X-macro in
// Components.h declares the C++ fields, the schema drives UI and serialization (the pattern
// custom-shader params use).
//
// To add an editable field:
//   1. Add the C++ member to MaterialComponent (or to MATERIAL_PROPS for the core PBR set).
//   2. Append a row to kMaterialPBRSchema with name / label / kind / offset / slider range /
//      visibility masks. The Inspector picks it up on the next build.

#include "ECS/Components.h"
#include "Graphics/ShadingModel.h"    // ShadingModel: for the modelMask column
#include "Resource/SystemHandles.h"   // Resource::MaterialGPUData: for gpuOffset

#include <cstddef>
#include <cstdint>

namespace MaterialSchema
{
    // Coarse lighting-family mask. The bit positions are the SHADERTYPE ordinals, but nothing authors
    // shaderType: the Inspector DERIVES this mask from the material's ShadingModel (Unlit -> SM_UNLIT,
    // Toon -> SM_ANY_NPR, everything else -> SM_ANY_PBR). The ordinals exist only because they are
    // serialized; see the X-macro in Components.h.
    constexpr uint32_t SM_PBR       = 1u << MaterialComponent::SHADERTYPE_PBR;
    constexpr uint32_t SM_PARALLAX  = 1u << MaterialComponent::SHADERTYPE_RESERVED_1;
    constexpr uint32_t SM_UNLIT     = 1u << MaterialComponent::SHADERTYPE_UNLIT;
    constexpr uint32_t SM_CLEARCOAT = 1u << MaterialComponent::SHADERTYPE_RESERVED_3;
    constexpr uint32_t SM_NPR_RAMP  = 1u << MaterialComponent::SHADERTYPE_NPR_RAMP;
    constexpr uint32_t SM_NPR_COLOR = 1u << MaterialComponent::SHADERTYPE_NPR_COLOR;

    constexpr uint32_t SM_ANY_PBR = SM_PBR | SM_PARALLAX | SM_CLEARCOAT;
    constexpr uint32_t SM_ANY_NPR = SM_NPR_RAMP | SM_NPR_COLOR;
    constexpr uint32_t SM_ANY     = SM_ANY_PBR | SM_UNLIT | SM_ANY_NPR;

    // A row no shader reads. Still serialized and uploaded (files keep their values, MaterialGPUData
    // keeps its layout), but the Inspector shows it on no material: a slider that changes nothing is
    // what this schema exists to prevent.
    constexpr uint32_t SM_HIDDEN  = 0u;

    // ---- Second visibility axis: the SHADING MODEL ----
    // The SM_* masks are COARSE (lit family / toon / unlit) and cannot express "only on a Subsurface
    // material": Subsurface, Anisotropic, TwoSidedFoliage and Standard share the SM_ANY_PBR bit. So a row
    // filters on either axis: visibleMask for the coarse family, modelMask for the specific model (bit N
    // = ShadingModel value N). MDL_ANY is the default: a row that does not set modelMask shows on every
    // model.
    constexpr uint32_t MDL_BIT(ShadingModel m)
    { return 1u << static_cast<uint32_t>(m); }

    constexpr uint32_t MDL_STANDARD   = MDL_BIT(ShadingModel::Standard);
    constexpr uint32_t MDL_UNLIT      = MDL_BIT(ShadingModel::Unlit);
    constexpr uint32_t MDL_SUBSURFACE = MDL_BIT(ShadingModel::Subsurface);
    constexpr uint32_t MDL_ANISO      = MDL_BIT(ShadingModel::Anisotropic);
    constexpr uint32_t MDL_FOLIAGE    = MDL_BIT(ShadingModel::TwoSidedFoliage);
    constexpr uint32_t MDL_TOON       = MDL_BIT(ShadingModel::Toon);
    constexpr uint32_t MDL_CLEARCOAT  = MDL_BIT(ShadingModel::ClearCoat);
    constexpr uint32_t MDL_ANY        = ~0u;

    // ---- Third visibility axis: the BLEND MODE ----
    // A row that only one compositing route reads (the transmission rows read nothing outside
    // BlendMode::Transmissive). Bit N = BlendMode value N; BLD_ANY is the default.
    constexpr uint32_t BLD_BIT(BlendMode m)
    { return 1u << static_cast<uint32_t>(m); }

    constexpr uint32_t BLD_TRANSMISSIVE = BLD_BIT(BlendMode::Transmissive);
    constexpr uint32_t BLD_ANY          = ~0u;

    enum class Kind : uint8_t
    {
        Float,           // single float: DragFloat / SliderFloat
        Color3,          // first 3 floats of XMFLOAT4: ColorEdit3 (no alpha)
        Color4,          // full XMFLOAT4: ColorEdit4
        FloatComponent,  // one component of an XMFLOAT4 (e.g. .w as strength): SliderFloat
    };

    // Sentinel for FieldDesc::gpuOffset: the field is NOT uploaded by the generic schema-driven scalar
    // loop in Renderer::WriteMaterialSlot. Used for colors (uploaded from the sRGB->linear cache, not the
    // raw field) and nprMinBrightness (gated to 0 for non-NPR materials). ~0 rather than 0, so a row that
    // forgets gpuOffset (-> 0) collides with roughnessMin's real offset and fails the duplicate-offset
    // check instead of silently corrupting that field.
    constexpr uint32_t kNoGpu = 0xFFFFFFFFu;

    struct FieldDesc
    {
        const char* name;        // stable identifier, used as .imat key + override key
        const char* label;       // human-readable Inspector label
        const char* group;       // optional grouping header in Inspector; nullptr = ungrouped
        Kind        kind;
        std::size_t offset;      // offsetof(MaterialComponent, member): CPU source
        uint8_t     component;   // for FloatComponent: 0=x 1=y 2=z 3=w. Ignored otherwise.
        float       min;
        float       max;
        uint32_t    visibleMask; // bitmask of SM_*: shown only when it intersects the family mask the Inspector derives
        // Byte offset of this field's float(s) within Resource::MaterialGPUData, computed via offsetof so it
        // can NEVER drift from the GPU struct. The generic WriteMaterialSlot loop copies CPU->GPU for every
        // Kind::Float row whose gpuOffset != kNoGpu; colors and gated fields use kNoGpu (filled specially).
        // Adding a scalar param = one schema row (with offsetof) + one material.hlsli field; the renderer
        // copy is automatic.
        uint32_t    gpuOffset;
        // Bitmask of MDL_*: the row shows only when (mask & (1 << customShadingModel)). Defaults to MDL_ANY;
        // only the Anisotropic / Subsurface / ClearCoat rows narrow it.
        //
        // SERIALIZATION IGNORES THIS, deliberately: the .imat writer and reader walk the schema unfiltered,
        // so switching a material's shading model back and forth never drops the values of the model it
        // left.
        uint32_t    modelMask = MDL_ANY;
        // Bitmask of BLD_*: the row shows only when (mask & (1 << userBlendMode)). Inspector only, like
        // modelMask: serialization keeps the values of a blend mode the material left.
        uint32_t    blendMask = BLD_ANY;
    };

    // Inspector and serializer both walk this in declaration order. Add new fields at the end of their
    // group; reordering existing rows changes .imat round-trip ordering (cosmetic only: values are
    // name-keyed). MGPU(f) -> byte offset of MaterialGPUData::f for the gpuOffset column; local to this
    // array, #undef'd right after it.
#define MGPU(f) static_cast<uint32_t>(offsetof(Resource::MaterialGPUData, f))
    inline constexpr FieldDesc kMaterialPBRSchema[] =
    {
        // ---- Base Color (every shader type) ----
        // PBR / NPR / Unlit all read baseColor as the albedo input. A color is uploaded from the
        // sRGB->linear cache, so gpuOffset = kNoGpu.
        { "baseColor",         "Base Color",       "Color",
          Kind::Color4,
          offsetof(MaterialComponent, baseColor),         0,
          0.f, 1.f,
          SM_ANY_PBR | SM_ANY_NPR | SM_UNLIT, kNoGpu },

        // ---- PBR scalars (default + parallax + clearcoat) ----
        { "roughnessMin",      "Roughness Min",    "PBR",
          Kind::Float,
          offsetof(MaterialComponent, roughnessMin),      0,
          0.f, 1.f,
          SM_ANY_PBR, MGPU(roughnessMin) },
        { "roughnessMax",      "Roughness Max",    "PBR",
          Kind::Float,
          offsetof(MaterialComponent, roughnessMax),      0,
          0.f, 1.f,
          SM_ANY_PBR, MGPU(roughnessMax) },
        { "metalnessMin",      "Metalness Min",    "PBR",
          Kind::Float,
          offsetof(MaterialComponent, metalnessMin),      0,
          0.f, 1.f,
          SM_ANY_PBR, MGPU(metalnessMin) },
        { "metalnessMax",      "Metalness Max",    "PBR",
          Kind::Float,
          offsetof(MaterialComponent, metalnessMax),      0,
          0.f, 1.f,
          SM_ANY_PBR, MGPU(metalnessMax) },
        { "reflectance",       "Reflectance",      "PBR",
          Kind::Float,
          offsetof(MaterialComponent, reflectance),       0,
          0.f, 1.f,
          SM_ANY_PBR, MGPU(reflectance) },
        { "saturation",        "Saturation",       "PBR",
          Kind::Float,
          offsetof(MaterialComponent, saturation),        0,
          0.f, 2.f,
          SM_ANY_PBR, MGPU(saturation) },

        // ---- Exposure protection ----
        // Toon AND Unlit take the full range: 1.0 = completely exposure-immune (the counter-scale exactly
        // cancels the tonemap exposure multiply, so the ACES input is 100% authored). Unlit qualifies because
        // it has no lighting term, so there is no physical exposure for it to opt out of. Every LIT model is
        // runtime-capped at 0.5: a PBR surface resists adaptation, it does not opt out. Deferred surfaces are
        // corrected by NPRStabilize.cs.hlsl; forward ones composite after NPRLitSnapshot and apply it
        // themselves in custom_surface.hlsli's unlit branch.
        { "exposureCompensation", "Exposure Protection",  "Lighting",
          Kind::Float,
          offsetof(MaterialComponent, exposureCompensation), 0,
          0.f, 1.f,
          SM_ANY, MGPU(exposureCompensation) },
        { "specularColor",     "Specular",         "PBR",
          Kind::Color4,
          offsetof(MaterialComponent, specularColor),     0,
          0.f, 1.f,
          SM_ANY_PBR, kNoGpu },

        // ---- Emissive (PBR + Unlit), color via the linear cache ----
        { "emissiveColor",     "Emissive Color",   "Emissive",
          Kind::Color3,
          offsetof(MaterialComponent, emissiveColor),     0,
          0.f, 1.f,
          SM_ANY_PBR | SM_UNLIT, kNoGpu },
        { "emissiveStrength",  "Emissive Strength","Emissive",
          Kind::FloatComponent,
          offsetof(MaterialComponent, emissiveColor),     3,
          0.f, 16.f,
          SM_ANY_PBR | SM_UNLIT, kNoGpu },

        // ---- Common (all engine shader types except custom) ----
        // NOTE: GPU member is named `normalStrength`, CPU member `normalMapStrength`.
        { "normalMapStrength", "Normal Strength",  "Common",
          Kind::Float,
          offsetof(MaterialComponent, normalMapStrength), 0,
          0.f, 4.f,
          SM_ANY_PBR | SM_ANY_NPR, MGPU(normalStrength) },
        { "alphaRef",          "Alpha Ref",        "Common",
          Kind::Float,
          offsetof(MaterialComponent, alphaRef),          0,
          0.f, 1.f,
          SM_ANY_PBR | SM_ANY_NPR | SM_UNLIT, MGPU(alphaRef) },

        // ---- NPR shared (NPR_RAMP + NPR_COLOR) ----
        // nprMinBrightness is gated to 0 for non-NPR materials (it doubles as the is-NPR flag in normal.w),
        // so it is filled specially -> kNoGpu.
        { "nprMinBrightness",  "Min Brightness",   "NPR",
          Kind::Float,
          offsetof(MaterialComponent, nprMinBrightness),  0,
          0.f, 1.f,
          SM_ANY_NPR, kNoGpu },
        { "nprShadowThreshold","Shadow Threshold", "NPR",
          Kind::Float,
          offsetof(MaterialComponent, nprShadowThreshold),0,
          0.f, 1.f,
          SM_ANY_NPR, MGPU(nprShadowThreshold) },
        { "nprShadowSmooth",   "Shadow Smooth",    "NPR",
          Kind::Float,
          offsetof(MaterialComponent, nprShadowSmooth),   0,
          0.001f, 0.5f,
          SM_ANY_NPR, MGPU(nprShadowSmooth) },
        // SM_HIDDEN: a fresnel rim exponent nothing reads; the toon rim reads nprRimWidth and
        // nprRimStrength.
        { "nprRimPower",       "Rim Power",        "NPR",
          Kind::Float,
          offsetof(MaterialComponent, nprRimPower),       0,
          1.f, 16.f,
          SM_HIDDEN, MGPU(nprRimPower) },
        { "nprRimStrength",    "Rim Strength",     "NPR",
          Kind::Float,
          offsetof(MaterialComponent, nprRimStrength),    0,
          0.f, 2.f,
          SM_ANY_NPR, MGPU(nprRimStrength) },
        // SM_HIDDEN: a ramp-tint blend nothing reads.
        { "nprRampBlend",      "Ramp Blend",       "NPR",
          Kind::Float,
          offsetof(MaterialComponent, nprRampBlend),      0,
          0.f, 1.f,
          SM_HIDDEN, MGPU(nprRampBlend) },
        // Relative ceiling, x albedo luminance. The range starts at 0.25 (not 0): the enable is the
        // NPR_BRIGHTNESS_CLAMP flag, so 0 need not mean "off" and the slider is monotonic across its range.
        { "nprBrightnessClamp","Brightness Clamp x Albedo", "NPR",
          Kind::Float,
          offsetof(MaterialComponent, nprBrightnessClamp),0,
          0.25f, 3.f,
          SM_ANY_NPR, MGPU(nprBrightnessClamp) },
        { "nprMaxBrightness",  "Max Brightness",   "NPR",
          Kind::Float,
          offsetof(MaterialComponent, nprMaxBrightness),  0,
          0.f, 8.f,
          SM_ANY_NPR, MGPU(nprMaxBrightness) },

        // ---- DanbaidongRP toon alignment (toon-override block in Lighting.ps) ----
        { "nprShadowStrength", "Shadow Strength",  "NPR",
          Kind::Float,
          offsetof(MaterialComponent, nprShadowStrength), 0,
          0.f, 1.f,
          SM_ANY_NPR, MGPU(nprShadowStrength) },
        // Half-width of the stylized cast-shadow edge, consumed by ToonVisibility() in Lighting.ps.hlsl.
        // Takes effect only when the material sets MaterialComponent::NPR_TOON_SHADOW_EDGE: NPR .imat files
        // already carry non-zero values here, so gating on "value != 0" would switch the feature on
        // everywhere at once. 0 = passthrough (the raw PCF penumbra), so the slider spans "off" to "hard
        // toon edge".
        { "nprSceneShadowSmooth","Scene Shadow Sharp","NPR",
          Kind::Float,
          offsetof(MaterialComponent, nprSceneShadowSmooth), 0,
          0.f, 1.f,
          SM_ANY_NPR, MGPU(nprSceneShadowSmooth) },
        { "nprMainLightLerp",  "Self Light Lerp",  "NPR",
          Kind::Float,
          offsetof(MaterialComponent, nprMainLightLerp),  0,
          0.f, 1.f,
          SM_ANY_NPR, MGPU(nprMainLightLerp) },
        { "nprDirectOcclusion","Direct Occlusion", "NPR",
          Kind::Float,
          offsetof(MaterialComponent, nprDirectOcclusion),0,
          0.f, 1.f,
          SM_ANY_NPR, MGPU(nprDirectOcclusion) },
        { "nprRimWidth",       "Rim Width",        "NPR",
          Kind::Float,
          offsetof(MaterialComponent, nprRimWidth),       0,
          0.f, 10.f,
          SM_ANY_NPR, MGPU(nprRimWidth) },

        // ---- NPR energy contract ----
        { "nprDirectLightScale", "Direct Light Scale", "NPR Energy",
          Kind::Float,
          offsetof(MaterialComponent, nprDirectLightScale), 0,
          0.f, 4.f,
          SM_ANY_NPR, MGPU(nprDirectLightScale) },
        { "nprAdditionalLightScale", "Additional Light Scale", "NPR Energy",
          Kind::Float,
          offsetof(MaterialComponent, nprAdditionalLightScale), 0,
          0.f, 2.f,
          SM_ANY_NPR, MGPU(nprAdditionalLightScale) },
        { "nprAmbientLightScale", "Ambient / IBL Scale", "NPR Energy",
          Kind::Float,
          offsetof(MaterialComponent, nprAmbientLightScale), 0,
          0.f, 2.f,
          SM_ANY_NPR, MGPU(nprAmbientLightScale) },
        { "nprSpecularLightScale", "Specular Scale", "NPR Energy",
          Kind::Float,
          offsetof(MaterialComponent, nprSpecularLightScale), 0,
          0.f, 2.f,
          SM_ANY_NPR, MGPU(nprSpecularLightScale) },
        { "nprSceneLightColorWeight", "Scene Light Color Weight", "NPR Energy",
          Kind::Float,
          offsetof(MaterialComponent, nprSceneLightColorWeight), 0,
          0.f, 1.f,
          SM_ANY_NPR, MGPU(nprSceneLightColorWeight) },
        { "nprSceneLightIntensityWeight", "Scene Light Intensity Weight", "NPR Energy",
          Kind::Float,
          offsetof(MaterialComponent, nprSceneLightIntensityWeight), 0,
          0.f, 1.f,
          SM_ANY_NPR, MGPU(nprSceneLightIntensityWeight) },
        { "nprReferenceLightLuminance", "Reference Light Luminance", "NPR Energy",
          Kind::Float,
          offsetof(MaterialComponent, nprReferenceLightLuminance), 0,
          0.01f, 16.f,
          SM_ANY_NPR, MGPU(nprReferenceLightLuminance) },
        { "nprExposureBiasEV", "Exposure Bias EV", "NPR Stability",
          Kind::Float,
          offsetof(MaterialComponent, nprExposureBiasEV), 0,
          -2.f, 2.f,
          SM_ANY_NPR, MGPU(nprExposureBiasEV) },
        { "nprShoulderStart", "Shoulder Start", "NPR Stability",
          Kind::Float,
          offsetof(MaterialComponent, nprShoulderStart), 0,
          0.1f, 8.f,
          SM_ANY_NPR, MGPU(nprShoulderStart) },
        { "nprShoulderRange", "Shoulder Range", "NPR Stability",
          Kind::Float,
          offsetof(MaterialComponent, nprShoulderRange), 0,
          0.05f, 8.f,
          SM_ANY_NPR, MGPU(nprShoulderRange) },
        { "nprShoulderStrength", "Shoulder Strength", "NPR Stability",
          Kind::Float,
          offsetof(MaterialComponent, nprShoulderStrength), 0,
          0.f, 1.f,
          SM_ANY_NPR, MGPU(nprShoulderStrength) },

        // ---- NPR specular ramp (toon highlight; opt-in via Spec Ramp Blend) ----
        { "nprSpecThreshold",  "Spec Threshold",   "NPR Specular",
          Kind::Float,
          offsetof(MaterialComponent, nprSpecThreshold),  0,
          0.f, 2.f,
          SM_ANY_NPR, MGPU(nprSpecThreshold) },
        { "nprSpecSmooth",     "Spec Smooth",      "NPR Specular",
          Kind::Float,
          offsetof(MaterialComponent, nprSpecSmooth),     0,
          0.001f, 0.5f,
          SM_ANY_NPR, MGPU(nprSpecSmooth) },
        { "nprSpecStrength",   "Spec Strength",    "NPR Specular",
          Kind::Float,
          offsetof(MaterialComponent, nprSpecStrength),   0,
          0.f, 4.f,
          SM_ANY_NPR, MGPU(nprSpecStrength) },
        { "nprSpecRampBlend",  "Spec Ramp Blend",  "NPR Specular",
          Kind::Float,
          offsetof(MaterialComponent, nprSpecRampBlend),  0,
          0.f, 1.f,
          SM_ANY_NPR, MGPU(nprSpecRampBlend) },

        // ---- Skin-layer texture-ramp blend weights: SM_HIDDEN ----
        // Nothing reads them; the rows keep the serialized values and the GPU layout.
        { "nprMidWeight",      "Mid Weight",       "NPR Ramp",
          Kind::Float,
          offsetof(MaterialComponent, nprMidWeight),      0,
          0.f, 1.f,
          SM_HIDDEN, MGPU(nprMidWeight) },
        { "nprVeinWeight",     "Vein Weight",      "NPR Ramp",
          Kind::Float,
          offsetof(MaterialComponent, nprVeinWeight),     0,
          0.f, 1.f,
          SM_HIDDEN, MGPU(nprVeinWeight) },
        { "nprSSSWeight",      "SSS Weight",       "NPR Ramp",
          Kind::Float,
          offsetof(MaterialComponent, nprSSSWeight),      0,
          0.f, 1.f,
          SM_HIDDEN, MGPU(nprSSSWeight) },

        // ---- NPR Color only (texture-less two-color ramp), colors via the cache ----
        { "nprDiffuseRampColor","Diffuse Ramp",    "NPR Color",
          Kind::Color3,
          offsetof(MaterialComponent, nprDiffuseRampColor),0,
          0.f, 1.f,
          SM_NPR_COLOR, kNoGpu },
        { "nprShadowRampColor","Shadow Ramp",      "NPR Color",
          Kind::Color3,
          offsetof(MaterialComponent, nprShadowRampColor),0,
          0.f, 1.f,
          SM_NPR_COLOR, kNoGpu },

        // ---- ShadingModel::Anisotropic ----
        // visibleMask stays SM_ANY; modelMask (MDL_ANISO) keeps these two rows off every other material.
        // Toon is a shading model of its own, so "anisotropic AND toon" cannot be authored.
        { "anisotropy",        "Anisotropy",       "Anisotropic",
          Kind::Float,
          offsetof(MaterialComponent, anisotropy),        0,
          -1.f, 1.f,
          SM_ANY, MGPU(anisotropy),         MDL_ANISO },
        { "anisotropyRotation","Rotation (turns)", "Anisotropic",
          Kind::Float,
          offsetof(MaterialComponent, anisotropyRotation),0,
          0.f, 1.f,
          SM_ANY, MGPU(anisotropyRotation), MDL_ANISO },

        // ---- ShadingModel::Subsurface / TwoSidedFoliage ----
        // Shared rows on purpose: both models are "light that came through the surface" and take the same
        // tint / wrap / strength, and on a non-custom material these rows are foliage's only authoring place
        // (the customParams[0..2] convention exists only on custom shaders). Foliage does not read
        // subsurfaceOpacity (a leaf is uniformly thin, which is why it costs no GBuffer channel), so that row
        // is Subsurface-only.
        { "subsurfaceColor",   "Subsurface Color", "Subsurface",
          Kind::Color3,
          offsetof(MaterialComponent, subsurfaceColor),   0,
          0.f, 1.f,
          SM_ANY, kNoGpu,                   MDL_SUBSURFACE | MDL_FOLIAGE },
        { "subsurfaceStrength","Scatter Strength", "Subsurface",
          Kind::Float,
          offsetof(MaterialComponent, subsurfaceStrength),0,
          0.f, 4.f,
          SM_ANY, MGPU(subsurfaceStrength), MDL_SUBSURFACE | MDL_FOLIAGE },
        { "subsurfaceWrap",    "Wrap",             "Subsurface",
          Kind::Float,
          offsetof(MaterialComponent, subsurfaceWrap),    0,
          0.f, 1.f,
          SM_ANY, MGPU(subsurfaceWrap),     MDL_SUBSURFACE | MDL_FOLIAGE },
        { "subsurfaceOpacity", "Opacity (thickness)", "Subsurface",
          Kind::Float,
          offsetof(MaterialComponent, subsurfaceOpacity), 0,
          0.f, 1.f,
          SM_ANY, MGPU(subsurfaceOpacity),  MDL_SUBSURFACE },

        // ---- ShadingModel::ClearCoat ----
        { "clearCoat",         "Clear Coat",       "Clear Coat",
          Kind::Float,
          offsetof(MaterialComponent, clearCoat),         0,
          0.f, 1.f,
          SM_ANY, MGPU(clearCoat),          MDL_CLEARCOAT },
        { "clearCoatRoughness","Coat Roughness",   "Clear Coat",
          Kind::Float,
          offsetof(MaterialComponent, clearCoatRoughness),0,
          0.f, 1.f,
          SM_ANY, MGPU(clearCoatRoughness), MDL_CLEARCOAT },

        // ---- BlendMode::Transmissive ----
        // Any lit model (the reflection lobe stays the model's); Unlit and Toon have no dielectric base.
        { "transmission",      "Transmission",     "Transmission",
          Kind::Float,
          offsetof(MaterialComponent, transmission),      0,
          0.f, 1.f,
          SM_ANY_PBR, MGPU(transmission),   MDL_ANY, BLD_TRANSMISSIVE },
        { "ior",               "IOR",              "Transmission",
          Kind::Float,
          offsetof(MaterialComponent, ior),               0,
          1.f, 3.f,
          SM_ANY_PBR, MGPU(ior),            MDL_ANY, BLD_TRANSMISSIVE },
        { "thickness",         "Thickness",        "Transmission",
          Kind::Float,
          offsetof(MaterialComponent, thickness),         0,
          0.f, 10.f,
          SM_ANY_PBR, MGPU(thickness),      MDL_ANY, BLD_TRANSMISSIVE },
        { "attenuationColor",  "Attenuation Color", "Transmission",
          Kind::Color3,
          offsetof(MaterialComponent, attenuationColor),  0,
          0.f, 1.f,
          SM_ANY_PBR, kNoGpu,               MDL_ANY, BLD_TRANSMISSIVE },
        { "attenuationDistance","Attenuation Distance", "Transmission",
          Kind::Float,
          offsetof(MaterialComponent, attenuationDistance),0,
          0.f, 100.f,
          SM_ANY_PBR, MGPU(attenuationDistance), MDL_ANY, BLD_TRANSMISSIVE },
    };
#undef MGPU

    constexpr std::size_t kFieldCount = sizeof(kMaterialPBRSchema) / sizeof(kMaterialPBRSchema[0]);

    // Compile-time guard for the gpuOffset column: every non-kNoGpu offset must be 4-byte aligned,
    // in-bounds of MaterialGPUData, and unique. Catches two rows pointing at the same GPU member or an
    // offset running off the struct at build time instead of silently corrupting a field.
    constexpr bool ValidateGpuOffsets()
    {
        for (std::size_t i = 0; i < kFieldCount; ++i)
        {
            const uint32_t a = kMaterialPBRSchema[i].gpuOffset;
            if (a == kNoGpu) continue;
            if (a % sizeof(float) != 0) return false;
            if (a + sizeof(float) > sizeof(Resource::MaterialGPUData)) return false;
            for (std::size_t j = i + 1; j < kFieldCount; ++j)
            {
                const uint32_t b = kMaterialPBRSchema[j].gpuOffset;
                if (b != kNoGpu && b == a) return false;
            }
        }
        return true;
    }
    static_assert(ValidateGpuOffsets(),
                  "MaterialSchema gpuOffset table has a misaligned, out-of-bounds, or duplicate entry");

    // Compile-time guard for the CPU offset/kind pairing: a row's offset + the bytes its Kind reads (1
    // float for Float, a full float4 for Color/FloatComponent) must stay in-bounds of MaterialComponent.
    // Catches a Kind/offset mismatch that would read/write past a scalar member (e.g. a Color row
    // pointing at a trailing float).
    constexpr bool ValidateCpuOffsets()
    {
        for (std::size_t i = 0; i < kFieldCount; ++i)
        {
            const auto& f = kMaterialPBRSchema[i];
            const std::size_t bytes = (f.kind == Kind::Float) ? sizeof(float)
                                                              : 4 * sizeof(float);
            if (f.offset + bytes > sizeof(MaterialComponent)) return false;
        }
        return true;
    }
    static_assert(ValidateCpuOffsets(),
                  "MaterialSchema CPU offset/Kind pairing reads past MaterialComponent");

    // Pointer to the first float of field @p f within @p mat; the caller reads as many floats as the
    // field's Kind needs.
    inline float* FieldPtr(MaterialComponent& mat, const FieldDesc& f)
    {
        return reinterpret_cast<float*>(reinterpret_cast<char*>(&mat) + f.offset);
    }
    inline const float* FieldPtr(const MaterialComponent& mat, const FieldDesc& f)
    {
        return reinterpret_cast<const float*>(reinterpret_cast<const char*>(&mat) + f.offset);
    }
}
