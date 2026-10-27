#ifndef SHARED_SAMPLER_SLOTS_HLSLI
#define SHARED_SAMPLER_SLOTS_HLSLI

// The fixed slots of the shader-visible sampler heap, one table for C++ and HLSL (plain
// #defines, valid in both).
//
// GraphicsDX12 creates these descriptions first, right after the sampler heap
// (CreateFixedSamplers), so the append-only, de-duplicating sampler pool hands out
// exactly these indices (checked at creation) and every later CreateSampler of the same
// description lands on them. Shaders open them as SamplerDescriptorHeap[SMP_...] through
// sampler_heap.hlsli; C++ reaches them through include/Graphics/SamplerSlots.h.
//
// UI's six come first, in UIDrawList's samplerId order (UIPass: filter-major,
// address-minor), so a UI draw's samplerId is its slot.
//
// Pure ASCII: the offline SDK dxc reads the system code page.

#define SMP_LINEAR_CLAMP                   0u  // MIN_MAG_MIP_LINEAR, CLAMP
#define SMP_LINEAR_WRAP                    1u  // MIN_MAG_MIP_LINEAR, WRAP
#define SMP_LINEAR_MIRROR                  2u  // MIN_MAG_MIP_LINEAR, MIRROR
#define SMP_POINT_CLAMP                    3u  // MIN_MAG_MIP_POINT, CLAMP
#define SMP_POINT_WRAP                     4u  // MIN_MAG_MIP_POINT, WRAP
#define SMP_POINT_MIRROR                   5u  // MIN_MAG_MIP_POINT, MIRROR
#define SMP_LINEAR_MIP_POINT_CLAMP         6u  // MIN_MAG_LINEAR_MIP_POINT, CLAMP
#define SMP_CMP_LINEAR_BORDER_WHITE_GE     7u  // COMPARISON_MIN_MAG_MIP_LINEAR, BORDER (opaque white), GREATER_EQUAL
#define SMP_CMP_LINEAR_MIP_POINT_CLAMP_GE  8u  // COMPARISON_MIN_MAG_LINEAR_MIP_POINT, CLAMP, GREATER_EQUAL
#define SMP_FIXED_COUNT                    9u

#endif // SHARED_SAMPLER_SLOTS_HLSLI
