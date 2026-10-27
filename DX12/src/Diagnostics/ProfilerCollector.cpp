// Diag::Prof collector (the contract is in include/Diagnostics/ProfilerCollector.h).

#include "Diagnostics/ProfilerCollector.h"
#include "Diagnostics/ProfilerMemory.h"
#include "Diagnostics/ProfilerFrame.h"
#include "Diagnostics/ProfilerGpu.h"

#include "System/CommandLine.h"
#include "System/PathUtil.h"
#include "System/Log.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <intrin.h>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if DIAG_PROFILER

namespace Diag::Prof
{
namespace
{
    constexpr uint8_t  kSegUntracked   = static_cast<uint8_t>(kUntracked); // no scope open
    constexpr uint32_t kTraceFrameCap  = 120;    // frames per window written to trace.json
    constexpr float    kQuietExternalPct = 3.0f; // machine counted "quiet" below this
    // A frame waits for the GPU timestamps of itself AND the next frame (the
    // derived numbers compare the two). They come back ~3 frames late; past
    // this many completed frames the frame is processed without them.
    constexpr uint64_t kGpuWaitFrames  = 8;
    // Hitch flight recorder: frames kept for after-the-fact capture, and the
    // shape of a hitch window around the slow frame.
    constexpr uint32_t kHistoryFrames  = 24;
    constexpr uint64_t kHitchPre       = 10;
    constexpr uint64_t kHitchPost      = 5;
    constexpr uint32_t kHitchTopDeltas = 8;
    // A CPU-parity pair further apart than this is counted per name (the
    // expected difference is two clock reads + the 100 ns duration quantum).
    constexpr double   kParityBadUs    = 5.0;
    // A named window whose EndWindow has not arrived (+1 must not wrap).
    constexpr uint64_t kOpenEnded      = ~0ull - 4;
    // The explanation's name for "no scope open on this thread".
    constexpr NameId   kUntrackedName  = 0xFFFF;

    constexpr double   kMinTrendSec    = 10.0;   // memory slopePerMin needs at least this span
    constexpr double   kBubbleMinMs    = 0.02;   // a GPU gap shorter than this is not a bubble

    const char* KeyName(NameId n) { return n == kUntrackedName ? "(untracked)" : NameOf(n); }

    // ==== Small helpers ====
    struct Dist { double p50 = 0, p95 = 0, p99 = 0, max = 0, mean = 0; size_t n = 0; };

    void Appendf(std::string& s, const char* fmt, ...)
    {
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        const int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        if (n > 0) s.append(buf, static_cast<size_t>(n < static_cast<int>(sizeof(buf)) ? n : sizeof(buf) - 1));
    }

    void AppendJsonString(std::string& s, const char* v)
    {
        s.push_back('"');
        for (const char* p = v ? v : ""; *p; ++p)
        {
            const unsigned char c = static_cast<unsigned char>(*p);
            if (c == '"' || c == '\\') { s.push_back('\\'); s.push_back(static_cast<char>(c)); }
            else if (c < 0x20) Appendf(s, "\\u%04x", c);
            else s.push_back(static_cast<char>(c));
        }
        s.push_back('"');
    }

    // Finite or JSON null. A NaN in a report is a bug that must not become a
    // parse error for every tool downstream of it.
    void AppendNum(std::string& s, double v, int decimals = 4)
    {
        if (!std::isfinite(v)) { s += "null"; return; }
        Appendf(s, "%.*f", decimals, v);
    }

    std::string Lower(std::string v)
    {
        for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return v;
    }

    const char* BuildConfigName()
    {
#if defined(_DEBUG)
        return "Debug";
#elif defined(TRACY_ENABLE)
        return "Profile";
#elif defined(WITH_HARNESS)
        return "Release";
#else
        return "Retail";
#endif
    }

    std::string CpuBrand()
    {
        int regs[4]{};
        char brand[49]{};
        __cpuid(regs, 0x80000000);
        if (static_cast<unsigned>(regs[0]) < 0x80000004u) return "unknown";
        for (int i = 0; i < 3; ++i)
        {
            __cpuid(regs, 0x80000002 + i);
            std::memcpy(brand + i * 16, regs, 16);
        }
        std::string s = brand;
        while (!s.empty() && s.front() == ' ') s.erase(s.begin());
        return s;
    }

    Dist Distribution(std::vector<double> v)
    {
        Dist d;
        v.erase(std::remove_if(v.begin(), v.end(), [](double x) { return !std::isfinite(x); }), v.end());
        d.n = v.size();
        if (v.empty()) return d;
        std::sort(v.begin(), v.end());
        auto rank = [&](double p) {
            size_t idx = static_cast<size_t>(std::ceil(p * static_cast<double>(v.size())));
            if (idx == 0) idx = 1;
            return v[(std::min)(idx, v.size()) - 1];
        };
        d.p50 = rank(0.50); d.p95 = rank(0.95); d.p99 = rank(0.99);
        d.max = v.back();
        double sum = 0; for (double x : v) sum += x;
        d.mean = sum / static_cast<double>(v.size());
        return d;
    }

    void AppendDist(std::string& s, const char* key, const Dist& d)
    {
        AppendJsonString(s, key);
        if (d.n == 0) { s += ":null"; return; }
        s += ":{\"p50\":"; AppendNum(s, d.p50);
        s += ",\"p95\":";  AppendNum(s, d.p95);
        s += ",\"p99\":";  AppendNum(s, d.p99);
        s += ",\"max\":";  AppendNum(s, d.max);
        s += ",\"mean\":"; AppendNum(s, d.mean);
        Appendf(s, ",\"n\":%zu}", d.n);
    }

    // ==== Segmentation ====
    // What ONE thread was doing over [a, b): the category (and name) of the innermost open
    // scope, as disjoint segments. `evs` sorted by start asc, end desc (a parent before the
    // children it contains).
    struct Seg { int64_t lo, hi; uint8_t cat; NameId name; };

    void Segment(const std::vector<Event>& evs, int64_t a, int64_t b, std::vector<Seg>& out)
    {
        struct Open { int64_t end; uint8_t cat; NameId name; };
        Open stack[kMaxDepth + 8];
        int sp = 0;
        int64_t t = a;
        auto emit = [&](int64_t lo, int64_t hi, uint8_t cat, NameId name) {
            if (lo < a) lo = a;
            if (hi > b) hi = b;
            if (hi > lo) out.push_back({ lo, hi, cat, name });
        };
        for (const Event& e : evs)
        {
            if (e.Category() == Cat::Marker) continue;
            const int64_t s = e.startNs;
            int64_t en = e.EndNs();
            while (sp && stack[sp - 1].end <= s)
            {
                emit(t, stack[sp - 1].end, stack[sp - 1].cat, stack[sp - 1].name);
                if (stack[sp - 1].end > t) t = stack[sp - 1].end;
                --sp;
            }
            if (sp) emit(t, s, stack[sp - 1].cat, stack[sp - 1].name);
            else    emit(t, s, kSegUntracked, 0);
            if (s > t) t = s;
            // A child may overhang its parent by the 100 ns duration rounding;
            // never let it outlive the scope that contains it.
            if (sp && en > stack[sp - 1].end) en = stack[sp - 1].end;
            if (sp < static_cast<int>(sizeof(stack) / sizeof(stack[0])))
                stack[sp++] = { en, static_cast<uint8_t>(e.Category()), e.name };
        }
        while (sp)
        {
            emit(t, stack[sp - 1].end, stack[sp - 1].cat, stack[sp - 1].name);
            if (stack[sp - 1].end > t) t = stack[sp - 1].end;
            --sp;
        }
        emit(t, b, kSegUntracked, 0);
    }

    // ==== Per-thread buffers + windows ====
    struct ThreadBuf
    {
        uint64_t cursor = 0;
        std::vector<Event> events;   // END order (the ring's order)
        size_t   first = 0;          // pruned prefix
        uint64_t lost = 0;
    };

    struct NameStat { double selfMs = 0.0; uint64_t count = 0; };

    struct Window
    {
        struct PassStat
        {
            std::vector<double> gpuMs, cpuMs;
            uint32_t frames = 0;
            GpuCounters counted;                  // summed over the counted frames
            uint32_t countedFrames = 0, nativeFrames = 0;
        };

        struct BubbleStat { double ms = 0.0; uint32_t count = 0; };

        struct Delta { uint8_t role; NameId name; double ms, medianMs; };

        CaptureRequest req;
        uint8_t  kind = 0;            // 0 -profile_window, 1 named / on demand, 2 hitch
        uint64_t last = 0;
        bool     started = false;
        bool     finished = false;
        bool     truncated = false;
        int64_t  beginNs = 0;
        int64_t  endNs = 0;
        uint64_t lost = 0;
        uint64_t framesTimeline = 0;
        std::vector<FrameRecord> records;
        std::vector<Event> events[kMaxThreads];
        std::unordered_map<uint32_t, NameStat> names;   // role << 16 | name
        // GPU (ProfilerGpu.h)
        std::vector<GpuSpan> gpuSpans;                  // every span of the window's frames
        std::unordered_map<uint32_t, PassStat> passes;  // queue << 16 | name
        std::unordered_map<uint32_t, BubbleStat> bubbles; // before << 16 | after
        uint64_t framesGpu = 0, framesGpuDerived = 0;
        // kind == 2: the slow frame and why.
        uint64_t hitchFrame = 0;
        float    hitchPeriodMs = 0.f, hitchMedianMs = 0.f;
        uint32_t hitchBaseFrames = 0;
        std::vector<Delta> hitchDeltas;

        std::string traceFile;
        uint32_t traceFrames = 0;
        uint64_t traceEvents = 0;
    };

    void AppendRecordJson(std::string& s, const FrameRecord& r);

    std::atomic<bool> g_parity{ false };

    class Collector
    {
    public:
        struct Request { int kind; std::string name; uint64_t frame; uint32_t count; };  // 0 capture, 1 begin, 2 end

        void Start(const CollectorConfig& cfg);
        void Stop();
        bool Running() const noexcept { return m_running; }
        void OnFrameCompleted(uint64_t frame) noexcept;

        // Window requests from any thread; applied by the collector thread
        // before it processes the next frame.
        bool Enqueue(Request r);

        void Status(CollectorStatus& out) const noexcept;
        void NoteExternal(uint64_t frame, NameId name, float ms);
        std::string RunJson() const;
        void SetLive(bool on) noexcept { m_liveOn.store(on, std::memory_order_relaxed); }
        bool CopyLive(uint32_t frames, LiveTimeline& out);
        std::vector<CaptureInfo> Captures();
        std::string OutDir() const { return m_outDir; }

    private:
        struct Bubble { NameId before, after; double ms; };
        // One processed frame's share of a capture window, built once per frame
        // (while a window wants it or the hitch recorder is on) and added to
        // every live window; the hitch recorder keeps the last kHistoryFrames.
        struct FrameContrib
        {
            uint64_t frame = ~0ull;
            int64_t  a = 0, b = 0;
            bool     timeline = false;
            bool     haveGpu = false;
            std::vector<Event> ev[kMaxThreads];                  // events ENDING in (a, b]
            std::vector<std::pair<uint32_t, NameStat>> names;    // role << 16 | name
            std::vector<GpuSpan> spans;
            std::vector<float>   spanCpuMs;                      // per span, 0 = no CPU scope
            std::vector<Bubble>  bubbles;
            // Hitch explanation input: self ms per role << 16 | name, worker
            // idle left out, no scope open = kUntrackedName. Sorted by key.
            std::vector<std::pair<uint32_t, float>> self;
        };

        struct ExtTiming { uint64_t frame; NameId name; float ms; };

        void Run();
        void Drain();
        void Process(uint64_t frame);
        bool WindowsWant(uint64_t frame) const;
        void BuildContrib(uint64_t frame, const std::vector<Seg>* segs, int64_t a, int64_t b,
                          FrameContrib& c);
        void AddContrib(Window& w, const FrameRecord& r, const FrameContrib& c);
        void ApplyRequests();
        std::string UniqueName(std::string name) const;
        void DetectHitch(uint64_t frame, const FrameRecord& r);
        void FeedWindows(uint64_t frame, const FrameRecord& r, const FrameContrib* c,
                         int64_t a, int64_t b);
        void FinishWindow(Window& w);
        void WriteTrace(Window& w);
        void WriteSummary();
        void ConsumeParity(uint64_t frame, bool timeline, int64_t a, int64_t b);
        void ProcessGpu(uint64_t frame, FrameRecord& r, int64_t a);

        CollectorConfig m_cfg;
        std::string     m_outDir;
        std::thread     m_thread;
        std::mutex      m_mutex;
        std::condition_variable m_cv;
        std::vector<uint64_t> m_queue;
        bool     m_stop = false;
        std::atomic<bool> m_running{ false };   // read by the control channel's pipe thread
        uint64_t m_latest = ~0ull;      // newest completed frame seen
        // Completed frames not processed yet, in order. Only frames the main
        // thread actually CLOSED arrive here - a stretch with the level off
        // produces none, so there is nothing to skip over.
        std::deque<uint64_t> m_pending;
        uint64_t m_processed = 0, m_timelineFrames = 0, m_skipped = 0;

        ThreadBuf m_buf[kMaxThreads];
        std::vector<Event> m_scratch;
        std::vector<Event> m_tmp;
        std::vector<Seg>   m_segs[kMaxThreads];
        std::vector<Window> m_windows;
        std::ofstream m_cpuOut;
        bool m_cpuOpen = false;
        std::ofstream m_passOut;
        bool m_passOpen = false;
        // GPU frames being analysed (~24 KB each: members, not locals).
        GpuFrameData m_g0, m_g1;
        bool m_haveG0 = false;
        // CPU record time per pass name for the frame being processed.
        std::vector<float>  m_cpuByName;
        std::vector<NameId> m_cpuTouched;
        std::vector<Bubble> m_bubbles;
        // Window requests (any thread -> collector).
        std::mutex m_reqMutex;
        std::vector<Request> m_requests;
        // This frame's contribution when the hitch recorder is off.
        FrameContrib m_cur;
        std::unordered_map<uint32_t, NameStat> m_nameAcc;
        std::unordered_map<uint32_t, float>    m_selfAcc;
        // Hitch recorder: ring of the last kHistoryFrames contributions (empty
        // = off), recent periods for the median a hitch is judged against.
        std::vector<FrameContrib> m_history;
        std::deque<float> m_recentPeriods;
        uint32_t m_hitches = 0, m_hitchesCovered = 0, m_hitchesOverCap = 0;
        uint64_t m_hitchCoveredUntil = 0;
        bool     m_hitchCovering = false;
        bool     m_started = false;                  // Start ran with a level on (run.json block)
        // Status for other threads (GetCollectorStatus).
        std::atomic<uint64_t> m_stProcessed{ 0 }, m_stTimeline{ 0 }, m_stLatest{ ~0ull }, m_stLost{ 0 };
        std::atomic<uint32_t> m_stWinRequested{ 0 }, m_stWinFinished{ 0 }, m_stHitches{ 0 };
        // Run-wide headline (every processed frame) for run.json.
        std::vector<double> m_runPeriod, m_runUtil, m_runSMain, m_runSLane, m_runWTotal, m_runAllocs;
        std::vector<double> m_runGpuBusy, m_runGpuUtil;
        uint64_t m_runBound[5]{};
        uint64_t m_runMemFrames = 0;
        double   m_runVramPeakMB = 0.0, m_runWsPeakMB = 0.0;
        double   m_runLedgerEndMB = 0.0, m_runLedgerPeakMB = 0.0, m_runUnattributedEndMB = 0.0;
        bool     m_runUnattributedValid = false;
        uint64_t m_runLost = 0, m_runSuspend = 0, m_runOrphaned = 0, m_runCross = 0;

        // CPU parity (-profile_parity): external readings in, per-frame sums out.
        std::mutex m_extMutex;
        std::vector<ExtTiming> m_ext, m_extWork;
        std::unordered_map<NameId, double>  m_extSum;
        std::unordered_map<NameId, int64_t> m_profSum;
        uint64_t m_parFrames = 0, m_parPairs = 0, m_parUnmatched = 0;
        std::vector<double> m_parDiffUs;
        double   m_parWorstUs = 0.0;
        NameId   m_parWorstName = 0;
        uint64_t m_parWorstFrame = 0;
        std::vector<NameId> m_parUnmatchedNames;
        // The editor's live timeline: the history ring is kept while this is on; its slots are
        // written under m_historyMutex.
        std::atomic<bool> m_liveOn{ false };
        std::mutex m_historyMutex;
        // Finished windows, for ListCaptures.
        std::mutex m_capturesMutex;
        std::vector<CaptureInfo> m_captures;
        std::unordered_map<NameId, uint32_t> m_parBad;   // pairs over kParityBadUs, per name
        // Run-wide overhead samples (every processed record), for the
        // [PROFILER] overhead line at Stop.
        std::vector<double> m_boundaryUs, m_mainScopes, m_totalScopes;
        std::string m_line;
    };

