#pragma once

// GPUProfiler - DX12 timestamp-query profiler.
//
// Records per-pass GPU timestamps via a D3D12 query heap + readback buffer.
// Thread-safe: workers may call BeginTimestamp concurrently (atomic index). Results
// come back kFrameCount-1 frames late (the frame pipelining; see
// GraphicsDX12::BeginFrame).
//
// Each region carries the queue that recorded it, and frame cost is an interval
// union, not a sum: a sum double-counts nesting (passes open parent regions around
// children; DDGI wraps eight), and max(graphics, compute) assumes the queues
// overlap, which is a measurement, not a given. perQueueTotalMs[q] is the union of
// q's own intervals (nesting-proof), effectiveFrameMs the union across all queues
// (real GPU busy time), and overlapMs how much concurrency happened: ~0 means the
// queues are serialized whatever the queue assignment says.
//
// The cross-queue union needs a common timeline: queue counters have independent
// bases and may differ in frequency. GetClockCalibration() gives a correlated
// (GPU, QPC) pair per queue, refreshed every frame. If an adapter refuses it,
// calibrationValid is false and effectiveFrameMs falls back to the max of the
// per-queue unions (still nesting-proof, blind to overlap).
//
// Readback buffer, names and queues are kept per frame slot so pipelining does not
// race the readback memcpy; slot indices match GraphicsDX12::m_frameIndex.
//
// Usage:
//   profiler.BeginFrame(slot)                 - read slot's results (resolved
//                                               kFrameCount-1 frames ago), reset
//                                               the live region counter
//   uint32_t r = profiler.BeginTimestamp(cl, "PassName", queueType)
//   ... record pass ...
//   profiler.EndTimestamp(cl, r)
//   profiler.ResolveQueries(primaryCL, slot)  - in EndFrame: writes slot's
//                                               readback buffer
//   ... Present + signal fence; kFrameCount-1 frames later BeginFrame(slot) reads
//   the results.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Graphics/FrameConstants.h"
#include "Graphics/GpuTimingView.h"   // TimingResult
#include "Diagnostics/ProfilerGpu.h"   // the engine profiler's GPU timeline hand-off
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <atomic>

struct GPUProfiler
{
    // Eight DDGI volumes plus both reflection layers exceed 128 regions, and
    // the engine profiler's GPU timeline wants every region a frame records:
    // 1024 (2048 queries, 16 KB of readback per slot). Overflow is counted per
    // frame (regionsDropped -> run.json gpuRegionsDropped, the profiler's
    // gpuDropped), never silently truncated.
    static constexpr uint32_t kMaxRegions = 1024;
    static constexpr uint32_t kMaxQueries = kMaxRegions * 2;
    // Per-frame ring depth (== GraphicsDX12::kFrameCount). With the CPU kFrameCount-1
    // frames ahead, one shared readback buffer would race: BeginFrame would read while
    // the GPU writes an in-flight resolve.
    static constexpr uint32_t kFrameCount = RHI::kFramesInFlight;

    // Mirror of RHI::QueueType without GraphicsStruct.h: GRAPHICS=0, COMPUTE=1,
    // COPY=2; keep in sync.
    static constexpr uint8_t kQueueGraphics = 0;
    static constexpr uint8_t kQueueCompute  = 1;
    static constexpr uint8_t kQueueCopy     = 2;
    static constexpr uint8_t kQueueCount    = 3;

    // The RHI's record, so IGraphicsDevice::GetGPUTimings can hand out a view of
    // `results` without this header.
    using TimingResult = RHI::GpuTimingResult;

    // ---- Lifecycle ----
    // Takes all three queues to read each one's timestamp frequency (graphics and
    // compute frequencies may differ; the compute or copy queue may be null on some
    // adapters).
    void Init(ID3D12Device* device,
              ID3D12CommandQueue* graphicsQueue,
              ID3D12CommandQueue* computeQueue,
              ID3D12CommandQueue* copyQueue);

    // Reads the previous results from slot's readback buffer and resets the live
    // counter. The caller must have waited on the slot's fence (its resolve completed
    // on the GPU).
    void BeginFrame(uint32_t slot);

    // Resolve live queries into slot's readback buffer at end of frame.
    // @p renderFrame is the engine frame whose command lists are being measured;
    // the engine profiler keys the GPU timeline by it (it comes back
    // resultsAgeFrames later, and "later" must not become "attributed later").
    void ResolveQueries(ID3D12GraphicsCommandList* cmd, uint32_t slot,
                        uint64_t renderFrame = ~0ull);

    // CPU time (steady_clock ns) of the frame's first and last
    // ExecuteCommandLists, for the engine profiler's "was the GPU waiting for
    // the CPU" test. Called by GraphicsDX12::EndFrame after it submits.
    void NoteSubmit(uint32_t slot, int64_t firstNs, int64_t lastNs);

