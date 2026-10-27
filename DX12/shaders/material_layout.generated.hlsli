// material_layout.generated.hlsli — GENERATED FILE, DO NOT EDIT BY HAND.
//
// Byte offsets of every Resource::MaterialGPUData member, emitted from
// include/Resource/SystemHandles.h by tools/gen_material_layout.py.
// Regenerate with:  python tools/gen_material_layout.py
//
// WHO NEEDS THIS. Only a shader that reads the material table as a RAW
// ByteAddressBuffer, because it cannot declare
// StructuredBuffer<MaterialGPUData> — today that is exactly one shader,
// HybridReflIntersectHW.cs.hlsl, whose table arrives through the shared
// bindless g_Buffers[] array. Every other shader gets the struct by name
// from material.hlsli and must keep doing so.
//
// The generator cross-checks its own parse against the static_asserts in
// SystemHandles.h and refuses to emit on any disagreement, and
// tools/ci_check.py fails the build if this file has drifted from the
// struct. So editing MaterialGPUData is a two-file edit (the C++ struct
// and the HLSL mirror in material.hlsli) and this table follows.

#ifndef MATERIAL_LAYOUT_GENERATED_HLSLI
#define MATERIAL_LAYOUT_GENERATED_HLSLI

// Byte stride of ONE MaterialGPUData element == sizeof() of the C++ mirror.
#define MATERIAL_GPU_DATA_STRIDE 976u

#define MAT_OFF_roughnessMin                 0u
#define MAT_OFF_roughnessMax                 4u
#define MAT_OFF_metalnessMin                 8u
#define MAT_OFF_metalnessMax                 12u
#define MAT_OFF_reflectance                  16u
#define MAT_OFF_normalStrength               20u
#define MAT_OFF_saturation                   24u
#define MAT_OFF_alphaRef                     28u
#define MAT_OFF_baseColor                    32u   // 16 bytes
#define MAT_OFF_specularColor                48u   // 16 bytes
#define MAT_OFF_emissiveColor                64u   // 16 bytes
#define MAT_OFF_shaderType                   80u
#define MAT_OFF_nprMinBrightness             84u
#define MAT_OFF_nprShadowThreshold           88u
#define MAT_OFF_nprShadowSmooth              92u
#define MAT_OFF_nprRimPower                  96u
#define MAT_OFF_nprRimStrength               100u
#define MAT_OFF_nprRampBlend                 104u
#define MAT_OFF_nprBrightnessClamp           108u
#define MAT_OFF_nprDiffuseRampColor          112u   // 12 bytes
#define MAT_OFF_nprMaxBrightness             124u
#define MAT_OFF_nprShadowRampColor           128u   // 12 bytes
#define MAT_OFF__padNprShadow                140u
#define MAT_OFF_nprMidWeight                 144u
#define MAT_OFF_nprVeinWeight                148u
#define MAT_OFF_nprSSSWeight                 152u
#define MAT_OFF__padNprSss                   156u
#define MAT_OFF_textureHandleIds             160u   // 32 bytes
#define MAT_OFF_paramCount                   192u
#define MAT_OFF_textureCount                 196u
#define MAT_OFF_materialFlags                200u
#define MAT_OFF_exposureCompensation         204u
#define MAT_OFF_customParams                 208u   // 512 bytes
#define MAT_OFF_customTextureIds             720u   // 32 bytes
#define MAT_OFF_customParamCount             752u
#define MAT_OFF_customTextureCount           756u
#define MAT_OFF_customShadingModel           760u
#define MAT_OFF_toonRampTex                  764u
#define MAT_OFF_nprSpecThreshold             768u
#define MAT_OFF_nprSpecSmooth                772u
#define MAT_OFF_nprSpecStrength              776u
#define MAT_OFF_nprSpecRampBlend             780u
#define MAT_OFF_nprShadowStrength            784u
#define MAT_OFF_nprSceneShadowSmooth         788u
#define MAT_OFF_nprMainLightLerp             792u
#define MAT_OFF_nprDirectOcclusion           796u
#define MAT_OFF_nprRimWidth                  800u
#define MAT_OFF__nprPad0                     804u
#define MAT_OFF__nprPad1                     808u
#define MAT_OFF__nprPad2                     812u
#define MAT_OFF_uvTilingOffset               816u   // 16 bytes
#define MAT_OFF_nprDirectLightScale          832u
#define MAT_OFF_nprAdditionalLightScale      836u
#define MAT_OFF_nprAmbientLightScale         840u
#define MAT_OFF_nprSpecularLightScale        844u
#define MAT_OFF_nprSceneLightColorWeight     848u
#define MAT_OFF_nprSceneLightIntensityWeight 852u
#define MAT_OFF_nprReferenceLightLuminance   856u
#define MAT_OFF_nprExposureBiasEV            860u
#define MAT_OFF_nprShoulderStart             864u
#define MAT_OFF_nprShoulderRange             868u
#define MAT_OFF_nprShoulderStrength          872u
#define MAT_OFF__nprEnergyPad0               876u
#define MAT_OFF_anisotropy                   880u
#define MAT_OFF_anisotropyRotation           884u
#define MAT_OFF_subsurfaceWrap               888u
#define MAT_OFF_subsurfaceStrength           892u
#define MAT_OFF_subsurfaceColor              896u   // 12 bytes
#define MAT_OFF_subsurfaceOpacity            908u
#define MAT_OFF_clearCoat                    912u
#define MAT_OFF_clearCoatRoughness           916u
#define MAT_OFF__clearCoatPad0               920u
#define MAT_OFF__clearCoatPad1               924u
#define MAT_OFF_faceShadow                   928u   // 16 bytes
#define MAT_OFF_transmission                 944u
#define MAT_OFF_ior                          948u
#define MAT_OFF_thickness                    952u
#define MAT_OFF_attenuationDistance          956u
#define MAT_OFF_attenuationColor             960u   // 12 bytes
#define MAT_OFF__transmissionPad0            972u

#endif // MATERIAL_LAYOUT_GENERATED_HLSLI
