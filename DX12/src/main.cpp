// wWinMain: process setup (host flag, argv, content and writable roots, logger, crash
// net, run watchdog, COM), App::Run, then ordered teardown. Compiled per exe (CLAUDE.md
// invariant 2): the one place the editor/game host decision is made.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <DbgHelp.h>
#include <werapi.h>     // WerSetFlags
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <csignal>      // SIGABRT
#include <cstdlib>      // _exit, _set_purecall_handler, _set_invalid_parameter_handler
#include <filesystem>
#include <exception>    // std::set_terminate
#include <stdexcept>
#include <string>
#include <thread>       // -crash_test_after_ms
#include <vector>

#include "System/Log.h"
#include "App.h"
#include "System/Exception_handle.h"
#include "System/PathUtil.h"
#include "System/HostPolicy.h"   // SetEditorHost - see the note in wWinMain
#include "Module/ModuleRegistry.h" // RegisterLinkedModules
#ifdef WITH_EDITOR
#include "Editor/EditorModule.h"   // RegisterLinkedEditorModules
#endif
#include "System/TaskSystem.h"
#include "System/CommandLine.h"  // -unattended / -artifacts / -maxseconds / -frames
#include "System/CrashReport.h"  // symbolized in-process stacks (no windbg on the box)
#include "System/RunWatchdog.h"  // the only unconditional bound on a run's wall time
#include "System/BootTimeline.h" // bootPhasesMs
#include "Harness/ExitCodes.h"
#include "Harness/RunLedger.h"
#include "Harness/Determinism.h"
#include "Graphics/GpuMemoryLedger.h"  // GpuLedger::LogResidual
#include "Harness/AssetTool.h"      // -assettool=<command>: the offline asset tool
#include "Resource/SharedContent.h" // a worktree's binary assets from the main checkout
#include "Resource/ContentRoots.h"  // MountContentRoots: before App builds the device
#include "System/BuildInfo.h"        // the Build line of the boot log
#include "System/PlayerText.h"       // the dialogs, in the player's language
#include "Platform/Splash.h"         // the boot splash

#pragma comment(lib, "Dbghelp.lib")
#pragma comment(lib, "wer.lib")

#ifdef WITH_EDITOR
// Headless UE-scene cook (no window / no D3D12 device needed):
//   Editor.exe -import_ue_scene=<path\to\scene_manifest.json>
//              [-import_out=<assetRoot>] [-import_name=<SceneName>]
//              [-import_no_mirror_bake]
// -import_no_mirror_bake keeps UE's negative scale on mirrored placements instead of
// baking a mirrored mesh variant - the renderer flips the front-face convention per
// draw, so this is the leaner (preferred) output.
// Exits with 0 on success, 2 on a failed import.
#include "Resource/UnrealSceneImporter.h"
#include "System/PathUtil.h"
#include <shellapi.h>
#include <string>
#pragma comment(lib, "shell32.lib")

// Returns the process exit code, or -1 when no headless import was requested.
static int RunHeadlessUeImportIfRequested()
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return -1;

    std::string manifest, outDir, name;
    bool bakeMirrors = true;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = Sys::WideToUtf8(argv[i]);
        if      (a.rfind("-import_ue_scene=", 0) == 0) manifest = a.substr(17);
        else if (a.rfind("-import_out=",      0) == 0) outDir   = a.substr(12);
        else if (a.rfind("-import_name=",     0) == 0) name     = a.substr(13);
        else if (a == "-import_no_mirror_bake")        bakeMirrors = false;
    }
    LocalFree(argv);
    if (manifest.empty()) return -1;

    LOG_INFO("Headless UE scene import: %s", manifest.c_str());
    Resource::UnrealSceneImporter::Options opt;
    opt.manifestPath = manifest;
    opt.outputDir    = outDir;   // empty -> "asset"
    opt.sceneName    = name;
    opt.bakeMirroredVariants = bakeMirrors;

    const auto res = Resource::UnrealSceneImporter::Run(opt, nullptr);
    return res.ok ? 0 : 2;
}
#endif // WITH_EDITOR

#ifdef _DEBUG
#include <wrl/client.h>
#include <dxgidebug.h>
#include <dxgi1_3.h>
#pragma comment(lib, "dxguid.lib")
using Microsoft::WRL::ComPtr;
#endif

// ==== Crash handling: log flush + minidump on the way down ====
// Access violations, stack overflow, pure-virtual calls and heap corruption are
// SEH/CRT faults, not C++ exceptions: wWinMain's try/catch never sees them, nor
// anything thrown on a TaskSystem worker (different stack). The Logger is
// asynchronous and flushes eagerly only on Error lines, so without this net the
// last lines before the crash die in its queue. The unhandled-exception filter
// (1) flushes the log synchronously, (2) writes a minidump next to it, and
// (3) tells the player where both files are, so a bug report can carry them.
// Limitation: on a stack overflow the filter runs on the overflowed stack with only
// the guard page left, so its std::string/MessageBox work can itself fail. wWinMain
// calls SetThreadStackGuarantee for the MAIN thread; workers get no such reserve.

// The crash folders a player's machine keeps (PruneCrashFolders, at boot).
static constexpr int kKeptCrashFolders = 10;

// Where the dump goes. With -artifacts=<dir>: beside the log there, which is one run's report
// already. A player's crash: a folder of its own under <log>/crashes/ - the dump, the log as it
// was and the build (ReportFatal), one folder to attach to a bug report. The root is resolved
// before any crash can happen (Logger::Initialize builds its path from it), so this does no
// Win32 known-folder work on the dying path; it only creates the two folders.
// @p outFolder: the crash folder ("" for an -artifacts run).
static std::string MakeCrashDumpPath(std::string& outFolder)
{
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char stamp[40];
    std::snprintf(stamp, sizeof(stamp), "crash_%04u%02u%02u_%02u%02u%02u",
                  static_cast<unsigned>(st.wYear),  static_cast<unsigned>(st.wMonth),
                  static_cast<unsigned>(st.wDay),   static_cast<unsigned>(st.wHour),
                  static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond));
    outFolder.clear();
    // LogDir(), not WritableDataDir(): with -artifacts=<dir> the log moves there, and
    // the dump must follow it - otherwise it lands in %LOCALAPPDATA% while the caller
    // collects the artifact directory.
    if (Sys::HasArtifactDir())
        return Sys::LogDir() + "/" + stamp + ".dmp";
    const std::string crashes = Sys::LogDir() + "/crashes";
    CreateDirectoryW(Sys::Utf8ToWide(crashes).c_str(), nullptr);
    outFolder = crashes + "/" + stamp;
    CreateDirectoryW(Sys::Utf8ToWide(outFolder).c_str(), nullptr);
    return outFolder + "/crash.dmp";
}

