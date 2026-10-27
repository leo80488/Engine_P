#include "Graphics/DescriptorHeapAllocator.h"
#include "Graphics/GpuMemoryLedger.h"   // the profiler's GPU resource ledger
#include "System/Check.h"
#include "System/Log.h"
#include "Graphics/BindlessKind.h"   // the bindless kind byte
#include <algorithm>   // sorting the fresh-slot tag histogram
#include <cassert>

using Microsoft::WRL::ComPtr;

// ==== DescriptorAllocation ====

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorAllocation::GetCpuHandle() const
{
    return m_cpuHandle;
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorAllocation::GetGpuCpuHandle() const
{
    return m_gpuCpuHandle;
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorAllocation::GetGpuHandle() const
{
    return m_gpuHandle;
}

UINT DescriptorAllocation::GetCount() const
{
    return m_count;
}

bool DescriptorAllocation::IsShaderVisible() const
{
    return m_shaderVisible;
}

bool DescriptorAllocation::IsValid() const
{
    return m_allocator != nullptr && m_count > 0;
}

void DescriptorAllocation::CopyToGpu()
{
    if (!m_allocator || !m_shaderVisible || m_count == 0)
        return;
    m_allocator->CopyToGpu(m_cpuIndex, m_gpuIndex, m_count);
}

void DescriptorAllocation::Free()
{
    if (m_allocator && m_count > 0)
    {
        m_allocator->FreeStatic(m_cpuIndex, m_gpuIndex, m_count, m_tagIndex);
        m_allocator = nullptr;
        m_count     = 0;
        m_tagIndex  = ~0u;
    }
}

// ==== DescriptorHeapAllocator ====

void DescriptorHeapAllocator::Init(
    ID3D12Device*              device,
    D3D12_DESCRIPTOR_HEAP_TYPE type,
    UINT                       cpuStagingCount,
    UINT                       gpuStaticCount,
    UINT                       gpuDynamicCount)
{
    m_device         = device;
    m_type           = type;
    m_descriptorSize = device->GetDescriptorHandleIncrementSize(type);
    m_shaderVisible  = (gpuStaticCount + gpuDynamicCount) > 0;

    // CPU staging heap: every static allocation reserves its slots here first.
    m_cpuCapacity = cpuStagingCount;
    m_cpuHeap.Reset();
    m_doubleFrees = 0;

    if (cpuStagingCount > 0)
        m_cpuCapacity = CreateHeapWithFallback(device, type, cpuStagingCount,
                                               D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
                                               m_cpuHeap, "CPU staging");
    m_cpuSlots.Reset(m_cpuCapacity);

    // GPU shader-visible heap
    m_gpuStaticCapacity  = gpuStaticCount;
    m_gpuDynamicCapacity = gpuDynamicCount;
    m_dynamicHead  = 0;
    m_dynamicUsed  = 0;
    m_dynamicQueue.clear();
    m_gpuHeap.Reset();

    if (m_shaderVisible)
    {
        const UINT got = CreateHeapWithFallback(device, type,
                                                gpuStaticCount + gpuDynamicCount,
                                                D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
                                                m_gpuHeap, "GPU shader-visible");
        // A shrunk heap shrinks the STATIC budget, not the dynamic ring: AllocateDynamic
        // degrades safely, but an over-reported static capacity would hand out slots past
        // the end of the heap.
        m_gpuStaticCapacity = (got > gpuDynamicCount) ? (got - gpuDynamicCount) : 0u;
    }
    m_gpuSlots.Reset(m_gpuStaticCapacity);

    // The CBV_SRV_UAV heaps record what each slot holds (kind, stride, generation).
    m_cpuKind.clear();   m_cpuStride.clear();
    m_gpuKind.clear();   m_gpuStride.clear();   m_gpuGen.clear();
    m_cpuStart = m_gpuCpuStart = 0;
    if (m_shaderVisible && type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && m_gpuHeap && m_cpuHeap)
    {
        const UINT gpuTotal = m_gpuStaticCapacity + m_gpuDynamicCapacity;
        m_cpuKind.assign(m_cpuCapacity, static_cast<uint8_t>(BK_FREE));
        m_cpuStride.assign(m_cpuCapacity, 0);
        m_gpuKind.assign(gpuTotal, static_cast<uint8_t>(BK_FREE));
        m_gpuStride.assign(gpuTotal, 0);
        m_gpuGen.assign(gpuTotal, 0);
        m_cpuStart    = m_cpuHeap->GetCPUDescriptorHandleForHeapStart().ptr;
        m_gpuCpuStart = m_gpuHeap->GetCPUDescriptorHandleForHeapStart().ptr;
    }

    LOG_INFO("DescriptorHeapAllocator[type %d]: cpu staging %u, gpu static %u, "
             "gpu dynamic %u (%u B/descriptor, ~%.1f MB total)",
             static_cast<int>(type), m_cpuCapacity, m_gpuStaticCapacity,
             m_gpuDynamicCapacity, m_descriptorSize,
             (static_cast<double>(m_cpuCapacity) + m_gpuStaticCapacity + m_gpuDynamicCapacity)
                 * m_descriptorSize / (1024.0 * 1024.0));
}

// Creates a descriptor heap; if the driver refuses the requested size, halves until
// it accepts. Returns the capacity obtained (0 on total failure, logged as an error).
// Do not let a failed create pass silently: a null heap makes every later allocation
// invalid and the first symptom is a fault inside the driver. The CBV_SRV_UAV heap
// asks for the D3D12 maximum of 1,000,000 descriptors, which a driver under memory
// pressure may refuse; a smaller heap (logged) beats refusing to boot.
UINT DescriptorHeapAllocator::CreateHeapWithFallback(
    ID3D12Device*                                 device,
    D3D12_DESCRIPTOR_HEAP_TYPE                    type,
    UINT                                          requested,
    D3D12_DESCRIPTOR_HEAP_FLAGS                   flags,
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>& outHeap,
    const char*                                   label)
{
    // Halve down to 1, not to a fixed floor: the RTV (1024), DSV (256) and sampler
    // (2048) heaps are smaller than any sensible floor for the CBV_SRV_UAV heap, and a
    // floor above the request would report a creatable heap as a total failure.
    for (UINT n = requested; n > 0u; n /= 2u)
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type           = type;
        desc.NumDescriptors = n;
        desc.Flags          = flags;
        const HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&outHeap));
        if (SUCCEEDED(hr))
        {
            GpuLedger::TrackDescriptorHeap(device, outHeap.Get(),   // the ledger keeps shader-visible heaps only
                type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? "DescriptorHeap.Sampler" : "DescriptorHeap.CbvSrvUav");
            if (n != requested)
            {
                LOG_WARNING("DescriptorHeapAllocator[type %d]: %s heap of %u descriptors "
                            "was refused; fell back to %u. Heavy scenes may exhaust it — "
                            "watch for the 75%%/90%% notices.",
                            static_cast<int>(type), label, requested, n);
            }
            return n;
        }
        outHeap.Reset();
    }
    LOG_ERROR("DescriptorHeapAllocator[type %d]: could NOT create the %s heap at ANY size "
              "(requested %u). Every allocation from it will fail.",
              static_cast<int>(type), label, requested);
    return 0;
}

