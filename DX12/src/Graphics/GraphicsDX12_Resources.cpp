#include "Graphics/GraphicsDX12.h"
#include "System/BoundedParallel.h"   // CreatePipelineStates
#include "Graphics/GpuMemoryLedger.h"   // the profiler's GPU resource ledger
#include "Graphics/GraphicsDX12Internal.h"
#include "Graphics/DxcCompiler.h"
#include "Graphics/PipelineStats.h"        // first-compile / pipeline-creation counters
#include "Graphics/UploadStats.h"          // upload-wait totals
#include "Diagnostics/Profiler.h"          // PROF_SCOPE around the driver's PSO builds
#include "System/Check.h"
#include "System/TaskSystem.h"             // OnWorkerThread (UploadStats::textureCreatesGame)
#include "System/ThreadContext.h"          // the Render context (the same)
#include "Graphics/TransientAliasArena.h"   // kTailGuard / kUsableSizeTag (placement range check)
#include "System/Log.h"
#include "d3dx12.h"
#include "Graphics/MeshPsoStream.h"   // the mesh PSO stream, shared with the boot precompile
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <chrono>     // FlushUploadAndWait stall instrumentation
#include <future>     // CreatePipelineStates: the driver's compiles side by side

using Microsoft::WRL::ComPtr;

// Resource creation and destruction for GraphicsDX12: Create{Buffer,Texture,Shader,Sampler,
// PipelineState}, Destroy{Buffer,Texture} and the batch-upload helpers that amortise per-buffer
// FlushUploadAndWait round trips. Kept apart so the command-recording side in GraphicsDX12.cpp
// stays readable.

thread_local GraphicsDX12::UploadContext* GraphicsDX12::t_loaderCtx   = nullptr;
thread_local uint32_t                     GraphicsDX12::t_loaderDepth = 0;

// ==== Loader uploads ====
// A loader worker's scope: a loader context of its own for the scope, which is one upload batch.
bool GraphicsDX12::BeginLoaderUploads()
{
    if (!m_loaderUploads) return false;
    if (t_loaderCtx) { ++t_loaderDepth; return true; }
    // The frame lanes keep their own contexts: a loader scope there would put
    // the frame's own staging into a context another thread may be submitting.
    (void)ENGINE_CHECK_ONCE(Sys::ThreadCtx::Current() != Sys::ThreadCtx::Render,
        "BeginLoaderUploads in the Render context - the loader contexts are for loading workers");
    UploadContext* ctx = nullptr;
    {
        std::unique_lock<std::mutex> lk(m_loaderMutex);
        m_loaderCv.wait(lk, [this] {
            for (bool busy : m_loaderCtxBusy) if (!busy) return true;
            return false;
        });
        for (uint32_t i = 0; i < kLoaderContexts; ++i)
            if (!m_loaderCtxBusy[i]) { m_loaderCtxBusy[i] = true; ctx = &m_loaderCtx[i]; break; }
    }
    t_loaderCtx   = ctx;
    t_loaderDepth = 1;
    ++ctx->batchDepth;   // the scope is one batch: its outermost End submits
    return true;
}

void GraphicsDX12::EndLoaderUploads()
{
    if (!t_loaderCtx) return;
    if (--t_loaderDepth > 0) return;
    UploadContext* ctx = t_loaderCtx;
    // Released on every exit - a flush that throws (a removed device) included -
    // or the context stays taken and this thread's next scope nests into it.
    struct Release
    {
        GraphicsDX12&  dev;
        UploadContext* ctx;
        ~Release()
        {
            ctx->batchDepth = 0;
            ctx->heapDepth  = 0;
            ctx->currentBufferHeap.Reset();
            ctx->heapOffset = ctx->heapSize = ctx->heapHint = 0;
            ctx->stagingKeepAlive.clear();
            ctx->stagingBytes = 0;
            t_loaderCtx   = nullptr;
            t_loaderDepth = 0;
            {
                std::lock_guard<std::mutex> lk(dev.m_loaderMutex);
                dev.m_loaderCtxBusy[ctx - dev.m_loaderCtx] = false;
            }
            dev.m_loaderCv.notify_one();
        }
    } release{ *this, ctx };
    // A heap or batch scope the owner left open would place or defer the next
    // owner's work in this one's.
    (void)ENGINE_CHECK_ONCE(ctx->heapDepth == 0 && ctx->batchDepth == 1,
        "EndLoaderUploads: an upload or heap batch opened in the scope is still open (batch %u, heap %u)",
        ctx->batchDepth, ctx->heapDepth);
    ctx->batchDepth = 1;
    // The scope's batch: execute, wait here, free the staging (Upload() is still this context until
    // `release` is destroyed).
    EndBufferUploadBatch();
}

// ==== Batch-upload scopes (CreateBuffer of a DEFAULT buffer defers its wait) ====
void GraphicsDX12::BeginBufferUploadBatch()
{
    // Nested scopes flatten into a counter; only the outermost pair flushes. The upload command list
    // is always open for recording (Reset at the end of LoadAssets / FlushUploadAndWait), so buffers
    // created inside the scope accumulate copies on it.
    ++Upload().batchDepth;
}

void GraphicsDX12::EndBufferUploadBatch()
{
    // Hoisted: Upload() picks the caller's context (loader scope or execution lane) on every call.
    UploadContext& ctx = Upload();
    if (ctx.batchDepth == 0)
    {
        LOG_ERROR("EndBufferUploadBatch called without matching Begin");
        return;
    }
    --ctx.batchDepth;
    if (ctx.batchDepth == 0)
    {
        // Final flush for the staging still queued: mid-batch flushes trim it, but a tail under
        // kBatchFlushBytes usually remains.
        FlushUploadAndWait();
        {
            // The batch's committed UPLOAD stagings, one per buffer / texture created with initial data,
            // released synchronously: over 10 ms on a cold prefab spawn, hence the scope.
            PROF_SCOPE("Gfx.ReleaseStaging");
            ctx.stagingKeepAlive.clear();
        }
        ctx.stagingBytes = 0;
    }
}

// ==== Buffer-heap suballocation scope ====
// Inside it, CreateBuffer places DEFAULT buffers into a shared heap. Nestable counter; only the
// outermost pair matters. Heaps made during the scope live on through each placed buffer's
// entry.placedHeap; End stops placing and drops the device's own reference to the last (maybe
// partly used) heap, so an unused tail is freed promptly.
void GraphicsDX12::BeginBufferHeapBatch(uint64_t heapBytesHint)
{
    UploadContext& ctx = Upload();
    if (ctx.heapDepth++ == 0) ctx.heapHint = heapBytesHint;
}

void GraphicsDX12::EndBufferHeapBatch()
{
    // Hoisted: Upload() picks the caller's context (loader scope or execution lane) on every call.
    UploadContext& ctx = Upload();
    if (ctx.heapDepth == 0)
    {
        LOG_ERROR("EndBufferHeapBatch called without matching Begin");
        return;
    }
    --ctx.heapDepth;
    if (ctx.heapDepth == 0)
    {
        ctx.currentBufferHeap.Reset();
        ctx.heapOffset = 0;
        ctx.heapSize   = 0;
        ctx.heapHint   = 0;
    }
}

void GraphicsDX12::FlushBatchStagingIfNeeded()
{
    // Hoisted: Upload() picks the caller's context (loader scope or execution lane) on every call.
    UploadContext& ctx = Upload();
    if (ctx.stagingBytes    < kBatchFlushBytes &&
        ctx.stagingKeepAlive.size() < kBatchFlushCount)
        return;

    // Mid-batch flush: execute the accumulated copies and wait for the GPU, so the staging buffers
    // can be released. The scope stays open (ctx.batchDepth unchanged): later CreateBuffer calls keep
    // deferring their waits.
    FlushUploadAndWait();
    ctx.stagingKeepAlive.clear();
    ctx.stagingBytes = 0;
}

void GraphicsDX12::FlushUploadAndWait()
{
    PROF_SCOPE("Gfx.FlushUpload");
    UploadContext& ctx = Upload();
    // Submit only the lists that hold work: with nothing recorded a flush returns at once (an empty
    // graphics list would still wait behind every frame queued ahead), under -upload_directqueue too,
    // which only moves the creation copies back to the graphics list. Every recording into either
    // list marks it dirty.
    const bool runCopy   = ctx.copyDirty;
    const bool runDirect = ctx.directDirty;
    if (!runCopy && !runDirect)
        return;   // nothing recorded since the last flush: nothing to wait for
    const auto _t0 = std::chrono::steady_clock::now();

    // The copy list first. It holds only copies into resources created since
    // the last flush, so nothing on the graphics list can have to precede it -
    // while a graphics command recorded after one of those creations may read
    // the new resource, so the graphics queue waits for this signal (GPU-side,
    // and only when both lists hold work).
    uint64_t cv = 0;
    if (runCopy)
    {
        ThrowIfFailed(ctx.copyList->Close());
        ID3D12CommandList* lists[] = { ctx.copyList.Get() };
        m_queues[2]->ExecuteCommandLists(1, lists);
        cv = ++ctx.copyFenceValue;
        ThrowIfFailed(m_queues[2]->Signal(ctx.copyFence.Get(), cv));
    }

    // Wait on this context's private fence, never the shared graphics counter: this upload has
    // nothing to do with the frame's submissions, and the shared counter would make one side's flush
    // wait for the other side's submissions, turning an independent upload into a stall.
    // Both sides' lists go to the same graphics queue, which is safe because this CPU-waits before
    // returning: neither side can observe the other's list half-executed.
    uint64_t fv = 0;
    if (runDirect)
    {
        if (runCopy)
            ThrowIfFailed(m_queues[0]->Wait(ctx.copyFence.Get(), cv));
        ThrowIfFailed(ctx.list->Close());
        ID3D12CommandList* lists[] = { ctx.list.Get() };
        m_queues[0]->ExecuteCommandLists(1, lists);
        fv = ++ctx.fenceValue;
        ThrowIfFailed(m_queues[0]->Signal(ctx.fence.Get(), fv));
    }
    // The copy queue holds nothing but these copies, so its wait is the copy;
    // the graphics signal queues behind every frame already submitted.
    if (runCopy)   WaitFence(ctx.copyFence.Get(), cv);
    if (runDirect) WaitFence(ctx.fence.Get(), fv);

    // Re-open for subsequent staging commands
    if (runCopy)
    {
        ThrowIfFailed(ctx.copyAllocator->Reset());
        ThrowIfFailed(ctx.copyList->Reset(ctx.copyAllocator.Get(), nullptr));
        ctx.copyDirty = false;
    }
    if (runDirect)
    {
        ThrowIfFailed(ctx.allocator->Reset());
        ThrowIfFailed(ctx.list->Reset(ctx.allocator.Get(), nullptr));
        ctx.directDirty = false;
    }
    // This whole round trip, dominated by the CPU-blocking fence wait, is the cost an async upload
    // path removes. The lane's resettable window and the process's never-reset totals
    // (frames.jsonl, Profiler.Upload) count the same span.
    const auto _took = std::chrono::steady_clock::now() - _t0;
    ctx.stallNs.fetch_add(UploadStats::ToNs(_took), std::memory_order_relaxed);
    ctx.flushCount.fetch_add(1, std::memory_order_relaxed);
    ctx.flushesLifetime.fetch_add(1, std::memory_order_relaxed);
    // A loader context's flush is a worker's wait, not a frame thread's.
    if (IsLoaderCtx(ctx))
    {
        UploadStats::loaderFlushes.fetch_add(1, std::memory_order_relaxed);
        UploadStats::loaderStallNs.fetch_add(UploadStats::ToNs(_took), std::memory_order_relaxed);
        return;
    }
    if (runCopy)   UploadStats::copyFlushes.fetch_add(1, std::memory_order_relaxed);
    if (runDirect) UploadStats::directFlushes.fetch_add(1, std::memory_order_relaxed);
    UploadStats::NoteFlush(_took);
    if (Sys::ThreadCtx::Current() != Sys::ThreadCtx::Render && !TaskSystem::OnWorkerThread())
        UploadStats::flushesGame.fetch_add(1, std::memory_order_relaxed);
}

// ==== Resource creation - CreateBuffer ====

// Unwind the views a partially built buffer already took; otherwise a failure part-way through
// CreateBuffer strands the earlier descriptors for the process lifetime, a permanent shrink of
// the heap.
static void ReleaseBufferViews(GPUBufferDX12& entry)
{
    if (entry.cbv.IsValid()) entry.cbv.Free();
    if (entry.srv.IsValid()) entry.srv.Free();
    if (entry.uav.IsValid()) entry.uav.Free();
    if (entry.clearUav.IsValid()) entry.clearUav.Free();
    entry.cbv = {}; entry.srv = {}; entry.uav = {}; entry.clearUav = {};
    entry.resource.Reset();
    entry.placedHeap.Reset();
}

// Texture twin, for a TextureDX12 that is still a LOCAL inside CreateTexture /
// CreateTexturePlaced: nothing has been published into m_texturePool, no
// bindless slot was taken and no command list can reference it, so every view
// is freed IMMEDIATELY (not through the deferred bucket, which would have no
// slot to reclaim). Walks the same view set DestroyTexture steals; vectors are
// walked whole and only the valid elements freed, so a failure at slice s or
// mip m of a partially-filled array unwinds cleanly.
static void ReleaseTextureViews(TextureDX12& entry)
{
    auto drop = [](DescriptorAllocation& a) { if (a.IsValid()) a.Free(); a = {}; };
    drop(entry.rtv);
    drop(entry.dsv);
    for (auto& a : entry.dsvReadOnly) drop(a);
    drop(entry.srv);
    drop(entry.uav);
    drop(entry.previewSrv);
    drop(entry.stencilSrv);
    drop(entry.uvPlaneSrv);
    for (auto& a  : entry.mipUavs)      drop(a);
    for (auto& a  : entry.sliceDsvs)    drop(a);
    for (auto& kv : entry.cubeFaceRtvs) drop(kv.second);
    for (auto& kv : entry.cubeMipUavs)  drop(kv.second);
    entry.mipUavs.clear();
    entry.sliceDsvs.clear();
    entry.cubeFaceRtvs.clear();
    entry.cubeMipUavs.clear();
    entry.resource.Reset();     // before the heap: a placed resource must not outlive it
    entry.placedHeap.Reset();
}

