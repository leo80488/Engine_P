#pragma once

// DescriptorHeapAllocator - two-tier CPU/GPU descriptor heap manager.
//
// Shader-visible types (CBV_SRV_UAV, SAMPLER):
//   CPU staging heap - non-shader-visible; descriptors are built here (CreateSRV ...).
//   GPU heap         - shader-visible, two regions:
//     [0 .. staticCap-1]      static region, persistent allocations
//     [staticCap .. total-1]  dynamic region, a per-frame ring
//   allocation.CopyToGpu() copies a staged descriptor into the static region;
//   AllocateDynamic() copies CPU descriptors straight into the ring.
// CPU-only types (RTV, DSV): only the CPU heap exists, AllocateDynamic() is invalid.
// Init(device, type, cpuStagingCount, gpuStaticCount, gpuDynamicCount) with both GPU
// counts 0 makes a CPU-only allocator.
//
// Thread safety: allocation, reclamation, free-list changes and CPU->GPU copies are
// serialized internally; heap accessors are safe once Init() has completed.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d12.h>
#include <wrl.h>
#include <vector>
#include <deque>
#include <cstdint>
#include <mutex>
#include <string>     // the fresh-slot tag histogram

#include "Graphics/DescriptorSlotRegion.h"

class DescriptorHeapAllocator;

// ==== DescriptorAllocation ====
// Returned by Allocate / AllocateStatic: the CPU staging and GPU static handles of
// the same descriptor slot(s).
struct DescriptorAllocation
{
    DescriptorAllocation() = default;

    // CPU staging heap CPU handle - pass to CreateSRV / CreateCBV / etc.
    D3D12_CPU_DESCRIPTOR_HANDLE GetCpuHandle() const;

    // GPU heap CPU handle - for direct GPU-heap writes (e.g. ImGui callbacks) or as the
    // CopyDescriptorsSimple destination.
    D3D12_CPU_DESCRIPTOR_HANDLE GetGpuCpuHandle() const;

    // GPU heap GPU handle - pass to SetGraphicsRootDescriptorTable.
    D3D12_GPU_DESCRIPTOR_HANDLE GetGpuHandle() const;

    UINT GetCount()          const;
    bool IsShaderVisible()   const;
    bool IsValid()           const;
    // The first slot's index in the shader-visible heap (the slot a generation is kept
    // for). 0 for a CPU-only allocator.
    UINT GetGpuIndex()       const { return m_gpuIndex; }

    // Copies the descriptors written via GetCpuHandle() from CPU staging into the GPU
    // static region (CopyDescriptorsSimple). No-op for CPU-only allocators (RTV / DSV).
    void CopyToGpu();

    // Returns all slots to the allocator's free lists.
    void Free();

private:
    friend class DescriptorHeapAllocator;

    D3D12_CPU_DESCRIPTOR_HANDLE m_cpuHandle    {};   // CPU staging
    D3D12_CPU_DESCRIPTOR_HANDLE m_gpuCpuHandle {};   // GPU heap CPU handle (shader-visible types only)
    D3D12_GPU_DESCRIPTOR_HANDLE m_gpuHandle    {};   // GPU heap GPU handle (shader-visible types only)

    UINT  m_cpuIndex  { 0 };
    UINT  m_gpuIndex  { 0 };
    UINT  m_count     { 0 };
    bool  m_shaderVisible { false };
    // The tag-histogram row this allocation was counted into, so Free() counts the
    // release against the same row. ~0u = untagged or a shader-visible allocator (which
    // records no tags). An index, not a pointer: the rows live in a growing std::vector.
    UINT  m_tagIndex  { ~0u };

    DescriptorHeapAllocator* m_allocator { nullptr };
};

// ==== DescriptorHeapAllocator ====
class DescriptorHeapAllocator
{
public:
    struct SlotRecord { uint8_t kind; uint16_t gen; uint16_t stride; };

    DescriptorHeapAllocator() = default;

    // @p cpuStagingCount: non-shader-visible staging slots; @p gpuStaticCount /
    // @p gpuDynamicCount: the shader-visible static region and ring sizes. For RTV / DSV
    // pass 0 for both GPU counts.
    void Init(
        ID3D12Device*              device,
        D3D12_DESCRIPTOR_HEAP_TYPE type,
        UINT                       cpuStagingCount,
        UINT                       gpuStaticCount,
        UINT                       gpuDynamicCount);

    // Releases both heaps and clears allocation state. Call only after every
    // outstanding DescriptorAllocation has been freed or destroyed.
    void Reset();