void DescriptorHeapAllocator::Reset()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    m_device = nullptr;
    m_type = {};
    m_descriptorSize = 0;
    m_shaderVisible = false;

    m_cpuHeap.Reset();
    m_cpuCapacity = 0;
    m_cpuSlots.Reset(0);
    m_doubleFrees = 0;

    m_gpuHeap.Reset();
    m_gpuStaticCapacity = 0;
    m_gpuDynamicCapacity = 0;
    m_gpuSlots.Reset(0);
    m_dynamicHead = 0;
    m_dynamicUsed = 0;
    m_dynamicQueue.clear();

    // The slot records describe the heaps just released.
    m_cpuKind.clear();   m_cpuStride.clear();
    m_gpuKind.clear();   m_gpuStride.clear();   m_gpuGen.clear();
    m_cpuStart = m_gpuCpuStart = 0;
    m_gpuMirror = nullptr;
    m_gpuMirrorEntries = 0;

    // The tag histogram describes this allocator's cursor, so it dies with it. Reset
    // runs at device shutdown after the [POOL] lines are emitted (GraphicsDX12's
    // destructor orders them that way), so no report loses it.
    m_tagCounts.clear();
    m_tagOverflowFresh = m_tagOverflowReused = 0;
    m_untaggedFresh    = m_untaggedReused    = 0;
}

// ==== Static allocation ====

// Rate-limited so an exhausted heap cannot itself become the problem: the first
// failure carries the full picture, then one line per 1000 after that.
void DescriptorHeapAllocator::ReportExhausted(const char* region, UINT count,
                                              UINT used, UINT capacity)
{
    ++m_exhaustedCount;
    if (m_exhaustedCount == 1 || (m_exhaustedCount % 1000) == 0)
    {
        LOG_ERROR("DescriptorHeapAllocator[type %d]: %s region EXHAUSTED — wanted %u, "
                  "used %u/%u (failure #%llu). Returning an INVALID handle; the caller "
                  "will pass cpuHandle.ptr=0 to D3D12 and the driver will fault. Either "
                  "descriptors are leaking across world reloads or the heap is too small "
                  "for this scene.",
                  static_cast<int>(m_type), region, count, used, capacity,
                  static_cast<unsigned long long>(m_exhaustedCount));
    }
}