    Collector g_collector;

    void FrameCompletedThunk(uint64_t frame) noexcept { g_collector.OnFrameCompleted(frame); }

    // ==== Collector ====
    void Collector::Start(const CollectorConfig& cfg)
    {
        m_cfg = cfg;
        SetOsSampling(cfg.osSampling);
        SetAllocCounting(cfg.allocCounting);   // before SetLevel: it folds both into one flag
        SetLevel(cfg.level);
        if (cfg.level == Level::Off || m_running) return;

        m_outDir = cfg.outDir.empty() ? ProfileOutputDir() : cfg.outDir;
        if (!m_outDir.empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(Sys::U8Path(m_outDir), ec);
            if (cfg.streams)
            {
                m_cpuOut = Sys::OpenOfstream(m_outDir + "/records.jsonl", std::ios::out | std::ios::trunc);
                m_cpuOpen = m_cpuOut.is_open();
                if (!m_cpuOpen)
                    LOG_ERROR("[PROFILER] cannot open '%s/records.jsonl'; per-frame records will not be written",
                              m_outDir.c_str());
            }
            if (cfg.gpu && cfg.streams)
            {
                m_passOut = Sys::OpenOfstream(m_outDir + "/passes.jsonl", std::ios::out | std::ios::trunc);
                m_passOpen = m_passOut.is_open();
            }
        }
        for (const CaptureRequest& req : cfg.windows)
        {
            Window w;
            w.req  = req;
            w.last = req.first + (req.count ? req.count - 1 : 0);
            m_windows.push_back(std::move(w));
        }
        if (cfg.hitchMs > 0.f) m_history.resize(kHistoryFrames);
        m_stWinRequested.store(static_cast<uint32_t>(cfg.windows.size()), std::memory_order_relaxed);
        g_parity.store(cfg.parity, std::memory_order_relaxed);
        m_started = true;
        m_scratch.resize(16384);
        // The per-scope cost, measured on THIS thread (main) at the run's own level, so the summary
        // can state the profiler's overhead as scopes x cost instead of asserting it.
        const double scopeNs = CalibrateScopeCost(20000);
        LOG_INFO("[PROFILER] calibration: scopeCostNs=%.1f level=%s", scopeNs, LevelName(cfg.level));
        m_stop = false;
        m_running = true;
        m_thread = std::thread([this] { Run(); });
        SetFrameCompletedCallback(&FrameCompletedThunk);
        // New fields go AFTER windows=: packs match the line's prefix literally.
        LOG_INFO("[PROFILER] level=%s os=%d gpu=%d alloc=%d windows=%zu hitchMs=%.1f out='%s'",
                 LevelName(cfg.level), cfg.osSampling ? 1 : 0, cfg.gpu ? 1 : 0,
                 cfg.allocCounting ? 1 : 0, cfg.windows.size(), static_cast<double>(cfg.hitchMs),
                 m_outDir.empty() ? "(none: no -artifacts=)" : m_outDir.c_str());
    }

    void Collector::Stop()
    {
        if (!m_running) return;
        SetFrameCompletedCallback(nullptr);
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_stop = true;
        }
        m_cv.notify_all();
        if (m_thread.joinable()) m_thread.join();
        m_running = false;
        if (m_cpuOpen) { m_cpuOut.flush(); m_cpuOut.close(); m_cpuOpen = false; }
        if (m_passOpen) { m_passOut.flush(); m_passOut.close(); m_passOpen = false; }
        {
            // The profiler's own cost over every frame it recorded - the number
            // the overhead packs gate precisely (an on/off frame-time A/B on a
            // loaded machine only resolves a blow-up).
            const Dist bu = Distribution(m_boundaryUs);
            const Dist ms = Distribution(m_mainScopes);
            const Dist ts = Distribution(m_totalScopes);
            const double cost = ScopeCostNs();
            LOG_INFO("[PROFILER] overhead: frames=%zu scopeCostNs=%.1f boundaryUsP50=%.2f boundaryUsP99=%.2f "
                     "mainScopesP50=%.0f totalScopesP50=%.0f mainEstMsP50=%.4f allThreadsEstMsCoreP50=%.4f",
                     bu.n, cost, bu.p50, bu.p99, ms.p50, ts.p50,
                     ms.p50 * cost * 1e-6 + bu.p50 * 1e-3,
                     ts.p50 * cost * 1e-6 + bu.p50 * 1e-3);
        }
        g_parity.store(false, std::memory_order_relaxed);
        if (m_cfg.parity)
        {
            const Dist pd = Distribution(m_parDiffUs);
            std::string unmatched;
            for (NameId nm : m_parUnmatchedNames) { if (!unmatched.empty()) unmatched += ','; unmatched += NameOf(nm); }
            std::vector<std::pair<NameId, uint32_t>> bad(m_parBad.begin(), m_parBad.end());
            std::sort(bad.begin(), bad.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
            uint64_t badTotal = 0;
            std::string badNames;
            for (size_t i = 0; i < bad.size(); ++i)
            {
                badTotal += bad[i].second;
                if (i >= 5) continue;
                if (!badNames.empty()) badNames += ',';
                badNames += NameOf(bad[i].first);
                badNames += ':' + std::to_string(bad[i].second);
            }
            LOG_INFO("[PROFILER] parity cpu: frames=%llu pairs=%llu unmatched=%llu bad=%llu diffUsP50=%.3f "
                     "diffUsP99=%.3f diffUsMax=%.3f worst='%s' worstFrame=%llu badNames='%s' unmatchedNames='%s'",
                     static_cast<unsigned long long>(m_parFrames), static_cast<unsigned long long>(m_parPairs),
                     static_cast<unsigned long long>(m_parUnmatched), static_cast<unsigned long long>(badTotal),
                     pd.p50, pd.p99, pd.max,
                     m_parWorstName ? NameOf(m_parWorstName) : "", static_cast<unsigned long long>(m_parWorstFrame),
                     badNames.c_str(), unmatched.c_str());
        }
        if (m_cfg.hitchMs > 0.f)
            LOG_INFO("[PROFILER] hitches: captured=%u covered=%u overCap=%u thresholdMs=%.1f max=%u fromFrame=%llu",
                     m_hitches, m_hitchesCovered, m_hitchesOverCap, static_cast<double>(m_cfg.hitchMs),
                     m_cfg.hitchMax, static_cast<unsigned long long>(m_cfg.hitchFrom));
        LOG_INFO("[PROFILER] stopped: processed=%llu timeline=%llu skipped=%llu crossThread=%llu "
                 "unregisteredThreads=%llu names=%u",
                 static_cast<unsigned long long>(m_processed),
                 static_cast<unsigned long long>(m_timelineFrames),
                 static_cast<unsigned long long>(m_skipped),
                 static_cast<unsigned long long>(CrossThreadDrops()),
                 static_cast<unsigned long long>(UnregisteredThreads()),
                 NameCount());
    }

    void Collector::OnFrameCompleted(uint64_t frame) noexcept
    {
        // Main thread, inside BeginFrame: one lock and a notify, nothing else.
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_queue.push_back(frame);
        }
        m_cv.notify_one();
    }

    void Collector::Run()
    {
        RegisterThread("Profiler.Collector", Role::Other);
        std::vector<uint64_t> batch;
        for (;;)
        {
            bool stop = false;
            {
                PROF_WAIT(WaitIdle, "Profiler.Collector.Wait");
                std::unique_lock<std::mutex> lk(m_mutex);
                m_cv.wait(lk, [&] { return m_stop || !m_queue.empty(); });
                batch.swap(m_queue);
                stop = m_stop;
            }
            // OS CPU first, as close to the boundary that woke us as possible
            // (the sample is attributed to the NEWEST completed frame).
            if (!batch.empty()) SampleOs(batch.back());
            for (uint64_t f : batch)
            {
                m_pending.push_back(f);
                if (m_latest == ~0ull || f > m_latest) m_latest = f;
            }
            batch.clear();
            if (m_latest == ~0ull) { if (stop) break; continue; }

            // The collector's own cost, visible in its per-thread row instead
            // of hiding as "untracked" while it competes with the workers.
            PROF_SCOPE("Profiler.Collector.Process");
            ApplyRequests();
            Drain();
            // Timeline analysis runs ONE FRAME LATE: a task that began in frame F
            // and ended in F+1 is in the ring only after F+1 completes.
            const bool lagged = GetLevel() >= Level::Timeline;
            // GPU numbers of f AND f+1 come back ~3 frames late; wait for them
            // while GPU timing is live, but never longer than kGpuWaitFrames.
            const uint64_t gpuLatest = LatestGpuFrame();
            const bool gpuLive = m_cfg.gpu && gpuLatest != ~0ull;
            while (!m_pending.empty())
            {
                const uint64_t f = m_pending.front();
                if (!stop && lagged && f >= m_latest) break;   // wait for a later frame
                if (!stop && gpuLive && gpuLatest < f + 1 && m_latest < f + kGpuWaitFrames) break;
                m_pending.pop_front();
                Process(f);
            }
            if (stop) break;
        }
        ApplyRequests();
        for (Window& w : m_windows)
        {
            if (w.last == kOpenEnded)
            {
                // EndWindow never came: the window ends where the run did.
                w.last = w.records.empty() ? w.req.first : w.records.back().frame;
                w.req.count = static_cast<uint32_t>(w.last - w.req.first + 1);
            }
            if (w.started && !w.finished) { w.truncated = true; FinishWindow(w); }
            else if (!w.started)
                LOG_WARNING("[PROFILER] window '%s' (first=%llu) never saw a frame - not in the summary",
                            w.req.name.c_str(), static_cast<unsigned long long>(w.req.first));
        }
        WriteSummary();
    }

    void Collector::Status(CollectorStatus& out) const noexcept
    {
        out.running          = m_running.load();
        out.level            = GetLevel();
        out.processed        = m_stProcessed.load(std::memory_order_relaxed);
        out.timelineFrames   = m_stTimeline.load(std::memory_order_relaxed);
        out.latestFrame      = m_stLatest.load(std::memory_order_relaxed);
        out.windowsRequested = m_stWinRequested.load(std::memory_order_relaxed);
        out.windowsFinished  = m_stWinFinished.load(std::memory_order_relaxed);
        out.hitches          = m_stHitches.load(std::memory_order_relaxed);
        out.eventsLost       = m_stLost.load(std::memory_order_relaxed);
    }

    void Collector::NoteExternal(uint64_t frame, NameId name, float ms)
    {
        std::lock_guard<std::mutex> lk(m_extMutex);
        m_ext.push_back({ frame, name, ms });
    }

    // CPU parity: per frame, per name, the external timer's sum against the
    // profiler's (every event of that name ENDING in the frame's window). Both
    // bracket the same call, so the difference is the two clock reads plus the
    // 100 ns duration quantum - a larger one is a lost, misattributed or
    // misnamed scope.
    void Collector::ConsumeParity(uint64_t frame, bool timeline, int64_t a, int64_t b)
    {
        {
            std::lock_guard<std::mutex> lk(m_extMutex);
            // This frame's readings and any older ones (a frame that was not
            // processed) leave the queue; later frames' stay.
            auto it = std::partition(m_ext.begin(), m_ext.end(),
                                     [&](const ExtTiming& e) { return e.frame > frame; });
            m_extWork.assign(it, m_ext.end());
            m_ext.erase(it, m_ext.end());
        }
        if (!timeline) return;
        m_extSum.clear();
        for (const ExtTiming& e : m_extWork)
            if (e.frame == frame) m_extSum[e.name] += e.ms;
        if (m_extSum.empty()) return;
        ++m_parFrames;
        m_profSum.clear();
        const uint32_t n = ThreadCount();
        for (uint32_t i = 0; i < n; ++i)
        {
            const ThreadBuf& tb = m_buf[i];
            for (size_t e = tb.first; e < tb.events.size(); ++e)
            {
                const Event& ev = tb.events[e];
                const int64_t end = ev.EndNs();
                if (!(end > a && end <= b) || ev.Category() == Cat::Marker) continue;
                if (m_extSum.find(ev.name) == m_extSum.end()) continue;
                m_profSum[ev.name] += static_cast<int64_t>(ev.dur100ns) * 100;
            }
        }
        for (const auto& kv : m_extSum)
        {
            auto p = m_profSum.find(kv.first);
            if (p == m_profSum.end())
            {
                ++m_parUnmatched;
                if (m_parUnmatchedNames.size() < 8 &&
                    std::find(m_parUnmatchedNames.begin(), m_parUnmatchedNames.end(), kv.first) == m_parUnmatchedNames.end())
                    m_parUnmatchedNames.push_back(kv.first);
                continue;
            }
            ++m_parPairs;
            const double diffUs = std::fabs(static_cast<double>(p->second) * 1e-3 - kv.second * 1e3);
            m_parDiffUs.push_back(diffUs);
            if (diffUs > kParityBadUs) ++m_parBad[kv.first];
            if (diffUs > m_parWorstUs) { m_parWorstUs = diffUs; m_parWorstName = kv.first; m_parWorstFrame = frame; }
        }
    }

    std::string Collector::RunJson() const
    {
        if (!m_started) return "null";
        std::string s;
        s.reserve(2048);
        const bool files = !m_outDir.empty();
        Appendf(s, "{\"level\":\"%s\",\"summary\":%s,\"records\":%s,\"framesProcessed\":%llu,\"framesTimeline\":%llu,",
                LevelName(m_cfg.level),
                files ? "\"profile/profile_summary.json\"" : "null",
                files && m_cfg.streams ? "\"profile/records.jsonl\"" : "null",
                static_cast<unsigned long long>(m_processed), static_cast<unsigned long long>(m_timelineFrames));
        uint32_t complete = 0;
        for (const Window& w : m_windows) if (w.finished && !w.truncated) ++complete;
        const bool valid = m_runLost == 0 && m_runSuspend == 0 && m_runOrphaned == 0 && m_runCross == 0;
        Appendf(s, "\"windows\":%zu,\"windowsComplete\":%u,\"hitches\":%u,\"valid\":%s,"
                   "\"validity\":{\"eventsLost\":%llu,\"droppedSuspend\":%llu,\"orphaned\":%llu,\"crossThread\":%llu,"
                   "\"unregisteredThreads\":%llu},",
                m_windows.size(), complete, m_hitches, valid ? "true" : "false",
                static_cast<unsigned long long>(m_runLost), static_cast<unsigned long long>(m_runSuspend),
                static_cast<unsigned long long>(m_runOrphaned), static_cast<unsigned long long>(m_runCross),
                static_cast<unsigned long long>(UnregisteredThreads()));
        const Dist per = Distribution(m_runPeriod);
        s += "\"headline\":{\"frameMsP50\":"; AppendNum(s, per.p50, 3);
        s += ",\"frameMsP95\":";   AppendNum(s, per.p95, 3);
        s += ",\"frameMsP99\":";   AppendNum(s, per.p99, 3);
        s += ",\"utilP50\":";      AppendNum(s, Distribution(m_runUtil).p50, 4);
        s += ",\"sMainMsP50\":";   AppendNum(s, Distribution(m_runSMain).p50, 3);
        s += ",\"sLaneMsP50\":";   AppendNum(s, Distribution(m_runSLane).p50, 3);
        s += ",\"wTotalMsP50\":";  AppendNum(s, Distribution(m_runWTotal).p50, 3);
        Appendf(s, ",\"gpuFrames\":%zu,\"gpuBusyMsP50\":", m_runGpuBusy.size());
        if (m_runGpuBusy.empty()) s += "null"; else AppendNum(s, Distribution(m_runGpuBusy).p50, 3);
        s += ",\"gpuUtilP50\":";
        if (m_runGpuUtil.empty()) s += "null"; else AppendNum(s, Distribution(m_runGpuUtil).p50, 4);
        // Share of the frames that have a derived GPU verdict.
        uint64_t judged = 0;
        for (uint64_t c : m_runBound) judged += c;
        static const char* const kBound[5] = { "unknown", "gpu", "cpu", "pacing", "mixed" };
        s += ",\"bound\":{";
        int majority = 0;
        for (int k = 0; k < 5; ++k)
        {
            if (k) s.push_back(',');
            Appendf(s, "\"%s\":", kBound[k]);
            AppendNum(s, judged ? static_cast<double>(m_runBound[k]) / static_cast<double>(judged) : 0.0, 4);
            if (m_runBound[k] > m_runBound[majority]) majority = k;
        }
        Appendf(s, "},\"boundFrames\":%llu,\"boundMajority\":\"%s\"", static_cast<unsigned long long>(judged),
                judged ? kBound[majority] : "unknown");
        s += ",\"allocsPerFrameP50\":"; AppendNum(s, Distribution(m_runAllocs).p50, 1);
        Appendf(s, ",\"memFrames\":%llu,\"vramPeakMB\":", static_cast<unsigned long long>(m_runMemFrames));
        if (m_runMemFrames) AppendNum(s, m_runVramPeakMB, 1); else s += "null";
        s += ",\"workingSetPeakMB\":";
        if (m_runMemFrames) AppendNum(s, m_runWsPeakMB, 1); else s += "null";
        s += ",\"gpuLedgerEndMB\":";   AppendNum(s, m_runLedgerEndMB, 1);
        s += ",\"gpuLedgerPeakMB\":";  AppendNum(s, m_runLedgerPeakMB, 1);
        s += ",\"unattributedLocalEndMB\":";
        if (m_runUnattributedValid) AppendNum(s, m_runUnattributedEndMB, 1); else s += "null";
        s += "},";
        const Dist bu = Distribution(m_boundaryUs);
        const Dist ms = Distribution(m_mainScopes);
        s += "\"overhead\":{\"scopeCostNs\":"; AppendNum(s, ScopeCostNs(), 1);
        s += ",\"boundaryUsP50\":"; AppendNum(s, bu.p50, 2);
        s += ",\"mainEstMsP50\":";  AppendNum(s, ms.p50 * ScopeCostNs() * 1e-6 + bu.p50 * 1e-3, 4);
        s += "},\"parity\":";
        if (!m_cfg.parity) s += "null";
        else
        {
            const Dist pd = Distribution(m_parDiffUs);
            Appendf(s, "{\"frames\":%llu,\"pairs\":%llu,\"unmatched\":%llu,\"diffUsP50\":",
                    static_cast<unsigned long long>(m_parFrames), static_cast<unsigned long long>(m_parPairs),
                    static_cast<unsigned long long>(m_parUnmatched));
            AppendNum(s, pd.p50, 3);
            s += ",\"diffUsP99\":"; AppendNum(s, pd.p99, 3);
            s += ",\"diffUsMax\":"; AppendNum(s, pd.max, 3);
            s += "}";
        }
        s += "}";
        return s;
    }

