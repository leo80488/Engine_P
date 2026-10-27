#include "Graphics/GPUProfiler.h"
#include "Graphics/GpuMemoryLedger.h"   // the profiler's GPU resource ledger
#include "System/Log.h"
#include "System/CommandLine.h"
#include "Harness/RunLedger.h"
#include "Diagnostics/Profiler.h"
#include <algorithm>
#include <cstring>
#ifdef TRACY_ENABLE
#include <tracy/Tracy.hpp>
#endif

using Microsoft::WRL::ComPtr;

void GPUProfiler::Init(ID3D12Device* device,
                       ID3D12CommandQueue* graphicsQueue,
                       ID3D12CommandQueue* computeQueue,
                       ID3D12CommandQueue* copyQueue)
{
    // One shared query heap: ResolveQueryData copies the live queries into the
    // recording slot's readback buffer, and each slot's regionCount snapshot says how
    // much of it to read.
    D3D12_QUERY_HEAP_DESC heapDesc{};
    heapDesc.Type  = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    heapDesc.Count = kMaxQueries;
    if (FAILED(device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&m_queryHeap))))
    {
        LOG_ERROR("GPUProfiler: CreateQueryHeap failed");
        return;
    }
    GpuLedger::Track(m_queryHeap.Get(), Diag::Prof::GpuMemCat::QueryHeap, uint64_t(kMaxQueries) * sizeof(uint64_t),
                     "GPUProfiler.QueryHeap");

    // Per-slot readback buffers (persistently mapped): BeginFrame N reads slot
    // N % kFrameCount, resolved kFrameCount-1 frames ago, while the other slots'
    // resolve copies are still in flight.
    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bufDesc{};
    bufDesc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width              = kMaxQueries * sizeof(uint64_t);
    bufDesc.Height             = 1;
    bufDesc.DepthOrArraySize   = 1;
    bufDesc.MipLevels          = 1;
    bufDesc.SampleDesc.Count   = 1;
    bufDesc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (uint32_t s = 0; s < kFrameCount; ++s)
    {
        FrameSlot& slot = m_slots[s];
        if (FAILED(device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&slot.readbackBuffer))))
        {
            LOG_ERROR("GPUProfiler: readback buffer[%u] creation failed", s);
            return;
        }
        GpuLedger::TrackCommitted(device, slot.readbackBuffer.Get(), Diag::Prof::GpuMemCat::Readback,
                                  "GPUProfiler.Readback");
        D3D12_RANGE readRange{ 0, kMaxQueries * sizeof(uint64_t) };
        slot.readbackBuffer->Map(0, &readRange,
            reinterpret_cast<void**>(&slot.mappedReadback));
    }

    // Per-queue timestamp frequency: graphics, compute and copy may each differ.
    ID3D12CommandQueue* queues[kQueueCount] = { graphicsQueue, computeQueue, copyQueue };
    for (uint8_t q = 0; q < kQueueCount; ++q)
    {
        m_queueFrequency[q] = 1;
        m_queues[q] = queues[q];   // borrowed; GraphicsDX12 outlives the profiler
        if (queues[q])
        {
            uint64_t f = 0;
            if (SUCCEEDED(queues[q]->GetTimestampFrequency(&f)) && f != 0)
                m_queueFrequency[q] = f;
        }
    }

    LARGE_INTEGER qpf{};
    QueryPerformanceFrequency(&qpf);
    m_qpcFrequency = (qpf.QuadPart > 0) ? static_cast<uint64_t>(qpf.QuadPart) : 1;

    RefreshCalibration();

    LOG_INFO("GPUProfiler: initialised (gfx=%llu compute=%llu copy=%llu, max %u regions, %u slots, "
             "cross-queue calibration %s)",
             m_queueFrequency[kQueueGraphics],
             m_queueFrequency[kQueueCompute],
             m_queueFrequency[kQueueCopy],
             kMaxRegions, kFrameCount,
             calibrationValid ? "OK" : "UNAVAILABLE (effectiveFrameMs falls back to max-of-queues)");
}

