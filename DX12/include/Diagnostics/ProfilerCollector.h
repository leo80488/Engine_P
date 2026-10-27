#pragma once

// Diag::Prof collector - the consumer half of the engine profiler.
// One background thread, started only when a level is on. Per completed frame it:
//   * drains every thread's event ring (Timeline+),
//   * computes the timeline half of the frame record (all-idle, the concurrency histogram) one
//     frame late, so a task that started in frame F and finished in F+1 is already in the ring
//     when F is analysed,
//   * appends the full record to <artifacts>/profile/records.jsonl and the frame's GPU pass table
//     to <artifacts>/profile/passes.jsonl,
//   * feeds any capture window, and when one completes writes <artifacts>/profile/<name>.trace.json
//     (Chrome Trace Event Format: open it in https://ui.perfetto.dev or chrome://tracing) and
//     rewrites <artifacts>/profile/profile_summary.json.
// It is a registered thread (Role::Other, "Profiler.Collector") on purpose: its own CPU shows up in
// the per-thread table instead of silently competing with the workers it measures.
// Flags (all validated: an unknown -profile* flag refuses the run with exit 12, because
// Sys::CommandLine has no table of known flags and a typo would otherwise measure nothing):
//   -profile=off|stats|timeline|detail     level (bare -profile = timeline)
//   -profile_window=<first>:<count>[:name][,...]   capture windows (Timeline+)
//   -profile_os=0|1                         per-thread OS CPU sampling (default 1)
//   -profile_trace=json|none                trace file per window (default json)
//   -profile_gpu=0|1                        GPU timestamps -> GPU timeline (default 1; the boot
//                                           raises a GPU-profiler want for it)
//   -profile_alloc=0|1                      per-thread operator new/delete counting
//                                           (default 1; the replacement stays)
//   -profile_hitch=<ms>[:<max>[:<fromFrame>]]  capture frames slower than <ms> (at most
//                                           <max>, default 3; none before <fromFrame>,
//                                           default 60) with 10 frames of lead-in and
//                                           5 after, and explain each (Timeline+)
//   -profile_parity=0|1                     CPU parity check (default 0, Timeline+): the
//                                           Scheduler's own per-system timer is fed in
//                                           (NoteExternalTiming) and compared with the
//                                           profiler's scope of the same system in the
//                                           same frame; "[PROFILER] parity cpu:" at stop
// Output needs -artifacts=; the level alone (HUD / Lua / live use) does not.
// Hitches: while -profile_hitch is on, the collector keeps the last kHistoryFrames frames' share of
// a window (events, GPU spans, per-name self time) in a flight recorder, so a slow frame is captured
// after the fact: its window hitch_<frame> starts 10 frames before it. The explanation (summary
// "hitch" block + a "[PROFILER] hitch frame=" log line) ranks, per thread role, the scopes whose
// self time in the slow frame grew most over their median in the frames before; worker idle is left
// out, and a thread's time with no scope open counts as "(untracked)". A slow frame inside the
// previous hitch's window is not captured again.
// On-demand windows: BeginNamedWindow / EndNamedWindow (Lua Profiler.BeginWindow / EndWindow)
// bracket frames [current + 1, current at End]; RequestCapture takes the next <count> frames. They
// land in the same summary and trace files as -profile_window windows (kind "named"), so one
// scenario run can A/B a feature by window name.

