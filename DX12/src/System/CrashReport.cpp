#include "System/CrashReport.h"
#include "System/PathUtil.h"
#include "System/Log.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <DbgHelp.h>
#include <tlhelp32.h>

#include <cstdio>
#include <mutex>
#include <vector>

#pragma comment(lib, "Dbghelp.lib")

namespace Sys
{
namespace CrashReport
{
namespace
{
    // dbghelp's Sym* family is not thread-safe. One process-wide lock around
    // every entry point: two threads dying at once serialize instead of
    // corrupting dbghelp's state and losing both traces.
    std::mutex g_symMutex;
    bool       g_symInitialized = false;

    void EnsureSymInitialized()
    {
        if (g_symInitialized) return;

        // UNDNAME + LOAD_LINES: without them every frame reads as a mangled name
        // with no source location. DEFERRED_LOADS keeps startup cheap: a module's
        // symbols are parsed only when a frame lands in it.
        ::SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS);

        // TRUE = invade the process: dbghelp enumerates the already-loaded
        // modules itself, so the PDBs sitting next to the exe are found without
        // us building a search path.
        g_symInitialized = ::SymInitialize(::GetCurrentProcess(), nullptr, TRUE) != FALSE;
        if (!g_symInitialized)
        {
            // Do not LOG here: this can run on the crash path before the caller
            // has decided what to say. The caller reports the empty trace.
        }
    }

    // Format one frame into @p out. Fixed buffers, no std::string building in
    // the hot part: this runs when the heap may already be the thing that died.
    void FormatFrame(DWORD64 addr, char* out, size_t outSize)
    {
        HANDLE proc = ::GetCurrentProcess();

        // SYMBOL_INFO is variable-length; the name follows the struct.
        alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 512] = {};
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen   = 511;

        DWORD64 symDisp = 0;
        const bool haveSym = ::SymFromAddr(proc, addr, &symDisp, sym) != FALSE;

        IMAGEHLP_LINE64 line = {};
        line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
        DWORD lineDisp = 0;
        const bool haveLine = ::SymGetLineFromAddr64(proc, addr, &lineDisp, &line) != FALSE;

        // The module name orients a frame that has no symbol at all
        // (third-party DLLs ship without PDBs - d3d12.dll, the vendor upscalers).
        IMAGEHLP_MODULE64 mod = {};
        mod.SizeOfStruct = sizeof(IMAGEHLP_MODULE64);
        const bool haveMod = ::SymGetModuleInfo64(proc, addr, &mod) != FALSE;

        if (haveSym && haveLine)
            std::snprintf(out, outSize, "0x%016llX  %s!%s + 0x%llX  (%s:%lu)",
                          static_cast<unsigned long long>(addr),
                          haveMod ? mod.ModuleName : "?", sym->Name,
                          static_cast<unsigned long long>(symDisp),
                          line.FileName ? line.FileName : "?",
                          static_cast<unsigned long>(line.LineNumber));
        else if (haveSym)
            std::snprintf(out, outSize, "0x%016llX  %s!%s + 0x%llX",
                          static_cast<unsigned long long>(addr),
                          haveMod ? mod.ModuleName : "?", sym->Name,
                          static_cast<unsigned long long>(symDisp));
        else if (haveMod)
            // The absolute address dies with ASLR; module+offset can still be
            // resolved offline against the same build's PDB when this process
            // could not load it (a PDB being rewritten by a build, say).
            std::snprintf(out, outSize, "0x%016llX  %s!<no symbol> (%s+0x%llX)",
                          static_cast<unsigned long long>(addr), mod.ModuleName,
                          mod.ModuleName,
                          static_cast<unsigned long long>(addr - mod.BaseOfImage));
        else
            std::snprintf(out, outSize, "0x%016llX  ?!<no symbol>",
                          static_cast<unsigned long long>(addr));
    }