// The log as it was, its last lines at every level and the build, beside a player's dump (no
// -artifacts).
static void FillCrashFolder(const std::string& folder, const char* reason)
{
    const std::wstring logW = Sys::Utf8ToWide(Sys::LogDir() + "/DX12Log.txt");
    CopyFileW(logW.c_str(), Sys::Utf8ToWide(folder + "/DX12Log.txt").c_str(), FALSE);
    // A shipped build's file holds WARNING and up; the record ring has the INFO lines before the crash.
    std::vector<LogRecord> recent;
    if (Logger::Get().TryCopyRecent(2000, recent) && !recent.empty())
    {
        HANDLE f = CreateFileW(Sys::Utf8ToWide(folder + "/recent_log.txt").c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE)
        {
            for (const LogRecord& r : recent)
            {
                DWORD written = 0;
                WriteFile(f, r.text.data(), static_cast<DWORD>(r.text.size()), &written, nullptr);
                WriteFile(f, "\r\n", 2, &written, nullptr);
            }
            CloseHandle(f);
        }
    }
    const Sys::BuildInfo& b = Sys::Build();
    char text[512];
    const int n = std::snprintf(text, sizeof(text), "%s %s\r\ncommit %s%s\r\nbranch %s\r\nconfig %s\r\nbuild %s\r\n"
                                "reason %s\r\n", b.product, b.version, b.gitSha, b.dirty ? " (dirty)" : "", b.branch,
                                b.config, b.buildId, reason ? reason : "crash");
    if (n > 0 && n < static_cast<int>(sizeof(text)))
    {
        HANDLE f = CreateFileW(Sys::Utf8ToWide(folder + "/build.txt").c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            WriteFile(f, text, static_cast<DWORD>(n), &written, nullptr);
            CloseHandle(f);
        }
    }
}

// Keeps the newest kKeptCrashFolders under <log>/crashes; at boot, never on the crash path.
static void PruneCrashFolders()
{
    if (Sys::HasArtifactDir()) return;
    std::error_code ec;
    const std::filesystem::path root = Sys::U8Path(Sys::LogDir() + "/crashes");
    if (!std::filesystem::is_directory(root, ec)) return;
    std::vector<std::filesystem::path> folders;
    for (const auto& e : std::filesystem::directory_iterator(root, ec))
        if (e.is_directory(ec) && e.path().filename().wstring().rfind(L"crash_", 0) == 0)
            folders.push_back(e.path());
    if (folders.size() <= static_cast<size_t>(kKeptCrashFolders)) return;
    std::sort(folders.begin(), folders.end());   // crash_<date>_<time>: oldest first
    for (size_t i = 0; i + kKeptCrashFolders < folders.size(); ++i)
        std::filesystem::remove_all(folders[i], ec);
}

// Writes the dump and returns its path; @p outWrote reports success.
// @p ep may be null (terminate/purecall paths have no exception record - the dump
// is still useful because it carries every thread's stack).
static std::string WriteCrashDump(EXCEPTION_POINTERS* ep, bool& outWrote, std::string& outFolder)
{
    outWrote = false;
    const std::string dumpPath = MakeCrashDumpPath(outFolder);

    // CreateFileW + Utf8ToWide, not fopen: the path runs through
    // %LOCALAPPDATA%, i.e. through the user's account name, which is routinely
    // non-ASCII. A narrow open would silently fail exactly for the users least
    // able to work around it.
    const std::wstring widePath = Sys::Utf8ToWide(dumpPath);
    HANDLE file = CreateFileW(widePath.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return dumpPath;

    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId          = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers    = FALSE;

    // IndirectlyReferencedMemory follows pointers found on the stacks, so the objects
    // the crashing frames were working on (entity arrays, descriptor allocations,
    // render-pass state) land in the dump instead of showing as <unavailable>.
    // DataSegs brings the globals/singletons (Logger, TaskSystem, ResourceManager);
    // ThreadInfo keeps every worker's context, because the real culprit is frequently
    // a thread other than the one that faulted.
    const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithIndirectlyReferencedMemory |
        MiniDumpWithDataSegs |
        MiniDumpWithThreadInfo);

    outWrote = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                                 file, type, ep ? &mei : nullptr,
                                 nullptr, nullptr) != FALSE;
    CloseHandle(file);
    return dumpPath;
}

// ==== A lost device: the restart and the wait for the old process ====
#if !defined(WITH_HARNESS) && !defined(WITH_EDITOR)
// The player build's single-instance mutex (wWinMain): the restart lets go of it, or the new process would
// find the old one "still running" and leave.
static HANDLE g_instanceMutex = nullptr;
#endif

// A lost device cannot be brought back inside this process (every resource went with it): a new process starts
// in its place with the same arguments, DRED armed (the dred_next_launch marker) and -after_device_lost=<our
// pid>, which makes it wait for us to exit before it opens the log we still hold. The exe is named by its module
// path, not by whatever the command line's first token resolves to.
static bool RestartAfterDeviceLoss()
{
#if !defined(WITH_HARNESS) && !defined(WITH_EDITOR)
    if (g_instanceMutex)
    {
        ::CloseHandle(g_instanceMutex);
        g_instanceMutex = nullptr;
    }
#endif
    wchar_t module[MAX_PATH * 4] = {};
    const DWORD n = ::GetModuleFileNameW(nullptr, module, static_cast<DWORD>(std::size(module)));
    if (n == 0 || n >= std::size(module))
        return false;
    std::wstring cmd = ::GetCommandLineW();
    cmd += L" -after_device_lost=" + std::to_wstring(::GetCurrentProcessId());
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(module, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        return false;
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return true;
}

// -after_device_lost=<pid> (the restarted process): waits, at most 30 s, for the process that lost its device
// to exit. Before the logger: it rotates DX12Log.txt, which the old process still has open, and two processes
// would write one file. Returns how long it waited, or -1 when that process was already gone.
static double WaitForProcessThatLostItsDevice(DWORD pid)
{
    const HANDLE old = ::OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!old)
        return -1.0;
    const DWORD t0 = ::GetTickCount();
    ::WaitForSingleObject(old, 30000);
    ::CloseHandle(old);
    return static_cast<double>(::GetTickCount() - t0);
}