    std::vector<CaptureInfo> Collector::Captures()
    {
        std::lock_guard<std::mutex> lk(m_capturesMutex);
        return m_captures;
    }

    bool Collector::CopyLive(uint32_t frames, LiveTimeline& out)
    {
        out.frames.clear();
        out.frameBegin.clear();
        out.threads.clear();
        out.gpu.clear();
        std::lock_guard<std::mutex> lk(m_historyMutex);
        if (m_history.empty()) return false;
        // The newest `frames` timeline slots, oldest first.
        std::vector<const FrameContrib*> slots;
        for (const FrameContrib& c : m_history)
            if (c.frame != ~0ull && c.timeline) slots.push_back(&c);
        if (slots.empty()) return false;
        std::sort(slots.begin(), slots.end(), [](const FrameContrib* a, const FrameContrib* b) { return a->frame < b->frame; });
        if (frames && slots.size() > frames) slots.erase(slots.begin(), slots.end() - frames);
        // Only a contiguous run of frames makes one timeline.
        size_t start = slots.size() - 1;
        while (start > 0 && slots[start - 1]->frame + 1 == slots[start]->frame) --start;
        slots.erase(slots.begin(), slots.begin() + static_cast<ptrdiff_t>(start));

        const uint32_t n = ThreadCount();
        std::vector<int> slotOf(kMaxThreads, -1);
        for (const FrameContrib* c : slots)
        {
            out.frames.push_back(c->frame);
            out.frameBegin.push_back(c->a);
            out.gpu.insert(out.gpu.end(), c->spans.begin(), c->spans.end());
            for (uint32_t i = 0; i < n && i < static_cast<uint32_t>(kMaxThreads); ++i)
            {
                if (c->ev[i].empty()) continue;
                if (slotOf[i] < 0)
                {
                    LiveThread t;
                    t.index = static_cast<uint16_t>(i);
                    ThreadCounters tc;
                    if (ReadThreadCounters(i, tc))
                    {
                        t.role = tc.role;
                        std::snprintf(t.name, sizeof(t.name), "%s", tc.name);
                    }
                    slotOf[i] = static_cast<int>(out.threads.size());
                    out.threads.push_back(std::move(t));
                }
                auto& ev = out.threads[static_cast<size_t>(slotOf[i])].events;
                ev.insert(ev.end(), c->ev[i].begin(), c->ev[i].end());
            }
        }
        out.frameBegin.push_back(slots.back()->b);
        return true;
    }

    bool Collector::Enqueue(Request r)
    {
        if (!m_running) return false;
        std::lock_guard<std::mutex> lk(m_reqMutex);
        m_requests.push_back(std::move(r));
        return true;
    }

