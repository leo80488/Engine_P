#pragma once

// FrameConstants - engine-wide frame-pipelining and alignment constants.
//
// They are constexpr, not IGraphicsDevice virtuals, because every consumer of the
// frame count uses it as a member-array bound (RHI::GPUBuffer
// m_buffers[kFramesInFlight]), which a runtime accessor cannot size. This header
// names no backend type and includes no D3D header. IGraphicsDevice::GetFrameCount()
// serves the runtime callers (UI readouts, code holding only an IGraphicsDevice&);
// the backend's GraphicsDX12::kFrameCount is defined as kFramesInFlight.
// Do not size a per-frame ring from anything else: a ring smaller than the real
// in-flight depth compiles, then has the CPU overwrite a slot the GPU still reads -
// silent corruption (torn descriptor reads, skinned-mesh flicker), not a crash.

#include <cstdint>

namespace RHI
{
    // Number of frames the renderer keeps in flight (swap-chain buffer count). Every
    // per-frame ring (constant buffers, pose / skinning uploads, descriptor staging,
    // profiler readback) sizes itself from this.
    inline constexpr uint32_t kFramesInFlight = 3;

    // Required alignment, in bytes, of a constant-buffer view's offset and size
    // (D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT), declared here so portable ring
    // allocators do not each restate 256.
    inline constexpr uint32_t kConstantBufferAlignment = 256;
}