bool GraphicsDX12::CreateBuffer(const RHI::GPUBufferDesc& desc,
                                RHI::GPUBuffer& outBuffer,
                                const void* initialData)
{
    PROF_SCOPE_D("Gfx.CreateBuffer");
    if (std::this_thread::get_id() == m_deviceFrameThread.load(std::memory_order_acquire))
        DrainFrameIfNeeded(Diag::DrainCause::ResourceOp);
    // Hoisted: Upload() picks the caller's context (loader scope or execution lane) on every call.
    UploadContext& ctx = Upload();
    if (desc.size == 0) { LOG_ERROR("CreateBuffer: size == 0"); return false; }

    GPUBufferDX12 entry;
    D3D12_HEAP_PROPERTIES hp{ ToD3D12HeapType(desc.usage) };

    const bool isCBV = RHI::HasFlag(desc.bindFlags, RHI::BindFlag::CONSTANT_BUFFER);
    UINT64 allocSize = isCBV ? (desc.size + 255) & ~255ull : desc.size;

    D3D12_RESOURCE_DESC rd = CD3DX12_RESOURCE_DESC::Buffer(allocSize);
    rd.Flags = ToD3D12ResourceFlags(desc.bindFlags);

    D3D12_RESOURCE_STATES initState = D3D12_RESOURCE_STATE_COMMON;
    if (desc.usage == RHI::Usage::UPLOAD)
        initState = D3D12_RESOURCE_STATE_GENERIC_READ;
    else if (desc.usage == RHI::Usage::READBACK)
        initState = D3D12_RESOURCE_STATE_COPY_DEST;

    // Inside a BeginBufferHeapBatch scope, DEFAULT buffers are placed resources suballocated from a
    // shared heap instead of one committed resource (one implicit heap) each: a character's ~210
    // per-part skinned buffers take a couple of CreateHeap calls instead of ~210 heap allocations.
    // Every entry placed in the heap holds a reference to it (entry.placedHeap), so it outlives its
    // buffers through ComPtr + deferred release. UPLOAD / READBACK stay committed; any failure falls
    // back to committed.
    bool placed = false;
    if (ctx.heapDepth > 0 && desc.usage == RHI::Usage::DEFAULT)
    {
        const D3D12_RESOURCE_ALLOCATION_INFO ai = m_device->GetResourceAllocationInfo(0, 1, &rd);
        UINT64 off = (ctx.heapOffset + (ai.Alignment - 1)) & ~(ai.Alignment - 1);
        if (!ctx.currentBufferHeap || off + ai.SizeInBytes > ctx.heapSize)
        {
            // Current heap full (or none yet): make a fresh one. Earlier buffers keep their old heap alive
            // through their own entry.placedHeap.
            const uint64_t heapBytes = ctx.heapHint ? ctx.heapHint : kBufferHeapBytes;
            ctx.heapSize = (ai.SizeInBytes > heapBytes) ? ai.SizeInBytes : heapBytes;
            D3D12_HEAP_DESC hd{};
            hd.SizeInBytes        = ctx.heapSize;
            hd.Properties.Type    = D3D12_HEAP_TYPE_DEFAULT;
            hd.Alignment          = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
            // Every placed buffer carved from this heap is written before it is
            // read (CreateBuffer either uploads initialData or the owner fills
            // it), so the driver's zero-fill of a multi-MB heap is dead work.
            hd.Flags              = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS
                                  | D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;
            ctx.currentBufferHeap.Reset();
            if (FAILED(m_device->CreateHeap(&hd, IID_PPV_ARGS(&ctx.currentBufferHeap))))
            {
                LOG_ERROR("CreateBuffer: heap-batch CreateHeap failed (%llu B) — committed fallback",
                          (unsigned long long)ctx.heapSize);
                ctx.currentBufferHeap.Reset();
            }
            if (ctx.currentBufferHeap)
                GpuLedger::TrackHeap(ctx.currentBufferHeap.Get(), Diag::Prof::GpuMemCat::BufferHeap,
                                     "GraphicsDX12.BufferHeap");
            ctx.heapOffset = 0;
            off = 0;
        }
        if (ctx.currentBufferHeap &&
            SUCCEEDED(m_device->CreatePlacedResource(
                ctx.currentBufferHeap.Get(), off, &rd, initState, nullptr,
                IID_PPV_ARGS(&entry.resource))))
        {
            entry.placedHeap   = ctx.currentBufferHeap;   // ComPtr ref keeps the heap alive
            GpuLedger::TrackPlaced(entry.resource.Get());
            ctx.heapOffset = off + ai.SizeInBytes;
            placed = true;
        }
    }
    if (!placed)
    {
        if (FAILED(m_device->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &rd, initState, nullptr,
                IID_PPV_ARGS(&entry.resource))))
        {
            LOG_ERROR("CreateBuffer: CreateCommittedResource failed");
            return false;
        }
        GpuLedger::TrackCommitted(m_device.Get(), entry.resource.Get(),
            desc.usage == RHI::Usage::UPLOAD   ? Diag::Prof::GpuMemCat::Upload
          : desc.usage == RHI::Usage::READBACK ? Diag::Prof::GpuMemCat::Readback
                                               : Diag::Prof::GpuMemCat::Buffer,
            desc.debugName);
    }
    entry.state = initState;
    // The buffer's name, for captures and the GPU memory report.
    if (desc.debugName && desc.debugName[0] != '\0')
    {
        const int needed = MultiByteToWideChar(CP_UTF8, 0, desc.debugName, -1, nullptr, 0);
        if (needed > 0)
        {
            std::wstring wname(static_cast<size_t>(needed - 1), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, desc.debugName, -1, wname.data(), needed);
            entry.resource->SetName(wname.c_str());
        }
    }

    if (initialData && desc.usage == RHI::Usage::UPLOAD)
    {
        void* mapped = nullptr;
        D3D12_RANGE readRange{ 0, 0 };
        const HRESULT hrMap = entry.resource->Map(0, &readRange, &mapped);
        if (FAILED(hrMap) || !mapped)
        {
            // Refuse: the buffer would come back with uninitialised contents behind a valid handle.
            LOG_ERROR("CreateBuffer: Map failed on a %llu-byte UPLOAD buffer (hr=0x%08X) — buffer NOT created",
                      (unsigned long long)desc.size, static_cast<unsigned>(hrMap));
            return false;
        }
        std::memcpy(mapped, initialData, static_cast<size_t>(desc.size));
        entry.resource->Unmap(0, nullptr);
    }
    else if (initialData && desc.usage == RHI::Usage::DEFAULT)
    {
        ComPtr<ID3D12Resource> staging;
        D3D12_HEAP_PROPERTIES uploadHp{ D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC   stageDesc = CD3DX12_RESOURCE_DESC::Buffer(desc.size);
        // CREATE_NOT_ZEROED: D3D12 zero-fills a committed resource before handing it over, which is pure
        // waste for staging: the memcpy below overwrites every byte the GPU reads. (Row padding in the
        // texture cases is never read: CopyTextureRegion consumes only rowSizeInBytes of each row.)
        ThrowIfFailed(m_device->CreateCommittedResource(
            &uploadHp, D3D12_HEAP_FLAG_CREATE_NOT_ZEROED, &stageDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging)));
        GpuLedger::TrackCommitted(m_device.Get(), staging.Get(), Diag::Prof::GpuMemCat::Upload,
                                  "CreateBuffer.Staging");
        Diag::Prof::NoteUploadBytes(desc.size);
        void* mapped = nullptr; D3D12_RANGE rr{ 0, 0 };
        ThrowIfFailed(staging->Map(0, &rr, &mapped));
        std::memcpy(mapped, initialData, static_cast<size_t>(desc.size));
        staging->Unmap(0, nullptr);

        // After copy, transition to an appropriate shader-readable state.
        const bool isSRV = RHI::HasFlag(desc.bindFlags, RHI::BindFlag::SHADER_RESOURCE);
        const D3D12_RESOURCE_STATES finalState = isSRV
            ? (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
            : D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;

        if (m_uploadCopyQueue)
        {
            // On the COPY queue, without a barrier: the buffer was created in COMMON, the copy promotes it to
            // COPY_DEST, and it decays back to COMMON when the copy's ExecuteCommandLists completes. A buffer
            // decays at the end of every ExecuteCommandLists anyway and is promoted to whatever its next use
            // needs, so the ledger's finalState is a correct BeforeState for any later barrier.
            ctx.copyList->CopyBufferRegion(entry.resource.Get(), 0, staging.Get(), 0, desc.size);
            ctx.copyDirty = true;
        }
        else
        {
            auto b1 = CD3DX12_RESOURCE_BARRIER::Transition(
                entry.resource.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            ctx.list->ResourceBarrier(1, &b1);
            ctx.list->CopyBufferRegion(entry.resource.Get(), 0, staging.Get(), 0, desc.size);
            auto b2 = CD3DX12_RESOURCE_BARRIER::Transition(
                entry.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, finalState);
            ctx.list->ResourceBarrier(1, &b2);
            ctx.directDirty = true;
        }
        entry.state = finalState;

        // Inside a batch scope: defer the fence wait until EndBufferUploadBatch(),
        // but cap how much staging piles up so the batch doesn't OOM on
        // scenes with thousands of small meshes.
        if (ctx.batchDepth > 0)
        {
            ctx.stagingBytes += desc.size;
            ctx.stagingKeepAlive.push_back(std::move(staging));
            FlushBatchStagingIfNeeded();
        }
        else
        {
            FlushUploadAndWait();
        }
    }

    if (RHI::HasFlag(desc.bindFlags, RHI::BindFlag::VERTEX_BUFFER))
    {
        entry.vbv.BufferLocation = entry.resource->GetGPUVirtualAddress();
        entry.vbv.SizeInBytes    = static_cast<UINT>(desc.size);
        entry.vbv.StrideInBytes  = desc.stride;
    }
    if (RHI::HasFlag(desc.bindFlags, RHI::BindFlag::INDEX_BUFFER))
    {
        entry.ibv.BufferLocation = entry.resource->GetGPUVirtualAddress();
        entry.ibv.SizeInBytes    = static_cast<UINT>(desc.size);
        entry.ibv.Format         = (desc.format == RHI::Format::R16_UINT)
                                   ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
    }
    if (isCBV)
    {
        // A single CBV descriptor is capped at 64 KB (D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16).
        // Larger buffers (e.g. the particle emitter ring: kMaxEmittersPerFrame slots) are bound through a
        // root CBV at a per-slot offset (SetComputeRootCBV), never a whole-buffer CBV, so skip the
        // descriptor: an illegal one trips DEVICE_REMOVED (DXGI_ERROR_INVALID_CALL) at creation. The
        // resource is still a valid constant buffer (UPLOAD -> GENERIC_READ).
        constexpr UINT64 kMaxCbvBytes = 65536;
        if (allocSize > kMaxCbvBytes)
        {
            LOG_WARNING("CreateBuffer: constant buffer %llu B exceeds the 64 KB single-CBV limit; "
                        "skipping whole-buffer CBV (bind via root CBV + per-slot offset)",
                        (unsigned long long)allocSize);
        }
        else
        {
            entry.cbv = m_cbvSrvUavAllocator.Allocate(1);
            // A failed descriptor allocation must not reach D3D12: on exhaustion AllocateStatic returns an
            // allocation whose CPU handle is {ptr = 0}, and Create*View then writes to address 0 inside the
            // driver (an access violation in nvwgf2umx.dll with nothing in the log). Fail the buffer
            // instead: callers handle a false return (e.g. SkinnedMeshSubsystem::RegisterSkinnedMesh skips
            // the mesh), and the allocator has already reported the exhaustion.
            if (!entry.cbv.IsValid())
            {
                LOG_ERROR("CreateBuffer: CBV_SRV_UAV heap exhausted — cannot create the CBV "
                              "for a %llu B buffer. The buffer is NOT created.",
                              (unsigned long long)desc.size);
                ReleaseBufferViews(entry);
                return false;
            }
            D3D12_CONSTANT_BUFFER_VIEW_DESC cbvd{};
            cbvd.BufferLocation = entry.resource->GetGPUVirtualAddress();
            cbvd.SizeInBytes    = static_cast<UINT>(allocSize);
            m_cbvSrvUavAllocator.WriteCbv(&cbvd, entry.cbv.GetCpuHandle());
            entry.cbv.CopyToGpu();
        }
    }
    if (RHI::HasFlag(desc.bindFlags, RHI::BindFlag::SHADER_RESOURCE))
    {
        entry.srv = m_cbvSrvUavAllocator.Allocate(1);
        // Exhausted: fail the buffer, as for the CBV.
        if (!entry.srv.IsValid())
        {
            LOG_ERROR("CreateBuffer: CBV_SRV_UAV heap exhausted — cannot create the SRV "
                      "for a %llu B buffer. The buffer is NOT created.",
                      (unsigned long long)desc.size);
            ReleaseBufferViews(entry);
            return false;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
        srvd.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
        srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

        if (RHI::HasFlag(desc.miscFlags, RHI::ResourceMiscFlag::BUFFER_RAW))
        {
            // ByteAddressBuffer: raw 32-bit access
            srvd.Format               = DXGI_FORMAT_R32_TYPELESS;
            srvd.Buffer.NumElements   = static_cast<UINT>(desc.size / 4);
            srvd.Buffer.Flags         = D3D12_BUFFER_SRV_FLAG_RAW;
        }
        else if (RHI::HasFlag(desc.miscFlags, RHI::ResourceMiscFlag::BUFFER_TYPED))
        {
            // Buffer<T>: the format's elements, desc.stride bytes each, no
            // structure stride (a typed view with one is rejected).
            srvd.Format                     = ToDxgiFormat(desc.format);
            srvd.Buffer.NumElements         = static_cast<UINT>(desc.size / (desc.stride ? desc.stride : 4u));
            srvd.Buffer.StructureByteStride = 0;
        }
        else
        {
            // StructuredBuffer or typed buffer
            srvd.Format                     = ToDxgiFormat(desc.format);
            srvd.Buffer.NumElements         = (desc.stride > 0)
                                              ? static_cast<UINT>(desc.size / desc.stride)
                                              : static_cast<UINT>(desc.size / 4);
            srvd.Buffer.StructureByteStride = desc.stride;
        }
        m_cbvSrvUavAllocator.WriteSrv(entry.resource.Get(), &srvd, entry.srv.GetCpuHandle());
        entry.srv.CopyToGpu();
    }

    if (RHI::HasFlag(desc.bindFlags, RHI::BindFlag::UNORDERED_ACCESS))
    {
        entry.uav = m_cbvSrvUavAllocator.Allocate(1);
        // Exhausted: fail the buffer, as for the CBV.
        if (!entry.uav.IsValid())
        {
            LOG_ERROR("CreateBuffer: CBV_SRV_UAV heap exhausted — cannot create the UAV "
                      "for a %llu B buffer. The buffer is NOT created.",
                      (unsigned long long)desc.size);
            ReleaseBufferViews(entry);
            return false;
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavd{};
        uavd.ViewDimension              = D3D12_UAV_DIMENSION_BUFFER;

        if (RHI::HasFlag(desc.miscFlags, RHI::ResourceMiscFlag::BUFFER_RAW))
        {
            // RWByteAddressBuffer: raw 32-bit access. Mirror the SRV path (R32_TYPELESS + RAW flag), or
            // D3D12 rejects the view as a typed view of format UNKNOWN and the device is removed.
            uavd.Format                     = DXGI_FORMAT_R32_TYPELESS;
            uavd.Buffer.NumElements         = static_cast<UINT>(desc.size / 4);
            uavd.Buffer.StructureByteStride = 0;
            uavd.Buffer.Flags               = D3D12_BUFFER_UAV_FLAG_RAW;
        }
        else if (RHI::HasFlag(desc.miscFlags, RHI::ResourceMiscFlag::BUFFER_TYPED))
        {
            // RWBuffer<T>: as the SRV above, with the typed format.
            uavd.Format                     = ToDxgiFormat(desc.format);
            uavd.Buffer.NumElements         = static_cast<UINT>(desc.size / (desc.stride ? desc.stride : 4u));
            uavd.Buffer.StructureByteStride = 0;
        }
        else
        {
            // RWStructuredBuffer or typed buffer.
            uavd.Format                     = DXGI_FORMAT_UNKNOWN;
            uavd.Buffer.NumElements         = (desc.stride > 0)
                                              ? static_cast<UINT>(desc.size / desc.stride)
                                              : static_cast<UINT>(desc.size / 4);
            uavd.Buffer.StructureByteStride = desc.stride;
        }

        m_cbvSrvUavAllocator.WriteUav(entry.resource.Get(), nullptr, &uavd, entry.uav.GetCpuHandle());
        entry.uav.CopyToGpu();

        // A structured view cannot be cleared: an opted-in buffer gets a raw one beside it for
        // ClearUavUint.
        if (RHI::HasFlag(desc.miscFlags, RHI::ResourceMiscFlag::UAV_CLEARABLE) &&
            uavd.Format == DXGI_FORMAT_UNKNOWN)
        {
            entry.clearUav = m_cbvSrvUavAllocator.Allocate(1);
            if (!entry.clearUav.IsValid())
            {
                LOG_ERROR("CreateBuffer: CBV_SRV_UAV heap exhausted - cannot create the clear view "
                          "for a %llu B buffer. The buffer is NOT created.",
                          (unsigned long long)desc.size);
                ReleaseBufferViews(entry);
                return false;
            }
            D3D12_UNORDERED_ACCESS_VIEW_DESC raw{};
            raw.ViewDimension      = D3D12_UAV_DIMENSION_BUFFER;
            raw.Format             = DXGI_FORMAT_R32_TYPELESS;
            raw.Buffer.NumElements = static_cast<UINT>(desc.size / 4);
            raw.Buffer.Flags       = D3D12_BUFFER_UAV_FLAG_RAW;
            m_cbvSrvUavAllocator.WriteUav(entry.resource.Get(), nullptr, &raw, entry.clearUav.GetCpuHandle());
            entry.clearUav.CopyToGpu();
        }
    }

    std::lock_guard<std::mutex> lock(m_resourceMutex);
    if (!m_bufferFreeList.empty())
    {
        outBuffer.handleId         = m_bufferFreeList.back();
        m_bufferFreeList.pop_back();
        m_bufferPool[outBuffer.handleId] = std::move(entry);
    }
    else
    {
        // The reserve is a HARD cap, not a hint: recording workers index this
        // vector without m_resourceMutex, so a reallocating push_back would
        // dangle every reference they hold. Refuse loudly instead.
        if (!ENGINE_CHECK_ONCE(m_bufferPool.size() < m_bufferPool.capacity(),
                "CreateBuffer: buffer pool full (%zu) — growing it would reallocate under "
                "lock-free readers; a %llu-byte buffer was NOT created. Raise GraphicsDX12::kMaxBufferPool.",
                m_bufferPool.capacity(), (unsigned long long)desc.size))
        {
            ReleaseBufferViews(entry);
            return false;
        }
        outBuffer.handleId = static_cast<uint32_t>(m_bufferPool.size());
        m_bufferPool.push_back(std::move(entry));
    }
    outBuffer.type = RHI::GPUResource::Type::Buffer;
    outBuffer.desc = desc;
    LOG_TRACE("CreateBuffer: %llu bytes", (unsigned long long)desc.size);
    return true;
}

// ==== Resource creation - CreateTexture ====

bool GraphicsDX12::CreateTexture(const RHI::TextureDesc& desc,
                                 RHI::Texture& outTexture,
                                 const RHI::SubresourceData* initialData)
{
    return CreateTextureImpl(desc, outTexture, initialData, nullptr, 0);
}

bool GraphicsDX12::CreateTextureInArena(const RHI::TextureDesc& desc, const TransientAliasArena& arena,
                                        uint64_t offset, RHI::Texture& outTexture)
{
    ID3D12Heap* heap = arena.Heap();
    return heap && CreateTextureImpl(desc, outTexture, nullptr, heap, offset);
}

void GraphicsDX12::DiscardTexture(const RHI::Texture& texture, RHI::CommandList cmd)
{
    if (!texture.IsValid() || texture.handleId >= m_texturePool.size()) return;
    const TextureDX12& t = m_texturePool[texture.handleId];
    if (t.resource) GetPoolEntry(cmd).GetCommandList()->DiscardResource(t.resource.Get(), nullptr);
}

bool GraphicsDX12::CreateTextureImpl(const RHI::TextureDesc& desc, RHI::Texture& outTexture,
                                     const RHI::SubresourceData* initialData, ID3D12Heap* placeHeap,
                                     uint64_t placeOffset)
{
    PROF_SCOPE_D("Gfx.CreateTexture");
    UploadStats::textureCreates.fetch_add(1, std::memory_order_relaxed);
    if (Sys::ThreadCtx::Current() != Sys::ThreadCtx::Render && !TaskSystem::OnWorkerThread())
        UploadStats::textureCreatesGame.fetch_add(1, std::memory_order_relaxed);
    if (std::this_thread::get_id() == m_deviceFrameThread.load(std::memory_order_acquire))
        DrainFrameIfNeeded(Diag::DrainCause::ResourceOp);
    // Hoisted: Upload() picks the caller's context (loader scope or execution lane) on every call.
    UploadContext& ctx = Upload();
    TextureDX12 entry;
    DXGI_FORMAT dxgiFmt = ToDxgiFormat(desc.format);
    D3D12_HEAP_PROPERTIES hp{ ToD3D12HeapType(desc.usage) };

    // Depth texture that also needs to be sampled as SRV requires a typeless resource format.
    const bool isDepthFmt = (dxgiFmt == DXGI_FORMAT_D32_FLOAT
                          || dxgiFmt == DXGI_FORMAT_D24_UNORM_S8_UINT
                          || dxgiFmt == DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
    const bool needsDepthSRV = isDepthFmt
        && RHI::HasFlag(desc.bindFlags, RHI::BindFlag::DEPTH_STENCIL)
        && RHI::HasFlag(desc.bindFlags, RHI::BindFlag::SHADER_RESOURCE);

    D3D12_RESOURCE_DESC rd{};
    switch (desc.type)
    {
    case RHI::TextureDesc::Type::TEXTURE_1D: rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D; break;
    case RHI::TextureDesc::Type::TEXTURE_3D: rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D; break;
    default:                                 rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; break;
    }
    rd.Width              = desc.width;
    rd.Height             = desc.height;
    rd.DepthOrArraySize   = static_cast<UINT16>((rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) ? desc.depth : desc.arraySize);
    rd.MipLevels          = static_cast<UINT16>(desc.mipLevels);
    // Typeless resource format for depth textures that need SRV access.
    DXGI_FORMAT resourceFmt = dxgiFmt;
    if (needsDepthSRV)
    {
        if (dxgiFmt == DXGI_FORMAT_D32_FLOAT)          resourceFmt = DXGI_FORMAT_R32_TYPELESS;
        else if (dxgiFmt == DXGI_FORMAT_D24_UNORM_S8_UINT) resourceFmt = DXGI_FORMAT_R24G8_TYPELESS;
        else if (dxgiFmt == DXGI_FORMAT_D32_FLOAT_S8X24_UINT) resourceFmt = DXGI_FORMAT_R32G8X24_TYPELESS;
    }
    rd.Format             = resourceFmt;
    rd.SampleDesc.Count   = desc.sampleCount;
    rd.SampleDesc.Quality = 0;
    rd.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags              = ToD3D12ResourceFlags(desc.bindFlags);

    // DPB-only NV12: REFERENCE_ONLY lets the driver use an opaque tiled layout (saves memory) but
    // forbids SRV / RTV / UAV, so the texture is decoder-internal: back-reference frames the renderer
    // never samples.
    const bool isNV12 = (dxgiFmt == DXGI_FORMAT_NV12 || dxgiFmt == DXGI_FORMAT_P010);
    if (RHI::HasFlag(desc.miscFlags, RHI::ResourceMiscFlag::VIDEO_DECODE_DPB_ONLY))
    {
        rd.Flags |= D3D12_RESOURCE_FLAG_VIDEO_DECODE_REFERENCE_ONLY
                  | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    }

    D3D12_RESOURCE_STATES initState = ToD3D12ResourceState(desc.layout);
    D3D12_CLEAR_VALUE cv{};
    D3D12_CLEAR_VALUE* pCv = nullptr;
    if (RHI::HasFlag(desc.bindFlags, RHI::BindFlag::RENDER_TARGET))
    {
        cv.Format = dxgiFmt;
        std::memcpy(cv.Color, desc.clear.color, sizeof(cv.Color));
        pCv = &cv; initState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    else if (RHI::HasFlag(desc.bindFlags, RHI::BindFlag::DEPTH_STENCIL))
    {
        cv.Format               = dxgiFmt; // use the original depth format (D32_FLOAT or D24_S8)
        cv.DepthStencil.Depth   = desc.clear.depthStencil.depth;
        cv.DepthStencil.Stencil = static_cast<UINT8>(desc.clear.depthStencil.stencil);
        pCv = &cv; initState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }

    // Skip the driver's zero-fill for render targets: they are cleared or fully written before the
    // first read.
    // Not for depth-stencil: a D24_S8 / D32_S8 target's stencil plane (subresource 1) is often never
    // cleared explicitly, and the debug layer reports it as used uninitialized. Not for SRV / UAV-only
    // textures, which may be sampled before their first write (history / accumulation buffers). Not
    // for RT + UAV textures: they are usually written first by a compute UAV pass and bound as an RT
    // later (e.g. ToneMapPass.FinalOutput, which UIPass then draws the HUD onto), and a UAV write does
    // not satisfy CREATE_NOT_ZEROED's rule that an RT / DS subresource be initialized by Clear /
    // Discard / Copy. The exclusion costs only a one-time zero-fill at allocation.
    D3D12_HEAP_FLAGS heapFlags = D3D12_HEAP_FLAG_NONE;
    if (hp.Type == D3D12_HEAP_TYPE_DEFAULT
        && RHI::HasFlag(desc.bindFlags, RHI::BindFlag::RENDER_TARGET)
        && !RHI::HasFlag(desc.bindFlags, RHI::BindFlag::DEPTH_STENCIL)
        && !RHI::HasFlag(desc.bindFlags, RHI::BindFlag::UNORDERED_ACCESS))
        heapFlags = D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;

    // The initial data goes on the COPY queue when the intended state is one a texture without
    // SIMULTANEOUS_ACCESS can be promoted to from COMMON (a shader-resource or copy state) and nothing
    // ever writes it on the GPU (no render target, depth or UAV binding). It is created in COMMON, the
    // copy promotes it to COPY_DEST and it decays back to COMMON when the copy completes, while the
    // ledger keeps initState: for a barrier on a resource in the common state either COMMON or any
    // promotable state is a correct BeforeState, and a read through a descriptor promotes it.
    // UAVs are excluded: a texture read through a descriptor (promoted) and then transitioned to UAV
    // from the ledger's state in a later list of the same ExecuteCommandLists (no decay between) is
    // outside that rule. The few textures both uploaded and GPU-written (the DDGI atlas, the water
    // ripple field, the RT resolves) are created once, so the graphics list costs them nothing.
    constexpr D3D12_RESOURCE_STATES kPromotableFromCommon =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_COPY_SOURCE | D3D12_RESOURCE_STATE_COPY_DEST;
    const bool copyQueueUpload = m_uploadCopyQueue && initialData && desc.usage == RHI::Usage::DEFAULT &&
                                 !pCv && rd.SampleDesc.Count <= 1 &&
                                 !RHI::HasFlag(desc.bindFlags, RHI::BindFlag::UNORDERED_ACCESS) &&
                                 (initState & ~kPromotableFromCommon) == 0;
    if (placeHeap)
    {
        // CreateTextureInArena: a serial phase (TransientAliasArena.h rule 3), no initial data, and the
        // placement inside the heap's usable size (CreateTexturePlaced's check: past it is unmapped memory).
        ENGINE_CHECK_ONCE(OwnsDeviceFrame() || !m_inFrame,
            "CreateTextureInArena(%s) off the device-frame thread - placed resources are made in a serial "
            "phase only", desc.debugName ? desc.debugName : "<unnamed>");
        const D3D12_RESOURCE_ALLOCATION_INFO ai = m_device->GetResourceAllocationInfo(0, 1, &rd);
        uint64_t usable = 0; UINT usableSz = sizeof(usable);
        const bool isArena = SUCCEEDED(placeHeap->GetPrivateData(TransientAliasArena::kUsableSizeTag, &usableSz,
                                                                 &usable)) && usableSz == sizeof(usable);
        const uint64_t limit = isArena ? usable : placeHeap->GetDesc().SizeInBytes;
        if (!ENGINE_CHECK_ONCE(!initialData && placeOffset + ai.SizeInBytes <= limit,
                "CreateTextureInArena('%s'): initial data, or %llu bytes at %llu past the heap's %llu",
                desc.debugName ? desc.debugName : "<unnamed>", (unsigned long long)ai.SizeInBytes,
                (unsigned long long)placeOffset, (unsigned long long)limit))
            return false;
        if (FAILED(m_device->CreatePlacedResource(placeHeap, placeOffset, &rd, initState, pCv,
                                                  IID_PPV_ARGS(&entry.resource))))
        {
            LOG_ERROR("CreateTextureInArena: CreatePlacedResource failed (off=%llu %ux%u '%s')",
                      (unsigned long long)placeOffset, desc.width, desc.height,
                      desc.debugName ? desc.debugName : "<unnamed>");
            return false;
        }
        // The texture keeps its heap alive through its own deferred release (TextureDX12::placedHeap).
        entry.placedHeap = placeHeap;
        GpuLedger::TrackPlaced(entry.resource.Get());
    }
    else
    {
        if (FAILED(m_device->CreateCommittedResource(
                &hp, heapFlags, &rd, copyQueueUpload ? D3D12_RESOURCE_STATE_COMMON : initState, pCv,
                IID_PPV_ARGS(&entry.resource))))
        {
            LOG_ERROR("CreateTexture: CreateCommittedResource failed");
            return false;
        }
        GpuLedger::TrackTexture(m_device.Get(), entry.resource.Get(), desc.debugName);
    }
    entry.state = initState;
    // A depth-stencil format's stencil plane starts in the same state.
    entry.planes       = RHI::HasStencilPlane(desc.format) ? 2 : 1;
    entry.stencilState = initState;

    // Optional debug name -> ID3D12Resource::SetName, for validation messages and PIX / RenderDoc
    // captures.
    if (desc.debugName && desc.debugName[0] != '\0')
    {
        const int needed = MultiByteToWideChar(CP_UTF8, 0, desc.debugName, -1, nullptr, 0);
        if (needed > 0)
        {
            std::wstring wname(static_cast<size_t>(needed - 1), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, desc.debugName, -1, wname.data(), needed);
            entry.resource->SetName(wname.c_str());
        }
    }

    if (RHI::HasFlag(desc.bindFlags, RHI::BindFlag::SHADER_RESOURCE) && isNV12)
    {
        // NV12 (P010 for 10-bit HDR) is planar YUV: plane 0 is luma (Y), plane 1 half-res interleaved
        // chroma (UV). D3D12 allows no typed SRV of the NV12 format itself, so each plane gets its own
        // typed SRV: srv for Y, uvPlaneSrv for UV.
        entry.srv        = m_cbvSrvUavAllocator.Allocate(1); // Y plane
        entry.uvPlaneSrv = m_cbvSrvUavAllocator.Allocate(1); // UV plane
        if (!entry.srv.IsValid() || !entry.uvPlaneSrv.IsValid())
        {
            // A half-allocated planar pair is unusable: fail the texture.
            LOG_ERROR("CreateTexture: CBV_SRV_UAV heap exhausted for NV12 '%s' — texture NOT created",
                      desc.debugName ? desc.debugName : "<unnamed>");
            ReleaseTextureViews(entry);
            return false;
        }

        const DXGI_FORMAT yFmt  = (dxgiFmt == DXGI_FORMAT_P010) ? DXGI_FORMAT_R16_UNORM
                                                                : DXGI_FORMAT_R8_UNORM;
        const DXGI_FORMAT uvFmt = (dxgiFmt == DXGI_FORMAT_P010) ? DXGI_FORMAT_R16G16_UNORM
                                                                : DXGI_FORMAT_R8G8_UNORM;

        D3D12_SHADER_RESOURCE_VIEW_DESC ySrvd{};
        ySrvd.Format                  = yFmt;
        ySrvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        ySrvd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        ySrvd.Texture2D.MipLevels     = 1;
        ySrvd.Texture2D.PlaneSlice    = 0;
        m_cbvSrvUavAllocator.WriteSrv(entry.resource.Get(), &ySrvd, entry.srv.GetCpuHandle());
        entry.srv.CopyToGpu();

        D3D12_SHADER_RESOURCE_VIEW_DESC uvSrvd{};
        uvSrvd.Format                  = uvFmt;
        uvSrvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        uvSrvd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        uvSrvd.Texture2D.MipLevels     = 1;
        uvSrvd.Texture2D.PlaneSlice    = 1;
        m_cbvSrvUavAllocator.WriteSrv(entry.resource.Get(), &uvSrvd, entry.uvPlaneSrv.GetCpuHandle());
        entry.uvPlaneSrv.CopyToGpu();
    }
    else if (RHI::HasFlag(desc.bindFlags, RHI::BindFlag::SHADER_RESOURCE))
    {
        // Every Allocate(1) here is checked: on exhaustion the allocator returns a default
        // DescriptorAllocation whose CPU handle is {ptr = 0}, and Create*View on it writes through
        // address 0 in the driver with no debug-layer message. Views the desc requested fail the texture;
        // derived views (stencil / preview) are skipped, because their accessors already degrade.
        entry.srv = m_cbvSrvUavAllocator.Allocate(1);
        if (!entry.srv.IsValid())
        {
            LOG_ERROR("CreateTexture: CBV_SRV_UAV heap exhausted (SRV) for '%s' — texture NOT created",
                      desc.debugName ? desc.debugName : "<unnamed>");
            ReleaseTextureViews(entry);
            return false;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
        // SRV format for depth textures: R32_FLOAT for D32, R24_UNORM_X8 for D24_S8, R32_FLOAT_X8X24 for
        // D32_S8X24 (shaders read .r as float in every case, so Texture2D<float> fits all of them).
        DXGI_FORMAT srvFmt = dxgiFmt;
        if (needsDepthSRV)
        {
            if (dxgiFmt == DXGI_FORMAT_D32_FLOAT)              srvFmt = DXGI_FORMAT_R32_FLOAT;
            else if (dxgiFmt == DXGI_FORMAT_D24_UNORM_S8_UINT) srvFmt = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            else if (dxgiFmt == DXGI_FORMAT_D32_FLOAT_S8X24_UINT) srvFmt = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        }
        srvd.Format                  = srvFmt;
        srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        switch (rd.Dimension)
        {
        case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
            if (desc.arraySize > 1) {
                srvd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
                srvd.Texture1DArray.MipLevels = desc.mipLevels;
                srvd.Texture1DArray.ArraySize = desc.arraySize;
            } else {
                srvd.ViewDimension       = D3D12_SRV_DIMENSION_TEXTURE1D;
                srvd.Texture1D.MipLevels = desc.mipLevels;
            }
            break;
        case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
            srvd.ViewDimension       = D3D12_SRV_DIMENSION_TEXTURE3D;
            srvd.Texture3D.MipLevels = desc.mipLevels;
            break;
        default: // 2D
            if (desc.sampleCount > 1) {
                srvd.ViewDimension = (desc.arraySize > 1) ? D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY
                                                           : D3D12_SRV_DIMENSION_TEXTURE2DMS;
            } else if (RHI::HasFlag(desc.miscFlags, RHI::ResourceMiscFlag::TEXTURECUBE)
                       && desc.arraySize == 6) {
                srvd.ViewDimension              = D3D12_SRV_DIMENSION_TEXTURECUBE;
                srvd.TextureCube.MipLevels      = desc.mipLevels;
                srvd.TextureCube.MostDetailedMip = 0;
                LOG_TRACE("CreateTexture: created TextureCube SRV (%ux%u mips=%u)", desc.width, desc.height, desc.mipLevels);
            } else if (RHI::HasFlag(desc.miscFlags, RHI::ResourceMiscFlag::TEXTURECUBE)
                       && desc.arraySize > 6 && (desc.arraySize % 6) == 0) {
                srvd.ViewDimension                          = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
                srvd.TextureCubeArray.MipLevels             = desc.mipLevels;
                srvd.TextureCubeArray.MostDetailedMip       = 0;
                srvd.TextureCubeArray.First2DArrayFace      = 0;
                srvd.TextureCubeArray.NumCubes              = desc.arraySize / 6;
                srvd.TextureCubeArray.ResourceMinLODClamp   = 0.0f;
                LOG_TRACE("CreateTexture: created TextureCubeArray SRV (%ux%u mips=%u cubes=%u)",
                         desc.width, desc.height, desc.mipLevels, desc.arraySize / 6);
            } else if (desc.arraySize > 1) {
                srvd.ViewDimension              = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                srvd.Texture2DArray.MipLevels   = desc.mipLevels;
                srvd.Texture2DArray.ArraySize   = desc.arraySize;
            } else {
                srvd.ViewDimension       = D3D12_SRV_DIMENSION_TEXTURE2D;
                srvd.Texture2D.MipLevels = desc.mipLevels;
            }
            break;
        }
        m_cbvSrvUavAllocator.WriteSrv(entry.resource.Get(), &srvd, entry.srv.GetCpuHandle());
        entry.srv.CopyToGpu();

        // Stencil-plane SRV for stencil-bearing depth formats: a second view of the same resource's
        // stencil plane, so compute shaders (e.g. TAA) can read stencil alongside depth. Freed with the
        // texture.
        const bool hasStencilPlane = (dxgiFmt == DXGI_FORMAT_D24_UNORM_S8_UINT
                                   || dxgiFmt == DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
        if (needsDepthSRV && hasStencilPlane
            && rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D
            && desc.sampleCount <= 1
            && desc.arraySize <= 1)
        {
            entry.stencilSrv = m_cbvSrvUavAllocator.Allocate(1);
            // DERIVED view the caller never asked for through bindFlags: on
            // exhaustion leave it invalid (GetTextureStencilSRV already
            // returns 0 for that) rather than failing the depth buffer itself.
            if (!entry.stencilSrv.IsValid())
            {
                LOG_ERROR("CreateTexture: CBV_SRV_UAV heap exhausted (stencil SRV) for '%s' — "
                          "stencil view skipped", desc.debugName ? desc.debugName : "<unnamed>");
            }
            else
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC stencilSrvd{};
                stencilSrvd.Format = (dxgiFmt == DXGI_FORMAT_D24_UNORM_S8_UINT)
                                   ? DXGI_FORMAT_X24_TYPELESS_G8_UINT
                                   : DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
                // Both are multi-plane resources: plane 0 = depth, plane 1 = stencil (8 bits), so the stencil
                // view needs PlaneSlice = 1 (the default 0 is an InvalidCall -> device removal at view creation).
                // The shader samples Texture2D<uint2> and reads .y (G in both layouts), so the component mapping
                // stays the identity default.
                stencilSrvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                stencilSrvd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
                stencilSrvd.Texture2D.MipLevels     = desc.mipLevels;
                stencilSrvd.Texture2D.PlaneSlice    = 1;
                m_cbvSrvUavAllocator.WriteSrv(entry.resource.Get(), &stencilSrvd,
                                              entry.stencilSrv.GetCpuHandle());
                entry.stencilSrv.CopyToGpu();
            }
        }

        // SRGB textures get a UNORM alias SRV for the editor preview: ImGui renders to a UNORM RTV, so
        // sampling an SRGB SRV would linearize without re-encoding on write and the preview would look
        // too dark. The alias returns the raw sRGB texels.
        auto stripSrgb = [](DXGI_FORMAT f) -> DXGI_FORMAT {
            switch (f) {
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case DXGI_FORMAT_BC1_UNORM_SRGB:      return DXGI_FORMAT_BC1_UNORM;
            case DXGI_FORMAT_BC2_UNORM_SRGB:      return DXGI_FORMAT_BC2_UNORM;
            case DXGI_FORMAT_BC3_UNORM_SRGB:      return DXGI_FORMAT_BC3_UNORM;
            case DXGI_FORMAT_BC7_UNORM_SRGB:      return DXGI_FORMAT_BC7_UNORM;
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
            case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM;
            default: return f;
            }
        };
        DXGI_FORMAT unormFmt = stripSrgb(dxgiFmt);
        if (unormFmt != dxgiFmt)
        {
            entry.previewSrv = m_cbvSrvUavAllocator.Allocate(1);
            // Derived, editor-only: GetTexturePreviewSRV falls back to `srv`
            // when this is invalid, so exhaustion costs a dark thumbnail, not
            // the texture.
            if (!entry.previewSrv.IsValid())
            {
                LOG_ERROR("CreateTexture: CBV_SRV_UAV heap exhausted (preview SRV) for '%s' — "
                          "preview alias skipped", desc.debugName ? desc.debugName : "<unnamed>");
            }
            else
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC previewSrvd = srvd;
                previewSrvd.Format = unormFmt;
                m_cbvSrvUavAllocator.WriteSrv(entry.resource.Get(), &previewSrvd, entry.previewSrv.GetCpuHandle());
                entry.previewSrv.CopyToGpu();
            }
        }
    }
    if (RHI::HasFlag(desc.bindFlags, RHI::BindFlag::RENDER_TARGET))
    {
        // Tagged with the texture's own name: the RTV / DSV high-water is a per-texture question (which
        // render targets did churn create that the free list could not serve?), and debugName is the
        // only string here that answers it.
        entry.rtv = m_rtvAllocator.Allocate(1, desc.debugName);
        if (!entry.rtv.IsValid())
        {
            LOG_ERROR("CreateTexture: RTV heap exhausted for '%s' — texture NOT created",
                      desc.debugName ? desc.debugName : "<unnamed>");
            ReleaseTextureViews(entry);
            return false;
        }
        D3D12_RENDER_TARGET_VIEW_DESC rtvd{ dxgiFmt, D3D12_RTV_DIMENSION_TEXTURE2D };
        m_device->CreateRenderTargetView(entry.resource.Get(), &rtvd, entry.rtv.GetCpuHandle());
    }
    if (RHI::HasFlag(desc.bindFlags, RHI::BindFlag::DEPTH_STENCIL))
    {
        entry.dsv = m_dsvAllocator.Allocate(1, desc.debugName);   // tagged with the texture's name, like the RTV
        if (!entry.dsv.IsValid())
        {
            LOG_ERROR("CreateTexture: DSV heap exhausted for '%s' — texture NOT created",
                      desc.debugName ? desc.debugName : "<unnamed>");
            ReleaseTextureViews(entry);
            return false;
        }
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvd{};
        dsvd.Format = dxgiFmt;
        if (desc.arraySize > 1)
        {
            dsvd.ViewDimension                   = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            dsvd.Texture2DArray.MipSlice         = 0;
            dsvd.Texture2DArray.FirstArraySlice  = 0;
            dsvd.Texture2DArray.ArraySize        = desc.arraySize;
        }
        else
        {
            dsvd.ViewDimension                   = D3D12_DSV_DIMENSION_TEXTURE2D;
        }
        m_device->CreateDepthStencilView(entry.resource.Get(), &dsvd, entry.dsv.GetCpuHandle());

        // The read-only variants a pass binds while a plane is in a DEPTH_READ state (RHI::DepthView);
        // 2D, non-array textures only.
        if (desc.arraySize <= 1)
        {
            const bool stencil = RHI::HasStencilPlane(desc.format);
            const D3D12_DSV_FLAGS kFlags[3] = {
                D3D12_DSV_FLAG_READ_ONLY_DEPTH,
                D3D12_DSV_FLAG_READ_ONLY_STENCIL,
                D3D12_DSV_FLAG_READ_ONLY_DEPTH | D3D12_DSV_FLAG_READ_ONLY_STENCIL };
            for (int v = 0; v < (stencil ? 3 : 1); ++v)
            {
                entry.dsvReadOnly[v] = m_dsvAllocator.Allocate(1, desc.debugName);
                if (!entry.dsvReadOnly[v].IsValid())
                {
                    LOG_ERROR("CreateTexture: DSV heap exhausted at the read-only DSVs of '%s' - texture NOT created",
                              desc.debugName ? desc.debugName : "<unnamed>");
                    ReleaseTextureViews(entry);
                    return false;
                }
                D3D12_DEPTH_STENCIL_VIEW_DESC ro = dsvd;
                ro.Flags = kFlags[v];
                m_device->CreateDepthStencilView(entry.resource.Get(), &ro, entry.dsvReadOnly[v].GetCpuHandle());
            }
        }

        // Per-slice DSVs for array depth textures (CSM cascades, etc.)
        if (desc.arraySize > 1)
        {
            entry.sliceDsvs.resize(desc.arraySize);
            D3D12_DEPTH_STENCIL_VIEW_DESC sliceDsvd{};
            sliceDsvd.Format                             = dxgiFmt;
            sliceDsvd.ViewDimension                      = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            sliceDsvd.Texture2DArray.MipSlice            = 0;
            sliceDsvd.Texture2DArray.ArraySize           = 1;
            for (uint32_t s = 0; s < desc.arraySize; ++s)
            {
                entry.sliceDsvs[s] = m_dsvAllocator.Allocate(1,
                                                             desc.debugName);
                if (!entry.sliceDsvs[s].IsValid())
                {
                    LOG_ERROR("CreateTexture: DSV heap exhausted at slice %u/%u of '%s' — texture NOT created",
                              s, desc.arraySize, desc.debugName ? desc.debugName : "<unnamed>");
                    ReleaseTextureViews(entry);
                    return false;
                }
                sliceDsvd.Texture2DArray.FirstArraySlice = s;
                m_device->CreateDepthStencilView(entry.resource.Get(), &sliceDsvd,
                                                 entry.sliceDsvs[s].GetCpuHandle());
            }
        }
    }

    if (RHI::HasFlag(desc.bindFlags, RHI::BindFlag::UNORDERED_ACCESS))
    {
        // Whole-resource (mip 0) UAV; covers all array slices when arraySize > 1.
        entry.uav = m_cbvSrvUavAllocator.Allocate(1);
        if (!entry.uav.IsValid())
        {
            LOG_ERROR("CreateTexture: CBV_SRV_UAV heap exhausted (UAV) for '%s' — texture NOT created",
                      desc.debugName ? desc.debugName : "<unnamed>");
            ReleaseTextureViews(entry);
            return false;
        }
        auto fillUav = [&](D3D12_UNORDERED_ACCESS_VIEW_DESC& uavd, uint32_t mip)
        {
            uavd.Format = dxgiFmt;
            if (desc.type == RHI::TextureDesc::Type::TEXTURE_3D)
            {
                uavd.ViewDimension          = D3D12_UAV_DIMENSION_TEXTURE3D;
                uavd.Texture3D.MipSlice     = mip;
                uavd.Texture3D.FirstWSlice  = 0;
                uavd.Texture3D.WSize        = (std::max)(desc.depth >> mip, 1u);
            }
            else if (desc.arraySize > 1)
            {
                // Cube or 2D array: the UAV covers all array slices at this mip.
                uavd.ViewDimension                       = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                uavd.Texture2DArray.MipSlice             = mip;
                uavd.Texture2DArray.FirstArraySlice      = 0;
                uavd.Texture2DArray.ArraySize            = desc.arraySize;
                uavd.Texture2DArray.PlaneSlice           = 0;
            }
            else
            {
                uavd.ViewDimension          = D3D12_UAV_DIMENSION_TEXTURE2D;
                uavd.Texture2D.MipSlice     = mip;
            }
        };
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uavd{};
            fillUav(uavd, 0);
            m_cbvSrvUavAllocator.WriteUav(entry.resource.Get(), nullptr, &uavd, entry.uav.GetCpuHandle());
            entry.uav.CopyToGpu();
        }

        // Per-mip UAVs for multi-mip textures (cubemap prefilter chains, etc.)
        if (desc.mipLevels > 1)
        {
            entry.mipUavs.resize(desc.mipLevels);
            for (uint32_t mip = 0; mip < desc.mipLevels; ++mip)
            {
                entry.mipUavs[mip] = m_cbvSrvUavAllocator.Allocate(1);
                if (!entry.mipUavs[mip].IsValid())
                {
                    LOG_ERROR("CreateTexture: CBV_SRV_UAV heap exhausted at mip UAV %u/%u of '%s' — texture NOT created",
                              mip, desc.mipLevels, desc.debugName ? desc.debugName : "<unnamed>");
                    ReleaseTextureViews(entry);
                    return false;
                }
                D3D12_UNORDERED_ACCESS_VIEW_DESC uavd{};
                fillUav(uavd, mip);
                m_cbvSrvUavAllocator.WriteUav(entry.resource.Get(), nullptr, &uavd,
                                              entry.mipUavs[mip].GetCpuHandle());
                entry.mipUavs[mip].CopyToGpu();
            }
        }
    }

    if (initialData && desc.usage == RHI::Usage::DEFAULT)
    {
        const UINT subresourceCount = desc.mipLevels * desc.arraySize;
        UINT64 uploadSize = 0;
        m_device->GetCopyableFootprints(&rd, 0, subresourceCount, 0, nullptr, nullptr, nullptr, &uploadSize);

        ComPtr<ID3D12Resource> staging;
        D3D12_HEAP_PROPERTIES uploadHp{ D3D12_HEAP_TYPE_UPLOAD };
        const auto uploadBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
        // CREATE_NOT_ZEROED: as for CreateBuffer's staging.
        ThrowIfFailed(m_device->CreateCommittedResource(
            &uploadHp, D3D12_HEAP_FLAG_CREATE_NOT_ZEROED, &uploadBufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging)));
        GpuLedger::TrackCommitted(m_device.Get(), staging.Get(), Diag::Prof::GpuMemCat::Upload,
                                  "CreateTexture.Staging");
        Diag::Prof::NoteUploadBytes(uploadSize);

        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresourceCount);
        std::vector<UINT>   numRows(subresourceCount);
        std::vector<UINT64> rowSizes(subresourceCount);
        m_device->GetCopyableFootprints(&rd, 0, subresourceCount, 0,
            footprints.data(), numRows.data(), rowSizes.data(), nullptr);

        uint8_t* stagingPtr = nullptr;
        D3D12_RANGE rr{ 0, 0 };
        ThrowIfFailed(staging->Map(0, &rr, reinterpret_cast<void**>(&stagingPtr)));
        for (UINT sub = 0; sub < subresourceCount; ++sub)
        {
            const auto& fp  = footprints[sub];
            uint8_t* dstRow = stagingPtr + fp.Offset;
            const uint8_t* srcRow = static_cast<const uint8_t*>(initialData[sub].dataPtr);
            for (UINT row = 0; row < numRows[sub]; ++row)
            {
                std::memcpy(dstRow, srcRow, static_cast<size_t>(rowSizes[sub]));
                dstRow  += fp.Footprint.RowPitch;
                srcRow  += initialData[sub].rowPitch;
            }
        }
        staging->Unmap(0, nullptr);

        if (copyQueueUpload)
        {
            // No barrier on the COPY queue (see copyQueueUpload).
            for (UINT sub = 0; sub < subresourceCount; ++sub)
            {
                CD3DX12_TEXTURE_COPY_LOCATION dst(entry.resource.Get(), sub);
                CD3DX12_TEXTURE_COPY_LOCATION src(staging.Get(), footprints[sub]);
                ctx.copyList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            }
            ctx.copyDirty = true;
        }
        else
        {
            auto toTransfer = CD3DX12_RESOURCE_BARRIER::Transition(
                entry.resource.Get(), entry.state, D3D12_RESOURCE_STATE_COPY_DEST);
            ctx.list->ResourceBarrier(1, &toTransfer);
            for (UINT sub = 0; sub < subresourceCount; ++sub)
            {
                CD3DX12_TEXTURE_COPY_LOCATION dst(entry.resource.Get(), sub);
                CD3DX12_TEXTURE_COPY_LOCATION src(staging.Get(), footprints[sub]);
                ctx.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            }
            auto fromTransfer = CD3DX12_RESOURCE_BARRIER::Transition(
                entry.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, entry.state);
            ctx.list->ResourceBarrier(1, &fromTransfer);
            ctx.directDirty = true;
        }

        // Inside a batch scope: defer the fence wait until EndBufferUploadBatch(), capping accumulated
        // staging so a load burst cannot run out of memory; outside one, flush synchronously. Textures
        // are usually larger than vertex streams, so batching matters even more here.
        if (ctx.batchDepth > 0)
        {
            ctx.stagingBytes += uploadSize;
            ctx.stagingKeepAlive.push_back(std::move(staging));
            FlushBatchStagingIfNeeded();
        }
        else
        {
            FlushUploadAndWait();
        }
    }

    entry.generation = NextTextureGeneration();   // a reused id is a new texture
    std::lock_guard<std::mutex> lock(m_resourceMutex);
    if (!m_textureFreeList.empty())
    {
        outTexture.handleId          = m_textureFreeList.back();
        m_textureFreeList.pop_back();
        m_texturePool[outTexture.handleId] = std::move(entry);
    }
    else
    {
        if (!ENGINE_CHECK_ONCE(m_texturePool.size() < m_texturePool.capacity(),
                "CreateTexture: texture pool full (%zu) — growing it would reallocate under "
                "lock-free readers (TranslateBarrier/BindResource/SetRenderTargets); '%s' NOT "
                "created. Raise GraphicsDX12::kMaxTexturePool.",
                m_texturePool.capacity(), desc.debugName ? desc.debugName : "<unnamed>"))
        {
            ReleaseTextureViews(entry);
            return false;
        }
        outTexture.handleId = static_cast<uint32_t>(m_texturePool.size());
        m_texturePool.push_back(std::move(entry));
    }
    outTexture.type = RHI::GPUResource::Type::Texture;
    outTexture.desc = desc;

    // Copy SRV into bindless texture table at slot [handleId].
    if (m_bindlessTexTable.IsValid() && outTexture.handleId < kMaxBindlessTextures)
    {
        const auto& poolEntry = m_texturePool[outTexture.handleId];
        if (poolEntry.srv.IsValid())
        {
            UINT descInc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            D3D12_CPU_DESCRIPTOR_HANDLE dst = m_bindlessTexTable.GetGpuCpuHandle();
            dst.ptr += static_cast<SIZE_T>(outTexture.handleId) * descInc;
            m_cbvSrvUavAllocator.CopyDescriptors(1, dst, poolEntry.srv.GetCpuHandle());
        }
    }

    LOG_TRACE("CreateTexture: %ux%u fmt=%u", desc.width, desc.height, (unsigned)desc.format);
    return true;
}

// ==== CreateTexturePlaced ====
// A transient texture placed in a caller-owned heap (CreatePlacedResource), so lifetime-disjoint
// textures can alias heap bytes. 2D and 3D, single slice, optional mip chain (with per-mip UAVs),
// SRV (+ optional UAV) only: no RT / DS / cube / array / MSAA / depth / upload.
namespace
{
    // One desc builder for CreateTexturePlaced and GetTextureAllocationInfo, so the allocation-info
    // sizing can never diverge from the actual create. Returns false for shapes the placed path does
    // not support. `resourceFlags` is passed in because ToD3D12ResourceFlags is a private static of
    // GraphicsDX12 (both callers are members and compute it).
    // @p rtDsAndArrays: also render targets, depth and arrays (GetTextureFootprint's sizing).
    bool BuildPlacedTextureDesc(const RHI::TextureDesc& desc, DXGI_FORMAT dxgiFmt,
                                D3D12_RESOURCE_FLAGS resourceFlags,
                                D3D12_RESOURCE_DESC& rd, const char* caller, bool rtDsAndArrays = false)
    {
        const bool rtDsOrArray = RHI::HasFlag(desc.bindFlags, RHI::BindFlag::RENDER_TARGET) ||
                                 RHI::HasFlag(desc.bindFlags, RHI::BindFlag::DEPTH_STENCIL) ||
                                 desc.arraySize > 1;
        if ((rtDsOrArray && !rtDsAndArrays) || desc.sampleCount > 1 ||
            desc.type == RHI::TextureDesc::Type::TEXTURE_1D)
        {
            LOG_ERROR("%s: unsupported shape for placed texture "
                      "(RT/DS/array/MSAA/1D are committed-only)", caller);
            return false;
        }
        const bool is3D = (desc.type == RHI::TextureDesc::Type::TEXTURE_3D);
        rd = {};
        rd.Dimension        = is3D ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                   : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width            = desc.width;
        rd.Height           = desc.height;
        rd.DepthOrArraySize = static_cast<UINT16>(is3D ? desc.depth : (desc.arraySize > 1 ? desc.arraySize : 1u));
        rd.MipLevels        = static_cast<UINT16>(desc.mipLevels > 0 ? desc.mipLevels : 1u);
        rd.Format           = dxgiFmt;
        rd.SampleDesc.Count = 1;
        rd.SampleDesc.Quality = 0;
        rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags            = resourceFlags;
        return true;
    }
} // namespace

bool GraphicsDX12::EnsureAliasArena(TransientAliasArena& arena, uint64_t bytes)
{
    return arena.Ensure(m_device.Get(), bytes) != nullptr;
}

bool GraphicsDX12::CreateTexturePlaced(const RHI::TextureDesc& desc, const TransientAliasArena& arena,
                                       uint64_t offset, RHI::Texture& outTexture)
{
    ID3D12Heap* heap = arena.Heap();
    return heap && CreateTexturePlaced(desc, heap, offset, outTexture);
}

// UAV clears. The view's CPU handle is the staging (non-shader-visible) copy every UAV is written
// to before it is copied into the shader-visible heap, as ClearUnorderedAccessView* requires.
// D3D12 clears only a raw or typed view: a structured buffer clears through the raw view
// ResourceMiscFlag::UAV_CLEARABLE gave it.
void GraphicsDX12::ClearUavUint(const RHI::GPUBuffer& buffer, const uint32_t values[4], RHI::CommandList cmd)
{
    if (!buffer.IsValid() || buffer.handleId >= m_bufferPool.size()) return;
    const GPUBufferDX12& b = m_bufferPool[buffer.handleId];
    const DescriptorAllocation* view = b.clearUav.IsValid() ? &b.clearUav : nullptr;
    if (!view)
    {
        const bool rawOrTyped = RHI::HasFlag(buffer.desc.miscFlags, RHI::ResourceMiscFlag::BUFFER_RAW) ||
                                RHI::HasFlag(buffer.desc.miscFlags, RHI::ResourceMiscFlag::BUFFER_TYPED);
        if (!ENGINE_CHECK_ONCE(b.resource && b.uav.IsValid() && rawOrTyped,
                               "ClearUavUint: buffer %u has no UAV, or a structured one - D3D12 clears only a raw "
                               "or typed view; create it with ResourceMiscFlag::UAV_CLEARABLE", buffer.handleId))
            return;
        view = &b.uav;
    }
    GetPoolEntry(cmd).GetCommandList()->ClearUnorderedAccessViewUint(
        view->GetGpuHandle(), view->GetCpuHandle(), b.resource.Get(), values, 0, nullptr);
}

void GraphicsDX12::ClearUavUint(const RHI::Texture& texture, const uint32_t values[4], RHI::CommandList cmd)
{
    if (!texture.IsValid() || texture.handleId >= m_texturePool.size()) return;
    const TextureDX12& t = m_texturePool[texture.handleId];
    if (!ENGINE_CHECK_ONCE(t.resource && t.uav.IsValid(), "ClearUavUint: texture %u has no UAV", texture.handleId))
        return;
    GetPoolEntry(cmd).GetCommandList()->ClearUnorderedAccessViewUint(
        t.uav.GetGpuHandle(), t.uav.GetCpuHandle(), t.resource.Get(), values, 0, nullptr);
}

void GraphicsDX12::ClearUavFloat(const RHI::Texture& texture, const float values[4], RHI::CommandList cmd)
{
    if (!texture.IsValid() || texture.handleId >= m_texturePool.size()) return;
    const TextureDX12& t = m_texturePool[texture.handleId];
    if (!ENGINE_CHECK_ONCE(t.resource && t.uav.IsValid(), "ClearUavFloat: texture %u has no UAV", texture.handleId))
        return;
    GetPoolEntry(cmd).GetCommandList()->ClearUnorderedAccessViewFloat(
        t.uav.GetGpuHandle(), t.uav.GetCpuHandle(), t.resource.Get(), values, 0, nullptr);
}

bool GraphicsDX12::GetTextureAllocationInfo(const RHI::TextureDesc& desc,
                                            uint64_t& outSize, uint64_t& outAlign)
{
    outSize = 0;
    outAlign = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    D3D12_RESOURCE_DESC rd{};
    if (!BuildPlacedTextureDesc(desc, ToDxgiFormat(desc.format),
                                ToD3D12ResourceFlags(desc.bindFlags), rd,
                                "GetTextureAllocationInfo"))
        return false;
    const D3D12_RESOURCE_ALLOCATION_INFO info =
        m_device->GetResourceAllocationInfo(0, 1, &rd);
    if (info.SizeInBytes == UINT64_MAX)
    {
        LOG_ERROR("GetTextureAllocationInfo: invalid resource desc (%ux%ux%u fmt=%u)",
                  desc.width, desc.height, desc.depth, (unsigned)desc.format);
        return false;
    }
    outSize  = info.SizeInBytes;
    outAlign = info.Alignment;
    return true;
}

bool GraphicsDX12::GetTextureFootprint(const RHI::TextureDesc& desc, uint64_t& outSize, uint64_t& outAlign)
{
    outSize  = 0;
    outAlign = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    D3D12_RESOURCE_DESC rd{};
    if (!BuildPlacedTextureDesc(desc, ToDxgiFormat(desc.format), ToD3D12ResourceFlags(desc.bindFlags), rd,
                                "GetTextureFootprint", true))
        return false;
    const D3D12_RESOURCE_ALLOCATION_INFO info = m_device->GetResourceAllocationInfo(0, 1, &rd);
    if (info.SizeInBytes == UINT64_MAX) return false;
    outSize  = info.SizeInBytes;
    outAlign = info.Alignment;
    return true;
}

bool GraphicsDX12::CreateTexturePlaced(const RHI::TextureDesc& desc,
                                       ID3D12Heap* heap, UINT64 heapOffset,
                                       RHI::Texture& outTexture)
{
    if (std::this_thread::get_id() == m_deviceFrameThread.load(std::memory_order_acquire))
        DrainFrameIfNeeded(Diag::DrainCause::ResourceOp);
    if (!heap) { LOG_ERROR("CreateTexturePlaced: null heap"); return false; }

    // Arena rule 3 (TransientAliasArena.h), made observable: every resident's disjointness argument
    // assumes placement in a serial phase, so a worker-side rebuild must create a committed texture
    // instead. A log line, not a refusal: a stray call still gets its resource ("suspicious", not
    // "black screen") and the name says who made it. With no frame open nobody races the alias arena
    // (init, graph compiles, every game-half path, which the render-thread park makes exclusive), so
    // that is a serial phase too.
    ENGINE_CHECK_ONCE(OwnsDeviceFrame() || !m_inFrame,
        "CreateTexturePlaced(%s, %ux%ux%u) off the device-frame thread — placed "
        "resources may only be created in a serial phase (see "
        "TransientAliasArena.h rule 3). The caller's worker-side rebuild path "
        "must pass allowPlaced=false and create a committed texture.",
        desc.debugName ? desc.debugName : "<unnamed>",
        desc.width, desc.height, desc.depth);

    TextureDX12 entry;
    const DXGI_FORMAT dxgiFmt = ToDxgiFormat(desc.format);
    const bool is3D = (desc.type == RHI::TextureDesc::Type::TEXTURE_3D);

    D3D12_RESOURCE_DESC rd{};
    if (!BuildPlacedTextureDesc(desc, dxgiFmt,
                                ToD3D12ResourceFlags(desc.bindFlags), rd,
                                "CreateTexturePlaced"))
        return false;

    const D3D12_RESOURCE_STATES initState = ToD3D12ResourceState(desc.layout);

    // Does this placement fit inside the heap? D3D12 does not check without the debug layer, and a
    // resource placed past the end reads unmapped memory on the GPU: a device hang with no CPU-side
    // error. (TransientAliasArena::kTailGuard puts slack after the arena's tail resident.) Refused, as
    // CreateTextureInArena refuses: every caller falls back to a committed texture or logs the failure.
    {
        const D3D12_HEAP_DESC hd = heap->GetDesc();
        const D3D12_RESOURCE_ALLOCATION_INFO ai =
            m_device->GetResourceAllocationInfo(0, 1, &rd);
        const uint64_t end = heapOffset + ai.SizeInBytes;
        // A TransientAliasArena heap carries its usable size (heap minus kTailGuard) as private data, and
        // that is its limit: a resident crossing into the guard band is the tail resident, the state that
        // hangs the GPU. Other heaps (SSR's private scratch heap has no guard) are checked against their
        // size.
        uint64_t usable = 0; UINT usableSz = sizeof(usable);
        const bool isArena = SUCCEEDED(heap->GetPrivateData(
                                 TransientAliasArena::kUsableSizeTag, &usableSz, &usable))
                             && usableSz == sizeof(usable);
        const uint64_t limit = isArena ? usable : hd.SizeInBytes;
        if (!ENGINE_CHECK_ONCE(end <= limit,
                "[ARENA-%s] placed '%s' %ux%u at off=%llu size=%llu -> end=%llu EXCEEDS "
                "the %s limit %llu by %llu bytes (heap %llu, tail guard %llu) — the next "
                "resident, or NRD's over-read, lands on unmapped memory - refused",
                isArena ? "GUARD" : "OOB",
                desc.debugName ? desc.debugName : "<unnamed>", desc.width, desc.height,
                (unsigned long long)heapOffset, (unsigned long long)ai.SizeInBytes,
                (unsigned long long)end, isArena ? "usable" : "heap",
                (unsigned long long)limit, (unsigned long long)(end - limit),
                (unsigned long long)hd.SizeInBytes,
                (unsigned long long)(isArena ? TransientAliasArena::kTailGuard : 0ull))
            && !m_lifetimeGuardsOff)
            return false;
    }

    if (FAILED(m_device->CreatePlacedResource(
            heap, heapOffset, &rd, initState, nullptr, IID_PPV_ARGS(&entry.resource))))
    {
        LOG_ERROR("CreateTexturePlaced: CreatePlacedResource failed (off=%llu %ux%ux%u)",
                  (unsigned long long)heapOffset, desc.width, desc.height, desc.depth);
        return false;
    }
    entry.state = initState;
    entry.planes       = RHI::HasStencilPlane(desc.format) ? 2 : 1;
    entry.stencilState = initState;
    // Keep the backing heap alive as long as this texture (and its kFrameCount-deferred release)
    // exists: see TextureDX12::placedHeap.
    entry.placedHeap = heap;
    GpuLedger::TrackPlaced(entry.resource.Get());

    // Optional debug name, as on the committed path; aliased transients are exactly the textures
    // worth telling apart in a capture.
    if (desc.debugName && desc.debugName[0] != '\0')
    {
        const int needed = MultiByteToWideChar(CP_UTF8, 0, desc.debugName, -1, nullptr, 0);
        if (needed > 0)
        {
            std::wstring wname(static_cast<size_t>(needed - 1), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, desc.debugName, -1, wname.data(), needed);
            entry.resource->SetName(wname.c_str());
        }
    }

    const uint32_t mipLevels = rd.MipLevels;

    if (RHI::HasFlag(desc.bindFlags, RHI::BindFlag::SHADER_RESOURCE))
    {
        // Same descriptor-exhaustion guards as CreateTexture (see there).
        entry.srv = m_cbvSrvUavAllocator.Allocate(1);
        if (!entry.srv.IsValid())
        {
            LOG_ERROR("CreateTexturePlaced: CBV_SRV_UAV heap exhausted (SRV) for '%s' — texture NOT created",
                      desc.debugName ? desc.debugName : "<unnamed>");
            ReleaseTextureViews(entry);
            return false;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
        srvd.Format                  = dxgiFmt;
        srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (is3D)
        {
            srvd.ViewDimension       = D3D12_SRV_DIMENSION_TEXTURE3D;
            srvd.Texture3D.MipLevels = mipLevels;
        }
        else
        {
            srvd.ViewDimension       = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvd.Texture2D.MipLevels = mipLevels;
        }
        m_cbvSrvUavAllocator.WriteSrv(entry.resource.Get(), &srvd, entry.srv.GetCpuHandle());
        entry.srv.CopyToGpu();
    }
    if (RHI::HasFlag(desc.bindFlags, RHI::BindFlag::UNORDERED_ACCESS))
    {
        auto fillUav = [&](D3D12_UNORDERED_ACCESS_VIEW_DESC& uavd, uint32_t mip)
        {
            uavd.Format = dxgiFmt;
            if (is3D)
            {
                uavd.ViewDimension         = D3D12_UAV_DIMENSION_TEXTURE3D;
                uavd.Texture3D.MipSlice    = mip;
                uavd.Texture3D.FirstWSlice = 0;
                uavd.Texture3D.WSize       = (std::max)(desc.depth >> mip, 1u);
            }
            else
            {
                uavd.ViewDimension        = D3D12_UAV_DIMENSION_TEXTURE2D;
                uavd.Texture2D.MipSlice   = mip;
            }
        };
        entry.uav = m_cbvSrvUavAllocator.Allocate(1);
        if (!entry.uav.IsValid())
        {
            LOG_ERROR("CreateTexturePlaced: CBV_SRV_UAV heap exhausted (UAV) for '%s' — texture NOT created",
                      desc.debugName ? desc.debugName : "<unnamed>");
            ReleaseTextureViews(entry);
            return false;
        }
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uavd{};
            fillUav(uavd, 0);
            m_cbvSrvUavAllocator.WriteUav(entry.resource.Get(), nullptr, &uavd, entry.uav.GetCpuHandle());
            entry.uav.CopyToGpu();
        }
        // Per-mip UAVs so GetTextureMipUAVGpuHandle works exactly like the
        // committed path (XeGTAO's 5-mip linear-depth prefilter needs u0..u4).
        if (mipLevels > 1)
        {
            entry.mipUavs.resize(mipLevels);
            for (uint32_t mip = 0; mip < mipLevels; ++mip)
            {
                entry.mipUavs[mip] = m_cbvSrvUavAllocator.Allocate(1);
                if (!entry.mipUavs[mip].IsValid())
                {
                    LOG_ERROR("CreateTexturePlaced: CBV_SRV_UAV heap exhausted at mip UAV %u/%u of '%s' — texture NOT created",
                              mip, mipLevels, desc.debugName ? desc.debugName : "<unnamed>");
                    ReleaseTextureViews(entry);
                    return false;
                }
                D3D12_UNORDERED_ACCESS_VIEW_DESC uavd{};
                fillUav(uavd, mip);
                m_cbvSrvUavAllocator.WriteUav(entry.resource.Get(), nullptr, &uavd,
                                              entry.mipUavs[mip].GetCpuHandle());
                entry.mipUavs[mip].CopyToGpu();
            }
        }
    }

    entry.generation = NextTextureGeneration();   // a reused id is a new texture
    std::lock_guard<std::mutex> lock(m_resourceMutex);
    if (!m_textureFreeList.empty())
    {
        outTexture.handleId = m_textureFreeList.back();
        m_textureFreeList.pop_back();
        m_texturePool[outTexture.handleId] = std::move(entry);
    }
    else
    {
        if (!ENGINE_CHECK_ONCE(m_texturePool.size() < m_texturePool.capacity(),
                "CreateTexturePlaced: texture pool full (%zu) — growing it would reallocate "
                "under lock-free readers; '%s' NOT created. Raise GraphicsDX12::kMaxTexturePool.",
                m_texturePool.capacity(), desc.debugName ? desc.debugName : "<unnamed>"))
        {
            ReleaseTextureViews(entry);
            return false;
        }
        outTexture.handleId = static_cast<uint32_t>(m_texturePool.size());
        m_texturePool.push_back(std::move(entry));
    }
    outTexture.type = RHI::GPUResource::Type::Texture;
    outTexture.desc = desc;

    // Mirror CreateTexture: publish the SRV into the bindless table slot.
    if (m_bindlessTexTable.IsValid() && outTexture.handleId < kMaxBindlessTextures)
    {
        const auto& poolEntry = m_texturePool[outTexture.handleId];
        if (poolEntry.srv.IsValid())
        {
            UINT descInc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            D3D12_CPU_DESCRIPTOR_HANDLE dst = m_bindlessTexTable.GetGpuCpuHandle();
            dst.ptr += static_cast<SIZE_T>(outTexture.handleId) * descInc;
            m_cbvSrvUavAllocator.CopyDescriptors(1, dst, poolEntry.srv.GetCpuHandle());
        }
    }
    return true;
}

// ==== UpdateTexture ====
// Re-upload bytes into an existing DEFAULT-heap texture, like CreateTexture's initialData path;
// for per-frame streaming (video playback, dynamic UI atlas). Synchronous (FlushUploadAndWait)
// outside a batch scope.
bool GraphicsDX12::UpdateTexture(RHI::Texture& texture,
                                  const RHI::SubresourceData* planes,
                                  uint32_t subresourceCount)
{
    // Hoisted: Upload() picks the caller's context (loader scope or execution lane) on every call.
    UploadContext& ctx = Upload();
    if (!texture.IsValid() || !planes || subresourceCount == 0) return false;
    if (texture.handleId >= m_texturePool.size()) return false;
    TextureDX12& entry = m_texturePool[texture.handleId];
    if (!entry.resource) return false;
    // This submission runs before the frame's queued stream copies, so on a stream target the data
    // would land in the wrong order.
    ENGINE_CHECK_ONCE(!entry.streamTarget,
        "UpdateTexture on texture %u, a stream target (QueueTextureUpload): the synchronous copy runs "
        "before this frame's queued copies - use QueueTextureUpload for every update of it",
        texture.handleId);

    D3D12_RESOURCE_DESC rd = entry.resource->GetDesc();
    const UINT actualSubresources = static_cast<UINT>(
        rd.MipLevels * rd.DepthOrArraySize);
    // Multi-plane formats (NV12 / P010) store each plane as its own subresource: total = mipLevels *
    // arraySize * planeCount. Accept either that count or planeCount (NV12: 2), the whole resource at
    // mip 0, array slice 0.
    UINT planeCount = 1;
    if (rd.Format == DXGI_FORMAT_NV12 || rd.Format == DXGI_FORMAT_P010) planeCount = 2;
    const UINT expected = actualSubresources * planeCount;
    if (subresourceCount != expected && subresourceCount != planeCount)
    {
        LOG_ERROR("UpdateTexture: subresourceCount=%u, expected %u (or %u for plane-only)",
                  subresourceCount, expected, planeCount);
        return false;
    }

    UINT64 uploadSize = 0;
    m_device->GetCopyableFootprints(&rd, 0, subresourceCount, 0,
                                     nullptr, nullptr, nullptr, &uploadSize);
    ComPtr<ID3D12Resource> staging;
    D3D12_HEAP_PROPERTIES uploadHp{ D3D12_HEAP_TYPE_UPLOAD };
    const auto stagingDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
    // CREATE_NOT_ZEROED: as for CreateBuffer's staging.
    if (FAILED(m_device->CreateCommittedResource(
            &uploadHp, D3D12_HEAP_FLAG_CREATE_NOT_ZEROED, &stagingDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&staging))))
    {
        LOG_ERROR("UpdateTexture: staging buffer alloc failed");
        return false;
    }
    GpuLedger::TrackCommitted(m_device.Get(), staging.Get(), Diag::Prof::GpuMemCat::Upload,
                              "UpdateTexture.Staging");
    Diag::Prof::NoteUploadBytes(uploadSize);

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresourceCount);
    std::vector<UINT>   numRows(subresourceCount);
    std::vector<UINT64> rowSizes(subresourceCount);
    m_device->GetCopyableFootprints(&rd, 0, subresourceCount, 0,
        footprints.data(), numRows.data(), rowSizes.data(), nullptr);

    uint8_t* stagingPtr = nullptr;
    D3D12_RANGE rr{ 0, 0 };
    ThrowIfFailed(staging->Map(0, &rr, reinterpret_cast<void**>(&stagingPtr)));
    for (UINT sub = 0; sub < subresourceCount; ++sub)
    {
        const auto& fp  = footprints[sub];
        uint8_t* dstRow = stagingPtr + fp.Offset;
        const uint8_t* srcRow = static_cast<const uint8_t*>(planes[sub].dataPtr);
        if (!srcRow) continue;
        for (UINT row = 0; row < numRows[sub]; ++row)
        {
            std::memcpy(dstRow, srcRow, static_cast<size_t>(rowSizes[sub]));
            dstRow += fp.Footprint.RowPitch;
            srcRow += planes[sub].rowPitch;
        }
    }
    staging->Unmap(0, nullptr);

    auto toCopy = CD3DX12_RESOURCE_BARRIER::Transition(
        entry.resource.Get(), entry.state, D3D12_RESOURCE_STATE_COPY_DEST);
    ctx.list->ResourceBarrier(1, &toCopy);
    for (UINT sub = 0; sub < subresourceCount; ++sub)
    {
        CD3DX12_TEXTURE_COPY_LOCATION dst(entry.resource.Get(), sub);
        CD3DX12_TEXTURE_COPY_LOCATION src(staging.Get(), footprints[sub]);
        ctx.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    auto fromCopy = CD3DX12_RESOURCE_BARRIER::Transition(
        entry.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, entry.state);
    ctx.list->ResourceBarrier(1, &fromCopy);
    // An existing texture, in whatever state the ledger says: the graphics list (only creation copies
    // use the COPY queue).
    ctx.directDirty = true;

    // Honour an open batch scope like CreateBuffer / CreateTexture: an unconditional
    // FlushUploadAndWait is a full graphics-queue drain, so a font page baking one new glyph or a
    // software-decoded video frame would stall mid-frame, and Mp4FrameSource's catch-up loop could
    // drain several times in one frame. Callers that need the copy visible at once open no batch.
    if (ctx.batchDepth > 0)
    {
        ctx.stagingBytes += uploadSize;
        ctx.stagingKeepAlive.push_back(std::move(staging));
        FlushBatchStagingIfNeeded();
        return true;
    }

    FlushUploadAndWait();
    return true;
}

// ==== CopyD3D12ResourceToTexture ====
// Synchronous GPU copy from an external ID3D12Resource (e.g. an FFmpeg D3D12VA hwaccel frame) into
// a managed RHI::Texture; Mp4FrameSource's hwaccel path.
bool GraphicsDX12::CopyD3D12ResourceToTexture(RHI::Texture& dst,
                                               ID3D12Resource* src,
                                               ID3D12Fence* waitFence,
                                               UINT64 waitFenceValue)
{
    // Hoisted: Upload() picks the caller's context (loader scope or execution lane) on every call.
    UploadContext& ctx = Upload();
    if (!dst.IsValid() || !src) return false;
    if (dst.handleId >= m_texturePool.size()) return false;
    TextureDX12& dstEntry = m_texturePool[dst.handleId];
    if (!dstEntry.resource) return false;
    ENGINE_CHECK_ONCE(!dstEntry.streamTarget,   // see UpdateTexture
        "CopyD3D12ResourceToTexture on texture %u, a stream target (QueueTextureUpload): the "
        "synchronous copy runs before this frame's queued copies", dst.handleId);
    // A source that does not fit (format, size, subresources) is refused,
    // never copied - an invalid copy the debug layer alone reports. The stream
    // path refuses it the same way.
    if (!NativeCopyFits(dstEntry.resource->GetDesc(), src->GetDesc()))
    {
        LOG_ERROR("CopyD3D12ResourceToTexture: the source differs from texture %u in format, size or "
                  "subresources", dst.handleId);
        return false;
    }

    // GPU-side wait for the producer's fence on the graphics queue: the upload list that
    // FlushUploadAndWait submits then waits until the producer (FFmpeg's video decode) has written
    // and signalled.
    if (waitFence && waitFenceValue > 0)
        ThrowIfFailed(m_queues[0]->Wait(waitFence, waitFenceValue));

    // FFmpeg's D3D12VA output sits in D3D12_RESOURCE_STATE_COMMON after the
    // video decode CL completes (its decode list transitions it back). The
    // copy reads it there through implicit promotion (COPY_SOURCE is
    // promotable from COMMON), so no barrier of ours touches the decoder's
    // surface. Only dst moves: current -> COPY_DEST -> back.
    const D3D12_RESOURCE_STATES dstStateBefore = dstEntry.state;
    const auto pre = CD3DX12_RESOURCE_BARRIER::Transition(
        dstEntry.resource.Get(), dstStateBefore, D3D12_RESOURCE_STATE_COPY_DEST);
    ctx.list->ResourceBarrier(1, &pre);

    // Whole-resource copy: CopyResource covers every subresource (NV12's Y and UV planes) when format
    // and size match.
    ctx.list->CopyResource(dstEntry.resource.Get(), src);

    const auto post = CD3DX12_RESOURCE_BARRIER::Transition(
        dstEntry.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, dstStateBefore);
    ctx.list->ResourceBarrier(1, &post);
    ctx.directDirty = true;   // the graphics list: it waits on the decoder's fence

    FlushUploadAndWait();
    return true;
}

// ==== Resource creation - CreateShader ====

bool GraphicsDX12::CreateShader(RHI::ShaderStage stage,
                                const void* bytecode, size_t bytecodeSize,
                                RHI::Shader& outShader)
{
    if (!bytecode || bytecodeSize == 0)
    {
        LOG_ERROR("CreateShader: empty bytecode");
        return false;
    }
    ShaderDX12 entry;
    const auto* ptr = static_cast<const uint8_t*>(bytecode);
    entry.bytecode.assign(ptr, ptr + bytecodeSize);

    // Stable FNV-1a over the bytecode: the same compile gives the same hash, so PSO cache names are
    // stable across runs even when the caller did not set desc.cacheKey.
    {
        uint64_t h = 14695981039346656037ull;
        for (uint8_t b : entry.bytecode) { h ^= b; h *= 1099511628211ull; }
        entry.bytecodeHash = h;
    }

    std::lock_guard<std::mutex> lock(m_resourceMutex);
    // Append-only: DestroyShader retires a slot, never recycles it (a bare index handle with no
    // generation would alias a different shader). The reserve is a hard cap, as for the texture and
    // buffer pools: CreatePipelineState reads this vector from workers.
    if (!ENGINE_CHECK_ONCE(m_shaderPool.size() < m_shaderPool.capacity(),
            "CreateShader: shader pool full (%zu) — growing it would reallocate under "
            "lock-free readers; shader NOT created. Raise GraphicsDX12::kMaxShaderPool "
            "(every hot reload retires slots permanently).",
            m_shaderPool.capacity()))
        return false;
    outShader.handleId = static_cast<uint32_t>(m_shaderPool.size());
    outShader.type      = RHI::GPUResource::Type::Shader;
    outShader.stage     = stage;
    m_shaderPool.push_back(std::move(entry));
    // Warn once at 90%: this is the only device pool that cannot recover. Every DestroyShader retires
    // its slot, so the count only grows, with hot reloads and with each open / close of a preview
    // panel (its idle GraphSet teardown: ~10 slots per cycle, ~400 cycles to the cap). The
    // ENGINE_CHECK_ONCE above fires at 100%, when shader creation fails and passes stop getting PSOs;
    // this says so while a session's worth of room is left.
    if (!m_shaderPoolHeadroomLogged &&
        m_shaderPool.size() * 10 >= m_shaderPool.capacity() * 9)
    {
        m_shaderPoolHeadroomLogged = true;
        LOG_WARNING("CreateShader: shader pool at %zu/%zu (90%%). Slots are "
                    "RETIRED, never recycled (a bare index handle has no "
                    "generation), so hot reloads and preview graph teardowns "
                    "ratchet this up for the life of the process. At 100%% "
                    "shader creation FAILS and passes stop getting PSOs.",
                    m_shaderPool.size(), m_shaderPool.capacity());
    }
    LOG_TRACE("CreateShader: stage=%u, %zu bytes", (unsigned)stage, bytecodeSize);
    return true;
}

// ==== Resource creation - CreateSampler ====

// Coupling guard: a field added to RHI::SamplerDesc and wired into D3D12_SAMPLER_DESC in
// CreateSampler but not into MakeSamplerKey would silently merge two different samplers. 24 = 4
// enum bytes + mipLodBias@4 + maxAnisotropy@8 + comparisonFunc/borderColor@12 + pad + minLod@16 +
// maxLod@20.
static_assert(sizeof(RHI::SamplerDesc) == 24,
              "RHI::SamplerDesc changed - add the new field to MakeSamplerKey "
              "(plan S4 M0.3): a field that reaches D3D12_SAMPLER_DESC but not "
              "the key lets two different samplers merge.");

// Canonical dedupe key: one field per slot, floats by exact bit pattern, so two keys are equal
// only when every field that reaches D3D12_SAMPLER_DESC is. Not a memcmp of RHI::SamplerDesc: its
// padding (uint8_t enums between floats / uint32) is not part of the value. Bit-exact floats are
// deliberately conservative: -0.0f and 0.0f sample identically but key apart, costing one extra
// descriptor, never a wrong merge.
GraphicsDX12::SamplerKey GraphicsDX12::MakeSamplerKey(const RHI::SamplerDesc& desc) noexcept
{
    auto bits = [](float f) noexcept
    {
        uint32_t u = 0;
        std::memcpy(&u, &f, sizeof(u));
        return u;
    };
    return SamplerKey{
        static_cast<uint32_t>(desc.filter),
        static_cast<uint32_t>(desc.addressU),
        static_cast<uint32_t>(desc.addressV),
        static_cast<uint32_t>(desc.addressW),
        desc.maxAnisotropy,
        static_cast<uint32_t>(desc.comparisonFunc),
        static_cast<uint32_t>(desc.borderColor),
        bits(desc.mipLodBias),
        bits(desc.minLod),
        bits(desc.maxLod),
    };
}

bool GraphicsDX12::CreateSampler(const RHI::SamplerDesc& desc, int& outDescriptorIndex)
{
    // Dedupe (kSamplerDedupe). Safe only because the pool is append-only and nothing mutates a
    // sampler after creation, so a recorded index stays valid for the device's life. A hit costs no
    // heap slot and no descriptor; the caller gets the same index, hence the same heap index out of
    // SetDynamicSampler.
    [[maybe_unused]] SamplerKey key{};   // both users are if-constexpr'd away when off
    if constexpr (kSamplerDedupe)
    {
        key = MakeSamplerKey(desc);
        std::lock_guard<std::mutex> lock(m_resourceMutex);
        const auto it = m_samplerDedupe.find(key);
        if (it != m_samplerDedupe.end())
        {
            ++m_samplerDedupeHits;
            outDescriptorIndex = it->second;
            LOG_TRACE("CreateSampler: descriptor index=%d (dedupe hit)", outDescriptorIndex);
            return true;
        }
    }

    // Hard cap, checked before a descriptor is spent: SetDynamicSampler (SamplerHeapIndex) indexes
    // m_samplerPool and reads size() from recording workers without a lock, so a reallocating
    // push_back would dangle an entry one of them is reading. The allocator (Init'd to kMaxSamplers)
    // is the first wall; this backstop keeps push_back honest if the two diverge. The unlocked read
    // can only err towards one extra descriptor allocation; the locked check below guards the
    // push_back. Refuse loudly: -1 is the failure SetDynamicSampler already reports (it leaves the
    // previous draw's sampler in the root constant), so a caller that ignores the return still gets
    // a named error, not silent mis-shading.
    if (!ENGINE_CHECK_ONCE(m_samplerPool.size() < m_samplerPool.capacity(),
            "CreateSampler: sampler pool full (%zu) - growing it would reallocate under "
            "lock-free readers (SetDynamicSampler); sampler NOT created. kMaxSamplers is already "
            "the D3D12 shader-visible cap, and the pool is APPEND-ONLY (no DestroySampler), "
            "so the fix is fewer distinct descs or a tombstone + FreeStatic free list, "
            "not a bigger number.",
            m_samplerPool.capacity()))
    {
        outDescriptorIndex = -1;
        return false;
    }

    auto allocation = m_samplerAllocator.Allocate(1);
    if (!allocation.IsValid())
    {
        LOG_ERROR("CreateSampler: sampler heap exhausted (max %u)", kMaxSamplers);
        outDescriptorIndex = -1;
        return false;
    }

    D3D12_SAMPLER_DESC sd{};
    sd.Filter         = ToD3D12Filter(desc.filter);
    sd.AddressU       = ToD3D12AddressMode(desc.addressU);
    sd.AddressV       = ToD3D12AddressMode(desc.addressV);
    sd.AddressW       = ToD3D12AddressMode(desc.addressW);
    sd.MipLODBias     = desc.mipLodBias;
    sd.MaxAnisotropy  = desc.maxAnisotropy;
    sd.ComparisonFunc = ToD3D12ComparisonFunc(desc.comparisonFunc);
    sd.MinLOD         = desc.minLod;
    sd.MaxLOD         = desc.maxLod;
    switch (desc.borderColor)
    {
    case RHI::SamplerBorderColor::OPAQUE_BLACK:
        sd.BorderColor[0]=sd.BorderColor[1]=sd.BorderColor[2]=0.0f; sd.BorderColor[3]=1.0f; break;
    case RHI::SamplerBorderColor::OPAQUE_WHITE:
        sd.BorderColor[0]=sd.BorderColor[1]=sd.BorderColor[2]=sd.BorderColor[3]=1.0f; break;
    default: break; // transparent black (all zero)
    }

    m_device->CreateSampler(&sd, allocation.GetCpuHandle());
    allocation.CopyToGpu();

    bool poolFull = false;
    {
        std::lock_guard<std::mutex> lock(m_resourceMutex);
        // Locked re-check of the cap: the unlocked pre-check above can be raced
        // by one extra allocation; this one cannot, and it is what actually
        // protects the push_back.
        if (m_samplerPool.size() < m_samplerPool.capacity())
        {
            outDescriptorIndex = static_cast<int>(m_samplerPool.size());
            m_samplerPool.push_back(std::move(allocation));
            if constexpr (kSamplerDedupe)
            {
                // try_emplace, not operator[]: if two threads raced past the
                // lookup above with the same desc, both descriptors are valid
                // and both indices work - the first one recorded stays the
                // canonical answer.
                m_samplerDedupe.try_emplace(key, outDescriptorIndex);
            }
        }
        else
            poolFull = true;
    }
    if (poolFull)
    {
        // Free OUTSIDE m_resourceMutex: DescriptorAllocation::Free takes the
        // allocator's own mutex and no other sampler path holds both, so this
        // keeps the lock graph free of a resourceMutex -> allocatorMutex edge.
        allocation.Free();
        (void)ENGINE_CHECK_ONCE(false,
            "CreateSampler: sampler pool full (%zu) at push (raced past the pre-check)",
            m_samplerPool.capacity());
        outDescriptorIndex = -1;
        return false;
    }

    LOG_TRACE("CreateSampler: descriptor index=%d", outDescriptorIndex);
    return true;
}

// ==== Resource creation - CreatePipelineState ====

namespace
{
    constexpr uint64_t kFnvBasis = 14695981039346656037ull;
    constexpr uint64_t kFnvPrime = 1099511628211ull;

    // A psoName's identity as a number, so "did this run store it" is a set probe. FNV-1a over the
    // UTF-16 code units, the same hash shape CreateShader uses over bytecode.
    uint64_t HashPsoName(const wchar_t* name)
    {
        uint64_t h = kFnvBasis;
        for (const wchar_t* p = name; *p; ++p)
        {
            h ^= static_cast<uint64_t>(static_cast<uint16_t>(*p));
            h *= kFnvPrime;
        }
        return h;
    }

    uint64_t Fnv1a64(const void* data, size_t size, uint64_t h)
    {
        const auto* b = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; ++i) { h ^= b[i]; h *= kFnvPrime; }
        return h;
    }
}