    std::string SanitizeWindowName(std::string name)
    {
        for (char& c : name)
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) c = '_';
        if (name.empty()) name = "w";
        return name;
    }

    std::string Collector::UniqueName(std::string name) const
    {
        name = SanitizeWindowName(std::move(name));
        std::string candidate = name;
        for (int k = 2; ; ++k)
        {
            bool taken = false;
            for (const Window& w : m_windows) if (w.req.name == candidate) { taken = true; break; }
            if (!taken) return candidate;
            candidate = name + "_" + std::to_string(k);
        }
    }

    // Requests -> windows. A request for a frame the collector has already
    // processed starts at the first frame it can still see (it cannot, in
    // practice: a request made in frame F is applied before F is processed).
    void Collector::ApplyRequests()
    {
        std::vector<Request> reqs;
        {
            std::lock_guard<std::mutex> lk(m_reqMutex);
            reqs.swap(m_requests);
        }
        if (reqs.empty()) return;
        const uint64_t earliest = !m_pending.empty() ? m_pending.front()
                                : (m_latest == ~0ull ? 0 : m_latest + 1);
        for (Request& q : reqs)
        {
            if (q.kind == 3)
            {
                // SetHitchCapture: frame = ms x 1000, count = max. A fresh budget.
                m_cfg.hitchMs   = static_cast<float>(static_cast<double>(q.frame) / 1000.0);
                m_cfg.hitchMax  = q.count ? q.count : 1;
                m_cfg.hitchFrom = 0;
                m_hitches = 0;
                m_hitchCovering = false;
                LOG_INFO("[PROFILER] hitch capture set: thresholdMs=%.1f max=%u", static_cast<double>(m_cfg.hitchMs),
                         m_cfg.hitchMax);
                continue;
            }
            if (q.kind == 2)
            {
                // The name Begin was given went through the same sanitising.
                q.name = SanitizeWindowName(std::move(q.name));
                Window* open = nullptr;
                for (Window& w : m_windows)
                    if (w.kind == 1 && w.last == kOpenEnded && w.req.name == q.name) { open = &w; break; }
                if (!open)
                {
                    LOG_WARNING("[PROFILER] EndWindow('%s'): no open window by that name", q.name.c_str());
                    continue;
                }
                open->last = q.frame < open->req.first ? open->req.first : q.frame;
                open->req.count = static_cast<uint32_t>(open->last - open->req.first + 1);
                LOG_INFO("[PROFILER] window '%s' ends: first=%llu last=%llu", open->req.name.c_str(),
                         static_cast<unsigned long long>(open->req.first),
                         static_cast<unsigned long long>(open->last));
                continue;
            }
            Window w;
            w.kind = 1;
            w.req.name  = UniqueName(q.name);
            w.req.first = q.frame < earliest ? earliest : q.frame;
            if (q.kind == 1) { w.last = kOpenEnded; w.req.count = 0; }
            else
            {
                w.req.count = q.count ? q.count : 1;
                w.last = w.req.first + w.req.count - 1;
            }
            m_stWinRequested.fetch_add(1, std::memory_order_relaxed);
            LOG_INFO("[PROFILER] window '%s' requested: first=%llu count=%u%s", w.req.name.c_str(),
                     static_cast<unsigned long long>(w.req.first), w.req.count,
                     q.kind == 1 ? " (open until EndWindow)" : "");
            m_windows.push_back(std::move(w));
        }
    }

    void Collector::Drain()
    {
        const uint32_t n = ThreadCount();
        for (uint32_t i = 0; i < n; ++i)
        {
            ThreadBuf& tb = m_buf[i];
            for (;;)
            {
                uint64_t copied = 0, lost = 0;
                const uint64_t next = ReadEvents(i, tb.cursor, m_scratch.data(),
                                                 m_scratch.size(), copied, lost);
                tb.lost += lost;
                if (copied)
                    tb.events.insert(tb.events.end(), m_scratch.begin(), m_scratch.begin() + static_cast<ptrdiff_t>(copied));
                const bool more = next != tb.cursor && next < RingHead(i);
                tb.cursor = next;
                if (!more) break;
            }
        }
    }

    void Collector::Process(uint64_t frame)
    {
        FrameRecord r;
        if (!GetFrameRecord(frame, r)) { ++m_skipped; return; }
        ++m_processed;
        m_boundaryUs.push_back(r.boundaryUs);
        m_mainScopes.push_back(r.mainScopes);
        m_totalScopes.push_back(r.totalScopes);
        const int64_t a = r.beginNs, b = r.endNs;
        const uint32_t n = ThreadCount();
        const bool timeline = r.level >= Level::Timeline;

        if (timeline)
        {
            TimelinePatch patch;
            uint64_t lostNow = 0;
            // Per-thread innermost segmentation of the window.
            for (uint32_t i = 0; i < n; ++i)
            {
                m_segs[i].clear();
                ThreadBuf& tb = m_buf[i];
                lostNow += tb.lost;
                tb.lost = 0;
                m_tmp.clear();
                for (size_t e = tb.first; e < tb.events.size(); ++e)
                {
                    const Event& ev = tb.events[e];
                    if (ev.EndNs() <= a && ev.Category() != Cat::Marker) continue;
                    if (ev.startNs >= b) continue;
                    m_tmp.push_back(ev);
                    ++patch.events;
                }
                std::sort(m_tmp.begin(), m_tmp.end(), [](const Event& x, const Event& y) {
                    if (x.startNs != y.startNs) return x.startNs < y.startNs;
                    return x.EndNs() > y.EndNs();
                });
                Segment(m_tmp, a, b, m_segs[i]);
            }
            patch.eventsLost = static_cast<uint32_t>(lostNow);

            // Concurrency: how many POOL threads are in Work at every instant,
            // and all-idle = main in Work while no WORKER is.
            struct Edge { int64_t t; int8_t d; uint8_t role; };
            std::vector<Edge> edges;
            edges.reserve(256);
            for (uint32_t i = 0; i < n; ++i)
            {
                ThreadCounters tc;
                if (!ReadThreadCounters(i, tc)) continue;
                if (tc.role != Role::Main && tc.role != Role::RenderOwner && tc.role != Role::Worker)
                    continue;
                int64_t lo = 0, hi = 0;
                bool open = false;
                for (const Seg& sg : m_segs[i])
                {
                    if (sg.cat != static_cast<uint8_t>(Cat::Work)) continue;
                    if (open && sg.lo <= hi) { if (sg.hi > hi) hi = sg.hi; continue; }
                    if (open)
                    {
                        edges.push_back({ lo, +1, static_cast<uint8_t>(tc.role) });
                        edges.push_back({ hi, -1, static_cast<uint8_t>(tc.role) });
                    }
                    lo = sg.lo; hi = sg.hi; open = true;
                }
                if (open)
                {
                    edges.push_back({ lo, +1, static_cast<uint8_t>(tc.role) });
                    edges.push_back({ hi, -1, static_cast<uint8_t>(tc.role) });
                }
            }
            std::sort(edges.begin(), edges.end(), [](const Edge& x, const Edge& y) {
                if (x.t != y.t) return x.t < y.t;
                return x.d < y.d;   // close before open at the same instant
            });
            int k = 0, kWorkers = 0, mainOn = 0;
            int64_t t = a;
            double conc[kFrameThreads + 1]{};
            double allIdle = 0.0;
            for (const Edge& e : edges)
            {
                if (e.t > t)
                {
                    const double dt = static_cast<double>(e.t - t) * 1e-6;
                    conc[(std::min)(k, kFrameThreads)] += dt;
                    if (mainOn > 0 && kWorkers == 0) allIdle += dt;
                    t = e.t;
                }
                k += e.d;
                if (e.role == static_cast<uint8_t>(Role::Worker)) kWorkers += e.d;
                if (e.role == static_cast<uint8_t>(Role::Main))   mainOn += e.d;
            }
            if (b > t) conc[0] += static_cast<double>(b - t) * 1e-6;
            for (int c = 0; c <= kFrameThreads; ++c) patch.concurrencyMs[c] = static_cast<float>(conc[c]);
            patch.allIdleMs = static_cast<float>(allIdle);
            if (PatchTimeline(frame, patch)) { ++m_timelineFrames; GetFrameRecord(frame, r); }
        }

        ProcessGpu(frame, r, a);

        // Status for other threads, and the run-wide headline for run.json.
        m_stProcessed.store(m_processed, std::memory_order_relaxed);
        m_stTimeline.store(m_timelineFrames, std::memory_order_relaxed);
        m_stLatest.store(frame, std::memory_order_relaxed);
        m_runLost += r.eventsLost; m_runSuspend += r.droppedSuspend;
        m_runOrphaned += r.orphaned; m_runCross += r.crossThread;
        m_stLost.store(m_runLost, std::memory_order_relaxed);
        m_runPeriod.push_back(r.periodMs);
        m_runUtil.push_back(r.util);
        m_runSMain.push_back(r.sMainMs);
        m_runSLane.push_back(r.sLaneMs);
        m_runWTotal.push_back(r.wTotalMs);
        m_runAllocs.push_back(r.allocs);
        if (r.gpuValid) m_runGpuBusy.push_back(r.gpuBusyMs);
        if (r.gpuDerived)
        {
            m_runGpuUtil.push_back(r.gpuUtil);
            const int bi = static_cast<int>(r.bound);
            if (bi >= 0 && bi < 5) ++m_runBound[bi];
        }
        {
            uint64_t total = 0, local = 0;
            for (int i = 0; i < kGpuMemCats; ++i)
            {
                total += r.gpuLedgerBytes[i];
                if (GpuMemCatIsLocal(static_cast<GpuMemCat>(i))) local += r.gpuLedgerBytes[i];
            }
            m_runLedgerEndMB  = static_cast<double>(total) / 1048576.0;
            m_runLedgerPeakMB = (std::max)(m_runLedgerPeakMB, m_runLedgerEndMB);
            if (r.memValid)
            {
                m_runUnattributedEndMB = (static_cast<double>(r.vramLocalBytes) - static_cast<double>(local)) / 1048576.0;
                m_runUnattributedValid = true;
            }
        }
        if (r.memValid)
        {
            ++m_runMemFrames;
            m_runVramPeakMB = (std::max)(m_runVramPeakMB, static_cast<double>(r.vramLocalBytes) / 1048576.0);
            m_runWsPeakMB   = (std::max)(m_runWsPeakMB, static_cast<double>(r.workingSetBytes) / 1048576.0);
        }
        if (m_cfg.parity) ConsumeParity(frame, timeline, a, b);

        if (m_cpuOpen)
        {
            m_line.clear();
            AppendRecordJson(m_line, r);
            m_line.push_back('\n');
            m_cpuOut.write(m_line.data(), static_cast<std::streamsize>(m_line.size()));
            m_cpuOut.flush();
        }

        // This frame's share of any capture window, built once - into the
        // hitch recorder's slot when that is on.
        FrameContrib* contrib = nullptr;
        if (m_history.empty() && (m_liveOn.load(std::memory_order_relaxed) || m_cfg.hitchMs > 0.f))
            m_history.resize(kHistoryFrames);
        if (!m_history.empty() || WindowsWant(frame))
        {
            if (m_history.empty())
            {
                contrib = &m_cur;
                BuildContrib(frame, timeline ? m_segs : nullptr, a, b, *contrib);
            }
            else
            {
                // A recorder slot: the editor may be copying it (CopyLive).
                std::lock_guard<std::mutex> lk(m_historyMutex);
                contrib = &m_history[frame % m_history.size()];
                BuildContrib(frame, timeline ? m_segs : nullptr, a, b, *contrib);
            }
        }
        FeedWindows(frame, r, contrib, a, b);
        if (m_cfg.hitchMs > 0.f && !m_history.empty() && contrib && contrib->timeline) DetectHitch(frame, r);

        // Events that ended at or before this window's end cannot overlap any
        // later window. Prune them (the buffers are in end order).
        for (uint32_t i = 0; i < n; ++i)
        {
            ThreadBuf& tb = m_buf[i];
            while (tb.first < tb.events.size() && tb.events[tb.first].EndNs() <= b) ++tb.first;
            if (tb.first > 4096 && tb.first * 2 > tb.events.size())
            {
                tb.events.erase(tb.events.begin(), tb.events.begin() + static_cast<ptrdiff_t>(tb.first));
                tb.first = 0;
            }
        }
    }

    // GPU half of one frame: derived numbers (period / util / starve / bound),
    // bubbles, and the per-pass table (GPU time joined with the CPU RECORD time
    // of the same pass, found by name among this frame's events - frame tags
    // carry the render frame onto the lane and its workers).
    void Collector::ProcessGpu(uint64_t frame, FrameRecord& r, int64_t a)
    {
        m_haveG0 = GetGpuFrame(frame, m_g0);
        m_bubbles.clear();
        if (!m_haveG0) return;
        const bool haveG1 = GetGpuFrame(frame + 1, m_g1);
        const double nan = std::nan("");

        GpuDerivedPatch gp;
        const double envelopeMs = static_cast<double>(m_g0.lastNs - m_g0.firstNs) * 1e-6;
        gp.bubbleMs = static_cast<float>((std::max)(0.0, envelopeMs - m_g0.busyMs));
        gp.submitToStartMs = m_g0.submitNs
            ? static_cast<float>(static_cast<double>(m_g0.firstNs - m_g0.submitNs) * 1e-6)
            : static_cast<float>(nan);
        gp.periodMs = gp.util = gp.starveMs = static_cast<float>(nan);
        if (haveG1)
        {
            const double period = static_cast<double>(m_g1.firstNs - m_g0.firstNs) * 1e-6;
            const double starve = (std::max)(0.0, static_cast<double>(m_g1.firstNs - m_g0.lastNs) * 1e-6);
            gp.periodMs   = static_cast<float>(period);
            gp.util       = period > 0.0 ? static_cast<float>(m_g0.busyMs / period) : static_cast<float>(nan);
            gp.starveMs   = static_cast<float>(starve);
            gp.submitLate = m_g1.submitNs != 0 && m_g1.submitNs > m_g0.lastNs;
            // What limited this frame. GPU: the next frame was already SUBMITTED before the GPU
            // finished this one (its queue never ran dry) and the GPU was busy for most of the period -
            // or main had to wait for it. PACING: the limiter slept. CPU: the GPU finished and sat idle
            // until the CPU submitted the next frame. Anything else: MIXED.
            // The queue test, not starveMs < epsilon, decides GPU: starve is measured between the
            // first/last INSTRUMENTED regions, so un-instrumented work at the frame edges reads as starve
            // on a GPU that never idled (util 0.95 with 0.39 ms "starve" and the CPU 2.3 ms ahead is a
            // GPU floor). When BOTH signals fire (main waited on the GPU early in the frame AND the GPU
            // then starved waiting for the next submission) the larger one decides: a 1.3 ms GPU wait
            // followed by 56 ms of GPU idle behind a CPU stall is CPU-bound.
            const bool saturated  = !gp.submitLate && gp.util >= 0.85f;
            const bool cpuStarved = gp.submitLate && starve >= 0.2 && r.mainWaitPacingMs < 1.0f;
            if (saturated)                                       gp.bound = Bound::Gpu;
            else if (cpuStarved && starve > r.mainWaitGpuMs)     gp.bound = Bound::Cpu;
            else if (r.mainWaitGpuMs > 0.5f)                     gp.bound = Bound::Gpu;
            else if (r.mainWaitPacingMs >= 1.0f)                 gp.bound = Bound::Pacing;
            else if (gp.submitLate && starve >= 0.2)             gp.bound = Bound::Cpu;
            else                                                 gp.bound = Bound::Mixed;
        }
        if (PatchGpuDerived(frame, gp)) GetFrameRecord(frame, r);

        // Bubbles: gaps between the union intervals of every queue's spans,
        // named by the span that closed the interval before and the one that
        // opened the interval after.
        {
            // spans are sorted by begin (GPUProfiler sorted them that way).
            int64_t curHi = 0; NameId curHiName = 0; bool open = false;
            for (uint32_t i = 0; i < m_g0.spanCount; ++i)
            {
                const GpuSpan& sp = m_g0.spans[i];
                if (!open) { curHi = sp.endNs; curHiName = sp.name; open = true; continue; }
                if (sp.beginNs <= curHi)
                {
                    if (sp.endNs > curHi) { curHi = sp.endNs; curHiName = sp.name; }
                    continue;
                }
                const double gap = static_cast<double>(sp.beginNs - curHi) * 1e-6;
                if (gap >= kBubbleMinMs) m_bubbles.push_back({ curHiName, sp.name, gap });
                curHi = sp.endNs; curHiName = sp.name;
            }
        }

        // CPU record time per pass name, from this frame's events (frame tag).
        if (m_cpuByName.size() < NameCount()) m_cpuByName.resize(NameCount() + 64, 0.f);
        for (NameId n : m_cpuTouched) m_cpuByName[n] = 0.f;
        m_cpuTouched.clear();
        if (r.level >= Level::Timeline)
        {
            const uint8_t tag = static_cast<uint8_t>(frame);
            const uint32_t n = ThreadCount();
            for (uint32_t i = 0; i < n; ++i)
            {
                const ThreadBuf& tb = m_buf[i];
                for (size_t e = tb.first; e < tb.events.size(); ++e)
                {
                    const Event& ev = tb.events[e];
                    if (ev.frameTag != tag || ev.EndNs() <= a) continue;
                    if (ev.Category() != Cat::Work || ev.name >= m_cpuByName.size()) continue;
                    if (m_cpuByName[ev.name] == 0.f) m_cpuTouched.push_back(ev.name);
                    m_cpuByName[ev.name] += static_cast<float>(ev.dur100ns) * 1e-4f;
                }
            }
        }

        if (m_passOpen)
        {
            m_line.clear();
            Appendf(m_line, "{\"frame\":%llu,\"busyMs\":", static_cast<unsigned long long>(frame));
            AppendNum(m_line, m_g0.busyMs, 4);
            m_line += ",\"periodMs\":"; AppendNum(m_line, gp.periodMs, 4);
            m_line += ",\"spans\":[";
            for (uint32_t i = 0; i < m_g0.spanCount; ++i)
            {
                const GpuSpan& sp = m_g0.spans[i];
                if (i) m_line.push_back(',');
                m_line += "{\"n\":"; AppendJsonString(m_line, NameOf(sp.name));
                Appendf(m_line, ",\"q\":%u,\"d\":%u,\"t\":", static_cast<unsigned>(sp.queue),
                        static_cast<unsigned>(sp.depth));
                AppendNum(m_line, static_cast<double>(sp.beginNs - m_g0.firstNs) * 1e-6, 4);
                m_line += ",\"ms\":";
                AppendNum(m_line, static_cast<double>(sp.endNs - sp.beginNs) * 1e-6, 4);
                m_line += ",\"cpu\":";
                const float cpu = sp.name < m_cpuByName.size() ? m_cpuByName[sp.name] : 0.f;
                if (cpu > 0.f) AppendNum(m_line, cpu, 4); else m_line += "null";
                // What the region's command lists recorded.
                if (sp.counters.valid)
                {
                    const GpuCounters& k = sp.counters;
                    Appendf(m_line, ",\"draws\":%u,\"indirect\":%u,\"dispatches\":%u,\"indirectDispatches\":%u,"
                                    "\"mesh\":%u,\"rays\":%u,\"pso\":%u,\"barriers\":%u", k.draws, k.drawsIndirect,
                            k.dispatches, k.dispatchesIndirect, k.dispatchesMesh, k.dispatchesRays, k.psoBinds,
                            k.barriers);
                    if (k.nativeTakes) m_line += ",\"native\":true";
                }
                m_line.push_back('}');
            }
            m_line += "]}\n";
            m_passOut.write(m_line.data(), static_cast<std::streamsize>(m_line.size()));
            m_passOut.flush();
        }
    }

    bool Collector::WindowsWant(uint64_t frame) const
    {
        for (const Window& w : m_windows)
            if (!w.finished && frame >= w.req.first && frame <= w.last) return true;
        return false;
    }

    void Collector::BuildContrib(uint64_t frame, const std::vector<Seg>* segs, int64_t a, int64_t b,
                                 FrameContrib& c)
    {
        c.frame = frame;
        c.a = a;
        c.b = b;
        c.timeline = segs != nullptr;
        c.haveGpu = m_haveG0;
        c.spans.clear();
        c.spanCpuMs.clear();
        c.bubbles.clear();
        c.names.clear();
        c.self.clear();
        for (auto& v : c.ev) v.clear();
        if (m_haveG0)
        {
            c.spans.assign(m_g0.spans, m_g0.spans + m_g0.spanCount);
            for (uint32_t i = 0; i < m_g0.spanCount; ++i)
            {
                const NameId nm = m_g0.spans[i].name;
                c.spanCpuMs.push_back(nm < m_cpuByName.size() ? m_cpuByName[nm] : 0.f);
            }
            c.bubbles = m_bubbles;
        }
        if (!segs) return;
        m_nameAcc.clear();
        m_selfAcc.clear();
        const uint32_t n = ThreadCount();
        for (uint32_t i = 0; i < n; ++i)
        {
            ThreadCounters tc;
            const bool haveTc = ReadThreadCounters(i, tc);
            const uint32_t role16 = haveTc ? static_cast<uint32_t>(tc.role) << 16 : 0;
            const ThreadBuf& tb = m_buf[i];
            for (size_t e = tb.first; e < tb.events.size(); ++e)
            {
                const Event& ev = tb.events[e];
                const int64_t end = ev.EndNs();
                // Each event belongs to exactly one frame: the one it ENDS
                // in. That already includes the first frame's lead-in (a
                // scope that began before the window and closed inside it).
                if (!(end > a && end <= b)) continue;
                c.ev[i].push_back(ev);
                if (haveTc && ev.Category() != Cat::Marker) m_nameAcc[role16 | ev.name].count++;
            }
            if (!haveTc) continue;
            for (const Seg& sg : segs[i])
            {
                const double ms = static_cast<double>(sg.hi - sg.lo) * 1e-6;
                if (sg.cat != kSegUntracked) m_nameAcc[role16 | sg.name].selfMs += ms;
                if (sg.cat == static_cast<uint8_t>(Cat::WaitIdle)) continue;
                m_selfAcc[role16 | (sg.cat == kSegUntracked ? kUntrackedName : sg.name)] +=
                    static_cast<float>(ms);
            }
        }
        c.names.assign(m_nameAcc.begin(), m_nameAcc.end());
        c.self.assign(m_selfAcc.begin(), m_selfAcc.end());
        std::sort(c.self.begin(), c.self.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    }

    void Collector::AddContrib(Window& w, const FrameRecord& r, const FrameContrib& c)
    {
        if (!w.started) { w.started = true; w.beginNs = c.a; }
        w.endNs = c.b;
        w.records.push_back(r);
        if (r.timelineValid) ++w.framesTimeline;
        w.lost += r.eventsLost;
        if (c.haveGpu)
        {
            ++w.framesGpu;
            if (r.gpuDerived) ++w.framesGpuDerived;
            w.gpuSpans.insert(w.gpuSpans.end(), c.spans.begin(), c.spans.end());
            for (size_t i = 0; i < c.spans.size(); ++i)
            {
                const GpuSpan& sp = c.spans[i];
                Window::PassStat& ps = w.passes[(static_cast<uint32_t>(sp.queue) << 16) | sp.name];
                ps.gpuMs.push_back(static_cast<double>(sp.endNs - sp.beginNs) * 1e-6);
                if (c.spanCpuMs[i] > 0.f) ps.cpuMs.push_back(c.spanCpuMs[i]);
                ++ps.frames;
                if (sp.counters.valid)
                {
                    ps.counted += sp.counters;
                    ++ps.countedFrames;
                    if (sp.counters.nativeTakes) ++ps.nativeFrames;
                }
            }
            for (const Bubble& bb : c.bubbles)
            {
                Window::BubbleStat& bs = w.bubbles[(static_cast<uint32_t>(bb.before) << 16) | bb.after];
                bs.ms += bb.ms;
                ++bs.count;
            }
        }
        if (!c.timeline) return;
        for (uint32_t i = 0; i < static_cast<uint32_t>(kMaxThreads); ++i)
            if (!c.ev[i].empty()) w.events[i].insert(w.events[i].end(), c.ev[i].begin(), c.ev[i].end());
        for (const auto& kv : c.names)
        {
            NameStat& ns = w.names[kv.first];
            ns.count  += kv.second.count;
            ns.selfMs += kv.second.selfMs;
        }
    }

    // A frame slower than the threshold: open a window around it from the recorder (kHitchPre
    // frames of lead-in are already there) and explain it.
    void Collector::DetectHitch(uint64_t frame, const FrameRecord& r)
    {
        // The baseline is the frames BEFORE this one.
        double median = 0.0;
        if (!m_recentPeriods.empty())
        {
            std::vector<float> v(m_recentPeriods.begin(), m_recentPeriods.end());
            std::nth_element(v.begin(), v.begin() + static_cast<ptrdiff_t>(v.size() / 2), v.end());
            median = v[v.size() / 2];
        }
        m_recentPeriods.push_back(r.periodMs);
        if (m_recentPeriods.size() > 60) m_recentPeriods.pop_front();

        if (frame < m_cfg.hitchFrom || r.periodMs < m_cfg.hitchMs) return;
        if (m_hitchCovering && frame <= m_hitchCoveredUntil)
        {
            ++m_hitchesCovered;
            LOG_INFO("[PROFILER] hitch frame=%llu periodMs=%.2f is inside the previous hitch window - not captured again",
                     static_cast<unsigned long long>(frame), static_cast<double>(r.periodMs));
            return;
        }
        if (m_hitches >= m_cfg.hitchMax) { ++m_hitchesOverCap; return; }
        ++m_hitches;

        const size_t ring = m_history.size();
        auto slot = [&](uint64_t x) -> const FrameContrib& { return m_history[x % ring]; };

        Window w;
        w.kind = 2;
        w.req.name = UniqueName("hitch_" + std::to_string(frame));
        w.hitchFrame = frame;
        w.hitchPeriodMs = r.periodMs;
        w.hitchMedianMs = static_cast<float>(median);
        uint64_t first = frame >= kHitchPre ? frame - kHitchPre : 0;
        while (first < frame && slot(first).frame != first) ++first;
        w.req.first = first;
        w.last      = frame + kHitchPost;
        w.req.count = static_cast<uint32_t>(w.last - first + 1);
        for (uint64_t x = first; x < frame; ++x)
        {
            FrameRecord rx;
            if (slot(x).frame != x || !GetFrameRecord(x, rx)) continue;
            AddContrib(w, rx, slot(x));
        }
        AddContrib(w, r, slot(frame));

        // Explanation: self time per (role, name) in the slow frame against its
        // median over the recorder's earlier frames (a frame where the scope
        // did not run counts as 0).
        std::unordered_map<uint32_t, std::vector<float>> base;
        uint32_t baseFrames = 0;
        const uint64_t lo = frame >= ring ? frame - ring + 1 : 0;
        for (uint64_t x = lo; x < frame; ++x)
        {
            const FrameContrib& c = slot(x);
            if (c.frame != x || !c.timeline) continue;
            for (const auto& kv : c.self) base[kv.first].push_back(kv.second);
            ++baseFrames;
        }
        for (const auto& kv : slot(frame).self)
        {
            double med = 0.0;
            auto it = base.find(kv.first);
            if (it != base.end() && baseFrames)
            {
                std::vector<float> v = it->second;
                v.resize(baseFrames, 0.f);
                std::nth_element(v.begin(), v.begin() + static_cast<ptrdiff_t>(v.size() / 2), v.end());
                med = v[v.size() / 2];
            }
            if (kv.second - med > 0.05)
                w.hitchDeltas.push_back({ static_cast<uint8_t>(kv.first >> 16),
                                          static_cast<NameId>(kv.first & 0xFFFFu), kv.second, med });
        }
        std::sort(w.hitchDeltas.begin(), w.hitchDeltas.end(), [](const Window::Delta& x, const Window::Delta& y) {
            return (x.ms - x.medianMs) > (y.ms - y.medianMs);
        });
        const Window::Delta* mainTop = nullptr;
        for (const Window::Delta& d : w.hitchDeltas)
            if (d.role == static_cast<uint8_t>(Role::Main)) { mainTop = &d; break; }
        const Window::Delta topCopy  = w.hitchDeltas.empty() ? Window::Delta{} : w.hitchDeltas.front();
        const Window::Delta mainCopy = mainTop ? *mainTop : Window::Delta{};
        if (w.hitchDeltas.size() > kHitchTopDeltas) w.hitchDeltas.resize(kHitchTopDeltas);
        w.hitchBaseFrames = baseFrames;

        LOG_INFO("[PROFILER] hitch frame=%llu periodMs=%.2f medianMs=%.2f baseFrames=%u "
                 "top='%s' topRole=%s topDeltaMs=%.2f mainTop='%s' mainTopDeltaMs=%.2f window='%s'",
                 static_cast<unsigned long long>(frame), static_cast<double>(r.periodMs), median, baseFrames,
                 w.hitchDeltas.empty() ? "" : KeyName(topCopy.name),
                 w.hitchDeltas.empty() ? "none" : RoleName(static_cast<Role>(topCopy.role)),
                 topCopy.ms - topCopy.medianMs,
                 mainTop ? KeyName(mainCopy.name) : "", mainCopy.ms - mainCopy.medianMs,
                 w.req.name.c_str());
        m_hitchCovering = true;
        m_hitchCoveredUntil = w.last;
        m_stWinRequested.fetch_add(1, std::memory_order_relaxed);
        m_stHitches.fetch_add(1, std::memory_order_relaxed);
        m_windows.push_back(std::move(w));
    }

    void Collector::FeedWindows(uint64_t frame, const FrameRecord& r, const FrameContrib* c,
                                int64_t a, int64_t b)
    {
        const uint32_t n = ThreadCount();
        for (Window& w : m_windows)
        {
            if (w.finished) continue;
            if (frame < w.req.first) continue;
            if (frame > w.last + 1) { if (w.started) FinishWindow(w); continue; }
            if (frame == w.last + 1)
            {
                // Straddlers: scopes still open at the window's end that closed
                // in this frame. Then the window is complete.
                if (w.started)
                {
                    for (uint32_t i = 0; i < n; ++i)
                    {
                        const ThreadBuf& tb = m_buf[i];
                        for (size_t e = tb.first; e < tb.events.size(); ++e)
                        {
                            const Event& ev = tb.events[e];
                            if (ev.EndNs() > a && ev.EndNs() <= b && ev.startNs < w.endNs)
                                w.events[i].push_back(ev);
                        }
                    }
                    FinishWindow(w);
                }
                continue;
            }
            // Inside the window (WindowsWant made sure c was built).
            if (c) AddContrib(w, r, *c);
        }
    }

    void Collector::FinishWindow(Window& w)
    {
        if (w.finished) return;
        w.finished = true;
        m_stWinFinished.fetch_add(1, std::memory_order_relaxed);
        if (m_cfg.traceJson && !m_outDir.empty() && !w.records.empty() &&
            w.records.front().level >= Level::Timeline)
            WriteTrace(w);
        {
            static const char* const kKinds[3] = { "capture", "named", "hitch" };
            CaptureInfo ci;
            ci.name     = w.req.name;
            ci.kind     = kKinds[w.kind < 3 ? w.kind : 0];
            ci.trace    = w.traceFile.empty() ? std::string() : m_outDir + "/" + w.traceFile;
            ci.first    = w.req.first;
            ci.count    = w.req.count;
            ci.recorded = static_cast<uint32_t>(w.records.size());
            ci.complete = !w.truncated;
            std::lock_guard<std::mutex> lk(m_capturesMutex);
            m_captures.push_back(std::move(ci));
        }
        uint64_t events = 0;
        for (const auto& v : w.events) events += v.size();
        // Validity on the log line as name=value pairs, so a scenario pack can gate it with
        // expect.logLineFields (the summary JSON carries the same numbers).
        uint64_t dS = 0, dO = 0, dC = 0;
        std::vector<double> untrackedPct, util, boundary, mainScopes;
        for (const FrameRecord& r : w.records)
        {
            dS += r.droppedSuspend; dO += r.orphaned; dC += r.crossThread;
            if (r.periodMs > 0.f) untrackedPct.push_back(100.0 * r.mainUntrackedMs / r.periodMs);
            util.push_back(r.util);
            boundary.push_back(r.boundaryUs);
            mainScopes.push_back(r.mainScopes);
        }
        const Dist ut = Distribution(std::move(untrackedPct));
        const Dist uu = Distribution(std::move(util));
        const Dist bu = Distribution(std::move(boundary));
        const Dist ms = Distribution(std::move(mainScopes));
        const double mainEstMs = ms.p50 * ScopeCostNs() * 1e-6 + bu.p50 * 1e-3;
        LOG_INFO("[PROFILER] window '%s' first=%llu last=%llu recorded=%zu timeline=%llu events=%llu "
                 "lost=%llu droppedSuspend=%llu orphaned=%llu crossThread=%llu "
                 "untrackedPctP50=%.2f utilP50=%.4f boundaryUsP50=%.1f mainScopesP50=%.0f "
                 "scopeCostNs=%.1f mainEstMsP50=%.4f truncated=%d trace='%s'",
                 w.req.name.c_str(),
                 static_cast<unsigned long long>(w.req.first),
                 static_cast<unsigned long long>(w.last),
                 w.records.size(),
                 static_cast<unsigned long long>(w.framesTimeline),
                 static_cast<unsigned long long>(events),
                 static_cast<unsigned long long>(w.lost),
                 static_cast<unsigned long long>(dS),
                 static_cast<unsigned long long>(dO),
                 static_cast<unsigned long long>(dC),
                 ut.p50, uu.p50, bu.p50, ms.p50, ScopeCostNs(), mainEstMs,
                 w.truncated ? 1 : 0,
                 w.traceFile.c_str());
        {
            std::vector<double> allocs, vram, ws, faults;
            for (const FrameRecord& r : w.records)
            {
                allocs.push_back(r.allocs);
                if (!r.memValid) continue;
                vram.push_back(static_cast<double>(r.vramLocalBytes) / 1048576.0);
                ws.push_back(static_cast<double>(r.workingSetBytes) / 1048576.0);
                faults.push_back(r.pageFaults);
            }
            const size_t sampled = vram.size();
            const Dist da = Distribution(std::move(allocs));
            const Dist dv = Distribution(std::move(vram));
            const Dist dw = Distribution(std::move(ws));
            const Dist df = Distribution(std::move(faults));
            LOG_INFO("[PROFILER] window '%s' memory: allocsP50=%.0f allocsMax=%.0f framesSampled=%zu "
                     "vramMBP50=%.1f vramMBMax=%.1f wsMBP50=%.1f wsMBMax=%.1f pageFaultsP50=%.0f",
                     w.req.name.c_str(), da.p50, da.max, sampled, dv.p50, dv.max, dw.p50, dw.max, df.p50);
        }
        WriteSummary();
        // The events are in the trace now; keep the records and name stats for
        // the summary, release the bulk.
        for (auto& v : w.events) { std::vector<Event>().swap(v); }
    }

    void Collector::WriteTrace(Window& w)
    {
        const std::string file = w.req.name + ".trace.json";
        const std::string path = m_outDir + "/" + file;
        std::ofstream out = Sys::OpenOfstream(path, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!out.is_open())
        {
            LOG_ERROR("[PROFILER] cannot write '%s'", path.c_str());
            return;
        }
        const size_t frames = (std::min)(w.records.size(), static_cast<size_t>(kTraceFrameCap));
        const int64_t base = w.beginNs;
        const int64_t cut  = frames ? w.records[frames - 1].endNs : w.endNs;

        std::string s;
        s.reserve(1 << 20);
        auto flush = [&] {
            if (s.size() > (1u << 20)) { out.write(s.data(), static_cast<std::streamsize>(s.size())); s.clear(); }
        };
        s += "{\"displayTimeUnit\":\"ms\",\"otherData\":{\"schema\":\"dx12.profile.trace/1\",\"window\":";
        AppendJsonString(s, w.req.name.c_str());
        Appendf(s, ",\"firstFrame\":%llu,\"frames\":%zu,\"framesInWindow\":%zu,\"clock\":\"steady_clock ns, ts relative to the window's first main loop top\"},",
                static_cast<unsigned long long>(w.req.first), frames, w.records.size());
        s += "\"traceEvents\":[";
        bool first = true;
        auto sep = [&] { if (!first) s.push_back(','); first = false; };

        sep(); s += "{\"name\":\"process_name\",\"ph\":\"M\",\"pid\":1,\"args\":{\"name\":\"DX12Engine\"}}";
        const uint32_t n = ThreadCount();
        for (uint32_t i = 0; i < n; ++i)
        {
            ThreadCounters tc;
            if (!ReadThreadCounters(i, tc)) continue;
            if (w.events[i].empty()) continue;
            const int sortIndex = tc.role == Role::Main ? 0 : tc.role == Role::RenderOwner ? 1 :
                                  tc.role == Role::Worker ? 10 + static_cast<int>(i) : 100 + static_cast<int>(i);
            sep(); Appendf(s, "{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,\"tid\":%lu,\"args\":{\"name\":",
                           static_cast<unsigned long>(tc.osTid));
            AppendJsonString(s, tc.name); s += "}}";
            sep(); Appendf(s, "{\"name\":\"thread_sort_index\",\"ph\":\"M\",\"pid\":1,\"tid\":%lu,\"args\":{\"sort_index\":%d}}",
                           static_cast<unsigned long>(tc.osTid), sortIndex);
        }

        // Frame boundaries + per-frame counters.
        uint32_t mainTid = 0;
        for (uint32_t i = 0; i < n; ++i)
        {
            ThreadCounters tc;
            if (ReadThreadCounters(i, tc) && tc.role == Role::Main) { mainTid = tc.osTid; break; }
        }
        for (size_t f = 0; f < frames; ++f)
        {
            const FrameRecord& r = w.records[f];
            const double ts = static_cast<double>(r.beginNs - base) * 1e-3;
            if (w.kind == 2 && r.frame == w.hitchFrame)
            {
                sep(); Appendf(s, "{\"name\":\"HITCH frame %llu: %.2f ms (median %.2f)\",\"ph\":\"i\",\"s\":\"g\",\"ts\":%.3f,\"pid\":1,\"tid\":%lu}",
                               static_cast<unsigned long long>(r.frame), static_cast<double>(w.hitchPeriodMs),
                               static_cast<double>(w.hitchMedianMs), ts, static_cast<unsigned long>(mainTid));
            }
            else
            {
                sep(); Appendf(s, "{\"name\":\"frame %llu\",\"ph\":\"i\",\"s\":\"g\",\"ts\":%.3f,\"pid\":1,\"tid\":%lu}",
                               static_cast<unsigned long long>(r.frame), ts, static_cast<unsigned long>(mainTid));
            }
            sep(); Appendf(s, "{\"name\":\"cpu\",\"ph\":\"C\",\"ts\":%.3f,\"pid\":1,\"args\":{\"util\":%.4f,\"workerWorkMs\":%.4f,\"sMainMs\":%.4f,\"allIdleMs\":%.4f}}",
                           ts, static_cast<double>(r.util), static_cast<double>(r.workerWorkMs),
                           static_cast<double>(r.sMainMs),
                           r.timelineValid ? static_cast<double>(r.allIdleMs) : 0.0);
        }

        uint64_t written = 0;
        for (uint32_t i = 0; i < n; ++i)
        {
            ThreadCounters tc;
            if (!ReadThreadCounters(i, tc)) continue;
            for (const Event& e : w.events[i])
            {
                if (e.startNs >= cut) continue;
                const double ts = static_cast<double>(e.startNs - base) * 1e-3;
                sep();
                s += "{\"name\":";
                AppendJsonString(s, NameOf(e.name));
                if (e.Category() == Cat::Marker)
                    Appendf(s, ",\"cat\":\"marker\",\"ph\":\"i\",\"s\":\"t\",\"ts\":%.3f,\"pid\":1,\"tid\":%lu}",
                            ts, static_cast<unsigned long>(tc.osTid));
                else
                    Appendf(s, ",\"cat\":\"%s\",\"ph\":\"X\",\"ts\":%.3f,\"dur\":%.3f,\"pid\":1,\"tid\":%lu}",
                            CatName(e.Category()), ts, static_cast<double>(e.dur100ns) * 0.1,
                            static_cast<unsigned long>(tc.osTid));
                ++written;
                flush();
            }
        }
        // GPU queues as their own process, one track per queue. The spans are
        // on the same steady_clock timeline as every CPU scope above.
        if (!w.gpuSpans.empty())
        {
            static const char* const kQueueNames[3] = { "Graphics queue", "Compute queue", "Copy queue" };
            sep(); s += "{\"name\":\"process_name\",\"ph\":\"M\",\"pid\":2,\"args\":{\"name\":\"GPU\"}}";
            for (int q = 0; q < 3; ++q)
            {
                sep(); Appendf(s, "{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":2,\"tid\":%d,\"args\":{\"name\":\"%s\"}}",
                               q, kQueueNames[q]);
            }
            for (const GpuSpan& sp : w.gpuSpans)
            {
                if (sp.beginNs >= cut) continue;
                sep();
                s += "{\"name\":";
                AppendJsonString(s, NameOf(sp.name));
                Appendf(s, ",\"cat\":\"gpu\",\"ph\":\"X\",\"ts\":%.3f,\"dur\":%.3f,\"pid\":2,\"tid\":%u}",
                        static_cast<double>(sp.beginNs - base) * 1e-3,
                        static_cast<double>(sp.endNs - sp.beginNs) * 1e-3,
                        static_cast<unsigned>(sp.queue));
                ++written;
                flush();
            }
        }
        s += "]}\n";
        out.write(s.data(), static_cast<std::streamsize>(s.size()));
        out.close();
        w.traceFile = file;
        w.traceFrames = static_cast<uint32_t>(frames);
        w.traceEvents = written;
    }

    void AppendRecordJson(std::string& s, const FrameRecord& r)
    {
        Appendf(s, "{\"frame\":%llu,\"periodMs\":", static_cast<unsigned long long>(r.frame));
        AppendNum(s, r.periodMs);
        Appendf(s, ",\"level\":\"%s\",\"P\":%u,\"workers\":%u", LevelName(r.level),
                static_cast<unsigned>(r.poolThreads), static_cast<unsigned>(r.workerThreads));
        s += ",\"util\":";            AppendNum(s, r.util);
        s += ",\"wTotalMs\":";        AppendNum(s, r.wTotalMs);
        s += ",\"sMainMs\":";         AppendNum(s, r.sMainMs);
        s += ",\"sLaneMs\":";         AppendNum(s, r.sLaneMs);
        s += ",\"workerWorkMs\":";    AppendNum(s, r.workerWorkMs);
        s += ",\"poolSpinMs\":";      AppendNum(s, r.poolSpinMs);
        s += ",\"mainUntrackedMs\":"; AppendNum(s, r.mainUntrackedMs);
        s += ",\"mainWaitGpuMs\":";   AppendNum(s, r.mainWaitGpuMs);
        s += ",\"mainWaitJoinMs\":";  AppendNum(s, r.mainWaitJoinMs);
        s += ",\"mainWaitPacingMs\":"; AppendNum(s, r.mainWaitPacingMs);
        s += ",\"osPoolCpuMs\":";
        if (r.osPoolCpuMs >= 0.f) AppendNum(s, r.osPoolCpuMs); else s += "null";
        s += ",\"allIdleMs\":";
        if (r.timelineValid) AppendNum(s, r.allIdleMs); else s += "null";
        s += ",\"concurrencyMs\":";
        if (r.timelineValid)
        {
            int top = r.poolThreads;
            if (top > kFrameThreads) top = kFrameThreads;
            s.push_back('[');
            for (int k = 0; k <= top; ++k) { if (k) s.push_back(','); AppendNum(s, r.concurrencyMs[k], 3); }
            s.push_back(']');
        }
        else s += "null";
        Appendf(s, ",\"events\":%u,\"eventsLost\":%u,\"droppedSuspend\":%u,\"orphaned\":%u,\"droppedDeep\":%u,\"crossThread\":%u",
                r.events, r.eventsLost, r.droppedSuspend, r.orphaned, r.droppedDeep, r.crossThread);
        // The per-thread split's own corrections (ProfilerFrame.h).
        Appendf(s, ",\"splitStale\":%u,\"splitTakenBackUs\":", r.splitStale);
        AppendNum(s, r.splitTakenBackUs, 3);
        // GPU (null when this frame's timestamps never came back).
        s += ",\"gpu\":";
        if (r.gpuValid)
        {
            s += "{\"busyMs\":"; AppendNum(s, r.gpuBusyMs);
            s += ",\"graphicsMs\":"; AppendNum(s, r.gpuQueueMs[0]);
            s += ",\"computeMs\":";  AppendNum(s, r.gpuQueueMs[1]);
            s += ",\"copyMs\":";     AppendNum(s, r.gpuQueueMs[2]);
            s += ",\"overlapMs\":";  AppendNum(s, r.gpuOverlapMs);
            s += ",\"periodMs\":";   AppendNum(s, r.gpuDerived ? r.gpuPeriodMs : std::nan(""));
            s += ",\"util\":";       AppendNum(s, r.gpuDerived ? r.gpuUtil : std::nan(""));
            s += ",\"starveMs\":";   AppendNum(s, r.gpuDerived ? r.gpuStarveMs : std::nan(""));
            s += ",\"bubbleMs\":";   AppendNum(s, r.gpuBubbleMs);
            s += ",\"submitToStartMs\":"; AppendNum(s, r.gpuSubmitToStartMs);
            Appendf(s, ",\"submitLate\":%s,\"regions\":%u,\"dropped\":%u,\"calibrated\":%s}",
                    r.gpuSubmitLate ? "true" : "false", static_cast<unsigned>(r.gpuRegions),
                    static_cast<unsigned>(r.gpuDropped), r.gpuCalibrated ? "true" : "false");
        }
        else s += "null";
        Appendf(s, ",\"bound\":\"%s\"", BoundName(r.bound));
        // Memory: heap traffic (every frame) and the collector's sample (null
        // until taken). MB as 1024*1024.
        Appendf(s, ",\"allocs\":%u,\"frees\":%u,\"allocMB\":", r.allocs, r.frees);
        AppendNum(s, static_cast<double>(r.allocBytes) / 1048576.0, 3);
        s += ",\"mem\":";
        if (r.memValid)
        {
            s += "{\"vramMB\":";        AppendNum(s, static_cast<double>(r.vramLocalBytes) / 1048576.0, 1);
            s += ",\"vramBudgetMB\":";  AppendNum(s, static_cast<double>(r.vramLocalBudget) / 1048576.0, 1);
            s += ",\"nonLocalMB\":";    AppendNum(s, static_cast<double>(r.vramNonLocalBytes) / 1048576.0, 1);
            s += ",\"workingSetMB\":";  AppendNum(s, static_cast<double>(r.workingSetBytes) / 1048576.0, 1);
            s += ",\"privateMB\":";     AppendNum(s, static_cast<double>(r.privateBytes) / 1048576.0, 1);
            Appendf(s, ",\"pageFaults\":%u}", r.pageFaults);
        }
        else s += "null";
        // GPU resource ledger (MB per category, only the non-empty ones), the
        // frame's staging uploads, and the gauges that were reported.
        s += ",\"gpuLedgerMB\":{";
        {
            bool first = true;
            for (int i = 0; i < kGpuMemCats; ++i)
            {
                if (!r.gpuLedgerBytes[i] && !r.gpuLedgerCount[i]) continue;
                if (!first) s.push_back(',');
                first = false;
                Appendf(s, "\"%s\":", GpuMemCatName(static_cast<GpuMemCat>(i)));
                AppendNum(s, static_cast<double>(r.gpuLedgerBytes[i]) / 1048576.0, 2);
            }
        }
        Appendf(s, "},\"gpuPlaced\":%d,\"uploadMB\":", r.gpuPlacedCount);
        AppendNum(s, static_cast<double>(r.uploadBytes) / 1048576.0, 3);
        s += ",\"gauges\":{";
        {
            bool first = true;
            for (int i = 0; i < kGauges; ++i)
            {
                if (r.gauges[i] != r.gauges[i]) continue;   // NaN: not reported
                if (!first) s.push_back(',');
                first = false;
                Appendf(s, "\"%s\":", GaugeName(static_cast<Gauge>(i)));
                AppendNum(s, r.gauges[i], 1);
            }
        }
        s += "}";
        s += ",\"boundaryUs\":"; AppendNum(s, r.boundaryUs, 2);
        Appendf(s, ",\"mainScopes\":%u,\"totalScopes\":%u", r.mainScopes, r.totalScopes);
        Appendf(s, ",\"sysBusy100ns\":%llu,\"sysTotal100ns\":%llu,\"procBusy100ns\":%llu",
                static_cast<unsigned long long>(r.sysBusy100ns),
                static_cast<unsigned long long>(r.sysTotal100ns),
                static_cast<unsigned long long>(r.procBusy100ns));
        s += ",\"threads\":[";
        for (uint8_t t = 0; t < r.threadCount; ++t)
        {
            const ThreadFrame& tf = r.threads[t];
            ThreadCounters tc;
            const char* name = ReadThreadCounters(tf.index, tc) ? tc.name : "?";
            if (t) s.push_back(',');
            Appendf(s, "{\"i\":%u,\"name\":", static_cast<unsigned>(tf.index));
            AppendJsonString(s, name);
            Appendf(s, ",\"role\":\"%s\"", RoleName(tf.role));
            for (int c = 0; c < static_cast<int>(Cat::Marker); ++c)
            {
                Appendf(s, ",\"%s\":", CatName(static_cast<Cat>(c)));
                AppendNum(s, tf.catMs[c], 3);
            }
            s += ",\"untracked\":"; AppendNum(s, tf.untrackedMs, 3);
            Appendf(s, ",\"scopes\":%u,\"allocs\":%u", tf.scopes, tf.allocs);
            s += ",\"os\":";
            if (tf.osCpuMs >= 0.f) AppendNum(s, tf.osCpuMs, 3); else s += "null";
            s.push_back('}');
        }
        s += "]}";
    }

    void Collector::WriteSummary()
    {
        if (m_outDir.empty()) return;
        std::string s;
        s.reserve(64 * 1024);
        s += "{\"schema\":\"dx12.profile/1\",";
        // Field semantics travel WITH the numbers, so a reader (human or model)
        // does not have to find this file's source to know what it measured.
        s += "\"semantics\":{"
             "\"window\":\"frames [first, first+count); a frame's window is the MAIN thread loop period (loop top to next loop top)\","
             "\"periodMs\":\"main loop period = true frame time including pacing\","
             "\"workMs\":\"self time of Work scopes (the innermost scope decides what a thread is doing)\","
             "\"untrackedMs\":\"window minus every scope's self time; on main = instrumentation gap, on a worker = time outside any task\","
             "\"wTotalMs\":\"Work summed over the pool (main + render-owner lane + task workers)\","
             "\"util\":\"wTotalMs / (P * periodMs); RECORDED, not an acceptance criterion\","
             "\"sMainMs\":\"main thread Work\",\"sLaneMs\":\"render-owner lane Work (Game.exe graph mode)\","
             "\"allIdleMs\":\"main in Work while no task worker is in Work (Timeline level)\","
             "\"concurrencyPct\":\"share of the window with exactly k pool threads in Work, k = index\","
             "\"osCpuMs\":\"OS-scheduled CPU time (QueryThreadCycleTime); wall Work above OS CPU = preemption / SMT sharing\","
             "\"topSelf\":\"self time per scope name, summed over all threads of a role, per frame\","
             "\"externalCpuPct\":\"machine CPU used by OTHER processes over the window, % of all logical processors\","
             "\"gpu.busyMs\":\"union of every queue's instrumented regions = GPU busy (nesting-proof)\","
             "\"gpu.periodMs\":\"first region of frame+1 minus first region of frame (the GPU's own period)\","
             "\"gpu.util\":\"busyMs / periodMs\","
             "\"gpu.starveMs\":\"GPU idle between the frame's last region and the next frame's first\","
             "\"gpu.bubbleMs\":\"envelope (first..last region) minus busy: gaps inside the frame\","
             "\"gpu.submitToStartMs\":\"CPU's first ExecuteCommandLists of the frame to its first GPU region\","
             "\"gpu.bound\":\"per frame: gpu = the next frame was submitted before this one finished and util >= 85% (or main waited on the GPU longer than the GPU starved); pacing = the limiter slept >= 1 ms; cpu = the GPU finished and idled until the CPU submitted the next frame, longer than main waited on it - so a CPU stall in frame N shows on frame N-1; else mixed\","
             "\"gpu.passes.cpuRecordMsP50\":\"CPU time recording that pass (same name, same frame tag), null when no CPU scope matches\","
             "\"memory.allocsPerFrame\":\"operator new calls over all threads (Lua / FreeType / prebuilt libraries' own malloc are not in it)\","
             "\"windows.kind\":\"capture = -profile_window, named = Lua BeginWindow/EndWindow/Capture, hitch = -profile_hitch (captured after the fact from the flight recorder)\","
             "\"windows.hitch.topDeltas\":\"self time per (role, scope) in the slow frame minus its median over the recorder's earlier frames (absent = 0); worker idle excluded; (untracked) = no scope open\","
             "\"memory.gpuLedger\":\"GPU memory the engine created, per category, until D3D12 really destroyed it (resources waiting in the deferred-release queue count); placed resources are counted in placedResources and add no bytes (their heap has them)\","
             "\"memory.top\":\"the 30 largest live GPU objects when the summary was written, by the name the engine created them under (empty: none) - committed resources, heaps, shader-visible descriptor heaps; a placed resource is in its heap (render-arch TL8.3)\","
             "\"memory.unattributedLocalMB\":\"DXGI local usage minus the ledger's local categories: SDK (DLSS / FSR), driver, ImGui and Tracy memory - large is expected, growth is not\","
             "\"memory.*MB\":\"sampled once per frame on the collector thread; slopePerMin = least-squares trend over the window, null when it spans under 10 s\""
             "},";
        s += "\"build\":{\"config\":";
        AppendJsonString(s, BuildConfigName());
        s += ",\"commandLine\":";
        AppendJsonString(s, Sys::CommandLine::Get().ToString().c_str());
        s += "},\"environment\":{\"cpu\":";
        AppendJsonString(s, CpuBrand().c_str());
        Appendf(s, ",\"logicalProcessors\":%lu,\"tscHz\":%.0f,\"level\":\"%s\",\"osSampling\":%s,"
                   "\"nopacing\":%s,\"fixeddt\":%s}",
                static_cast<unsigned long>(::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)),
                TscHz(), LevelName(GetLevel()), OsSampling() ? "true" : "false",
                Sys::CommandLine::Get().Has("-nopacing") ? "true" : "false",
                Sys::CommandLine::Get().Has("-fixeddt") ? "true" : "false");
        s += ",\"windows\":[";
        bool firstWindow = true;
        for (const Window& w : m_windows)
        {
            if (!w.started) continue;
            if (!firstWindow) s.push_back(',');
            firstWindow = false;
            const size_t nf = w.records.size();
            s += "{\"name\":"; AppendJsonString(s, w.req.name.c_str());
            static const char* const kKinds[3] = { "capture", "named", "hitch" };
            Appendf(s, ",\"kind\":\"%s\"", kKinds[w.kind < 3 ? w.kind : 0]);
            if (w.kind == 2)
            {
                Appendf(s, ",\"hitch\":{\"frame\":%llu,\"periodMs\":", static_cast<unsigned long long>(w.hitchFrame));
                AppendNum(s, w.hitchPeriodMs, 3);
                s += ",\"medianMs\":"; AppendNum(s, w.hitchMedianMs, 3);
                Appendf(s, ",\"baseFrames\":%u,\"topDeltas\":[", w.hitchBaseFrames);
                for (size_t i = 0; i < w.hitchDeltas.size(); ++i)
                {
                    const Window::Delta& d = w.hitchDeltas[i];
                    if (i) s.push_back(',');
                    s += "{\"name\":"; AppendJsonString(s, KeyName(d.name));
                    Appendf(s, ",\"role\":\"%s\",\"selfMs\":", RoleName(static_cast<Role>(d.role)));
                    AppendNum(s, d.ms, 3);
                    s += ",\"medianMs\":"; AppendNum(s, d.medianMs, 3);
                    s += ",\"deltaMs\":";  AppendNum(s, d.ms - d.medianMs, 3);
                    s.push_back('}');
                }
                s += "]}";
            }
            Appendf(s, ",\"first\":%llu,\"count\":%u,\"framesRecorded\":%zu,\"framesTimeline\":%llu,"
                       "\"complete\":%s,\"truncated\":%s,",
                    static_cast<unsigned long long>(w.req.first), w.req.count, nf,
                    static_cast<unsigned long long>(w.framesTimeline),
                    w.finished && !w.truncated ? "true" : "false", w.truncated ? "true" : "false");
            s += "\"trace\":";
            if (w.traceFile.empty()) s += "null"; else AppendJsonString(s, w.traceFile.c_str());
            Appendf(s, ",\"traceFrames\":%u,\"traceEvents\":%llu,", w.traceFrames,
                    static_cast<unsigned long long>(w.traceEvents));

            // Validity.
            uint64_t dS = 0, dO = 0, dD = 0, dC = 0;
            std::vector<double> untrackedPct;
            for (const FrameRecord& r : w.records)
            {
                dS += r.droppedSuspend; dO += r.orphaned; dD += r.droppedDeep; dC += r.crossThread;
                if (r.periodMs > 0.f) untrackedPct.push_back(100.0 * r.mainUntrackedMs / r.periodMs);
            }
            const Dist ut = Distribution(untrackedPct);
            Appendf(s, "\"validity\":{\"eventsLost\":%llu,\"droppedSuspend\":%llu,\"orphaned\":%llu,"
                       "\"droppedDeep\":%llu,\"crossThread\":%llu,\"mainUntrackedPctP50\":",
                    static_cast<unsigned long long>(w.lost), static_cast<unsigned long long>(dS),
                    static_cast<unsigned long long>(dO), static_cast<unsigned long long>(dD),
                    static_cast<unsigned long long>(dC));
            AppendNum(s, ut.p50, 2);
            s += "},";

            // Machine load over the window.
            uint64_t sysBusy = 0, sysTotal = 0, procBusy = 0;
            for (const FrameRecord& r : w.records)
            { sysBusy += r.sysBusy100ns; sysTotal += r.sysTotal100ns; procBusy += r.procBusy100ns; }
            if (sysTotal > 0)
            {
                const double sysPct  = 100.0 * static_cast<double>(sysBusy) / static_cast<double>(sysTotal);
                const double procPct = 100.0 * static_cast<double>(procBusy) / static_cast<double>(sysTotal);
                const double extPct  = (std::max)(0.0, sysPct - procPct);
                s += "\"machine\":{\"sysCpuPct\":"; AppendNum(s, sysPct, 2);
                s += ",\"procCpuPct\":";            AppendNum(s, procPct, 2);
                s += ",\"externalCpuPct\":";        AppendNum(s, extPct, 2);
                Appendf(s, ",\"quiet\":%s,\"quietThresholdPct\":%.1f},",
                        extPct < kQuietExternalPct ? "true" : "false", static_cast<double>(kQuietExternalPct));
            }
            else s += "\"machine\":null,";

            auto collect = [&](auto field) {
                std::vector<double> v; v.reserve(nf);
                for (const FrameRecord& r : w.records) v.push_back(field(r));
                return Distribution(std::move(v));
            };

            // The profiler's own cost, MEASURED: a calibrated per-scope cost
            // times the scopes actually closed, plus the frame-boundary call.
            // An in-process on/off A/B of frame time cannot resolve a number
            // this small on a loaded machine; this can.
            {
                const double cost = ScopeCostNs();
                const Dist bu = collect([](const FrameRecord& r) { return double(r.boundaryUs); });
                const Dist ms = collect([](const FrameRecord& r) { return double(r.mainScopes); });
                const Dist ts = collect([](const FrameRecord& r) { return double(r.totalScopes); });
                s += "\"profilerOverhead\":{\"scopeCostNs\":"; AppendNum(s, cost, 2);
                s += ",";
                AppendDist(s, "boundaryUs", bu);  s += ",";
                AppendDist(s, "mainScopes", ms);  s += ",";
                AppendDist(s, "totalScopes", ts);
                s += ",\"mainEstMsP50\":";
                AppendNum(s, ms.p50 * cost * 1e-6 + bu.p50 * 1e-3, 4);
                s += ",\"allThreadsEstMsCoreP50\":";
                AppendNum(s, ts.p50 * cost * 1e-6 + bu.p50 * 1e-3, 4);
                s += ",\"note\":\"collector thread cost is its own row in cpu.threads (Profiler.Collector)\"},";
            }
            s += "\"frame\":{";
            AppendDist(s, "periodMs", collect([](const FrameRecord& r) { return double(r.periodMs); }));
            s += "},\"cpu\":{";
            const FrameRecord& r0 = w.records.back();
            Appendf(s, "\"P\":%u,\"workers\":%u,", static_cast<unsigned>(r0.poolThreads),
                    static_cast<unsigned>(r0.workerThreads));
            AppendDist(s, "util",            collect([](const FrameRecord& r) { return double(r.util); }));            s += ",";
            AppendDist(s, "wTotalMs",        collect([](const FrameRecord& r) { return double(r.wTotalMs); }));        s += ",";
            AppendDist(s, "sMainMs",         collect([](const FrameRecord& r) { return double(r.sMainMs); }));         s += ",";
            AppendDist(s, "sLaneMs",         collect([](const FrameRecord& r) { return double(r.sLaneMs); }));         s += ",";
            AppendDist(s, "workerWorkMs",    collect([](const FrameRecord& r) { return double(r.workerWorkMs); }));    s += ",";
            AppendDist(s, "poolSpinMs",      collect([](const FrameRecord& r) { return double(r.poolSpinMs); }));      s += ",";
            AppendDist(s, "mainUntrackedMs", collect([](const FrameRecord& r) { return double(r.mainUntrackedMs); })); s += ",";
            AppendDist(s, "mainWaitGpuMs",   collect([](const FrameRecord& r) { return double(r.mainWaitGpuMs); }));   s += ",";
            AppendDist(s, "mainWaitJoinMs",  collect([](const FrameRecord& r) { return double(r.mainWaitJoinMs); }));  s += ",";
            AppendDist(s, "mainWaitPacingMs",collect([](const FrameRecord& r) { return double(r.mainWaitPacingMs); }));s += ",";
            {
                std::vector<double> v;
                for (const FrameRecord& r : w.records) if (r.timelineValid) v.push_back(r.allIdleMs);
                AppendDist(s, "allIdleMs", Distribution(std::move(v)));
                s += ",";
            }
            {
                std::vector<double> v;
                for (const FrameRecord& r : w.records) if (r.osPoolCpuMs >= 0.f) v.push_back(r.osPoolCpuMs);
                AppendDist(s, "osPoolCpuMs", Distribution(std::move(v)));
            }

            // Concurrency histogram, as % of the summed window.
            {
                double conc[kFrameThreads + 1]{};
                double total = 0.0;
                int top = 0;
                for (const FrameRecord& r : w.records)
                {
                    if (!r.timelineValid) continue;
                    top = (std::max)(top, (std::min)(static_cast<int>(r.poolThreads), kFrameThreads));
                    for (int k = 0; k <= kFrameThreads; ++k) { conc[k] += r.concurrencyMs[k]; total += r.concurrencyMs[k]; }
                }
                s += ",\"concurrencyPct\":";
                if (total > 0.0)
                {
                    s.push_back('[');
                    for (int k = 0; k <= top; ++k) { if (k) s.push_back(','); AppendNum(s, 100.0 * conc[k] / total, 2); }
                    s.push_back(']');
                }
                else s += "null";
            }

            // Per-thread means.
            s += ",\"threads\":[";
            {
                struct Acc { double cat[kCatCount]{}; double untracked = 0; double os = 0; uint32_t osN = 0; uint32_t n = 0; Role role = Role::Other; };
                std::vector<Acc> acc(kMaxThreads);
                for (const FrameRecord& r : w.records)
                    for (uint8_t t = 0; t < r.threadCount; ++t)
                    {
                        const ThreadFrame& tf = r.threads[t];
                        Acc& a = acc[tf.index];
                        a.role = tf.role;
                        for (int c = 0; c < kCatCount; ++c) a.cat[c] += tf.catMs[c];
                        a.untracked += tf.untrackedMs;
                        if (tf.osCpuMs >= 0.f) { a.os += tf.osCpuMs; ++a.osN; }
                        ++a.n;
                    }
                bool firstT = true;
                for (uint32_t i = 0; i < static_cast<uint32_t>(kMaxThreads); ++i)
                {
                    const Acc& a = acc[i];
                    if (!a.n) continue;
                    ThreadCounters tc;
                    const char* name = ReadThreadCounters(i, tc) ? tc.name : "?";
                    if (!firstT) s.push_back(',');
                    firstT = false;
                    s += "{\"name\":"; AppendJsonString(s, name);
                    Appendf(s, ",\"role\":\"%s\",\"tid\":%lu", RoleName(a.role),
                            static_cast<unsigned long>(tc.osTid));
                    for (int c = 0; c < static_cast<int>(Cat::Marker); ++c)
                    {
                        Appendf(s, ",\"%sMs\":", CatName(static_cast<Cat>(c)));
                        AppendNum(s, a.cat[c] / a.n, 3);
                    }
                    s += ",\"untrackedMs\":"; AppendNum(s, a.untracked / a.n, 3);
                    s += ",\"osCpuMs\":";
                    if (a.osN) AppendNum(s, a.os / a.osN, 3); else s += "null";
                    s.push_back('}');
                }
            }
            s += "]";

            // Top self time per role.
            s += ",\"topSelf\":{";
            {
                const Role roles[] = { Role::Main, Role::RenderOwner, Role::Worker, Role::Other };
                bool firstRole = true;
                for (Role role : roles)
                {
                    std::vector<std::pair<NameId, NameStat>> list;
                    for (const auto& kv : w.names)
                        if ((kv.first >> 16) == static_cast<uint32_t>(role))
                            list.push_back({ static_cast<NameId>(kv.first & 0xFFFFu), kv.second });
                    if (list.empty()) continue;
                    std::sort(list.begin(), list.end(), [](const auto& x, const auto& y) {
                        return x.second.selfMs > y.second.selfMs;
                    });
                    if (list.size() > 20) list.resize(20);
                    if (!firstRole) s.push_back(',');
                    firstRole = false;
                    Appendf(s, "\"%s\":[", RoleName(role));
                    const double frames = w.framesTimeline ? static_cast<double>(w.framesTimeline) : 1.0;
                    for (size_t i = 0; i < list.size(); ++i)
                    {
                        if (i) s.push_back(',');
                        s += "{\"name\":"; AppendJsonString(s, NameOf(list[i].first));
                        s += ",\"selfMsPerFrame\":"; AppendNum(s, list[i].second.selfMs / frames, 4);
                        s += ",\"countPerFrame\":";  AppendNum(s, static_cast<double>(list[i].second.count) / frames, 2);
                        s.push_back('}');
                    }
                    s.push_back(']');
                }
            }
            s += "}}";   // topSelf, cpu

            // ---- GPU (ProfilerGpu.h) ----
            s += ",\"gpu\":";
            if (!w.framesGpu) s += "null";
            else
            {
                Appendf(s, "{\"frames\":%llu,\"framesDerived\":%llu,",
                        static_cast<unsigned long long>(w.framesGpu),
                        static_cast<unsigned long long>(w.framesGpuDerived));
                auto gcollect = [&](auto field, bool derivedOnly) {
                    std::vector<double> v;
                    for (const FrameRecord& r : w.records)
                        if (r.gpuValid && (!derivedOnly || r.gpuDerived)) v.push_back(field(r));
                    return Distribution(std::move(v));
                };
                AppendDist(s, "busyMs",     gcollect([](const FrameRecord& r) { return double(r.gpuBusyMs); }, false));     s += ",";
                AppendDist(s, "graphicsMs", gcollect([](const FrameRecord& r) { return double(r.gpuQueueMs[0]); }, false)); s += ",";
                AppendDist(s, "computeMs",  gcollect([](const FrameRecord& r) { return double(r.gpuQueueMs[1]); }, false)); s += ",";
                AppendDist(s, "copyMs",     gcollect([](const FrameRecord& r) { return double(r.gpuQueueMs[2]); }, false)); s += ",";
                AppendDist(s, "overlapMs",  gcollect([](const FrameRecord& r) { return double(r.gpuOverlapMs); }, false));  s += ",";
                AppendDist(s, "bubbleMs",   gcollect([](const FrameRecord& r) { return double(r.gpuBubbleMs); }, false));   s += ",";
                AppendDist(s, "submitToStartMs", gcollect([](const FrameRecord& r) { return double(r.gpuSubmitToStartMs); }, false)); s += ",";
                AppendDist(s, "periodMs",   gcollect([](const FrameRecord& r) { return double(r.gpuPeriodMs); }, true));    s += ",";
                AppendDist(s, "util",       gcollect([](const FrameRecord& r) { return double(r.gpuUtil); }, true));        s += ",";
                AppendDist(s, "starveMs",   gcollect([](const FrameRecord& r) { return double(r.gpuStarveMs); }, true));    s += ",";
                bool calibrated = true;
                uint64_t dropped = 0;
                for (const FrameRecord& r : w.records)
                    if (r.gpuValid) { calibrated = calibrated && r.gpuCalibrated; dropped += r.gpuDropped; }
                Appendf(s, "\"calibrated\":%s,\"regionsDropped\":%llu,",
                        calibrated ? "true" : "false", static_cast<unsigned long long>(dropped));

                // What limited the window's frames.
                uint64_t boundCount[5]{};
                for (const FrameRecord& r : w.records)
                    ++boundCount[static_cast<int>(r.bound) < 5 ? static_cast<int>(r.bound) : 0];
                s += "\"bound\":{";
                for (int bi = 0; bi < 5; ++bi)
                {
                    if (bi) s.push_back(',');
                    Appendf(s, "\"%s\":", BoundName(static_cast<Bound>(bi)));
                    AppendNum(s, nf ? static_cast<double>(boundCount[bi]) / static_cast<double>(nf) : 0.0, 3);
                }
                s += "},";

                // Per-pass table, by mean GPU time.
                struct Row
                {
                    NameId name; uint8_t queue; Dist gpu; Dist cpu; uint32_t frames;
                    GpuCounters counted; uint32_t countedFrames = 0, nativeFrames = 0;   // summed over the counted frames
                };
                std::vector<Row> rows;
                double busyMean = 0.0;
                {
                    double sum = 0.0; uint64_t cnt = 0;
                    for (const FrameRecord& r : w.records) if (r.gpuValid) { sum += r.gpuBusyMs; ++cnt; }
                    busyMean = cnt ? sum / static_cast<double>(cnt) : 0.0;
                }
                for (const auto& kv : w.passes)
                {
                    Row row;
                    row.name   = static_cast<NameId>(kv.first & 0xFFFFu);
                    row.queue  = static_cast<uint8_t>(kv.first >> 16);
                    row.gpu    = Distribution(kv.second.gpuMs);
                    row.cpu    = Distribution(kv.second.cpuMs);
                    row.frames = kv.second.frames;
                    row.counted = kv.second.counted;
                    row.countedFrames = kv.second.countedFrames;
                    row.nativeFrames = kv.second.nativeFrames;
                    rows.push_back(row);
                }
                std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) { return x.gpu.mean > y.gpu.mean; });
                if (rows.size() > 60) rows.resize(60);
                static const char* const kQ[3] = { "graphics", "compute", "copy" };
                s += "\"passes\":[";
                for (size_t i = 0; i < rows.size(); ++i)
                {
                    const Row& row = rows[i];
                    if (i) s.push_back(',');
                    s += "{\"name\":"; AppendJsonString(s, NameOf(row.name));
                    Appendf(s, ",\"queue\":\"%s\",\"frames\":%u,\"gpuMsP50\":", row.queue < 3 ? kQ[row.queue] : "?", row.frames);
                    AppendNum(s, row.gpu.p50);
                    s += ",\"gpuMsP95\":";  AppendNum(s, row.gpu.p95);
                    s += ",\"gpuMsMean\":"; AppendNum(s, row.gpu.mean);
                    s += ",\"shareOfBusyPct\":";
                    // Mean over the frames the pass ran in, against the mean busy
                    // time; nested regions (a parent and its children) both count,
                    // so shares can sum past 100 - they are not a partition.
                    AppendNum(s, busyMean > 0.0 ? 100.0 * row.gpu.mean * row.frames /
                                     (busyMean * static_cast<double>(w.framesGpu ? w.framesGpu : 1)) : 0.0, 2);
                    s += ",\"cpuRecordMsP50\":";
                    if (row.cpu.n) AppendNum(s, row.cpu.p50); else s += "null";
                    // Per frame, over the frames the pass was counted.
                    if (row.countedFrames)
                    {
                        const double n = static_cast<double>(row.countedFrames);
                        s += ",\"drawsMean\":";      AppendNum(s, row.counted.draws / n, 2);
                        s += ",\"indirectMean\":";   AppendNum(s, row.counted.drawsIndirect / n, 2);
                        s += ",\"dispatchesMean\":"; AppendNum(s, row.counted.dispatches / n, 2);
                        s += ",\"indirectDispatchesMean\":"; AppendNum(s, row.counted.dispatchesIndirect / n, 2);
                        s += ",\"meshMean\":";       AppendNum(s, row.counted.dispatchesMesh / n, 2);
                        s += ",\"raysMean\":";       AppendNum(s, row.counted.dispatchesRays / n, 2);
                        s += ",\"psoMean\":";        AppendNum(s, row.counted.psoBinds / n, 2);
                        s += ",\"barriersMean\":";   AppendNum(s, row.counted.barriers / n, 2);
                        Appendf(s, ",\"nativeFrames\":%u", row.nativeFrames);
                    }
                    s.push_back('}');
                }
                s += "],";

                // Bubbles: GPU gaps between instrumented work, by the pair of
                // regions around them.
                struct BRow { NameId before, after; double msPerFrame; uint32_t count; };
                std::vector<BRow> brows;
                const double gf = w.framesGpu ? static_cast<double>(w.framesGpu) : 1.0;
                for (const auto& kv : w.bubbles)
                    brows.push_back({ static_cast<NameId>(kv.first >> 16), static_cast<NameId>(kv.first & 0xFFFFu),
                                      kv.second.ms / gf, kv.second.count });
                std::sort(brows.begin(), brows.end(), [](const BRow& x, const BRow& y) { return x.msPerFrame > y.msPerFrame; });
                if (brows.size() > 12) brows.resize(12);
                s += "\"bubbles\":[";
                for (size_t i = 0; i < brows.size(); ++i)
                {
                    if (i) s.push_back(',');
                    s += "{\"after\":"; AppendJsonString(s, NameOf(brows[i].before));
                    s += ",\"before\":"; AppendJsonString(s, NameOf(brows[i].after));
                    s += ",\"msPerFrame\":"; AppendNum(s, brows[i].msPerFrame);
                    Appendf(s, ",\"count\":%u}", brows[i].count);
                }
                s += "]}";
            }

            // ---- Memory ----
            s += ",\"memory\":{";
            {
                constexpr double kMB = 1048576.0;
                AppendDist(s, "allocsPerFrame", collect([](const FrameRecord& r) { return double(r.allocs); })); s += ",";
                AppendDist(s, "freesPerFrame",  collect([](const FrameRecord& r) { return double(r.frees); }));  s += ",";
                AppendDist(s, "allocMBPerFrame", collect([](const FrameRecord& r) { return double(r.allocBytes) / 1048576.0; })); s += ",";
                std::vector<const FrameRecord*> mem;
                for (const FrameRecord& r : w.records) if (r.memValid) mem.push_back(&r);
                Appendf(s, "\"framesSampled\":%zu", mem.size());
                if (!mem.empty())
                {
                    auto series = [&](const char* key, auto field) {
                        std::vector<double> v; v.reserve(mem.size());
                        for (const FrameRecord* r : mem) v.push_back(field(*r));
                        const Dist d = Distribution(v);
                        // Least-squares slope against the frame's start time: a
                        // leak (or a stream-in) is a trend, not a percentile.
                        double sx = 0, sy = 0, sxx = 0, sxy = 0;
                        const double t0 = static_cast<double>(mem.front()->beginNs);
                        for (size_t i = 0; i < mem.size(); ++i)
                        {
                            const double x = (static_cast<double>(mem[i]->beginNs) - t0) * 1e-9 / 60.0; // minutes
                            sx += x; sy += v[i]; sxx += x * x; sxy += x * v[i];
                        }
                        const double n = static_cast<double>(mem.size());
                        const double den = n * sxx - sx * sx;
                        const double slope = den > 0.0 ? (n * sxy - sx * sy) / den : 0.0;
                        // A trend needs a span: over a fraction of a second a
                        // 0.3 MB step extrapolates to hundreds of MB/min.
                        const double spanSec =
                            static_cast<double>(mem.back()->beginNs - mem.front()->beginNs) * 1e-9;
                        s += ",";
                        AppendJsonString(s, key);
                        s += ":{\"start\":"; AppendNum(s, v.front(), 1);
                        s += ",\"end\":";    AppendNum(s, v.back(), 1);
                        s += ",\"peak\":";   AppendNum(s, d.max, 1);
                        s += ",\"p50\":";    AppendNum(s, d.p50, 1);
                        s += ",\"slopePerMin\":";
                        if (spanSec >= kMinTrendSec) AppendNum(s, slope, 2);
                        else s += "null";
                        s += "}";
                    };
                    series("vramMB",       [&](const FrameRecord& r) { return double(r.vramLocalBytes) / kMB; });
                    series("vramBudgetMB", [&](const FrameRecord& r) { return double(r.vramLocalBudget) / kMB; });
                    series("nonLocalMB",   [&](const FrameRecord& r) { return double(r.vramNonLocalBytes) / kMB; });
                    series("workingSetMB", [&](const FrameRecord& r) { return double(r.workingSetBytes) / kMB; });
                    series("privateMB",    [&](const FrameRecord& r) { return double(r.privateBytes) / kMB; });
                    std::vector<double> pf;
                    for (const FrameRecord* r : mem) pf.push_back(r->pageFaults);
                    s += ",";
                    AppendDist(s, "pageFaultsPerFrame", Distribution(std::move(pf)));
                }
                // GPU resource ledger per category over the window (start, end,
                // peak MB; live objects at the end), and what DXGI reports that
                // the ledger does not (local VRAM only: Upload / Readback live
                // in system memory).
                s += ",\"gpuLedger\":{";
                if (!w.records.empty())
                {
                    const FrameRecord& r0 = w.records.front();
                    const FrameRecord& r1 = w.records.back();
                    bool firstCat = true;
                    for (int i = 0; i < kGpuMemCats; ++i)
                    {
                        uint64_t peak = 0;
                        for (const FrameRecord& r : w.records) peak = (std::max)(peak, r.gpuLedgerBytes[i]);
                        if (!peak && !r1.gpuLedgerCount[i]) continue;
                        if (!firstCat) s.push_back(',');
                        firstCat = false;
                        Appendf(s, "\"%s\":{\"startMB\":", GpuMemCatName(static_cast<GpuMemCat>(i)));
                        AppendNum(s, static_cast<double>(r0.gpuLedgerBytes[i]) / kMB, 2);
                        s += ",\"endMB\":";  AppendNum(s, static_cast<double>(r1.gpuLedgerBytes[i]) / kMB, 2);
                        s += ",\"peakMB\":"; AppendNum(s, static_cast<double>(peak) / kMB, 2);
                        Appendf(s, ",\"count\":%d}", r1.gpuLedgerCount[i]);
                    }
                }
                s += "}";
                // The largest live GPU objects, by name, when the summary is written - the ledger above,
                // object by object.
                {
                    std::vector<GpuObjectInfo> top;
                    ReadGpuObjects(top, 30);
                    s += ",\"top\":[";
                    for (size_t k = 0; k < top.size(); ++k)
                    {
                        if (k) s.push_back(',');
                        s += "{\"name\":"; AppendJsonString(s, top[k].name.c_str());
                        Appendf(s, ",\"category\":\"%s\",\"mb\":", GpuMemCatName(top[k].cat));
                        AppendNum(s, static_cast<double>(top[k].bytes) / kMB, 3);
                        s.push_back('}');
                    }
                    s += "]";
                }
                {
                    auto localOf = [](const FrameRecord& r) {
                        uint64_t b = 0;
                        for (int i = 0; i < kGpuMemCats; ++i)
                            if (GpuMemCatIsLocal(static_cast<GpuMemCat>(i))) b += r.gpuLedgerBytes[i];
                        return static_cast<double>(b) / 1048576.0;
                    };
                    auto totalOf = [](const FrameRecord& r) {
                        uint64_t b = 0;
                        for (int i = 0; i < kGpuMemCats; ++i) b += r.gpuLedgerBytes[i];
                        return static_cast<double>(b) / 1048576.0;
                    };
                    s += ",";
                    AppendDist(s, "gpuLedgerTotalMB", collect([&](const FrameRecord& r) { return totalOf(r); }));
                    s += ",";
                    AppendDist(s, "gpuLedgerLocalMB", collect([&](const FrameRecord& r) { return localOf(r); }));
                    std::vector<double> un;
                    for (const FrameRecord& r : w.records)
                        if (r.memValid) un.push_back(static_cast<double>(r.vramLocalBytes) / kMB - localOf(r));
                    s += ",";
                    AppendDist(s, "unattributedLocalMB", Distribution(std::move(un)));
                    s += ",\"placedResources\":";
                    Appendf(s, "%d", w.records.back().gpuPlacedCount);
                    s += ",";
                    AppendDist(s, "uploadMBPerFrame", collect([](const FrameRecord& r) { return double(r.uploadBytes) / 1048576.0; }));
                    s += ",\"gauges\":{";
                    bool firstG = true;
                    for (int g = 0; g < kGauges; ++g)
                    {
                        std::vector<double> gv;
                        for (const FrameRecord& r : w.records)
                            if (r.gauges[g] == r.gauges[g]) gv.push_back(r.gauges[g]);
                        if (gv.empty()) continue;
                        if (!firstG) s.push_back(',');
                        firstG = false;
                        AppendDist(s, GaugeName(static_cast<Gauge>(g)), Distribution(std::move(gv)));
                    }
                    s += "}";
                }
                // Which threads allocate: per-frame mean, top 8.
                std::vector<std::pair<uint16_t, double>> perThread;
                {
                    std::unordered_map<uint16_t, double> acc;
                    for (const FrameRecord& r : w.records)
                        for (uint8_t t = 0; t < r.threadCount; ++t) acc[r.threads[t].index] += r.threads[t].allocs;
                    for (const auto& kv : acc) perThread.push_back({ kv.first, kv.second / static_cast<double>(nf ? nf : 1) });
                    std::sort(perThread.begin(), perThread.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
                    if (perThread.size() > 8) perThread.resize(8);
                }
                s += ",\"topAllocThreads\":[";
                for (size_t i = 0; i < perThread.size(); ++i)
                {
                    ThreadCounters tc;
                    const char* name = ReadThreadCounters(perThread[i].first, tc) ? tc.name : "?";
                    if (i) s.push_back(',');
                    s += "{\"name\":"; AppendJsonString(s, name);
                    s += ",\"allocsPerFrame\":"; AppendNum(s, perThread[i].second, 1);
                    s.push_back('}');
                }
                s += "]";
            }
            s += "}";
            s += "}";   // window
        }
        s += "]}\n";

        const std::string path = m_outDir + "/profile_summary.json";
        std::ofstream out = Sys::OpenOfstream(path, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!out.is_open()) { LOG_ERROR("[PROFILER] cannot write '%s'", path.c_str()); return; }
        out.write(s.data(), static_cast<std::streamsize>(s.size()));
    }
} // namespace

