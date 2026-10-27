#pragma once

// Per-frame CPU records of Diag::Prof.
//
// THE WINDOW. A frame's window is the MAIN thread's loop period
// [loop top of frame N, loop top of frame N+1). BeginFrame(N+1) closes frame N, so while
// frame N+1 runs the newest complete record is frame N: a one-frame lag every consumer
// reports as `profFrame` (the way gpuAgeFrames is reported, not assumed).
//
// STATS HALF. For every registered thread, the time spent in each category over the window:
// the category of its innermost open scope, "untracked" when none is open. Every scope
// boundary closes the outgoing interval (Profiler.h); the frame boundary reads each thread's
// counters through a seqlock and splits the interval the thread is still in. So a thread's
// categories sum to the window exactly: a worker in one long idle wait across the boundary
// is idle in both frames.
//
// RACES (SplitThreadWindow). A transition is stamped before it is published, so a worker
// preempted between the two publishes an interval the previous boundary already counted in
// the outgoing category: that time is taken back from the next window, never counted twice.
// A thread whose writer stays mid-transition through every read is split from its last
// consistent state. The record counts both (splitTakenBackUs, splitStale).
//
// TIMELINE HALF (Level::Timeline, filled by the collector thread one frame behind): all-idle
// (main working while no worker is) and the concurrency histogram, from the event rings with
// exact window clipping. `timelineValid` is false until then.
//
// Definitions match docs/perf/cpu_utilization_diagnosis.md:
//   W_total = sum of Work over the pool (main + render-owner lane + workers)
//   util    = W_total / (P * T)
//   S_main  = main's Work;  S_lane = the render-owner lane's Work
// util is recorded, never an acceptance criterion (it is the shadow of the span, not a target).

#include "Diagnostics/Profiler.h"
#include "Diagnostics/ProfilerMemory.h"

namespace Diag::Prof
{
    constexpr int kFrameThreads = 32;   // threads carried per record (registry order)
    constexpr int kFrameRing    = 256;  // records kept

    // What limited a frame (inputs in ProfilerGpu.h; the rule is in the collector's ProcessGpu).
    enum class Bound : uint8_t { Unknown = 0, Gpu = 1, Cpu = 2, Pacing = 3, Mixed = 4 };
    const char* BoundName(Bound b) noexcept;

    struct ThreadFrame
    {
        uint16_t index = 0;               // registry index (stable for the process)
        Role     role  = Role::Other;
        float    catMs[kCatCount]{};      // self time by category (Marker is always 0)
        float    untrackedMs = 0.f;       // no scope open (exact; categories + this = window)
        float    osCpuMs     = -1.f;      // OS-scheduled CPU in the window; < 0 = not measured
        uint32_t scopes      = 0;         // scopes closed in the window (the overhead driver)
        uint32_t allocs      = 0;         // operator-new calls on this thread in the window
    };

    struct FrameRecord
    {
        uint64_t frame   = 0;
        int64_t  beginNs = 0;             // main loop top of `frame`
        int64_t  endNs   = 0;             // main loop top of `frame + 1`
        float    periodMs = 0.f;
        Level    level    = Level::Off;
        bool     valid    = false;
        bool     osValid  = false;
        uint8_t  threadCount = 0;         // entries used in threads[]
        uint8_t  poolThreads = 0;         // P
        uint8_t  workerThreads = 0;
        ThreadFrame threads[kFrameThreads]{};

        // ---- Stats aggregates ----
        float wTotalMs       = 0.f;       // Work summed over the pool
        float sMainMs        = 0.f;
        float sLaneMs        = 0.f;       // 0 when there is no render-owner lane
        float workerWorkMs   = 0.f;
        float poolSpinMs     = 0.f;
        float util           = 0.f;       // W_total / (P * T)
        float mainUntrackedMs  = 0.f;
        float mainWaitGpuMs    = 0.f;
        float mainWaitJoinMs   = 0.f;
        float mainWaitPacingMs = 0.f;
        float osPoolCpuMs    = -1.f;      // OS CPU summed over the pool; < 0 = not measured

        // Whole-machine load over the window (GetSystemTimes / GetProcessTimes).
        // Raw 100 ns deltas so a window can be summed exactly; the per-frame
        // ratio is noisy (the counters tick at the clock interrupt).
        uint64_t sysBusy100ns  = 0;       // all processes, all cores
        uint64_t sysTotal100ns = 0;
        uint64_t procBusy100ns = 0;       // this process

