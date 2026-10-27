#pragma once

// Core ECS components: MaterialComponent and MeshComponent (Wicked Engine-style, on DirectXMath +
// std), plus the render-pipeline components the Renderer reads (bounds, mesh handle, lights,
// decals, cameras).

#include "ECS/ECS.h"
#include "ECS/GuidComponent.h"
#include "Graphics/ShadingModel.h"
#include "Resource/MaterialDomain.h"

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>
#include <string>
#include <string_view>   // TransparentStringHash (heterogeneous custom-bag lookups)
#include <DirectXMath.h>

// Keep the Windows min / max macros from breaking std::min / std::max.
#ifndef NOMINMAX
#define NOMINMAX
#endif

// ==== Helper types ====

struct AABB
{
    DirectX::XMFLOAT3 _min = { 0, 0, 0 };
    DirectX::XMFLOAT3 _max = { 0, 0, 0 };
};

enum class BlendMode : uint32_t
{
    Opaque,
    Alpha,          // SRC_ALPHA / INV_SRC_ALPHA, forward transparent pass
    Additive,       // ONE / ONE, forward transparent pass
    Premultiplied,  // ONE / INV_SRC_ALPHA, forward transparent pass
    Multiply,       // DST_COLOR / INV_SRC_ALPHA, forward transparent pass
    // Unreal-style masked: deferred GBuffer path + ALPHA_TEST clip of
    // baseColor.a x BASECOLORMAP.a x OPACITYMAP.r against alphaRef ("Mask Clip Value").
    // Blend modes are serialized as integers: append new ones, never reorder.
    Masked,
    // Stochastic (screen-door) transparency on the deferred route: GBuffer + ALPHA_TEST permutation
    // with the Masked coverage, but the threshold is a per-pixel, per-frame dither instead of
    // alphaRef, so TAA / DLSS integrate it into apparent transparency. The surface gets everything
    // the deferred pipeline does (RT reflections, SSR, DDGI, RTAO, clustered lights, shadows), which
    // the forward route cannot. Needs temporal AA (TAA / DLSS / FSR2); under FXAA it is a visible
    // screen-door. Shadow passes use a static dither, the RT scene treats it like Alpha (an
    // opacity-attenuating occluder), probe bakes treat it as opaque.
    // Scope: a fade / LOD-crossfade / foliage coverage tool, not for thin glass. Coverage == opacity,
    // so a 0.39-alpha pane keeps only 39% of the highlight and reflection energy, and TAA's
    // disocclusion boost rejects the alternating pane / background depth, so it never fully
    // integrates. Glass stays on forward Alpha and gets traced reflections through
    // features.transparentReflections.
    Dithered,
    // A dielectric that light crosses (glass, liquids, gems): forward, like Alpha, but opacity
    // stays coverage and the MaterialComponent transmission fields say what crosses a covered
    // pixel. The background behind it is refracted and tinted before the surface's own lighting
    // is added on top.
    Transmissive,
    Count
};

// True for the forward-transparent modes. Masked and Opaque both draw in the deferred
// GBuffer pass: use this instead of `!= BlendMode::Opaque` when deciding "does this draw skip
// the opaque path" (cull exemptions, forward routing), or Masked silently inherits transparent
// semantics.
constexpr bool IsTransparentBlendMode(BlendMode m)
{
    return m == BlendMode::Alpha || m == BlendMode::Additive ||
           m == BlendMode::Premultiplied || m == BlendMode::Multiply ||
           m == BlendMode::Transmissive;
}
constexpr bool IsTransmissiveBlendMode(BlendMode m) { return m == BlendMode::Transmissive; }
// Dithered draws on the OPAQUE route (see the enum) but is translucent by
// intent: the RT alpha record treats it as an opacity-attenuating occluder like
// Alpha, and anything that reasons about "what the artist meant" should too.
constexpr bool IsDitheredBlendMode(BlendMode m) { return m == BlendMode::Dithered; }

// MaterialGPUData.materialFlags bit that WriteMaterialSlot sets for Dithered. Not
// a MaterialComponent::flags bit (it is derived from userBlendMode, which
// HashMatParams already mixes), so it lives outside MATERIAL_GPU_FLAGS. Mirror:
// MAT_FLAG_DITHERED_COVERAGE in shaders/material.hlsli.
inline constexpr uint32_t kMatGpuFlagDitheredCoverage = 1u << 6;

// MaterialGPUData.materialFlags bit: this material slot has a FRAME RECORD - the deferred toon
// reads the face head frame from the per-frame record buffer (LightCB.faceFrameIdx) instead of
// customParams[0..1]. Set only by the main emit, for a PER_FRAME_PARAMS material, through
// WriteMaterialSlot's frameRecord argument - never by the preview or the path tracer's table,
// which write no records. The slot key carries the material's identity there, so the bit cannot
// be shared with a record that lacks it. Mirror: MAT_FLAG_FRAME_RECORD in shaders/material.hlsli.
inline constexpr uint32_t kMatGpuFlagFrameRecord = 1u << 8;

// MaterialGPUData.materialFlags bit: the material's custom pixel shader reads the frame's time
// (ShaderReflect::Reflection::readsFrameTime) - it animates on its own, a scrolling texture, a
// pulsing emissive. The path tracer restarts where such a material is on screen while the world
// clock runs (PathTracePass's motion probe). Derived from the shader, whose identity the slot key
// carries. Mirror: MAT_FLAG_READS_TIME in shaders/material.hlsli.
inline constexpr uint32_t kMatGpuFlagReadsTime = 1u << 9;

// ---- alphaRef: one authored field, a meaning per blend mode ----
//   * Opaque and the four forward modes: an opt-in alpha-test threshold. The default 1.0 is "no
//     alpha test"; at or below kAlphaRefNoClipThreshold the ALPHA_TEST permutation turns on
//     (IsAlphaTestEnabled).
//   * Masked: Unreal's "Opacity Mask Clip Value". Masked always clips, so the shared default 1.0
//     would discard every texel that is not perfectly opaque; an unset value clips at
//     kMaskClipDefault.
//   * Dithered: ignored. The threshold is the per-pixel dither, so the published value is 1.0
//     ("no clip"); a leftover authored threshold would otherwise punch holes into probe bakes
//     (ProbeCapture.ps clips on the published alphaRef and has no dither branch).
// MaskClipValueFor() reconciles them where the value is PUBLISHED: WriteMaterialSlot
// (MaterialGPUData.alphaRef, read by every raster clip site) and DDGISceneAS (the RT alpha
// record). Do not rewrite the authored field to make Masked usable: an Opaque material that only
// visited Masked would come back alpha-tested, and a scene save would persist that as a
// MaterialOverride.
constexpr float kAlphaRefNoClipThreshold = 1.0f - 1.0f / 256.0f;
constexpr float kMaskClipDefault         = 0.5f;   // Unreal's familiar default
constexpr float kAlphaRefNoClip           = 1.0f;
constexpr float MaskClipValueFor(BlendMode mode, float alphaRef)
{
    if (mode == BlendMode::Dithered) return kAlphaRefNoClip;
    return (mode == BlendMode::Masked && alphaRef > kAlphaRefNoClipThreshold)
         ? kMaskClipDefault : alphaRef;
}
static_assert(MaskClipValueFor(BlendMode::Masked, 1.0f) == kMaskClipDefault,
              "an unset (1.0) Alpha Ref on a Masked material clips at the default");
static_assert(MaskClipValueFor(BlendMode::Masked, 0.3f) == 0.3f,
              "an authored Masked clip value is published as-is");
static_assert(MaskClipValueFor(BlendMode::Opaque, 1.0f) == 1.0f,
              "Opaque never inherits the Masked default (that was the leak)");
static_assert(MaskClipValueFor(BlendMode::Alpha, 0.5f) == 0.5f,
              "forward modes keep their authored alpha-test threshold");
static_assert(MaskClipValueFor(BlendMode::Dithered, 0.5f) == kAlphaRefNoClip,
              "Dithered publishes no clip threshold: the dither IS the threshold");

enum class StencilRef : uint32_t
{
    Default = 0,
    Custom
};

enum class ShadingRate : uint8_t
{
    Rate_1x1,
    Rate_1x2,
    Rate_2x1,
    Rate_2x2
};