    // ---- Per-pass recording (thread-safe) ----
    // @p queueType is the RHI::QueueType of the recording CL, so per-queue totals and
    // the effective frame time need no CL state at readback.
    uint32_t BeginTimestamp(ID3D12GraphicsCommandList* cmd,
                            const char* name,
                            uint8_t queueType);
    void     EndTimestamp(ID3D12GraphicsCommandList* cmd, uint32_t regionIndex);
    // What the region's command lists recorded: set by the device at the region's end
    // (its own list) and by RenderGraph for a pass (every list the pass recorded);
    // travels with the span to the profiler.
    void     AnnotateRegion(uint32_t regionIndex, const Diag::Prof::GpuCounters& counters);

private:
    struct FrameSlot
    {
        Microsoft::WRL::ComPtr<ID3D12Resource>  readbackBuffer;
        uint64_t*    mappedReadback   = nullptr;
        const char*  regionNames [kMaxRegions] {};
        uint8_t      regionQueues[kMaxRegions] {};
        Diag::Prof::GpuCounters regionCounters[kMaxRegions] {};   // AnnotateRegion
        uint32_t     regionCount      = 0; // snapshot taken at ResolveQueries
        uint32_t     regionsDropped   = 0;
        // Stamped at ResolveQueries: BeginFrame(slot) reads what ResolveQueries wrote on
        // the previous visit to this slot, kFrameCount (3) frames earlier, and without the
        // stamp a consumer would publish that as the current frame's cost - plausible, and
        // attributed to the wrong frame.
        uint64_t     resolveSeq       = 0;   // m_resolveSeq when this slot was written
        uint64_t     renderFrame      = ~0ull; // engine frame measured (ResolveQueries)
        int64_t      submitFirstNs    = 0;     // NoteSubmit
        int64_t      submitLastNs     = 0;
    };

    // Map a raw timestamp on queue q into the shared nanosecond timeline.
    double TicksToSharedNs(uint64_t t, uint8_t q) const;
    // Maps a raw timestamp on queue q into steady_clock nanoseconds (the engine
    // profiler's time base).
    int64_t TicksToSteadyNs(uint64_t t, uint8_t q) const;

    void   RefreshCalibration();

public:
    // ---- Results (valid after BeginFrame, from an earlier frame) ----
    TimingResult results[kMaxRegions];
    uint32_t     resultCount = 0;

    // totalGpuMs: naive sum of every region. It double-counts nested regions; do not
    // use it as a frame cost.
    float        totalGpuMs              = 0.f;
    // Union of queue q's own intervals. Nesting-proof: a child region inside a
    // parent adds nothing.
    float        perQueueTotalMs[kQueueCount] {};
    // Union across ALL queues = real GPU busy time for the frame.
    float        effectiveFrameMs        = 0.f;
    // sum(perQueueTotalMs) - effectiveFrameMs = time two or more queues were
    // busy simultaneously. ~0 means async compute is not actually async.
    float        overlapMs               = 0.f;
    // False when the adapter would not give a clock calibration; effectiveFrameMs
    // then degrades to max-of-per-queue-unions and overlapMs is meaningless.
    bool         calibrationValid        = false;
    bool         hasResults              = false;
    // How many profiler frames ago the current results were measured. An age because
    // GraphicsDX12::m_frameIndex is a slot index (0..kFrameCount-1), not an absolute
    // counter; a consumer subtracts it from its own frame index. Normally kFrameCount
    // (3); larger if a frame skipped a resolve.
    uint32_t     resultsAgeFrames        = 0;
    uint64_t     resultsSequence         = 0; // fence-completed resolve identity

    bool         enabled                 = false;

private:
    // One readback + name / queue snapshot per in-flight frame. The live counter
    // (m_regionCount) is shared (atomic across workers) and snapshotted into the slot
    // at ResolveQueries.
    FrameSlot              m_slots[kFrameCount];
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_queryHeap;
    uint64_t               m_queueFrequency[kQueueCount]{ 1, 1, 1 };
    // Monotonic count of ResolveQueries calls == profiler frames. Used only
    // to derive resultsAgeFrames; never exposed directly.
    uint64_t               m_resolveSeq = 0;

    // Cross-queue clock calibration, refreshed every BeginFrame. Each queue's
    // timestamp counter is free-running with its own base, so a per-queue
    // (gpuTick, qpcTick) correlated pair is what lets timestamps from different
    // queues be compared. Refreshed per frame because the two clocks drift;
    // over the ~2 frames between recording and readback the residual error is
    // nanoseconds.
    ID3D12CommandQueue*    m_queues[kQueueCount] { nullptr, nullptr, nullptr };
    uint64_t               m_calGpu[kQueueCount] {};
    uint64_t               m_calQpc[kQueueCount] {};
    uint64_t               m_qpcFrequency = 1;

    // Built in BeginFrame and handed to Diag::Prof::SubmitGpuFrame. A member,
    // not a local: it carries up to kMaxRegions spans (~24 KB).
    Diag::Prof::GpuFrameData m_profFrame;

    // Live counter: written by BeginTimestamp on any worker thread, snapshotted into
    // the current slot at ResolveQueries.
    std::atomic<uint32_t>  m_regionCount{ 0 };

    // Slot currently being recorded. Set at BeginFrame, used by
    // BeginTimestamp (writes to slot's name/queue arrays) and ResolveQueries
    // (snapshots regionCount into slot).
    uint32_t               m_recordSlot   = 0;
};
