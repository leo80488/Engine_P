#pragma once
// The only unconditional bound on a run's wall time. -frames=N bounds a run that still
// ticks; this catches one that stops: every fence wait is WaitForSingleObject(INFINITE),
// so a wedged GPU, an unresolved TDR or a scheduler/worker deadlock leaves a live process
// that never reaches another frame boundary. An external kill would leave no log flush,
// no dump, no exit code and no hint where it stopped.
// On fire: log the timeout, log every other thread's symbolized stack (CrashReport - names
// the wedged wait), write a full-memory hang minidump next to the log, write the crash
// run.json, then TerminateProcess(ExitCode::WatchdogTimeout). The hard kill is correct
// only here: the main thread is wedged, so ShutdownAllSystems is unreachable; everywhere
// else PostQuitMessage keeps ShutdownAllSystems and SavePSOLibrary running.
// Costs nothing unless StartRunWatchdog gets a positive timeout.
// Timeout: boot to the first frame takes ~20 s (device, shaders, PSO warm, scene load), so
// below ~30 s it reports a cold boot as a hang. Budget the scene load, not the frame count.

#include <string>

namespace Sys
{
    /// Arms the watchdog. @p seconds <= 0 is a no-op (no thread). @p dumpPath is where the
    /// hang minidump goes; empty picks a timestamped name under LogDir(). Calling again moves
    /// the deadline instead of starting a second thread.
    void StartRunWatchdog(double seconds, const std::string& dumpPath = {});

    /// Disarms. Called on every orderly exit path, so a slow clean shutdown (PSO cache save,
    /// teardown) is never taken for a hang. Safe when never started.
    void StopRunWatchdog();

    /// Moves the deadline to @p seconds from now. ReportFatal calls this first: a minidump
    /// plus stack symbolization can take seconds, and a watchdog firing inside
    /// MiniDumpWriteDump would replace a diagnosable crash with a bare "watchdog_timeout".
    /// It also keeps the two dumps apart (dbghelp is not thread-safe). Extending rather than
    /// cancelling keeps the watchdog armed in case the crash path itself wedges.
    void ExtendRunWatchdog(double seconds);
}
