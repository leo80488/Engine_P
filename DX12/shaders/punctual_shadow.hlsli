#ifndef PUNCTUAL_SHADOW_HLSLI
#define PUNCTUAL_SHADOW_HLSLI
// Spot and point light shadows - the one implementation, shared by the deferred pass
// (Lighting.ps) and the forward translucent path's clustered loop (custom_surface.hlsli
// MS_ForwardLighting), so both shade a lamp's shadow identically.
// This file is the graphics binding: it declares the spot-shadow view buffer (t22,
// Gfx::SpotShadowVP), opens the atlases from the heap and calls the math in
// punctual_shadow_eval.hlsli, which takes all of them as parameters. The includer declares,
// before including this:
//   * LightCB (light_cb.hlsli): cameraPos, shadowNormalOffset, pointShadowIdx;
//   * SamplerComparisonState gShadowSampler (s2) - reversed-Z GREATER_EQUAL.
#include "punctual_shadow_eval.hlsli"
#include "bindless.hlsli"   // typed bindless handles

StructuredBuffer<SpotShadowView>  SpotShadowVPs   : register(t22, space0);

float SampleSpotShadow(float3 worldPos, float3 worldDx, float3 worldDy, float3 N, float3 lightPos,
                       float spotHalfAngle, uint sliceIdx)
{
    const SpotShadowView shadowView = SpotShadowVPs[sliceIdx];
    if (shadowView.atlasIndex == 0xFFFFFFFFu) return 1.0;
    Texture2DArray<float> atlas = ResourceDescriptorHeap[NonUniformResourceIndex(shadowView.atlasIndex)];
    return SpotShadowFromAtlas(shadowView, atlas, worldPos, worldDx, worldDy, N, lightPos,
                               spotHalfAngle, cameraPos, shadowNormalOffset);
}

// Per-point-light omnidirectional (cubemap) shadow (opt-in via LightData::castsShadow).
// One cube (6 faces) per active caster, addressed by light.shadowSliceIdx (the same field
// spot lights use for their atlas slice: a light is point XOR spot). Reversed-Z depth.
// PointShadowPass renders the faces with a 90 deg perspective each, NearZ = radius,
// FarZ = POINT_SHADOW_NEAR (the SpotShadowPass reversed-Z convention). The hardware cube
// sampler picks the face + texel from the world-space direction; the matching reversed-Z
// reference depth is reconstructed from the fragment's distance, so no per-light VP matrix.
// No register declaration: the atlas is opened from ResourceDescriptorHeap[pointShadowIdx]
// (it cannot live in g_AllTextures[] at t0 space2, which is declared Texture2D). The
// comparison sampler stays a root-bound s2: ResourceDescriptorHeap covers CBV/SRV/UAV only.
float SamplePointShadow(float3 worldPos, float3 N, float3 lightPos, float radius, uint cubeIdx)
{
    // pointShadowIdx is wave-uniform (CB-sourced): no NonUniformResourceIndex.
    // TextureCubeArray<float> must match what LightingPass created: the live PointShadowPass
    // atlas is D32_FLOAT with array_size = 6*kMaxCasters and misc TEXTURECUBE -> an R32_FLOAT
    // TEXTURECUBEARRAY SRV, and the fallback is a 1x1 12-slice cube array of the same shape. A
    // wrong element type gives no D3D12 validation message under Tier 3, only garbage or a TDR.
    // Opened inside the function so the read stays behind the caller's gate
    // (light.type == 1 && light.shadowSliceIdx != 0xFFFFFFFF).
    TextureCubeArray<float> pointShadowAtlas = BindlessTexCubeArray<float>(pointShadowIdx);
    return PointShadowFromCube(pointShadowAtlas, gShadowSampler, worldPos, N, lightPos, radius, cubeIdx);
}

#endif // PUNCTUAL_SHADOW_HLSLI
