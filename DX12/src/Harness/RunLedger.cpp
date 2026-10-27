#include "Harness/RunLedger.h"

#ifdef WITH_HARNESS
#include "Diagnostics/ProfilerCollector.h"   // RunProfileJson
#include "ECS/AccessGuard.h"                 // counters.accessGuardViolations
#include "Harness/ExitCodes.h"
#include "Harness/ShaderVariants.h"   // -shader_variants
#include "Harness/PakOrder.h"         // -pak_order
#include "Harness/PsoVariants.h"      // -pso_variants
#include "System/BuildInfo.h"         // run.json "build"
#include "System/CommandLine.h"
#include "System/PathUtil.h"
#include "System/Log.h"
#include "System/BootTimeline.h"   // bootPhasesMs
#include "Module/ModuleRegistry.h"  // modules
#include "Graphics/GPUProfiler.h"
#include "Graphics/GpuMemoryLedger.h"   // GpuLedger::ViewBytes
#include "Graphics/VideoMemory.h"         // the budget block
#include "Graphics/ViewSlot.h"          // RHI::kMaxViews
#include "Renderer/ViewBudget.h"
#include "System/ProcessMemory.h"
#include <algorithm>
#include <cmath>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <string>

namespace Harness
{
namespace
{
    // Minimal JSON string escaping. Paths are the reason this is needed at all:
    // every Windows path in the ledger can carry backslashes, and an unescaped
    // one makes the file unparseable exactly when a caller most needs to read it.
    std::string JsonEscape(const std::string& in)
    {
        std::string out;
        out.reserve(in.size() + 8);
        for (char c : in)
        {
            switch (c)
            {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20)
                    {
                        char esc[8];
                        std::snprintf(esc, sizeof(esc), "\\u%04X",
                                      static_cast<unsigned>(static_cast<unsigned char>(c)));
                        out += esc;
                    }
                    else out += c;
                    break;
            }
        }
        return out;
    }

