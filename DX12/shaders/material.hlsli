#ifndef MATERIAL_HLSLI
#define MATERIAL_HLSLI

// material.hlsli - GPU-side material data layout, mirroring C++ Resource::MaterialGPUData
// (include/Resource/SystemHandles.h). Engine PBR + NPR fields are named members, so DXC
// reflection on StructuredBuffer<MaterialGPUData> surfaces them by name for the inspector.
// Each field block is a multiple of 16 bytes so HLSL natural alignment and #pragma pack(1)
// on the C++ side land at the same offsets: keep the pad fields when reordering.

// Shader type values (must match C++ MaterialComponent::SHADERTYPE).
#define SHADER_PBR        0
#define SHADER_UNLIT      2
#define SHADER_NPR_RAMP   4
#define SHADER_NPR_COLOR  5

// Texture handle indices (material.textureHandleIds[N]). Must match the
// MATERIAL_TEXTURE_SLOTS enum order in ECS/Components.h. Metallic/roughness/AO are packed
// in SURFACEMAP:
//   0 BASECOLORMAP  1 NORMALMAP  2 SURFACEMAP  3 EMISSIVEMAP  4 RAMPMAP
//   5 OPACITYMAP (Masked mode opacity mask, .r; -1 = unbound -> mask 1.0)
#define MAT_TEX_EMISSIVEMAP 3
#define MAT_TEX_RAMPMAP     4
#define MAT_TEX_OPACITYMAP  5

// Custom-texture slot (material.customTextureIds[N], t<N> space3) where a toon shader
// that does not name its ramp `//@engine toonRamp` keeps it. No lighting path reads the
// slot: the material packer (Renderer::WriteMaterialSlot, rampSlot) copies the ramp's id into
// MaterialGPUData::toonRampTex, which the deferred toon, the forward toon and the RT
// reflection hit read.
#define MAT_CUSTOM_TEX_FACE_RAMP 3u

struct MaterialGPUData
{
    // ---- PBR scalars (16B per block) ----
    float roughnessMin;      // remap: lerp(min, max, surfaceMap.g); metalness likewise from .b
    float roughnessMax;
    float metalnessMin;
    float metalnessMax;

    float reflectance;       // F0 = 0.16 * reflectance^2 for dielectrics
    float normalStrength;
    float saturation;
    float alphaRef;

    // ---- PBR colors (linear-space, from MaterialComponent's cache) ----
    float4 baseColor;        // .rgb tint, .a = master alpha
    float4 specularColor;    // .rgb specular F0 override (specular-glossiness path)
    float4 emissiveColor;    // .rgb tint, .a = strength

    // ---- NPR (only meaningful when shaderType == SHADER_NPR_*) ----
    uint   shaderType;       // see SHADER_* enum above
    float  nprMinBrightness; // also the "is NPR" flag in the normal.w encoding
    float  nprShadowThreshold;
    float  nprShadowSmooth;

    float  nprRimPower;
    float  nprRimStrength;
    float  nprRampBlend;     // 0 = baseColor only, 1 = full ramp
    float  nprBrightnessClamp; // relative ceiling x albedo luminance; gated by MAT_FLAG_NPR_BRIGHTNESS_CLAMP

    // float3 + float = 16B, no internal padding under HLSL natural alignment.
    float3 nprDiffuseRampColor;
    float  nprMaxBrightness; // absolute HDR ceiling (0 = none); min()s with LightCB.nprMaxBrightness

    float3 nprShadowRampColor;
    float  _padNprShadow;

    float  nprMidWeight;     // skin layer blend weights
    float  nprVeinWeight;
    float  nprSSSWeight;
    float  _padNprSss;

    // ---- Texture bindings + counts ----
    int    textureHandleIds[8];   // bindless indices; -1 if unloaded (32B)
    uint   paramCount;            // nonzero once the packer filled the engine fields; 0 -> vertex-colour preview defaults
    uint   textureCount;
    uint   materialFlags;         // MAT_FLAG_* bitmask
    // 0 = normal AE. Toon and Unlit take the full 0..1 range (1 = the counter-scale exactly
    // cancels ToneMap's exposure multiply, so the pixel is 100% authored); every lit model is
    // capped at 0.5 at runtime. Applied by NPRStabilize.cs.hlsl for deferred surfaces and, since
    // those composite after NPRLitSnapshot, in custom_surface.hlsli's unlit branch for forward ones.
    float  exposureCompensation;

