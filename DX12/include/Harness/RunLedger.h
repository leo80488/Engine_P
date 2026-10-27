#pragma once
// Harness/RunLedger.h - the machine-readable verdict for one run (run.json).
// The exit code is one integer and the log must be grepped with patterns that break when
// a message is reworded; run.json is the stable channel: one small file saying what the
// run was asked to do, what it did, and whether that counts as a pass.
// It is always written - on the orderly exit, a C++ exception, an SEH crash and a
// watchdog kill - so a run with no run.json failed before the harness was alive, which is
// itself the diagnosis.
// Two writers: Finalize() is the orderly path (drains the logger, resolves the verdict,
// writes temp + rename so a reader never sees half an object); WriteCrashJson() is the
// dying path (fixed stack buffer, snprintf, raw CreateFileW/WriteFile, no allocation -
// the heap is what may have just died).
// Exit-code resolution lives here only (ResolveExitCode), so the number in run.json and
// the number the process returns cannot disagree.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include <map>

struct GPUProfiler;

#ifndef WITH_HARNESS
// Retail: an inert stub (see the note in Determinism.h). ResolveExitCode still passes
// the raw code through rather than returning 0: a shipped process that always exits 0 is
// the defect the harness exists to fix, and ExitCodes.h stays in Retail for that reason.
namespace Harness
{
    class RunLedger
    {
    public:
        static RunLedger& Get() { static RunLedger s_instance; return s_instance; }

        void Begin() {}
        bool Enabled() const { return false; }

        void NoteScene(const std::string&) {}
        void NoteFrames(std::uint64_t)     {}
        void NoteFrameProgress(std::uint64_t) {}
        void NoteVram(std::uint64_t, std::uint64_t) {}
        void NoteSyntheticInputFrame() {}
        void NoteInputFiles(const std::string&, const std::string&) {}
        void NoteGpuFrame(const GPUProfiler&) {}
        void NoteGpuRegionsDropped(std::uint32_t) {}
        void NoteDeviceLost(const char*)   {}
        bool DeviceLost() const { return false; }
        void NoteReason(const std::string&) {}
        void NoteDisplay(const char*, const char*, std::uint32_t, std::uint32_t,
                         std::uint32_t, std::uint32_t, float) {}
        void NoteGameFlow(const std::string&, std::uint32_t, std::uint32_t,
                          std::uint32_t) {}
        void NoteWindow(double, std::uint64_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) {}
        void NoteSection(const char*, const std::string&) {}

        void NoteExpectation(bool, const char*, const char*) {}
        void NoteScriptFault(const char*, const char*)        {}
        std::uint32_t PassCount()    const { return 0; }
        std::uint32_t FailCount()    const { return 0; }
        std::uint32_t ScriptFaults() const { return 0; }

        int  ResolveExitCode(int rawExitCode) const { return rawExitCode; }
        int  Finalize(int rawExitCode)              { return rawExitCode; }
        void WriteCrashJson(int, const char*) const {}

    private:
        RunLedger() = default;
    };
}
#else
namespace Harness
{
    class RunLedger
    {
    public:
        static RunLedger& Get();

        /// Capture the invocation. Called from main.cpp once the logger is up.
        /// Cheap and safe when no -artifacts= was given (the ledger then simply
        /// never writes a file, but still resolves the exit code).
        void Begin();

        /// True when a run.json will actually be produced, i.e. -artifacts= was
        /// given. Everything else on this class works regardless.
        bool Enabled() const { return m_enabled; }

        // ---- Facts the run accumulates ----
        void NoteScene(const std::string& scene)  { m_scene = scene; }
        /// Also snapshots the run's memory for run.json "memory": the GPU ledger by category, its ten largest
        /// objects, each secondary view's bytes and the process's private bytes. Called at loop exit, before the
        /// teardown gives the ledger back.
        void NoteFrames(std::uint64_t frames);

        /// The frame count so far, updated every frame by the main loop. NoteFrames at loop
        /// exit is the orderly number; this is the one the crash writer and the watchdog report.
        void NoteFrameProgress(std::uint64_t frames)
        {
            m_framesSoFar.store(frames, std::memory_order_relaxed);
        }

        /// The process's local video memory, sampled by the device every frame (DXGI
        /// QueryVideoMemoryInfo: CurrentUsage against the OS budget); keeps the peak usage, the
        /// lowest budget and the frames over it. Past its budget the driver pages over PCIe (GPU
        /// waits of seconds a frame), which reads as a hang or a regression; several engines on
        /// one card (scenario_run --jobs, other sessions) are the usual cause, and the runner
        /// reads this to tell the machine's failure from the engine's and to learn each pack's
        /// VRAM need. Atomics: the render owner writes, the crash writer and the watchdog read.
        void NoteVram(std::uint64_t usageBytes, std::uint64_t budgetBytes);