// Per-material shadow-caster cull mode. Default = back-cull + bias. Front-cull is the closed-mesh
// shortcut: only inner faces reach the shadow map, so self-shadow acne cannot happen whatever the
// bias. None is for single-sided geometry (hair cards, eyelash planes, cloth, leaves) that would
// lose its shadow under either cull. Alpha-test and mirrored draws always render with
// CullMode::NONE (ShadowPass), so this does not affect them.
enum class ShadowCullMode : uint8_t
{
    Default = 0,  // back-cull
    Front   = 1,  // front-cull: closed-mesh characters / organic solids
    None    = 2,  // no cull: single-sided geometry
};

// X(enum_name, label, default_path): the TEXTURESLOT enum, the constructor's default paths and
// the inspector labels; .imat keys and overrides iterate it too. Roughness / metalness / AO share
// SURFACEMAP (R = AO, G = roughness, B = metalness; see GBuffer.ps). The enum order is a GPU
// contract: the MAT_TEX_* indices in shaders/material.hlsli must match the slot positions.
#define MATERIAL_TEXTURE_SLOTS(X) \
    X(BASECOLORMAP, "Base Color",    "engine/textures/Default_BaseColor.itex") \
    X(NORMALMAP,    "Normal Map",    "engine/textures/Default_Normal.itex")    \
    X(SURFACEMAP,   "Surface (ARM)", "engine/textures/Default_ARM.itex") \
    X(EMISSIVEMAP,  "Emissive Map",  "engine/textures/Default_Emissive.itex")  \
    X(RAMPMAP,      "Ramp Texture",  "engine/textures/T_R2T1LinnaeaMd10011_Skin.itex")  \
    X(OPACITYMAP,   "Opacity Mask",  "")  /* .r multiplies the Masked/alpha-test clip; empty = no mask (1.0). MAT_TEX_OPACITYMAP=5 in material.hlsli */ \

// X(enum_name, label): the SHADERTYPE enum.
// Ordinals are serialized: rename, never remove or reorder. Every .imat stores `shaderType = N` as
// a bare integer and MaterialSchema derives its SM_* bits as `1u << SHADERTYPE_*`, so a renumber
// moves the C++ but not the files: the wrong Inspector rows appear, and a value past the enum
// shifts `1u << shaderType` by an unbounded amount.
// SHADERTYPE_RESERVED_1 / _3 hold their ordinals so nothing after them moves: parallax occlusion is
// a UV effect (author POM in a custom shader's EvaluateMaterial), and clear coat is
// ShadingModel::ClearCoat.
#define MATERIAL_SHADER_TYPES(X) \
    X(SHADERTYPE_PBR,                         "PBR")            \
    X(SHADERTYPE_RESERVED_1,                   "(retired)")      \
    X(SHADERTYPE_UNLIT,                        "Unlit")          \
    X(SHADERTYPE_RESERVED_3,                   "(retired)")     \
    X(SHADERTYPE_NPR_RAMP,                    "NPR Ramp")       \
    X(SHADERTYPE_NPR_COLOR,                   "NPR Color")

// X(type, name, label, min, max): declares the core PBR fields (defaults in the constructor). The
// inspector and .imat serialization read kMaterialPBRSchema (ECS/MaterialSchema.h), so a new row
// also needs a schema row.
#define MATERIAL_PROPS(X) \
    X(float,             roughnessMax,         "Roughness Max",   0.f, 1.f) \
    X(float,             roughnessMin,         "Roughness Min",   0.f, 1.f) \
    X(float,             metalnessMax,         "Metalness Max",   0.f, 1.f) \
    X(float,             metalnessMin,         "Metalness Min",   0.f, 1.f) \
    X(float,             reflectance,       "Reflectance",     0.f, 1.f) \
    X(float,             normalMapStrength, "Normal Strength", 0.f, 4.f) \
    X(float,             saturation,        "Saturation",      0.f, 2.f) \
    X(float,             alphaRef,          "Alpha Ref",       0.f, 1.f) \
    X(DirectX::XMFLOAT4, baseColor,         "Base Color",      0.f, 1.f) \
    X(DirectX::XMFLOAT4, specularColor,     "Specular",        0.f, 1.f) \
    X(DirectX::XMFLOAT4, emissiveColor,     "Emissive",        0.f, 1.f)

// ==== MaterialComponent (alignas(32): PBR material and texture slots) ====

struct alignas(32) MaterialComponent
{
    enum FLAGS : uint32_t
    {
        EMPTY = 0,
        DIRTY = 1 << 0,
        CAST_SHADOW = 1 << 1,
        USE_VERTEXCOLORS = 1 << 5,
        SPECULAR_GLOSSINESS_WORKFLOW = 1 << 6,
        OCCLUSION_PRIMARY = 1 << 7,
        OCCLUSION_SECONDARY = 1 << 8,
        USE_WIND = 1 << 9,
        DISABLE_RECEIVE_SHADOW = 1 << 10,
        DOUBLE_SIDED = 1 << 11,
        OUTLINE = 1 << 12,
        OUTLINE_SCREENSPACE = 1 << 13,  // enable screen-space edge detection outline
        // Skip XeGTAO on this material's pixels (characters / skin, so they neither cast spurious AO
        // nor accumulate temporal noise).
        EXCLUDE_FROM_SSAO = 1 << 14,
        // Opt into the relative NPR brightness ceiling (nprBrightnessClamp). Off by default: on at 1.0
        // it pins every NPR pixel to its own albedo luminance, so the material cannot brighten under
        // stronger light and rim / spec die on dark textures.
        NPR_BRIGHTNESS_CLAMP = 1 << 15,
        // GBuffer writes RT4 extra.b = 1 (hair mask) so FaceOverlayPass can redraw brows / lashes over
        // this material's pixels: a plain hair material joins the see-through-brows effect without a
        // custom surface shader (same RT4 contract as asset/Shaders/Hair.ps.hlsl).
        HAIR_MASK = 1 << 16,
        // Opt into the stylized cast-shadow edge (ToonVisibility, sharpness = nprSceneShadowSmooth).
        // A flag rather than keying on the float: .imat is a full dump (MaterialSerializer writes every
        // Kind::Float row when there is no base), so every NPR material on disk already stores a
        // non-zero nprSceneShadowSmooth; keying on it would harden every cast-shadow terminator at once,
        // with no opt-out.
        NPR_TOON_SHADOW_EDGE = 1 << 20,
        ASSET_DIRTY = 1 << 17,  // runtime instance differs from on-disk .imat (unsaved)
        INTERNAL = 1 << 18,
        GPU_LINEAR_CACHE_DIRTY = 1u << 19, // renderer-owned: sRGB colors changed, linear cache needs refresh
        SOURCE_OUTLINE_EXEMPT = 1u << 22, // visible surfaces bypass source depth/normal outlines
        // An engine system writes frameParams every frame from live scene state (FaceShadingSystem's
        // head-bone axes). frameParams stay out of matHash, so the content is stable, but the material
        // is per entity:
        //   * its persistent slot is keyed by the component's identity (MaterialBindingKey::identity),
        //     because the slot carries this frame's values: the b8 CBV (repacked every frame on the
        //     per-frame ring, frameParams over customParams by name) and the frame record the deferred
        //     toon reads;
        //   * it never takes a surface-cache page (SurfaceAtlasEligible): a page is baked once, and a
        //     shader that shades with a frame param (EndfieldFace bends its normals toward HeadCenter)
        //     would freeze it. It keeps the alias proxy.
        // Bit 21, not 20: NPR_TOON_SHADOW_EDGE owns 1 << 20 (declared above, out of numeric order). Do
        // not reuse bits 2-4: scenes on disk may still carry them.
        // Runtime-derived, never authored: masked out of scene serialization (kNonSerializedFlags) and
        // re-set every frame by the driving system.
        PER_FRAME_PARAMS = 1u << 21,
    };

    // Flags derived from live scene state that must never round-trip through a saved scene.
    // PER_FRAME_PARAMS is re-asserted every frame by the driving system; persisted, it would keep a
    // material out of surface-cache paging after it stops being driven (e.g. its
    // FaceShadingComponent is removed), with nothing on screen to say why. DIRTY / ASSET_DIRTY /
    // GPU_LINEAR_CACHE_DIRTY do persist.
    static constexpr uint32_t kNonSerializedFlags = PER_FRAME_PARAMS;

    enum SHADERTYPE : uint32_t
    {
#define SHADER_ENUM_(e, label) e,
        MATERIAL_SHADER_TYPES(SHADER_ENUM_)
#undef SHADER_ENUM_
        SHADERTYPE_COUNT
    };

