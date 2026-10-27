#pragma once

// MaterialCBVRing - per-frame upload-heap ring for custom-material CBVs.
// A custom GBuffer pixel shader may declare its own cbuffer at register(b8, space0); the renderer
// packs MaterialComponent::customParams by the shader's reflection into this CPU-mapped memory,
// which rotates through kFramesInFlight buffers so the GPU never reads a slice being rewritten.
// Usage:
//     ring.Init(gfx);                              // once
//     ring.BeginFrame(gfx.GetUploadSlot());        // start of each frame
//     auto [cpu, gpu] = ring.Allocate(cbufSize);   // per material, then memcpy into cpu
//     dx12.BindCustomMaterialCBV(gpu, cmdList);    // per draw
// Allocate rounds every slice up to 256 B, the D3D12 CBV address alignment.

#include "Graphics/FrameConstants.h"
#include <cstdint>
#include <d3d12.h>
#include <wrl/client.h>

class IGraphicsDevice;

// MaterialCBVStore - persistent per-material-slot CBV blocks.
// ONE persistently mapped UPLOAD buffer; slot i owns bytes [i*kSlotStride, (i+1)*kSlotStride). No
// BeginFrame: a slot's GPU VA is stable for the slot's lifetime, which is what makes customCbvVA a
// retainable IndirectGroup / DrawPacket key field.
// Safety contract (enforced by the Renderer, not this class): a slot's bytes are written ONLY when
// the slot is (re)assigned to a new MaterialBindingKey, and slot reuse is deferred past
// frames-in-flight (Renderer::kMatSlotReuseLagEpochs), so the GPU never reads a half-written slot.
class MaterialCBVStore
{
public:
    // 256 B covers every b8 packer on the renderer's material path (the particle 96 B and TextFX 80 B
    // ABIs live in their own systems' rings). A reflected b8 cbuffer larger than this falls back to the
    // per-frame MaterialCBVRing.
    static constexpr uint32_t kSlotStride = RHI::kConstantBufferAlignment;

    bool Init(IGraphicsDevice& gfx, uint32_t maxSlots);
    void Shutdown();
    bool IsValid() const { return m_mapped != nullptr; }

    void*    SlotCpu(uint32_t slot) const
    { return m_mapped + static_cast<size_t>(slot) * kSlotStride; }
    uint64_t SlotVA(uint32_t slot) const
    { return m_baseVA + static_cast<uint64_t>(slot) * kSlotStride; }

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> m_buffer;
    uint8_t*                  m_mapped   = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS m_baseVA   = 0;
    uint32_t                  m_maxSlots = 0;
};

class MaterialCBVRing
{
public:
    struct Slice
    {
        void*                       cpu;  // CPU-writable pointer into the upload heap
        D3D12_GPU_VIRTUAL_ADDRESS   gpu;  // GPU virtual address of the same region
    };

    // Call once at renderer startup. @p bytesPerFrame is the per-frame capacity; 256 KB holds ~1000
    // materials of ~256 B each.
    bool Init(IGraphicsDevice& gfx, uint32_t bytesPerFrame = 256 * 1024);
    void Shutdown();

    // Starts a frame: selects buffer @p frameIdx % kFramesInFlight and resets the cursor. Pass the
    // game side's upload slot (IGraphicsDevice::GetUploadSlot()), not a frame counter: under
    // -slotkey=dxgi the slot is the back-buffer index, so a counter would pick a buffer a frame in
    // flight may still be reading.
    void BeginFrame(uint32_t frameIdx);

    // Allocate takes @p sizeBytes (rounded up to 256 B) from the current ring buffer; {nullptr, 0} when
    // the buffer is full.
    Slice Allocate(uint32_t sizeBytes);

    uint32_t GetUsedBytes() const { return m_cursor; }
    uint32_t GetCapacityBytes() const { return m_bytesPerFrame; }

private:
    static constexpr uint32_t kFramesInFlight = RHI::kFramesInFlight;
    static constexpr uint32_t kCBVAlign       = RHI::kConstantBufferAlignment;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_buffers[kFramesInFlight]{};
    uint8_t*                               m_mapped[kFramesInFlight]{};
    D3D12_GPU_VIRTUAL_ADDRESS              m_baseVA[kFramesInFlight]{};
    uint32_t                               m_bytesPerFrame = 0;
    uint32_t                               m_current       = 0;
    uint32_t                               m_cursor        = 0;
    bool                                   m_overflowWarned = false;
};
