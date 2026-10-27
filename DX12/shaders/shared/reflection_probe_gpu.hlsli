// One reflection probe as the GPU reads it: the element of the probe StructuredBuffer.
// C++ reads it as Reflection::GPUReflectionProbe (include/Graphics/ReflectionProbeTypes.h),
// the shaders as ReflectionProbe (reflection_probe.hlsli, ClusterCullProbes.cs).
//
// 192 bytes, 12 rows of 16, typed fields (a float3 and a trailing scalar per row) so the
// shaders read them without asfloat. worldToProxy / worldToInfl are rigid (rotation +
// translation, scale stripped) world->local transforms as three float4 rows, so a ray t
// solved in local space is the world distance - the OBB slab parallax and the Lagarde
// roughness correction rely on it. Renderer_DDGI.cpp packs the buffer; cubemapSrvIndex is
// an absolute shader-visible heap index for ResourceDescriptorHeap[] (SM 6.6).
//
// Keep this file pure ASCII (gpu_struct_types.hlsli).

#ifndef SHARED_REFLECTION_PROBE_GPU_HLSLI
#define SHARED_REFLECTION_PROBE_GPU_HLSLI

#include "gpu_struct_types.hlsli"

GPU_SHARED_BEGIN

struct GPU_SHARED_ALIGN16 ReflectionProbe
{
    float4 worldToProxyR0, worldToProxyR1, worldToProxyR2;  // [0..48)
    float4 worldToInflR0,  worldToInflR1,  worldToInflR2;   // [48..96)
    float3 proxyExtents;    uint  proxyShape;   // [96..112)  box half extents or (radius,0,0); EProxyShape
    float3 capturePosWS;    float mipCount;     // [112..128) capture point (world); probe cube mip count
    float3 inflExtents;     uint  inflShape;    // [128..144) box half extents or (outerR,innerR,0); EInfluenceShape
    float3 fadePos;         float intensity;    // [144..160) +X/+Y/+Z inward fade dist (m); radiance multiplier
    float3 fadeNeg;         uint  priority;     // [160..176) -X/-Y/-Z inward fade dist (m); priority (debug only)
    uint   cubemapSrvIndex;                     // [176..180) absolute shader-visible heap index (bindless)
    uint   flags;                               // [180..184) GPUProbeFlag / REFLECTION_PROBE_FLAG_* bits
    uint2  _pad;
};

GPU_SHARED_SIZE(ReflectionProbe, 192)
GPU_SHARED_OFFSET(ReflectionProbe, worldToInflR0, 48)
GPU_SHARED_OFFSET(ReflectionProbe, proxyExtents, 96)
GPU_SHARED_OFFSET(ReflectionProbe, proxyShape, 108)
GPU_SHARED_OFFSET(ReflectionProbe, capturePosWS, 112)
GPU_SHARED_OFFSET(ReflectionProbe, mipCount, 124)
GPU_SHARED_OFFSET(ReflectionProbe, inflExtents, 128)
GPU_SHARED_OFFSET(ReflectionProbe, inflShape, 140)
GPU_SHARED_OFFSET(ReflectionProbe, fadePos, 144)
GPU_SHARED_OFFSET(ReflectionProbe, intensity, 156)
GPU_SHARED_OFFSET(ReflectionProbe, fadeNeg, 160)
GPU_SHARED_OFFSET(ReflectionProbe, priority, 172)
GPU_SHARED_OFFSET(ReflectionProbe, cubemapSrvIndex, 176)
GPU_SHARED_OFFSET(ReflectionProbe, flags, 180)

GPU_SHARED_END

#endif // SHARED_REFLECTION_PROBE_GPU_HLSLI
