#pragma once

#include "Resource/ResourceHandle.h"
#include <cstdint>

// System-level opaque handles issued by the resource systems. They share the generational
// Handle struct; the ResourceType tag names the issuing system. Invalid sentinel: Handle{}
// (generation == 0).
namespace Resource
{
    using TextureHandle   = Handle;
    using MeshHandle      = Handle;
    using AnimHandle      = Handle;
    using AudioHandle     = Handle;

    constexpr TextureHandle  kInvalidTextureHandle  = Handle{};
    constexpr MeshHandle     kInvalidMeshHandle     = Handle{};
    constexpr AnimHandle     kInvalidAnimHandle2    = Handle{};  // kInvalidAnimHandle (uint32) is taken by AnimationComponents.h
    constexpr AudioHandle    kInvalidAudioHandle    = Handle{};

    constexpr uint32_t kMaxTextureSlots   = 8;
    // Per-material scratch for custom-shader parameters and textures, filled by the renderer from
    // MaterialComponent::customParams / customTextures when a custom GBuffer PS is active. Shaders read
    // g_Materials[materialIdx].customParams[N] / customTextureIds[N], packed in the shader's
    // declaration order (Renderer::WriteMaterialSlot). 8 textures cover an NPR skin shader's full set
    // (albedo / normal / surface / SSS / ramp / specular / occlusion / emissive).
    constexpr uint32_t kMaxCustomParams   = 32;  // 32 x float4 = 512 B
    constexpr uint32_t kMaxCustomTextures = 8;   //  8 x int32  =  32 B

    // GPU material record. Must be byte-for-byte identical to the HLSL MaterialGPUData in
    // shaders/material.hlsli: the renderer writes one and the GPU reads the other. Fields are named so
    // DXC reflection exposes them. Each block is 16 bytes so HLSL's natural alignment matches this
    // pack(1) struct.
#pragma pack(push, 1)
    struct MaterialGPUData
    {
        // ---- PBR scalars (16B per block) ----
        float    roughnessMin;
        float    roughnessMax;
        float    metalnessMin;
        float    metalnessMax;

        float    reflectance;
        float    normalStrength;
        float    saturation;
        float    alphaRef;

        // ---- PBR colors (linear-space) ----
        float    baseColor[4];
        float    specularColor[4];
        float    emissiveColor[4];

        // ---- NPR fields ----
        uint32_t shaderType;
        float    nprMinBrightness;
        float    nprShadowThreshold;
        float    nprShadowSmooth;

        float    nprRimPower;
        float    nprRimStrength;
        float    nprRampBlend;
        float    nprBrightnessClamp;

        // float3 + float = 16 B contiguous on both sides
        float    nprDiffuseRampColor[3];
        float    nprMaxBrightness;

        float    nprShadowRampColor[3];
        float    _padNprShadow;

        float    nprMidWeight;
        float    nprVeinWeight;
        float    nprSSSWeight;
        float    _padNprSss;

        // ---- Texture bindings + counts ----
        int32_t  textureHandleIds[kMaxTextureSlots];
        uint32_t paramCount;            // nonzero once the packer filled the engine fields; 0 -> vertex-colour preview defaults
        uint32_t textureCount;
        uint32_t materialFlags;         // MAT_FLAG_* bitmask (see material.hlsli)
        float    exposureCompensation;  // 0 = normal AE; NPR full range (1 = exposure-immune), non-NPR capped 0.5

        // ---- Custom-shader scratch (32 params / 8 textures) ----
        float    customParams[kMaxCustomParams][4];
        int32_t  customTextureIds[kMaxCustomTextures];
        uint32_t customParamCount;
        uint32_t customTextureCount;
        uint32_t customShadingModel;    // 0=Standard 1=Unlit 2=ClearCoat 3=Subsurface 4=Anisotropic
        // Bindless id of the toon ramp (-1 = none) for the deferred and forward toon and the RT
        // reflection hit: the custom texture the shader names `//@engine toonRamp`, else its t3.
        int32_t  toonRampTex;

