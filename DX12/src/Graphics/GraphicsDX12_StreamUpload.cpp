// GraphicsDX12_StreamUpload.cpp - the GPU half of the streaming texture upload.
//
// QueueTextureUpload copies the caller's rows into the upload slot's persistently mapped UPLOAD
// ring and returns, instead of UpdateTexture's synchronous FlushUploadAndWait (a CPU wait behind
// the graphics queue's whole backlog: about one GPU frame when GPU-bound). The render half records
// every queued copy into ONE "StreamUpload" command list at the start of RenderAsync. That list is
// begun before any other list of the frame and depends on nothing, and EndFrame's FIFO Kahn sort
// submits dependency-free lists first in pool order, so the copies execute before every reader of
// the frame (the graph's UI passes, the preview, the primary list's ImGui).
// StreamUploaderEndFrame checks that order every frame.
//
// When, where and in what order (the frame window, carry-over, the ring, merging, request order,
// lost frames, the CPU copies' cap) is Graphics/StreamUploadQueue.h's job, with its unit test;
// this file owns the D3D12 objects and writes the bytes where the queue says. Each item holds its
// own reference to the destination (StreamCopy); TextureDX12::generation keeps a reused texture
// id out of an older texture's merges.
//
// Threads: QueueTextureUpload on the game half (refused on the render half's thread and inside a
// device-execution scope); StreamUploaderOpenSlot on whichever thread acquires the upload slot;
// RecordPendingStreamUploads and StreamUploaderEndFrame on the render half. m_stream.mutex guards
// the queue; the list a Take hands out is touched again only by the next Open of the same slot,
// which cannot happen before that slot's GPU work retired, so recording reads it unlocked.

#include "Graphics/GraphicsDX12.h"
#include "Graphics/GraphicsDX12Internal.h"
#include "Graphics/GpuMemoryLedger.h"
#include "Graphics/UploadStats.h"
#include "Resource/EngineDevSettings.h"   // PeekEngineDevStreamRingMiB
#include "System/Check.h"
#include "System/CommandLine.h"
#include "System/Log.h"
#include "System/TaskSystem.h"      // OnWorkerThread: a render pass's recording worker
#include "System/ThreadContext.h"
#include "d3dx12.h"

#include <cstring>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr uint64_t kDefaultRingMiB = 8;     // 8 MiB per slot
    constexpr uint64_t kMaxRingMiB     = 512;
    constexpr uint64_t kMinRetainCap   = 16ull * 1024 * 1024;   // the CPU copies' cap: 4 rings, at least this

    bool IsPlanar(DXGI_FORMAT f) { return f == DXGI_FORMAT_NV12 || f == DXGI_FORMAT_P010; }

    bool IsBlockCompressed(DXGI_FORMAT f)
    {
        return (f >= DXGI_FORMAT_BC1_TYPELESS && f <= DXGI_FORMAT_BC5_SNORM) ||
               (f >= DXGI_FORMAT_BC6H_TYPELESS && f <= DXGI_FORMAT_BC7_UNORM_SRGB);
    }

    uint64_t AlignUp(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

    // The worse of two answers, for a request split into planes: the caller
    // learns the least that happened to any of them.
    StreamUpload::Result Worse(StreamUpload::Result a, StreamUpload::Result b)
    {
        auto rank = [](StreamUpload::Result r)
        {
            switch (r)
            {
            case StreamUpload::Result::Queued:      return 0;
            case StreamUpload::Result::Replaced:    return 1;
            case StreamUpload::Result::CarriedOver: return 2;
            case StreamUpload::Result::Fallback:    return 3;
            case StreamUpload::Result::Dropped:     return 4;
            case StreamUpload::Result::Rejected:    return 5;
            }
            return 5;
        };
        return rank(b) > rank(a) ? b : a;
    }

    // @p rows rows of @p rowBytes each, from @p src (@p srcPitch apart) to
    // @p dst (@p dstPitch apart).
    void CopyRows(uint8_t* dst, uint64_t dstPitch, const uint8_t* src, uint64_t srcPitch,
                  uint32_t rows, uint64_t rowBytes)
    {
        for (uint32_t r = 0; r < rows; ++r)
            std::memcpy(dst + r * dstPitch, src + r * srcPitch, static_cast<size_t>(rowBytes));
    }
}