    uint32_t flags = CAST_SHADOW | GPU_LINEAR_CACHE_DIRTY;

    // Linear-space colour cache: RefreshLinearCache (from Renderer::WriteMaterialSlot) refills it
    // when GPU_LINEAR_CACHE_DIRTY is set; later frames reuse it (saves ~14 powf per batch).
    mutable DirectX::XMFLOAT4 _linearBaseColor     = { 1, 1, 1, 1 };
    mutable DirectX::XMFLOAT4 _linearSpecularColor = { 1, 1, 1, 1 };
    mutable DirectX::XMFLOAT4 _linearEmissiveColor = { 1, 1, 1, 0 };
    mutable DirectX::XMFLOAT3 _linearNprDiffuseRamp = { 1, 0.92f, 0.82f };
    mutable DirectX::XMFLOAT3 _linearNprShadowRamp  = { 0.38f, 0.32f, 0.5f };
    // sRGB -> linear image of `subsurfaceColor` below (0.80 / 0.35 / 0.25 ^ 2.2).
    mutable DirectX::XMFLOAT3 _linearSubsurfaceColor = { 0.612f, 0.102f, 0.047f };
    // sRGB -> linear image of `attenuationColor` below.
    mutable DirectX::XMFLOAT3 _linearAttenuationColor = { 1.0f, 1.0f, 1.0f };

    // Compatibility field, NOT a lighting axis: nothing routes on it; customShadingModel is the
    // lighting axis (see Graphics/ShadingStencil.h). It exists because older builds read it from
    // .imat files, and a file written before `matfmt = 3` carries its routing decision here
    // (MaterialSerializer folds it onto the model at load). SyncLegacyShaderType keeps it
    // consistent with customShadingModel.
    SHADERTYPE shaderType = SHADERTYPE_PBR;

    StencilRef engineStencilRef = StencilRef::Default;
    BlendMode userBlendMode = BlendMode::Opaque;
    // Shadow-pass rasterizer cull override (see ShadowCullMode).
    ShadowCullMode shadowCullMode = ShadowCullMode::Default;

    // ---- Material domain (Resource/MaterialDomain.h) ----
    // Opaque (default) = surface material, GBuffer / Transparent draw path.
    // Decal = the material authors a clustered projected decal: the renderer builds a
    // DecalPass::ResolvedDecal from it for entities that also carry a DecalComponent, and domain
    // gates exclude it from mesh-surface / trail / particle look resolution.
    // Serialized as `domain` in .imat.
    Resource::MaterialDomain domain = Resource::MaterialDomain::Opaque;

    // ---- Decal-domain parameters (domain == Decal only) ----
    // GBuffer channels the decal overwrites, same bits as DecalMaterialAsset::FLAGS / the
    // DECAL_WRITE_* HLSL defines: 1 BaseColor, 2 Normal, 4 Roughness, 8 Specular, 16 AO.
    // Slots: BASECOLORMAP -> base, NORMALMAP -> normal, OPACITYMAP -> opacity mask (.r),
    // SURFACEMAP -> roughness (.g) + AO (.r) (ARM channel select via DECAL_SRC_ARM); specular writes
    // the flat `reflectance` scalar.
    uint32_t decalWriteMask      = 1u;    // WRITE_BASECOLOR
    float    decalAngleFadeStart = 0.3f;  // surface-vs-projection-axis fade start
    int32_t  decalSortLayer      = 0;     // apply order within a cluster (DecalClusterCull sorts ascending)

    // ---- Shading-model and domain helpers ----
    // The shaderType matching a shading model: the inverse of the fold ApplyMaterialText performs at
    // load, so a resaved file is self-consistent and an older build picks the same lighting route.
    // Toon maps to NPR_COLOR: the ramp source is the material's toon ramp binding
    // (MaterialGPUData::toonRampTex), not shaderType, and NPR_COLOR is what the editor's combo wrote
    // for both NPR_RAMP and NPR_COLOR.
    static constexpr SHADERTYPE LegacyShaderTypeFor(ShadingModel m)
    {
        switch (m)
        {
        case ShadingModel::Unlit:     return SHADERTYPE_UNLIT;
        case ShadingModel::Toon:      return SHADERTYPE_NPR_COLOR;
        case ShadingModel::ClearCoat: return SHADERTYPE_RESERVED_3;
        default:                      return SHADERTYPE_PBR;
        }
    }
    // Re-derives shaderType. Call after any change to customShadingModel.
    constexpr void SyncLegacyShaderType() { shaderType = LegacyShaderTypeFor(customShadingModel); }

    constexpr bool IsDecalDomain() const
    { return domain == Resource::MaterialDomain::Decal; }

    // Water is a shading model, not a domain (as Unreal's Single Layer Water): an ordinary surface
    // material with a translucent blend mode and ShadingModel::Water. It routes the draw to
    // DrawFilter::Water / WaterSurfacePass (which writes depth and stamps the GBuffer for SSR) and
    // out of the GBuffer, Transparent, shadow, RT / DDGI and VFX-look paths. The look is the
    // material's custom shader; water_common.hlsli supplies scene depth / colour, the sky-probe
    // reflection, CSM, clustered lights and the ripple field.
    constexpr bool IsWaterShadingModel() const
    { return customShadingModel == ShadingModel::Water; }

    // ---- Core PBR fields ----
    // From MATERIAL_PROPS; defaults are set in the constructor.
#define MAT_DECL_(type, name, label, mn, mx) type name{};
    MATERIAL_PROPS(MAT_DECL_)
#undef MAT_DECL_

    // ---- Texture-slot and name-map types, frameParams names ----
    enum TEXTURESLOT : uint32_t
    {
#define SLOT_ENUM_(e, label, path) e,
        MATERIAL_TEXTURE_SLOTS(SLOT_ENUM_)
#undef SLOT_ENUM_
        TEXTURESLOT_COUNT
    };

    struct TextureMap
    {
        std::string name;
        uint32_t uvset = 0;
    };

    // Hash of NameMap, the custom-shader stores' map. Transparent, so find() takes a string_view /
    // const char* without building a std::string (HashMatParams queries per texture per rebuild);
    // the key stays std::string.
    struct TransparentStringHash
    {
        using is_transparent = void;
        size_t operator()(std::string_view s) const noexcept
        { return std::hash<std::string_view>{}(s); }
        size_t operator()(const std::string& s) const noexcept
        { return std::hash<std::string_view>{}(std::string_view(s)); }
        size_t operator()(const char* s) const noexcept
        { return std::hash<std::string_view>{}(std::string_view(s)); }
    };
    template <typename V>
    using NameMap = std::unordered_map<std::string, V,
                                       TransparentStringHash, std::equal_to<>>;

    // The frameParams FaceShadingSystem writes. HeadForward / HeadRight are also
    // the face V2 contract's head frame (face_sdf.hlsli), which the emit copies
    // into the slot's frame record as (HeadForward.xz, HeadRight.xz).
    static constexpr const char* kFrameHeadForward = "HeadForward";
    static constexpr const char* kFrameHeadRight   = "HeadRight";
    static constexpr const char* kFrameHeadCenter  = "HeadCenter";

    // ---- Shading-model-independent parameters ----
    // UV0 transform uv' = uv * xy + zw (the inspector's Tiling / Offset; UV1 is never tiled).
    // Raster shaders apply it through MaterialUV(), RT hits through its twin RTInstanceUV().
    DirectX::XMFLOAT4 texMulAdd = { 1, 1, 0, 0 };
    // Serialized only, no shader reads it: parallax is written in a custom shader's
    // EvaluateMaterial (asset/Shaders/Eye.ps.hlsl's iris parallax).
    float parallaxOcclusionMapping = 0.0f;

    // Exposure protection [0, 1]: 0 = normal auto-exposure. The AA-prepass HDR stabilizer reads the
    // GPU exposure buffer and counter-scales. NPR materials take the full range: 1 cancels the
    // tonemap exposure exactly, so the character's ACES input is fully authored and never moves with
    // adaptation. Non-NPR materials clamp to 0.5 in the shader: a PBR surface resists adaptation, it
    // does not opt out of physical exposure. RT-reflection hit shading applies the same
    // counter-scale so mirrors match the primary view.
    float exposureCompensation = 0.0f;

