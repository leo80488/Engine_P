// shared/gpu_light.hlsli - one clustered light as the GPU reads it: the element
// of the light StructuredBuffer read by the cluster cull, the deferred / forward
// lighting, the volumetric fog and ReSTIR DI / PT. C++ fills it in ClusterPass
// (the main view), the preview layer (Preview::GPULight, packed on the game half)
// and the volumetric fog (VolumetricFogPass::VolLight); include/Graphics/GpuLight.h
// is the C++ door. One definition for both languages: do not copy it.
//
// Keep this file pure ASCII (gpu_struct_types.hlsli).

#ifndef SHARED_GPU_LIGHT_HLSLI
#define SHARED_GPU_LIGHT_HLSLI

#include "gpu_struct_types.hlsli"

GPU_SHARED_BEGIN

// 64 bytes, four rows.
struct GPULight
{
    float3 position;   float radius;
    float3 color;      float intensity;
    float3 direction;  float spotAngle;      // half-angle in radians
    uint   type;                             // 0 = directional, 1 = point, 2 = spot (LightType)
    uint   shadowSliceIdx;                   // 0xFFFFFFFF = no shadow map; else the spot atlas slice
    // Two IEEE floats in integer storage: the integer type keeps every consumer's
    // vector load of the record a plain uint load. The samplers decode them with
    // asfloat().
    uint   powerBits;                        // RIS proposal power; 0 for a directional light
    uint   sourceRadiusBits;                 // a point / spot's emitter radius, 0 = punctual
#ifdef __cplusplus
    void  SetPower(float p)        { powerBits = std::bit_cast<uint32_t>(p); }
    void  SetSourceRadius(float r) { sourceRadiusBits = std::bit_cast<uint32_t>(r); }
    float Power() const            { return std::bit_cast<float>(powerBits); }
    float SourceRadius() const     { return std::bit_cast<float>(sourceRadiusBits); }
#endif
};

GPU_SHARED_SIZE(GPULight, 64)
GPU_SHARED_OFFSET(GPULight, radius, 12)
GPU_SHARED_OFFSET(GPULight, color, 16)
GPU_SHARED_OFFSET(GPULight, direction, 32)
GPU_SHARED_OFFSET(GPULight, spotAngle, 44)
GPU_SHARED_OFFSET(GPULight, type, 48)
GPU_SHARED_OFFSET(GPULight, shadowSliceIdx, 52)
GPU_SHARED_OFFSET(GPULight, powerBits, 56)
GPU_SHARED_OFFSET(GPULight, sourceRadiusBits, 60)

GPU_SHARED_END

#endif // SHARED_GPU_LIGHT_HLSLI
