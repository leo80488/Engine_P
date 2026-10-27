#include "Graphics/MaterialSRVRing.h"
#include "Graphics/GraphicsDX12.h"
#include "System/Log.h"

bool MaterialSRVStore::Init(IGraphicsDevice& gfxBase, uint32_t maxTables)
{
    auto& dx12 = static_cast<GraphicsDX12&>(gfxBase);
    if (maxTables == 0) { LOG_ERROR("MaterialSRVStore::Init: maxTables 0"); return false; }

    m_descIncBytes = dx12.GetDevice()->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    const uint32_t descCount = maxTables * GraphicsDX12::kCustomMatTextureSlots;
    m_alloc = dx12.GetCbvSrvUavAllocator().AllocateStatic(descCount);
    if (!m_alloc.IsValid())
    {
        LOG_ERROR("MaterialSRVStore::Init: AllocateStatic(%u) failed", descCount);
        return false;
    }

    m_maxTables     = maxTables;
    m_next          = 0;
    m_exhaustWarned = false;
    LOG_INFO("MaterialSRVStore: initialised (%u tables × %u slots, persistent)",
             m_maxTables, GraphicsDX12::kCustomMatTextureSlots);
    return true;
}

void MaterialSRVStore::Shutdown()
{
    m_alloc         = {};
    m_maxTables     = 0;
    m_next          = 0;
    m_descIncBytes  = 0;
    m_exhaustWarned = false;
}

uint32_t MaterialSRVStore::AllocateBlock()
{
    if (m_next >= m_maxTables)
    {
        if (!m_exhaustWarned)
        {
            LOG_WARNING("MaterialSRVStore: pool exhausted (%u tables); further "
                        "custom materials fall back to the per-frame SRV ring",
                        m_maxTables);
            m_exhaustWarned = true;
        }
        return ~0u;
    }
    return m_next++;
}

MaterialSRVStore::Block MaterialSRVStore::GetBlock(uint32_t blockIdx) const
{
    Block out;
    if (blockIdx >= m_next) return out;
    const uint64_t first = static_cast<uint64_t>(blockIdx) *
                           GraphicsDX12::kCustomMatTextureSlots;
    // GpuCpuHandle = the CPU-writable handle of the shader-visible heap (the CopyDescriptorsSimple
    // destination), as in MaterialSRVRing::Allocate.
    out.cpu.ptr      = m_alloc.GetGpuCpuHandle().ptr + first * m_descIncBytes;
    out.gpu          = m_alloc.GetGpuHandle().ptr + first * m_descIncBytes;
    out.descIncBytes = m_descIncBytes;
    return out;
}

bool MaterialSRVRing::Init(IGraphicsDevice& gfxBase, uint32_t maxMatsPerFrame)
{
    auto& dx12 = static_cast<GraphicsDX12&>(gfxBase);

    const uint32_t descPerMat = GraphicsDX12::kCustomMatTextureSlots;
    m_descPerFrame = maxMatsPerFrame * descPerMat;

    auto& alloc = dx12.GetCbvSrvUavAllocator();
    m_descIncBytes = dx12.GetDevice()->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    for (uint32_t f = 0; f < kFramesInFlight; ++f)
    {
        m_frames[f] = alloc.AllocateStatic(m_descPerFrame);
        if (!m_frames[f].IsValid())
        {
            LOG_ERROR("MaterialSRVRing::Init: AllocateStatic(%u) failed on frame %u",
                      m_descPerFrame, f);
            return false;
        }
    }

    m_current = 0;
    m_cursor  = 0;
    m_overflowWarned = false;
    LOG_INFO("MaterialSRVRing: initialised (%u descriptors × %u frames, inc=%u)",
             m_descPerFrame, kFramesInFlight, m_descIncBytes);
    return true;
}

void MaterialSRVRing::Shutdown()
{
    for (auto& f : m_frames)
        f = {};   // DescriptorAllocation releases itself on reset
    m_descPerFrame = 0;
    m_descIncBytes = 0;
    m_cursor       = 0;
    m_current      = 0;
    m_overflowWarned = false;
    m_fallbackCpu  = {};
}

void MaterialSRVRing::BeginFrame(uint32_t frameIdx)
{
    m_current = frameIdx % kFramesInFlight;
    m_cursor  = 0;
    m_overflowWarned = false;
}

MaterialSRVRing::Slice MaterialSRVRing::Allocate(uint32_t count)
{
    if (count == 0) return {};
    if (m_cursor > m_descPerFrame || count > m_descPerFrame - m_cursor)
    {
        if (!m_overflowWarned)
        {
            LOG_WARNING("MaterialSRVRing::Allocate: ring full (%u + %u > %u); "
                        "further overflow warnings suppressed for this frame",
                        m_cursor, count, m_descPerFrame);
            m_overflowWarned = true;
        }
        return {};
    }

    const auto& frame = m_frames[m_current];
    // GpuCpuHandle = the CPU-writable handle of the shader-visible heap, the CopyDescriptorsSimple
    // destination.
    D3D12_CPU_DESCRIPTOR_HANDLE baseCpu = frame.GetGpuCpuHandle();
    D3D12_GPU_DESCRIPTOR_HANDLE baseGpu = frame.GetGpuHandle();

    Slice out;
    out.cpu.ptr       = baseCpu.ptr + static_cast<std::size_t>(m_cursor) * m_descIncBytes;
    out.gpu           = baseGpu.ptr + static_cast<uint64_t>(m_cursor) * m_descIncBytes;
    out.descIncBytes  = m_descIncBytes;
    m_cursor += count;
    return out;
}