    // ---- NPR toon parameters (ShadingModel::Toon) ----
    // nprMinBrightness 0 = true-black shadows, as PBR, so AutoExposure meters NPR and PBR alike;
    // raise it for a soft-shadow floor.
    float nprMinBrightness   = 0.0f;   // minimum brightness in shadow (0 = full dark, 1 = no shadow)
    float nprShadowThreshold = 0.5f;   // NdotL threshold for shadow/lit transition
    float nprShadowSmooth    = 0.05f;  // smoothstep width around threshold
    float nprRimPower        = 4.0f;   // rim light falloff exponent
    float nprRimStrength     = 0.3f;   // rim light intensity
    float nprRampBlend       = 1.0f;   // 0 = pure baseColor tint, 1 = full ramp color
    // Relative ceiling: output luminance <= Luminance(albedo) x nprBrightnessClamp. Gated by the
    // NPR_BRIGHTNESS_CLAMP flag, not a 0 sentinel, so the slider stays continuous at its low end;
    // read only when the flag is set.
    float nprBrightnessClamp = 1.0f;   // ceiling = albedo luminance x this (flag-gated)
    // Absolute ceiling for the final NPR colour: an exposed-space max-RGB soft shoulder (knee at half
    // the value, asymptote at the value) in NPRStabilize, after the artistic shoulder below.
    // Exposed-space so it holds at the ACES input under any adaptation; max-RGB so a saturated
    // channel cannot slip past a luma test; soft so highlights do not flatten into a plateau.
    // 0 = no absolute ceiling - but the artistic shoulder (nprShoulderStart / Range / Strength) is on
    // by default and already caps exposed max-RGB at Start + Range (1.75); set nprShoulderStrength 0
    // for a truly uncapped material. min()ed with LightCB.nprMaxBrightness, the scene-wide ceiling.
    float nprMaxBrightness   = 0.0f;   // exposed-space soft max-RGB ceiling (0 = disabled)

    // ---- NPR ramp-texture row blend weights (uploaded in MaterialGPUData; no shader reads them) ----
    float nprMidWeight       = 0.4f;   // mid-tone ramp row blend weight
    float nprVeinWeight      = 0.5f;   // deep-shadow vein ramp row blend weight (modulated by shadowMask * ao)
    float nprSSSWeight       = 0.3f;   // SSS ramp row blend weight

    // ---- NPR specular ramp (toon highlight) ----
    // Stylizes the accumulated PHYSICAL specular into a stepped anime highlight.
    // Opt-in: nprSpecRampBlend 0 = physical specular (unchanged), 1 = fully stylized.
    float nprSpecThreshold   = 0.5f;   // luminance cut-on for the stylized highlight
    float nprSpecSmooth      = 0.07f;  // smoothstep width around the threshold
    float nprSpecStrength    = 1.0f;   // stylized highlight intensity multiplier
    float nprSpecRampBlend   = 0.0f;   // 0 = physical spec, 1 = fully stylized (toon)

    // ---- DanbaidongRP toon alignment (Lighting.ps toon block) ----
    // Defaults keep this engine's look; DanbaidongRP's shipped values are noted per field.
    float nprShadowStrength    = 1.0f;  // 0 = no toon shadow at all (his _ShadowStrength, ships 1.0)
    // Half-width of the stylized cast-shadow edge (ToonVisibility); read only when the material sets
    // NPR_TOON_SHADOW_EDGE. Keep the 0.1 default: every .imat on disk already stores it.
    float nprSceneShadowSmooth = 0.1f;
    // 0 = the scene light drives fully, -> 1 = fixed white self-light (his _MainLightColorLerp
    // ships 0.5: decouples the character from scene light intensity, including the backlit ramp
    // side).
    float nprMainLightLerp     = 0.0f;
    // How much AO darkens DIRECT light (diffuse and spec). 0.1 = DanbaidongRP's value: direct
    // nearly unoccluded, cavities darken via ambient only. 1 = full AO on direct, which eats toon
    // spec in AO-dark areas.
    float nprDirectOcclusion   = 0.1f;
    float nprRimWidth          = 2.5f;  // screen-space depth-rim width in ~pixels (his _DirectRimWidth)

    // ---- NPR energy contract ----
    // A toon ramp replaces the NdotL response, not the Lambert 1/pi energy normalization. These
    // scales budget each lighting family without retuning physically authored scene lights for
    // characters.
    float nprDirectLightScale       = 1.0f;
    float nprAdditionalLightScale   = 0.35f;
    float nprAmbientLightScale      = 0.75f;
    float nprSpecularLightScale     = 0.75f;

    // Split physical light chroma from intensity coupling. Defaults keep full
    // scene response; lower intensity weight moves toward the reference luma
    // while retaining scene hue according to color weight.
    float nprSceneLightColorWeight     = 1.0f;
    float nprSceneLightIntensityWeight = 1.0f;
    float nprReferenceLightLuminance   = 1.0f;
    float nprExposureBiasEV            = 0.0f;

    // Final NPR HDR shoulder, after lighting + emissive and before TAA / FSR; exposure-space and
    // hue-preserving. Start + Range is the asymptotic maximum channel at Strength 1.
    // Default-on ceiling: with these defaults every NPR material's exposed max-RGB is capped at 1.75,
    // so "no ceiling" (nprMaxBrightness = 0) is not an NPR material's default state and bloom-range
    // highlights are compressed; author Strength = 0 to opt a material out. These are schema Float
    // rows that the full-dump serializer bakes into every saved .imat: changing the defaults here
    // does not change saved materials (as with nprSceneShadowSmooth).
    float nprShoulderStart    = 1.0f;
    float nprShoulderRange    = 0.75f;
    float nprShoulderStrength = 1.0f;

    // ---- NPR colour ramp (toon materials without a ramp texture) ----
    // Two authored colours drive a texture-less diffuse ramp.
    DirectX::XMFLOAT4 nprDiffuseRampColor = { 1.00f, 0.92f, 0.82f, 1.0f }; // lit side tint
    // Shadow side tint, near-black by default (DanbaidongRP's _ShadowColor = black): the toon diffuse
    // is albedo x shadowRamp x lightColor with no NdotL, so any brightness here makes the backlit
    // side scale with the directional light's intensity. Near-black leaves the backlit side to
    // ambient / IBL; raise it deliberately for a coloured shadow that follows the light.
    DirectX::XMFLOAT4 nprShadowRampColor  = { 0.02f, 0.02f, 0.03f, 1.0f }; // shadow side tint

    // ---- ShadingModel::Anisotropic (customShadingModel) ----
    // Brushed metal / hair / satin: a streak-shaped specular lobe. Inert on every other model, so
    // these live here rather than in customParams (which exist only on custom-shader materials, and
    // whose slots 0..2 Lighting.ps already books three times over; see shaders/material.hlsli).
    // The sign follows Unreal: positive widens the lobe along the tangent. Hair and brushed metal,
    // whose authored tangent runs along the strand, want a negative value (or a 0.25 rotation).
    float anisotropy         = 0.0f;   // [-1, 1]; 0 = isotropic (model inert)
    float anisotropyRotation = 0.0f;   // [0, 1] turns, about the shading normal

    // ---- ShadingModel::Subsurface ----
    // Wrap-softened diffuse + a per-light transmission lobe. There is no screen-space diffusion pass,
    // so `subsurfaceWrap` is what softens skin's terminator. TwoSidedFoliage reads subsurfaceColor /
    // Wrap / Strength as its transmission tint and wrap; the skin-ish red default is inert until a
    // material selects one of the two models. `_linearSubsurfaceColor` must stay the sRGB -> linear
    // image of this value (RefreshLinearCache recomputes it on the first dirty frame; the literal
    // there is only the seed).
    DirectX::XMFLOAT4 subsurfaceColor = { 0.80f, 0.35f, 0.25f, 1.0f }; // sRGB tint
    // Wrap redistributes diffuse energy, it adds none: the normalisation is (NdotL + w) / (1 + w)^2,
    // so the terminator brightens and the fully lit side dims (at w = 0.5 a face-on pixel keeps 2/3
    // of its diffuse). Expect the material to read slightly darker as you raise it and compensate
    // with base colour. 0 is exactly saturate(NdotL), bit for bit.
    float subsurfaceWrap     = 0.5f;   // [0,1] diffuse terminator wrap; 0 = off exactly
    float subsurfaceStrength = 1.0f;   // scales the transmission lobe; 0 = off