// Correlate each queue's free-running timestamp counter with QPC so timestamps
// from different queues can be placed on one timeline. Called every frame: the
// GPU and CPU clocks drift relative to each other, and a stale correlation
// would show up as phantom overlap (or phantom serialisation) between queues.
void GPUProfiler::RefreshCalibration()
{
    bool allOk = true;
    for (uint8_t q = 0; q < kQueueCount; ++q)
    {
        if (!m_queues[q]) continue;   // absent queue never contributes spans
        uint64_t gpuTick = 0, cpuTick = 0;
        if (SUCCEEDED(m_queues[q]->GetClockCalibration(&gpuTick, &cpuTick)))
        {
            m_calGpu[q] = gpuTick;
            m_calQpc[q] = cpuTick;
        }
        else
        {
            allOk = false;
        }
    }
    // The graphics queue is the timeline's origin, so its calibration is the
    // one that must exist.
    calibrationValid = allOk && (m_queues[kQueueGraphics] != nullptr);
}

// Raw queue-local timestamp -> nanoseconds on the shared timeline, with the
// graphics queue's calibration point as the origin. Signed arithmetic
// throughout: timestamps recorded ~2 frames before the calibration point are
// legitimately NEGATIVE relative to it, and doing this in uint64 would wrap.
double GPUProfiler::TicksToSharedNs(uint64_t t, uint8_t q) const
{
    if (q >= kQueueCount) q = kQueueGraphics;
    const double freq = static_cast<double>(m_queueFrequency[q]);
    // Offset from this queue's own calibration point, in ns.
    const double localNs =
        (static_cast<double>(static_cast<int64_t>(t - m_calGpu[q]))) * (1e9 / freq);
    // Where that calibration point sits on the shared (QPC) timeline, in ns,
    // relative to the graphics queue's calibration point.
    const double baseNs =
        (static_cast<double>(static_cast<int64_t>(m_calQpc[q] - m_calQpc[kQueueGraphics])))
        * (1e9 / static_cast<double>(m_qpcFrequency));
    return baseNs + localNs;
}

int64_t GPUProfiler::TicksToSteadyNs(uint64_t t, uint8_t q) const
{
    if (q >= kQueueCount) q = kQueueGraphics;
    const double freq = static_cast<double>(m_queueFrequency[q]);
    const double localNs =
        static_cast<double>(static_cast<int64_t>(t - m_calGpu[q])) * (1e9 / freq);
    return Diag::Prof::QpcToSteadyNs(static_cast<int64_t>(m_calQpc[q])) +
           static_cast<int64_t>(localNs);
}

void GPUProfiler::NoteSubmit(uint32_t slot, int64_t firstNs, int64_t lastNs)
{
    if (slot >= kFrameCount) return;
    m_slots[slot].submitFirstNs = firstNs;
    m_slots[slot].submitLastNs  = lastNs;
}