// ==== Lifetime ====
bool GraphicsDX12::InitStreamUploaderLocked()
{
    if (m_stream.initialized) return true;
    if (m_stream.initFailed || !m_device) return false;

    // engine.json's upload.streamRingMiB (or its -set:), read now: the rings
    // are made at the first QueueTextureUpload, long after the boot load.
    uint64_t mib = kDefaultRingMiB;
    if (const int peek = Resource::PeekEngineDevStreamRingMiB(); peek > 0)
        mib = static_cast<uint64_t>(peek);
    if (const int flag = Sys::CommandLine::Get().GetInt("-streamupload_ring_mib", 0); flag > 0)
        mib = static_cast<uint64_t>(flag);
    if (mib > kMaxRingMiB) mib = kMaxRingMiB;
    uint64_t bytes = mib * 1024ull * 1024ull;
#if defined(WITH_HARNESS) && WITH_HARNESS
    // A ring too small for the probe's frame (the texture_stream_upload_fallback
    // pack): MustDeliver then goes through staging every frame.
    if (const int kib = Sys::CommandLine::Get().GetInt("-streamupload_ring_kib", 0); kib > 0)
        bytes = (std::min)(static_cast<uint64_t>(kib) * 1024ull, kMaxRingMiB * 1024ull * 1024ull);
#endif

    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_UPLOAD };
    const auto rd = CD3DX12_RESOURCE_DESC::Buffer(bytes);
    for (uint32_t s = 0; s < kFrameCount; ++s)
    {
        // CREATE_NOT_ZEROED: every byte a copy reads was written by
        // QueueTextureUpload first.
        const HRESULT hr = m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_CREATE_NOT_ZEROED, &rd,
                                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                             IID_PPV_ARGS(&m_stream.ring[s]));
        if (FAILED(hr))
        {
            LOG_HRESULT("CreateCommittedResource(StreamUploadRing)", hr);
            LOG_ERROR("[UPLOAD_STREAM] ring %u (%llu KiB) could not be created; QueueTextureUpload "
                      "falls back to Rejected for this run", s, static_cast<unsigned long long>(bytes / 1024));
            for (auto& r : m_stream.ring) r.Reset();
            m_stream.initFailed = true;
            return false;
        }
        m_stream.ring[s]->SetName(L"StreamUploadRing");
        GpuLedger::TrackCommitted(m_device.Get(), m_stream.ring[s].Get(), Diag::Prof::GpuMemCat::Upload,
                                  "StreamUploadRing");
        D3D12_RANGE none{ 0, 0 };   // write-only: the CPU never reads it back
        void* p = nullptr;
        ThrowIfFailed(m_stream.ring[s]->Map(0, &none, &p));
        m_stream.mapped[s] = static_cast<uint8_t*>(p);
    }
    // The CPU copies' cap: carried-over requests, and MustDeliver ones until submitted. A normal
    // frame holds at most about a ring's worth.
    const uint64_t retainCap = (std::max)(kMinRetainCap, 4 * bytes);
    m_stream.queue.Init(kFrameCount, bytes, retainCap);
    m_stream.ringBytes = bytes;
    m_stream.initialized = true;
#if defined(WITH_HARNESS) && WITH_HARNESS
    // Reverse arm: record the copies into the PRIMARY list, which is submitted last, so every
    // reader of the frame sees the previous frame's data. The probe must measure that as mismatches
    // (-streamupload_probe_primary, which also arms it), the font pack as a glyph missing from its
    // first frame (-streamupload_primary).
    m_stream.recordIntoPrimary = Sys::CommandLine::Get().Has("-streamupload_probe_primary") ||
                                 Sys::CommandLine::Get().Has("-streamupload_primary");
    if (m_stream.recordIntoPrimary)
        LOG_WARNING("[UPLOAD_STREAM] queued copies are recorded into the PRIMARY list (submitted last) - "
                    "the reverse arm; readers see the data a frame late");
    // The order check's reverse arm: an empty graphics list begun before the
    // StreamUpload list, so it is submitted first - EndFrame must say so.
    m_stream.orderBreak = Sys::CommandLine::Get().Has("-streamupload_probe_orderbreak");
    if (m_stream.orderBreak)
        LOG_WARNING("[UPLOAD_STREAM] -streamupload_probe_orderbreak: a graphics list is begun before the "
                    "StreamUpload list - the order check's reverse arm");
#endif
    LOG_INFO("[UPLOAD_STREAM] ring %llu KiB x %u slots (persistently mapped UPLOAD), CPU copies capped at %llu KiB",
             static_cast<unsigned long long>(bytes / 1024), static_cast<unsigned>(kFrameCount),
             static_cast<unsigned long long>(retainCap / 1024));
    StreamUploaderPublishLocked();
    return true;
}

void GraphicsDX12::ShutdownStreamUploader()
{
    std::lock_guard<std::mutex> lock(m_stream.mutex);
    if (!m_stream.initialized) return;
    const StreamUpload::Counters& c = m_stream.queue.Stats();
    LOG_INFO("[UPLOAD_STREAM] summary requests=%llu bytes=%llu queued=%llu replaced=%llu carriedOver=%llu "
             "fallback=%llu dropped=%llu rejected=%llu stale=%llu requeued=%llu lostDropped=%llu lists=%llu "
             "copies=%llu ringHighWater=%llu ringBytes=%llu retainedHighWater=%llu retainCap=%llu capped=%llu "
             "takeWaits=%llu takeWaitMaxUs=%.1f takeWaitTotalMs=%.3f serialized=%llu",
             static_cast<unsigned long long>(c.requests), static_cast<unsigned long long>(c.bytes),
             static_cast<unsigned long long>(c.queued), static_cast<unsigned long long>(c.replaced),
             static_cast<unsigned long long>(c.carriedOver), static_cast<unsigned long long>(c.fallback),
             static_cast<unsigned long long>(c.dropped), static_cast<unsigned long long>(c.rejected),
             static_cast<unsigned long long>(m_stream.stale + c.forgotten), static_cast<unsigned long long>(c.requeued),
             static_cast<unsigned long long>(c.lostDropped),
             static_cast<unsigned long long>(m_stream.recordedLists),
             static_cast<unsigned long long>(m_stream.recordedCopies),
             static_cast<unsigned long long>(m_stream.queue.RingHighWater()),
             static_cast<unsigned long long>(m_stream.ringBytes),
             static_cast<unsigned long long>(m_stream.queue.RetainedHighWater()),
             static_cast<unsigned long long>(m_stream.queue.RetainCap()),
             static_cast<unsigned long long>(c.capped),
             static_cast<unsigned long long>(m_stream.takeWaits),
             static_cast<double>(m_stream.takeWaitMaxNs) / 1000.0,
             static_cast<double>(m_stream.takeWaitNs) / 1.0e6,
             static_cast<unsigned long long>(m_stream.serialized));
    for (uint32_t s = 0; s < kFrameCount; ++s)
    {
        if (m_stream.ring[s] && m_stream.mapped[s]) m_stream.ring[s]->Unmap(0, nullptr);
        m_stream.mapped[s] = nullptr;
        m_stream.ring[s].Reset();
    }
    m_stream.queue.Init(kFrameCount, 0);   // drops every item, staging and all
    m_stream.initialized = false;
}