    // Walk one thread. @p ctx is modified by StackWalk64, so callers pass a copy.
    // Frames are appended to @p out as formatted strings.
    //
    // Caller must hold g_symMutex.
    void WalkThread(HANDLE thread, CONTEXT& ctx, std::vector<std::string>& out,
                    int maxFrames = 64)
    {
        STACKFRAME64 frame = {};
        frame.AddrPC.Offset    = ctx.Rip;   // x64 only: the engine has no x86 config
        frame.AddrPC.Mode      = AddrModeFlat;
        frame.AddrFrame.Offset = ctx.Rbp;
        frame.AddrFrame.Mode   = AddrModeFlat;
        frame.AddrStack.Offset = ctx.Rsp;
        frame.AddrStack.Mode   = AddrModeFlat;

        HANDLE proc = ::GetCurrentProcess();
        char   buf[1024];

        for (int i = 0; i < maxFrames; ++i)
        {
            if (!::StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thread, &frame, &ctx,
                               nullptr, ::SymFunctionTableAccess64,
                               ::SymGetModuleBase64, nullptr))
                break;
            if (frame.AddrPC.Offset == 0)
                break;

            FormatFrame(frame.AddrPC.Offset, buf, sizeof(buf));
            out.emplace_back(buf);
        }
    }
}  // namespace

void Initialize()
{
    std::lock_guard<std::mutex> lock(g_symMutex);
    EnsureSymInitialized();
}

std::string SymbolizeAddress(const void* addr)
{
    // Same dbghelp path as the crash walker; audit paths only (see the declaration).
    std::lock_guard<std::mutex> lock(g_symMutex);
    EnsureSymInitialized();
    if (!g_symInitialized)
        return "<symbols unavailable>";
    char buf[1024];
    FormatFrame(reinterpret_cast<DWORD64>(addr), buf, sizeof(buf));
    return buf;
}

