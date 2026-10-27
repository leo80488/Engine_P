#pragma once

// The clustered-light record (64 bytes) C++ fills and the shaders read:
// GpuShared::GPULight, defined once for both languages in
// shaders/shared/gpu_light.hlsli. ClusterPass, the preview layer
// (Preview::GPULight) and the volumetric fog (VolumetricFogPass::VolLight) name it;
// the last two words are IEEE floats in integer storage - write them with SetPower /
// SetSourceRadius.

#include "../../shaders/shared/gpu_light.hlsli"
