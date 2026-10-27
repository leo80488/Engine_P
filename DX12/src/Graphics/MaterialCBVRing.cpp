#include "Graphics/MaterialCBVRing.h"
#include "Graphics/GpuMemoryLedger.h"   // the profiler's GPU resource ledger
#include "Graphics/GraphicsDX12.h"
#include "System/Log.h"

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr uint32_t AlignUp(uint32_t v, uint32_t a)
    {
        return (v + a - 1u) & ~(a - 1u);
    }
}

bool MaterialCBVStore::Init(IGraphicsDevice& gfxBase, uint32_t maxSlots)
{
    auto& gfx = static_cast<GraphicsDX12&>(gfxBase);
    ID3D12Device* dev = gfx.GetDevice();
    if (!dev) { LOG_ERROR("MaterialCBVStore::Init: device null"); return false; }
    if (maxSlots == 0) { LOG_ERROR("MaterialCBVStore::Init: maxSlots 0"); return false; }

    static_assert(MaterialCBVStore::kSlotStride % 256 == 0,
                  "CBV root binds require 256B-aligned slot addresses");

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width            = static_cast<uint64_t>(maxSlots) * kSlotStride;
    desc.Height           = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT hr = dev->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE,
        &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&m_buffer));
    if (FAILED(hr))
    {
        LOG_ERROR("MaterialCBVStore::Init: CreateCommittedResource failed hr=0x%08X", hr);
        return false;
    }
    GpuLedger::TrackCommitted(dev, m_buffer.Get(), Diag::Prof::GpuMemCat::Upload, "MaterialCBVRing");

    D3D12_RANGE readRange{ 0, 0 };
    void* mapped = nullptr;
    hr = m_buffer->Map(0, &readRange, &mapped);
    if (FAILED(hr))
    {
        LOG_ERROR("MaterialCBVStore::Init: Map failed hr=0x%08X", hr);
        m_buffer.Reset();
        return false;
    }
    m_mapped   = static_cast<uint8_t*>(mapped);
    m_baseVA   = m_buffer->GetGPUVirtualAddress();
    m_maxSlots = maxSlots;
    LOG_INFO("MaterialCBVStore: initialised (%u slots × %u bytes, persistent)",
             m_maxSlots, kSlotStride);
    return true;
}

void MaterialCBVStore::Shutdown()
{
    if (m_buffer && m_mapped)
        m_buffer->Unmap(0, nullptr);
    m_mapped   = nullptr;
    m_baseVA   = 0;
    m_maxSlots = 0;
    m_buffer.Reset();
}

bool MaterialCBVRing::Init(IGraphicsDevice& gfxBase, uint32_t bytesPerFrame)
{
    auto& gfx = static_cast<GraphicsDX12&>(gfxBase);
    ID3D12Device* dev = gfx.GetDevice();
    if (!dev) { LOG_ERROR("MaterialCBVRing::Init: device null"); return false; }

    m_bytesPerFrame = AlignUp(bytesPerFrame, kCBVAlign);

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width            = m_bytesPerFrame;
    desc.Height           = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    for (uint32_t i = 0; i < kFramesInFlight; ++i)
    {
        HRESULT hr = dev->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&m_buffers[i]));
        if (FAILED(hr))
        {
            LOG_ERROR("MaterialCBVRing::Init: CreateCommittedResource failed hr=0x%08X", hr);
            return false;
        }
        GpuLedger::TrackCommitted(dev, m_buffers[i].Get(), Diag::Prof::GpuMemCat::Upload, "MaterialCBVRing");

        D3D12_RANGE readRange{ 0, 0 };
        void* mapped = nullptr;
        hr = m_buffers[i]->Map(0, &readRange, &mapped);
        if (FAILED(hr))
        {
            LOG_ERROR("MaterialCBVRing::Init: Map failed hr=0x%08X", hr);
            return false;
        }
        m_mapped[i] = static_cast<uint8_t*>(mapped);
        m_baseVA[i] = m_buffers[i]->GetGPUVirtualAddress();
    }

    m_current = 0;
    m_cursor  = 0;
    m_overflowWarned = false;
    LOG_INFO("MaterialCBVRing: initialised (%u bytes × %u frames)",
             m_bytesPerFrame, kFramesInFlight);
    return true;
}

void MaterialCBVRing::Shutdown()
{
    for (uint32_t i = 0; i < kFramesInFlight; ++i)
    {
        if (m_buffers[i] && m_mapped[i])
            m_buffers[i]->Unmap(0, nullptr);
        m_mapped[i] = nullptr;
        m_baseVA[i] = 0;
        m_buffers[i].Reset();
    }
    m_bytesPerFrame = 0;
    m_cursor = 0;
    m_overflowWarned = false;
}

void MaterialCBVRing::BeginFrame(uint32_t frameIdx)
{
    m_current = frameIdx % kFramesInFlight;
    m_cursor  = 0;
    m_overflowWarned = false;
}

MaterialCBVRing::Slice MaterialCBVRing::Allocate(uint32_t sizeBytes)
{
    const uint32_t aligned = AlignUp(sizeBytes, kCBVAlign);
    if (m_cursor + aligned > m_bytesPerFrame)
    {
        if (!m_overflowWarned)
        {
            LOG_WARNING("MaterialCBVRing::Allocate: ring full (%u + %u > %u); "
                        "further overflow warnings suppressed for this frame",
                        m_cursor, aligned, m_bytesPerFrame);
            m_overflowWarned = true;
        }
        return { nullptr, 0 };
    }

    Slice out;
    out.cpu = m_mapped[m_current] + m_cursor;
    out.gpu = m_baseVA[m_current] + m_cursor;
    m_cursor += aligned;
    return out;
}