// A store, successful or not (E_INVALIDARG = the library already holds the
// name: a later Load of it is a session dupe either way). Throttled warning.
void GraphicsDX12::NotePsoLibHit(uint64_t psoNameHash)
{
    // Count first, insert after: the first ask for a name is the disk hit.
    ++m_psoLibHits;
    std::lock_guard<std::mutex> nameLock(m_psoLibNamesMutex);
    if (m_psoLibStoredNames.count(psoNameHash)) ++m_psoLibSessionDupes;
    else                                        ++m_psoLibDiskHits;
    m_psoLibStoredNames.insert(psoNameHash);
}

void GraphicsDX12::NotePsoLibStore(uint64_t psoNameHash, HRESULT hrStore)
{
    {
        std::lock_guard<std::mutex> nameLock(m_psoLibNamesMutex);
        m_psoLibStoredNames.insert(psoNameHash);
    }
    if (SUCCEEDED(hrStore))
        m_psoLibStoresSinceSave.fetch_add(1, std::memory_order_relaxed);
    if (FAILED(hrStore))
    {
        static std::atomic<uint32_t> s_storeFailCount{ 0 };
        if (s_storeFailCount.fetch_add(1) == 0)
            LOG_WARNING("CreatePipelineState: StorePipeline failed "
                        "(HRESULT=0x%08X). 0x80070057 = duplicate PSO name in "
                        "the library — harmless. Further occurrences are "
                        "counted, not logged.",
                        static_cast<unsigned>(hrStore));
    }
}