// Re-entrancy guard shared by all three handlers: the reporting path touches the
// heap and the disk, either of which may be exactly what just died. Recursing into
// it would turn a diagnosable crash into a hang or a dump storm.
// It records WHICH thread is reporting, because a second fault is not always a
// recursion: render-graph workers binding the same broken object die within
// microseconds of each other, and ending the process from the second one would cut
// the first one's report short (a bare "CRASH:" line, no stack, no dump, no
// run.json). So another thread is PARKED here - for good once the report is on disk
// (the reporter's own return ends the process), and for at most 30 s before that,
// in case it holds something the reporter waits for. Its exception is kept for
// ReportFatal to walk. A fault inside the report on the reporting thread itself
// still ends the process at once.
static volatile LONG                g_crashReporter   = 0;   // thread id; 0 = none
static volatile LONG                g_crashReportDone = 0;
static volatile LONG                g_filterRearmed   = 0;   // times the vectored handler found our filter replaced
static bool                         g_reassertDisabled = false;   // -crash_test_no_reassert
// -crash_test_legacy_net (harness): the net as it was before the invalid-parameter / abort handlers, the
// fatal-error-after-a-loss dialog and the restart's wait - the reverse arm of test_crash_folder / test_device_lost.
static bool                         g_legacyNet = false;
static EXCEPTION_POINTERS* volatile g_crashOtherEp    = nullptr;
static volatile LONG                g_crashOtherTid   = 0;

// True: this thread writes the report. False: it is being written already, by
// another thread (after waiting as described above) or by this one.
static bool ClaimCrashReport(EXCEPTION_POINTERS* ep)
{
    const LONG self  = static_cast<LONG>(GetCurrentThreadId());
    const LONG owner = InterlockedCompareExchange(&g_crashReporter, self, 0);
    if (owner == 0)    return true;
    if (owner == self) return false;
    if (ep && InterlockedCompareExchangePointer(
                  reinterpret_cast<PVOID volatile*>(&g_crashOtherEp), ep, nullptr) == nullptr)
        g_crashOtherTid = self;
    for (int waitedMs = 0; waitedMs < 30000; waitedMs += 50)
    {
        if (g_crashReportDone) Sleep(INFINITE);
        Sleep(50);
    }
    return false;
}