// Counts one allocation into the tag histogram; returns its row, or ~0u when not
// counted in a row. Called with m_mutex held. Fresh == the allocation advanced the
// staging region's cursor, the only event that raises the monotonic number
// GetStaticUsage reports (the number a pack can bound).
UINT DescriptorHeapAllocator::NoteTag(const char* tag, bool fresh) const
{
    // Shader-visible allocators are not tagged: the CBV_SRV_UAV heap serves tens of
    // thousands of mostly unnamed allocations a session, and a histogram of that is
    // neither readable nor free. The histogram is for the small CPU-only heaps.
    if (m_shaderVisible) return ~0u;
    if (!tag || !*tag)
    {
        if (fresh) ++m_untaggedFresh; else ++m_untaggedReused;
        return ~0u;
    }
    for (size_t i = 0; i < m_tagCounts.size(); ++i)
    {
        TagCount& tc = m_tagCounts[i];
        if (tc.tag == tag)
        {
            if (fresh) ++tc.fresh; else ++tc.reused;
            return static_cast<UINT>(i);
        }
    }
    if (m_tagCounts.size() >= kMaxTagRows)
    {
        if (fresh) ++m_tagOverflowFresh; else ++m_tagOverflowReused;
        return ~0u;
    }
    m_tagCounts.push_back({ std::string(tag), fresh ? 1u : 0u,
                            fresh ? 0u : 1u, 0u });
    return static_cast<UINT>(m_tagCounts.size() - 1);
}

void DescriptorHeapAllocator::ReportTagHistogram(const char* label) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_shaderVisible) return;
    if (m_tagCounts.empty() && m_untaggedFresh == 0 && m_untaggedReused == 0)
        return;

    // Sorted by fresh, descending: the top rows are the ones that raised the high-water.
    std::vector<const TagCount*> rows;
    rows.reserve(m_tagCounts.size());
    for (const auto& tc : m_tagCounts) rows.push_back(&tc);
    std::sort(rows.begin(), rows.end(),
              [](const TagCount* a, const TagCount* b)
              { return a->fresh > b->fresh; });

    UINT freshTotal = m_untaggedFresh + m_tagOverflowFresh;
    UINT freedTotal = 0;
    for (const auto* r : rows) { freshTotal += r->fresh; freedTotal += r->freed; }
    LOG_INFO("[POOL] %s fresh-slot tags: fresh=%u freed=%u used=%u free=%zu "
             "tags=%zu (fresh = an allocation the free list could NOT serve, "
             "i.e. one that raised the monotonic high-water; freed = handed "
             "back, so fresh-freed is the net residue)",
             label, freshTotal, freedTotal, m_cpuSlots.Cursor(),
             m_cpuSlots.FreeBlocks(), m_tagCounts.size());
    for (const auto* r : rows)
    {
        if (r->fresh == 0 && r->reused == 0 && r->freed == 0) continue;
        LOG_INFO("[POOL]   %s fresh=%u reused=%u freed=%u net=%d  <- %s",
                 label, r->fresh, r->reused, r->freed,
                 static_cast<int>(r->fresh + r->reused) -
                 static_cast<int>(r->freed), r->tag.c_str());
    }
    if (m_untaggedFresh || m_untaggedReused)
        LOG_INFO("[POOL]   %s fresh=%u reused=%u  <- (untagged call site)",
                 label, m_untaggedFresh, m_untaggedReused);
    if (m_tagOverflowFresh || m_tagOverflowReused)
        LOG_INFO("[POOL]   %s fresh=%u reused=%u  <- (other: more than %zu "
                 "distinct tags)", label, m_tagOverflowFresh,
                 m_tagOverflowReused, kMaxTagRows);
}

DescriptorAllocation DescriptorHeapAllocator::AllocateStatic(UINT count,
                                                             const char* tag)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    DescriptorAllocation result;
    if (!m_cpuHeap || count == 0)
        return result;

    // CPU staging slot: first fit from the free list, else the cursor.
    bool freshSlot = false;
    const UINT cpuIndex = m_cpuSlots.Allocate(count, &freshSlot);
    if (cpuIndex == DescriptorSlotRegion::kNone)
    {
        // Log it: the caller passes the invalid handle (ptr 0) to D3D12, and the driver
        // crash comes frames later with nothing else in the log tying it to exhaustion.
        ReportExhausted("CPU staging", count, m_cpuSlots.Cursor(), m_cpuCapacity);
        return result;
    }
    // A fresh slot is one the free list could not serve: the only kind that raises the
    // monotonic cursor GetStaticUsage reports.
    const UINT tagIndex = NoteTag(tag, freshSlot);

    // GPU static slot (shader-visible heaps only): first fit from the free list, else
    // the cursor.
    UINT gpuIndex = 0;
    if (m_shaderVisible)
    {
        gpuIndex = m_gpuSlots.Allocate(count);
        if (gpuIndex == DescriptorSlotRegion::kNone)
        {
            // CPU staging was reserved first: give it back, or repeated failures would leak CPU
            // descriptor slots permanently.
            m_cpuSlots.Free(cpuIndex, count);
            ReportExhausted("GPU static", count, m_gpuSlots.Cursor(), m_gpuStaticCapacity);
            return result;
        }
    }

    // Pressure watch, like PoseRingBuffer::BeginFrame's 75%/90% notices: the heap
    // filling up must show before the allocation that fails, because that failure
    // surfaces as a driver fault with no context.
    if (m_cpuCapacity)
    {
        const UINT cursor = m_cpuSlots.Cursor();
        const UINT pct    = (cursor * 100u) / m_cpuCapacity;
        if (!m_warned90 && pct >= 90)
        {
            m_warned90 = true;
            LOG_WARNING("DescriptorHeapAllocator[type %d]: CPU staging at %u%% "
                        "(%u/%u). Exhaustion faults inside the D3D12 driver.",
                        static_cast<int>(m_type), pct, cursor, m_cpuCapacity);
        }
        else if (!m_warned75 && pct >= 75)
        {
            m_warned75 = true;
            LOG_INFO("DescriptorHeapAllocator[type %d]: CPU staging at %u%% (%u/%u).",
                     static_cast<int>(m_type), pct, cursor, m_cpuCapacity);
        }
    }

    // Handles into both heaps.
    result.m_cpuHandle = m_cpuHeap->GetCPUDescriptorHandleForHeapStart();
    result.m_cpuHandle.ptr += static_cast<SIZE_T>(cpuIndex) * m_descriptorSize;

    if (m_shaderVisible && m_gpuHeap)
    {
        result.m_gpuCpuHandle = m_gpuHeap->GetCPUDescriptorHandleForHeapStart();
        result.m_gpuCpuHandle.ptr += static_cast<SIZE_T>(gpuIndex) * m_descriptorSize;
        result.m_gpuHandle = m_gpuHeap->GetGPUDescriptorHandleForHeapStart();
        result.m_gpuHandle.ptr += static_cast<SIZE_T>(gpuIndex) * m_descriptorSize;
    }

    result.m_cpuIndex      = cpuIndex;
    result.m_gpuIndex      = gpuIndex;
    result.m_count         = count;
    result.m_shaderVisible = m_shaderVisible;
    result.m_tagIndex      = tagIndex;     // tag histogram row, ~0u = not in a row
    result.m_allocator     = this;
    return result;
}