    // ---- Custom-shader scratch (32 params, 8 textures) ----
    float4 customParams[32];      // 512B
    int    customTextureIds[8];   // 32B
    uint   customParamCount;
    uint   customTextureCount;
    uint   customShadingModel;    // SHADING_MODEL_* below
    // Bindless id of the toon ramp (-1 = none), resolved by the material packer: the custom
    // texture the shader names `//@engine toonRamp`, else its t3 (MAT_CUSTOM_TEX_FACE_RAMP).
    int    toonRampTex;

    // ---- NPR specular ramp (toon highlight) - must match C++ tail ----
    float  nprSpecThreshold;
    float  nprSpecSmooth;
    float  nprSpecStrength;
    float  nprSpecRampBlend;      // 0 = physical specular, 1 = fully stylized

    // ---- DanbaidongRP toon alignment - must match C++ tail (32B) ----
    float  nprShadowStrength;     // lerp(1, formShadowArea, s) - _ShadowStrength
    float  nprSceneShadowSmooth;  // ToonVisibility edge half-width; 0 = raw PCF penumbra.
                                  // Read ONLY when MAT_FLAG_NPR_TOON_SHADOW_EDGE is set.
    float  nprMainLightLerp;      // lerp(lightColor, white, t) - _MainLightColorLerp
    float  nprDirectOcclusion;    // directOcc = lerp(1-p, 1, ao) - _DirectOcclusion

    float  nprRimWidth;           // screen-space depth-rim width - _DirectRimWidth
    float  _nprPad0;
    float  _nprPad1;
    float  _nprPad2;

    // ---- UV transform - must match C++ tail (16B) ----
    // uv' = uv * xy + zw. Mirror of MaterialComponent::texMulAdd. Sample it through
    // MaterialUV() below rather than unpacking it by hand.
    float4 uvTilingOffset;

    // ---- NPR energy + exposed-HDR stability - must match C++ tail (48B) ----
    float  nprDirectLightScale;
    float  nprAdditionalLightScale;
    float  nprAmbientLightScale;
    float  nprSpecularLightScale;

    float  nprSceneLightColorWeight;
    float  nprSceneLightIntensityWeight;
    float  nprReferenceLightLuminance;
    float  nprExposureBiasEV;

    float  nprShoulderStart;
    float  nprShoulderRange;
    float  nprShoulderStrength;
    float  _nprEnergyPad0;

    // ---- Anisotropic + Subsurface shading models - must match C++ tail (32B) ----
    // Engine shading-model parameters, not custom-shader scratch: named MaterialGPUData members
    // with MaterialSchema rows, so a plain (non-custom) material can author them from the
    // Inspector (customParams exist only on custom-shader materials, and their Inspector block
    // is gated on shader reflection). Read only when customShadingModel selects the matching
    // model (SHADING_MODEL_* below); inert defaults elsewhere. See ShadingModel.h. Two-sided
    // foliage reads wrap/strength/colour here too; on a custom shader the material packer fills
    // them from its foliage parameters.
    float  anisotropy;           // [-1,1]; 0 = isotropic. + widens the lobe ALONG
                                 //   the tangent (UE's convention).
    float  anisotropyRotation;   // [0,1] turns; rotates the mesh tangent about the
                                 //   normal before it becomes the anisotropy axis.
    float  subsurfaceWrap;       // [0,1] diffuse terminator wrap. 0 = off exactly.
    float  subsurfaceStrength;   // scales the whole transmission lobe. 0 = off.

    float3 subsurfaceColor;      // LINEAR tint of the transmitted light
    float  subsurfaceOpacity;    // [0,1] default for the per-pixel RT4.r channel:
                                 //   1 = solid/thick, 0 = thin and see-through.