// ==== Stats ====
void GraphicsDX12::StreamUploaderPublishLocked() const
{
    const StreamUpload::Counters& c = m_stream.queue.Stats();
    UploadStats::streamRequests.store(c.requests, std::memory_order_relaxed);
    UploadStats::streamBytes.store(c.bytes, std::memory_order_relaxed);
    UploadStats::streamReplaced.store(c.replaced, std::memory_order_relaxed);
    UploadStats::streamCarriedOver.store(c.carriedOver, std::memory_order_relaxed);
    UploadStats::streamFallback.store(c.fallback, std::memory_order_relaxed);
    UploadStats::streamDropped.store(c.dropped + c.lostDropped, std::memory_order_relaxed);
    UploadStats::streamRejected.store(c.rejected, std::memory_order_relaxed);
    UploadStats::streamStale.store(m_stream.stale + c.forgotten, std::memory_order_relaxed);
    UploadStats::streamCopies.store(m_stream.recordedCopies, std::memory_order_relaxed);
    UploadStats::streamRingHighWater.store(m_stream.queue.RingHighWater(), std::memory_order_relaxed);
}

StreamUpload::Stats GraphicsDX12::GetStreamUploadStats() const
{
    std::lock_guard<std::mutex> lock(m_stream.mutex);
    StreamUpload::Stats s;
    s.active = m_stream.initialized;
    if (!m_stream.initialized) return s;
    const StreamUpload::Counters& c = m_stream.queue.Stats();
    s.ringBytesPerSlot = m_stream.ringBytes;
    s.slots          = kFrameCount;
    s.requests       = c.requests;
    s.bytes          = c.bytes;
    s.queued         = c.queued;
    s.replaced       = c.replaced;
    s.carriedOver    = c.carriedOver;
    s.fallback       = c.fallback;
    s.dropped        = c.dropped;
    s.rejected       = c.rejected;
    s.stale          = m_stream.stale + c.forgotten;
    s.requeued       = c.requeued;
    s.lostDropped    = c.lostDropped;
    s.recordedLists  = m_stream.recordedLists;
    s.recordedCopies = m_stream.recordedCopies;
    s.ringHighWater  = m_stream.queue.RingHighWater();
    s.retainedBytes     = m_stream.queue.RetainedBytes();
    s.retainedHighWater = m_stream.queue.RetainedHighWater();
    s.retainCap         = m_stream.queue.RetainCap();
    s.capped            = c.capped;
    return s;
}

// ==== The game half: queue ====
GraphicsDX12::StreamRows GraphicsDX12::StreamRowsOf(const StreamUpload::Item<StreamCopy>& it)
{
    // The item's own copy is already in the copy's layout: whole rows of RowPitch.
    StreamRows r;
    r.data     = it.bytes.data();
    r.pitch    = it.payload.footprint.RowPitch;
    r.rowBytes = it.payload.footprint.RowPitch;
    r.rows     = r.pitch ? static_cast<uint32_t>(it.size / r.pitch) : 0;
    return r;
}

bool GraphicsDX12::StreamUploaderPlaceBytesLocked(StreamUpload::Item<StreamCopy>& it, uint32_t slot, uint64_t offset,
                                                  bool needsStaging, const StreamRows& src)
{
    if (!src.data || src.rows == 0) return false;
    const uint64_t pitch = it.payload.footprint.RowPitch;
    if (!needsStaging)
    {
        CopyRows(m_stream.mapped[slot] + offset, pitch, src.data, src.pitch, src.rows, src.rowBytes);
        it.payload.staging.Reset();
        return true;
    }
    // Fallback: a one-off committed UPLOAD buffer. It travels with the item into the slot's
    // recorded list and is released only when that slot is acquired again (Queue::Open's
    // `released`), after its GPU work retired - never through the device's frame-keyed release queue.
    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_UPLOAD };
    const auto rd = CD3DX12_RESOURCE_DESC::Buffer(it.size);
    ComPtr<ID3D12Resource> staging;
    HRESULT hr = m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_CREATE_NOT_ZEROED, &rd,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&staging));
    if (FAILED(hr))
    {
        LOG_HRESULT("CreateCommittedResource(StreamUpload.Staging)", hr);
        return false;
    }
    GpuLedger::TrackCommitted(m_device.Get(), staging.Get(), Diag::Prof::GpuMemCat::Upload,
                              "StreamUpload.Staging");
    D3D12_RANGE none{ 0, 0 };
    void* p = nullptr;
    hr = staging->Map(0, &none, &p);
    if (FAILED(hr))
    {
        LOG_HRESULT("Map(StreamUpload.Staging)", hr);
        return false;
    }
    CopyRows(static_cast<uint8_t*>(p), pitch, src.data, src.pitch, src.rows, src.rowBytes);
    staging->Unmap(0, nullptr);
    it.payload.staging = std::move(staging);
    return true;
}

StreamUpload::Result GraphicsDX12::StreamUploaderReject(const char* why, uint32_t id)
{
    // The first few, then only the counter: a caller on the wrong thread would
    // otherwise log every frame.
    static std::atomic<uint32_t> s_logged{ 0 };
    if (s_logged.fetch_add(1, std::memory_order_relaxed) < 8)
        LOG_WARNING("[UPLOAD_STREAM] QueueTextureUpload rejected (texture %s%u): %s",
                    id & 0x80000000u ? "native " : "", id & 0x7FFFFFFFu, why);
    std::lock_guard<std::mutex> lock(m_stream.mutex);
    m_stream.queue.CountRejected();
    StreamUploaderPublishLocked();
    return StreamUpload::Result::Rejected;
}