void GPUProfiler::BeginFrame(uint32_t slot)
{
    // The caller waited on the slot's fence: the resolve from kFrameCount-1 frames
    // ago is complete.
    hasResults  = false;
    resultCount = 0;
    totalGpuMs  = 0.f;
    for (uint8_t q = 0; q < kQueueCount; ++q) perQueueTotalMs[q] = 0.f;

    if (slot >= kFrameCount) slot = 0;
    FrameSlot& s = m_slots[slot];
    resultsSequence = s.resolveSeq;
    resultsAgeFrames = static_cast<uint32_t>(m_resolveSeq + 1 - s.resolveSeq);

    overlapMs = 0.f;
    RefreshCalibration();

    // Intervals on the shared nanosecond timeline, kept so the unions below can
    // be computed per queue and across all queues.
    struct Span { double lo, hi; uint8_t q; uint32_t region; };
    static thread_local Span t_spans[kMaxRegions];   // 24 KB: not on the stack
    uint32_t spanCount = 0;

    if (s.mappedReadback && s.regionCount > 0)
    {
        for (uint32_t i = 0; i < s.regionCount; ++i)
        {
            const uint8_t qt = (s.regionQueues[i] < kQueueCount)
                ? s.regionQueues[i] : kQueueGraphics;
            const double tickToMs = 1000.0 / static_cast<double>(m_queueFrequency[qt]);

            uint64_t begin = s.mappedReadback[i * 2];
            uint64_t end   = s.mappedReadback[i * 2 + 1];
            float ms = (end > begin)
                ? static_cast<float>(static_cast<double>(end - begin) * tickToMs)
                : 0.f;

            results[resultCount].name      = s.regionNames[i]; // still valid (static strings)
            results[resultCount].gpuMs     = ms;
            results[resultCount].queueType = qt;
            totalGpuMs += ms;   // naive sum: not a frame cost (see the header)
            ++resultCount;

            if (end > begin)
                t_spans[spanCount++] = { TicksToSharedNs(begin, qt),
                                       TicksToSharedNs(end,   qt), qt, i };
        }
        hasResults = (resultCount > 0);
    }

    // Sort spans by start once; both unions sweep this order. Ties put the longer span
    // first, so a parent precedes the children it contains: the profiler's
    // nesting-depth pass relies on it.
    std::sort(t_spans, t_spans + spanCount, [](const Span& a, const Span& b) {
        if (a.lo != b.lo) return a.lo < b.lo;
        return a.hi > b.hi;
    });

    // Union length of the spans selected by `wantQueue` (0xFF = all queues). A nested
    // child lies inside its parent and extends nothing, which makes this immune to a
    // sum's double-counting.
    auto unionMs = [&](uint8_t wantQueue) -> float
    {
        double total = 0.0, curLo = 0.0, curHi = 0.0;
        bool open = false;
        for (uint32_t i = 0; i < spanCount; ++i)
        {
            if (wantQueue != 0xFFu && t_spans[i].q != wantQueue) continue;
            if (!open) { curLo = t_spans[i].lo; curHi = t_spans[i].hi; open = true; continue; }
            if (t_spans[i].lo <= curHi) { if (t_spans[i].hi > curHi) curHi = t_spans[i].hi; }
            else { total += curHi - curLo; curLo = t_spans[i].lo; curHi = t_spans[i].hi; }
        }
        if (open) total += curHi - curLo;
        return static_cast<float>(total * 1e-6);   // ns -> ms
    };

    for (uint8_t q = 0; q < kQueueCount; ++q)
        perQueueTotalMs[q] = unionMs(q);

    if (calibrationValid)
    {
        // Real GPU busy time: the union across every queue. Only meaningful
        // once the queues share a timeline.
        effectiveFrameMs = unionMs(0xFFu);
        const float sumOfQueues = perQueueTotalMs[kQueueGraphics]
                                + perQueueTotalMs[kQueueCompute]
                                + perQueueTotalMs[kQueueCopy];
        overlapMs = sumOfQueues - effectiveFrameMs;
        if (overlapMs < 0.f) overlapMs = 0.f;   // clock jitter at the margins
    }
    else
    {
        // No shared timeline: fall back to the critical-path estimate (max of the
        // per-queue unions). Still nesting-proof, but blind to overlap; treat it as a lower
        // bound.
        effectiveFrameMs = perQueueTotalMs[kQueueGraphics];
        if (perQueueTotalMs[kQueueCompute] > effectiveFrameMs)
            effectiveFrameMs = perQueueTotalMs[kQueueCompute];
        if (perQueueTotalMs[kQueueCopy] > effectiveFrameMs)
            effectiveFrameMs = perQueueTotalMs[kQueueCopy];
    }

#ifdef TRACY_ENABLE
    // Publish the totals as plots. They come from this profiler's index-paired regions
    // (idx*2 / idx*2+1), so they hold even where the per-pass Tracy GPU zone view does
    // not.
    if (hasResults)
    {
        TracyPlot("GPU.GraphicsMs",       perQueueTotalMs[kQueueGraphics]);
        TracyPlot("GPU.ComputeMs",        perQueueTotalMs[kQueueCompute]);
        TracyPlot("GPU.CopyMs",           perQueueTotalMs[kQueueCopy]);
        TracyPlot("GPU.EffectiveFrameMs", effectiveFrameMs);
        // The async-compute scoreboard: if this stays near zero the queues are
        // serialised regardless of what queue each CL was opened on.
        TracyPlot("GPU.OverlapMs",        overlapMs);
    }
#endif

    // Opt-in diagnostics, consumed on the render thread after safe readback.
    // Parent/child regions intentionally remain separate; do not sum them.
    static const int s_logEvery = Sys::CommandLine::Get().GetInt("-gpulog_every", 0);
    if (hasResults && s_logEvery > 0 && s.resolveSeq % static_cast<uint64_t>(s_logEvery) == 0)
    {
        double first = 0.0, last = 0.0;
        if (spanCount) { first = t_spans[0].lo; last = t_spans[0].hi; }
        for (uint32_t i = 1; i < spanCount; ++i) last = (std::max)(last, t_spans[i].hi);
        LOG_INFO("[GPUFRAME] seq=%llu age=%u calibrated=%u regions=%u dropped=%u graphics=%.4f compute=%.4f copy=%.4f busy=%.4f overlap=%.4f envelope=%.4f",
            static_cast<unsigned long long>(s.resolveSeq), resultsAgeFrames,
            calibrationValid ? 1u : 0u, resultCount, s.regionsDropped,
            perQueueTotalMs[0], perQueueTotalMs[1], perQueueTotalMs[2],
            effectiveFrameMs, overlapMs, calibrationValid ? (last - first) * 1e-6 : -1.0);
        for (uint32_t i = 0; i < resultCount; ++i)
            LOG_INFO("[GPUPASS] seq=%llu queue=%u name=%s ms=%.4f",
                static_cast<unsigned long long>(s.resolveSeq), results[i].queueType,
                results[i].name ? results[i].name : "unnamed", results[i].gpuMs);
    }

    // The engine profiler's GPU timeline (Diagnostics/ProfilerGpu.h): the same spans
    // in steady_clock ns, keyed by the render frame they measured. Only when the
    // profiler is on: interning ~100 region names a frame is work nobody asked for
    // otherwise.
    if (hasResults && Diag::Prof::Enabled(Diag::Prof::Level::Stats))
    {
        Diag::Prof::GpuFrameData& g = m_profFrame;
        g.valid      = true;
        g.frame      = s.renderFrame;
        g.resolveSeq = s.resolveSeq;
        g.calibrated = calibrationValid;
        g.regions    = resultCount;
        g.dropped    = s.regionsDropped;
        g.busyMs     = effectiveFrameMs;
        for (uint8_t q = 0; q < kQueueCount && q < Diag::Prof::kGpuQueues; ++q)
            g.queueMs[q] = perQueueTotalMs[q];
        g.overlapMs    = overlapMs;
        g.submitNs     = s.submitFirstNs;
        g.submitLastNs = s.submitLastNs;
        g.spanCount    = 0;
        g.firstNs = 0; g.lastNs = 0;
        // Nesting depth per queue: spans are sorted by start (parents first on
        // ties), so a stack of open ends per queue gives each span's depth.
        int64_t openEnd[Diag::Prof::kGpuQueues][32];
        int     openTop[Diag::Prof::kGpuQueues] = {};
        for (uint32_t i = 0; i < spanCount && g.spanCount < Diag::Prof::kMaxGpuSpans; ++i)
        {
            const Span& sp = t_spans[i];
            const uint32_t ri = sp.region;
            Diag::Prof::GpuSpan& out = g.spans[g.spanCount++];
            out.beginNs = TicksToSteadyNs(s.mappedReadback[ri * 2],     sp.q);
            out.endNs   = TicksToSteadyNs(s.mappedReadback[ri * 2 + 1], sp.q);
            out.name    = Diag::Prof::Intern(results[ri].name ? results[ri].name : "unnamed");
            out.queue   = sp.q;
            out.counters = s.regionCounters[ri];   // AnnotateRegion's counters
            const uint8_t q = sp.q < Diag::Prof::kGpuQueues ? sp.q : 0;
            int& top = openTop[q];
            while (top > 0 && openEnd[q][top - 1] <= out.beginNs) --top;
            out.depth = static_cast<uint8_t>(top);
            if (top < 32) openEnd[q][top++] = out.endNs;
            if (!g.firstNs || out.beginNs < g.firstNs) g.firstNs = out.beginNs;
            if (out.endNs > g.lastNs) g.lastNs = out.endNs;
        }
        if (g.frame != ~0ull) Diag::Prof::SubmitGpuFrame(g);
    }

    Harness::RunLedger::Get().NoteGpuFrame(*this);
    // Consume once, including when profiling is toggled off before a resolve.
    s.regionCount = 0;
    s.regionsDropped = 0;
    // Start a new recording window on this slot.
    m_recordSlot = slot;
    m_regionCount.store(0, std::memory_order_relaxed);
}