        /// Where the run's input came from (run.json "input"): the frames in
        /// which the harness's SYNTHETIC device held a key (Input::SynthHold,
        /// -input_replay) - such a run tests a game action through the action
        /// mapping, never OS input - and the replay / record files. Main thread.
        void NoteSyntheticInputFrame() { m_synthInputFrames.fetch_add(1, std::memory_order_relaxed); }
        void NoteInputFiles(const std::string& replay, const std::string& record)
        {
            m_inputReplay = replay;
            m_inputRecord = record;
        }

        // Called only on the render owner after fence-safe profiler readback.
        // Duplicate region names are summed within a frame, never across parent
        // and child names. age is the maximum observed readback age.
        void NoteGpuFrame(const GPUProfiler& profiler);
        void NoteGpuRegionsDropped(std::uint32_t count);

        /// Latched when the D3D12 device is lost (TDR / DEVICE_REMOVED). A separate verdict
        /// (ExitCode::DeviceLost): an automated run hits it most often when something else
        /// competes for the GPU (an editor session, a driver update, another test). It says
        /// nothing about the code under test, so folding it into the generic exception bucket
        /// would make every such run look like a regression.
        void NoteDeviceLost(const char* reason);
        bool DeviceLost() const { return m_deviceLost; }

        /// The display configuration this run RESOLVED to - not the requested one: a display
        /// that refuses HDR leaves the request set. A golden image needs it: with HDR output the
        /// final texture is R16G16B16A16_FLOAT in PQ/Rec.2020 and CaptureTextureToPNG
        /// down-converts by saturate+quantise, so the SDR and HDR PNGs of one frame differ
        /// (everything above paper white clips).
        /// @p outputW / @p outputH: the swap-chain size - a screenshot's pixel dimensions.
        /// @p renderW / @p renderH: the internal render size, different whenever an upscaler is
        /// active.
        /// @p renderScale: native (1.0) and upscaled give different images at the same output
        /// size, so the size alone does not make two captures comparable.
        void NoteDisplay(const char* hdrMode, const char* compositionFormat,
                         std::uint32_t outputW, std::uint32_t outputH,
                         std::uint32_t renderW, std::uint32_t renderH,
                         float renderScale);

        /// Free-text reason for the verdict, e.g. "watchdog_timeout".
        void NoteReason(const std::string& reason) { m_reason = reason; }

        /// Where the top-level game flow ended, recorded once at teardown: when a flow assertion
        /// fails the next question is "what state DID it reach, and was a request refused", and
        /// the state machine is serialised nowhere else.
        /// @p transitions / @p rejected are counted over the retained history ring
        /// (GameFlowStateMachine::kHistoryCapacity records), so on a longer run they are lower
        /// bounds - enough for "was anything refused"; -dumpflow=<path> writes the exact picture.
        /// @p invalidStates is exact: quarantined definitions in the live table. Non-zero means
        /// validation rejected a state (bad parent, unknown set) and every switch into it is
        /// refused for the whole run.
        /// Does not move the verdict itself: ValidateDefinitions already LOG_ERRORs each finding,
        /// so -maxerrors=N is the knob.
        void NoteGameFlow(const std::string& finalState,
                          std::uint32_t transitions,
                          std::uint32_t rejected,
                          std::uint32_t invalidStates);

        /// What the main window counted (Plat::MainWindowStats), at its destruction: the longest time
        /// between two message pumps since it was shown, the notifications its bounded queue dropped and
        /// merged, the message pumps run while main waited for the render-owner lane, the fullscreen
        /// refits. run.json "window".
        void NoteWindow(double longestUnpumpedMs, std::uint64_t pumps, std::uint32_t eventsDropped,
                        std::uint32_t eventsMerged, std::uint32_t nestedPumps, std::uint32_t displayRefits);

        /// A block a feature keeps in run.json under its own top-level key: @p json is a complete JSON
        /// value, replacing what @p name held before. The feature calls it whenever its state changes
        /// (the data tables' refusals), so a run that ends any way still records the latest, and a
        /// scenario asserts it with expect.runJson. @p name must not be a key run.json already has.
        void NoteSection(const char* name, const std::string& json);

        // ---- Scenario assertions ----
        /// One Test.Expect / Test.Fail result. @p site is a "file.lua:line" attribution
        /// resolved at the Lua call site.
        /// These only record, never raise a Lua error: raising would unwind OnUpdate, be
        /// double-counted as a script fault by the traceback handler, and park the driver (the
        /// script system clears hasUpdate after a throw), losing every later assertion.
        void NoteExpectation(bool passed, const char* site, const char* message);

        /// A Lua error: the -exec'd chunk failed to load / return a table, or a callback threw
        /// at runtime. Its own counter, and it fails the run even if no expectation ran: after a
        /// throw in OnUpdate the script system sets hasUpdate = false and only the editor's
        /// ExitPlay re-arms it, so a Game.exe run would render on to -frames=N with a dead
        /// scenario and exit 0.
        void NoteScriptFault(const char* site, const char* what);