DescriptorAllocation DescriptorHeapAllocator::Allocate(UINT count,
                                                       const char* tag)
{
    return AllocateStatic(count, tag);
}

// ==== Dynamic ring-buffer allocation ====

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeapAllocator::AllocateDynamic(
    UINT                        count,
    D3D12_CPU_DESCRIPTOR_HANDLE srcCpuHandle,
    UINT64                      fenceValue)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    D3D12_GPU_DESCRIPTOR_HANDLE invalid{};
    if (!m_shaderVisible || !m_gpuHeap || count == 0 || count > m_gpuDynamicCapacity)
        return invalid;

    const UINT capacity = m_gpuDynamicCapacity;
    const UINT base     = m_gpuStaticCapacity;   // the dynamic region follows the static one in the GPU heap

    UINT physOffset = m_dynamicHead;

    if (physOffset + count > capacity)
    {
        // Not enough contiguous space at the end: waste the tail slots (queued with this
        // fence) and wrap to 0.
        const UINT waste = capacity - physOffset;
        if (m_dynamicUsed + waste + count > capacity)
            return invalid;   // cannot fit even after wrapping

        if (waste > 0)
            m_dynamicQueue.push_back({ fenceValue, physOffset, waste });
        m_dynamicUsed += waste;
        m_dynamicHead  = 0;
        physOffset     = 0;
    }
    else
    {
        if (m_dynamicUsed + count > capacity)
            return invalid;
    }

    // Copy srcCpuHandle -> the GPU dynamic region.
    D3D12_CPU_DESCRIPTOR_HANDLE dst = m_gpuHeap->GetCPUDescriptorHandleForHeapStart();
    dst.ptr += static_cast<SIZE_T>(base + physOffset) * m_descriptorSize;
    m_device->CopyDescriptorsSimple(count, dst, srcCpuHandle, m_type);
    if (!m_gpuKind.empty())
    {
        // The kind record travels with the copy, and an overwritten ring slot gets a new
        // generation, like a reused static slot.
        UINT srcIndex = 0;
        const int srcHeap = Locate(srcCpuHandle, srcIndex);
        BumpGenLocked(base + physOffset, count);
        CopyKindsLocked(1, base + physOffset, srcHeap, srcIndex, count);
    }

    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = m_gpuHeap->GetGPUDescriptorHandleForHeapStart();
    gpuHandle.ptr += static_cast<SIZE_T>(base + physOffset) * m_descriptorSize;

    m_dynamicQueue.push_back({ fenceValue, physOffset, count });
    m_dynamicUsed += count;
    m_dynamicHead  = physOffset + count;
    if (m_dynamicHead >= capacity) m_dynamicHead = 0;

    return gpuHandle;
}

void DescriptorHeapAllocator::ReclaimDynamic(UINT64 completedFence)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    while (!m_dynamicQueue.empty())
    {
        const auto& front = m_dynamicQueue.front();
        if (front.fenceValue > completedFence)
            break;
        m_dynamicUsed -= front.count;
        m_dynamicQueue.pop_front();
    }
}

// ==== Internal (called by DescriptorAllocation) ====