std::string ProfileOutputDir()
{
    if (!Sys::HasArtifactDir()) return {};
    return Sys::ArtifactDir() + "/profile";
}

bool ParseCommandLine(CollectorConfig& out, std::string& error)
{
    const auto& cl = Sys::CommandLine::Get();

    // Refuse every -profile* token that is not in the table: CommandLine has no
    // table of its own, so a typo would otherwise run, measure nothing, and exit 0.
    static const char* const kKnown[] = { "profile", "profile_window", "profile_os", "profile_trace",
                                          "profile_gpu", "profile_alloc", "profile_hitch", "profile_parity" };
    for (const std::string& raw : cl.Args())
    {
        std::string a = raw;
        size_t dashes = 0;
        while (dashes < a.size() && a[dashes] == '-') ++dashes;
        if (dashes == 0) continue;
        std::string key = Lower(a.substr(dashes, a.find('=') == std::string::npos ? std::string::npos
                                                                                 : a.find('=') - dashes));
        if (key.rfind("profile", 0) != 0) continue;
        bool known = false;
        for (const char* k : kKnown) if (key == k) { known = true; break; }
        if (!known)
        {
            error = "unknown profiler flag '" + raw + "' (known: -profile, -profile_window, -profile_os, "
                    "-profile_trace, -profile_gpu, -profile_alloc, -profile_hitch, -profile_parity)";
            return false;
        }
    }

    if (cl.Has("-profile"))
    {
        const std::string v = Lower(cl.Get("-profile", "timeline"));
        if      (v == "off")      out.level = Level::Off;
        else if (v == "stats")    out.level = Level::Stats;
        else if (v == "timeline" || v.empty()) out.level = Level::Timeline;
        else if (v == "detail")   out.level = Level::Detail;
        else { error = "-profile=" + v + " is not off|stats|timeline|detail"; return false; }
    }
    if (cl.Has("-profile_os"))
    {
        const std::string v = cl.Get("-profile_os", "1");
        if (v != "0" && v != "1") { error = "-profile_os=" + v + " is not 0|1"; return false; }
        out.osSampling = v == "1";
    }
    if (cl.Has("-profile_hitch"))
    {
        const std::string v = cl.Get("-profile_hitch");
        const char* p = v.c_str();
        char* end = nullptr;
        const double ms = std::strtod(p, &end);
        unsigned long long mx = out.hitchMax, from = out.hitchFrom;
        bool ok = !v.empty() && end != p && ms > 0.0 && ms < 100000.0;
        if (ok && *end == ':')
        {
            char* e2 = nullptr;
            mx = std::strtoull(end + 1, &e2, 10);
            ok = e2 != end + 1 && mx >= 1 && mx <= 64;
            if (ok && *e2 == ':')
            {
                char* e3 = nullptr;
                from = std::strtoull(e2 + 1, &e3, 10);
                ok = e3 != e2 + 1 && *e3 == '\0';
            }
            else ok = ok && *e2 == '\0';
        }
        else ok = ok && *end == '\0';
        if (!ok) { error = "-profile_hitch=" + v + " is not <ms>[:<max 1-64>[:<fromFrame>]]"; return false; }
        out.hitchMs   = static_cast<float>(ms);
        out.hitchMax  = static_cast<uint32_t>(mx);
        out.hitchFrom = from;
        if (out.level == Level::Off && !cl.Has("-profile")) out.level = Level::Timeline;
        if (out.level < Level::Timeline)
        { error = "-profile_hitch needs -profile=timeline (or detail): a hitch is captured from the event rings"; return false; }
    }
    if (cl.Has("-profile_parity"))
    {
        const std::string v = cl.Get("-profile_parity", "1");
        if (v != "0" && v != "1") { error = "-profile_parity=" + v + " is not 0|1"; return false; }
        out.parity = v == "1";
        if (out.parity && out.level == Level::Off && !cl.Has("-profile")) out.level = Level::Timeline;
        if (out.parity && out.level < Level::Timeline)
        { error = "-profile_parity needs -profile=timeline (or detail): the check reads the event rings"; return false; }
    }
    if (cl.Has("-profile_alloc"))
    {
        const std::string v = cl.Get("-profile_alloc", "1");
        if (v != "0" && v != "1") { error = "-profile_alloc=" + v + " is not 0|1"; return false; }
        out.allocCounting = v == "1";
    }
    if (cl.Has("-profile_gpu"))
    {
        const std::string v = cl.Get("-profile_gpu", "1");
        if (v != "0" && v != "1") { error = "-profile_gpu=" + v + " is not 0|1"; return false; }
        out.gpu = v == "1";
    }
    if (cl.Has("-profile_trace"))
    {
        const std::string v = Lower(cl.Get("-profile_trace", "json"));
        if (v != "json" && v != "none") { error = "-profile_trace=" + v + " is not json|none"; return false; }
        out.traceJson = v == "json";
    }
    if (cl.Has("-profile_window"))
    {
        const std::string spec = cl.Get("-profile_window");
        if (spec.empty()) { error = "-profile_window needs <first>:<count>[:name][,...]"; return false; }
        size_t pos = 0;
        int index = 0;
        while (pos <= spec.size())
        {
            size_t comma = spec.find(',', pos);
            if (comma == std::string::npos) comma = spec.size();
            const std::string item = spec.substr(pos, comma - pos);
            pos = comma + 1;
            if (item.empty()) { if (comma == spec.size()) break; continue; }
            const size_t c1 = item.find(':');
            const size_t c2 = c1 == std::string::npos ? std::string::npos : item.find(':', c1 + 1);
            char* e1 = nullptr; char* e2 = nullptr;
            const std::string sFirst = item.substr(0, c1);
            const std::string sCount = c1 == std::string::npos ? std::string()
                                     : item.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1);
            const unsigned long long first = std::strtoull(sFirst.c_str(), &e1, 10);
            const unsigned long long count = std::strtoull(sCount.c_str(), &e2, 10);
            if (sFirst.empty() || sCount.empty() || !e1 || *e1 || !e2 || *e2 || count == 0 || count > 100000)
            {
                error = "-profile_window item '" + item + "' is not <first>:<count>[:name] with 1 <= count <= 100000";
                return false;
            }
            CaptureRequest req;
            req.first = first;
            req.count = static_cast<uint32_t>(count);
            req.name  = c2 == std::string::npos ? ("w" + std::to_string(index)) : item.substr(c2 + 1);
            for (char ch : req.name)
                if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-'))
                { error = "-profile_window name '" + req.name + "' may only use [A-Za-z0-9_-]"; return false; }
            out.windows.push_back(req);
            ++index;
            if (comma == spec.size()) break;
        }
        if (out.level == Level::Off && !cl.Has("-profile")) out.level = Level::Timeline;
        if (out.level < Level::Timeline)
        { error = "-profile_window needs -profile=timeline (or detail): a window exports the event rings"; return false; }
        if (!Sys::HasArtifactDir())
        { error = "-profile_window needs -artifacts=<dir>: the window is written to <dir>/profile/"; return false; }
    }
    return true;
}

