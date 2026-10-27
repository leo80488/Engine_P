#ifndef SAMPLER_HEAP_HLSLI
#define SAMPLER_HEAP_HLSLI

// Samplers from the shader-visible sampler heap, for shaders under the shared graphics
// root signature. The signature sets SAMPLER_HEAP_DIRECTLY_INDEXED, so a sampler is an
// index, not a bound table:
//
//   SmpFixed(slot) / SmpCmp(slot) - a fixed slot, SMP_* from shared/sampler_slots.hlsli
//                                   (a compile-time index: no constant to read);
//   SmpDynamic()                  - the one sampler a pass picks at run time: the
//                                   material sampler (anisotropy + the upscale mip
//                                   bias, or LinearWrap where the shadow /
//                                   surface-cache / probe passes draw the same
//                                   materials), Terrain.ps's and Water.ps's, or a UI
//                                   draw's. Its heap index is a 1-DWORD root constant
//                                   the pass sets (IGraphicsDevice::SetDynamicSampler).
//
// A shader maps its own sampler names onto these with #defines, so sampling sites stay
// as written. Graphics only: the shared compute signature samples through its three
// static samplers (space2) and has no b0 space9.

#include "shared/sampler_slots.hlsli"

// RHI::Slots::Gfx::DynamicSampler - a 32-bit root constant.
cbuffer DynamicSamplerCB : register(b0, space9)
{
    uint g_DynamicSamplerIdx;
};

SamplerState           SmpFixed(uint slot) { return SamplerDescriptorHeap[slot]; }
SamplerComparisonState SmpCmp(uint slot)   { return SamplerDescriptorHeap[slot]; }
SamplerState           SmpDynamic()        { return SamplerDescriptorHeap[g_DynamicSamplerIdx]; }

#endif // SAMPLER_HEAP_HLSLI
