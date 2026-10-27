#pragma once
// System/CrashReport.h - minidumps and in-process symbolized stack traces.
//
// An automated caller has no cdb / windbg, so a .dmp alone only says something
// died. The stack is therefore symbolized inside the dying process, where dbghelp
// still sees the loaded modules and the PDBs next to the exe, and each frame goes
// to DX12Log.txt as a `CRASH-STACK:` line. The dump is still written for a human
// with a debugger.
//
// Hangs: a fence wait can block with no timeout, so a wedged GPU is a process that
// simply stops. LogAllThreadStacks() suspends and walks every other thread, so the
// wedged wait appears by name in the log.
//
// Thread safety: dbghelp's Sym* family is not thread-safe. Every entry point takes
// one process-wide lock, so two threads dying at once serialize.
//
// Crash-path discipline: these run when the heap may be corrupt. They allocate as
// little as possible (fixed stack buffers for symbol lookup, no std::string
// building in the walk), every step degrades to "print the raw address" rather
// than failing the trace, and none of them throws.

#include <string>

struct _EXCEPTION_POINTERS;

namespace Sys
{
namespace CrashReport
{
    // Resolve dbghelp and load symbols for the process. Safe to call more than
    // once; the first call wins. Called lazily by the functions below, so an
    // explicit call is only needed to pay the cost early (symbol load can take
    // a second) rather than on the dying path.
    void Initialize();

    // Resolve one code address to `module!symbol + 0xNN (file:line)`, or a
    // best-effort subset when the PDB does not cover it. For DIAGNOSTIC code
    // that captured a return address and needs to name its caller: a raw
    // address does not survive ASLR, so it cannot be resolved after the run.
    //
    // Takes the process-wide dbghelp lock and may pay a one-second symbol load
    // on the first call. Audit paths only, never a per-frame one.
    std::string SymbolizeAddress(const void* addr);

    // Write a minidump to @p utf8Path; returns true if the file was written.
    // @p ep may be null (the terminate and pure-call paths have no exception
    // record; the dump still carries every thread's stack). @p allThreadMemory
    // is true for a HANG dump: full thread stacks and referenced memory for
    // every thread, not just the faulting one.
    bool WriteMinidump(const std::string& utf8Path,
                       _EXCEPTION_POINTERS* ep,
                       bool allThreadMemory = false);

    // Walk and LOG_ERROR the calling thread's stack, one `CRASH-STACK:` line
    // per frame, innermost first, as `module!symbol + 0xNN (file:line)`.
    // @p ep supplies the faulting context; pass null to walk from here.
    // @p threadId names the thread @p ep belongs to when that is NOT the
    // calling one (a sibling that faulted too and is parked in the crash
    // handler); 0 = the calling thread.
    void LogStackTrace(_EXCEPTION_POINTERS* ep, const char* header = nullptr,
                       unsigned long threadId = 0);

    // Suspend every other thread in this process, walk each one, and log its
    // frames under a `HANG-STACK[tid]:` prefix, then the calling thread's own
    // stack as `CRASH-STACK:` lines. For hangs and deadlocks, where the
    // interesting stack is never the one that noticed the problem.
    //
    // Deadlock: frames are collected into memory while threads are suspended,
    // ALL threads are resumed, and only then is anything logged. Do not log
    // during the walk: LOG_ERROR takes a mutex the async logger's worker may
    // hold, and that worker is one of the suspended threads.
    void LogAllThreadStacks(const char* header = nullptr);
}
}
