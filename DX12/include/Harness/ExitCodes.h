#pragma once
// Harness/ExitCodes.h - the process exit-code taxonomy.
//
// The exit code is the one channel every caller gets for free, so the common
// failures are self-describing before anyone opens a log: "ran perfectly",
// "booted an empty world because the scene name was wrong" and "killed by the
// watchdog after hanging" each have their own code. App::Run returns the
// WM_QUIT wParam, so PostQuitMessage carries these values. run.json records the
// same value plus a reason string; this header is the single place the numbers
// are defined, so the two cannot drift.
//
// Range: values stay under 128 and avoid 2 (shell "misuse of builtin") and
// 126/127 (shell "cannot execute"). Codes >= 10 are harness verdicts; 0/1/3
// mean what they mean in every build, so scripts that test them keep working.
//
// SEH: a process killed by an unhandled SEH exception that gets past the crash
// filter reports the raw exception code (e.g. 0xC0000005), far outside this
// range. Treat anything not listed here as "died hard" and go to the log +
// minidump.

namespace Harness
{
    enum ExitCode : int
    {
        // ---- General outcomes (every build) ----
        /// Ran to completion; no verdict was violated.
        Ok               = 0,
        /// A C++ exception escaped App::Run.
        Exception        = 1,
        /// std::terminate / pure-virtual call. Set by _exit(3).
        Terminate        = 3,

        // ---- Harness verdicts ----
        /// A scenario's Test.Expect(...) failed.
        AssertionFailed  = 10,
        /// Logged errors exceeded -maxerrors=N.
        ErrorBudget      = 11,
        /// The run could not be SET UP as asked: an unresolvable -scene=, an unreadable
        /// -exec= script, an artifact dir that cannot be made. Distinct from a failed
        /// assertion on purpose: nothing was tested.
        PreconditionFail = 12,
        /// -maxseconds elapsed; the process was killed from outside the main thread.
        WatchdogTimeout  = 13,
        /// -require_draws was set and a frame submitted nothing: the signature of a Hi-Z
        /// false cull (black window, black screenshot, zero ERROR lines, exit 0).
        NoDraws          = 14,
        /// DXGI_ERROR_DEVICE_REMOVED / TDR. Separate from Exception: it is what an
        /// automated run hits most often when something else competes for the GPU, and it
        /// says nothing about the code under test.
        DeviceLost       = 15,
    };

    /// Human-readable name for a code, for run.json and the exit log line.
    /// Returns "unknown" for anything not in the enum (including raw SEH codes).
    inline const char* ExitCodeName(int code)
    {
        switch (code)
        {
            case Ok:               return "ok";
            case Exception:        return "exception";
            case Terminate:        return "terminate";
            case AssertionFailed:  return "assertion_failed";
            case ErrorBudget:      return "error_budget";
            case PreconditionFail: return "precondition_failed";
            case WatchdogTimeout:  return "watchdog_timeout";
            case NoDraws:          return "no_draws";
            case DeviceLost:       return "device_lost";
            default:               return "unknown";
        }
    }
}