    // Raw write, no CRT streams. Shared by both writers so the crash path and
    // the orderly path cannot drift into different file-creation semantics.
    bool WriteWholeFile(const std::string& utf8Path, const char* data, size_t len)
    {
        const std::wstring wide = Sys::Utf8ToWide(utf8Path);
        HANDLE h = ::CreateFileW(wide.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        const BOOL ok = ::WriteFile(h, data, static_cast<DWORD>(len), &written, nullptr);
        ::CloseHandle(h);
        return ok != FALSE && written == len;
    }
}

RunLedger& RunLedger::Get()
{
    static RunLedger s_ledger;
    return s_ledger;
}

void RunLedger::Begin()
{
    const auto& cl = Sys::CommandLine::Get();
    m_commandLine  = cl.ToString();
    m_maxErrors    = cl.Has("-maxerrors") ? cl.GetInt("-maxerrors", -1) : -1;
    m_expectMin    = cl.Has("-expect_min") ? cl.GetInt("-expect_min", -1) : -1;
    m_enabled      = Sys::HasArtifactDir();
    // Before App: the device's first pipeline must find the recorder on, the pak mount its first read.
    EnablePsoVariantsIfRequested();
    EnablePakOrderIfRequested();
    if (m_enabled)
    {
        m_path = Sys::ArtifactDir() + "/run.json";

        // Pre-convert for the crash writer (see m_pathW). An over-long path disables the crash
        // writer rather than truncating to a wrong filename and writing the verdict elsewhere.
        const std::wstring wide = Sys::Utf8ToWide(m_path);
        if (wide.size() < (sizeof(m_pathW) / sizeof(m_pathW[0])))
            std::wmemcpy(m_pathW, wide.c_str(), wide.size() + 1);
        else
            LOG_WARNING("RunLedger: artifact path is too long for the crash "
                        "writer (%zu wide chars); a crash will still log but "
                        "will not leave a run.json.", wide.size());

        // Same trick for the log path. Escaping here (the heap is fine at
        // startup) keeps WriteCrashJson a pure snprintf over resident bytes.
        const std::string esc = JsonEscape(Sys::LogDir() + "/DX12Log.txt");
        if (esc.size() < sizeof(m_logPathJson))
            std::memcpy(m_logPathJson, esc.c_str(), esc.size() + 1);
    }
}

void RunLedger::NoteGpuFrame(const GPUProfiler& profiler)
{
    if (!m_enabled || !profiler.hasResults) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_gpuProfiledFrames;
    std::map<std::string, float> frame;
    for (uint32_t i = 0; i < profiler.resultCount; ++i)
    {
        const auto& r = profiler.results[i];
        if (r.name && std::isfinite(r.gpuMs) && r.gpuMs >= 0.f)
            frame[r.name] += r.gpuMs;
    }
    for (const auto& [name, ms] : frame)
    {
        auto& samples = m_gpuPasses[name];
        samples.ms.push_back(ms);
        samples.age = (std::max)(samples.age, profiler.resultsAgeFrames);
    }
}

void RunLedger::NoteFrames(std::uint64_t frames)
{
    m_frames = frames;
    NoteMemory();
}

void RunLedger::NoteMemory()
{
    namespace P = Diag::Prof;
    constexpr double kMiB = 1024.0 * 1024.0;
    char num[128];
    P::GpuLedger ledger;
    P::ReadGpuLedger(ledger);
    std::string j = "{";
    std::snprintf(num, sizeof(num), "\"gpuTotalMB\": %.1f, \"gpuLocalMB\": %.1f, \"placed\": %lld, ",
                  double(ledger.totalBytes) / kMiB, double(ledger.localBytes) / kMiB,
                  static_cast<long long>(ledger.placed));
    j += num;
    j += "\"categories\": {";
    for (int i = 0; i < P::kGpuMemCats; ++i)
    {
        std::snprintf(num, sizeof(num), "%s\"%s\": {\"mb\": %.1f, \"count\": %lld}", i ? ", " : "",
                      P::GpuMemCatName(static_cast<P::GpuMemCat>(i)), double(ledger.bytes[i]) / kMiB,
                      static_cast<long long>(ledger.count[i]));
        j += num;
    }
    j += "}, \"top\": [";
    std::vector<P::GpuObjectInfo> top;
    P::ReadGpuObjects(top, 10);
    for (std::size_t i = 0; i < top.size(); ++i)
    {
        std::snprintf(num, sizeof(num), "%s{\"name\": \"", i ? ", " : "");
        j += num;
        j += JsonEscape(top[i].name);
        std::snprintf(num, sizeof(num), "\", \"category\": \"%s\", \"mb\": %.1f}",
                      P::GpuMemCatName(top[i].cat), double(top[i].bytes) / kMiB);
        j += num;
    }
    j += "], \"views\": [";
    bool firstView = true;
    for (std::uint32_t slot = 1; slot < RHI::kMaxViews; ++slot)
    {
        const std::uint64_t bytes = GpuLedger::ViewBytes(slot);
        if (!bytes) continue;
        std::snprintf(num, sizeof(num), "%s{\"slot\": %u, \"mb\": %.1f}", firstView ? "" : ", ", slot,
                      double(bytes) / kMiB);
        j += num;
        firstView = false;
    }
    j += "], \"budget\": ";
    std::snprintf(num, sizeof(num), "{\"stage\": %d, \"refusals\": %u, \"osChanges\": %u, \"residencyHigh\": %u}",
                  static_cast<int>(ViewBudget::Current()), ViewBudget::Refusals(), VideoMemory::BudgetChanges(),
                  VideoMemory::PrioritizedCount());
    j += num;
    j += ", \"process\": ";
    const Sys::ProcessMemory pm = Sys::QueryProcessMemory();
    if (pm.valid)
    {
        std::snprintf(num, sizeof(num), "{\"privateMB\": %.1f, \"workingSetMB\": %.1f, \"peakWorkingSetMB\": %.1f}",
                      double(pm.privateBytes) / kMiB, double(pm.workingSet) / kMiB,
                      double(pm.peakWorkingSet) / kMiB);
        j += num;
    }
    else j += "null";
    j += "}";
    std::lock_guard<std::mutex> lock(m_mutex);
    m_memoryJson = std::move(j);
}

void RunLedger::NoteVram(std::uint64_t usageBytes, std::uint64_t budgetBytes)
{
    std::uint64_t peak = m_vramPeak.load(std::memory_order_relaxed);
    while (usageBytes > peak &&
           !m_vramPeak.compare_exchange_weak(peak, usageBytes, std::memory_order_relaxed)) {}
    if (budgetBytes == 0) return;
    std::uint64_t low = m_vramMinBudget.load(std::memory_order_relaxed);
    while ((low == 0 || budgetBytes < low) &&
           !m_vramMinBudget.compare_exchange_weak(low, budgetBytes, std::memory_order_relaxed)) {}
    if (usageBytes > budgetBytes) m_vramOverFrames.fetch_add(1, std::memory_order_relaxed);
}

void RunLedger::NoteGpuRegionsDropped(std::uint32_t count)
{
    if (!m_enabled || !count) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_gpuRegionsDropped += count;
}

void RunLedger::NoteDeviceLost(const char* reason)
{
    // Locked: reachable from ThrowIfFailedImpl on ANY thread, including a
    // TaskSystem worker creating a resource at the moment the device dies.
    std::lock_guard<std::mutex> lock(m_mutex);
    // First one wins: the initial reason is the cause, anything after it is a
    // consequence of running on a dead device.
    if (m_deviceLost) return;
    m_deviceLost       = true;
    m_deviceLostReason = reason ? reason : "unknown";
}
void RunLedger::NoteDisplay(const char* hdrMode, const char* compositionFormat,
                            std::uint32_t outputW, std::uint32_t outputH,
                            std::uint32_t renderW, std::uint32_t renderH,
                            float renderScale)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_hdrMode      = hdrMode ? hdrMode : "unknown";
    m_compFormat   = compositionFormat ? compositionFormat : "unknown";
    m_outputWidth  = outputW;
    m_outputHeight = outputH;
    m_renderWidth  = renderW;
    m_renderHeight = renderH;
    m_renderScale  = renderScale;
}

