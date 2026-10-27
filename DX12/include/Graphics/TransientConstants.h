#pragma once

// Graphics/TransientConstants.h - this frame's constants, allocated per draw or dispatch.
// Passes reach it as cl.AllocConstants<T>() or AllocConstants<T>(gfx) (Graphics/IGraphicsDevice.h).
//
// One mapped UPLOAD buffer per frame slot. A recording thread takes a chunk of the slot with one
// atomic add and hands out 256-byte-aligned pieces of it with no lock, so Phase B's parallel
// recording needs none; a piece larger than a chunk takes a run of its own.
// GraphicsDX12::BeginFrame resets the slot right after WaitForNextFrameSlot has retired the frame
// that used it last, so an allocation lives until this frame's GPU work retires.
//
// Overflow is loud: the allocation comes back empty (the caller skips that draw),
// ENGINE_CHECK_ONCE says so, and [CBALLOC] counts it.

#include <atomic>
#include <cstdint>
#include <d3d12.h>
#include <wrl/client.h>

class TransientConstantAllocator
{
public:
    static constexpr uint32_t kAlign = 256;   // a root CBV address's alignment

    struct Alloc
    {
        void*    cpu = nullptr;
        uint64_t gpu = 0;
    };

    struct Stats
    {
        uint64_t frames = 0;        // BeginFrames since Init
        uint32_t capacity = 0;      // per slot
        uint32_t chunk = 0;
        uint32_t lastTaken = 0;     // chunk space the last finished frame took
        uint32_t peakTaken = 0;     // ... the busiest frame
        uint64_t allocations = 0;   // since Init
        uint64_t overflows = 0;
    };

    bool Init(ID3D12Device* device, uint32_t slots, uint32_t bytesPerSlot, uint32_t chunkBytes);
    void Shutdown();
    bool IsValid() const { return m_capacity != 0; }

    // The render half starts a frame on @p slot, whose previous frame has
    // retired. Nothing may still be recording into the frame before it.
    void BeginFrame(uint32_t slot);

    // Any recording thread. @p bytes is rounded up to kAlign.
    Alloc Allocate(uint32_t bytes);

    Stats GetStats() const;

private:
    static constexpr uint32_t kMaxSlots = 4;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_buffers[kMaxSlots];
    uint8_t*  m_mapped[kMaxSlots] = {};
    uint64_t  m_baseVA[kMaxSlots] = {};
    uint32_t  m_slots = 0, m_capacity = 0, m_chunk = 0;

    std::atomic<uint32_t> m_slot{ 0 };
    std::atomic<uint64_t> m_epoch{ 1 };      // a thread's chunk is good for one epoch (one frame)
    std::atomic<uint32_t> m_taken{ 0 };      // chunk space handed out this frame
    std::atomic<uint64_t> m_allocations{ 0 };
    std::atomic<uint64_t> m_overflows{ 0 };
    uint64_t m_frames = 0;
    uint32_t m_lastTaken = 0, m_peakTaken = 0;
};