// A compute pipeline from the PSO library when it holds psoName, else built by the driver and
// stored under it: the graphics path's sequence, under the same per-name lease (two threads never
// work on one name inside the library at once; a waiter then loads what the first stored).
// Counted apart from the graphics + mesh numbers (computeHits / computeMisses / computeCompiled
// on the `[PSO] library` line) so their pins keep their meaning. profile = false on a thread that
// must not enter a profiler scope (CreatePipelineStates' std::async jobs). No library: the
// driver builds it.
bool GraphicsDX12::CreateComputePSOViaLibrary(const wchar_t* psoName, uint64_t psoNameHash,
                                              const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc,
                                              Microsoft::WRL::ComPtr<ID3D12PipelineState>& out,
                                              bool profile, HRESULT& hrOut)
{
    struct Lease
    {
        GraphicsDX12* owner;
        uint64_t      name;
        Lease(GraphicsDX12* o, uint64_t n) : owner(o), name(n) { if (owner) owner->AcquirePsoLibName(name); }
        Lease(const Lease&)            = delete;
        Lease& operator=(const Lease&) = delete;
        ~Lease() { if (owner) owner->ReleasePsoLibName(name); }
    } lease(m_psoLibrary ? this : nullptr, psoNameHash);

    const D3D12_SHADER_BYTECODE stages[Gfx::PsoManifest::kStageCount] = { {}, {}, {}, {}, {}, {}, {}, desc.CS };
    if (m_psoLibrary)
    {
        if (SUCCEEDED(m_psoLibrary->LoadComputePipeline(psoName, &desc, IID_PPV_ARGS(&out))))
        {
            ++m_psoLibComputeHits;
            hrOut = S_OK;
            RecordPipeline(Gfx::PsoManifest::Kind::Compute, psoName, desc.pRootSignature, stages, nullptr, 0);
            return true;
        }
        ++m_psoLibComputeMisses;
    }
    const auto t0 = std::chrono::steady_clock::now();
    if (profile)
    {
        PROF_SCOPE("PSO.CreateCompute");
        hrOut = m_device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&out));
    }
    else
    {
        hrOut = m_device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&out));
    }
    if (FAILED(hrOut)) return false;
    const uint32_t buildUs = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count());
    PipelineStats::psoCreates.fetch_add(1, std::memory_order_relaxed);
    if (m_psoLibrary)
    {
        ++m_psoLibComputeCompiled;
        NotePsoLibStore(psoNameHash, m_psoLibrary->StorePipeline(psoName, out.Get()));
    }
    RecordPipeline(Gfx::PsoManifest::Kind::Compute, psoName, desc.pRootSignature, stages, nullptr, buildUs);
    return true;
}