void RunLedger::NoteGameFlow(const std::string& finalState,
                             std::uint32_t transitions,
                             std::uint32_t rejected,
                             std::uint32_t invalidStates)
{
    // Locked like the rest of the Note* family, so a caller on another thread stays safe
    // (the one caller is the main thread at teardown).
    std::lock_guard<std::mutex> lock(m_mutex);
    m_flowFinalState    = finalState;
    m_flowTransitions   = transitions;
    m_flowRejected      = rejected;
    m_flowInvalidStates = invalidStates;
}

void RunLedger::NoteWindow(double longestUnpumpedMs, std::uint64_t pumps, std::uint32_t eventsDropped,
                           std::uint32_t eventsMerged, std::uint32_t nestedPumps, std::uint32_t displayRefits)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_windowNoted         = true;
    m_windowUnpumpedMs    = longestUnpumpedMs;
    m_windowPumps         = pumps;
    m_windowEventsDropped = eventsDropped;
    m_windowEventsMerged  = eventsMerged;
    m_windowNestedPumps   = nestedPumps;
    m_windowDisplayRefits = displayRefits;
}

void RunLedger::NoteSection(const char* name, const std::string& json)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sections[name] = json;
}

void RunLedger::NoteExpectation(bool passed, const char* site, const char* message)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (passed) { ++m_passCount; return; }

    ++m_failCount;
    std::string entry = (site ? site : "?");
    entry += ": ";
    entry += (message ? message : "(no message)");

    if (m_firstFailure.empty()) m_firstFailure = entry;

    // Bounded transcript: a scenario asserting inside OnUpdate can fail once per
    // frame for the whole run, and 10 000 identical strings in run.json help
    // nobody. The COUNT stays exact; only the list is capped.
    if (m_failures.size() < 32) m_failures.push_back(std::move(entry));
}