    // ---- ClearCoat - must match C++ tail (16B) ----
    float  clearCoat;            // [0,1] coat strength; 0 == Standard exactly
    float  clearCoatRoughness;   // [0,1] the COAT's roughness, not the base's
    float  _clearCoatPad0;
    float  _clearCoatPad1;

    // ---- Face shadow - must match C++ tail (16B) ----
    // A V2 face's SDF terminator (offset, sharpness, strength, uvSet) for the deferred toon:
    // the parameter the custom shader names `//@engine faceShadow`, else its customParams[2].
    // Zero elsewhere.
    float4 faceShadow;

    // ---- Transmission - must match C++ tail (32B) ----
    // BlendMode::Transmissive only: the glTF transmission / volume / ior fields.
    float  transmission;         // [0,1] share of the dielectric base that refracts
    float  ior;                  // interior / exterior; the reflection's F0 follows it
    float  thickness;            // world units; 0 = thin-walled
    float  attenuationDistance;  // world units; 0 = no absorption
    float3 attenuationColor;     // LINEAR colour light keeps after attenuationDistance
    float  _transmissionPad0;
};

// ==== Byte offsets: deliberately not here ====
// HybridReflIntersectHW.cs.hlsl reads the material table as a raw ByteAddressBuffer (it
// arrives through the shared bindless ByteAddressBuffer array), so it indexes
// `slot * stride + fieldOffset` by hand. Those offsets live in
// shaders/material_layout.generated.hlsli, emitted from Resource::MaterialGPUData by
// tools/gen_material_layout.py and cross-checked against its static_asserts
// (EngineCore.vcxproj regenerates it, ci_check.py fails on drift). Editing this struct is a
// two-file edit: the C++ mirror and this one.
// Do not re-add a stride or offset constant here: a hand-kept copy breaks silently when a
// field is appended or inserted. Every other shader uses the struct by name.

// Material-level UV transform (Inspector "UV Transform" -> Tiling / Offset).
// Every material-texture fetch from the mesh's UV0 must go through this, or the Tiling knob
// silently does nothing on that path. Callers include custom_surface.hlsli's MS_MakeParams
// (GBuffer, forward transparent, every custom-surface hook), Shadow.ps's alpha test and
// ProbeCapture.ps. UV1 is deliberately not transformed: it is a second UV set (lightmap /
// detail) whose parameterisation is independent of the base-map tiling.
float2 MaterialUV(MaterialGPUData mat, float2 uv)
{
    return uv * mat.uvTilingOffset.xy + mat.uvTilingOffset.zw;
}