void DescriptorHeapAllocator::FreeStatic(UINT cpuIndex, UINT gpuIndex, UINT count,
                                         UINT tagIndex)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (count == 0) return;
    // Every slot handed out in both regions, or this is a second Free of one allocation (a copy
    // of a DescriptorAllocation freed again): putting the slots on the free lists twice would hand
    // them to two owners. Checked in both regions before either changes.
    const bool handedOut = m_cpuSlots.IsHandedOut(cpuIndex, count) &&
                           (!m_shaderVisible || m_gpuSlots.IsHandedOut(gpuIndex, count));
    if (!ENGINE_CHECK_ONCE(handedOut,
                           "DescriptorHeapAllocator[type %d]: Free of %u slot(s) at staging %u / gpu %u "
                           "that are not handed out (freed already) - refused",
                           static_cast<int>(m_type), count, cpuIndex, gpuIndex))
    {
        ++m_doubleFrees;
        return;
    }
    m_cpuSlots.Free(cpuIndex, count);
    if (m_shaderVisible)
        m_gpuSlots.Free(gpuIndex, count);
    if (!m_gpuKind.empty())
    {
        // Free slots hold nothing, and the next owner of a GPU slot gets a new generation:
        // a handle made before this free names the old one, which is how the debug lane
        // spots a stale handle.
        BumpGenLocked(gpuIndex, count);
        SetKindsLocked(0, cpuIndex, count, static_cast<uint8_t>(BK_FREE), 0);
        SetKindsLocked(1, gpuIndex, count, static_cast<uint8_t>(BK_FREE), 0);
    }
    // Counted against the row the allocation was counted into, so fresh minus freed
    // is a per-tag net that names the residue the high-water cursor carries.
    if (tagIndex < m_tagCounts.size()) ++m_tagCounts[tagIndex].freed;
}

void DescriptorHeapAllocator::CopyToGpu(UINT cpuIndex, UINT gpuIndex, UINT count)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_shaderVisible || !m_cpuHeap || !m_gpuHeap || count == 0)
        return;

    D3D12_CPU_DESCRIPTOR_HANDLE src = m_cpuHeap->GetCPUDescriptorHandleForHeapStart();
    src.ptr += static_cast<SIZE_T>(cpuIndex) * m_descriptorSize;

    D3D12_CPU_DESCRIPTOR_HANDLE dst = m_gpuHeap->GetCPUDescriptorHandleForHeapStart();
    dst.ptr += static_cast<SIZE_T>(gpuIndex) * m_descriptorSize;

    m_device->CopyDescriptorsSimple(count, dst, src, m_type);
    if (!m_gpuKind.empty())
        CopyKindsLocked(1, gpuIndex, 0, cpuIndex, count);   // the kind records follow the copy
}

// ==== Descriptor kind records: what each slot holds ====

namespace
{
    // The component class a view of @p f returns: uint / sint formats, and
    // float for everything else (FLOAT, UNORM, SNORM, SRGB, depth).
    uint32_t ClassOfFormat(DXGI_FORMAT f)
    {
        switch (f)
        {
        case DXGI_FORMAT_R32G32B32A32_UINT:
        case DXGI_FORMAT_R32G32B32_UINT:
        case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R32G32_UINT:
        case DXGI_FORMAT_R10G10B10A2_UINT:
        case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R16G16_UINT:
        case DXGI_FORMAT_R32_UINT:
        case DXGI_FORMAT_R8G8_UINT:
        case DXGI_FORMAT_R16_UINT:
        case DXGI_FORMAT_R8_UINT:
        case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:   // the stencil planes
        case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
            return BK_CLASS_UINT;
        case DXGI_FORMAT_R32G32B32A32_SINT:
        case DXGI_FORMAT_R32G32B32_SINT:
        case DXGI_FORMAT_R16G16B16A16_SINT:
        case DXGI_FORMAT_R32G32_SINT:
        case DXGI_FORMAT_R8G8B8A8_SINT:
        case DXGI_FORMAT_R16G16_SINT:
        case DXGI_FORMAT_R32_SINT:
        case DXGI_FORMAT_R8G8_SINT:
        case DXGI_FORMAT_R16_SINT:
        case DXGI_FORMAT_R8_SINT:
            return BK_CLASS_SINT;
        default:
            return BK_CLASS_FLOAT;
        }
    }

    // The shape of a texture's default view (a null view description).
    uint32_t DefaultTextureShape(const D3D12_RESOURCE_DESC& rd)
    {
        switch (rd.Dimension)
        {
        case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
            return rd.DepthOrArraySize > 1 ? BK_TEX1DARRAY : BK_TEX1D;
        case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
            if (rd.SampleDesc.Count > 1)
                return rd.DepthOrArraySize > 1 ? BK_TEX2DMSARRAY : BK_TEX2DMS;
            return rd.DepthOrArraySize > 1 ? BK_TEX2DARRAY : BK_TEX2D;
        case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
            return BK_TEX3D;
        default:
            return BK_FREE;   // a buffer has no default view
        }
    }
}