        // ---- Timeline half (collector, one frame later) ----
        bool     timelineValid = false;
        float    allIdleMs     = 0.f;     // main in Work AND no worker in Work
        float    concurrencyMs[kFrameThreads + 1]{};   // time with exactly k pool threads in Work
        uint32_t events        = 0;       // ring events overlapping the window
        uint32_t eventsLost    = 0;       // overwritten before the collector read them

        // ---- Validity (deltas over the window) ----
        uint32_t droppedSuspend = 0;
        uint32_t orphaned       = 0;
        uint32_t droppedDeep    = 0;
        uint32_t crossThread    = 0;
        // The time split's own corrections (BeginFrame); neither breaks the identity
        // categories + untracked == window. splitStale: threads split from their last consistent
        // state (the writer was mid-transition through every read). splitTakenBackUs: time a
        // late-published transition had already reported in its outgoing category, taken back from
        // the other categories, summed over the threads.
        uint32_t splitStale       = 0;
        float    splitTakenBackUs = 0.f;

        // ---- Profiler's own cost (measured, not assumed) ----
        // boundaryUs: wall time of the BeginFrame call that produced this record, on main (the
        // profiler's only per-frame main-thread work besides its scopes); mainScopes x ScopeCostNs()
        // is the rest.
        float    boundaryUs     = 0.f;
        uint32_t mainScopes     = 0;
        uint32_t totalScopes    = 0;

        // ---- GPU (ProfilerGpu.h) ----
        // Patched when the frame's timestamps come back, gpuAgeFrames later; the derived half also
        // needs frame + 1.
        bool     gpuValid      = false;
        bool     gpuCalibrated = false;
        uint16_t gpuRegions    = 0;
        uint16_t gpuDropped    = 0;
        float    gpuBusyMs     = 0.f;
        float    gpuQueueMs[3]{};
        float    gpuOverlapMs  = 0.f;
        int64_t  gpuFirstNs    = 0;
        int64_t  gpuLastNs     = 0;
        int64_t  gpuSubmitNs   = 0;
        bool     gpuDerived    = false;
        float    gpuPeriodMs   = 0.f;
        float    gpuUtil       = 0.f;
        float    gpuStarveMs   = 0.f;
        float    gpuBubbleMs   = 0.f;
        float    gpuSubmitToStartMs = 0.f;
        bool     gpuSubmitLate = false;
        Bound    bound         = Bound::Unknown;

        // ---- Memory ----
        // Heap traffic: counted in the global operator new/delete replacement (per thread, summed
        // here; allocs includes unregistered threads). operator new only: Lua's allocator, FreeType
        // and prebuilt libraries' own malloc are not in it - they show in the working set below.
        uint32_t allocs      = 0;
        uint32_t frees       = 0;
        uint64_t allocBytes  = 0;
        // Sampled by the collector at the boundary (like the OS CPU column):
        // memValid is false until it did.
        bool     memValid          = false;
        uint64_t vramLocalBytes    = 0;   // DXGI local segment CurrentUsage
        uint64_t vramLocalBudget   = 0;
        uint64_t vramNonLocalBytes = 0;   // system memory the GPU is using
        uint64_t workingSetBytes   = 0;
        uint64_t privateBytes      = 0;   // commit charge
        uint32_t pageFaults        = 0;   // delta over the window (soft + hard)

        // ---- GPU resource ledger + gauges (ProfilerMemory.h) ----
        // Snapshotted at the boundary; valid in every record of a profiler build. Bytes the engine
        // created and D3D12 has not destroyed yet.
        uint64_t gpuLedgerBytes[kGpuMemCats]{};
        int32_t  gpuLedgerCount[kGpuMemCats]{};
        int32_t  gpuPlacedCount = 0;
        uint64_t uploadBytes    = 0;      // staging bytes copied during the window
        float    gauges[kGauges]{};       // NaN = nobody reported it
    };

    // Measured cost of one scope begin+end on the calling thread at the CURRENT
    // level (see CalibrateScopeCost). 0 until calibrated.
    double ScopeCostNs() noexcept;
    // Time @p iterations empty scopes on the calling thread (must be a
    // registered thread with the level on) and remember the per-scope cost.
    double CalibrateScopeCost(uint32_t iterations) noexcept;