// Per-material GPU flag bitmask (MaterialGPUData.materialFlags), written by
// Renderer::WriteMaterialSlot from MaterialComponent::FLAGS in Components.h. Keep in sync.
#define MAT_FLAG_EXCLUDE_FROM_SSAO       (1u << 0)   // pixel is skipped by XeGTAO
#define MAT_FLAG_SOURCE_OUTLINE_EXEMPT   (1u << 7)   // source postprocess visible-surface exclusion
#define MAT_FLAG_DISABLE_RECEIVE_SHADOW  (1u << 1)   // pixel ignores CSM (always lit)
#define MAT_FLAG_USE_VERTEXCOLOR         (1u << 2)   // multiply baseColor by per-vertex color (MaterialComponent::USE_VERTEXCOLORS)
// Opt into the relative NPR brightness ceiling (nprBrightnessClamp x albedo luminance) in
// Lighting.ps's NPR_PASS. A flag, not `nprBrightnessClamp > 0`: a value gate with the 1.0
// default pins every NPR pixel to its own albedo (no highlight, no response to light).
#define MAT_FLAG_NPR_BRIGHTNESS_CLAMP    (1u << 3)
// GBuffer writes RT4 extra.b = 1 (hair mask) so FaceOverlayPass can redraw brows/lashes
// over this material's pixels: the opt-in for plain (non-custom-shader) hair materials;
// custom hair shaders write s.extra themselves.
#define MAT_FLAG_HAIR_MASK               (1u << 4)
// Opt into the stylized cast-shadow edge in the NPR toon block: directVisibility goes
// through ToonVisibility() (lighting_types.hlsli) with half-width nprSceneShadowSmooth
// instead of a raw saturate(). ToonVisibility is endpoint-pinned (exactly 0 at 0, 1 at 1);
// do not use SigmoidSharp here: SigmoidSharp(0, 0.5, 0.1) = 0.15 leaks 15% of the sun into
// full shadow. See VIS-LINEAR in Lighting.ps.hlsl.
// Flag-gated, not gated on the float being non-zero: every NPR .imat on disk carries a
// non-zero nprSceneShadowSmooth, so a value gate would turn it on everywhere at once. See
// MaterialComponent::NPR_TOON_SHADOW_EDGE.
#define MAT_FLAG_NPR_TOON_SHADOW_EDGE    (1u << 5)
// BlendMode::Dithered (stochastic transparency on the deferred route). Set by WriteMaterialSlot
// from userBlendMode, not a MaterialComponent::_flags bit (mirror: kMatGpuFlagDitheredCoverage
// in Components.h). custom_surface.hlsli's ALPHA_TEST block clips coverage against a
// temporal dither instead of alphaRef; Shadow.ps against a static one. See dither.hlsli.
#define MAT_FLAG_DITHERED_COVERAGE       (1u << 6)
// This material slot has a frame record: a PER_FRAME_PARAMS face whose head frame
// (HeadForward.xz, HeadRight.xz) the deferred toon reads from
// BindlessSBuf<float4>(gFaceFrameIdx)[matIdx] instead of customParams[0..1]. Set only by
// the main emit (mirror: kMatGpuFlagFrameRecord in Components.h).
#define MAT_FLAG_FRAME_RECORD            (1u << 8)
// The material's custom pixel shader reads the frame's time: it animates without moving geometry.
// Set by WriteMaterialSlot from the shader's reflection; read by the path tracer's motion probe
// (mirror: kMatGpuFlagReadsTime in Components.h).
#define MAT_FLAG_READS_TIME              (1u << 9)

// Shading-model constants - must match C++ enum ShadingModel.
#define SHADING_MODEL_STANDARD     0u
#define SHADING_MODEL_UNLIT        1u
#define SHADING_MODEL_CLEARCOAT    2u
// Subsurface. Wrap-softened diffuse + a per-light transmission lobe, tinted by the named
// subsurfaceColor/Wrap/Strength/Opacity fields above. Per-pixel data rides RT4: .r = opacity
// (SurfaceData::subsurfaceOpacity, 1 = solid), .g = a [0,1] mask (SurfaceData::subsurfaceMask)
// so a hook can paint scatter onto ears/nostrils only. The engine writes both in
// MS_EncodeGBuffer: a hook fills the SurfaceData fields, never RT4 directly.
#define SHADING_MODEL_SUBSURFACE   3u
// Anisotropic GGX. Per-pixel data rides RT4: .r = the anisotropy direction as an angle in
// the shading normal's tangent plane, .g = the signed anisotropy. The hook fills
// SurfaceData::anisotropy / ::anisotropyDirWS and the engine packs. Codec: shading_models.hlsli.
#define SHADING_MODEL_ANISOTROPIC  4u
// Water surface. Not a deferred lighting model: the dedicated forward WaterPass renders
// water (Fresnel sky reflection + flow normals + SSR + interactive ripple normals), so the
// deferred Lighting.ps never sees it. A reserved tag for the water surface system.
#define SHADING_MODEL_WATER        5u
// Two-sided foliage (Unreal's TwoSidedFoliage). Adds a transmission lobe on top of the
// standard result: light hitting the far side of a thin leaf glows out toward the viewer.
// Lit with -N and needs no thickness channel (unlike SUBSURFACE), so it costs nothing in the
// GBuffer. Parameters are the named subsurfaceColor/Wrap/Strength - see ShadingModel.h.
#define SHADING_MODEL_TWOSIDED_FOLIAGE 6u
// Cel / anime shading, covering both NPR_RAMP and NPR_COLOR shaderTypes: the ramp source is
// whether the material binds a toon ramp (toonRampTex >= 0), as toonCtx.active and the RT
// reflection hit shader key on. Selects the NPR deferred permutation via ShadingStencil::RefFor.
#define SHADING_MODEL_TOON             7u

#endif // MATERIAL_HLSLI