uint8_t DescriptorHeapAllocator::KindOfSrv(ID3D12Resource* res,
                                           const D3D12_SHADER_RESOURCE_VIEW_DESC* desc)
{
    using RHI::Bindless::MakeKind;
    if (!desc)
    {
        if (!res) return static_cast<uint8_t>(BK_FREE);   // D3D12 refuses this
        const D3D12_RESOURCE_DESC rd = res->GetDesc();
        return MakeKind(DefaultTextureShape(rd), ClassOfFormat(rd.Format));
    }
    bool     isNull = (res == nullptr);
    uint32_t cls    = ClassOfFormat(desc->Format);
    uint32_t shape  = BK_FREE;
    switch (desc->ViewDimension)
    {
    case D3D12_SRV_DIMENSION_BUFFER:
        if (desc->Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW)  { shape = BK_RAWBUFFER;        cls = BK_CLASS_NONE; }
        else if (desc->Buffer.StructureByteStride > 0)       { shape = BK_STRUCTUREDBUFFER; cls = BK_CLASS_NONE; }
        else                                                   shape = BK_TYPEDBUFFER;
        break;
    case D3D12_SRV_DIMENSION_TEXTURE1D:        shape = BK_TEX1D;         break;
    case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:   shape = BK_TEX1DARRAY;    break;
    case D3D12_SRV_DIMENSION_TEXTURE2D:        shape = BK_TEX2D;         break;
    case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:   shape = BK_TEX2DARRAY;    break;
    case D3D12_SRV_DIMENSION_TEXTURE2DMS:      shape = BK_TEX2DMS;       break;
    case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY: shape = BK_TEX2DMSARRAY;  break;
    case D3D12_SRV_DIMENSION_TEXTURE3D:        shape = BK_TEX3D;         break;
    case D3D12_SRV_DIMENSION_TEXTURECUBE:      shape = BK_TEXCUBE;       break;
    case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY: shape = BK_TEXCUBEARRAY;  break;
    case D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE:
        // Always written with a null resource; the structure is the address.
        shape  = BK_ACCELSTRUCT;
        cls    = BK_CLASS_NONE;
        isNull = (desc->RaytracingAccelerationStructure.Location == 0);
        break;
    default:
        return static_cast<uint8_t>(BK_FREE);
    }
    return MakeKind(shape, cls, false, isNull);
}

uint8_t DescriptorHeapAllocator::KindOfUav(ID3D12Resource* res,
                                           const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc)
{
    using RHI::Bindless::MakeKind;
    if (!desc)
    {
        if (!res) return static_cast<uint8_t>(BK_FREE);
        const D3D12_RESOURCE_DESC rd = res->GetDesc();
        return MakeKind(DefaultTextureShape(rd), ClassOfFormat(rd.Format), true);
    }
    const bool isNull = (res == nullptr);
    uint32_t   cls    = ClassOfFormat(desc->Format);
    uint32_t   shape  = BK_FREE;
    switch (desc->ViewDimension)
    {
    case D3D12_UAV_DIMENSION_BUFFER:
        if (desc->Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW)  { shape = BK_RAWBUFFER;        cls = BK_CLASS_NONE; }
        else if (desc->Buffer.StructureByteStride > 0)       { shape = BK_STRUCTUREDBUFFER; cls = BK_CLASS_NONE; }
        else                                                   shape = BK_TYPEDBUFFER;
        break;
    case D3D12_UAV_DIMENSION_TEXTURE1D:      shape = BK_TEX1D;       break;
    case D3D12_UAV_DIMENSION_TEXTURE1DARRAY: shape = BK_TEX1DARRAY;  break;
    case D3D12_UAV_DIMENSION_TEXTURE2D:      shape = BK_TEX2D;       break;
    case D3D12_UAV_DIMENSION_TEXTURE2DARRAY: shape = BK_TEX2DARRAY;  break;
    case D3D12_UAV_DIMENSION_TEXTURE3D:      shape = BK_TEX3D;       break;
    default:
        return static_cast<uint8_t>(BK_FREE);
    }
    return MakeKind(shape, cls, true, isNull);
}

uint8_t DescriptorHeapAllocator::KindOfCbv(const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc)
{
    return RHI::Bindless::MakeKind(BK_CBV, BK_CLASS_NONE, false,
                                   desc == nullptr || desc->BufferLocation == 0);
}

int DescriptorHeapAllocator::Locate(D3D12_CPU_DESCRIPTOR_HANDLE h, UINT& index) const
{
    if (m_descriptorSize == 0 || h.ptr == 0)
        return -1;
    if (!m_cpuKind.empty() && h.ptr >= m_cpuStart)
    {
        const SIZE_T i = (h.ptr - m_cpuStart) / m_descriptorSize;
        if (i < m_cpuKind.size()) { index = static_cast<UINT>(i); return 0; }
    }
    if (!m_gpuKind.empty() && h.ptr >= m_gpuCpuStart)
    {
        const SIZE_T i = (h.ptr - m_gpuCpuStart) / m_descriptorSize;
        if (i < m_gpuKind.size()) { index = static_cast<UINT>(i); return 1; }
    }
    return -1;
}

