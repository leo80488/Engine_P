#pragma once

// Reflection-probe CPU<->GPU types and renderer-wide constants.
// The GPU layout of one probe is defined ONCE, for C++ and the shaders alike, in
// shaders/shared/reflection_probe_gpu.hlsli; this header names it
// GpuShared::ReflectionProbe. The probe StructuredBuffer is packed by
// Renderer::BuildScene_UploadProbes (Renderer_DDGI.cpp) and sampled bindlessly:
// each probe carries an ABSOLUTE shader-visible-heap index (`cubemapSrvIndex`) that
// shaders feed to ResourceDescriptorHeap[NonUniformResourceIndex(idx)] (SM6.6;
// needs the HEAP_DIRECTLY_INDEXED root-sig flag, see
// GraphicsDX12::CreateDefaultRootSignature).

#include <cstdint>
#include <DirectXMath.h>

#include "../../shaders/shared/reflection_probe_gpu.hlsli"

namespace Reflection
{
    // Total probe cap. Every probe is a view over one shared cube array, so this also
    // sizes that array (kMaxReflectionProbes*6 slices). The CPU full-sort assumes
    // count < 256; the shared-array VRAM is the limiter (it grows linearly).
    // TODO(reflection_probe_proxy_influence_impl_plan M4): standalone per-probe cubes,
    // which lift this coupling.
    inline constexpr uint32_t kMaxReflectionProbes = 64;

    // Default per-face cubemap resolution (prefilter mip 0); every probe bakes at it.
    // TODO(reflection_probe_proxy_influence_impl_plan M4): per-probe faceResolution
    // overrides it once each probe has its own cube.
    inline constexpr uint32_t kProbeCubemapSize = 128;

    // log2(128) + 1 - the same chain depth as SkyIBLPass::kSpecularMips.
    inline constexpr uint32_t kProbeCubemapMips = 7;

    inline constexpr uint32_t kInvalidProbeSlice = ~0u;

    // GPU layout of one probe: 192 bytes (12 x float4), defined in
    // shaders/shared/reflection_probe_gpu.hlsli with its offsets asserted there.

    // Flag bits stored in GpuShared::ReflectionProbe::flags. Mirrors the ECS component.
    enum GPUProbeFlag : uint32_t
    {
        GPU_PROBE_FLAG_BAKED     = 1u << 0,
        // Scale the capture by local / capture-point far-field irradiance
        // (shaders/probe_normalization.hlsli). Set on every probe unless -probe_norm_off.
        GPU_PROBE_FLAG_NORMALIZE = 1u << 1,
    };

    // Proxy / Influence shape enum VALUES - mirror EProxyShape (ProxyVolumeComponent.h),
    // EInfluenceShape (ReflectionProbeComponent.h) and the PROXY_* / INFL_* HLSL
    // defines in reflection_probe.hlsli. Plain constants so this Graphics header does
    // not depend on the ECS headers.
    inline constexpr uint32_t kProxyShapeInfinite    = 0;
    inline constexpr uint32_t kProxyShapeSphere      = 1;
    inline constexpr uint32_t kProxyShapeOrientedBox = 2;
    inline constexpr uint32_t kInflShapeBox          = 0;
    inline constexpr uint32_t kInflShapeSphere       = 1;
}