    // ---- ShadingModel::ClearCoat ----
    // A thin dielectric film over the base (car paint, lacquer, varnish, wet enamel): a sharp
    // environment reflection over a base that keeps its own, usually blurrier, highlight, which one
    // GGX lobe cannot express. The coat's F0 is fixed at 0.04 (IOR 1.5) and deliberately not
    // authored. Strength 0 removes the coat lobe and the base attenuation exactly, so the material
    // renders as Standard.
    float clearCoat          = 0.0f;   // [0,1] strength
    // [0, 1] the coat's own roughness, independent of roughnessMin / Max (a mirror coat over a
    // rough base is the point).
    float clearCoatRoughness = 0.1f;
    // ShadingModel::Subsurface only, not clear coat (TwoSidedFoliage ignores it too). [0, 1]
    // 1 = solid / thick, 0 = thin / see-through: the default of the per-pixel RT4.r channel, which a
    // custom hook may override with a thickness map.
    float subsurfaceOpacity  = 0.5f;

    // ---- BlendMode::Transmissive ----
    // glTF KHR_materials_transmission / _volume / _ior. Opacity stays coverage; these describe the
    // light that crosses a covered pixel, tinted by baseColor (once through the whole object).
    // Read only on a Transmissive material, where the reflection's F0 follows `ior` instead of
    // `reflectance` so reflection and refraction agree.
    float transmission        = 0.0f;   // [0,1] share of the dielectric base that refracts, not diffuses
    float ior                 = 1.5f;   // interior / exterior index of refraction
    float thickness           = 0.0f;   // world units through the volume; 0 = a thin wall (no bend)
    float attenuationDistance = 0.0f;   // world units after which light keeps attenuationColor; 0 = clear
    DirectX::XMFLOAT4 attenuationColor = { 1.0f, 1.0f, 1.0f, 1.0f };   // sRGB; .w unused

    // ---- Stencil, texture animation, outline, texture slots ----
    uint8_t userStencilRef = 0;

    DirectX::XMFLOAT2 texAnimDirection = { 0, 0 };
    float texAnimFrameRate = 0.0f;
    float texAnimElapsedTime = 0.0f;

    // Per-material outline thickness (pixels, screen-space invariant).
    // Only used when OUTLINE flag is set.
    float outlinePixels = 2.0f;

    TextureMap textures[TEXTURESLOT_COUNT];

    // ---- Custom-shader parameter stores ----
    // Filled from the custom shader's reflection (MaterialReflectionSync) and keyed by the binding /
    // variable NAME, so values survive register reshuffles between shader edits.
    //   customTextures: authored TextureMap entries (`name` + `uvset`); GPU handles live only in the
    //                   Renderer's shared RuntimeMaterialResource.
    //   customParams:   always 4 floats: scalar [0], vec2 [0..1], vec3 [0..2], vec4 all four; integer
    //                   vars are bit-cast into the same array.
    NameMap<TextureMap>            customTextures;
    NameMap<std::array<float, 4>>  customParams;

    // Per-frame values an ENGINE SYSTEM writes from live scene state - the face's HeadForward /
    // HeadRight / HeadCenter (FaceShadingSystem) - kept apart from the authored customParams.
    // RUNTIME-ONLY: never serialized, not in matHash, not shown as authored values. The b8 packer
    // reads a name here before customParams (FindGpuParam); the deferred toon reads the head frame
    // from the material slot's frame record. A material with entries here carries PER_FRAME_PARAMS.
    NameMap<std::array<float, 4>>  frameParams;

    // Reflected per-param metadata (runtime-only, never serialized), rebuilt by MaterialReflectionSync
    // from the shader's `//@default / @min / @max / @label` annotations (DXC reflection cannot carry
    // cbuffer defaults). Drives the seeded default of a new param, diff-against-default
    // serialization (only overrides reach the .imat), and the inspector slider range + label.
    struct CustomParamMeta
    {
        std::array<float, 4> defaultValue = { 0, 0, 0, 0 };
        std::array<float, 4> minValue     = { 0, 0, 0, 0 };
        std::array<float, 4> maxValue     = { 1, 1, 1, 1 };
        bool                 hasRange     = false;
        std::string          label;
        // 1-4 from the reflected VarType; 0 = unknown (the editor falls back to live reflection /
        // Float4). Lets inspectors size widgets when the reflection lives in a pass-local library the
        // editor cannot reach (trail / particle-driven materials).
        uint8_t              numComponents = 0;
        // Name starts with "Tex_": the b8 value is a bindless texture index written by the owning
        // system (float, -1 = unbound), not a tunable.
        bool                 isTextureSlot = false;
        // Reflected VarType::Bool. The float bag stores it as 0/1 in [0] (serialization is unchanged),
        // but the inspector draws a checkbox and the b8 packer writes the HLSL cbuffer bool ABI (a 32-bit
        // 1/0), not a float bit pattern.
        bool                 isBool        = false;
        // Inspector SECTION from `@group "Color"` + a stable sort key (the
        // var's cbuffer byte offset = declaration order, identical across
        // PS/VS reflections). The inspector sorts by sortOrder and emits a
        // separator header whenever the group changes.
        std::string          group;
        uint16_t             sortOrder     = 0xFFFF;
    };
    NameMap<CustomParamMeta> customParamMeta;

    // Set by the .imat loader when the file has no `cparamFmt` marker (it predates
    // diff-against-default params). The reflection Sync then moves any param still at the
    // DefaultForType sentinel to its reflected default, so the material keeps its look. Runtime-only;
    // cleared once the material's shader stages have synced (Renderer::ResolveCustomVSID, and
    // TrailSystem / ParticleSystem for their look materials).
    bool customParamsLegacyFormat = false;

    // Runtime-only: MaterialReflectionSync's "already synced" stamp. [63:32] FNV32 of both custom
    // shader paths and of what picks the draw variant (blend mode, alpha test, double-sided, water),
    // [31:2] the ShaderLibrary reload epoch, [1] VS stage synced, [0] PS stage synced; 0 = never.
    // Keeps the steady state at a path hash + integer compares (MaterialReflectionSync::StageSynced /
    // MarkStageSynced).
    uint64_t reflSyncStamp = 0;

    // ---- Custom GBuffer pixel shader ----
    // useCustomShader + customShaderPath make the GBuffer pass compile the user's HLSL and route this
    // material's draws through that PSO; customShadingModel tells the lighting pass how to read the
    // GBuffer it writes. customShaderPath (relative to the shader directory) is the persistent
    // identity; dynamic ShaderIDs are cached by the Renderer, never serialized or copied with the
    // component.
    bool         useCustomShader      = false;
    // Authored standard texture/scalar slots describe the RT surface exactly.
    // Keep the custom raster shader, but bypass guessed aliases and UV atlasing.
    bool         rtUseStandardMaterial = false;
    std::string  customShaderPath;
    ShadingModel customShadingModel   = ShadingModel::Standard;

    // ---- Custom GBuffer vertex shader ----
    // Optional, independent of customShaderPath. When set (and useCustomShader), draws use this file's
    // vertex shader instead of GBuffer_VS; it is authored against shaders/custom_vertex.hlsli (one
    // CustomizeVertex() hook). Empty = engine default. The path is the persistent identity; the
    // dynamic ShaderID is owned by the Renderer.
    std::string  customVertexShaderPath;

    // ---- Material graph ----
    // Editor-only: the material graph (.imatgraph) this material's custom shader is generated from.
    // Round-trips through the .imat as `materialGraph = <path>`; the runtime ignores it.
    std::string  materialGraph;
    // Editor-only: the graph's Static Switch Parameter values this material (or instance) picked
    // (`sswitch_<name> = 0|1`); customShaderPath names the shader compiled for that combination. A
    // name not listed takes the graph's default.
    NameMap<bool> staticSwitches;

    // ---- Layer mask, sampler, camera source (no draw path reads them) ----
    uint32_t layerMask = ~0u;
    int samplerDescriptor = -1;

    Entity cameraSource = NullEntity;

    // ---- Constructor and accessors ----
    // The value a b8 member named `name` is packed from: this frame's
    // frameParams entry when a system writes one, else the authored customParams
    // one. Null when neither has it.
    const std::array<float, 4>* FindGpuParam(std::string_view name) const
    {
        if (!frameParams.empty())
            if (auto it = frameParams.find(name); it != frameParams.end()) return &it->second;
        auto it = customParams.find(name);
        return it != customParams.end() ? &it->second : nullptr;
    }