void DescriptorHeapAllocator::SetKindsLocked(int heap, UINT index, UINT count,
                                             uint8_t kind, uint16_t stride) const
{
    if (heap < 0) return;
    std::vector<uint8_t>&  kinds   = heap == 0 ? m_cpuKind   : m_gpuKind;
    std::vector<uint16_t>& strides = heap == 0 ? m_cpuStride : m_gpuStride;
    for (UINT k = 0; k < count && index + k < kinds.size(); ++k)
    {
        kinds[index + k]   = kind;
        strides[index + k] = stride;
    }
    if (heap == 1)
        MirrorLocked(index, count);
}

void DescriptorHeapAllocator::CopyKindsLocked(int dstHeap, UINT dst, int srcHeap,
                                              UINT src, UINT count) const
{
    if (dstHeap < 0) return;
    std::vector<uint8_t>&  dk = dstHeap == 0 ? m_cpuKind   : m_gpuKind;
    std::vector<uint16_t>& ds = dstHeap == 0 ? m_cpuStride : m_gpuStride;
    for (UINT k = 0; k < count && dst + k < dk.size(); ++k)
    {
        if (srcHeap >= 0)
        {
            const std::vector<uint8_t>&  sk = srcHeap == 0 ? m_cpuKind   : m_gpuKind;
            const std::vector<uint16_t>& ss = srcHeap == 0 ? m_cpuStride : m_gpuStride;
            if (src + k < sk.size())
            {
                dk[dst + k] = sk[src + k];
                ds[dst + k] = ss[src + k];
                continue;
            }
        }
        // Copied from a heap nobody records: what it holds is unknown, and
        // unknown is Free - a handle made to it is refused, not trusted.
        dk[dst + k] = static_cast<uint8_t>(BK_FREE);
        ds[dst + k] = 0;
    }
    if (dstHeap == 1)
        MirrorLocked(dst, count);
}

void DescriptorHeapAllocator::BumpGenLocked(UINT gpuIndex, UINT count)
{
    for (UINT k = 0; k < count && gpuIndex + k < m_gpuGen.size(); ++k)
    {
        uint16_t& g = m_gpuGen[gpuIndex + k];
        g = static_cast<uint16_t>((g + 1u) & BINDLESS_GEN_MASK);
    }
    MirrorLocked(gpuIndex, count);
}

void DescriptorHeapAllocator::MirrorLocked(UINT gpuIndex, UINT count) const
{
    if (!m_gpuMirror) return;
    for (UINT k = 0; k < count; ++k)
    {
        const UINT i = gpuIndex + k;
        if (i >= m_gpuMirrorEntries || i >= m_gpuKind.size()) break;
        m_gpuMirror[i] = static_cast<uint32_t>(m_gpuKind[i])
                       | (static_cast<uint32_t>(m_gpuGen[i]) << 8);
    }
}

void DescriptorHeapAllocator::AttachGpuMirror(uint32_t* mapped, UINT entries)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_gpuMirror        = mapped;
    m_gpuMirrorEntries = mapped ? entries : 0u;
    MirrorLocked(0, m_gpuMirrorEntries);
}

void DescriptorHeapAllocator::WriteSrv(ID3D12Resource* res,
                                       const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,
                                       D3D12_CPU_DESCRIPTOR_HANDLE dst) const
{
    m_device->CreateShaderResourceView(res, desc, dst);
    if (m_gpuKind.empty()) return;
    const uint16_t stride = (desc && desc->ViewDimension == D3D12_SRV_DIMENSION_BUFFER)
        ? static_cast<uint16_t>(desc->Buffer.StructureByteStride) : uint16_t(0);
    NoteKind(dst, KindOfSrv(res, desc), 1, stride);
}

void DescriptorHeapAllocator::WriteUav(ID3D12Resource* res, ID3D12Resource* counter,
                                       const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc,
                                       D3D12_CPU_DESCRIPTOR_HANDLE dst) const
{
    m_device->CreateUnorderedAccessView(res, counter, desc, dst);
    if (m_gpuKind.empty()) return;
    const uint16_t stride = (desc && desc->ViewDimension == D3D12_UAV_DIMENSION_BUFFER)
        ? static_cast<uint16_t>(desc->Buffer.StructureByteStride) : uint16_t(0);
    NoteKind(dst, KindOfUav(res, desc), 1, stride);
}

void DescriptorHeapAllocator::WriteCbv(const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc,
                                       D3D12_CPU_DESCRIPTOR_HANDLE dst) const
{
    m_device->CreateConstantBufferView(desc, dst);
    if (m_gpuKind.empty()) return;
    NoteKind(dst, KindOfCbv(desc), 1, 0);
}

void DescriptorHeapAllocator::CopyDescriptors(UINT count, D3D12_CPU_DESCRIPTOR_HANDLE dst,
                                              D3D12_CPU_DESCRIPTOR_HANDLE src) const
{
    m_device->CopyDescriptorsSimple(count, dst, src, m_type);
    if (m_gpuKind.empty()) return;
    UINT dstIndex = 0, srcIndex = 0;
    const int dstHeap = Locate(dst, dstIndex);
    const int srcHeap = Locate(src, srcIndex);
    std::lock_guard<std::mutex> lock(m_mutex);
    CopyKindsLocked(dstHeap, dstIndex, srcHeap, srcIndex, count);
}