    // ---- Static allocation ----
    // Allocates @p count contiguous slots in the static region, with both CPU staging and
    // GPU handles; call allocation.CopyToGpu() after writing to the CPU handle.
    //
    // @p tag is a diagnostic label, not an identity. Only CPU-only allocators (RTV / DSV)
    // record it, and only for allocations that advanced the cursor (not served from the
    // free list), i.e. those that raised the high-water GetStaticUsage reports, so the
    // histogram says which allocations a rise came from. The string is copied under the
    // allocator's mutex before the call returns (rows match by content), so @p tag only
    // has to live for the call: a stack buffer or a temporary's c_str() is fine.
    DescriptorAllocation Allocate(UINT count, const char* tag = nullptr);
    DescriptorAllocation AllocateStatic(UINT count, const char* tag = nullptr);

    // Logs the fresh-slot tag histogram, newest-first by count, one line per tag under a
    // `[POOL] <label> fresh-slot tags:` header. No-op for shader-visible allocators and
    // for one nothing tagged. Takes the mutex and allocates: a shutdown / boundary call
    // (the device calls it at shutdown beside the other pool lines), never per frame.
    void ReportTagHistogram(const char* label) const;

    // ---- Observability ----
    // Occupancy, used/capacity per region, so pressure shows before it becomes a driver
    // fault.
    //
    // Read this before asserting on the number: it is a monotonic high-water cursor (each
    // region's DescriptorSlotRegion::Cursor), not live occupancy - FreeStatic pushes onto a
    // free list and never lowers the cursor. A flat value proves the cursor is bounded,
    // never that anything was recycled; the recycling signal is the free-list block count.
    // The 75%/90% pressure notices in AllocateStatic watch the CPU staging region only: a
    // shader-visible allocator whose GPU static region is the smaller one fills it with no
    // notice, and only ReportExhausted fires at the failure.
    void GetStaticUsage(UINT& cpuUsed, UINT& cpuCap,
                        UINT& gpuUsed, UINT& gpuCap) const
    {
        cpuUsed = m_cpuSlots.Cursor(); cpuCap = m_cpuCapacity;
        gpuUsed = m_gpuSlots.Cursor(); gpuCap = m_gpuStaticCapacity;
    }
    unsigned long long GetExhaustedCount() const { return m_exhaustedCount; }
    // FreeStatic calls refused as a second Free of one allocation. An unlocked read of a
    // mutex-guarded counter, like GetExhaustedCount.
    unsigned long long GetDoubleFreeCount() const { return m_doubleFrees; }

    // Logs the static slots still handed out and the double frees refused, on one name=value
    // line a pack can bound: `[POOL] <label> residual live= runs= doubleFrees= first=`
    // (first: up to eight runs as start+count). For shutdown, before Reset: what is live
    // then is what its owners never freed, the device pools' views included (the pools are
    // cleared, not freed). Takes the mutex and walks the handed-out bits up to the cursor.
    void ReportResidual(const char* label) const;

    // Free-list blocks that hold @p alloc's first CPU staging slot: 1 once it is freed, 2
    // if a second Free put it there again. For the device's lifetime probe.
    UINT FreeBlocksCovering(const DescriptorAllocation& alloc) const;

    // Blocks on the static free lists. The lists are not coalesced and AllocateStatic
    // scans them linearly under the mutex, so this is both the recycling signal and a
    // fragmentation / cost signal (N blocks = every allocation may walk N entries). For
    // shader-visible allocators the two lists move in lockstep; they are reported
    // separately only to make a divergence visible.
    void GetFreeListBlocks(UINT& cpuBlocks, UINT& gpuStaticBlocks) const;

    // One-scalar forms for telemetry tables (Debug.Stats, editor panels), of the region
    // that governs this allocator: GPU static for shader-visible types, CPU staging for
    // RTV / DSV. GetStaticCapacity is the effective ceiling, min(cpu, gpu) for
    // shader-visible types, because AllocateStatic needs a slot in both regions.
    UINT GetStaticUsed()       const;
    UINT GetStaticCapacity()   const;
    // Static slots allocated and not yet freed - the live count the high-water cursor
    // (GetStaticUsage) is not. An unlocked read of a mutex-guarded counter: a gauge, one
    // frame stale is fine.
    UINT GetStaticLive() const { return m_cpuSlots.Live(); }
    UINT GetStaticFreeBlocks() const;

    // ---- Dynamic ring-buffer allocation ----
    // Copies @p count descriptors from @p srcCpuHandle into the dynamic ring and returns
    // the GPU handle for this frame's draw. @p fenceValue is the fence value signaled
    // after the GPU work using this allocation is submitted. Returns {0} when the ring
    // is full.
    D3D12_GPU_DESCRIPTOR_HANDLE AllocateDynamic(
        UINT                        count,
        D3D12_CPU_DESCRIPTOR_HANDLE srcCpuHandle,
        UINT64                      fenceValue);

    // Reclaims dynamic slots whose fenceValue <= @p completedFence.
    void ReclaimDynamic(UINT64 completedFence);