    MaterialComponent()
    {
        // Defaults for the MATERIAL_PROPS fields.
        roughnessMax         = 1.0f;
        roughnessMin         = 0.0f;

        metalnessMax         = 1.0f;
        metalnessMin         = 0.0f;

        reflectance       = 0.5f;
        normalMapStrength = 1.0f;
        saturation        = 1.0f;
        alphaRef          = 1.0f;
        baseColor         = { 1, 1, 1, 1 };
        specularColor     = { 1, 1, 1, 1 };
        emissiveColor     = { 1, 1, 1, 0 };

#define SLOT_DEFAULT_(e, label, path) textures[e].name = path;
        MATERIAL_TEXTURE_SLOTS(SLOT_DEFAULT_)
#undef SLOT_DEFAULT_
    }

    constexpr void SetUserStencilRef(uint8_t value) { userStencilRef = value & 0x0Fu; }
    uint32_t GetStencilRef() const;

    constexpr float GetOpacity() const { return baseColor.w; }
    constexpr float GetEmissiveStrength() const { return emissiveColor.w; }

    constexpr void SetDirty(bool value = true)
    {
        if (value) flags |= DIRTY | GPU_LINEAR_CACHE_DIRTY;
        else       flags &= ~DIRTY;   // keep GPU_LINEAR_CACHE_DIRTY until renderer consumes it
    }
    // Marks GPU dirty and asset dirty (the editor calls it when the user edits a property). Also
    // sets GPU_LINEAR_CACHE_DIRTY, or the cached _linear* colours stay stale and the GPU uploads the
    // old value.
    constexpr void SetEditDirty()
    { flags |= DIRTY | ASSET_DIRTY | GPU_LINEAR_CACHE_DIRTY; }
    constexpr bool IsDirty() const { return (flags & DIRTY) != 0; }

    // Asset dirty: runtime instance differs from on-disk .imat file.
    // Set when editor modifies material; cleared on Save-to-disk.
    constexpr void SetAssetDirty(bool value = true) { if (value) flags |= ASSET_DIRTY; else flags &= ~ASSET_DIRTY; }
    constexpr bool IsAssetDirty() const { return (flags & ASSET_DIRTY) != 0; }

    constexpr void SetInternal(bool value = true) { if (value) flags |= INTERNAL; else flags &= ~INTERNAL; }
    constexpr bool IsInternal() const { return (flags & INTERNAL) != 0; }

    // Deliberately NOT SetDirty: this is an observation about who writes the
    // material, not an edit to it. Marking it dirty here would re-run the whole
    // material texture sync every frame for exactly the materials whose
    // frameParams change every frame.
    constexpr void SetPerFrameParams(bool value = true)
    { if (value) flags |= PER_FRAME_PARAMS; else flags &= ~PER_FRAME_PARAMS; }
    constexpr bool HasPerFrameParams() const { return (flags & PER_FRAME_PARAMS) != 0; }

    constexpr void SetCastShadow(bool value) { SetDirty(); if (value) flags |= CAST_SHADOW; else flags &= ~CAST_SHADOW; }
    constexpr void SetReceiveShadow(bool value) { SetDirty(); if (value) flags &= ~DISABLE_RECEIVE_SHADOW; else flags |= DISABLE_RECEIVE_SHADOW; }
    constexpr void SetOcclusionEnabled_Primary(bool value) { SetDirty(); if (value) flags |= OCCLUSION_PRIMARY; else flags &= ~OCCLUSION_PRIMARY; }
    constexpr void SetOcclusionEnabled_Secondary(bool value) { SetDirty(); if (value) flags |= OCCLUSION_SECONDARY; else flags &= ~OCCLUSION_SECONDARY; }

    constexpr bool IsCastingShadow() const { return (flags & CAST_SHADOW) != 0; }
    // Masked always alpha-tests (Unreal semantics: alphaRef is the Mask Clip Value); other modes opt
    // in by lowering alphaRef below ~0.996. This predicate also feeds the RT alpha-clip path
    // (DDGISceneAS) and the shadow-pass ALPHA_TEST permutation: keep raster and RT in lockstep.
    constexpr bool IsAlphaTestEnabled() const
    {
        return userBlendMode == BlendMode::Masked
            || userBlendMode == BlendMode::Dithered   // ALPHA_TEST perm, dither threshold
            || alphaRef <= kAlphaRefNoClipThreshold;
    }
    // The clip threshold the GPU tests against. Differs from the authored alphaRef only for Masked
    // with alphaRef unset (1.0) and for Dithered; see MaskClipValueFor(). Publish sites:
    // WriteMaterialSlot, DDGISceneAS.
    constexpr float MaskClipValue() const { return MaskClipValueFor(userBlendMode, alphaRef); }
    constexpr bool IsUsingVertexColors() const { return (flags & USE_VERTEXCOLORS) != 0; }
    constexpr bool IsUsingWind() const { return (flags & USE_WIND) != 0; }
    constexpr bool IsReceiveShadow() const { return (flags & DISABLE_RECEIVE_SHADOW) == 0; }
    constexpr bool IsUsingSpecularGlossinessWorkflow() const { return (flags & SPECULAR_GLOSSINESS_WORKFLOW) != 0; }
    constexpr bool IsOcclusionEnabled_Primary() const { return (flags & OCCLUSION_PRIMARY) != 0; }
    constexpr bool IsOcclusionEnabled_Secondary() const { return (flags & OCCLUSION_SECONDARY) != 0; }
    // True when the material opts into a custom VERTEX shader (path set + master toggle).
    bool UsesCustomVertexShader() const { return useCustomShader && !customVertexShaderPath.empty(); }
    // True when the material opts into a custom shader of EITHER stage.
    bool HasAnyCustomShader() const
    { return useCustomShader && (!customShaderPath.empty() || !customVertexShaderPath.empty()); }
    constexpr bool IsDoubleSided() const { return (flags & DOUBLE_SIDED) != 0; }
    constexpr bool IsOutlineEnabled() const { return (flags & OUTLINE) != 0; }
    constexpr bool IsOutlineScreenSpaceEnabled() const { return (flags & OUTLINE_SCREENSPACE) != 0; }
    constexpr bool IsNprBrightnessClampEnabled() const { return (flags & NPR_BRIGHTNESS_CLAMP) != 0; }
    constexpr bool IsNprToonShadowEdgeEnabled() const { return (flags & NPR_TOON_SHADOW_EDGE) != 0; }
    constexpr bool IsSourceOutlineExempt() const { return (flags & SOURCE_OUTLINE_EXEMPT) != 0; }
    constexpr bool IsHairMaskEnabled() const { return (flags & HAIR_MASK) != 0; }

    BlendMode GetBlendMode() const;

    constexpr void SetBaseColor(const DirectX::XMFLOAT4& value) { SetDirty(); baseColor = value; }
    constexpr void SetSpecularColor(const DirectX::XMFLOAT4& value) { SetDirty(); specularColor = value; }
    constexpr void SetEmissiveColor(const DirectX::XMFLOAT4& value) { SetDirty(); emissiveColor = value; }
    constexpr void SetRoughnessMax(float value) { SetDirty(); roughnessMax = value; }
    constexpr void SetRoughnessMin(float value) { SetDirty(); roughnessMin = value; }

    constexpr void SetReflectance(float value) { SetDirty(); reflectance = value; }
    constexpr void SetMetalnessMax(float value) { SetDirty(); metalnessMax = value; }
    constexpr void SetMetalnessMin(float value) { SetDirty(); metalnessMin = value; }