void RunLedger::NoteScriptFault(const char* site, const char* what)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_scriptFaults;

    std::string entry = "SCRIPT FAULT ";
    entry += (site ? site : "?");
    entry += ": ";
    entry += (what ? what : "(no message)");

    if (m_firstFailure.empty()) m_firstFailure = entry;
    if (m_failures.size() < 32) m_failures.push_back(std::move(entry));
}

int RunLedger::ResolveExitCode(int rawExitCode) const
{
    // ExitCode:: qualification is required, not stylistic: inside this class
    // the bare name `DeviceLost` binds to the member function DeviceLost()
    // rather than to the enumerator, which compiles as a pointer-to-member and
    // fails only at the conversion to int.
    if (m_deviceLost)                 return ExitCode::DeviceLost;
    if (rawExitCode != ExitCode::Ok)  return rawExitCode;

    // A script fault outranks a failed expectation: the scenario STOPPED RUNNING (after a
    // throw in OnUpdate the script system parks the driver, and nothing in a Game.exe run
    // re-arms it), so the assertions that never ran are unknowns, not passes.
    if (m_scriptFaults > 0)  return ExitCode::AssertionFailed;
    if (m_failCount   > 0)   return ExitCode::AssertionFailed;

    // -expect_min=N guards the silent-nothing case: a driver whose OnUpdate
    // never ran, or whose gate condition was never satisfied, asserts zero
    // times and would otherwise pass. Requiring a floor makes "asserted
    // nothing" a failure instead of a green run.
    if (m_expectMin >= 0 && m_passCount < static_cast<uint32_t>(m_expectMin))
        return ExitCode::AssertionFailed;

    if (m_maxErrors >= 0)
    {
        const uint32_t errs = Logger::Get().ErrorTotal();
        if (errs > static_cast<uint32_t>(m_maxErrors))
            return ExitCode::ErrorBudget;
    }
    return ExitCode::Ok;
}