bool GraphicsDX12::CreateComputePipelineCached(const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc,
                                               const void* rootSigBlob, size_t rootSigBlobSize,
                                               Microsoft::WRL::ComPtr<ID3D12PipelineState>& out)
{
    // Named by what the ISA depends on: the module's serialized root signature
    // and the bytecode. "NPSO_" keeps the name space apart from ComputePSOName's.
    wchar_t  psoName[64] = {};
    uint64_t nameHash    = 0;
    if (m_psoLibrary)
    {
        uint64_t h = Fnv1a64(rootSigBlob, rootSigBlobSize, kFnvBasis);
        h = Fnv1a64(desc.CS.pShaderBytecode, desc.CS.BytecodeLength, h);
        swprintf_s(psoName, L"NPSO_%016llX", static_cast<unsigned long long>(h));
        nameHash = HashPsoName(psoName);
    }
    NoteRootSignature(desc.pRootSignature, rootSigBlob, rootSigBlobSize);
    HRESULT hr = S_OK;
    return CreateComputePSOViaLibrary(psoName, nameHash, desc, out, /*profile*/false, hr);
}

bool GraphicsDX12::CreateGraphicsPipelineCached(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
                                                const void* rootSigBlob, size_t rootSigBlobSize, uint64_t stateKey,
                                                Microsoft::WRL::ComPtr<ID3D12PipelineState>& out)
{
    // "NGPSO_" keeps the name space apart from ComputePSOName's and NPSO_'s.
    wchar_t  psoName[64] = {};
    uint64_t nameHash    = 0;
    if (m_psoLibrary)
    {
        uint64_t h = Fnv1a64(rootSigBlob, rootSigBlobSize, kFnvBasis);
        h = Fnv1a64(desc.VS.pShaderBytecode, desc.VS.BytecodeLength, h);
        h = Fnv1a64(desc.PS.pShaderBytecode, desc.PS.BytecodeLength, h);
        h = Fnv1a64(&stateKey, sizeof(stateKey), h);
        swprintf_s(psoName, L"NGPSO_%016llX", static_cast<unsigned long long>(h));
        nameHash = HashPsoName(psoName);
    }
    struct Lease
    {
        GraphicsDX12* owner;
        uint64_t      name;
        Lease(GraphicsDX12* o, uint64_t n) : owner(o), name(n) { if (owner) owner->AcquirePsoLibName(name); }
        Lease(const Lease&)            = delete;
        Lease& operator=(const Lease&) = delete;
        ~Lease() { if (owner) owner->ReleasePsoLibName(name); }
    } lease(m_psoLibrary ? this : nullptr, nameHash);

    NoteRootSignature(desc.pRootSignature, rootSigBlob, rootSigBlobSize);
    const D3D12_SHADER_BYTECODE stages[Gfx::PsoManifest::kStageCount] = { desc.VS, desc.PS, {}, {}, {}, {}, {}, {} };
    if (m_psoLibrary)
    {
        if (SUCCEEDED(m_psoLibrary->LoadGraphicsPipeline(psoName, &desc, IID_PPV_ARGS(&out))))
        {
            NotePsoLibHit(nameHash);
            if (Gfx::PsoManifest::RecordingEnabled())
            {
                const Gfx::PsoManifest::State state = Gfx::PsoManifest::StateOf(desc);
                RecordPipeline(Gfx::PsoManifest::Kind::Graphics, psoName, desc.pRootSignature, stages, &state, 0);
            }
            return true;
        }
        ++m_psoLibMisses;
        PipelineStats::psoLibMisses.fetch_add(1, std::memory_order_relaxed);
    }
    if (FAILED(m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&out)))) return false;
    PipelineStats::psoCreates.fetch_add(1, std::memory_order_relaxed);
    if (m_psoLibrary)
    {
        ++m_psoLibCompiled;
        NotePsoLibStore(nameHash, m_psoLibrary->StorePipeline(psoName, out.Get()));
    }
    if (Gfx::PsoManifest::RecordingEnabled() && m_psoLibrary)
    {
        const Gfx::PsoManifest::State state = Gfx::PsoManifest::StateOf(desc);
        RecordPipeline(Gfx::PsoManifest::Kind::Graphics, psoName, desc.pRootSignature, stages, &state, 0);
    }
    return true;
}