    constexpr void SetEmissiveStrength(float value) { SetDirty(); emissiveColor.w = value; }
    constexpr void SetSaturation(float value) { SetDirty(); saturation = value; }
    constexpr void SetNormalMapStrength(float value) { SetDirty(); normalMapStrength = value; }
    constexpr void SetParallaxOcclusionMapping(float value) { SetDirty(); parallaxOcclusionMapping = value; }
    constexpr void SetOpacity(float value) { SetDirty(); baseColor.w = value; }
    constexpr void SetAlphaRef(float value) { SetDirty(); alphaRef = value; }
    constexpr void SetTransmission(float value) { SetDirty(); transmission = value; }
    constexpr void SetIor(float value) { SetDirty(); ior = value; }
    constexpr void SetThickness(float value) { SetDirty(); thickness = value; }
    constexpr void SetAttenuationDistance(float value) { SetDirty(); attenuationDistance = value; }
    constexpr void SetAttenuationColor(const DirectX::XMFLOAT4& value) { SetDirty(); attenuationColor = value; }
    constexpr void SetUseVertexColors(bool value) { SetDirty(); if (value) flags |= USE_VERTEXCOLORS; else flags &= ~USE_VERTEXCOLORS; }
    constexpr void SetUseWind(bool value) { SetDirty(); if (value) flags |= USE_WIND; else flags &= ~USE_WIND; }
    constexpr void SetUseSpecularGlossinessWorkflow(bool value) { SetDirty(); if (value) flags |= SPECULAR_GLOSSINESS_WORKFLOW; else flags &= ~SPECULAR_GLOSSINESS_WORKFLOW; }
    constexpr void SetDoubleSided(bool value = true) { if (value) flags |= DOUBLE_SIDED; else flags &= ~DOUBLE_SIDED; }
    constexpr void SetOutlineEnabled(bool value = true) { if (value) flags |= OUTLINE; else flags &= ~OUTLINE; }
    constexpr void SetOutlineScreenSpaceEnabled(bool value = true) { if (value) flags |= OUTLINE_SCREENSPACE; else flags &= ~OUTLINE_SCREENSPACE; }
    constexpr void SetNprBrightnessClampEnabled(bool value = true) { SetDirty(); if (value) flags |= NPR_BRIGHTNESS_CLAMP; else flags &= ~NPR_BRIGHTNESS_CLAMP; }
    constexpr void SetNprToonShadowEdgeEnabled(bool value = true) { SetDirty(); if (value) flags |= NPR_TOON_SHADOW_EDGE; else flags &= ~NPR_TOON_SHADOW_EDGE; }
    constexpr void SetHairMaskEnabled(bool value = true) { SetDirty(); if (value) flags |= HAIR_MASK; else flags &= ~HAIR_MASK; }
};

// ==== MeshComponent (alignas(32): mesh vertices / indices and subsets) ====

struct alignas(32) MeshComponent
{
    enum FLAGS : uint32_t
    {
        EMPTY = 0,
        RENDERABLE = 1 << 0,
        DOUBLE_SIDED = 1 << 1,
        DYNAMIC = 1 << 2,
        DOUBLE_SIDED_SHADOW = 1 << 7,
    };

    enum MESH_SUBSET_FLAGS : uint32_t
    {
        MESH_SUBSET_DOUBLESIDED = 1 << 0,
    };

    struct MeshSubset
    {
        std::string surfaceName;
        Entity materialID = NullEntity;
        uint32_t indexOffset = 0;
        uint32_t indexCount = 0;
        uint32_t materialIndex = 0;
        uint32_t flags = 0;
        constexpr bool IsDoubleSided() const { return (flags & MESH_SUBSET_DOUBLESIDED) != 0; }
    };

    struct BufferView
    {
        uint64_t offset = ~0ull;
        uint64_t size = 0ull;
        int descriptorSrv = -1;
        constexpr bool IsValid() const { return offset != ~0ull; }
    };

    uint32_t flags = RENDERABLE;

    std::vector<DirectX::XMFLOAT3> vertexPositions;
    std::vector<DirectX::XMFLOAT3> vertexNormals;
    std::vector<DirectX::XMFLOAT4> vertexTangents;
    std::vector<DirectX::XMFLOAT2> vertexUvSet0;
    std::vector<DirectX::XMFLOAT2> vertexUvSet1;
    std::vector<DirectX::XMUINT4> vertexBoneIndices;
    std::vector<DirectX::XMFLOAT4> vertexBoneWeights;
    std::vector<uint32_t> vertexColors;
    std::vector<uint32_t> indices;

    std::vector<MeshSubset> subsets;

    Entity armatureID = NullEntity;
    float tessellationFactor = 0.0f;
    uint32_t subsetsPerLod = 0;

    AABB aabb;

    BufferView ib;
    BufferView vbPos;
    BufferView vbNor;
    BufferView vbTan;
    BufferView vbUvs;
    BufferView vbCol;
    BufferView vbBon;

    uint32_t geometryOffset = 0;

    constexpr void SetRenderable(bool value) { if (value) flags |= RENDERABLE; else flags &= ~RENDERABLE; }
    constexpr void SetDoubleSided(bool value) { if (value) flags |= DOUBLE_SIDED; else flags &= ~DOUBLE_SIDED; }
    constexpr void SetDoubleSidedShadow(bool value) { if (value) flags |= DOUBLE_SIDED_SHADOW; else flags &= ~DOUBLE_SIDED_SHADOW; }
    constexpr void SetDynamic(bool value) { if (value) flags |= DYNAMIC; else flags &= ~DYNAMIC; }

    constexpr bool IsRenderable() const { return (flags & RENDERABLE) != 0; }
    constexpr bool IsDoubleSided() const { return (flags & DOUBLE_SIDED) != 0; }
    constexpr bool IsDoubleSidedShadow() const { return (flags & DOUBLE_SIDED_SHADOW) != 0; }
    constexpr bool IsDynamic() const { return (flags & DYNAMIC) != 0; }
    constexpr bool IsSkinned() const { return armatureID != NullEntity; }

    uint32_t GetLODCount() const;
    void GetLODSubsetRange(uint32_t lod, uint32_t& firstSubset, uint32_t& lastSubset) const;

    void ComputeNormals(bool smooth);
    void FlipCulling();
    void Recenter();
};

// ==== Render-pipeline components ====
// Read by the Renderer's scene build, which translates them for the passes; render passes must
// not read them.

// Transforms live in LocalTransform / GlobalTransform (HierarchyComponents.h);
// TransformSystem::Propagate owns the parent-inherited world matrix.

// BoundingVolume - axis-aligned box in local space (center + half-extents); spawned primitives
// carry it.
struct BoundingVolume
{
    DirectX::XMFLOAT3 center  = { 0.f, 0.f, 0.f };
    DirectX::XMFLOAT3 extents = { 0.5f, 0.5f, 0.5f }; // half-extents
};

// MeshHandle - index into the Renderer-owned GPU mesh registry.
struct MeshHandle
{
    uint32_t gpuMeshID = UINT32_MAX; // UINT32_MAX = invalid / not uploaded
    bool IsValid() const { return gpuMeshID != UINT32_MAX; }
};

// MaterialHandle - index into a Renderer GPU material table; no system writes or reads it.
struct MaterialHandle
{
    uint32_t materialID = UINT32_MAX; // UINT32_MAX = invalid
    bool IsValid() const { return materialID != UINT32_MAX; }
};

// VisibleTag - zero-size culling marker; no system writes or reads it (culling runs inside the
// Renderer).
struct VisibleTag {};

// LightType - discriminator for LightData.
enum class LightType : uint8_t
{
    Directional = 0,
    Point       = 1,
    Spot        = 2,
};

// LightData - optical parameters only; position and direction come from the entity's transform:
//   position  <- GlobalTransform translation
//   direction <- GlobalTransform forward (+Z), via LightUtil::DirectionFromWorld
// Aim a light by rotating the entity (gizmo, animation track, parent bone); there is no direction
// field. A `dir=` key in a scene file is folded into the entity rotation on load
// (ComponentSerializers).
struct LightData
{
    float             radius    = 10.f;       // attenuation range (point/spot)
    DirectX::XMFLOAT3 color     = { 1.f, 1.f, 1.f };
    float             intensity = 1.f;
    float             spotAngle = 0.5236f;    // spot half-angle in radians (default 30 deg)
    LightType         type      = LightType::Directional;
    // Opt-in shadow caster for spot and point lights:
    //   Spot  -> SpotShadowPass renders one depth map into the shared 2D atlas; Lighting and
    //            VolumetricFog sample it.
    //   Point -> PointShadowPass renders 6 faces into a cube atlas; only Lighting.ps samples it
    //            (VolumetricFog does not).
    // Both are capped (Spot / PointShadowPass::kMaxCasters); excess casters silently get no shadow.
    // Directional lights always use CSM regardless of this flag.
    bool              castsShadow = false;
    bool              enabled = true; // authoring / runtime switch; preserves intensity
    // Physical emitter radius (world units), Point / Spot only; 0 = a punctual (delta) light. Raster
    // shading and the shadow maps ignore it; the ray-traced paths (ReSTIR DI, the path tracer) aim
    // each visibility ray at a point on the emitter, so shadows get the penumbra of a light this
    // size. UE imports fill it from SourceRadius or an equal-area disk of a rect light.
    float             sourceRadius = 0.f;