    // ---- Heap accessors ----
    // Primary heap: the GPU heap for shader-visible types, the CPU heap otherwise. For
    // ID3D12GraphicsCommandList::SetDescriptorHeaps.
    ID3D12DescriptorHeap* GetHeap()    const;

    // GPU-visible heap (nullptr for CPU-only types).
    ID3D12DescriptorHeap* GetGpuHeap() const;

    // CPU staging heap.
    ID3D12DescriptorHeap* GetCpuHeap() const;

    UINT GetDescriptorSize() const;
    bool IsShaderVisible()   const;
    UINT GetCapacity()       const;   // GPU total (or CPU total for CPU-only)

    // ---- Internal (called by DescriptorAllocation) ----
    // @p tagIndex is the histogram row the allocation was counted into; ~0u for
    // untagged / shader-visible allocators. Refused (logged once, counted in
    // GetDoubleFreeCount) unless every slot is handed out in both regions: a second Free
    // of one allocation, through a copy of it, would otherwise give its slots to two owners.
    void FreeStatic(UINT cpuIndex, UINT gpuIndex, UINT count,
                    UINT tagIndex = ~0u);
    void CopyToGpu (UINT cpuIndex, UINT gpuIndex, UINT count);

    // ---- What each descriptor is ----
    // The shader-visible CBV_SRV_UAV allocator records, per descriptor in both regions, a
    // kind byte (shaders/shared/bindless_kind.hlsli) and a structured stride, plus a
    // 12-bit generation per GPU slot. So every descriptor written into its heaps goes
    // through it: WriteSrv / WriteUav / WriteCbv derive the kind from the view description
    // and record it at the handle written (CPU staging or the GPU heap's CPU handle), and
    // CopyDescriptors / CopyToGpu / AllocateDynamic carry the record along. A writer the
    // engine does not own (the ImGui backend) reports with NoteKind. FreeStatic marks
    // slots free and bumps the generation; BumpGeneration does that for a slot reused in
    // place (a bindless texture table entry). A handle outside these heaps is written but
    // not recorded; the other allocators record nothing.
    bool     TracksKinds() const { return !m_gpuKind.empty(); }
    void     WriteSrv(ID3D12Resource* res, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,
                      D3D12_CPU_DESCRIPTOR_HANDLE dst) const;
    void     WriteUav(ID3D12Resource* res, ID3D12Resource* counter,
                      const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc,
                      D3D12_CPU_DESCRIPTOR_HANDLE dst) const;
    void     WriteCbv(const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc,
                      D3D12_CPU_DESCRIPTOR_HANDLE dst) const;
    void     CopyDescriptors(UINT count, D3D12_CPU_DESCRIPTOR_HANDLE dst,
                             D3D12_CPU_DESCRIPTOR_HANDLE src) const;
    void     NoteKind(D3D12_CPU_DESCRIPTOR_HANDLE written, uint8_t kind,
                      UINT count = 1, uint16_t stride = 0) const;
    void     BumpGeneration(UINT gpuIndex);

    // What a GPU heap slot holds: its kind (BK_FREE past the end or when not recording),
    // its generation and its structured stride (0 unless structured).
    SlotRecord RecordAtGpu(UINT gpuIndex) const;
    // GPU slot count per kind byte, over every slot ever handed out (static high-water +
    // the dynamic ring). out[BK_FREE] counts the freed ones.
    void     KindHistogram(UINT (&out)[256]) const;
    // The bindless debug lane's (-bindless_validate) GPU copy of the GPU-slot record: one
    // uint per slot, kind | gen << 8, in a mapped UPLOAD buffer shaders read at
    // BINDLESS_VALIDATE_TABLE. Attaching fills it; every later change is written through
    // under the same lock. @p entries: the mirror's length in uints.
    void     AttachGpuMirror(uint32_t* mapped, UINT entries);

    // View description -> kind (null resource -> BK_NULL). A null @p desc means the
    // resource's default view.
    static uint8_t KindOfSrv(ID3D12Resource* res, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc);
    static uint8_t KindOfUav(ID3D12Resource* res, const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc);
    static uint8_t KindOfCbv(const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc);

private:
    struct DynamicEntry{ UINT64 fenceValue; UINT physOffset; UINT count; };

    // ---- Fresh-slot tag histogram ----
    struct TagCount { std::string tag; UINT fresh; UINT reused; UINT freed; };
    static constexpr size_t kMaxTagRows = 64;

    // ---- Heap creation and exhaustion ----
    void ReportExhausted(const char* region, UINT count, UINT used, UINT capacity);
    static UINT CreateHeapWithFallback(ID3D12Device* device,
                                       D3D12_DESCRIPTOR_HEAP_TYPE type,
                                       UINT requested,
                                       D3D12_DESCRIPTOR_HEAP_FLAGS flags,
                                       Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>& outHeap,
                                       const char* label);