bool GraphicsDX12::CreatePipelineState(const RHI::PipelineStateDesc& desc,
                                       RHI::PipelineState& outPSO)
{
    PipelineStateDX12 internal;

    // If a compute shader is set and no vertex shader, create a compute PSO.
    if (desc.cs && !desc.vs)
    {
        if (!m_computeRootSignature)
        { LOG_ERROR("CreatePipelineState: compute root signature not initialized"); return false; }
        // A pipeline layout replaces the shared compute root signature; BindPipelineState sets
        // whichever the PSO was built with.
        Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature = m_computeRootSignature;
        if (desc.layout && !LookupPipelineLayout(*desc.layout, rootSignature)) return false;
        internal.rootSignature = rootSignature;
        internal.isCompute     = true;

        D3D12_COMPUTE_PIPELINE_STATE_DESC cpsoDesc{};
        cpsoDesc.pRootSignature = rootSignature.Get();
        if (!LookupShaderBytecode(desc.cs, cpsoDesc.CS, nullptr, "CS")) return false;

        // Through the PSO library, like the graphics branch. The name is taken before the lease
        // (ComputePSOName takes m_resourceMutex); for a compute desc it mixes the CS bytecode and the
        // layout's blob hash, else m_rootSigHash (which folds the compute signature in).
        wchar_t  psoName[64] = {};
        uint64_t nameHash    = 0;
        if (m_psoLibrary)
        {
            ComputePSOName(desc, psoName, 64);
            nameHash = HashPsoName(psoName);
        }
        HRESULT hr = S_OK;
        if (!CreateComputePSOViaLibrary(psoName, nameHash, cpsoDesc, internal.pso, /*profile*/true, hr))
        {
            LOG_ERROR("CreatePipelineState: CreateComputePipelineState failed 0x%08X", static_cast<unsigned>(hr));
            return false;
        }

        return PublishPipelineState(std::move(internal), outPSO, "compute");
    }

    // Reuse the root signature built once in LoadAssets().
    if (!m_defaultRootSignature)
    { LOG_ERROR("CreatePipelineState: default root signature not initialised"); return false; }
    // A pipeline layout replaces the shared graphics signature here too; ComputePSOName then names
    // the PSO by the layout's blob hash, so the library never hands it out for another layout.
    internal.rootSignature = m_defaultRootSignature;
    if (desc.layout && !LookupPipelineLayout(*desc.layout, internal.rootSignature)) return false;

    // PSO library plumbing shared by the graphics and mesh branches (a psoName's identity as a
    // number: HashPsoName).
    auto hashPsoName = [](const wchar_t* name) -> uint64_t { return HashPsoName(name); };
    // From the Load to the Store, only one thread at a time works on this psoName inside the library:
    // two concurrent loads of one name corrupt the pipeline the library remembers for it
    // (m_psoLibInflight, GraphicsDX12.h). Taken after every m_resourceMutex step (bytecode lookups,
    // ComputePSOName) and released before PublishPipelineState, or by the destructor on a failure.
    struct PsoLibNameLease
    {
        GraphicsDX12* owner;
        uint64_t      name;
        PsoLibNameLease(GraphicsDX12* o, uint64_t n) : owner(o), name(n)
        { if (owner) owner->AcquirePsoLibName(name); }
        PsoLibNameLease(const PsoLibNameLease&)            = delete;
        PsoLibNameLease& operator=(const PsoLibNameLease&) = delete;
        ~PsoLibNameLease() { Release(); }
        void Release() { if (owner) owner->ReleasePsoLibName(name); owner = nullptr; }
    };
    // A library hit, by kind (disk hit or session dupe; see the graphics branch).
    auto noteLibraryHit = [this](uint64_t nameHash) { NotePsoLibHit(nameHash); };
    // A store, successful or not: NotePsoLibStore, shared with compute.
    auto noteLibraryStore = [this](uint64_t nameHash, HRESULT hrStore) { NotePsoLibStore(nameHash, hrStore); };

    // Helper to extract bytecode from a Shader handle. An ABSENT stage (null /
    // invalid handle) is legal and yields empty bytecode; a DESTROYED handle
    // is not, and LookupShaderBytecode reports it once.
    auto getByteCode = [this](const RHI::Shader* s, const char* stage) -> D3D12_SHADER_BYTECODE
    {
        D3D12_SHADER_BYTECODE bc{ nullptr, 0 };
        if (!s || !s->IsValid()) return bc;
        (void)LookupShaderBytecode(s, bc, nullptr, stage);
        return bc;
    };

    // Translate blend state
    D3D12_BLEND_DESC blendDesc{};
    if (desc.bs)
    {
        blendDesc.AlphaToCoverageEnable  = desc.bs->alphaToCoverageEnable;
        blendDesc.IndependentBlendEnable = desc.bs->independentBlendEnable;
        for (int i = 0; i < 8; ++i)
        {
            const auto& s = desc.bs->renderTarget[i];
            auto& d = blendDesc.RenderTarget[i];
            d.BlendEnable           = s.blendEnable;
            d.SrcBlend              = ToD3D12Blend(s.srcBlend);
            d.DestBlend             = ToD3D12Blend(s.destBlend);
            d.BlendOp               = ToD3D12BlendOp(s.blendOp);
            d.SrcBlendAlpha         = ToD3D12Blend(s.srcBlendAlpha);
            d.DestBlendAlpha        = ToD3D12Blend(s.destBlendAlpha);
            d.BlendOpAlpha          = ToD3D12BlendOp(s.blendOpAlpha);
            // D3D12 allows only the least significant 4 bits (R/G/B/A); RHI::ColorWrite::ENABLE_ALL is ~0u
            d.RenderTargetWriteMask = static_cast<UINT8>(s.renderTargetWriteMask) & 0x0Fu;
        }
    }
    else
    {
        for (auto& rt : blendDesc.RenderTarget)
            rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }

    // Translate rasterizer state
    D3D12_RASTERIZER_DESC rasterDesc{};
    if (desc.rs)
    {
        rasterDesc.FillMode              = ToD3D12FillMode(desc.rs->fillMode);
        rasterDesc.CullMode              = ToD3D12CullMode(desc.rs->cullMode);
        rasterDesc.FrontCounterClockwise = desc.rs->frontCounterClockwise;
        rasterDesc.DepthBias             = desc.rs->depthBias;
        rasterDesc.DepthBiasClamp        = desc.rs->depthBiasClamp;
        rasterDesc.SlopeScaledDepthBias  = desc.rs->slopeScaledDepthBias;
        rasterDesc.DepthClipEnable       = desc.rs->depthClipEnable;
        rasterDesc.MultisampleEnable     = desc.rs->multisampleEnable;
        rasterDesc.AntialiasedLineEnable = desc.rs->antialiasedLineEnable;
        rasterDesc.ConservativeRaster    = desc.rs->conservativeRasterizationEnable
                                          ? D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON
                                          : D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    }
    else
    {
        rasterDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    }

    // Translate depth-stencil state
    D3D12_DEPTH_STENCIL_DESC dsDesc{};
    if (desc.dss)
    {
        auto toStencilOp = [](RHI::StencilOp o) -> D3D12_STENCIL_OP {
            switch(o) {
            case RHI::StencilOp::KEEP:     return D3D12_STENCIL_OP_KEEP;
            case RHI::StencilOp::ZERO:     return D3D12_STENCIL_OP_ZERO;
            case RHI::StencilOp::REPLACE:  return D3D12_STENCIL_OP_REPLACE;
            case RHI::StencilOp::INCR_SAT: return D3D12_STENCIL_OP_INCR_SAT;
            case RHI::StencilOp::DECR_SAT: return D3D12_STENCIL_OP_DECR_SAT;
            case RHI::StencilOp::INVERT:   return D3D12_STENCIL_OP_INVERT;
            case RHI::StencilOp::INCR:     return D3D12_STENCIL_OP_INCR;
            case RHI::StencilOp::DECR:     return D3D12_STENCIL_OP_DECR;
            default:                       return D3D12_STENCIL_OP_KEEP;
            }
        };
        dsDesc.DepthEnable      = desc.dss->depthEnable;
        dsDesc.DepthWriteMask   = (desc.dss->depthWriteMask == RHI::DepthWriteMask::ALL)
                                  ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        dsDesc.DepthFunc        = ToD3D12ComparisonFunc(desc.dss->depthFunc);
        dsDesc.StencilEnable    = desc.dss->stencilEnable;
        dsDesc.StencilReadMask  = desc.dss->stencilReadMask;
        dsDesc.StencilWriteMask = desc.dss->stencilWriteMask;
        dsDesc.FrontFace = { toStencilOp(desc.dss->frontFace.stencilFailOp),
                             toStencilOp(desc.dss->frontFace.stencilDepthFailOp),
                             toStencilOp(desc.dss->frontFace.stencilPassOp),
                             ToD3D12ComparisonFunc(desc.dss->frontFace.stencilFunc) };
        dsDesc.BackFace  = { toStencilOp(desc.dss->backFace.stencilFailOp),
                             toStencilOp(desc.dss->backFace.stencilDepthFailOp),
                             toStencilOp(desc.dss->backFace.stencilPassOp),
                             ToD3D12ComparisonFunc(desc.dss->backFace.stencilFunc) };
    }
    else
    {
        // No depth-stencil state given: depth and stencil off, but every enum must still hold a legal
        // D3D12 value; the zero-filled desc has DepthFunc = 0, which is not a D3D12_COMPARISON_FUNC.
        dsDesc               = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        dsDesc.DepthEnable   = FALSE;
        dsDesc.StencilEnable = FALSE;
    }
    // What a draw with this pipeline writes through the bound DSV: the draw-time depth tripwire
    // compares it with the pass's declaration.
    {
        auto opWrites = [](const D3D12_DEPTH_STENCILOP_DESC& f) {
            return f.StencilFailOp != D3D12_STENCIL_OP_KEEP || f.StencilDepthFailOp != D3D12_STENCIL_OP_KEEP ||
                   f.StencilPassOp != D3D12_STENCIL_OP_KEEP;
        };
        internal.writesDepth   = dsDesc.DepthEnable && dsDesc.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL;
        internal.writesStencil = dsDesc.StencilEnable && dsDesc.StencilWriteMask != 0 &&
                                 (opWrites(dsDesc.FrontFace) || opWrites(dsDesc.BackFace));
        internal.stencilWriteMask = internal.writesStencil ? dsDesc.StencilWriteMask : 0;
    }

    // Map RHI topology to D3D12 topology type
    auto toTopologyType = [](RHI::PrimitiveTopology t) -> D3D12_PRIMITIVE_TOPOLOGY_TYPE {
        switch (t) {
        case RHI::PrimitiveTopology::POINTLIST:   return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        case RHI::PrimitiveTopology::LINELIST:
        case RHI::PrimitiveTopology::LINESTRIP:   return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        case RHI::PrimitiveTopology::PATCHLIST:   return D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
        default:                                  return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        }
    };

    // Mesh-shader path: AS + MS (+ optional PS) through a pipeline state stream and
    // ID3D12Device2::CreatePipelineState. Uses the PSO library like the graphics branch: the same name
    // (ComputePSOName mixes the AS / MS bytecode), the same name lease from the Load to the Store,
    // ID3D12PipelineLibrary1::LoadPipeline with the very stream desc that was stored, and its own
    // hit / miss / compile counters beside the shared ones (misses == compiled holds per kind).
    if (desc.ms || desc.as)
    {
        ComPtr<ID3D12Device2> device2;
        if (FAILED(m_device.As(&device2)))
        {
            LOG_ERROR("CreatePipelineState (mesh): ID3D12Device2 unavailable");
            return false;
        }

        MeshPsoStream stream{};

        stream.rootSig  = internal.rootSignature.Get();
        stream.as       = getByteCode(desc.as, "AS");
        stream.ms       = getByteCode(desc.ms, "MS");
        stream.ps       = getByteCode(desc.ps, "PS");
        stream.blend    = CD3DX12_BLEND_DESC(blendDesc);
        stream.raster   = CD3DX12_RASTERIZER_DESC(rasterDesc);
        stream.depth    = CD3DX12_DEPTH_STENCIL_DESC(dsDesc);
        stream.dsFormat = ToDxgiFormat(desc.dsvFormat);

        D3D12_RT_FORMAT_ARRAY rtfa{};
        rtfa.NumRenderTargets = desc.rtvCount;
        for (UINT i = 0; i < desc.rtvCount && i < 8u; ++i)
            rtfa.RTFormats[i] = ToDxgiFormat(desc.rtvFormats[i]);
        stream.rtFormats = rtfa;

        DXGI_SAMPLE_DESC sd{};
        sd.Count   = desc.sampleCount ? desc.sampleCount : 1;
        sd.Quality = 0;
        stream.sample     = sd;
        stream.sampleMask = desc.sampleMask;
        // The topology subobject is what makes a stored mesh PSO loadable: the rasterizer takes the
        // topology from the MS's OutputTopology, but without the subobject every LoadPipeline in a later
        // process fails with "the pipeline state desc provided does not match the one used to create the
        // PSO". desc.pt is also mixed into the name and must agree with the MS's output topology
        // (TRIANGLELIST, the default, for every mesh shader in the engine).
        stream.topology   = toTopologyType(desc.pt);

        D3D12_PIPELINE_STATE_STREAM_DESC streamDesc{ sizeof(stream), &stream };

        wchar_t psoName[64] = {};
        ComputePSOName(desc, psoName, 64);
        const uint64_t psoNameHash = hashPsoName(psoName);
        ComPtr<ID3D12PipelineLibrary1> lib1;
        if (m_psoLibrary) (void)m_psoLibrary.As(&lib1);
        PsoLibNameLease nameLease(lib1 ? this : nullptr, psoNameHash);

        bool loadedFromLibrary = false;
        if (lib1)
        {
            if (SUCCEEDED(lib1->LoadPipeline(psoName, &streamDesc, IID_PPV_ARGS(&internal.pso))))
            {
                loadedFromLibrary = true;
                noteLibraryHit(psoNameHash);
                ++m_psoLibMeshHits;
            }
            else
            {
                ++m_psoLibMisses;
                ++m_psoLibMeshMisses;
                PipelineStats::psoLibMisses.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (!loadedFromLibrary && desc.libraryOnly) return false;
        uint32_t buildUs = 0;
        if (!loadedFromLibrary)
        {
            HRESULT hr;
            {
                PROF_SCOPE("PSO.CreateMesh");
                const auto t0 = std::chrono::steady_clock::now();
                hr = device2->CreatePipelineState(&streamDesc, IID_PPV_ARGS(&internal.pso));
                buildUs = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - t0).count());
            }
            if (FAILED(hr))
            {
                LOG_ERROR("CreatePipelineState (mesh): CreatePipelineState failed 0x%08X",
                          static_cast<unsigned>(hr));
                return false;
            }
            PipelineStats::psoCreates.fetch_add(1, std::memory_order_relaxed);
            if (lib1)
            {
                ++m_psoLibCompiled;
                ++m_psoLibMeshCompiled;
                noteLibraryStore(psoNameHash, lib1->StorePipeline(psoName, internal.pso.Get()));
            }
        }
        if (Gfx::PsoManifest::RecordingEnabled())
        {
            Gfx::PsoManifest::State state;
            state.blend            = blendDesc;
            state.sampleMask       = desc.sampleMask;
            state.raster           = rasterDesc;
            state.depthStencil     = dsDesc;
            state.topologyType     = toTopologyType(desc.pt);
            state.numRenderTargets = rtfa.NumRenderTargets;
            for (UINT i = 0; i < 8u; ++i) state.rtvFormats[i] = rtfa.RTFormats[i];
            state.dsvFormat     = ToDxgiFormat(desc.dsvFormat);
            state.sampleCount   = sd.Count;
            state.sampleQuality = sd.Quality;
            D3D12_SHADER_BYTECODE stages[Gfx::PsoManifest::kStageCount] = {};
            stages[Gfx::PsoManifest::kAS] = getByteCode(desc.as, "AS");
            stages[Gfx::PsoManifest::kMS] = getByteCode(desc.ms, "MS");
            stages[Gfx::PsoManifest::kPS] = getByteCode(desc.ps, "PS");
            RecordPipeline(Gfx::PsoManifest::Kind::Mesh, psoName, internal.rootSignature.Get(), stages, &state,
                           buildUs);
        }

        nameLease.Release();
        return PublishPipelineState(std::move(internal), outPSO, "mesh");
    }

    // Translate input layout
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputElements;
    if (desc.il)
    {
        for (const auto& e : desc.il->elements)
        {
            D3D12_INPUT_ELEMENT_DESC ied{};
            ied.SemanticName         = e.semanticName;
            ied.SemanticIndex        = e.semanticIndex;
            ied.Format               = ToDxgiFormat(e.format);
            ied.InputSlot            = e.inputSlot;
            ied.AlignedByteOffset    = (e.alignedByteOffset == RHI::InputLayout::kAppendAlignedElement)
                                       ? D3D12_APPEND_ALIGNED_ELEMENT : e.alignedByteOffset;
            ied.InputSlotClass       = (e.inputSlotClass == RHI::InputClassification::PER_INSTANCE_DATA)
                                       ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                                       : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
            ied.InstanceDataStepRate = (ied.InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA) ? 1 : 0;
            inputElements.push_back(ied);
        }
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature        = internal.rootSignature.Get();
    psoDesc.VS                    = getByteCode(desc.vs, "VS");
    psoDesc.PS                    = getByteCode(desc.ps, "PS");
    psoDesc.HS                    = getByteCode(desc.hs, "HS");
    psoDesc.DS                    = getByteCode(desc.ds, "DS");
    psoDesc.GS                    = getByteCode(desc.gs, "GS");
    psoDesc.BlendState            = blendDesc;
    psoDesc.SampleMask            = desc.sampleMask;
    psoDesc.RasterizerState       = rasterDesc;
    psoDesc.DepthStencilState     = dsDesc;
    psoDesc.InputLayout           = { inputElements.data(), static_cast<UINT>(inputElements.size()) };
    psoDesc.PrimitiveTopologyType = toTopologyType(desc.pt);
    psoDesc.NumRenderTargets      = desc.rtvCount;
    for (UINT i = 0; i < 8u; ++i)
        psoDesc.RTVFormats[i] = (i < desc.rtvCount) ? ToDxgiFormat(desc.rtvFormats[i]) : DXGI_FORMAT_UNKNOWN;
    psoDesc.DSVFormat             = ToDxgiFormat(desc.dsvFormat);
    const UINT sampleCount = desc.sampleCount ? desc.sampleCount : 1;
    psoDesc.SampleDesc.Count      = sampleCount;
    psoDesc.SampleDesc.Quality    = 0;  // 0 required when Count==1; for MSAA use device CheckFeatureSupport

    // Compute a stable name for this PSO (used by PipelineLibrary cache)
    wchar_t psoName[64] = {};
    ComputePSOName(desc, psoName, 64);
    const uint64_t psoNameHash = hashPsoName(psoName);
    PsoLibNameLease nameLease(m_psoLibrary ? this : nullptr, psoNameHash);

    // Try loading from PSO library (avoids ISA recompilation on warm start)
    bool loadedFromLibrary = false;
    if (m_psoLibrary)
    {
        HRESULT hrLoad = m_psoLibrary->LoadGraphicsPipeline(psoName, &psoDesc,
                                                             IID_PPV_ARGS(&internal.pso));
        if (SUCCEEDED(hrLoad))
            loadedFromLibrary = true;
        // E_INVALIDARG = not in the library (a miss or a bytecode mismatch), E_FAIL = incompatible: both
        // expected on first use; fall through to CreateGraphicsPipelineState. Counted either way: the
        // debug layer reports a miss only at Info level, so this is the only record of how cold the run
        // was.
        if (loadedFromLibrary)
        {
            // Which kind of hit: the library is filled from disk at Init and by this run's own StorePipeline
            // calls, so the stored-name set is the only thing that tells them apart, and `diskHits == 0` makes
            // "-pso_cache_dir really made this run cold" an assertion instead of an assumption. The load
            // path must insert too: on a warm run (nothing compiled, nothing stored) the second ask for a
            // name would otherwise count as a disk hit and sessionDupes could assert nothing.
            // noteLibraryHit counts first, inserts after.
            noteLibraryHit(psoNameHash);
        }
        else
        {
            ++m_psoLibMisses;
            PipelineStats::psoLibMisses.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (!loadedFromLibrary && desc.libraryOnly) return false;

    uint32_t buildUs = 0;
    if (!loadedFromLibrary)
    {
        HRESULT hr;
        {
            PROF_SCOPE("PSO.CreateGraphics");
            const auto t0 = std::chrono::steady_clock::now();
            hr = m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&internal.pso));
            buildUs = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0).count());
        }
        if (FAILED(hr))
        {
            LOG_ERROR("CreatePipelineState: CreateGraphicsPipelineState failed, HRESULT=0x%08X (see D3D12 docs / WinError.h)", static_cast<unsigned>(hr));
            return false;
        }
        PipelineStats::psoCreates.fetch_add(1, std::memory_order_relaxed);
        // Only a compile that followed a library consultation, so `misses == compiled` holds exactly when
        // the library was present for every graphics PSO this run built.
        if (m_psoLibrary) ++m_psoLibCompiled;

        // Store in the library for the next warm start, recorded whether or not the store succeeded:
        // E_INVALIDARG ("the library already holds that name") makes a later Load of it a session dupe
        // as surely as a successful store. The warning is throttled.
        if (m_psoLibrary)
            noteLibraryStore(psoNameHash, m_psoLibrary->StorePipeline(psoName, internal.pso.Get()));
    }
    if (Gfx::PsoManifest::RecordingEnabled())
    {
        const D3D12_SHADER_BYTECODE stages[Gfx::PsoManifest::kStageCount] = {
            psoDesc.VS, psoDesc.PS, psoDesc.GS, psoDesc.HS, psoDesc.DS, {}, {}, {} };
        const Gfx::PsoManifest::State state = Gfx::PsoManifest::StateOf(psoDesc);
        RecordPipeline(Gfx::PsoManifest::Kind::Graphics, psoName, psoDesc.pRootSignature, stages, &state, buildUs);
    }

    nameLease.Release();
    return PublishPipelineState(std::move(internal), outPSO, "graphics");
}