    // Main thread, loop top: closes the previous frame's window (producing its
    // record) and opens `frame`'s. Also sets the main thread's frame tag.
    // A no-op below Level::Stats apart from the tag and CurrentFrame().
    void BeginFrame(uint64_t frame) noexcept;

    // One thread's share of a closing window, as BeginFrame splits it (pure arithmetic,
    // tests/unit/profiler_time_split_test.cpp).
    //   cumNs      the thread's time per slot at the window edge: published accNs plus the
    //              current interval (slot @p cur) up to the edge. Signed: a transition stamped
    //              after the edge makes that interval negative; the slot sum stays exact.
    //   reportedNs cumulative time the earlier windows reported (zeros for a new thread).
    // Writes outNs >= 0 summing to sum(cumNs) - sum(reportedNs) (the window, by telescoping) and
    // adds it to reportedNs. A slot with a negative delta (the previous edge counted a
    // late-published transition's interval in the outgoing slot) reports 0, and that much is taken
    // back from the positive deltas, current slot first, then the largest: reportedNs never
    // decreases, so an over-reported slot stays silent until its true total catches up and nothing
    // is counted twice. Returns the time taken back, in ns.
    int64_t SplitThreadWindow(const int64_t (&cumNs)[kAccCount], uint8_t cur,
                              int64_t (&reportedNs)[kAccCount], uint64_t (&outNs)[kAccCount]) noexcept;

    // The frame the main thread is in (set by BeginFrame at every level): what
    // a capture request made "now" is relative to.
    uint64_t CurrentFrame() noexcept;

    // Copy a record out. false when it is not (or no longer) in the ring.
    bool GetFrameRecord(uint64_t frame, FrameRecord& out) noexcept;
    bool GetLatestFrameRecord(FrameRecord& out) noexcept;
    uint64_t LatestRecordedFrame() noexcept;   // ~0ull when none

    // Collector side: fill the timeline half of an existing record.
    struct TimelinePatch
    {
        float    allIdleMs = 0.f;
        float    concurrencyMs[kFrameThreads + 1]{};
        uint32_t events = 0;
        uint32_t eventsLost = 0;
    };
    bool PatchTimeline(uint64_t frame, const TimelinePatch& patch) noexcept;

    // Collector side: the GPU numbers that need frame + 1 (ProfilerGpu.h).
    struct GpuDerivedPatch
    {
        float periodMs = 0.f, util = 0.f, starveMs = 0.f, bubbleMs = 0.f, submitToStartMs = 0.f;
        bool  submitLate = false;
        Bound bound = Bound::Unknown;
    };
    bool PatchGpuDerived(uint64_t frame, const GpuDerivedPatch& patch) noexcept;

    // Called (on main, from BeginFrame, after the record is published) with the
    // frame that just completed. One slot; the collector owns it.
    using FrameCompletedFn = void (*)(uint64_t completedFrame) noexcept;
    void SetFrameCompletedCallback(FrameCompletedFn fn) noexcept;

    // OS per-thread CPU sampling (QueryThreadCycleTime per registered thread,
    // plus GetSystemTimes / GetProcessTimes for the machine's load). On by
    // default; `-profile_os=0` turns it off. It runs on the COLLECTOR thread,
    // never on main: SampleOs(frame) is called once per completed frame right
    // after the collector wakes, and patches the deltas into that frame's
    // record (osValid). Without a running collector there is no OS column.
    void SetOsSampling(bool on) noexcept;
    bool OsSampling() noexcept;
    void SampleOs(uint64_t frame) noexcept;

    // Video memory for the collector's per-frame sample. GraphicsDX12 installs
    // it once the adapter exists; it is called on the COLLECTOR thread
    // (IDXGIAdapter3::QueryVideoMemoryInfo is free-threaded), never on main.
    struct VideoMemory
    {
        uint64_t localUsage = 0, localBudget = 0, nonLocalUsage = 0, nonLocalBudget = 0;
    };
    using VideoMemoryQueryFn = bool (*)(VideoMemory& out) noexcept;
    void SetVideoMemoryQuery(VideoMemoryQueryFn fn) noexcept;

    // TSC frequency used to convert thread cycles to time (0 until calibrated).
    double TscHz() noexcept;
}