        // ---- NPR specular ramp (toon highlight) - 16B ----
        // Stylizes the physical specular into a stepped anime highlight in the NPR lighting variant.
        float    nprSpecThreshold;
        float    nprSpecSmooth;
        float    nprSpecStrength;
        float    nprSpecRampBlend;      // 0 = physical specular, 1 = fully stylized

        // ---- DanbaidongRP toon alignment - 32B ----
        // Read only by Lighting.ps's toon-override block; mirrors the PBRToonBase material knobs
        // (semantics in the MaterialSchema rows).
        float    nprShadowStrength;     // lerp(1, formShadowArea, s) - _ShadowStrength
        float    nprSceneShadowSmooth;  // ToonVisibility edge half-width;
                                        // gated by MAT_FLAG_NPR_TOON_SHADOW_EDGE
        float    nprMainLightLerp;      // lerp(lightColor, white, t) - _MainLightColorLerp
        float    nprDirectOcclusion;    // directOcc = lerp(1-p, 1, ao) - _DirectOcclusion

        float    nprRimWidth;           // screen-space depth-rim width - _DirectRimWidth
        float    _nprPad0;
        float    _nprPad1;
        float    _nprPad2;

        // ---- UV transform - 16B ----
        // Material texture-coordinate transform uv' = uv * xy + zw, mirror of MaterialComponent::texMulAdd
        // (Inspector "UV Transform" -> Tiling/Offset; .imat keys texTiling/texOffset). Applied by
        // MaterialUV() in material.hlsli on every path that samples a material texture with UV0
        // (GBuffer/forward surface template, shadow alpha-test, probe capture).
        // The default must be (1,1,0,0) wherever a slot is initialised: a zeroed slot collapses every UV to
        // one texel.
        float    uvTilingOffset[4];

        // ---- NPR energy + exposed-HDR stability - 48B ----
        float    nprDirectLightScale;
        float    nprAdditionalLightScale;
        float    nprAmbientLightScale;
        float    nprSpecularLightScale;

        float    nprSceneLightColorWeight;
        float    nprSceneLightIntensityWeight;
        float    nprReferenceLightLuminance;
        float    nprExposureBiasEV;

        float    nprShoulderStart;
        float    nprShoulderRange;
        float    nprShoulderStrength;
        float    _nprEnergyPad0;

        // ---- Anisotropic + Subsurface shading models - 32B ----
        // Engine shading-model parameters, not custom-shader scratch: named members (with MaterialSchema
        // rows) so a plain material can author them. customParams[] exists only on a custom-shader material,
        // its Inspector block depends on shader reflection, and slots 0..2 are already claimed inside
        // Lighting.ps. Read only when customShadingModel selects the matching model.
        float    anisotropy;         // [-1,1]; + widens the lobe ALONG the tangent
        float    anisotropyRotation; // [0,1] turns, about the shading normal
        float    subsurfaceWrap;     // [0,1] diffuse terminator wrap; 0 = off
        float    subsurfaceStrength; // scales the transmission lobe; 0 = off

        float    subsurfaceColor[3]; // LINEAR (uploaded from the sRGB -> linear cache)
        float    subsurfaceOpacity;  // [0,1] default for the per-pixel RT4.r

        // ---- ClearCoat - 16B ----
        // A second thin dielectric specular layer. The coat's F0 is fixed at 0.04 (IOR 1.5) and not
        // authored: a coat with another F0 is a different material.
        float    clearCoat;          // [0,1] strength; 0 == Standard exactly
        float    clearCoatRoughness; // [0,1] the COAT's roughness, not the base's
        float    _clearCoatPad0;
        float    _clearCoatPad1;

        // ---- Face shadow - 16B ----
        // A V2 face's SDF terminator (offset, sharpness, strength, uvSet), read by the deferred toon: the
        // parameter the custom shader names `//@engine faceShadow`, else its customParams[2] (the V2
        // contract in face_sdf.hlsli). Zero on a material without one.
        float    faceShadow[4];

