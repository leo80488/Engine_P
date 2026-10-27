// GBuffer.ps.hlsl - PBR geometry pass pixel shader.
//
// This PS is the engine-owned surface template with the WEAK-DEFAULT material hook
// (standard PBR). Texture sampling, normal-map / TBN math, masked alpha-test,
// geometric specular AA, TAA velocity and the 6-MRT GBuffer encode live in
// custom_surface.hlsli / material_surface.hlsli, shared with every custom material
// shader. A custom material replaces ONLY the EvaluateMaterial hook (see
// custom_surface.hlsli) and reuses this same main() + GBuffer encode, so it can
// never desync the RT layout below.
//
// Render target layout (world position is reconstructed from depth in Lighting):
//   TARGET0  albedo    R11G11B10_FLOAT     - baseColor (no emissive - see TARGET5)
//   TARGET1  normal    R16G16B16A16_UNORM  - world normal packed to [0,1],
//                                             .a = exact 16-bit int / 65535:
//                                             bits 0..14 = matIdx, bit 15 = EXCLUDED
//                                             from XeGTAO. UNORM (not FLOAT) because
//                                             FP16 holds integers exactly only up to
//                                             2048 and material slots go above that
//                                             (default cap 4096).
//   TARGET2  surface   R8G8B8A8_UNORM      - roughness(R), metalness(G), AO(B), reflectance(A)
//   TARGET3  velocity  R16G16_FLOAT        - NDC motion vector
//   TARGET4  extra     R16G16B16A16_FLOAT  - shading-model scratch (SSS thickness, NPR term...)
//   TARGET5  sceneCol  R16G16B16A16_FLOAT  - HdrSceneColor: emissive seed for
//                                             Unreal-style additive lighting (emissive
//                                             never passes through the BRDF).
//
// Surface map input channels: R=AO, G=roughness, B=metalness. Roughness / metalness
// are REMAPPED via lerp(min, max, textureValue) from the material constant buffer.
//
// Permutations handled by the template: ALPHA_TEST (masked clip), DOUBLE_SIDED
// (back-face normal flip via SV_IsFrontFace). BILLBOARD / SKINNED are vertex-only.

#include "custom_surface.hlsli"