void StartCollector(const CollectorConfig& config) { g_collector.Start(config); }

bool GetCollectorStatus(CollectorStatus& out) noexcept
{
    if (!g_collector.Running()) { out = CollectorStatus{}; out.level = GetLevel(); return false; }
    g_collector.Status(out);
    return true;
}

bool LatestRecordJson(std::string& out)
{
    FrameRecord r;
    if (!GetLatestFrameRecord(r)) return false;
    out.clear();
    AppendRecordJson(out, r);
    return true;
}

std::string RunProfileJson() { return g_collector.RunJson(); }

bool ParityEnabled() noexcept { return g_parity.load(std::memory_order_relaxed); }

void SetLiveTimeline(bool on) noexcept { g_collector.SetLive(on); }

bool CopyLiveTimeline(uint32_t frames, LiveTimeline& out)
{
    return g_collector.Running() && g_collector.CopyLive(frames, out);
}

std::vector<CaptureInfo> ListCaptures() { return g_collector.Captures(); }

std::string CollectorOutputDir() { return g_collector.Running() ? g_collector.OutDir() : std::string(); }

bool SetHitchCapture(float ms, uint32_t max)
{
    const double v = ms > 0.f ? static_cast<double>(ms) : 0.0;
    return g_collector.Enqueue({ 3, std::string(), static_cast<uint64_t>(v * 1000.0), max });
}