        std::uint32_t PassCount()    const { return m_passCount; }
        std::uint32_t FailCount()    const { return m_failCount; }
        std::uint32_t ScriptFaults() const { return m_scriptFaults; }

        // ---- Verdict ----
        /// Fold the raw process exit code together with everything observed during the run.
        /// Precedence, highest first: DeviceLost (an environment failure; the run proved
        /// nothing), a non-zero @p rawExitCode (something more specific already decided), a
        /// script fault, a failed expectation, a pass count under -expect_min (these three
        /// AssertionFailed), logged errors over -maxerrors=N (ErrorBudget), else Ok.
        /// Call it after the logger has drained, or the error count trails the log and the last
        /// frames' failures vanish.
        int ResolveExitCode(int rawExitCode) const;

        /// Drain the logger, resolve the verdict, write run.json and return the final exit code.
        /// One-shot: a second call (the orderly path racing the crash handler) is ignored and
        /// returns the first call's result.
        /// Call it after TaskSystem::Shutdown() (whose worker-exception path still logs errors)
        /// and before Logger::Shutdown() closes the file.
        int Finalize(int rawExitCode);

        /// Allocation-free emergency writer for the crash path. Produces the
        /// same field names as Finalize with whatever is known.
        void WriteCrashJson(int exitCode, const char* reason) const;

    private:
        void NoteMemory();

        struct GpuPassSamples
        {
            std::vector<float> ms;
            std::uint32_t age = 0;
        };

        RunLedger() = default;

        std::string  m_path;                 // <artifacts>/run.json
        std::string  m_commandLine;
        std::string  m_scene;
        std::string  m_reason;
        std::string  m_deviceLostReason;
        std::string  m_flowFinalState;        // empty == NoteGameFlow never ran
        std::uint32_t m_flowTransitions  = 0;
        std::uint32_t m_flowRejected     = 0;
        std::uint32_t m_flowInvalidStates = 0;
        bool          m_windowNoted        = false;   // NoteWindow ran
        double        m_windowUnpumpedMs   = 0.0;
        std::uint64_t m_windowPumps        = 0;
        std::uint32_t m_windowEventsDropped = 0, m_windowEventsMerged = 0, m_windowNestedPumps = 0,
                      m_windowDisplayRefits = 0;
        std::map<std::string, std::string> m_sections;   // NoteSection: name -> JSON value
        std::string  m_hdrMode      = "unknown";
        std::string  m_compFormat   = "unknown";
        std::uint32_t m_outputWidth  = 0;
        std::uint32_t m_outputHeight = 0;
        std::uint32_t m_renderWidth  = 0;
        std::uint32_t m_renderHeight = 0;
        float         m_renderScale  = 1.0f;
        std::string  m_firstFailure;         // "site: message" of failure #1
        std::vector<std::string> m_failures; // all of them, for run.json
        std::uint32_t m_passCount    = 0;
        std::uint32_t m_failCount    = 0;
        std::uint32_t m_scriptFaults = 0;
        int          m_expectMin     = -1;   // -1 = no minimum required
        std::uint64_t m_frames     = 0;
        std::string   m_memoryJson;          // empty == NoteMemory never ran

        std::map<std::string, GpuPassSamples> m_gpuPasses;
        std::uint64_t m_gpuProfiledFrames = 0;
        std::uint64_t m_gpuRegionsDropped = 0;
        std::atomic<std::uint64_t> m_framesSoFar{ 0 };
        std::atomic<std::uint64_t> m_vramPeak{ 0 };          // bytes
        std::atomic<std::uint64_t> m_vramMinBudget{ 0 };     // bytes; 0 = never sampled
        std::atomic<std::uint64_t> m_vramOverFrames{ 0 };
        std::atomic<std::uint64_t> m_synthInputFrames{ 0 };
        std::string                m_inputReplay, m_inputRecord;
        int          m_maxErrors   = -1;     // -1 = no budget (default)
        bool         m_enabled     = false;
        bool         m_deviceLost  = false;
        // Guards the Note* mutators and the counters: NoteDeviceLost is reachable from
        // ThrowIfFailedImpl on ANY thread (a TaskSystem worker creating a resource), the scenario
        // counters from the main thread's Lua tick. WriteCrashJson does not take it: a crash
        // handler must never block on a mutex the dead thread may hold.
        mutable std::mutex m_mutex;

        mutable long m_finalized   = 0;      // InterlockedExchange latch
        mutable int  m_finalCode   = 0;

        // The run.json path, converted to UTF-16 at Begin() into a fixed buffer:
        // WriteCrashJson must not call Sys::Utf8ToWide (it allocates), so the dying path is a
        // pure CreateFileW/WriteFile pair.
        wchar_t      m_pathW[512]  = { 0 };

        // The log path, JSON-escaped at Begin() into a fixed buffer for the same reason:
        // run.json publishes "logPath", and a crash is exactly when finding the log matters.
        char         m_logPathJson[512] = { 0 };
    };
}
#endif  // WITH_HARNESS