// Refused: the render half while it records a frame, since a request there has no frame left to
// land in that it could know about - the device's frame thread (the render-owner lane, or main
// under -renderowneroff and at boot) between RecordPendingStreamUploads and EndFrame, and the
// TaskSystem workers its passes record on, in the Render context - and anyone inside a
// device-execution scope (the -renderowneroff Editor's UI build). The editor's UI build is in the
// Render context too, but runs before its frame records (on main beside an idle lane, or on the
// device thread ahead of the render half at boot), so inside the upload window: its ImGui
// texture updates land in that frame.
// Not refused, but wrong: Render-context work on the lane before RecordPendingStreamUploads
// (BeginFrame, Renderer::BeginFrame) under frame overlap, where main may already have opened the
// next frame: a request there lands one frame late. Nothing calls from there; never queue
// uploads from the render half.
const char* GraphicsDX12::StreamUploadCallRefused() const
{
    if (m_deviceExecution.Owns())
        return "called inside a device-execution scope (the -renderowneroff Editor's UI build)";
    if (Sys::ThreadCtx::Current() == Sys::ThreadCtx::Render &&
        (TaskSystem::OnWorkerThread() ||
         // the thread test first: only the frame thread reads its own flag
         (std::this_thread::get_id() == m_deviceFrameThread.load(std::memory_order_acquire) &&
          m_stream.recording)))
        return "called from the render half while it records (a game-half system or tick queues uploads)";
    return nullptr;
}

StreamUpload::Result GraphicsDX12::QueueTextureUpload(const RHI::Texture& dst, const StreamUpload::Desc& d)
{
    if (const char* why = StreamUploadCallRefused())
        return StreamUploaderReject(why, dst.handleId);
    if (!dst.IsValid() || dst.handleId >= m_texturePool.size())
        return StreamUploaderReject("not a texture", dst.handleId);
    TextureDX12& entry = m_texturePool[dst.handleId];
    if (!entry.resource)
        return StreamUploaderReject("the texture has no resource", dst.handleId);
    return QueueStreamCopies(entry.resource, entry.state, dst.handleId, entry.generation, d, &entry.streamTarget);
}

// A resource the pool does not own (the ImGui backend's textures). The key keeps it apart from
// every pool texture (the high bit, then the pointer) and from every other native resource; the
// copy holds its own reference.
StreamUpload::Result GraphicsDX12::QueueNativeTextureUpload(ID3D12Resource* dst, D3D12_RESOURCE_STATES state,
                                                            const StreamUpload::Desc& d)
{
    const uint64_t p = reinterpret_cast<uintptr_t>(dst) >> 4;
    const uint32_t keyTexture    = 0x80000000u | static_cast<uint32_t>(p & 0x7FFFFFFFu);
    const uint32_t keyGeneration = static_cast<uint32_t>(p >> 31);
    if (const char* why = StreamUploadCallRefused())
        return StreamUploaderReject(why, keyTexture);
    if (!dst)
        return StreamUploaderReject("no resource", keyTexture);
    return QueueStreamCopies(ComPtr<ID3D12Resource>(dst), state, keyTexture, keyGeneration, d, nullptr);
}