void NoteExternalTiming(uint64_t frame, const char* name, float ms)
{
    if (!ParityEnabled() || !name) return;
    g_collector.NoteExternal(frame, Intern(name), ms);
}

bool BeginNamedWindow(const std::string& name)
{
    return g_collector.Enqueue({ 1, name, CurrentFrame() + 1, 0 });
}

bool EndNamedWindow(const std::string& name)
{
    return g_collector.Enqueue({ 2, name, CurrentFrame(), 0 });
}

bool RequestCapture(const std::string& name, uint32_t frames)
{
    return g_collector.Enqueue({ 0, name.empty() ? std::string("capture") : name, CurrentFrame() + 1, frames });
}
void StopCollector() { g_collector.Stop(); }
bool CollectorRunning() noexcept { return g_collector.Running(); }

} // namespace Diag::Prof

#else // !DIAG_PROFILER

namespace Diag::Prof
{
    bool ParseCommandLine(CollectorConfig&, std::string&) { return true; }
    void StartCollector(const CollectorConfig&) {}
    void StopCollector() {}
    bool CollectorRunning() noexcept { return false; }
    bool GetCollectorStatus(CollectorStatus& out) noexcept { out = CollectorStatus{}; return false; }
    bool LatestRecordJson(std::string&) { return false; }
    std::string RunProfileJson() { return "null"; }
    bool ParityEnabled() noexcept { return false; }
    void NoteExternalTiming(uint64_t, const char*, float) {}
    void SetLiveTimeline(bool) noexcept {}
    bool CopyLiveTimeline(uint32_t, LiveTimeline&) { return false; }
    std::vector<CaptureInfo> ListCaptures() { return {}; }
    std::string CollectorOutputDir() { return {}; }
    bool SetHitchCapture(float, uint32_t) { return false; }
    bool BeginNamedWindow(const std::string&) { return false; }
    bool EndNamedWindow(const std::string&) { return false; }
    bool RequestCapture(const std::string&, uint32_t) { return false; }
    std::string ProfileOutputDir() { return {}; }
}

#endif