#include "Diagnostics/Profiler.h"
#include "Diagnostics/ProfilerGpu.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Diag::Prof
{
    struct CaptureRequest
    {
        std::string name;
        uint64_t    first = 0;
        uint32_t    count = 0;
    };

    struct CollectorConfig
    {
        Level level = Level::Off;
        bool  osSampling = true;
        bool  traceJson  = true;
        bool  gpu        = true;
        bool  allocCounting = true;
        float    hitchMs   = 0.f;      // 0 = off
        uint32_t hitchMax  = 3;
        uint64_t hitchFrom = 60;
        bool     parity    = false;
        std::vector<CaptureRequest> windows;
        // Where window files go; empty = <artifacts>/profile (nothing without
        // -artifacts=). The editor's Profiler window points it at log/profile/.
        std::string outDir;
        // records.jsonl / passes.jsonl, one row per frame. Off for the editor's
        // own collector: an editor session left open for an hour would write
        // gigabytes. Capture windows are written either way.
        bool streams = true;
    };

    // What the collector has done so far, readable from ANY thread (the
    // control channel answers `profile status` on its pipe thread, so it works
    // while the frame loop is blocked).
    struct CollectorStatus
    {
        bool     running = false;
        Level    level = Level::Off;
        uint64_t processed = 0, timelineFrames = 0;
        uint64_t latestFrame = ~0ull;           // newest processed frame, ~0ull = none
        uint32_t windowsRequested = 0, windowsFinished = 0, hitches = 0;
        uint64_t eventsLost = 0;
    };

    // Parse and validate every -profile* argument. false = refuse the run;
    // @p error says why. Never logs (the caller does, with its own prefix).
    bool ParseCommandLine(CollectorConfig& out, std::string& error);

    // Apply the level and, when it is on, start the collector thread.
    void StartCollector(const CollectorConfig& config);

    // Finish every window that has data (a window cut short by the end of the
    // run is written and marked truncated), flush, join. Idempotent.
    void StopCollector();

    bool CollectorRunning() noexcept;

    // Thread-safe. false when there is no collector (no -profile, or a level
    // only switched on from Lua). Names are sanitised to [A-Za-z0-9_-] and made
    // unique per run (a second "x" becomes "x_2").
    bool BeginNamedWindow(const std::string& name);
    bool EndNamedWindow(const std::string& name);
    bool RequestCapture(const std::string& name, uint32_t frames);

    // Thread-safe snapshot of the collector's progress. false = no collector.
    bool GetCollectorStatus(CollectorStatus& out) noexcept;

    // The newest complete frame record, in the records.jsonl row format.
    // false when there is none (level off, or the first frame not closed yet).
    // Needs no collector - only the recorder.
    bool LatestRecordJson(std::string& out);

    // run.json's "profile" block: a JSON object - level, output paths,
    // validity totals, a run-wide headline (frame / CPU / GPU / bound /
    // memory), overhead, parity - or "null" when no collector ran.
    // Call after StopCollector.
    std::string RunProfileJson();

    // -profile_parity: an external timer's reading of one named scope in one
    // frame (Scheduler::RunPhase feeds every system it runs). Thread-safe; a
    // no-op unless parity is on. ParityEnabled is one relaxed load.
    bool ParityEnabled() noexcept;
    void NoteExternalTiming(uint64_t frame, const char* name, float ms);

    // ---- The editor's Profiler window ----
    // A copy of the newest frames' events, per thread, and their GPU spans - the flight recorder the
    // hitch capture uses, kept while any caller wants it (SetLiveTimeline(true)) or -profile_hitch is
    // on. Timeline+ frames only.
    struct LiveThread
    {
        uint16_t           index = 0;
        Role               role  = Role::Other;
        char               name[48]{};
        std::vector<Event> events;        // END order within each frame
    };
    struct LiveTimeline
    {
        std::vector<uint64_t> frames;     // oldest first
        std::vector<int64_t>  frameBegin; // loop top (ns) of each frame, plus the end of the last
        std::vector<LiveThread> threads;
        std::vector<GpuSpan>  gpu;
    };
    void SetLiveTimeline(bool on) noexcept;
    bool CopyLiveTimeline(uint32_t frames, LiveTimeline& out);

    // Windows written so far this run (any thread).
    struct CaptureInfo
    {
        std::string name, kind, trace;
        uint64_t    first = 0;
        uint32_t    count = 0, recorded = 0;
        bool        complete = false;
    };
    std::vector<CaptureInfo> ListCaptures();
    std::string CollectorOutputDir();     // where window files go; empty = none

    // -profile_hitch at runtime: capture frames slower than `ms` (at most
    // `max` more; ms <= 0 turns it off). false = no collector.
    bool SetHitchCapture(float ms, uint32_t max);
    std::string ProfileOutputDir();   // "<artifacts>/profile", empty without -artifacts=
}
