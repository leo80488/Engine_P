#pragma once

// The process's own memory, in every configuration (Retail included: the profiler's counters are
// compiled out there). For before / after measurements of a packaged build: a [MEM] line at the
// moments the engine logs one (after the pak mounts, the first frame, each scene load).

#include <cstdint>

namespace Sys
{
    struct ProcessMemory
    {
        bool     valid          = false;
        uint64_t privateBytes   = 0;   // committed private memory (PrivateUsage)
        uint64_t workingSet     = 0;   // resident now
        uint64_t peakWorkingSet = 0;
    };

    ProcessMemory QueryProcessMemory();

    // How to read the process's local video memory now (bytes in use, the OS budget); false when it cannot be.
    // The device installs it (VideoMemory::Sample), so System does not depend on Graphics.
    using VideoMemoryProbe = bool (*)(uint64_t& usageBytes, uint64_t& budgetBytes);
    void SetVideoMemoryProbe(VideoMemoryProbe probe);

    // Logs "[MEM] <tag>: private=<MiB> ws=<MiB> peakWs=<MiB>" and, once the device has sampled it,
    // " vram=<MiB>/<MiB>" (local usage / budget).
    void LogProcessMemory(const char* tag);
}