// Shared tail for every fatal path: flush, dump, tell the player, flush again.
static void ReportFatal(EXCEPTION_POINTERS* ep, const char* reason)
{
    // FIRST, before any of the slow work below: push the watchdog deadline out.
    // Writing a minidump and symbolizing a stack takes seconds with cold PDBs; if
    // -maxseconds expired midway, TerminateProcess would replace a diagnosable crash
    // with a bare "watchdog_timeout" and no dump. Extend rather than cancel, so the
    // watchdog still covers this path itself wedging.
    Sys::ExtendRunWatchdog(120.0);

    // Flush BEFORE the dump. The dump can be re-attempted if this step somehow
    // faults; the queued log lines cannot be recovered once the process dies.
    Logger::Get().FlushBlocking();
    LOG_ERROR("CRASH: %s", reason ? reason : "unhandled exception");
    if (g_filterRearmed != 0)
        LOG_ERROR("CRASH: the unhandled-exception filter had been replaced; the vectored handler put it back "
                  "before this report (%ld time(s))", g_filterRearmed);
    Logger::Get().FlushBlocking();

    // The symbolized stack, BEFORE the dump: it makes a crash actionable without a
    // debugger (the development machines have no cdb.exe or windbg, so for an automated
    // caller the .dmp alone is not actionable). Written first because it is the
    // cheaper of the two and the more likely to survive a sick heap.
    Sys::CrashReport::LogStackTrace(ep, reason ? reason : "unhandled exception");
    Logger::Get().FlushBlocking();

    // A thread that faulted while this one was reporting is parked in
    // ClaimCrashReport; its stack is frequently the other half of the story.
    if (EXCEPTION_POINTERS* other = g_crashOtherEp)
    {
        char header[128];
        std::snprintf(header, sizeof(header),
                      "thread %ld also faulted (0x%08lX) while this report was written",
                      g_crashOtherTid,
                      other->ExceptionRecord ? other->ExceptionRecord->ExceptionCode : 0ul);
        Sys::CrashReport::LogStackTrace(other, header,
                                        static_cast<unsigned long>(g_crashOtherTid));
        Logger::Get().FlushBlocking();
    }

    bool wrote = false;
    std::string crashFolder;
    const std::string dumpPath = WriteCrashDump(ep, wrote, crashFolder);
    const DWORD err = wrote ? 0u : GetLastError();

    if (wrote) LOG_ERROR("CRASH: minidump written to %s", dumpPath.c_str());
    else       LOG_ERROR("CRASH: FAILED to write minidump to %s (GetLastError=%lu)",
                         dumpPath.c_str(), err);
    Logger::Get().FlushBlocking();

    // Verdict first, message second. Everything below this line builds
    // std::strings on a heap that may be exactly what died; the crash writer
    // touches no heap at all, so it must not be sequenced behind them.
    Harness::RunLedger::Get().WriteCrashJson(Harness::Exception,
                                             reason ? reason : "crash");    InterlockedExchange(&g_crashReportDone, 1);   // parked threads stop counting down
    if (!crashFolder.empty()) FillCrashFolder(crashFolder, reason);

    Plat::Splash::Close();   // not over the dialog
    std::string msg;
    char text[2048];
    // A fatal error that follows a lost device IS the loss (Streamline's shutdown, a worker on a dead
    // queue, a driver fault in the teardown): say that, with the restart offer, not "the game stopped".
    std::string lossDetail;
    const bool deviceLost = !g_legacyNet && Sys::LastPlayerError(&lossDetail) == Sys::PlayerError::DeviceLost;
    const bool offerRestart = deviceLost && !Sys::CommandLine::Get().Has("-after_device_lost");
    if (deviceLost)
    {
        std::snprintf(text, sizeof(text), Sys::Text(Sys::PlayerText::DeviceLost), lossDetail.c_str(),
                      Sys::LogDir().c_str());
        msg = text;
        if (offerRestart) msg += Sys::Text(Sys::PlayerText::RestartAfterLoss);
        msg += std::string("\n\n(") + (reason ? reason : "unhandled exception") +
               (!crashFolder.empty() ? "; " + crashFolder : std::string()) + ")";
    }
    else if (wrote && !crashFolder.empty())
    {
        std::snprintf(text, sizeof(text), Sys::Text(Sys::PlayerText::CrashWithFolder), crashFolder.c_str(),
                      reason ? reason : "unhandled exception");
        msg = text;
    }
    else if (wrote)
    {
        msg = std::string("The application has crashed.\n\nReason: ") + (reason ? reason : "unhandled exception") +
              "\n\nA crash dump was written to:\n" + dumpPath +
              "\n\nPlease attach that file together with DX12Log.txt from:\n" + Sys::LogDir() +
              "\nto your bug report.";
    }
    else
    {
        std::snprintf(text, sizeof(text), Sys::Text(Sys::PlayerText::CrashNoFolder),
                      reason ? reason : "unhandled exception", Sys::LogDir().c_str());
        msg = std::string(text) + "\n\n(" + dumpPath + ": GetLastError=" + std::to_string(err) + ")";
    }

    // -unattended: no human is watching, so a modal dialog is not a report but a hang -
    // the process would sit on the MessageBox until an external timeout killed it,
    // discarding the exit code. Everything above is already in the log; say so there
    // and let the process die with its code.
    if (Sys::CommandLine::Get().Has("-unattended"))
    {
        LOG_ERROR("UNATTENDED: fatal-error dialog suppressed. %s", msg.c_str());
        Logger::Get().FlushBlocking();
    }
    else
    {
        // MessageBoxW, not A: both paths above are UTF-8 and MessageBoxA would
        // reinterpret them as the ANSI code page.
        char titleText[256];
        std::snprintf(titleText, sizeof(titleText),
                      Sys::Text(deviceLost ? Sys::PlayerText::GraphicsTitle : Sys::PlayerText::FatalTitle),
                      Sys::Build().product);
        const std::wstring title = Sys::Utf8ToWide(titleText);
        // Yes / No: restart after a loss, or open the crash folder; a plain OK otherwise.
        const bool offerFolder = !deviceLost && wrote && !crashFolder.empty();
        // Topmost and foreground: an owner-less box can open behind a frozen fullscreen window.
        const int answer = MessageBoxW(nullptr, Sys::Utf8ToWide(msg).c_str(), title.c_str(),
                                       ((offerFolder || offerRestart) ? MB_YESNO : MB_OK) | MB_ICONERROR |
                                       MB_TOPMOST | MB_SETFOREGROUND);
        if (offerFolder && answer == IDYES)
            ShellExecuteW(nullptr, L"open", Sys::Utf8ToWide(crashFolder).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (offerRestart && answer == IDYES)
            RestartAfterDeviceLoss();
    }
}

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep)
{
    if (!ClaimCrashReport(ep))
        return EXCEPTION_EXECUTE_HANDLER;

    char reason[128];
    const DWORD code = (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionCode : 0;
    std::snprintf(reason, sizeof(reason), "unhandled SEH exception 0x%08lX", code);
    ReportFatal(ep, reason);

    // EXCEPTION_EXECUTE_HANDLER from an unhandled-exception filter = terminate
    // the process. Do not return EXCEPTION_CONTINUE_SEARCH: that hands control
    // to WER, which kills us before anything above has run on a released build.
    return EXCEPTION_EXECUTE_HANDLER;
}

// std::terminate fires for an exception that escapes a thread function, a throw
// during unwinding, or a failed noexcept. It bypasses the SEH filter entirely, so
// it needs its own hook or those deaths stay invisible.
static void TerminateHandler()
{
    if (ClaimCrashReport(nullptr))
        ReportFatal(nullptr, "std::terminate (exception escaped a thread or noexcept function)");
    // Must not return. _exit skips atexit/static destructors on purpose: the
    // process state is already undefined and running them invites a second,
    // less informative crash.
    _exit(3);
}

// Calling a virtual through a partially-constructed/destructed object. Common
// enough with the engine's system/pass hierarchies to be worth its own hook.
static void PureCallHandler()
{
    if (ClaimCrashReport(nullptr))
        ReportFatal(nullptr, "pure virtual function call");
    _exit(3);
}

// A CRT function handed an argument it rejects (a null buffer to strcpy_s, a bad format to printf_s). Without a
// handler the CRT fast-fails the process - a death no unhandled-exception filter ever sees; with one it comes here.
static void InvalidParameterHandler(const wchar_t* /*expression*/, const wchar_t* function, const wchar_t* file,
                                    unsigned line, uintptr_t /*reserved*/)
{
    char reason[512];
    std::snprintf(reason, sizeof(reason), "invalid CRT parameter in %ls (%ls:%u)", function ? function : L"?",
                  file ? file : L"?", line);
    if (ClaimCrashReport(nullptr))
        ReportFatal(nullptr, reason);
    _exit(3);
}

// abort(): the CRT raises SIGABRT, and by default then writes its message and hands the process to Windows Error
// Reporting. The handler reports first; _set_abort_behavior (InstallCrashNet) keeps WER and the message out of it.
static void AbortHandler(int /*signal*/)
{
    if (ClaimCrashReport(nullptr))
        ReportFatal(nullptr, "abort() called");
    _exit(3);
}

// ==== The filter, kept ours ====
// A DLL the engine loads (a driver component, Streamline, an overlay) may set its own unhandled-exception filter,
// and ours is silently gone: the next fault dies with no dump and no log. Ours is re-installed after the
// components that do it have run (the boot marks below) and, last resort, by the vectored handler at the first
// fatal-class exception. -crash_test_no_reassert (harness) turns both off.
static void ReassertCrashFilter(const char* when, bool quiet)
{
    if (g_reassertDisabled)
        return;
    LPTOP_LEVEL_EXCEPTION_FILTER previous = SetUnhandledExceptionFilter(CrashHandler);
    if (previous == CrashHandler)
        return;
    if (quiet)
        InterlockedIncrement(&g_filterRearmed);   // no logging on a dying thread; ReportFatal says it
    else
        LOG_WARNING("[CRASH] the unhandled-exception filter had been replaced (by %p) - installed again after %s",
                    reinterpret_cast<void*>(previous), when);
}

// The boot marks that follow a component that may have replaced it: the device, Streamline's init and its feature
// probes, the renderer.
static void ReassertOnBootMark(const char* name)
{
    static const char* const kAfter[] = { "device", "streamlineInit", "streamlineFeatures", "renderer", "firstFrame" };
    for (const char* mark : kAfter)
        if (std::strcmp(mark, name) == 0)
        {
            ReassertCrashFilter(name, false);
            return;
        }
}

// First-chance, before any frame's handler: a fatal-class code (an access violation, a stack overflow, heap
// corruption) is the moment to be sure the filter that will report it is ours. It handles nothing.
static LONG CALLBACK FatalExceptionWatcher(EXCEPTION_POINTERS* ep)
{
    switch (ep->ExceptionRecord->ExceptionCode)
    {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
    case 0xC0000374:   // STATUS_HEAP_CORRUPTION
        ReassertCrashFilter("", true);
        break;
    default:
        break;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Everything that turns a death into a report, in one place: the SEH filter, std::terminate, pure calls, the CRT's
// invalid-parameter and abort paths, no OS fault dialog (the filter reports, a dialog would only sit under it),
// no heap in a Windows Error Reporting dump and no thread suspension while it writes one.
static void InstallCrashNet()
{
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    ::WerSetFlags(WER_FAULT_REPORTING_FLAG_NOHEAP | WER_FAULT_REPORTING_FLAG_DISABLE_THREAD_SUSPENSION);
    SetUnhandledExceptionFilter(CrashHandler);
    std::set_terminate(TerminateHandler);
    _set_purecall_handler(PureCallHandler);
    if (!g_legacyNet)
    {
        _set_invalid_parameter_handler(InvalidParameterHandler);
        std::signal(SIGABRT, AbortHandler);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    }
    ::AddVectoredExceptionHandler(1, FatalExceptionWatcher);
    Sys::Boot::AddMarkListener(&ReassertOnBootMark);
    LOG_INFO("[CRASH] net installed: SEH filter, terminate handler, pure call, invalid parameter, SIGABRT, a vectored "
             "re-arm, no fault dialog, WER without heap");
}

#ifdef WITH_HARNESS
// A component's unhandled-exception filter that reports nothing (-crash_test_replace_filter).
static LONG WINAPI ForeignExceptionFilter(EXCEPTION_POINTERS* /*ep*/)
{
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, PWSTR /*lpCmdLine*/, int /*nCmdShow*/)
{
    // Boot timeline: the time before this line is the loader + static
    // initialisation; run.json's bootPhasesMs counts every mark from process creation.
    Sys::Boot::Mark("main");
    // No OS fault dialog from here on, before the crash net (InstallCrashNet) exists to report instead.
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    // ---- Publish "am I an authoring host?" to EngineCore ----
    // FIRST thing in main, before anything engine-side can read it. EngineCore.lib is
    // compiled ONCE and linked into both exes, so nothing in it can see WITH_EDITOR -
    // only this TU is compiled per target. Engine code asks Sys::IsEditorHost().
    // Do not #ifdef WITH_EDITOR inside EngineCore: it silently takes the game branch in
    // both exes (see System/HostPolicy.h).
#ifdef WITH_EDITOR
    Sys::SetEditorHost(true);
#else
    Sys::SetEditorHost(false);
#endif
    // ---- The modules this exe links (src/Modules/Modules.generated.cpp) ----
    // Before anything can build the component registry, which asks every linked module for
    // its serializers. Registration only stores factories: nothing here may log.
    Module::RegisterLinkedModules(Module::Registry::Get());
#ifdef WITH_EDITOR
    Editor::RegisterLinkedEditorModules(Editor::ModuleRegistry::Get());
#endif
    // Before App constructs the device: a module whose SDK hooks it installs itself here.
    Module::Registry::Get().OnProcessStart();

    // ---- Parse argv, BEFORE anything resolves a writable path ----
    // The log's own destination comes from -artifacts=, so this precedes
    // Logger::Initialize. Neither call below may log: the logger does not exist yet.
    // Both stash their complaints and replay them once it does, so a typo'd flag never
    // presents as "the flag did nothing".
    // CommandLine is NOT lazy: skipping this call leaves every flag reading as absent,
    // silently, everywhere.
    Sys::CommandLine::Initialize();

    // The restart after a lost device (-after_device_lost=<pid of the process that lost it>) waits for that
    // process to exit before anything here touches the log it still holds open.
    DWORD  lostDevicePid    = 0;
    double lostDeviceWaitMs = -1.0;
    if (Sys::CommandLine::Get().Has("-after_device_lost"))
    {
        lostDevicePid = static_cast<DWORD>(Sys::CommandLine::Get().GetInt("-after_device_lost", 0));
        // -crash_test_legacy_net: no wait, the way the restart was before (the log rotation raced the old process).
        bool wait = true;
#ifdef WITH_HARNESS
        wait = !Sys::CommandLine::Get().Has("-crash_test_legacy_net");
#endif
        if (wait && lostDevicePid != 0 && lostDevicePid != ::GetCurrentProcessId())
            lostDeviceWaitMs = WaitForProcessThatLostItsDevice(lostDevicePid);
    }

    // ---- Content roots, BEFORE anything resolves an asset path ----
    //   -project=<dir>   the project to open (a directory holding game.json).
    //   -engine=<dir>    where engine-owned content lives.
    // Both default to the working directory (the single-root layout). A shipped build (no
    // harness) reads neither, nor -artifacts: its content is its pak, its output the player's folder.
#ifdef WITH_HARNESS
    if (Sys::CommandLine::Get().Has("-project"))
    {
        const std::string dir = Sys::CommandLine::Get().Get("-project");
        if (!dir.empty()) Sys::SetProjectDir(dir);
    }
    if (Sys::CommandLine::Get().Has("-engine"))
    {
        const std::string dir = Sys::CommandLine::Get().Get("-engine");
        if (!dir.empty()) Sys::SetEngineDir(dir);
    }
#endif

    bool artifactDirFailed = false;
    bool artifactDirEmpty  = false;
#ifdef WITH_HARNESS
    if (Sys::CommandLine::Get().Has("-artifacts"))
    {
        const std::string dir = Sys::CommandLine::Get().Get("-artifacts");

        // A bare `-artifacts` with no `=dir` must be a hard error, not a no-op:
        // SetArtifactDir("") CLEARS the root, so the flag would silently disable the whole
        // harness (no run.json, no frames.jsonl, no screenshots) and the run would still
        // exit 0.
        if (dir.empty()) artifactDirEmpty  = true;
        else             artifactDirFailed = !Sys::SetArtifactDir(dir);
    }
#endif

    // A shipped exe's folder is its product's (game.json's name, compiled in), not its file name: a
    // player who renames the exe keeps their settings and saves. A harness build keeps the exe's stem,
    // which the tests' renamed copies use to get a folder of their own.
#ifndef WITH_HARNESS
    if (std::strcmp(Sys::Build().product, "Game") != 0 && std::strcmp(Sys::Build().product, "unknown") != 0)
        Sys::SetProductName(Sys::Build().product);
#endif
    // -user_dir=<dir>: the folder this run writes settings.json, saves, the PSO cache and the log
    // to, instead of the per-user one - a QA run of a shipped exe that must not touch (or inherit)
    // a player's folder on the same machine (tools/package_smoke.py).
    const std::string userDir = Sys::CommandLine::Get().Get("-user_dir");

    // ---- Where this build writes settings.json / save<N>.json / DX12Log.txt ----
    // The editor is an AUTHORING tool run out of the source tree, so its writable root
    // is the working directory: settings.json stays in the repo (diffable, committable)
    // and the log sits next to the code being read. This also matches how the editor
    // resolves every loose asset ("asset/...", "shaders/...").
    // Game.exe is what a player runs, so it keeps the per-user root: under Program Files
    // a relative "DX12Log.txt" is UAC-redirected into the VirtualStore (or denied), two
    // Windows accounts would share one save file, and from a shortcut the CWD is not
    // even the exe directory. This is the one decision that differs between the
    // authoring and the shipped build, so it is made here, in the only TU compiled per
    // target, and nowhere else.
    // pso_cache.bin is NOT affected: it keeps its own two-root scheme
    // (System/PsoCachePaths.cpp).
#ifdef WITH_EDITOR
    {
        std::error_code cwdEc;
        const std::filesystem::path cwd = std::filesystem::current_path(cwdEc);
        if (!cwdEc)
            Sys::SetWritableDataRoot(Sys::PathToUtf8(cwd));
        // On failure we simply leave the default (per-user root) in place.
    }
#endif
    if (!userDir.empty())
    {
        std::error_code ec;
        const std::filesystem::path dir = std::filesystem::absolute(Sys::U8Path(userDir), ec);
        std::filesystem::create_directories(dir, ec);
        Sys::SetWritableDataRoot(Sys::PathToUtf8(dir));
    }
    // Sys::LogDir(): with -artifacts=<dir> the log (and the crash dump with it) goes
    // into the run's own folder, so everything one automated run produced can be
    // collected, diffed or deleted as a unit; without the flag it is
    // <WritableDataDir>/log. For Game.exe that root is per-user, not the working
    // directory, so a plain Game.exe writes %LOCALAPPDATA%\Game\log\DX12Log.txt.
#if !defined(WITH_HARNESS) && !defined(WITH_EDITOR)
    // One player instance per folder: a second launch (a double-click while the first one boots)
    // brings the first window forward and leaves. Keyed by the writable folder, so a QA run with
    // -user_dir does not meet a player's game. The mutex is held until the process exits.
    std::wstring instanceName = L"Local\\DX12Engine.instance.";
    for (wchar_t c : Sys::Utf8ToWide(Sys::WritableDataDir()))
        instanceName += (c == L'\\' || c == L':' || c == L'/') ? L'_' : c;
    HANDLE instanceMutex = g_instanceMutex = ::CreateMutexW(nullptr, FALSE, instanceName.c_str());
    if (instanceMutex && ::GetLastError() == ERROR_ALREADY_EXISTS)
    {
        // By its title, the product's name (App's MakeWindowDesc): every product and the editor share
        // the window class. Asynchronous, because the first instance may still be booting and not
        // pumping messages; restored only when minimized, so a fullscreen or maximized one stays so.
        if (HWND other = ::FindWindowW(L"DX12EngineWindow", Sys::Utf8ToWide(Sys::ProductTitle()).c_str()))
        {
            if (::IsIconic(other)) ::ShowWindowAsync(other, SW_RESTORE);
            ::SetForegroundWindow(other);
        }
        ::CloseHandle(instanceMutex);
        return Harness::Ok;
    }
#endif
    Logger::Get().Initialize(Sys::LogDir() + "/DX12Log.txt");
    Sys::Boot::Mark("logger");

    // A log that failed to open is INVISIBLE from inside: Initialize() returns quietly,
    // every LOG_ line is still formatted and counted, and run.json still reports an
    // honest error count and a `pass` verdict - green on a run that produced no
    // evidence, a real risk because Initialize must create the LogDir() subdirectory.
    // Report it on stderr and OutputDebugString, since the logger is what is missing.
    if (!Logger::Get().IsInitialized())
    {
        const std::string where = Sys::LogDir() + "/DX12Log.txt";
        const std::string why =
            "FATAL: could not open the log file '" + where + "'. "
            "The directory may be unwritable or the path too long.";
        std::fprintf(stderr, "%s\n", why.c_str());
        ::OutputDebugStringW(Sys::Utf8ToWide(why + "\n").c_str());

        // Unattended means nobody will read that message, and a run with no log
        // cannot be judged. Refuse rather than exit 0 on no evidence.
        if (Sys::CommandLine::Get().Has("-unattended"))
            return Harness::PreconditionFail;
        // Interactive: the editor is still usable and the in-app log panel still fills,
        // so carry on - having said so.
    }

    // First lines in every log: which roots this process actually resolved. A
    // bug report that starts with "settings did not stick" is answered here
    // instead of by guessing how the exe was launched. Also in a shipped build's
    // WARNING-level file (Logger::KeepInFile).
    {
        Logger::KeepInFile keepHeader;
        LOG_INFO("Product     : %s", Sys::ProductName().c_str());
        const Sys::BuildInfo& b = Sys::Build();
        LOG_INFO("Build       : %s %s (%s%s, %s, %s, build %s)", b.product, b.version, b.gitSha, b.dirty ? "+dirty" : "",
                 b.branch, b.config, b.buildId);
        LOG_INFO("Install root: %s", Sys::ExeDir().c_str());
        LOG_INFO("Writable    : %s", Sys::WritableDataDir().c_str());
        LOG_INFO("Diagnostics : %s", Sys::DiagnosticsDir().c_str());
        LOG_INFO("Log dir     : %s", Sys::LogDir().c_str());
        LOG_INFO("Command line: %s", Sys::CommandLine::Get().ToString().c_str());
        if (lostDevicePid != 0)
        {
            if (lostDeviceWaitMs < 0.0)
                LOG_INFO("[BOOT] process %lu (the one that lost its device) was already gone",
                         static_cast<unsigned long>(lostDevicePid));
            else
                LOG_INFO("[BOOT] waited %.0f ms for process %lu (the one that lost its device) to exit",
                         lostDeviceWaitMs, static_cast<unsigned long>(lostDevicePid));
        }
    }

    // A person launching the game or the editor sees it start: the main window stays hidden through
    // the device and the content (seconds, with no message pump) and shows at the first frame, and the
    // splash covers the wait. It closes itself at that frame.
    if (!Sys::CommandLine::Get().Has("-hidden") && !Sys::CommandLine::Get().Has("-unattended"))
    {
        const std::string splashTitle = Sys::IsEditorHost() ? std::string(Sys::Build().product) + " - Editor"
                                                            : std::string(Sys::Build().product);
        Plat::Splash::Show(splashTitle, Sys::Text(Sys::PlayerText::Loading));
    }

    // The logger exists now, so anything the pre-log phase wanted to say can be
    // said. Ordering matters: these lines explain why a flag did not take effect.
    Sys::CommandLine::ReplayDiagnostics();
    Harness::RunLedger::Get().Begin();

    // Parsed here, after the logger (the numeric readers warn on a malformed
    // value) and before App exists. TaskSystem reads it in its constructor, so
    // nothing may construct that singleton earlier than this line.
    Harness::Determinism::Initialize();
    if (artifactDirEmpty)
    {
        LOG_ERROR("-artifacts was given with no directory. It must be written as "
                  "-artifacts=<dir>; a bare -artifacts would silently disable "
                  "every harness output and still exit 0.");
        Logger::Get().FlushBlocking();
        Logger::Get().Shutdown();
        return Harness::PreconditionFail;
    }
    if (artifactDirFailed)
    {
        // A harness run asked for its output in a specific place and cannot have it.
        // Continuing would scatter artifacts where the caller will not find them and
        // report success for a run whose evidence is missing - so this is a precondition
        // failure, not a warning.
        LOG_ERROR("-artifacts='%s' could not be created; refusing to run so the "
                  "artifacts are not silently written somewhere else.",
                  Sys::CommandLine::Get().Get("-artifacts").c_str());
        Logger::Get().FlushBlocking();
        Logger::Get().Shutdown();
        return Harness::PreconditionFail;
    }

    // Install the crash net immediately after the logger, before ANY engine
    // object exists: device creation, shader compilation and asset loading all
    // happen before the first frame and all of them can fault.
#ifdef WITH_HARNESS
    g_reassertDisabled = Sys::CommandLine::Get().Has("-crash_test_no_reassert");
    g_legacyNet        = Sys::CommandLine::Get().Has("-crash_test_legacy_net");
#endif
    InstallCrashNet();
#ifdef WITH_HARNESS
    // -crash_test_replace_filter=early: something replaces the filter right after, as a component loaded by
    // the device would; the boot marks must put ours back (test_crash_folder).
    if (Sys::CommandLine::Get().Get("-crash_test_replace_filter") == "early")
        SetUnhandledExceptionFilter(ForeignExceptionFilter);
#endif

    // Reserve stack for the filter itself so a MAIN-THREAD stack overflow still
    // produces a dump instead of a second fault inside the handler. 64 KiB is
    // enough for the string/MessageBox/dbghelp work in ReportFatal.
    {
        ULONG stackGuarantee = 64 * 1024;
        SetThreadStackGuarantee(&stackGuarantee);
    }

    // ---- -maxseconds=N: the only unconditional bound on this run ----
    // Armed here - after the crash net and the logger, before ANY engine object
    // exists - because boot itself can hang: device creation, shader compilation, PSO
    // warm and the pak mount all happen before the first frame.
    // Read AFTER Logger::Initialize on purpose: GetFloat can LOG_WARNING on a malformed
    // value, and that warning needs an open log file.
    // Leave headroom: boot and vsync-paced frames all count against it, so a tight
    // -maxseconds reports a cold boot as a hang.
    {
        const double maxSeconds = Sys::CommandLine::Get().GetFloat("-maxseconds", 0.0f);
        Sys::StartRunWatchdog(maxSeconds);
    }

    LOG_INFO("Application started");
    PruneCrashFolders();

#ifdef WITH_HARNESS
    // -crash_test_after_ms=N: a fault on a thread of its own N ms in - the crash path end to end (the dump,
    // a player's crash folder, the verdict) for tests/unit/test_crash_folder.py and test_device_lost.py.
    // -crash_test_kind= picks the fault: av (default), invalid_parameter (a CRT call with a bad argument),
    // abort (abort()), terminate (std::terminate), replace_filter (something replaces the unhandled-exception
    // filter first, then av: the vectored handler must put ours back), device_lost_av (the device is noted
    // lost first, then av: the dialog must say so).
    if (Sys::CommandLine::Get().Has("-crash_test_after_ms"))
    {
        const int ms = Sys::CommandLine::Get().GetInt("-crash_test_after_ms", 3000);
        const std::string kind = Sys::CommandLine::Get().Get("-crash_test_kind", "av");
        std::thread([ms, kind]
        {
            Sleep(static_cast<DWORD>(ms > 0 ? ms : 0));
            if (kind == "invalid_parameter")
            {
                char* volatile destination = nullptr;
                strcpy_s(destination, 4, "abc");
            }
            else if (kind == "abort")
            {
                std::abort();
            }
            else if (kind == "terminate")
            {
                std::terminate();
            }
            if (kind == "replace_filter")
                SetUnhandledExceptionFilter(ForeignExceptionFilter);
            if (kind == "device_lost_av")
                Sys::NotePlayerError(Sys::PlayerError::DeviceLost, "DXGI_ERROR_DEVICE_REMOVED (test)");
            *static_cast<volatile int*>(nullptr) = 0;
        }).detach();
    }
#endif

    // COM on the main thread, single-threaded apartment: the UI thread's apartment
    // (shell dialogs, OLE drag-drop, the clipboard, WIC decodes made here), so callers
    // on this thread need not initialise it themselves. A later
    // CoInitializeEx(MULTITHREADED) on this thread gets RPC_E_CHANGED_MODE, which
    // callers (UnrealSceneImporter) treat as usable.
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(comHr))
        LOG_WARNING("CoInitializeEx(STA) on the main thread failed (0x%08lX)", static_cast<unsigned long>(comHr));

    // A worktree reading its binary assets from the main checkout
    // (Resource/SharedContent.h) registers that root BEFORE anything reads an asset:
    // the editor theme loads its fonts while App is constructed, ahead of App::Run's
    // own root registration. A no-op (no root registered) without a
    // .shared_content.json.
    ::Resource::SharedContent::Configure();

#ifdef WITH_HARNESS
    // -assettool=<command>: the offline asset tool (Harness/AssetTool.h). No
    // window, device or world is created; the tool writes its JSON and the
    // process exits with the tool's code.
    {
        const int toolCode = Harness::RunAssetToolIfRequested();
        if (toolCode >= 0)
        {
            TaskSystem::Get().Shutdown();
            Logger::Get().Shutdown();
            return toolCode;
        }
    }
#endif

#ifdef WITH_EDITOR
    {
        const int headlessCode = RunHeadlessUeImportIfRequested();
        if (headlessCode >= 0)
        {
            TaskSystem::Get().Shutdown();
            Logger::Get().Shutdown();
            return headlessCode;
        }
    }
#endif

    int exitCode = 0;
    try
    {
        // The content roots and game.ipak before App: its members build the device, which peeks
        // engine.json (the sealed copy, in a package) - and a mount reads only the pak's index.
        ::Resource::MountContentRoots();
        App app;
        exitCode = app.Run();
        Sys::Boot::Mark("runReturned");   // Run's locals destroyed (the renderer and what it owns)
    }
    catch (const ExceptionHandle& e)
    {
        LOG_ERROR(e.what());
        Logger::Get().FlushBlocking();
        // See the note in ReportFatal: unattended, a modal is a hang, not a report.
        // ExceptionHandle is the window-creation path (Plat::Window::Exception).
        if (!Sys::CommandLine::Get().Has("-unattended"))
        {
            Plat::Splash::Close();   // the topmost splash would cover the dialog
            MessageBoxW(nullptr, Sys::Utf8ToWide(e.what()).c_str(), L"Error",
                        MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
        }
        exitCode = Harness::Exception;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR(e.what());
        Logger::Get().FlushBlocking();
        // Same as above. This is the ThrowIfFailed path (it throws std::runtime_error) and
        // the "No game assets found" path (ContentRoots) - the two most likely first failures
        // for an automated caller.
        if (!Sys::CommandLine::Get().Has("-unattended"))
        {
            Plat::Splash::Close();
            // A lost device ends here as a failed D3D call, a GPU below the minimum as a failed device
            // creation: say what it was, not "DX12 call failed".
            std::string detail;
            const Sys::PlayerError kind = Sys::LastPlayerError(&detail);
            char text[2048];
            char title[256];
            const bool graphics = kind != Sys::PlayerError::None;
            // -after_device_lost: this run is the restart a loss offered; a second loss is only reported.
            const bool offerRestart = kind == Sys::PlayerError::DeviceLost &&
                                      !Sys::CommandLine::Get().Has("-after_device_lost");
            if (kind == Sys::PlayerError::DeviceLost)
            {
                std::snprintf(text, sizeof(text), Sys::Text(Sys::PlayerText::DeviceLost), detail.c_str(),
                              Sys::LogDir().c_str());
                if (offerRestart) strncat_s(text, Sys::Text(Sys::PlayerText::RestartAfterLoss), _TRUNCATE);
            }
            else if (kind == Sys::PlayerError::MinimumSpec)
                std::snprintf(text, sizeof(text), Sys::Text(Sys::PlayerText::MinimumSpec), detail.c_str());
            else
                std::snprintf(text, sizeof(text), Sys::Text(Sys::PlayerText::StartFailed), e.what(),
                              Sys::LogDir().c_str());
            std::snprintf(title, sizeof(title),
                          Sys::Text(graphics ? Sys::PlayerText::GraphicsTitle : Sys::PlayerText::FatalTitle),
                          Sys::Build().product);
            const int answer = MessageBoxW(nullptr, Sys::Utf8ToWide(text).c_str(), Sys::Utf8ToWide(title).c_str(),
                                           (offerRestart ? MB_YESNO : MB_OK) | MB_ICONERROR |
                                           MB_TOPMOST | MB_SETFOREGROUND);
            // A lost device cannot be brought back inside this process (every resource went with it): a
            // new one starts in its place, with DRED armed (dred_next_launch) and the same arguments.
            if (offerRestart && answer == IDYES)
                RestartAfterDeviceLoss();
        }
        exitCode = Harness::Exception;
    }
    catch (...)
    {
        // Anything not derived from std::exception (Lua's error objects, a bare
        // `throw <enum>`): log it and exit with a code instead of reaching std::terminate.
        LOG_ERROR("Unknown exception type escaped App::Run()");
        Logger::Get().FlushBlocking();
        if (!Sys::CommandLine::Get().Has("-unattended"))
        {
            Plat::Splash::Close();
            MessageBoxW(nullptr,
                        L"An unknown error occurred. See DX12Log.txt for details.",
                        L"Error", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
        }
        exitCode = Harness::Exception;
    }

    Plat::Splash::Close();   // a run that ended before its first frame
    Sys::Boot::Mark("appDestroyed");   // App (the window, the device) destroyed
    LOG_INFO("Application exiting normally");
    GpuLedger::LogResidual();
    TaskSystem::Get().Shutdown();
    if (SUCCEEDED(comHr))
        CoUninitialize();

#ifdef _DEBUG
    // After App and all DX12 objects are destroyed, check for true app-level leaks.
    // DXGI_DEBUG_RLO_IGNORE_INTERNAL filters out objects the debug layer holds
    // for its own bookkeeping, so only genuine unreleased objects appear.
    {
        ComPtr<IDXGIDebug1> dxgiDebug;
        if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiDebug))))
        {
            dxgiDebug->ReportLiveObjects(
                DXGI_DEBUG_ALL,
                DXGI_DEBUG_RLO_FLAGS(DXGI_DEBUG_RLO_SUMMARY | DXGI_DEBUG_RLO_IGNORE_INTERNAL));
        }
    }
#endif

    // Disarm here and nowhere else. Earlier would leave TaskSystem::Shutdown() above
    // (which joins every worker thread, a real hang candidate) uncovered. Later is
    // useless: after Logger::Shutdown() a watchdog report would have nowhere to go.
    Sys::StopRunWatchdog();

    // Finalize runs AFTER TaskSystem::Shutdown above (its worker-exception path still
    // emits LOG_ERROR, and those errors belong to this run's verdict) and BEFORE
    // Logger::Shutdown below, which closes the file the ledger needs drained. Finalize
    // does the drain itself. Do not move it into App::Run: ShutdownAllSystems and
    // SavePSOLibrary have not run there yet.
    Sys::Boot::Mark("shutdown");   // teardown done; Finalize publishes the timeline
    exitCode = Harness::RunLedger::Get().Finalize(exitCode);

    LOG_INFO("Application exiting (code=%d %s)", exitCode,
             Harness::ExitCodeName(exitCode));
    Logger::Get().FlushBlocking();
    Logger::Get().Shutdown();

    return exitCode;
}