uint32_t GPUProfiler::BeginTimestamp(ID3D12GraphicsCommandList* cmd,
                                     const char* name,
                                     uint8_t queueType)
{
    uint32_t idx = m_regionCount.fetch_add(1, std::memory_order_relaxed);
    if (idx >= kMaxRegions) return ~0u;

    FrameSlot& s = m_slots[m_recordSlot];
    s.regionNames[idx]  = name;
    s.regionQueues[idx] = (queueType < kQueueCount) ? queueType : kQueueGraphics;
    s.regionCounters[idx] = {};
    cmd->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, idx * 2);
    return idx;
}

void GPUProfiler::EndTimestamp(ID3D12GraphicsCommandList* cmd, uint32_t regionIndex)
{
    if (regionIndex >= kMaxRegions) return;
    cmd->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, regionIndex * 2 + 1);
}

void GPUProfiler::AnnotateRegion(uint32_t regionIndex, const Diag::Prof::GpuCounters& counters)
{
    if (regionIndex >= kMaxRegions) return;
    m_slots[m_recordSlot].regionCounters[regionIndex] = counters;
}

void GPUProfiler::ResolveQueries(ID3D12GraphicsCommandList* cmd, uint32_t slot,
                                 uint64_t renderFrame)
{
    uint32_t count = m_regionCount.load(std::memory_order_relaxed);
    if (slot >= kFrameCount) slot = 0;
    FrameSlot& s = m_slots[slot];
    s.renderFrame   = renderFrame;
    s.submitFirstNs = 0;   // NoteSubmit fills them after this frame's submission
    s.submitLastNs  = 0;
    // Advance even for an empty recording, so age describes profiler frames
    // rather than only the subset that happened to emit timestamp regions.
    s.resolveSeq = ++m_resolveSeq;
    s.regionsDropped = count > kMaxRegions ? count - kMaxRegions : 0;
    Harness::RunLedger::Get().NoteGpuRegionsDropped(s.regionsDropped);
    s.regionCount = 0;
    if (count == 0) return;
    static bool s_warnedOverflow = false;
    if (count > kMaxRegions && !s_warnedOverflow)
    {
        LOG_WARNING("GPUProfiler: %u regions exceed capacity %u; timings are incomplete", count, kMaxRegions);
        s_warnedOverflow = true;
    }
    if (count > kMaxRegions) count = kMaxRegions;
    if (!s.readbackBuffer) return;

    cmd->ResolveQueryData(
        m_queryHeap.Get(),
        D3D12_QUERY_TYPE_TIMESTAMP,
        0, count * 2,
        s.readbackBuffer.Get(), 0);

    // Region names and queues were written into this slot by BeginTimestamp
    // (BeginFrame set m_recordSlot to it); the next BeginFrame on this slot,
    // kFrameCount-1 frames later, reads regionCount to know how much of the buffer is
    // populated.
    s.regionCount = count;
}