int RunLedger::Finalize(int rawExitCode)
{
    // One-shot latch, not mere idempotence: a crashed run can reach here twice (the orderly
    // path racing the crash handler), and the second writer would truncate the first's file
    // to whatever it knows. InterlockedExchange makes the first caller the only one.
    if (::InterlockedExchange(&m_finalized, 1) != 0)
        return m_finalCode;

    // Drain BEFORE reading the counters: the error tally is incremented on the logger
    // worker as lines are formatted, so sampling it with entries in flight undercounts - and
    // those are the final frames' entries, the failures a verdict must catch.
    const bool drained = Logger::Get().DrainAndQuiesce();

    const uint32_t errs  = Logger::Get().ErrorTotal();
    const uint32_t warns = Logger::Get().WarningTotal();
    m_finalCode = ResolveExitCode(rawExitCode);

    if (m_finalCode == ExitCode::ErrorBudget && m_reason.empty())
        m_reason = "error_budget_exceeded";
    if (m_finalCode == ExitCode::AssertionFailed && m_reason.empty())
    {
        if      (m_scriptFaults > 0) m_reason = "script_fault";
        else if (m_failCount   > 0)  m_reason = "expectation_failed";
        else                         m_reason = "expect_min_not_met";
    }
    if (m_deviceLost && m_reason.empty())
        m_reason = "device_lost";
    if (m_reason.empty())
        m_reason = ExitCodeName(m_finalCode);

    if (!drained)
        LOG_WARNING("RunLedger: logger did not quiesce within the timeout; the "
                    "error count in run.json may undercount the tail of the run.");

    // -shader_variants: every shader variant the run used - after the teardown, which the
    // recipe snapshot survives, and before the unread-flag report below, so the flag counts
    // as read.
    WriteShaderVariantsIfRequested();
    WritePsoVariantsIfRequested();
    WritePakOrderIfRequested();

    // Flags given but never read. A misspelled flag is ignored by every reader, so only the
    // run can say so. Taken after teardown, so a flag consulted while shutting down counts as
    // read. INFO, not WARNING: packs bound engineWarnings; scenario_run decides what fails
    // (run.json unreadFlags).
    const std::vector<std::string> unread = Sys::CommandLine::Get().UnreadKeys();
    if (!unread.empty())
    {
        std::string list;
        for (const std::string& k : unread) { list += ' '; list += k; }
        LOG_INFO("[CMDLINE] unread flags (%zu):%s", unread.size(), list.c_str());
    }

    if (!m_enabled)
        return m_finalCode;   // no -artifacts=: verdict resolved, nothing to write

    std::string j;
    j.reserve(1024);
    j += "{\n";
    j += "  \"schema\": 1,\n";
    j += "  \"verdict\": \"";  j += (m_finalCode == ExitCode::Ok ? "pass" : "fail"); j += "\",\n";
    j += "  \"exitCode\": ";   j += std::to_string(m_finalCode); j += ",\n";
    j += "  \"exitName\": \""; j += ExitCodeName(m_finalCode);   j += "\",\n";
    j += "  \"reason\": \"";   j += JsonEscape(m_reason);        j += "\",\n";
    j += "  \"commandLine\": \""; j += JsonEscape(m_commandLine); j += "\",\n";
    {
        // Which build ran (System/BuildInfo.h).
        const Sys::BuildInfo& b = Sys::Build();
        j += "  \"build\": { \"product\": \""; j += JsonEscape(b.product);
        j += "\", \"version\": \"";             j += JsonEscape(b.version);
        j += "\", \"gitSha\": \"";              j += JsonEscape(b.gitSha);
        j += "\", \"dirty\": ";                 j += b.dirty ? "true" : "false";
        j += ", \"branch\": \"";                j += JsonEscape(b.branch);
        j += "\", \"config\": \"";              j += JsonEscape(b.config);
        j += "\", \"buildId\": \"";             j += JsonEscape(b.buildId); j += "\" },\n";
    }
    j += "  \"accessGuardStrict\": "; j += ECS::AccessGuard::IsStrict() ? "true" : "false"; j += ",\n";
    j += "  \"scene\": \"";    j += JsonEscape(m_scene);         j += "\",\n";
    j += "  \"framesRun\": ";  j += std::to_string(m_frames);    j += ",\n";
    j += "  \"artifactDir\": \""; j += JsonEscape(Sys::ArtifactDir()); j += "\",\n";
    // Where the log actually is, so a tool never reconstructs it: Sys::LogDir() is the
    // artifact dir for an -artifacts= run but a `log/` subfolder for the default roots.
    j += "  \"logPath\": \""; j += JsonEscape(Sys::LogDir() + "/DX12Log.txt");
    j += "\",\n";
    j += "  \"deviceLost\": ";  j += (m_deviceLost ? "true" : "false"); j += ",\n";
    j += "  \"deviceLostReason\": \""; j += JsonEscape(m_deviceLostReason); j += "\",\n";
    // The VRAM watch (NoteVram); null when the device never sampled it.
    if (const std::uint64_t minBudget = m_vramMinBudget.load(std::memory_order_relaxed))
    {
        char v[160];
        std::snprintf(v, sizeof(v),
                      "  \"vram\": {\"peakMB\": %llu, \"minBudgetMB\": %llu, \"overBudgetFrames\": %llu},\n",
                      static_cast<unsigned long long>(m_vramPeak.load(std::memory_order_relaxed) >> 20),
                      static_cast<unsigned long long>(minBudget >> 20),
                      static_cast<unsigned long long>(m_vramOverFrames.load(std::memory_order_relaxed)));
        j += v;
    }
    else j += "  \"vram\": null,\n";
    // The memory snapshot of the loop's exit (NoteMemory); null when the run never reached it.
    j += "  \"memory\": "; j += m_memoryJson.empty() ? "null" : m_memoryJson; j += ",\n";
    // Input provenance: frames the synthetic device held a key in (a game
    // action test, not OS input), and the replay / record files.
    j += "  \"input\": {\"syntheticFrames\": ";
    j += std::to_string(m_synthInputFrames.load(std::memory_order_relaxed));
    j += ", \"replay\": \""; j += JsonEscape(m_inputReplay);
    j += "\", \"record\": \""; j += JsonEscape(m_inputRecord); j += "\"},\n";
    j += "  \"loggerQuiesced\": "; j += (drained ? "true" : "false"); j += ",\n";
    j += "  \"display\": {\n";
    j += "    \"hdrMode\": \""; j += JsonEscape(m_hdrMode); j += "\",\n";
    j += "    \"compositionFormat\": \""; j += JsonEscape(m_compFormat); j += "\",\n";
    j += "    \"outputWidth\": "; j += std::to_string(m_outputWidth); j += ",\n";
    j += "    \"outputHeight\": "; j += std::to_string(m_outputHeight); j += ",\n";
    j += "    \"renderWidth\": "; j += std::to_string(m_renderWidth); j += ",\n";
    j += "    \"renderHeight\": "; j += std::to_string(m_renderHeight); j += ",\n";
    j += "    \"renderScale\": "; j += std::to_string(m_renderScale); j += "\n";
    j += "  },\n";
    // "recorded" separates "the flow ended with no state" from "nobody told the ledger" (a
    // crash before teardown, or the GameFlow probe unbound), which would otherwise publish a
    // confident "<none>".
    j += "  \"gameFlow\": {\n";
    j += "    \"recorded\": "; j += (m_flowFinalState.empty() ? "false" : "true");
    j += ",\n";
    j += "    \"finalState\": \""; j += JsonEscape(m_flowFinalState); j += "\",\n";
    j += "    \"transitions\": "; j += std::to_string(m_flowTransitions); j += ",\n";
    j += "    \"rejected\": ";    j += std::to_string(m_flowRejected);    j += ",\n";
    j += "    \"invalidStates\": "; j += std::to_string(m_flowInvalidStates); j += "\n";
    j += "  },\n";
    // What the main window counted. "recorded" separates "none" from "nobody told the ledger" (no
    // window, or a crash first). eventsDropped is the notifications its bounded queue refused.
    j += "  \"window\": {\n";
    j += "    \"recorded\": "; j += (m_windowNoted ? "true" : "false"); j += ",\n";
    {
        char one[96];
        std::snprintf(one, sizeof(one), "%.1f", m_windowUnpumpedMs);
        j += "    \"longestUnpumpedMs\": "; j += one; j += ",\n";
    }
    j += "    \"pumps\": "; j += std::to_string(m_windowPumps); j += ",\n";
    j += "    \"eventsDropped\": "; j += std::to_string(m_windowEventsDropped); j += ",\n";
    j += "    \"eventsMerged\": "; j += std::to_string(m_windowEventsMerged); j += ",\n";
    j += "    \"nestedPumps\": "; j += std::to_string(m_windowNestedPumps); j += ",\n";
    j += "    \"displayRefits\": "; j += std::to_string(m_windowDisplayRefits); j += "\n";
    j += "  },\n";
    j += "  \"expectations\": {\n";
    j += "    \"passed\": "; j += std::to_string(m_passCount);    j += ",\n";
    j += "    \"failed\": "; j += std::to_string(m_failCount);    j += ",\n";
    j += "    \"scriptFaults\": "; j += std::to_string(m_scriptFaults); j += ",\n";
    j += "    \"expectMin\": "; j += std::to_string(m_expectMin);    j += ",\n";
    j += "    \"firstFailure\": \""; j += JsonEscape(m_firstFailure); j += "\",\n";
    j += "    \"failures\": [";
    for (size_t fi = 0; fi < m_failures.size(); ++fi)
    {
        if (fi) j += ", ";
        j += "\""; j += JsonEscape(m_failures[fi]); j += "\"";
    }
    j += "]\n";
    j += "  },\n";
    j += "  \"counters\": {\n";
    j += "    \"engineErrors\": ";   j += std::to_string(errs);  j += ",\n";
    j += "    \"engineWarnings\": "; j += std::to_string(warns); j += ",\n";
    // Every DeclareAccess violation, not one per call site: scenario_run fails a pack on a non-zero
    // count whatever its engineErrors bound is (ECS/AccessGuard.h).
    j += "    \"accessGuardViolations\": ";
    j += std::to_string(ECS::AccessGuard::Violations().load(std::memory_order_relaxed)); j += "\n";
    j += "  },\n";
    // Finalize runs after the render owner joins. Crash JSON intentionally
    // omits this allocating telemetry; the runner rejects missing GPU evidence.
    j += "  \"gpuProfiledFrames\": " + std::to_string(m_gpuProfiledFrames) + ",\n";
    j += "  \"gpuRegionsDropped\": ";
    j += m_gpuProfiledFrames ? std::to_string(m_gpuRegionsDropped) : "null";
    j += ",\n  \"gpuPasses\": {";
    bool firstGpuPass = true;
    for (auto& [name, samples] : m_gpuPasses)
    {
        auto& values = samples.ms;
        if (values.empty()) continue;
        std::sort(values.begin(), values.end());
        const auto percentile = [&](size_t percent) {
            return values[(values.size() * percent + 99) / 100 - 1];
        };
        if (!firstGpuPass) j += ",";
        firstGpuPass = false;
        j += "\n    \"" + JsonEscape(name) + "\": {\"p50\": " + std::to_string(percentile(50));
        j += ", \"p90\": " + std::to_string(percentile(90));
        j += ", \"max\": " + std::to_string(values.back());
        j += ", \"frames\": " + std::to_string(values.size());
        j += ", \"age\": " + std::to_string(samples.age) + "}";
    }
    j += "\n  },\n";
    // The engine profiler's run-level block: level, output paths, validity totals, a
    // run-wide headline and the profiler's own overhead; null when no collector ran. App
    // stopped the collector before ShutdownAllSystems, so these numbers are final.
    j += "  \"profile\": "; j += Diag::Prof::RunProfileJson(); j += ",\n";
    // Where startup (and teardown) time went: named marks in ms since PROCESS CREATION, in
    // the order taken. A phase this run never reached is absent, not zero.
    // tools/boot_report.py aggregates them over a corpus run.
    j += "  \"bootPhasesMs\": {";
    {
        Sys::Boot::Phase phases[Sys::Boot::kMaxPhases];
        const std::size_t n = Sys::Boot::Snapshot(phases, Sys::Boot::kMaxPhases);
        for (std::size_t i = 0; i < n; ++i)
        {
            char one[96];
            std::snprintf(one, sizeof(one), "%s\"%s\": %.1f", i ? ", " : "", phases[i].name, phases[i].ms);
            j += one;
        }
    }
    j += "},\n";
    // The modules this exe knows: linked into it, and enabled by the project (game.json).
    j += "  \"modules\": {";
    {
        bool first = true;
        for (const Module::Registry::Status& s : Module::Registry::Get().Statuses())
        {
            j += first ? "" : ", ";
            first = false;
            j += "\"" + JsonEscape(s.name) + "\": {\"linked\": " + (s.linked ? "true" : "false") +
                 ", \"enabled\": " + (s.enabled ? "true" : "false") + "}";
        }
    }
    j += "},\n";
    // The blocks features keep under their own key (NoteSection).
    for (const auto& [name, json] : m_sections)
    {
        j += "  \"" + JsonEscape(name) + "\": " + json + ",\n";
    }
    // Normalised keys (lower case, one leading '-'), in argv order.
    j += "  \"unreadFlags\": [";
    for (std::size_t i = 0; i < unread.size(); ++i)
    {
        if (i) j += ", ";
        j += "\""; j += JsonEscape(unread[i]); j += "\"";
    }
    j += "],\n";
    j += "  \"maxErrors\": "; j += std::to_string(m_maxErrors); j += "\n";
    j += "}\n";

    // Temp + rename so a reader polling the directory never sees a partial
    // object. MoveFileExW with REPLACE_EXISTING is atomic within a volume.
    const std::string tmp = m_path + ".tmp";
    if (WriteWholeFile(tmp, j.data(), j.size()))
    {
        if (!::MoveFileExW(Sys::Utf8ToWide(tmp).c_str(),
                           Sys::Utf8ToWide(m_path).c_str(),
                           MOVEFILE_REPLACE_EXISTING))
        {
            LOG_ERROR("RunLedger: could not rename '%s' -> '%s' (GetLastError=%lu)",
                      tmp.c_str(), m_path.c_str(), ::GetLastError());
        }
    }
    else
    {
        LOG_ERROR("RunLedger: could not write '%s' (GetLastError=%lu)",
                  tmp.c_str(), ::GetLastError());
    }

    LOG_INFO("RunLedger: %s (exit %d %s), expects %u/%u, faults=%u, "
             "errors=%u warnings=%u, frames=%llu",
             m_finalCode == ExitCode::Ok ? "PASS" : "FAIL", m_finalCode,
             ExitCodeName(m_finalCode), m_passCount, m_passCount + m_failCount,
             m_scriptFaults, errs, warns,
             static_cast<unsigned long long>(m_frames));
    if (!m_firstFailure.empty())
        LOG_ERROR("RunLedger: first failure was %s", m_firstFailure.c_str());
    return m_finalCode;
}

