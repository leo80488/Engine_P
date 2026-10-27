#include "System/BootTimeline.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstring>

namespace Sys::Boot
{
namespace
{
    Phase                    g_phases[kMaxPhases];
    std::atomic<std::size_t> g_count{ 0 };
    // Slot 0 is SetMarkListener's (the boot splash); the others are AddMarkListener's.
    constexpr std::size_t     kListenerSlots = 4;
    std::atomic<MarkListener> g_listeners[kListenerSlots];

    // Process creation as a FILETIME tick count (100 ns). Read once; falls back
    // to the first call's own time when GetProcessTimes is unavailable, which
    // loses only the loader's share.
    long long CreationTicks()
    {
        static const long long s_ticks = []
        {
            FILETIME created{}, exited{}, kernel{}, user{};
            if (::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user))
                return (static_cast<long long>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
            FILETIME now{};
            ::GetSystemTimePreciseAsFileTime(&now);
            return (static_cast<long long>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
        }();
        return s_ticks;
    }
}

double NowMs()
{
    FILETIME now{};
    ::GetSystemTimePreciseAsFileTime(&now);
    const long long t = (static_cast<long long>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    return static_cast<double>(t - CreationTicks()) / 10000.0;
}

void Mark(const char* name)
{
    if (!name) return;
    const double ms = NowMs();
    const std::size_t n = g_count.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < n; ++i)
        if (std::strcmp(g_phases[i].name, name) == 0) return;   // first mark wins
    if (n >= kMaxPhases) return;
    g_phases[n] = Phase{ name, ms };
    g_count.store(n + 1, std::memory_order_release);
    for (std::atomic<MarkListener>& slot : g_listeners)
        if (MarkListener listener = slot.load(std::memory_order_acquire)) listener(name);
}

void SetMarkListener(MarkListener listener)
{
    g_listeners[0].store(listener, std::memory_order_release);
}

bool AddMarkListener(MarkListener listener)
{
    for (std::size_t i = 1; i < kListenerSlots; ++i)
    {
        MarkListener expected = nullptr;
        if (g_listeners[i].compare_exchange_strong(expected, listener, std::memory_order_acq_rel))
            return true;
    }
    return false;
}

void RemoveMarkListener(MarkListener listener)
{
    for (std::size_t i = 1; i < kListenerSlots; ++i)
    {
        MarkListener expected = listener;
        g_listeners[i].compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
    }
}

std::size_t Snapshot(Phase* out, std::size_t max)
{
    const std::size_t n = g_count.load(std::memory_order_acquire);
    const std::size_t k = n < max ? n : max;
    for (std::size_t i = 0; i < k; ++i) out[i] = g_phases[i];
    return k;
}
}