StreamUpload::Result GraphicsDX12::QueueStreamCopies(const ComPtr<ID3D12Resource>& res, D3D12_RESOURCE_STATES state,
                                                     uint32_t keyTexture, uint32_t keyGeneration,
                                                     const StreamUpload::Desc& d, bool* streamTarget)
{
    using StreamUpload::Result;
    using ItemT = StreamUpload::Item<StreamCopy>;
    auto reject = [this, keyTexture](const char* why) { return StreamUploaderReject(why, keyTexture); };

    if (!d.planes || d.planeCount == 0)
        return reject("no data");
    const D3D12_RESOURCE_DESC rd = res->GetDesc();
    if (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
        return reject("2D textures only");
    const bool planar = IsPlanar(rd.Format);
    const uint32_t planes = planar ? 2u : 1u;
    if (d.planeCount != planes)
        return reject(planar ? "a planar format takes two planes" : "one plane expected");
    if (d.mip >= rd.MipLevels || d.arraySlice >= rd.DepthOrArraySize)
        return reject("mip or array slice out of range");
    const bool hasRect = d.rect.width != 0;
    if (hasRect)
    {
        if (planar || IsBlockCompressed(rd.Format))
            return reject("a rect needs a single-plane, uncompressed format");
        const uint64_t mipW = (std::max)(uint64_t(1), rd.Width >> d.mip);
        const uint64_t mipH = (std::max)(uint64_t(1), uint64_t(rd.Height) >> d.mip);
        if (d.rect.height == 0 || uint64_t(d.rect.x) + d.rect.width > mipW ||
            uint64_t(d.rect.y) + d.rect.height > mipH)
            return reject("the rect is outside the subresource");
    }

    // One item per plane, in the copy's own layout: rows RowPitch (256) apart.
    struct Plane
    {
        ItemT          item;
        const uint8_t* src = nullptr;
        uint64_t       srcPitch = 0;
        uint32_t       rows = 0;
        uint64_t       rowBytes = 0;
    };
    Plane pl[2];
    for (uint32_t p = 0; p < planes; ++p)
    {
        const UINT sub = D3D12CalcSubresource(d.mip, d.arraySlice, p, rd.MipLevels, rd.DepthOrArraySize);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT   rows = 0;
        UINT64 rowBytes = 0, total = 0;
        m_device->GetCopyableFootprints(&rd, sub, 1, 0, &fp, &rows, &rowBytes, &total);
        if (total == 0 || rows == 0)
            return reject("no copyable footprint");
        if (hasRect)
        {
            const uint64_t texel = rowBytes / (std::max)(1u, fp.Footprint.Width);
            rowBytes = texel * d.rect.width;
            rows     = d.rect.height;
            fp.Footprint.Width    = d.rect.width;
            fp.Footprint.Height   = d.rect.height;
            fp.Footprint.RowPitch = static_cast<UINT>(AlignUp(rowBytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
        }
        const RHI::SubresourceData& sd = d.planes[p];
        if (!sd.dataPtr || sd.rowPitch < rowBytes)
            return reject("plane data missing or its row pitch is too small");

        Plane& q = pl[p];
        q.item.key.texture     = keyTexture;
        q.item.key.generation  = keyGeneration;
        q.item.key.subresource = sub;
        if (hasRect)
        {
            q.item.key.x = d.rect.x; q.item.key.y = d.rect.y;
            q.item.key.w = d.rect.width; q.item.key.h = d.rect.height;
        }
        q.item.policy = d.policy;
        q.item.size   = uint64_t(fp.Footprint.RowPitch) * rows;
        q.item.payload.dstResource = res;     // see StreamCopy: the recorder reads only this
        q.item.payload.before      = state;
        q.item.payload.dstX      = hasRect ? d.rect.x : 0u;
        q.item.payload.dstY      = hasRect ? d.rect.y : 0u;
        q.item.payload.footprint = fp.Footprint;
        q.src      = static_cast<const uint8_t*>(sd.dataPtr);
        q.srcPitch = sd.rowPitch;
        q.rows     = rows;
        q.rowBytes = rowBytes;
    }

    std::lock_guard<std::mutex> lock(m_stream.mutex);
    if (!m_stream.initialized && !InitStreamUploaderLocked())
    {
        m_stream.queue.CountRejected();
        return Result::Rejected;
    }
    if (streamTarget) *streamTarget = true;   // a pool texture: the sync paths refuse it from here on

    Result worst = Result::Queued;
    for (uint32_t p = 0; p < planes; ++p)
    {
        Plane& q = pl[p];
        const uint64_t pitch = q.item.payload.footprint.RowPitch;
        const StreamRows rows{ q.src, q.srcPitch, q.rows, q.rowBytes };
        // The bytes travel with the item when it has no ring place yet (a carry-over) or must survive
        // a lost frame (MustDeliver), within the cap. Past the cap a carry-over goes in without them
        // and is dropped; a MustDeliver lands without its lost-frame copy.
        const bool wantBytes = q.item.policy == StreamUpload::Policy::MustDeliver ||
                               !m_stream.queue.AcceptsOpenFrame();
        if (wantBytes && !m_stream.queue.CanRetain(q.item.size))
        {
            m_stream.queue.CountCapped();
            static std::atomic<uint32_t> s_cappedLogged{ 0 };
            if (s_cappedLogged.fetch_add(1, std::memory_order_relaxed) < 4)
                LOG_WARNING("[UPLOAD_STREAM] the CPU-copy cap (%llu KiB, %llu held) refused texture %u's %llu "
                            "bytes: %s", static_cast<unsigned long long>(m_stream.queue.RetainCap() / 1024),
                            static_cast<unsigned long long>(m_stream.queue.RetainedBytes()), keyTexture,
                            static_cast<unsigned long long>(q.item.size),
                            m_stream.queue.AcceptsOpenFrame() ? "delivered without its lost-frame copy"
                                                              : "a carry-over, dropped");
        }
        else if (wantBytes)
        {
            q.item.bytes.assign(static_cast<size_t>(q.item.size), 0);
            CopyRows(q.item.bytes.data(), pitch, q.src, q.srcPitch, q.rows, q.rowBytes);
        }
        const StreamUpload::Placement at = m_stream.queue.Admit(std::move(q.item));
        Result r = at.result;
        switch (at.result)
        {
        case Result::Queued:
        case Result::Replaced:
        case Result::Fallback:
        {
            // From the caller's rows - still valid during the call - whether or
            // not the item also kept its own copy.
            ItemT& it = m_stream.queue.Pending(at.slot)[at.index];
            const bool staged = at.result == Result::Fallback;
            if (!StreamUploaderPlaceBytesLocked(it, at.slot, at.offset, staged, rows) && staged)
            {
                // The recorder skips an item that has no staging (stale), so
                // the caller must not believe it delivered: Rejected keeps a
                // font page dirty and retries it next frame.
                LOG_ERROR("[UPLOAD_STREAM] a fallback staging buffer (%llu bytes) could not be made; "
                          "the copy will be skipped and the request reported Rejected",
                          static_cast<unsigned long long>(it.size));
                r = Result::Rejected;
            }
            break;
        }
        default: break;   // CarriedOver keeps its bytes; Dropped / Rejected are counted
        }
        worst = Worse(worst, r);
    }
    StreamUploaderPublishLocked();
    return worst;
}

// ==== GPU-to-GPU copies ====
StreamUpload::Result GraphicsDX12::QueueNativeTextureCopy(const RHI::Texture& dst, ID3D12Resource* src,
                                                          ID3D12Fence* srcFence, uint64_t srcFenceValue,
                                                          std::shared_ptr<void> keepAlive, StreamUpload::Policy policy)
{
    using StreamUpload::Result;
    if (const char* why = StreamUploadCallRefused())
        return StreamUploaderReject(why, dst.handleId);
    if (!src)
        return StreamUploaderReject("the copy has no source", dst.handleId);
    if (!dst.IsValid() || dst.handleId >= m_texturePool.size() || !m_texturePool[dst.handleId].resource)
        return StreamUploaderReject("not a texture", dst.handleId);
    TextureDX12& entry = m_texturePool[dst.handleId];
    if (entry.resource.Get() == src)
        return StreamUploaderReject("a copy onto itself", dst.handleId);
    if (!NativeCopyFits(entry.resource->GetDesc(), src->GetDesc()))
        return StreamUploaderReject("the copy's source differs in format, size or subresources", dst.handleId);

    StreamUpload::Item<StreamCopy> item;
    item.key.texture     = dst.handleId;
    item.key.generation  = entry.generation;
    item.key.subresource = 0xFFFFFFFFu;   // all of it - never a plane or rect upload's key
    item.policy          = policy;
    item.gpuSource       = true;
    item.payload.dstResource   = entry.resource;
    item.payload.before        = entry.state;
    item.payload.srcResource   = src;
    item.payload.srcFence      = srcFence;
    item.payload.srcFenceValue = srcFenceValue;
    item.payload.keepAlive     = std::move(keepAlive);

    StreamUpload::Result r;
    {
        std::lock_guard<std::mutex> lock(m_stream.mutex);
        if (!m_stream.initialized && !InitStreamUploaderLocked())
        {
            m_stream.queue.CountRejected();
            return Result::Rejected;
        }
        entry.streamTarget = true;
        r = m_stream.queue.Admit(std::move(item)).result;
        StreamUploaderPublishLocked();
    }
    return r;
}

bool GraphicsDX12::NativeCopyFits(const D3D12_RESOURCE_DESC& dst, const D3D12_RESOURCE_DESC& src)
{
    return dst.Dimension == src.Dimension && dst.Width == src.Width && dst.Height == src.Height &&
           dst.DepthOrArraySize == src.DepthOrArraySize && dst.MipLevels == src.MipLevels &&
           dst.Format == src.Format;
}

void GraphicsDX12::StreamUploaderBeforeSubmit()
{
    // The StreamUpload list is the first graphics list submitted (the order check), so a queue Wait
    // here, ahead of every submission, holds back exactly the GPU work that could read a decoder's
    // frame before the decoder signals - like CopyD3D12ResourceToTexture's queue Wait, without its
    // CPU wait.
    for (const auto& [fence, value] : m_stream.waits)
        if (fence && value > 0) ThrowIfFailed(m_queues[0]->Wait(fence.Get(), value));
    m_stream.waits.clear();
}

// ==== The slot boundary ====
void GraphicsDX12::StreamUploaderOpenSlot(uint64_t frame, uint32_t slot)
{
    using ItemT = StreamUpload::Item<StreamCopy>;
    std::vector<ItemT> released;   // destroyed after the lock: their staging goes
    {
        std::lock_guard<std::mutex> lock(m_stream.mutex);
        if (!m_stream.initialized || slot >= kFrameCount) return;
        const std::vector<StreamUpload::Placement> placed = m_stream.queue.Open(frame, slot, released);
        for (const StreamUpload::Placement& p : placed)
        {
            if (p.copy) continue;   // a GPU copy: its source travels with it, nothing to write
            ItemT& it = m_stream.queue.Pending(slot)[p.index];
            if (!StreamUploaderPlaceBytesLocked(it, slot, p.offset, p.needsStaging, StreamRowsOf(it)))
                LOG_ERROR("[UPLOAD_STREAM] a carried-over upload could not be placed (%llu bytes)",
                          static_cast<unsigned long long>(it.size));
            if (it.policy == StreamUpload::Policy::LatestWins) m_stream.queue.DropBytes(it);
        }
        StreamUploaderPublishLocked();
    }
}

// ==== The render half: record, then EndFrame ====
void GraphicsDX12::RecordPendingStreamUploads()
{
    using ItemT = StreamUpload::Item<StreamCopy>;
    m_stream.recording = true;   // until EndFrame: this thread is the render half recording a frame
    m_stream.listIndex = UINT32_MAX;
    const uint64_t frame = m_renderFrame.load(std::memory_order_relaxed);
    const uint32_t slot  = GetFrameIndex();
    std::vector<ItemT>* items = nullptr;
    std::vector<ItemT>  released;   // what a lost frame held: its staging goes after the lock
    {
        // A game-half call copies its bytes into the ring under this lock, so
        // the take can wait for one such copy: counted and timed, only then.
        std::unique_lock<std::mutex> lock(m_stream.mutex, std::try_to_lock);
        if (!lock.owns_lock())
        {
            const auto t0 = std::chrono::steady_clock::now();
            lock.lock();
            const uint64_t ns = UploadStats::ToNs(std::chrono::steady_clock::now() - t0);
            ++m_stream.takeWaits;
            m_stream.takeWaitNs += ns;
            m_stream.takeWaitMaxNs = (std::max)(m_stream.takeWaitMaxNs, ns);
        }
        if (!m_stream.initialized || slot >= kFrameCount) return;
        // Still set: the previous take never reached its EndFrame (the render
        // half threw). Take finds that frame and gives its MustDeliver
        // requests to this one, in request order (StreamUploadQueue.h).
        if (m_stream.takenSlot != UINT32_MAX)
        {
            static std::atomic<uint32_t> s_lostLogged{ 0 };
            if (s_lostLogged.fetch_add(1, std::memory_order_relaxed) < 4)
                LOG_WARNING("[UPLOAD_STREAM] frame %llu's uploads (slot %u) never reached the GPU; frame %llu "
                            "records its MustDeliver requests again",
                            static_cast<unsigned long long>(m_stream.takenFrame), m_stream.takenSlot,
                            static_cast<unsigned long long>(frame));
        }
        std::vector<StreamUpload::Placement> reclaimed;
        items = m_stream.queue.Take(frame, slot, reclaimed, released);
        for (const StreamUpload::Placement& p : reclaimed)
        {
            if (p.copy) continue;   // a GPU copy: its source travels with it, nothing to write
            ItemT& it = (*items)[p.index];
            if (!StreamUploaderPlaceBytesLocked(it, slot, p.offset, p.needsStaging, StreamRowsOf(it)))
                LOG_ERROR("[UPLOAD_STREAM] a lost frame's upload could not be placed again (%llu bytes)",
                          static_cast<unsigned long long>(it.size));
        }
        m_stream.takenSlot  = slot;
        m_stream.takenFrame = frame;
    }
    if (!items || items->empty()) return;

    // Everything below reads the ITEMS, never the texture pool: the game half
    // may destroy a texture (and CreateTexture reuse its id) while this
    // records, and DestroyTexture parks the lane only when it runs on the
    // device frame thread. Each item holds its own reference to the resource
    // and the state it was in when queued (StreamCopy).
    std::vector<uint8_t> skip(items->size(), 0);
    uint64_t stale = 0;
    for (size_t i = 0; i < items->size(); ++i)
    {
        const ItemT& it = (*items)[i];
        if (!it.payload.dstResource || (it.staged && !it.payload.staging) ||
            (it.gpuSource && !it.payload.srcResource))
        {
            skip[i] = 1;
            ++stale;
        }
    }

    // The states inside this list: every destination goes to COPY_DEST in one
    // batch up front and back to the state it entered with (its queue-time
    // state: the state book never sees it move) in one batch at the end. A GPU
    // copy's source is never transitioned here: it is in COMMON and the copy
    // reads it through implicit promotion (QueueNativeTextureCopy), so this
    // queue never moves a resource another queue - a decoder - may be using.
    struct Track
    {
        ID3D12Resource*       res;
        D3D12_RESOURCE_STATES entry, now;
    };
    std::vector<Track> track;
    auto find = [&track](ID3D12Resource* r, D3D12_RESOURCE_STATES entry) -> size_t
    {
        for (size_t t = 0; t < track.size(); ++t)
            if (track[t].res == r) return t;
        track.push_back({ r, entry, entry });
        return track.size() - 1;
    };
    std::vector<size_t> dstTrack(items->size(), 0);
    for (size_t i = 0; i < items->size(); ++i)
        if (!skip[i]) dstTrack[i] = find((*items)[i].payload.dstResource.Get(), (*items)[i].payload.before);

    RHI::CommandList cl{};
    if (m_stream.recordIntoPrimary)
        cl.internalId = 0;   // the reverse arm: the primary list, submitted LAST
    else
    {
#if defined(WITH_HARNESS) && WITH_HARNESS
        if (m_stream.orderBreak)
        {
            // The order check's reverse arm: begun first, depending on
            // nothing, so EndFrame submits it ahead of the copies.
            const RHI::CommandList early = BeginCommandList(RHI::QueueType::GRAPHICS);
            SetCommandListName(early, "StreamUploadOrderBreak");
        }
#endif
        cl = BeginCommandList(RHI::QueueType::GRAPHICS);
        SetCommandListName(cl, "StreamUpload");
        m_stream.listIndex = cl.internalId;
        m_stream.listFrame = frame;
        // The primary list is pool entry 0, so with no dependency of its own it
        // would be submitted FIRST - and its usual edge to the graph's tail is
        // conditional (FramePipeline: an invalid lastPassCL adds none). This
        // edge makes it wait for the copies on every frame.
        RHI::CommandList primary{};
        primary.internalId = 0;
        AddCommandListDependency(primary, cl);
    }
    ID3D12GraphicsCommandList* native = GetPoolEntry(cl).GetCommandList();
    if (!native) return;
    const uint32_t region = BeginGPUTimestampNoTracy(cl, "StreamUpload");
    BeginEventMarker(cl, "StreamUpload");

    std::vector<D3D12_RESOURCE_BARRIER> pending;
    auto move = [&](size_t t, D3D12_RESOURCE_STATES to)
    {
        if (track[t].now == to) return;
        pending.push_back(CD3DX12_RESOURCE_BARRIER::Transition(track[t].res, track[t].now, to));
        track[t].now = to;
    };
    auto flush = [&]
    {
        if (pending.empty()) return;
        native->ResourceBarrier(static_cast<UINT>(pending.size()), pending.data());
        JournalNativeBarriers(cl, pending.data(), static_cast<uint32_t>(pending.size()));
        pending.clear();
    };
    for (size_t t = 0; t < track.size(); ++t) move(t, D3D12_RESOURCE_STATE_COPY_DEST);

    // Two copies into overlapping texels of one subresource in this list are
    // two writes nothing orders - a font page's MustDeliver prewarm rect and the
    // same frame's new glyphs inside it (never merged) - so a transition pair
    // between them makes the later one wait for the earlier.
    struct Written
    {
        ID3D12Resource* res;
        uint32_t        sub;                 // UINT32_MAX: every subresource
        uint32_t        x0, y0, x1, y1;      // texels, x1 / y1 exclusive
    };
    std::vector<Written> written;
    auto overlaps = [](const Written& a, const Written& b)
    {
        return a.res == b.res && (a.sub == UINT32_MAX || b.sub == UINT32_MAX || a.sub == b.sub) &&
               a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
    };
    uint64_t copies = 0, serialized = 0;
    m_stream.waits.clear();
    for (size_t i = 0; i < items->size(); ++i)
    {
        if (skip[i]) continue;
        const ItemT& it = (*items)[i];
        Written w{ it.payload.dstResource.Get(), UINT32_MAX, 0, 0, UINT32_MAX, UINT32_MAX };
        if (!it.gpuSource)
        {
            w.sub = it.key.subresource;
            if (it.key.w != 0)
            {
                w.x0 = it.payload.dstX;
                w.y0 = it.payload.dstY;
                w.x1 = it.payload.dstX + it.payload.footprint.Width;
                w.y1 = it.payload.dstY + it.payload.footprint.Height;
            }
        }
        for (const Written& e : written)
            if (overlaps(e, w))
            {
                ++serialized;
                move(dstTrack[i], D3D12_RESOURCE_STATE_COMMON);
                move(dstTrack[i], D3D12_RESOURCE_STATE_COPY_DEST);
                break;
            }
        written.push_back(w);
        flush();
        if (it.gpuSource)
        {
            // All of the source (both planes of NV12), read in COMMON: no barrier on it.
            native->CopyResource(it.payload.dstResource.Get(), it.payload.srcResource.Get());
            if (it.payload.srcFence)
                m_stream.waits.emplace_back(it.payload.srcFence, it.payload.srcFenceValue);
        }
        else
        {
            ID3D12Resource* srcRes = it.staged ? it.payload.staging.Get() : m_stream.ring[slot].Get();
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT placed{};
            placed.Offset    = it.staged ? 0 : it.offset;
            placed.Footprint = it.payload.footprint;
            const CD3DX12_TEXTURE_COPY_LOCATION dstLoc(it.payload.dstResource.Get(), it.key.subresource);
            const CD3DX12_TEXTURE_COPY_LOCATION srcLoc(srcRes, placed);
            native->CopyTextureRegion(&dstLoc, it.payload.dstX, it.payload.dstY, 0, &srcLoc, nullptr);
        }
        ++copies;
    }

    // Back to the state each resource entered with.
    for (size_t t = 0; t < track.size(); ++t) move(t, track[t].entry);
    flush();

    EndEventMarker(cl);
    EndGPUTimestampNoTracy(cl, region);

    std::lock_guard<std::mutex> lock(m_stream.mutex);
    m_stream.stale += stale;
    ++m_stream.recordedLists;
    m_stream.recordedCopies += copies;
    m_stream.serialized     += serialized;
    StreamUploaderPublishLocked();
}

void GraphicsDX12::StreamUploaderEndFrame()
{
    // The order check: the StreamUpload list waits on nothing and is the FIRST graphics list this
    // frame submits. EndFrame's FIFO Kahn sort submits every list without a dependency first, in pool
    // order, so a graphics list begun before RecordPendingStreamUploads and depending on nothing
    // would run ahead of the copies and a reader in it would see the previous frame's bytes. The
    // primary list (pool 0) is begun earlier and waits on the StreamUpload list. (Async compute must
    // not read a stream target at all.) Only the frame that recorded the list checks it.
    const uint64_t frame = m_renderFrame.load(std::memory_order_relaxed);
    if (m_stream.listIndex != UINT32_MAX && m_stream.listFrame == frame &&
        m_stream.listIndex < m_commandListCount.load(std::memory_order_relaxed))
    {
        const uint32_t li = m_stream.listIndex;
        ENGINE_CHECK_ONCE(m_commandLists[li]->waitFor.empty(),
                          "[UPLOAD_STREAM] the StreamUpload list (pool %u) waits on another list: its "
                          "copies are no longer first", li);
        for (uint32_t j : m_submitOrder)
        {
            if (j == li) break;
            const CommandListDX12& other = *m_commandLists[j];
            ENGINE_CHECK_ONCE(other.queue != RHI::QueueType::GRAPHICS,
                              "[UPLOAD_STREAM] graphics list %u ('%s') was submitted before the StreamUpload "
                              "list (pool %u): a reader there sees the previous frame's uploads",
                              j, other.name ? other.name : "?", li);
        }
    }
    m_stream.listIndex = UINT32_MAX;
    m_stream.recording = false;

    // The frame that took the slot is the one submitted. A stale take (its own
    // EndFrame never ran) stays for the next take to find as lost.
    std::lock_guard<std::mutex> lock(m_stream.mutex);
    if (m_stream.initialized && m_stream.takenSlot != UINT32_MAX && m_stream.takenFrame == frame)
    {
        m_stream.queue.Submitted(m_stream.takenSlot);
        m_stream.takenSlot = UINT32_MAX;
    }
}

// The probe's reader (IGraphicsDevice.h): a stream target into a READBACK
// buffer with barriers the state book never sees. The book state it starts
// from is stable - nothing transitions a stream target through the book.
void GraphicsDX12::CopyTexture2DToBufferInPlace(const RHI::Texture& src, const RHI::GPUBuffer& dst,
                                                RHI::CommandList cmd)
{
    if (!src.IsValid() || !dst.IsValid() || src.handleId >= m_texturePool.size() ||
        dst.handleId >= m_bufferPool.size())
        return;
    ID3D12GraphicsCommandList* cl = GetPoolEntry(cmd).GetCommandList();
    ID3D12Resource* srcRes = m_texturePool[src.handleId].resource.Get();
    ID3D12Resource* dstRes = m_bufferPool[dst.handleId].resource.Get();
    if (!cl || !srcRes || !dstRes) return;
    const D3D12_RESOURCE_STATES state = m_texturePool[src.handleId].state;
    const bool move = state != D3D12_RESOURCE_STATE_COPY_SOURCE;
    D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(srcRes, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (move)
    {
        cl->ResourceBarrier(1, &b);
        JournalNativeBarriers(cmd, &b, 1);
    }
    // Every plane of a planar format (GetTexture2DReadbackFootprint's layout).
    const D3D12_RESOURCE_DESC srcDesc = srcRes->GetDesc();
    const UINT planes = IsPlanar(srcDesc.Format) ? 2u : 1u;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[2]{};
    m_device->GetCopyableFootprints(&srcDesc, 0, planes, 0, fp, nullptr, nullptr, nullptr);
    for (UINT p = 0; p < planes; ++p)
    {
        const CD3DX12_TEXTURE_COPY_LOCATION srcLoc(srcRes, p);
        const CD3DX12_TEXTURE_COPY_LOCATION dstLoc(dstRes, fp[p]);
        cl->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
    }
    if (move)
    {
        b = CD3DX12_RESOURCE_BARRIER::Transition(srcRes, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
        cl->ResourceBarrier(1, &b);
        JournalNativeBarriers(cmd, &b, 1);
    }
}

void GraphicsDX12::StreamUploaderForget(uint32_t texture, uint32_t generation)
{
    using ItemT = StreamUpload::Item<StreamCopy>;
    std::vector<ItemT> released;   // after the lock: their references and staging go
    std::lock_guard<std::mutex> lock(m_stream.mutex);
    if (!m_stream.initialized) return;
    m_stream.queue.Forget(texture, generation, released);
    StreamUploaderPublishLocked();
}