void DescriptorHeapAllocator::NoteKind(D3D12_CPU_DESCRIPTOR_HANDLE written, uint8_t kind,
                                       UINT count, uint16_t stride) const
{
    if (m_gpuKind.empty()) return;
    UINT index = 0;
    const int heap = Locate(written, index);
    if (heap < 0) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    SetKindsLocked(heap, index, count, kind, stride);
}

void DescriptorHeapAllocator::BumpGeneration(UINT gpuIndex)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    BumpGenLocked(gpuIndex, 1);
}

DescriptorHeapAllocator::SlotRecord DescriptorHeapAllocator::RecordAtGpu(UINT gpuIndex) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (gpuIndex >= m_gpuKind.size())
        return { static_cast<uint8_t>(BK_FREE), 0, 0 };
    return { m_gpuKind[gpuIndex], m_gpuGen[gpuIndex], m_gpuStride[gpuIndex] };
}

void DescriptorHeapAllocator::KindHistogram(UINT (&out)[256]) const
{
    for (UINT& n : out) n = 0;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_gpuKind.empty()) return;
    const UINT staticEnd = (std::min)(m_gpuSlots.Cursor(), static_cast<UINT>(m_gpuKind.size()));
    for (UINT i = 0; i < staticEnd; ++i)
        ++out[m_gpuKind[i]];
    for (UINT i = m_gpuStaticCapacity; i < m_gpuKind.size(); ++i)
        ++out[m_gpuKind[i]];
}

// ==== Heap accessors ====

ID3D12DescriptorHeap* DescriptorHeapAllocator::GetHeap() const
{
    return m_shaderVisible ? m_gpuHeap.Get() : m_cpuHeap.Get();
}

ID3D12DescriptorHeap* DescriptorHeapAllocator::GetGpuHeap() const
{
    return m_gpuHeap.Get();
}

ID3D12DescriptorHeap* DescriptorHeapAllocator::GetCpuHeap() const
{
    return m_cpuHeap.Get();
}

UINT DescriptorHeapAllocator::GetDescriptorSize() const
{
    return m_descriptorSize;
}

bool DescriptorHeapAllocator::IsShaderVisible() const
{
    return m_shaderVisible;
}

UINT DescriptorHeapAllocator::GetCapacity() const
{
    return m_shaderVisible ? (m_gpuStaticCapacity + m_gpuDynamicCapacity) : m_cpuCapacity;
}

// ==== Observability ====
// The free-list readers take m_mutex: AllocateStatic / FreeStatic push and pop
// those vectors from RenderGraph workers, so an unlocked size() would race a
// reallocation. Do not call them from inside a locked allocator path: m_mutex is
// not recursive.

void DescriptorHeapAllocator::GetFreeListBlocks(UINT& cpuBlocks,
                                                UINT& gpuStaticBlocks) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    cpuBlocks       = static_cast<UINT>(m_cpuSlots.FreeBlocks());
    gpuStaticBlocks = static_cast<UINT>(m_gpuSlots.FreeBlocks());
}

UINT DescriptorHeapAllocator::GetStaticUsed() const
{
    // Monotonic high-water cursor, not live occupancy (see the header).
    return m_shaderVisible ? m_gpuSlots.Cursor() : m_cpuSlots.Cursor();
}

UINT DescriptorHeapAllocator::GetStaticCapacity() const
{
    // The effective wall is whichever region runs out first, because
    // AllocateStatic reserves a slot in CPU staging AND in GPU static.
    if (!m_shaderVisible)
        return m_cpuCapacity;
    return (m_cpuCapacity < m_gpuStaticCapacity) ? m_cpuCapacity : m_gpuStaticCapacity;
}

UINT DescriptorHeapAllocator::GetStaticFreeBlocks() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<UINT>(m_shaderVisible ? m_gpuSlots.FreeBlocks()
                                             : m_cpuSlots.FreeBlocks());
}

UINT DescriptorHeapAllocator::FreeBlocksCovering(const DescriptorAllocation& alloc) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<UINT>(m_cpuSlots.BlocksCovering(alloc.m_cpuIndex));
}

void DescriptorHeapAllocator::ReportResidual(const char* label) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<DescriptorSlotRegion::Block> runs;
    const size_t total = m_cpuSlots.LiveRuns(runs, 8);
    std::string first;
    for (const DescriptorSlotRegion::Block& r : runs)
    {
        if (!first.empty()) first += ',';
        first += std::to_string(r.start) + "+" + std::to_string(r.count);
    }
    LOG_INFO("[POOL] %s residual live=%u runs=%zu doubleFrees=%llu first=%s",
             label, m_cpuSlots.Live(), total, m_doubleFrees, first.empty() ? "-" : first.c_str());
}
