#include "Graphics/GpuMemoryLedger.h"
#include "Diagnostics/Profiler.h"   // DIAG_PROFILER
#include "Graphics/VideoMemory.h"   // a render target's residency priority
#include "Graphics/ViewSlot.h"      // RHI::kMaxViews
#include "System/Log.h"

#include <d3d12.h>
#include <atomic>
#include <cstdio>
#include <new>
#include <string>
#include <vector>

#if DIAG_PROFILER

namespace
{
    // {5B7E4C1A-8F2D-4E6B-9C3A-1D2E3F405161}
    const GUID kLedgerTag = { 0x5b7e4c1a, 0x8f2d, 0x4e6b, { 0x9c, 0x3a, 0x1d, 0x2e, 0x3f, 0x40, 0x51, 0x61 } };

    // The secondary view this thread's allocations are counted to (SetViewAttribution), and each view's bytes.
    thread_local uint32_t t_viewSlot = 0;
    std::atomic<int64_t>  g_viewBytes[RHI::kMaxViews];

    // Held by D3D12 as the object's private data; its last Release - which is
    // D3D12 destroying the object - takes the bytes back out of the ledger.
    class LedgerTag final : public IUnknown
    {
    public:
        LedgerTag(Diag::Prof::GpuMemCat cat, uint64_t bytes, bool placed, uint64_t registered, uint32_t view) noexcept
            : m_cat(cat), m_bytes(bytes), m_placed(placed), m_registered(registered), m_view(view) {}

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
        {
            if (!out) return E_POINTER;
            if (riid == __uuidof(IUnknown)) { *out = static_cast<IUnknown*>(this); AddRef(); return S_OK; }
            *out = nullptr;
            return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return m_ref.fetch_add(1, std::memory_order_relaxed) + 1; }
        ULONG STDMETHODCALLTYPE Release() override
        {
            const ULONG n = m_ref.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (n == 0) delete this;
            return n;
        }

    private:
        ~LedgerTag()
        {
            if (m_placed) Diag::Prof::NoteGpuPlaced(-1);
            else          Diag::Prof::NoteGpuFree(m_cat, m_bytes);
            Diag::Prof::UnregisterGpuObject(m_registered);
            if (m_view && !m_placed)
                g_viewBytes[m_view].fetch_sub(static_cast<int64_t>(m_bytes), std::memory_order_relaxed);
        }
        std::atomic<ULONG>    m_ref{ 1 };
        Diag::Prof::GpuMemCat m_cat;
        uint64_t              m_bytes;
        bool                  m_placed;
        uint64_t              m_registered;   // its live-object entry (0: none)
        uint32_t              m_view;         // the secondary view it is counted to (0: none)
    };

    void Attach(ID3D12Object* obj, Diag::Prof::GpuMemCat cat, uint64_t bytes, bool placed,
                const char* name) noexcept
    {
        if (!obj) return;
        if (placed) Diag::Prof::NoteGpuPlaced(+1);
        else        Diag::Prof::NoteGpuAlloc(cat, bytes);
        // What holds bytes is listed in the live-object list, under its name.
        const uint64_t registered = (!placed && bytes) ? Diag::Prof::RegisterGpuObject(cat, bytes, name) : 0;
        const uint32_t view = t_viewSlot;
        if (view && !placed) g_viewBytes[view].fetch_add(static_cast<int64_t>(bytes), std::memory_order_relaxed);
        LedgerTag* tag = new (std::nothrow) LedgerTag(cat, bytes, placed, registered, view);
        if (!tag)
        {
            if (view && !placed) g_viewBytes[view].fetch_sub(static_cast<int64_t>(bytes), std::memory_order_relaxed);
            // Nothing will ever subtract it - take it back out now.
            if (placed) Diag::Prof::NoteGpuPlaced(-1);
            else        Diag::Prof::NoteGpuFree(cat, bytes);
            Diag::Prof::UnregisterGpuObject(registered);
            return;
        }
        // D3D12 AddRefs the tag; ours is dropped right after. On failure the
        // Release below destroys the tag and the destructor evens the ledger.
        obj->SetPrivateDataInterface(kLedgerTag, tag);
        tag->Release();
    }
}

namespace GpuLedger
{
    void Track(ID3D12Object* obj, Diag::Prof::GpuMemCat cat, uint64_t bytes, const char* name) noexcept
    {
        Attach(obj, cat, bytes, false, name);
    }

    void TrackCommitted(ID3D12Device* dev, ID3D12Resource* res, Diag::Prof::GpuMemCat cat, const char* name) noexcept
    {
        if (!dev || !res) return;
        const D3D12_RESOURCE_DESC d = res->GetDesc();
        const D3D12_RESOURCE_ALLOCATION_INFO ai = dev->GetResourceAllocationInfo(0, 1, &d);
        Attach(res, cat, ai.SizeInBytes, false, name);
    }