    // ---- Fresh-slot tag histogram ----
    // Called with m_mutex held. Returns the row index (or ~0u).
    UINT NoteTag(const char* tag, bool fresh) const;

    // ---- Descriptor kind records (see the public block) ----
    // Which heap a CPU handle points into: 0 staging, 1 GPU heap, -1 neither.
    // Reads only what Init set, so it needs no lock.
    int  Locate(D3D12_CPU_DESCRIPTOR_HANDLE h, UINT& index) const;
    // Called with m_mutex held.
    void SetKindsLocked(int heap, UINT index, UINT count, uint8_t kind, uint16_t stride) const;
    void CopyKindsLocked(int dstHeap, UINT dst, int srcHeap, UINT src, UINT count) const;
    void BumpGenLocked(UINT gpuIndex, UINT count);

    void MirrorLocked(UINT gpuIndex, UINT count) const;

    // ---- CPU staging heap (always present) ----
    ID3D12Device*                               m_device         { nullptr };
    D3D12_DESCRIPTOR_HEAP_TYPE                  m_type           {};
    UINT                                        m_descriptorSize { 0 };
    bool                                        m_shaderVisible  { false };

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_cpuHeap;
    unsigned long long m_exhaustedCount = 0;
    unsigned long long m_doubleFrees    = 0;   // FreeStatic calls refused
    bool m_warned75 = false;
    bool m_warned90 = false;

    UINT m_cpuCapacity  { 0 };
    // The staging slots' cursor, free list and handed-out bits (the live count included).
    DescriptorSlotRegion m_cpuSlots;

    // ---- GPU shader-visible heap (shader-visible types only) ----
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_gpuHeap;
    UINT m_gpuStaticCapacity  { 0 };
    UINT m_gpuDynamicCapacity { 0 };

    // GPU static: the same bookkeeping, sized m_gpuStaticCapacity.
    DescriptorSlotRegion m_gpuSlots;

    // GPU dynamic: ring buffer at heap indices [m_gpuStaticCapacity .. gpuTotal-1].
    UINT m_dynamicHead { 0 };   // next write offset (0-based within dynamic region)
    UINT m_dynamicUsed { 0 };   // slots currently in flight
    std::deque<DynamicEntry> m_dynamicQueue;

    // ---- Fresh-slot tag histogram ----
    // CPU-only allocators (RTV / DSV) only, and only allocations not served from the
    // free list. A flat vector, not a map: few distinct tags (one per texture name that
    // ever needed a view), the scan runs under a mutex already held, and insertion order
    // is information (boot first, then the session).
    // Bounded by kMaxTagRows so thousands of distinct names cannot leak; past the cap
    // everything lands in one "(other)" row. `freed` shows the residue: fresh > freed
    // left slots in use, fresh == freed handed every one back (cursor up, free list up
    // by the same amount, net occupancy unchanged).
    mutable std::vector<TagCount> m_tagCounts;
    mutable UINT m_tagOverflowFresh  = 0;
    mutable UINT m_tagOverflowReused = 0;
    mutable UINT m_untaggedFresh     = 0;
    mutable UINT m_untaggedReused    = 0;

    // ---- Descriptor kind records (see the public block) ----
    // `mutable`: they shadow what the heaps hold, and the device writes descriptors from
    // const paths (CreateTexture2DSRVAtCpu ...). Sized once in Init, never reallocated,
    // guarded by m_mutex. 1M + 1M slots: 2 MB of kinds, 4 MB of strides, 2 MB of
    // generations.
    mutable std::vector<uint8_t>  m_cpuKind;     // per CPU staging slot
    mutable std::vector<uint8_t>  m_gpuKind;     // per GPU slot (static + dynamic)
    mutable std::vector<uint16_t> m_cpuStride;   // structured stride, per CPU staging slot
    mutable std::vector<uint16_t> m_gpuStride;   // structured stride, per GPU slot
    std::vector<uint16_t>         m_gpuGen;      // per GPU slot; the low 12 bits count
    SIZE_T m_cpuStart    { 0 };   // CPU handle of the staging heap's slot 0
    SIZE_T m_gpuCpuStart { 0 };   // CPU handle of the GPU heap's slot 0

    // The debug lane's mirror (AttachGpuMirror); null outside it. Points into mapped
    // memory, written from const paths like the record it copies.
    uint32_t* m_gpuMirror        = nullptr;
    UINT      m_gpuMirrorEntries = 0;

    // Resource creation runs on RenderGraph workers and the renderer's post-process
    // worker: every mutable cursor and free list is guarded so two lazy resize / rebuild
    // paths cannot hand out the same slot.
    mutable std::mutex m_mutex;
};