// ==== Destroy buffers / textures ====

void GraphicsDX12::DestroyBuffer(RHI::GPUBuffer& buffer)
{
    if (!buffer.IsValid()) return;
    const uint32_t id = buffer.handleId;
    // An id the pool never handed out: indexing the pool with it reads past its end.
    if (!m_lifetimeGuardsOff &&
        !ENGINE_CHECK_ONCE(id < m_bufferPool.size(),
                           "DestroyBuffer: buffer %u is outside the pool (%zu ids) - refused",
                           id, m_bufferPool.size()))
    {
        m_destroyRefusals.fetch_add(1, std::memory_order_relaxed);
        buffer.Reset();
        return;
    }
    if (std::this_thread::get_id() == m_deviceFrameThread.load(std::memory_order_acquire))
        DrainFrameIfNeeded(Diag::DrainCause::ResourceOp);
    auto& entry = m_bufferPool[id];
    // Queue resource + descriptors for release once the GPU is done with this
    // backbuffer slot (kFrameCount frames from now, processed in BeginFrame).
    std::lock_guard<std::mutex> lock(m_deferredMutex);
    // Destroyed already (through another copy of the handle): the first Destroy queued the id, and a
    // second would put it on the free list twice, for two buffers to share. Checked under the lock so
    // two racing Destroys of one buffer queue it once.
    if (!m_lifetimeGuardsOff &&
        !ENGINE_CHECK_ONCE(entry.resource.Get() != nullptr,
                           "DestroyBuffer: buffer %u was destroyed already - refused", id))
    {
        m_destroyRefusals.fetch_add(1, std::memory_order_relaxed);
        buffer.Reset();
        return;
    }
    auto& dr = PendingReleaseBucket();
    if (entry.cbv.IsValid()) { dr.descriptors.push_back(entry.cbv); entry.cbv = {}; }
    if (entry.srv.IsValid()) { dr.descriptors.push_back(entry.srv); entry.srv = {}; }
    // The UAV too: CreateBuffer allocates one for every UNORDERED_ACCESS buffer, and a missed one
    // leaks a CBV_SRV_UAV slot for the process lifetime.
    if (entry.uav.IsValid()) { dr.descriptors.push_back(entry.uav); entry.uav = {}; }
    if (entry.clearUav.IsValid()) { dr.descriptors.push_back(entry.clearUav); entry.clearUav = {}; }
    if (entry.resource)      { dr.resources.push_back(std::move(entry.resource)); }
    // Defer the backing heap too (if suballocated). DeferredReleaseList declares heaps before
    // resources, so on drain the placed resource releases before its heap, as D3D12 requires.
    // Buffers sharing one heap each push a reference; the heap frees with the last one.
    if (entry.placedHeap)    { dr.heaps.push_back(std::move(entry.placedHeap)); }
    dr.bufferSlots.push_back(id);
    buffer.Reset();
}

void GraphicsDX12::DestroyTexture(RHI::Texture& texture)
{
    if (!texture.IsValid()) return;
    // An HDR slot's record belongs to the slot and DestroyHdrTarget gives it back; releasing it here
    // would leave the slot on a freed id.
    for (uint32_t slot = 0; slot < kMaxHdrTargets; ++slot)
        if (!ENGINE_CHECK_ONCE(m_hdrTargets[slot].tex.handleId != texture.handleId,
                               "DestroyTexture: texture %u is HDR slot %u's target - refused "
                               "(DestroyHdrTarget owns it)", texture.handleId, slot))
            return;
    const uint32_t id = texture.handleId;
    // An id the pool never handed out: indexing the pool with it reads past its end.
    if (!m_lifetimeGuardsOff &&
        !ENGINE_CHECK_ONCE(id < m_texturePool.size(),
                           "DestroyTexture: texture %u is outside the pool (%zu ids) - refused",
                           id, m_texturePool.size()))
    {
        m_destroyRefusals.fetch_add(1, std::memory_order_relaxed);
        texture.Reset();
        return;
    }
    if (std::this_thread::get_id() == m_deviceFrameThread.load(std::memory_order_acquire))
        DrainFrameIfNeeded(Diag::DrainCause::ResourceOp);
    auto& entry = m_texturePool[id];
    // A stream target's queued uploads that no frame has taken yet go with it; one being recorded
    // right now keeps its own reference to the resource.
    if (entry.streamTarget) StreamUploaderForget(id, entry.generation);
    // Queue everything for deferred release (GPU may still be reading this
    // texture / its descriptors for up to kFrameCount frames).
    std::lock_guard<std::mutex> lock(m_deferredMutex);
    // Destroyed already (through another copy of the handle): the first Destroy queued the id, and a
    // second would put it on the free list twice, for two textures to share. Checked under the lock
    // so two racing Destroys of one texture queue it once.
    if (!m_lifetimeGuardsOff &&
        !ENGINE_CHECK_ONCE(entry.resource.Get() != nullptr,
                           "DestroyTexture: texture %u was destroyed already - refused", id))
    {
        m_destroyRefusals.fetch_add(1, std::memory_order_relaxed);
        texture.Reset();
        return;
    }
    auto& dr = PendingReleaseBucket();
    auto steal = [&](DescriptorAllocation& a) {
        if (a.IsValid()) { dr.descriptors.push_back(a); a = {}; }
    };
    steal(entry.rtv);
    steal(entry.dsv);
    for (auto& a : entry.dsvReadOnly) steal(a);
    steal(entry.srv);
    steal(entry.uav);
    steal(entry.previewSrv);
    steal(entry.stencilSrv);
    steal(entry.uvPlaneSrv);
    for (auto& a : entry.mipUavs)   steal(a);
    for (auto& a : entry.sliceDsvs) steal(a);
    for (auto& kv : entry.cubeFaceRtvs) steal(kv.second);
    for (auto& kv : entry.cubeMipUavs)  steal(kv.second);
    entry.mipUavs.clear();
    entry.sliceDsvs.clear();
    entry.cubeFaceRtvs.clear();
    entry.cubeMipUavs.clear();
    if (entry.resource) dr.resources.push_back(std::move(entry.resource));
    // Defer the backing aliasing heap too (if placed). DeferredReleaseList declares heaps before
    // resources, so on drain the placed resource releases before its heap, as D3D12 requires.
    // Textures sharing one heap each push a reference; the heap frees with the last one.
    if (entry.placedHeap) { dr.heaps.push_back(std::move(entry.placedHeap)); }
    dr.textureSlots.push_back(id);
    texture.Reset();
}

// ==== ReplaceTexture: new contents under the same handleId (a reimport) ====
// Consumers cache a texture three ways: its handleId (the bindless index), its SRV's GPU handle
// (GetTextureSRVGpuHandle; runtime materials freeze it once resolved) and the preview alias, so a
// new texture would leave them all on the old resource until it was freed under them. The new
// resource is built as a temporary texture and moved into the live pool entry, whose own
// descriptors (and bindless slot [handleId]) are rewritten to view it; the temporary, now holding
// the old resource, goes through DestroyTexture's frame-stamped release. The GPU must be idle
// (the caller's WaitIdleAndReleaseDeferred): descriptors in-flight frames read are rewritten.
bool GraphicsDX12::ReplaceTexture(RHI::Texture& texture, const RHI::TextureDesc& desc,
                                  const RHI::SubresourceData* initialData)
{
    if (!texture.IsValid() || texture.handleId >= static_cast<uint32_t>(m_texturePool.size()))
        return false;
    if (desc.bindFlags != RHI::BindFlag::SHADER_RESOURCE || desc.format == RHI::Format::NV12)
    {
        LOG_ERROR("ReplaceTexture: only a shader-resource-only, non-planar texture can be replaced");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_resourceMutex);
        const TextureDX12& live = m_texturePool[texture.handleId];
        // Queued uploads hold the current resource, so their copies would land in the one this replaces
        // and be lost.
        if (!ENGINE_CHECK_ONCE(!live.streamTarget,
                               "ReplaceTexture: texture %u is a streaming upload target - refused",
                               texture.handleId))
            return false;
        if (!live.resource || !live.srv.IsValid() || live.rtv.IsValid() || live.dsv.IsValid() ||
            live.uav.IsValid() || live.stencilSrv.IsValid() || live.uvPlaneSrv.IsValid() ||
            live.placedHeap || !live.mipUavs.empty())
        {
            LOG_ERROR("ReplaceTexture: texture %u has views other than its SRV (or is placed) - refused",
                      texture.handleId);
            return false;
        }
    }

    RHI::Texture fresh;
    if (!CreateTexture(desc, fresh, initialData))
        return false;

    {
        std::lock_guard<std::mutex> lock(m_resourceMutex);
        TextureDX12& live = m_texturePool[texture.handleId];
        TextureDX12& tmp  = m_texturePool[fresh.handleId];
        std::swap(live.resource, tmp.resource);
        std::swap(live.state, tmp.state);

        // CPU-to-CPU copy (both staging descriptors are CPU-readable), then the
        // live allocation's own shader-visible mirror.
        auto rewrite = [&](DescriptorAllocation& dst, const DescriptorAllocation& src)
        {
            m_cbvSrvUavAllocator.CopyDescriptors(1, dst.GetCpuHandle(), src.GetCpuHandle());
            dst.CopyToGpu();
        };
        rewrite(live.srv, tmp.srv);
        if (live.previewSrv.IsValid())
            rewrite(live.previewSrv, tmp.previewSrv.IsValid() ? tmp.previewSrv : tmp.srv);
        else if (tmp.previewSrv.IsValid())
            std::swap(live.previewSrv, tmp.previewSrv);   // the new format is sRGB: its alias moves over

        if (m_bindlessTexTable.IsValid() && texture.handleId < kMaxBindlessTextures)
        {
            const UINT inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            D3D12_CPU_DESCRIPTOR_HANDLE dst = m_bindlessTexTable.GetGpuCpuHandle();
            dst.ptr += static_cast<SIZE_T>(texture.handleId) * inc;
            m_cbvSrvUavAllocator.CopyDescriptors(1, dst, live.srv.GetCpuHandle());
        }
    }

    DestroyTexture(fresh);   // the OLD resource + the temporary views, frame-stamped
    texture.desc = desc;
    return true;
}

