#include "System/RunWatchdog.h"
#include "System/CrashReport.h"
#include "System/PathUtil.h"
#include "System/Log.h"
#include "Harness/ExitCodes.h"
#include "Harness/RunLedger.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

namespace Sys
{
namespace
{
    std::mutex              g_mutex;
    std::condition_variable g_cv;
    std::thread             g_thread;
    bool                    g_running   = false;   // guarded by g_mutex
    bool                    g_cancelled = false;   // guarded by g_mutex
    std::chrono::steady_clock::time_point g_deadline;
    std::string             g_dumpPath;            // guarded by g_mutex

    std::string DefaultDumpPath()
    {
        SYSTEMTIME st = {};
        ::GetLocalTime(&st);
        char stamp[48];
        std::snprintf(stamp, sizeof(stamp), "/hang_%04u%02u%02u_%02u%02u%02u.dmp",
                      static_cast<unsigned>(st.wYear),   static_cast<unsigned>(st.wMonth),
                      static_cast<unsigned>(st.wDay),    static_cast<unsigned>(st.wHour),
                      static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond));

        // Sys::LogDir(): the artifact root in a harness run (log, dump and verdict in one
        // directory), <writable>/log otherwise. Do not hand-roll the choice: the dump would drift
        // away from the log.
        return Sys::LogDir() + stamp;
    }

    void WatchdogMain()
    {
        std::unique_lock<std::mutex> lock(g_mutex);

        // wait_until in a loop returns on the deadline, a cancel, or a re-arm; re-reading
        // g_deadline each pass lets StartRunWatchdog be called twice without a second thread.
        for (;;)
        {
            if (g_cancelled) return;
            const auto deadline = g_deadline;
            if (g_cv.wait_until(lock, deadline) == std::cv_status::timeout)
            {
                if (g_cancelled)               return;   // cancelled in the race window
                if (std::chrono::steady_clock::now() < g_deadline) continue;  // re-armed
                break;                                   // genuinely expired
            }
        }

        const std::string dumpPath = g_dumpPath.empty() ? DefaultDumpPath() : g_dumpPath;
        lock.unlock();

        // The process dies from here on; leave enough to explain why, most likely to succeed
        // first.
        LOG_ERROR("WATCHDOG: run exceeded its time budget — the main thread has "
                  "not reached a frame boundary. Killing the process.");
        Logger::Get().FlushBlocking();

        // Stacks first: they name the wedged wait, which no external timeout can.
        Sys::CrashReport::LogAllThreadStacks("watchdog timeout");
        Logger::Get().FlushBlocking();

        bool wrote = Sys::CrashReport::WriteMinidump(dumpPath, nullptr, /*allThreadMemory*/ true);
        if (wrote) LOG_ERROR("WATCHDOG: hang dump written to %s", dumpPath.c_str());
        else       LOG_ERROR("WATCHDOG: FAILED to write hang dump to %s (GetLastError=%lu)",
                             dumpPath.c_str(), ::GetLastError());

        LOG_ERROR("WATCHDOG: exiting with code %d (%s)",
                  static_cast<int>(Harness::WatchdogTimeout),
                  Harness::ExitCodeName(Harness::WatchdogTimeout));
        Logger::Get().FlushBlocking();

        // The verdict, from the heap-free writer: without it the runner reports "no run.json -
        // crash before boot" and loses run.json fields such as "vram".
        Harness::RunLedger::Get().WriteCrashJson(
            static_cast<int>(Harness::WatchdogTimeout),
            "watchdog_timeout: the run outlived -maxseconds (all-thread stacks in DX12Log.txt)");

        // TerminateProcess, not exit(): the main thread is wedged, and exit() would run static
        // destructors on this thread while the main thread holds engine locks, hanging the
        // shutdown.
        ::TerminateProcess(::GetCurrentProcess(),
                           static_cast<UINT>(Harness::WatchdogTimeout));
    }
}

void StartRunWatchdog(double seconds, const std::string& dumpPath)
{
    if (seconds <= 0.0) return;   // not armed: no thread, no cost

    std::lock_guard<std::mutex> lock(g_mutex);
    g_deadline = std::chrono::steady_clock::now() +
                 std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                     std::chrono::duration<double>(seconds));
    g_dumpPath = dumpPath;

    if (g_running)
    {
        // Re-arm: move the deadline and wake the waiter.
        g_cv.notify_all();
        return;
    }

    g_running   = true;
    g_cancelled = false;
    g_thread    = std::thread(WatchdogMain);

    // Detached on purpose: the watchdog either kills the process or is cancelled at
    // shutdown; joining a thread that fires when the main thread is stuck would deadlock the
    // clean path.
    g_thread.detach();

    LOG_INFO("Watchdog armed: %.1f s", seconds);
}

void ExtendRunWatchdog(double seconds)
{
    if (seconds <= 0.0) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_running) return;      // never armed: nothing to extend
        g_deadline = std::chrono::steady_clock::now() +
                     std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                         std::chrono::duration<double>(seconds));
    }
    // Wake the waiter so WatchdogMain re-reads the deadline instead of firing on the old one.
    g_cv.notify_all();
}

void StopRunWatchdog()
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_running) return;
        g_cancelled = true;
    }
    g_cv.notify_all();
}

}  // namespace Sys
