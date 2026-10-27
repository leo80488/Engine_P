#pragma once

// GPU timestamp results as the device hands them out: IGraphicsDevice::GetGPUTimings
// fills a view of the profiler's own arrays, so a caller reads the regions without
// the D3D12 profiler. GPUProfiler's result record is GpuTimingResult.

#include <cstdint>

namespace RHI
{
    // One resolved timestamp region.
    struct GpuTimingResult
    {
        const char* name      = nullptr;
        float       gpuMs     = 0.f;
        uint8_t     queueType = 0;   // RHI::QueueType: GRAPHICS 0, COMPUTE 1, COPY 2
    };

    // The last resolved frame of regions, valid until the profiler's next
    // BeginFrame - read it where the device frame runs. The field names are
    // GPUProfiler's.
    struct GpuTimingView
    {
        const GpuTimingResult* results          = nullptr;
        uint32_t               resultCount      = 0;
        uint32_t               resultsAgeFrames = 0;   // how many profiler frames ago they were measured
        uint64_t               resultsSequence  = 0;   // fence-completed resolve identity
        bool                   enabled          = false;
        bool                   hasResults       = false;
        bool                   calibrationValid = false;   // the queues' clocks share a timeline
    };
}
