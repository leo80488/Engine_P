#include "Graphics/TransientConstants.h"

#include "Graphics/GpuMemoryLedger.h"
#include "System/Check.h"
#include "System/Log.h"

#include <algorithm>

namespace
{
    // The chunk this thread is handing out: [offset, end) of the current slot,
    // good while the allocator's epoch is the one it was taken in.
    struct ThreadChunk
    {
        const TransientConstantAllocator* owner = nullptr;
        uint64_t epoch  = 0;
        uint32_t offset = 0;
        uint32_t end    = 0;
    };
    thread_local ThreadChunk t_chunk;

    constexpr uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1u) & ~(a - 1u); }
}

bool TransientConstantAllocator::Init(ID3D12Device* device, uint32_t slots, uint32_t bytesPerSlot,
                                      uint32_t chunkBytes)
{
    if (!ENGINE_CHECK(device && slots > 0 && slots <= kMaxSlots && chunkBytes >= kAlign,
                      "TransientConstantAllocator::Init: bad arguments (slots %u, chunk %u)", slots, chunkBytes))
        return false;
    m_chunk    = AlignUp(chunkBytes, kAlign);
    m_capacity = AlignUp(std::max(bytesPerSlot, m_chunk), m_chunk);

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width            = m_capacity;
    desc.Height           = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (uint32_t i = 0; i < slots; ++i)
    {
        HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(&m_buffers[i]));
        if (FAILED(hr))
        {
            LOG_ERROR("TransientConstantAllocator::Init: CreateCommittedResource failed hr=0x%08X", hr);
            Shutdown();
            return false;
        }
        m_buffers[i]->SetName(L"TransientConstants");
        GpuLedger::TrackCommitted(device, m_buffers[i].Get(), Diag::Prof::GpuMemCat::Upload, "TransientConstants");
        D3D12_RANGE noRead{ 0, 0 };
        void* mapped = nullptr;
        hr = m_buffers[i]->Map(0, &noRead, &mapped);
        if (FAILED(hr))
        {
            LOG_ERROR("TransientConstantAllocator::Init: Map failed hr=0x%08X", hr);
            Shutdown();
            return false;
        }
        m_mapped[i] = static_cast<uint8_t*>(mapped);
        m_baseVA[i] = m_buffers[i]->GetGPUVirtualAddress();
    }
    m_slots = slots;
    LOG_INFO("TransientConstantAllocator: %u slots x %u KiB, %u-byte chunks", slots, m_capacity / 1024, m_chunk);
    return true;
}

void TransientConstantAllocator::Shutdown()
{
    for (uint32_t i = 0; i < kMaxSlots; ++i)
    {
        if (m_buffers[i] && m_mapped[i]) m_buffers[i]->Unmap(0, nullptr);
        m_mapped[i] = nullptr;
        m_baseVA[i] = 0;
        m_buffers[i].Reset();
    }
    m_slots = m_capacity = 0;
}

void TransientConstantAllocator::BeginFrame(uint32_t slot)
{
    if (!m_capacity) return;
    const uint32_t taken = std::min(m_taken.load(std::memory_order_relaxed), m_capacity);
    if (m_frames)
    {
        m_lastTaken = taken;
        m_peakTaken = std::max(m_peakTaken, taken);
    }
    ++m_frames;
    m_slot.store(slot % m_slots, std::memory_order_relaxed);
    m_taken.store(0, std::memory_order_relaxed);
    // Release: a thread that sees the new epoch sees the new slot and cursor.
    m_epoch.fetch_add(1, std::memory_order_release);
}

TransientConstantAllocator::Alloc TransientConstantAllocator::Allocate(uint32_t bytes)
{
    if (!m_capacity || bytes == 0) return {};
    const uint32_t size  = AlignUp(bytes, kAlign);
    const uint64_t epoch = m_epoch.load(std::memory_order_acquire);
    const uint32_t slot  = m_slot.load(std::memory_order_relaxed);
    ThreadChunk& t = t_chunk;
    const bool chunkGood = t.owner == this && t.epoch == epoch && t.offset + size <= t.end;
    if (!chunkGood)
    {
        // A new chunk - or, for a piece bigger than a chunk, a run of its own
        // that leaves the thread's chunk where it was.
        const uint32_t take = std::max(size, m_chunk);
        uint32_t start = m_taken.load(std::memory_order_relaxed);
        do
        {
            if (take > m_capacity - std::min(start, m_capacity))
            {
                m_overflows.fetch_add(1, std::memory_order_relaxed);
                ENGINE_CHECK_ONCE(false, "TransientConstantAllocator: the frame's %u KiB of constants are "
                                  "used up (%u more bytes asked for) - the draw or dispatch is skipped; "
                                  "raise the per-slot size in GraphicsDX12", m_capacity / 1024, size);
                return {};
            }
        } while (!m_taken.compare_exchange_weak(start, start + take, std::memory_order_relaxed));
        m_allocations.fetch_add(1, std::memory_order_relaxed);
        if (size > m_chunk)
            return { m_mapped[slot] + start, m_baseVA[slot] + start };
        t = { this, epoch, start, start + take };
    }
    else
        m_allocations.fetch_add(1, std::memory_order_relaxed);
    Alloc a{ m_mapped[slot] + t.offset, m_baseVA[slot] + t.offset };
    t.offset += size;
    return a;
}

TransientConstantAllocator::Stats TransientConstantAllocator::GetStats() const
{
    Stats s;
    s.frames      = m_frames;
    s.capacity    = m_capacity;
    s.chunk       = m_chunk;
    s.lastTaken   = m_lastTaken;
    s.peakTaken   = m_peakTaken;
    s.allocations = m_allocations.load(std::memory_order_relaxed);
    s.overflows   = m_overflows.load(std::memory_order_relaxed);
    return s;
}