// ==== Shader / PSO release: append-only pools, retired slots never recycled ====
// RHI::Shader / RHI::PipelineState are bare index handles with no generation counter, copied by
// value into ~60 pass members, and BindPipelineState has to trust the index: recycling a slot
// would let a stale copy silently bind a different, valid pipeline. So Destroy* releases the
// payload (the DXIL blob / the driver PSO) and leaves the slot alive but empty; a stale handle
// then fails loudly in BindPipelineState instead of aliasing. Slot reuse needs a generation field
// on GPUResource first.

// Several compute pipelines at once (IGraphicsDevice). The lookups and the publication run on
// this thread, in order; only the driver's CreateComputePipelineState runs concurrently: it is
// free-threaded and it is the whole cost (a RayQuery kernel's is a driver compile on every run).
// The jobs run on this thread and a bounded set of short-lived ones (Sys::RunBounded), not the
// task system: a one-off wait on the driver inside a frame's setup, and the frame's workers may be
// what that frame is about to need. Nothing on those threads enters a profiler scope: the
// profiler's thread registry is never recycled. Each job goes through
// the PSO library (CreateComputePSOViaLibrary) and holds at most one name lease at a time, so two
// jobs with one desc serialize and none waits on another while holding a lease.
bool GraphicsDX12::CreatePipelineStates(const RHI::PipelineStateDesc* descs, uint32_t count,
                                        RHI::PipelineState* outs)
{
    if (count == 0) return true;
    if (!descs || !outs) return false;
    if (!m_computeRootSignature)
    { LOG_ERROR("CreatePipelineStates: compute root signature not initialized"); return false; }
    struct Job
    {
        PipelineStateDX12                internal;
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        wchar_t                           psoName[64] = {};
        uint64_t                          nameHash = 0;
        HRESULT                           hr    = E_FAIL;
        bool                              ready = false;
    };
    std::vector<Job> jobs(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        const RHI::PipelineStateDesc& d = descs[i];
        if (!ENGINE_CHECK_ONCE(d.cs && !d.vs, "CreatePipelineStates: compute pipelines only (entry %u)", i))
            continue;
        ComPtr<ID3D12RootSignature> rootSignature = m_computeRootSignature;
        if (d.layout && !LookupPipelineLayout(*d.layout, rootSignature)) continue;
        Job& job = jobs[i];
        if (!LookupShaderBytecode(d.cs, job.desc.CS, nullptr, "CS")) continue;
        job.internal.rootSignature = rootSignature;
        job.internal.isCompute     = true;
        job.desc.pRootSignature    = rootSignature.Get();
        if (m_psoLibrary)
        {
            ComputePSOName(d, job.psoName, 64);   // on this thread: it takes m_resourceMutex
            job.nameHash = HashPsoName(job.psoName);
        }
        job.ready                  = true;
    }
    auto create = [this](Job& job)
    {
        if (job.ready)
            (void)CreateComputePSOViaLibrary(job.psoName, job.nameHash, job.desc, job.internal.pso,
                                             /*profile*/false, job.hr);
    };
    Sys::RunBounded(count, Sys::BurstHelpers(), [&](uint32_t i) { create(jobs[i]); });
    bool ok = true;
    for (uint32_t i = 0; i < count; ++i)
    {
        Job& job = jobs[i];
        if (!job.ready) { ok = false; continue; }
        if (FAILED(job.hr))
        {
            LOG_ERROR("CreatePipelineStates: CreateComputePipelineState (entry %u of %u) failed 0x%08X",
                      i, count, static_cast<unsigned>(job.hr));
            ok = false;
            continue;
        }
        // (psoCreates was counted by CreateComputePSOViaLibrary - a library hit is not a create)
        if (!PublishPipelineState(std::move(job.internal), outs[i], "compute")) ok = false;
    }
    return ok;
}

bool GraphicsDX12::LookupShaderBytecode(const RHI::Shader* s, D3D12_SHADER_BYTECODE& out,
                                        uint64_t* outHash, const char* stage)
{
    out = { nullptr, 0 };
    if (outHash) *outHash = 0;
    if (!s || !s->IsValid()) return false;
    // Under the lock: CreateShader push_backs from other workers (and the
    // pool cap only makes that safe for the vector, not for the read). The
    // bytecode buffer itself is heap-stable (a moved std::vector keeps its
    // buffer), so the pointer stays valid after the lock drops; only a
    // DestroyShader (hot reload, GPU flushed, serial) can free it.
    std::lock_guard<std::mutex> lock(m_resourceMutex);
    if (!ENGINE_CHECK_ONCE(s->handleId < m_shaderPool.size(),
            "CreatePipelineState: %s shader handle %u is out of range (pool %zu)",
            stage, s->handleId, m_shaderPool.size()))
        return false;
    const ShaderDX12& p = m_shaderPool[s->handleId];
    if (!ENGINE_CHECK_ONCE(!p.bytecode.empty(),
            "CreatePipelineState: %s shader handle %u was DESTROYED (hot reload) — the "
            "caller must re-fetch it from its ShaderLibrary before rebuilding the PSO",
            stage, s->handleId))
        return false;
    out = { p.bytecode.data(), p.bytecode.size() };
    if (outHash) *outHash = p.bytecodeHash;
    return true;
}

// The per-psoName lease CreatePipelineState holds while it is inside the
// pipeline library (why: the m_psoLibInflight comment in GraphicsDX12.h).
void GraphicsDX12::AcquirePsoLibName(uint64_t psoNameHash)
{
    std::unique_lock<std::mutex> lock(m_psoLibInflightMutex);
    bool waited = false;
    while (!m_psoLibInflight.insert(psoNameHash).second)
    {
        waited = true;
        m_psoLibInflightCv.wait(lock);
    }
    if (waited) m_psoLibNameWaits.fetch_add(1, std::memory_order_relaxed);
}

void GraphicsDX12::ReleasePsoLibName(uint64_t psoNameHash)
{
    {
        std::lock_guard<std::mutex> lock(m_psoLibInflightMutex);
        m_psoLibInflight.erase(psoNameHash);
    }
    m_psoLibInflightCv.notify_all();
}

bool GraphicsDX12::PublishPipelineState(PipelineStateDX12&& internal,
                                        RHI::PipelineState& outPSO, const char* kind)
{
    // The handle the caller passed IN is retired only now, on success: ~60
    // passes rebuild straight onto a live member (`CreatePipelineState(d,
    // m_pso)`), and a failed rebuild must leave them with the OLD, working
    // pipeline rather than an orphaned one. Doing it here covers every such
    // site by construction instead of trusting each ReloadShaders.
    const RHI::PipelineState previous = outPSO;
    bool sayHeadroom = false;
    size_t psoCount  = 0;
    {
        std::lock_guard<std::mutex> lock(m_resourceMutex);
        const uint32_t handle = static_cast<uint32_t>(m_psoPool.size());
        const bool pushed = m_psoPool.push_back(std::make_unique<PipelineStateDX12>(std::move(internal)));
        // outPSO is untouched on failure: the caller keeps the pipeline it had.
        if (!ENGINE_CHECK_ONCE(pushed, "CreatePipelineState: the PSO pool is full (%zu) - %s not created",
                               m_psoPool.kCapacity, kind ? kind : "pipeline"))
            return false;
        outPSO.handleId = handle;
        outPSO.type      = RHI::GPUResource::Type::PipelineState;
        // Warn once at 90% of the soft ceiling. The pool has no practical cap and must not get a low
        // one: growing it keeps the addresses lock-free readers hold stable, and refusing would turn "a
        // lot of PSOs" into "a pass has no pipeline". Like the shader pool, DestroyPipelineState retires
        // its cell rather than recycling it (a bare index handle has no generation), so the count only
        // grows, with hot reloads and preview GraphSet teardowns. kSoftMaxPsoPool is what a pack's `max
        // psos` is measured against; this says the session is heading there while there is still room.
        psoCount    = m_psoPool.size();
        sayHeadroom = !m_psoPoolHeadroomLogged &&
                      psoCount * 10 >= kSoftMaxPsoPool * 9;
        if (sayHeadroom) m_psoPoolHeadroomLogged = true;
    }
    if (sayHeadroom)
        LOG_WARNING("CreatePipelineState: PSO pool at %zu/%zu (90%% of the SOFT "
                    "ceiling). Slots are RETIRED, never recycled, so hot "
                    "reloads and preview graph teardowns ratchet this up for "
                    "the life of the process. Nothing refuses at the ceiling - "
                    "it is the number the corpus bounds `psos=` against - but a "
                    "count that keeps climbing with no new content is a "
                    "per-cycle PSO leak.", psoCount, kSoftMaxPsoPool);
    if (previous.IsValid() && previous.handleId != outPSO.handleId)
    {
        RHI::PipelineState old = previous;
        DestroyPipelineState(old);
    }
    LOG_TRACE("CreatePipelineState (%s): OK", kind);
    return true;
}

void GraphicsDX12::DestroyShader(RHI::Shader& shader)
{
    if (!shader.IsValid()) return;
    {
        std::lock_guard<std::mutex> lock(m_resourceMutex);
        if (shader.handleId < m_shaderPool.size())
        {
            ShaderDX12& entry = m_shaderPool[shader.handleId];
            entry.bytecode.clear();
            entry.bytecode.shrink_to_fit();   // actually give the DXIL back
            // bytecodeHash is kept: ComputePSOName of a stale handle stays stable, and
            // LookupShaderBytecode's empty-bytecode check reports it.
        }
    }
    shader.Reset();
}

void GraphicsDX12::DestroyPipelineState(RHI::PipelineState& pso)
{
    if (!pso.IsValid()) return;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> deadPso;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> deadSig;
    {
        std::lock_guard<std::mutex> lock(m_resourceMutex);
        if (pso.handleId < m_psoPool.size() && m_psoPool[pso.handleId])
        {
            PipelineStateDX12& entry = *m_psoPool[pso.handleId];
            deadPso = std::move(entry.pso);
            deadSig = std::move(entry.rootSignature);
            entry.pso.Reset();
            entry.rootSignature.Reset();
        }
    }
    // Deferred like DestroyTexture: a command list recorded this frame may still reference the
    // pipeline (the hot-reload caller flushes first; the contract is for every other caller). Taken
    // without m_resourceMutex held: the two never nest.
    if (deadPso || deadSig)
    {
        std::lock_guard<std::mutex> lock(m_deferredMutex);
        auto& dr = PendingReleaseBucket();
        if (deadPso) dr.pipelineObjects.push_back(std::move(deadPso));
        if (deadSig) dr.pipelineObjects.push_back(std::move(deadSig));
    }
    pso.Reset();
}

// ==== Pipeline layouts ====

bool GraphicsDX12::SerializePipelineLayout(const RHI::PipelineLayoutDesc& desc, std::vector<uint8_t>& blob,
                                           uint32_t& dwords, std::string& error) const
{
    blob.clear();
    dwords = 0;
    if (desc.paramCount && !desc.params)
    {
        error = "the parameter list is incomplete (a root slot given twice or skipped)";
        return false;
    }
    // Field for field: every member a hand-filled description could set comes
    // from the RHI one and everything else stays zero, so the blob is the one
    // that description serialized to (each migration compares the two).
    std::vector<D3D12_DESCRIPTOR_RANGE> ranges(desc.rangeCount);
    for (uint32_t i = 0; i < desc.rangeCount; ++i)
    {
        const RHI::DescriptorRange& r = desc.ranges[i];
        D3D12_DESCRIPTOR_RANGE&     d = ranges[i];
        d.RangeType = r.type == RHI::DescriptorRange::Type::UAV ? D3D12_DESCRIPTOR_RANGE_TYPE_UAV
                    : r.type == RHI::DescriptorRange::Type::CBV ? D3D12_DESCRIPTOR_RANGE_TYPE_CBV
                                                                : D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        d.NumDescriptors                    = r.count;
        d.BaseShaderRegister                = r.baseRegister;
        d.RegisterSpace                     = r.space;
        d.OffsetInDescriptorsFromTableStart = r.tableOffset;
    }
    auto visibility = [](RHI::ShaderVisibility v)
    {
        switch (v)
        {
        case RHI::ShaderVisibility::VS: return D3D12_SHADER_VISIBILITY_VERTEX;
        case RHI::ShaderVisibility::HS: return D3D12_SHADER_VISIBILITY_HULL;
        case RHI::ShaderVisibility::DS: return D3D12_SHADER_VISIBILITY_DOMAIN;
        case RHI::ShaderVisibility::GS: return D3D12_SHADER_VISIBILITY_GEOMETRY;
        case RHI::ShaderVisibility::PS: return D3D12_SHADER_VISIBILITY_PIXEL;
        case RHI::ShaderVisibility::AS: return D3D12_SHADER_VISIBILITY_AMPLIFICATION;
        case RHI::ShaderVisibility::MS: return D3D12_SHADER_VISIBILITY_MESH;
        default:                        return D3D12_SHADER_VISIBILITY_ALL;
        }
    };
    std::vector<D3D12_ROOT_PARAMETER> params(desc.paramCount);
    for (uint32_t i = 0; i < desc.paramCount; ++i)
    {
        const RHI::LayoutParam& s = desc.params[i];
        D3D12_ROOT_PARAMETER&   d = params[i];
        d.ShaderVisibility = visibility(s.visibility);
        switch (s.kind)
        {
        case RHI::LayoutParam::Kind::CONSTANTS:
            d.ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            d.Constants.ShaderRegister = s.reg;
            d.Constants.RegisterSpace  = s.space;
            d.Constants.Num32BitValues = s.num32BitValues;
            dwords += s.num32BitValues;
            break;
        case RHI::LayoutParam::Kind::CBV:
        case RHI::LayoutParam::Kind::SRV:
        case RHI::LayoutParam::Kind::UAV:
            d.ParameterType = s.kind == RHI::LayoutParam::Kind::CBV ? D3D12_ROOT_PARAMETER_TYPE_CBV
                            : s.kind == RHI::LayoutParam::Kind::SRV ? D3D12_ROOT_PARAMETER_TYPE_SRV
                                                                     : D3D12_ROOT_PARAMETER_TYPE_UAV;
            d.Descriptor.ShaderRegister = s.reg;
            d.Descriptor.RegisterSpace  = s.space;
            dwords += 2;
            break;
        case RHI::LayoutParam::Kind::TABLE:
            if (s.rangeCount == 0 || s.firstRange + s.rangeCount > desc.rangeCount)
            {
                error = "root parameter " + std::to_string(i) + " is a table without ranges";
                return false;
            }
            d.ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            d.DescriptorTable.NumDescriptorRanges = s.rangeCount;
            d.DescriptorTable.pDescriptorRanges   = ranges.data() + s.firstRange;
            dwords += 1;
            break;
        }
    }
    if (dwords > 64)
    {
        error = std::to_string(dwords) + " DWORDs (a root signature holds 64)";
        return false;
    }
    std::vector<D3D12_STATIC_SAMPLER_DESC> samplers(desc.samplerCount);
    for (uint32_t i = 0; i < desc.samplerCount; ++i)
    {
        const RHI::StaticSampler&  s = desc.samplers[i];
        D3D12_STATIC_SAMPLER_DESC& d = samplers[i];
        d.Filter           = ToD3D12Filter(s.desc.filter);
        d.AddressU         = ToD3D12AddressMode(s.desc.addressU);
        d.AddressV         = ToD3D12AddressMode(s.desc.addressV);
        d.AddressW         = ToD3D12AddressMode(s.desc.addressW);
        d.MipLODBias       = s.desc.mipLodBias;
        d.MaxAnisotropy    = s.desc.maxAnisotropy;
        d.ComparisonFunc   = ToD3D12ComparisonFunc(s.desc.comparisonFunc);
        d.BorderColor      = s.desc.borderColor == RHI::SamplerBorderColor::OPAQUE_BLACK
                                 ? D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK
                           : s.desc.borderColor == RHI::SamplerBorderColor::OPAQUE_WHITE
                                 ? D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE
                                 : D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        d.MinLOD           = s.desc.minLod;
        d.MaxLOD           = s.desc.maxLod;
        d.ShaderRegister   = s.reg;
        d.RegisterSpace    = s.space;
        d.ShaderVisibility = visibility(s.visibility);
    }
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters     = desc.paramCount;
    rsd.pParameters       = params.empty() ? nullptr : params.data();
    rsd.NumStaticSamplers = desc.samplerCount;
    rsd.pStaticSamplers   = samplers.empty() ? nullptr : samplers.data();
    if (RHI::HasFlag(desc.flags, RHI::LayoutFlags::INPUT_ASSEMBLER))
        rsd.Flags |= D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    if (RHI::HasFlag(desc.flags, RHI::LayoutFlags::HEAP_INDEXED_RESOURCES))
        rsd.Flags |= D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
    if (RHI::HasFlag(desc.flags, RHI::LayoutFlags::HEAP_INDEXED_SAMPLERS))
        rsd.Flags |= D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;
    Microsoft::WRL::ComPtr<ID3DBlob> serialized, errors;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)))
    {
        error = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize())
                       : std::string("D3D12SerializeRootSignature failed");
        return false;
    }
    const auto* bytes = static_cast<const uint8_t*>(serialized->GetBufferPointer());
    blob.assign(bytes, bytes + serialized->GetBufferSize());
    return true;
}

bool GraphicsDX12::CreatePipelineLayout(const RHI::PipelineLayoutDesc& desc, RHI::PipelineLayout& outLayout)
{
    const char* name = desc.name && desc.name[0] ? desc.name : "(unnamed)";
    std::vector<uint8_t> blob;
    uint32_t             dwords = 0;
    std::string          error;
    if (!SerializePipelineLayout(desc, blob, dwords, error))
    {
        LOG_ERROR("CreatePipelineLayout '%s': %s", name, error.c_str());
        return false;
    }
    PipelineLayoutDX12 entry;
    if (FAILED(m_device->CreateRootSignature(0, blob.data(), blob.size(), IID_PPV_ARGS(&entry.rootSignature))))
    {
        LOG_ERROR("CreatePipelineLayout '%s': CreateRootSignature failed", name);
        return false;
    }
    NoteRootSignature(entry.rootSignature.Get(), blob.data(), blob.size());
    uint64_t hash = 14695981039346656037ull;   // FNV-1a: the [PIPELINE_LAYOUT] line's identity of the blob
    for (uint8_t b : blob) { hash ^= b; hash *= 1099511628211ull; }
    entry.name      = name;
    entry.blobHash  = hash;
    entry.blobBytes = static_cast<uint32_t>(blob.size());
    entry.dwords    = dwords;
    const int wideLen = MultiByteToWideChar(CP_UTF8, 0, name, -1, nullptr, 0);
    if (wideLen > 0)
    {
        std::wstring wname(static_cast<size_t>(wideLen - 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, name, -1, wname.data(), wideLen);
        entry.rootSignature->SetName(wname.c_str());
    }
    // As PublishPipelineState: the handle passed in is retired only now, on
    // success, so a failed rebuild leaves the caller its working layout.
    const RHI::PipelineLayout previous = outLayout;
    {
        std::lock_guard<std::mutex> lock(m_resourceMutex);
        outLayout.handleId = static_cast<uint32_t>(m_layoutPool.size());
        outLayout.type      = RHI::GPUResource::Type::PipelineLayout;
        m_layoutPool.push_back(std::move(entry));
    }
    if (previous.IsValid() && previous.handleId != outLayout.handleId)
    {
        RHI::PipelineLayout old = previous;
        DestroyPipelineLayout(old);
    }
    LOG_INFO("[PIPELINE_LAYOUT] %s params=%u dwords=%u samplers=%u bytes=%zu blob=%016llx", name, desc.paramCount, dwords,
             desc.samplerCount, blob.size(), static_cast<unsigned long long>(hash));
    return true;
}

void GraphicsDX12::DestroyPipelineLayout(RHI::PipelineLayout& layout)
{
    if (!layout.IsValid()) return;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> dead;
    {
        std::lock_guard<std::mutex> lock(m_resourceMutex);
        if (layout.handleId < m_layoutPool.size())
            dead = std::move(m_layoutPool[layout.handleId].rootSignature);
    }
    // Deferred like DestroyPipelineState: a list recorded this frame may still set it.
    if (dead)
    {
        std::lock_guard<std::mutex> lock(m_deferredMutex);
        PendingReleaseBucket().pipelineObjects.push_back(std::move(dead));
    }
    layout.Reset();
}

bool GraphicsDX12::LookupPipelineLayout(const RHI::PipelineLayout& layout,
                                        Microsoft::WRL::ComPtr<ID3D12RootSignature>& out)
{
    std::lock_guard<std::mutex> lock(m_resourceMutex);
    if (!ENGINE_CHECK_ONCE(layout.IsValid() && layout.handleId < m_layoutPool.size() &&
                           m_layoutPool[layout.handleId].rootSignature,
                           "CreatePipelineState: pipeline layout handle %u is invalid, destroyed or out of range "
                           "(pool %zu)", layout.handleId, m_layoutPool.size()))
        return false;
    out = m_layoutPool[layout.handleId].rootSignature;
    return true;
}