    void TrackTexture(ID3D12Device* dev, ID3D12Resource* res, const char* name) noexcept
    {
        if (!res) return;
        const D3D12_RESOURCE_DESC d = res->GetDesc();
        using C = Diag::Prof::GpuMemCat;
        const C cat = (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) ? C::DepthStencil
                    : (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ? C::RenderTarget
                                                                          : C::Texture;
        TrackCommitted(dev, res, cat, name);
        VideoMemory::PrioritizeTexture(dev, res);
    }

    void TrackHeap(ID3D12Heap* heap, Diag::Prof::GpuMemCat cat, const char* name) noexcept
    {
        if (!heap) return;
        Attach(heap, cat, heap->GetDesc().SizeInBytes, false, name);
    }

    void TrackPlaced(ID3D12Resource* res) noexcept
    {
        Attach(res, Diag::Prof::GpuMemCat::Other, 0, true, nullptr);
    }

    void TrackDescriptorHeap(ID3D12Device* dev, ID3D12DescriptorHeap* heap, const char* name) noexcept
    {
        if (!dev || !heap) return;
        const D3D12_DESCRIPTOR_HEAP_DESC d = heap->GetDesc();
        if (!(d.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)) return;   // CPU-only: system RAM
        const uint64_t bytes = uint64_t(d.NumDescriptors) * dev->GetDescriptorHandleIncrementSize(d.Type);
        Attach(heap, Diag::Prof::GpuMemCat::DescriptorHeap, bytes, false, name);
    }
}

namespace GpuLedger
{
    uint32_t SetViewAttribution(uint32_t slot) noexcept
    {
        const uint32_t previous = t_viewSlot;
        t_viewSlot = slot < RHI::kMaxViews ? slot : 0u;
        return previous;
    }
    uint64_t ViewBytes(uint32_t slot) noexcept
    {
        if (slot == 0 || slot >= RHI::kMaxViews) return 0;
        const int64_t bytes = g_viewBytes[slot].load(std::memory_order_relaxed);
        return bytes > 0 ? static_cast<uint64_t>(bytes) : 0u;
    }

    void LogResidual()
    {
        namespace P = Diag::Prof;
        P::GpuLedger ledger;
        P::ReadGpuLedger(ledger);
        std::vector<P::GpuObjectInfo> top;
        P::ReadGpuObjects(top, 5);
        std::string categories, names;
        char one[128];
        for (int i = 0; i < P::kGpuMemCats; ++i)
        {
            if (!ledger.bytes[i] && ledger.count[i] <= 0) continue;
            std::snprintf(one, sizeof(one), "%s%s=%.2fMiB/%lld", categories.empty() ? "" : " ",
                          P::GpuMemCatName(static_cast<P::GpuMemCat>(i)), double(ledger.bytes[i]) / 1048576.0,
                          static_cast<long long>(ledger.count[i]));
            categories += one;
        }
        for (const P::GpuObjectInfo& o : top)
        {
            std::snprintf(one, sizeof(one), "%s%s=%.2fMiB", names.empty() ? "" : ", ",
                          o.name.empty() ? "(unnamed)" : o.name.c_str(), double(o.bytes) / 1048576.0);
            names += one;
        }
        LOG_INFO("[LEDGER] residual: total=%.2f MiB placed=%lld categories: %s; largest: %s",
                 double(ledger.totalBytes) / 1048576.0, static_cast<long long>(ledger.placed),
                 categories.empty() ? "-" : categories.c_str(), names.empty() ? "-" : names.c_str());
    }
}

#else // !DIAG_PROFILER

namespace GpuLedger
{
    void Track(ID3D12Object*, Diag::Prof::GpuMemCat, uint64_t, const char*) noexcept {}
    void TrackCommitted(ID3D12Device*, ID3D12Resource*, Diag::Prof::GpuMemCat, const char*) noexcept {}
    void TrackTexture(ID3D12Device* dev, ID3D12Resource* res, const char*) noexcept
    {
        VideoMemory::PrioritizeTexture(dev, res);
    }
    void TrackHeap(ID3D12Heap*, Diag::Prof::GpuMemCat, const char*) noexcept {}
    void TrackPlaced(ID3D12Resource*) noexcept {}
    void TrackDescriptorHeap(ID3D12Device*, ID3D12DescriptorHeap*, const char*) noexcept {}
    uint32_t SetViewAttribution(uint32_t) noexcept { return 0; }
    uint64_t ViewBytes(uint32_t) noexcept { return 0; }
    void LogResidual() {}
}

#endif