    // Runtime-only (not serialized, not in the inspector): monotonic first-sight stamp the light
    // gather sorts by, giving a deterministic creation order over a pool walk. m_frameLights order is
    // load-bearing: shadow slices go to the first kMaxCasters and the sun is the last directional.
    // 0 = not stamped yet.
    uint32_t          gatherSeq = 0;
};

// ==== DecalComponent - per-entity instance of a clustered decal ====
// Holds a shared DecalMaterialAsset (textures, tint, flags, blend); many entities can share one
// asset, while per-instance state (transform, lifetime, fade) stays here. The volume is a unit
// cube at the entity's GlobalTransform, sized by its scale: the DecalApply CS projects each pixel
// into decal-local [-0.5, 0.5]^3 and discards outside, so axis-aligned scaling is a box clip.
// Full include, not a forward declaration: std::shared_ptr<T>'s destructor needs T complete
// wherever the DecalComponent destructor is emitted (vector growth, pool clear);
// DecalMaterialAsset.h is a light include.
#include "Resource/DecalMaterialAsset.h"

struct DecalComponent
{
    // Shared material template, owned by DecalMaterialLibrary. nullptr = no decal this frame
    // (disables an instance without destroying it).
    std::shared_ptr<Resource::DecalMaterialAsset> material;

    // ---- Per-instance state ----
    // Runtime multiplier on the asset's opacity; DecalLifetimeSystem drives it for dynamic decals
    // (fades to 0 before expiry). Static decals leave it at 1.
    float fadeAlpha = 1.0f;

    // -1 = static (never expires). Otherwise seconds left: DecalLifetimeSystem counts it down every
    // frame and expires the decal at 0.
    float lifetime = -1.0f;

    // Seconds before expiry over which fadeAlpha decays linearly 1 -> 0; 0 = no fade (instant pop).
    // Only meaningful when lifetime >= 0.
    float fadeOutDuration = 0.5f;

    // True: destroy the entity on expiry. False: remove only the DecalComponent and keep the entity
    // for reuse (pooled decals such as DecalSpawner's pool).
    bool destroyEntityOnExpire = true;

    // Per-instance tint: w = 0 uses the asset tint as-is, w > 0 replaces it (shade variations of one
    // material without authoring more assets).
    DirectX::XMFLOAT4 tintOverride = { 0.f, 0.f, 0.f, 0.f };
};

// VolumetricLightComponent - opt-in marker on a light entity that injects it into the volumetric
// fog froxel grid; without it the light lights surfaces only and is invisible in the fog.
// `intensityScale` scales the light's brightness in the fog only (a dramatic flashlight shaft over
// a sensibly lit surface).
struct VolumetricLightComponent
{
    bool  enabled        = true;
    float intensityScale = 1.f;
};

// CameraData - generic camera parameters, reflected for the inspector; the renderer's camera is
// CameraComponent.
struct CameraData
{
    float fov               = DirectX::XM_PIDIV4; // vertical FOV in radians
    float nearZ             = 0.1f;
    float farZ              = 1000.f;
    float aspectRatioOverride = 0.f; // 0 = use window aspect ratio
};

// CameraComponent - lens / projection parameters of a camera entity. The pose lives on
// LocalTransform / GlobalTransform, the FPS-controller state on CameraControllerComponent.
struct CameraComponent
{
    float fov   = DirectX::XM_PI / 3.f;  // vertical FOV in radians
    float nearZ = 0.1f;
    float farZ  = 200.f;
};

// ActiveCameraTag - data-less marker for the camera the renderer uses this frame.
// App::RefreshMainCamera prefers the tagged entity, else the first entity with a
// CameraComponent. Switch with Lua Camera.SetActive(entityId) / Camera.GetActive()
// (LuaPlayerBindings), which strips the tag from the previous holder; C++ adds / removes the tag
// directly and must keep at most one.
struct ActiveCameraTag : ComponentBase {};

// EditorCameraTag - the editor's own free-fly camera entity, created by
// Editor::EditorCameraController::EnsureEditorCamera in Edit mode as a SceneTransientTag entity
// (never serialized, so editor navigation cannot pollute authored scenes). It joins the VCam
// stack at a very high priority while editing and is disabled in Play, where the scene's cameras
// take over. It deliberately has no CameraComponent, so RefreshMainCamera / audio-listener
// fallbacks never pick it as the scene camera.
struct EditorCameraTag : ComponentBase {};

// CameraControllerComponent - FPS-style controller state of a camera entity. CameraSystem drives
// the entity's LocalTransform from yaw / pitch (WASD translation, mouse rotation). yaw / pitch are
// the authoritative Euler state, so pitch clamping is singularity-free; the quaternion is
// mirrored onto LocalTransform.rotation.
struct CameraControllerComponent
{
    // Free (default): fly-cam, mouse rotates and WASD strafes. The follow modes are for gameplay
    // cameras on a player or NPC: the position derives from `followTarget` each frame, mouse input
    // drives yaw / pitch only, and WASD is ignored (the player controller owns horizontal motion).
    enum class Mode : std::uint8_t
    {
        Free        = 0,   // mouse-driven yaw/pitch + WASD position
        ThirdPerson = 1,   // orbit followTarget at thirdPersonDistance
        FirstPerson = 2,   // glue camera to followTarget + headOffset
    };

    float yaw              = -2.47f;  // radians, rotation around world-Y
    float pitch            =  0.44f;  // radians, rotation around cam-X (clamped to ~+-89 deg)
    float mouseSensitivity = 0.003f;
    float moveSpeed        = 10.0f;

    // ---- Follow-mode parameters (ignored when mode == Free) ----
    Mode          mode = Mode::Free;
    // Save-stable (GUID-based) follow target reference.
    AttachmentRef followTarget;
    // Distance from focus point to camera in ThirdPerson. The camera sits
    // *behind* the target along (-forward) by this many metres.
    float   thirdPersonDistance = 4.0f;
    // First-person eye position offset from the target's pivot. Default
    // ~1.65 m matches an average human eye height for a feet-pivoted mesh.
    DirectX::XMFLOAT3 headOffset = { 0.f, 1.65f, 0.f };
    // Third-person focus offset from the target's pivot: about chest height plus a small lateral
    // shoulder bias so the character is not dead-centred.
    DirectX::XMFLOAT3 shoulderOffset = { 0.30f, 1.60f, 0.f };

    // Third-person spring-arm collision: with a PhysicsSystem passed to ResolveFollowing, the camera
    // sweeps a sphere from the focus point toward its desired position and clamps the distance on a
    // hit so it does not poke through walls. Disable for cinematics that need a fixed camera.
    bool  cameraCollisionEnabled = true;
    // Sphere-cast probe radius (metres). Larger = camera backs off earlier
    // before clipping the wall. Match this to a typical near-plane buffer
    // (~10-30 cm) so the near-clip never enters the wall.
    float cameraProbeRadius     = 0.20f;

    // ---- Third-person follow smoothing (spring-arm damping) ----
    // The focus point (orbit pivot) eases toward the target, frame-rate independent
    // (alpha = 1 - exp(-dt / lag)); yaw / pitch are never smoothed, so mouse-look stays instant.
    //   followLag   - focus smoothing time constant in seconds; 0 = rigid (default), ~0.1 for a
    //                 cinematic trailing lag.
    //   distanceLag - ease-out time constant of the collision distance; 0 = instant (default), > 0
    //                 keeps an intermittent wall probe from punching the camera in / out.
    // Optional polish, off by default. Not the fix for follow jitter from a child mesh missing
    // physics interpolation: that is PhysicsSystem::ApplyRenderInterpolation via PropagateSubtree.
    float followLag   = 0.0f;
    float distanceLag = 0.0f;

    // ---- Transient smoothing state (not serialized) ----
    // followSmoothInit = false snaps on the first resolve (spawn / world load / mode switch);
    // smoothedFocus / smoothedDistance carry the filtered values across frames. Not serialized, so a
    // freshly loaded controller re-snaps.
    DirectX::XMFLOAT3 smoothedFocus    = { 0.f, 0.f, 0.f };
    float             smoothedDistance = 0.f;
    bool              followSmoothInit = false;
};