void RunLedger::WriteCrashJson(int exitCode, const char* reason) const
{
    if (!m_enabled) return;
    if (::InterlockedExchange(&m_finalized, 1) != 0) return;

    // NO std::string, NO ofstream, NO nlohmann below this line: the heap may have just
    // died, and this is the last chance to leave a verdict. m_path was built at Begin(), so
    // reading it allocates nothing.
    // Fields are only what snprintf can produce from resident data; a crash's counters are
    // in DX12Log.txt, which ReportFatal has flushed by now. framesRun: NoteFrames runs only at
    // loop exit, so a crash or a watchdog kill reports the per-frame count. vram: atomics,
    // no allocation.
    const std::uint64_t frames = m_frames ? m_frames : m_framesSoFar.load(std::memory_order_relaxed);
    char vram[160] = "null";
    if (const std::uint64_t minBudget = m_vramMinBudget.load(std::memory_order_relaxed))
        std::snprintf(vram, sizeof(vram), "{\"peakMB\": %llu, \"minBudgetMB\": %llu, \"overBudgetFrames\": %llu}",
                      static_cast<unsigned long long>(m_vramPeak.load(std::memory_order_relaxed) >> 20),
                      static_cast<unsigned long long>(minBudget >> 20),
                      static_cast<unsigned long long>(m_vramOverFrames.load(std::memory_order_relaxed)));
    char buf[2048];
    const int n = std::snprintf(buf, sizeof(buf),
        "{\n"
        "  \"schema\": 1,\n"
        "  \"verdict\": \"fail\",\n"
        "  \"exitCode\": %d,\n"
        "  \"exitName\": \"%s\",\n"
        "  \"reason\": \"%s\",\n"
        "  \"crashed\": true,\n"
        "  \"framesRun\": %llu,\n"
        "  \"vram\": %s,\n"
        "  \"logPath\": \"%s\",\n"
        "  \"note\": \"written from the crash path; counters unavailable, see DX12Log.txt\"\n"
        "}\n",
        exitCode, ExitCodeName(exitCode),
        reason ? reason : "crash",
        static_cast<unsigned long long>(frames),
        vram,
        m_logPathJson);
    if (n <= 0 || n >= static_cast<int>(sizeof(buf))) return;   // truncated = not JSON

    // Straight to the final name: a crash has no second chance at a rename. m_pathW was
    // converted at Begin(); nothing allocates here.
    if (m_pathW[0] == 0) return;   // empty == crash writer disabled
    HANDLE h = ::CreateFileW(m_pathW, GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    ::WriteFile(h, buf, static_cast<DWORD>(n), &written, nullptr);
    ::CloseHandle(h);
}

}  // namespace Harness

#endif  // WITH_HARNESS