        // ---- Transmission - 32B ----
        // BlendMode::Transmissive (MaterialComponent's glTF transmission fields). Read only on a
        // Transmissive material; uploaded for every material like any schema scalar.
        float    transmission;        // [0,1] share of the dielectric base that refracts
        float    ior;                 // interior / exterior; the reflection's F0 follows it
        float    thickness;           // world units; 0 = thin-walled
        float    attenuationDistance; // world units; 0 = no absorption
        float    attenuationColor[3]; // LINEAR (uploaded from the sRGB -> linear cache)
        float    _transmissionPad0;
    };
#pragma pack(pop)
    static_assert(sizeof(MaterialGPUData) <= 2048,
                  "MaterialGPUData too large for one StructuredBuffer element slot");
    // 10 PBR blocks x 16B + 8x4B textureHandleIds + 4x4B counts/flags
    // + 32x16B customParams + 8x4B customTextureIds + 4x4B counts/model/ramp
    // + 4x4B NPR specular ramp + 8x4B Danbaidong toon alignment
    // + 4x4B UV transform (uvTilingOffset) + 12x4B NPR energy/stability
    // + 8x4B anisotropy/subsurface + 4x4B clear coat + 4x4B face shadow + 8x4B transmission
    // = 160 + 32 + 16 + 512 + 32 + 16 + 16 + 32 + 16 + 48 + 32 + 16 + 16 + 32 = 976 bytes
    // Changing the layout is a two-file edit: this struct and the HLSL mirror in shaders/material.hlsli.
    // shaders/material_layout.generated.hlsli (MATERIAL_GPU_DATA_STRIDE + MAT_OFF_*, which
    // HybridReflIntersectHW.cs.hlsl uses to read the table as a raw ByteAddressBuffer) follows: the build
    // regenerates it with tools/gen_material_layout.py and ci_check fails on drift. That tool parses this
    // struct as text and checks these static_asserts, so members stay 4-byte scalars or arrays of them.
    // A stale table shears every reflected NPR character's toon knobs with no build or runtime error.
    static_assert(sizeof(MaterialGPUData) == 976,
                  "MaterialGPUData layout drifted — HLSL mirror in material.hlsli must match");
    // sizeof cannot catch a reorder (two swapped 4-byte fields keep the size), so the tail offsets are
    // anchored too; they match the offsets DXC emits for the HLSL mirror (rawBufferLoad elementOffset).
    static_assert(offsetof(MaterialGPUData, nprSpecThreshold) == 768,
                  "MaterialGPUData tail moved — re-check material.hlsli");
    static_assert(offsetof(MaterialGPUData, nprRimWidth)      == 800,
                  "MaterialGPUData tail moved — re-check material.hlsli");
    static_assert(offsetof(MaterialGPUData, uvTilingOffset)   == 816,
                  "MaterialGPUData tail moved — re-check material.hlsli");
    static_assert(offsetof(MaterialGPUData, nprDirectLightScale) == 832,
                  "MaterialGPUData NPR energy block moved — re-check material.hlsli");
    static_assert(offsetof(MaterialGPUData, nprShoulderStart) == 864,
                  "MaterialGPUData NPR shoulder block moved — re-check material.hlsli");
    static_assert(offsetof(MaterialGPUData, anisotropy) == 880,
                  "MaterialGPUData shading-model block moved — re-check material.hlsli");
    static_assert(offsetof(MaterialGPUData, subsurfaceColor) == 896,
                  "MaterialGPUData subsurface block moved — re-check material.hlsli");
    static_assert(offsetof(MaterialGPUData, clearCoat) == 912,
                  "MaterialGPUData clear-coat block moved — re-check material.hlsli");
    static_assert(offsetof(MaterialGPUData, toonRampTex) == 764,
                  "MaterialGPUData toon ramp moved — re-check material.hlsli");
    static_assert(offsetof(MaterialGPUData, faceShadow) == 928,
                  "MaterialGPUData face-shadow row moved — re-check material.hlsli");
    static_assert(offsetof(MaterialGPUData, transmission) == 944,
                  "MaterialGPUData transmission block moved - re-check material.hlsli");
}
