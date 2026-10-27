// Forward PBR pixel shader for alpha-blended geometry: the engine surface template's FORWARD
// variant with the weak-default (standard PBR) material hook. Material evaluation (textures,
// normal/TBN, roughness/metal/AO, emissive) is material_surface.hlsli's EvaluateMaterial; the
// forward BRDF + IBL + reflection probes + glass-split premultiplied output are custom_surface.hlsli's
// MS_IS_FORWARD branch. A custom translucent material replaces ONLY EvaluateMaterial and reuses this
// forward shading.
// Drawn by TransparentPass after GBuffer + Lighting + Skybox, with GBuffer.vs.hlsl vertices (same
// PVF layout). The blend mode is a PSO permutation (ALPHA_BLEND / ADDITIVE_BLEND /
// PREMULTIPLIED_BLEND / MULTIPLY_BLEND) and the forward output honours each: PREMULTIPLIED returns
// premultiplied rgb, the default ALPHA_BLEND pre-divides so SRC_ALPHA * rgb reproduces it.
// Output: SV_TARGET0 = float4(lit HDR radiance, surface alpha).
// Root bindings the PS reads (must match TransparentPass.cpp): b0 PushConstants, b1 PerViewCB,
// b2 LightCB, b4 fog CB, t0 InstanceBuffer (dissolve params), t2 MaterialGPUData, t3/t4/t5
// material textures, t6 gIrradiance, t7 gRadiance, t8 gBRDFLUT, t9 CSM, t13-t15 clustered
// lights, t19 gSkySH, t22 spot-shadow views, t0 space2 bindless. No root samplers: the material
// sampler is the pass's dynamic sampler-heap slot (SmpDynamic; its index is the b0 space9 root
// constant SetDynamicSampler sets), every other sampler (IBL, fog, CSM comparison) is a fixed
// heap slot. Reflection probes bind nothing: they are read through ResourceDescriptorHeap from
// LightCB.reflProbeBufIdx.

#define SURFACE_FORWARD
#include "custom_surface.hlsli"
