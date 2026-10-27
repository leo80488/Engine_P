#include "System/ProcessMemory.h"

#include "System/Log.h"

#include <atomic>
#include <psapi.h>   // GetProcessMemoryInfo (kernel32 K32 export; no extra lib); windows.h via the PCH

namespace Sys
{
    namespace
    {
        std::atomic<VideoMemoryProbe> g_vramProbe{ nullptr };
    }

    void SetVideoMemoryProbe(VideoMemoryProbe probe)
    {
        g_vramProbe.store(probe, std::memory_order_release);
    }

    ProcessMemory QueryProcessMemory()
    {
        ProcessMemory out;
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        if (::GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                                   sizeof(pmc)))
        {
            out.valid          = true;
            out.privateBytes   = pmc.PrivateUsage;
            out.workingSet     = pmc.WorkingSetSize;
            out.peakWorkingSet = pmc.PeakWorkingSetSize;
        }
        return out;
    }

    void LogProcessMemory(const char* tag)
    {
        const ProcessMemory m = QueryProcessMemory();
        if (!m.valid)
        {
            LOG_INFO("[MEM] %s: unavailable", tag);
            return;
        }
        constexpr double kMiB = 1024.0 * 1024.0;
        uint64_t usage = 0, budget = 0;
        const VideoMemoryProbe probe = g_vramProbe.load(std::memory_order_acquire);
        if (!probe || !probe(usage, budget) || !budget)
        {
            LOG_INFO("[MEM] %s: private=%.1f MiB ws=%.1f MiB peakWs=%.1f MiB", tag,
                     m.privateBytes / kMiB, m.workingSet / kMiB, m.peakWorkingSet / kMiB);
            return;
        }
        LOG_INFO("[MEM] %s: private=%.1f MiB ws=%.1f MiB peakWs=%.1f MiB vram=%.1f/%.1f MiB", tag,
                 m.privateBytes / kMiB, m.workingSet / kMiB, m.peakWorkingSet / kMiB,
                 usage / kMiB, budget / kMiB);
    }
}
