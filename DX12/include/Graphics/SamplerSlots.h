#pragma once

// The fixed slots of the shader-visible sampler heap (SMP_LINEAR_CLAMP ...
// SMP_CMP_LINEAR_MIP_POINT_CLAMP_GE, SMP_FIXED_COUNT), defined once for C++ and HLSL in
// shaders/shared/sampler_slots.hlsli. GraphicsDX12::CreateFixedSamplers creates them
// first, so a slot is both the sampler pool index and the heap index the shaders read;
// a pass whose run-time sampler is a fixed one passes the slot straight to
// IGraphicsDevice::SetDynamicSampler.

#include "../../shaders/shared/sampler_slots.hlsli"
