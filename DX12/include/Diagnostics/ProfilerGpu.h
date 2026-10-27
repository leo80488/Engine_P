#pragma once

// Diag::Prof GPU timeline.
//
// SOURCE. GPUProfiler::BeginFrame hands its calibrated timestamp regions (begin/end, queue,
// depth) here through SubmitGpuFrame whenever the engine profiler is on.
//
// ONE TIME BASE. Every span is converted to steady_clock nanoseconds - the QPC-derived epoch
// every CPU scope in the ring uses - through the queue's own GetClockCalibration pair and the
// same integer QPC->ns formula MSVC's steady_clock applies. So a GPU pass and the CPU scope
// that recorded it are comparable in one trace, and a GPU "start" can be compared with the
// CPU instant the frame was submitted.
//
// WHICH FRAME. A GPU frame is keyed by the RENDER frame whose command lists it measured
// (stamped at ResolveQueries), not by when the numbers came back - they come back
// GPUProfiler::kFrameCount frames later (gpuAgeFrames). That frame's record is patched when
// they arrive.
//
// WHAT IS DERIVED, and why it needs the NEXT frame. busy / per-queue / overlap are this
// frame's alone. The rest compares frame F with F+1:
//   periodMs    first span of F+1 - first span of F   (the GPU's own frame period)
//   util        busy(F) / period(F)
//   starveMs    max(0, first(F+1) - last(F))           (GPU idle between frames)
//   submitLate  CPU submitted F+1 after the GPU had finished F: the GPU waited for the CPU
//   bound       gpu | cpu | pacing | mixed, per frame, from the above
// The envelope is the first/last INSTRUMENTED region, so un-instrumented GPU work outside it
// is invisible - reported, not hidden: bubbleMs is envelope minus busy.

#include "Diagnostics/Profiler.h"
#include "Diagnostics/GpuCounters.h"

namespace Diag::Prof
{
    constexpr uint32_t kMaxGpuSpans = 1024;   // == GPUProfiler::kMaxRegions
    constexpr uint32_t kGpuRing     = 32;     // GPU frames kept
    constexpr uint8_t  kGpuQueues   = 3;      // graphics, compute, copy

    struct GpuSpan
    {
        int64_t beginNs = 0;     // steady_clock ns
        int64_t endNs   = 0;
        NameId  name    = 0;
        uint8_t queue   = 0;     // 0 graphics, 1 compute, 2 copy
        uint8_t depth   = 0;     // nesting depth within its queue (DDGI wraps 8 children)
        // What the region's command lists recorded (valid when the device counted it: every
        // region GraphicsDX12 opens).
        GpuCounters counters;
    };

    struct GpuFrameData
    {
        bool     valid      = false;   // set by the submitter
        uint64_t frame      = 0;       // render frame measured
        uint64_t resolveSeq = 0;       // GPUProfiler resolve identity
        bool     calibrated = false;
        uint32_t regions    = 0;
        uint32_t dropped    = 0;       // regions past kMaxGpuSpans this frame
        float    busyMs     = 0.f;     // union over all queues
        float    queueMs[kGpuQueues]{};
        float    overlapMs  = 0.f;
        int64_t  firstNs    = 0;       // envelope of the instrumented regions
        int64_t  lastNs     = 0;
        int64_t  submitNs   = 0;       // CPU time of the frame's FIRST ExecuteCommandLists; 0 = unknown
        int64_t  submitLastNs = 0;     // ... and its last one
        uint32_t spanCount  = 0;
        GpuSpan  spans[kMaxGpuSpans];
    };

    // GPUProfiler::BeginFrame (render half). Copies the frame in and patches the
    // matching FrameRecord's GPU fields. No-op below Level::Stats.
    void SubmitGpuFrame(const GpuFrameData& data) noexcept;

    // Collector side. false when the frame is not (or no longer) in the ring.
    bool GetGpuFrame(uint64_t frame, GpuFrameData& out) noexcept;
    // Newest GPU frame received; ~0ull when none has arrived (GPU profiling
    // off, or not yet warmed up).
    uint64_t LatestGpuFrame() noexcept;

    // steady_clock::now() semantics for a raw QPC value: the collector and
    // GPUProfiler must convert calibration points EXACTLY the way the CPU clock
    // does, or GPU spans drift against CPU scopes by the rounding difference.
    int64_t QpcToSteadyNs(int64_t qpc) noexcept;
}