bool WriteMinidump(const std::string& utf8Path, _EXCEPTION_POINTERS* ep,
                   bool allThreadMemory)
{
    // CreateFileW + Utf8ToWide, not fopen: the path runs through the artifact
    // root or %LOCALAPPDATA%, i.e. through the user's account name, which is
    // routinely non-ASCII. A narrow open would silently fail exactly for the
    // users least able to work around it.
    Sys::CreateParentDirs(utf8Path);
    const std::wstring widePath = Sys::Utf8ToWide(utf8Path);
    HANDLE file = ::CreateFileW(widePath.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    MINIDUMP_EXCEPTION_INFORMATION mei = {};
    mei.ThreadId          = ::GetCurrentThreadId();
    mei.ExceptionPointers = reinterpret_cast<PEXCEPTION_POINTERS>(ep);
    mei.ClientPointers    = FALSE;

    // IndirectlyReferencedMemory follows pointers found on the stacks, so the
    // objects the crashing frames were working on (entity arrays, descriptor
    // allocations, render-pass state) land in the dump instead of showing as
    // <unavailable>. DataSegs brings the globals/singletons, ThreadInfo keeps
    // every worker's context because the real culprit is frequently a thread
    // other than the one that faulted.
    int type = MiniDumpWithIndirectlyReferencedMemory |
               MiniDumpWithDataSegs |
               MiniDumpWithThreadInfo;

    // A HANG has no faulting thread - the answer is in some other thread's wait.
    // Full stack memory for every thread costs size but is the only way to see
    // which fence, which lock, which queue.
    if (allThreadMemory)
        type |= MiniDumpWithFullMemoryInfo | MiniDumpWithProcessThreadData;

    const BOOL ok = ::MiniDumpWriteDump(::GetCurrentProcess(), ::GetCurrentProcessId(),
                                        file, static_cast<MINIDUMP_TYPE>(type),
                                        ep ? &mei : nullptr, nullptr, nullptr);
    ::CloseHandle(file);
    return ok != FALSE;
}

void LogStackTrace(_EXCEPTION_POINTERS* ep, const char* header, unsigned long threadId)
{
    std::vector<std::string> frames;
    {
        std::lock_guard<std::mutex> lock(g_symMutex);
        EnsureSymInitialized();
        if (!g_symInitialized)
        {
            LOG_ERROR("CRASH-STACK: unavailable (SymInitialize failed, GetLastError=%lu)",
                      ::GetLastError());
            return;
        }

        CONTEXT ctx = {};
        auto* pep = reinterpret_cast<PEXCEPTION_POINTERS>(ep);
        if (pep && pep->ContextRecord)
        {
            ctx = *pep->ContextRecord;
        }
        else
        {
            // No exception record (terminate / pure-call / an explicit request).
            // RtlCaptureContext gives this thread's current context; the first
            // few frames will be inside this file, which is harmless.
            ::RtlCaptureContext(&ctx);
        }

        // GetCurrentThread() is a pseudo-handle meaning "the calling thread",
        // which is exactly right here and needs no CloseHandle. Another
        // thread's context is walked with that thread's own handle.
        HANDLE other = threadId ? ::OpenThread(THREAD_QUERY_INFORMATION | THREAD_GET_CONTEXT,
                                               FALSE, threadId)
                                : nullptr;
        WalkThread(other ? other : ::GetCurrentThread(), ctx, frames);
        if (other) ::CloseHandle(other);
    }

    if (header) LOG_ERROR("CRASH-STACK: %s", header);
    if (frames.empty())
    {
        LOG_ERROR("CRASH-STACK: <no frames recovered>");
        return;
    }
    for (size_t i = 0; i < frames.size(); ++i)
        LOG_ERROR("CRASH-STACK: [%02zu] %s", i, frames[i].c_str());
}

void LogAllThreadStacks(const char* header)
{
    const DWORD selfPid = ::GetCurrentProcessId();
    const DWORD selfTid = ::GetCurrentThreadId();

    // tid -> its frames. Collected while threads are suspended; logged only
    // after every one has been resumed: LOG_ERROR takes a mutex the (suspended)
    // logger worker may hold.
    std::vector<std::pair<DWORD, std::vector<std::string>>> perThread;

    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
    {
        LOG_ERROR("HANG-STACK: cannot enumerate threads (GetLastError=%lu)",
                  ::GetLastError());
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_symMutex);
        EnsureSymInitialized();

        THREADENTRY32 te = {};
        te.dwSize = sizeof(te);
        if (::Thread32First(snap, &te))
        {
            do
            {
                if (te.th32OwnerProcessID != selfPid) continue;
                if (te.th32ThreadID == selfTid)       continue;   // walked separately

                HANDLE th = ::OpenThread(
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                    FALSE, te.th32ThreadID);
                if (!th) continue;

                std::vector<std::string> frames;
                if (::SuspendThread(th) != static_cast<DWORD>(-1))
                {
                    CONTEXT ctx = {};
                    ctx.ContextFlags = CONTEXT_FULL;
                    if (::GetThreadContext(th, &ctx))
                        WalkThread(th, ctx, frames);

                    ::ResumeThread(th);
                }
                ::CloseHandle(th);

                if (!frames.empty())
                    perThread.emplace_back(te.th32ThreadID, std::move(frames));
            }
            while (::Thread32Next(snap, &te));
        }
    }
    ::CloseHandle(snap);

    // Every thread is running again: safe to talk to the async logger now.
    if (header) LOG_ERROR("HANG-STACK: %s", header);
    LOG_ERROR("HANG-STACK: %zu other thread(s) walked; this thread is %lu",
              perThread.size(), static_cast<unsigned long>(selfTid));

    for (const auto& t : perThread)
        for (size_t i = 0; i < t.second.size(); ++i)
            LOG_ERROR("HANG-STACK[%lu]: [%02zu] %s",
                      static_cast<unsigned long>(t.first), i, t.second[i].c_str());

    // And this thread, which by construction was not in the snapshot walk.
    LogStackTrace(nullptr, "watchdog thread (the one that noticed, not the cause)");
}

}  // namespace CrashReport
}  // namespace Sys
