// Diag::Prof core - the contract is in include/Diagnostics/Profiler.h.
// Dependency rule: this translation unit uses the Win32 API and the standard library and nothing
// from the engine - tests/unit/test_task_graph.ps1 compiles TaskSystem.cpp straight from source
// with a stubbed logger, and TaskSystem registers its workers here. Logging, command-line parsing
// and file output live in ProfilerCollector.cpp.
// Allocation rule: the recorder uses no CRT heap. Names live in a static arena, the thread registry
// and the frame-record ring are static arrays, and event rings come from VirtualAlloc, so profiling
// never shows up in the allocation counts it measures; the task-graph unit test asserts exact
// operator-new counts around instrumented code. Only the live GPU object table (RegisterGpuObject)
// allocates.

#include "Diagnostics/Profiler.h"
#include "Diagnostics/ProfilerFrame.h"
#include "Diagnostics/ProfilerGpu.h"
#include "Diagnostics/ProfilerMemory.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <limits>
#include <intrin.h>
#include <psapi.h>   // GetProcessMemoryInfo (kernel32 K32 export; no extra lib)

namespace Diag::Prof
{
    const char* GpuMemCatName(GpuMemCat c) noexcept
    {
        switch (c)
        {
        case GpuMemCat::Texture:        return "texture";
        case GpuMemCat::RenderTarget:   return "renderTarget";
        case GpuMemCat::DepthStencil:   return "depthStencil";
        case GpuMemCat::Buffer:         return "buffer";
        case GpuMemCat::Upload:         return "upload";
        case GpuMemCat::Readback:       return "readback";
        case GpuMemCat::AccelStruct:    return "accelStruct";
        case GpuMemCat::TransientHeap:  return "transientHeap";
        case GpuMemCat::BufferHeap:     return "bufferHeap";
        case GpuMemCat::QueryHeap:      return "queryHeap";
        case GpuMemCat::DescriptorHeap: return "descriptorHeap";
        case GpuMemCat::SwapChain:      return "swapChain";
        case GpuMemCat::Other:          return "other";
        case GpuMemCat::Count:          break;
        }
        return "?";
    }

    const char* GaugeName(Gauge g) noexcept
    {
        switch (g)
        {
        case Gauge::LuaKB:               return "luaKB";
        case Gauge::JoltStepPeakKB:      return "joltStepPeakKB";
        case Gauge::DescriptorsLive:     return "descriptorsLive";
        case Gauge::DescriptorsCapacity: return "descriptorsCapacity";
        case Gauge::Count:               break;
        }
        return "?";
    }

    const char* CatName(Cat c) noexcept
    {
        switch (c)
        {
        case Cat::Work:       return "work";
        case Cat::Spin:       return "spin";
        case Cat::WaitIdle:   return "idle";
        case Cat::WaitJoin:   return "join";
        case Cat::WaitGpu:    return "gpu";
        case Cat::WaitPacing: return "pacing";
        case Cat::WaitOther:  return "other";
        case Cat::Marker:     return "marker";
        }
        return "?";
    }

    const char* LevelName(Level l) noexcept
    {
        switch (l)
        {
        case Level::Off:      return "off";
        case Level::Stats:    return "stats";
        case Level::Timeline: return "timeline";
        case Level::Detail:   return "detail";
        }
        return "?";
    }

    const char* RoleName(Role r) noexcept
    {
        switch (r)
        {
        case Role::Main:        return "main";
        case Role::RenderOwner: return "renderowner";
        case Role::Worker:      return "worker";
        case Role::Other:       return "other";
        }
        return "?";
    }

    const char* BoundName(Bound b) noexcept
    {
        switch (b)
        {
        case Bound::Unknown: return "unknown";
        case Bound::Gpu:     return "gpu";
        case Bound::Cpu:     return "cpu";
        case Bound::Pacing:  return "pacing";
        case Bound::Mixed:   return "mixed";
        }
        return "?";
    }

    int64_t QpcToSteadyNs(int64_t ctr) noexcept
    {
        // Exactly MSVC's steady_clock::now() arithmetic (<chrono>), so a QPC calibration point lands on
        // the same nanosecond a CPU scope would.
        static const int64_t s_freq = [] {
            LARGE_INTEGER f{};
            ::QueryPerformanceFrequency(&f);
            return f.QuadPart > 0 ? static_cast<int64_t>(f.QuadPart) : 1;
        }();
        constexpr int64_t kNs = 1'000'000'000;
        if (s_freq == 10'000'000) return ctr * 100;
        const int64_t whole = (ctr / s_freq) * kNs;
        const int64_t part  = (ctr % s_freq) * kNs / s_freq;
        return whole + part;
    }

    int64_t SplitThreadWindow(const int64_t (&cumNs)[kAccCount], uint8_t cur,
                              int64_t (&reportedNs)[kAccCount], uint64_t (&outNs)[kAccCount]) noexcept
    {
        // Signed deltas against what was reported, not against the previous edge's reading: a reading can
        // be higher than the truth (it charged the outgoing slot up to its edge for a transition published
        // later). Clamping the next delta to 0 and rebasing on the lower reading would count that interval
        // twice, once in each slot.
        int64_t d[kAccCount];
        int64_t positive = 0, owed = 0;
        for (int c = 0; c < kAccCount; ++c)
        {
            d[c] = cumNs[c] - reportedNs[c];
            if (d[c] < 0) { owed -= d[c]; d[c] = 0; }
            else          positive += d[c];
        }
        if (owed >= positive)
        {
            // Nothing elapsed (net): report nothing, and keep the totals.
            for (uint64_t& v : outNs) v = 0;
            return 0;
        }
        // Take `owed` back. The current slot first: a late-published
        // transition's interval sits in the slot it switched to, which the
        // thread is usually still in. Then the largest, until it is paid.
        const int64_t takenBack = owed;
        auto take = [&d, &owed](int c) noexcept
        {
            const int64_t n = d[c] < owed ? d[c] : owed;
            d[c] -= n;
            owed -= n;
        };
        if (cur < kAccCount) take(cur);
        while (owed > 0)
        {
            int big = 0;
            for (int c = 1; c < kAccCount; ++c)
                if (d[c] > d[big]) big = c;
            take(big);
        }
        for (int c = 0; c < kAccCount; ++c)
        {
            outNs[c] = static_cast<uint64_t>(d[c]);
            reportedNs[c] += d[c];
        }
        return takenBack;
    }

    namespace Detail
    {
        // Exists in every configuration: Enabled()/GetLevel() read it inline.
        std::atomic<uint8_t> g_level{ 0 };
    }
}

#if DIAG_PROFILER

namespace Diag::Prof
{
namespace Detail
{
    ThreadState           g_threads[kMaxThreads];
    std::atomic<uint32_t> g_threadClaimed{ 0 };     // slots handed out (may exceed kMaxThreads)
    std::atomic<uint64_t> g_crossThread{ 0 };
    std::atomic<uint64_t> g_unregistered{ 0 };
    std::atomic<bool>     g_allocOn{ false };
    std::atomic<uint64_t> g_allocsUnattributed{ 0 };

    void CountCrossThreadDrop() noexcept
    {
        g_crossThread.fetch_add(1, std::memory_order_relaxed);
    }
}

// ---- GPU resource ledger + gauges (ProfilerMemory.h) ----
// Constant-initialised atomics: a resource created during static init or long before any profiler
// level is set is still counted.
namespace
{
    std::atomic<uint64_t> g_gpuBytes[kGpuMemCats];
    std::atomic<int64_t>  g_gpuCount[kGpuMemCats];
    std::atomic<int64_t>  g_gpuPlaced{ 0 };
    std::atomic<uint64_t> g_uploadBytes{ 0 };
    std::atomic<double>   g_gauge[kGauges];
    std::atomic<bool>     g_gaugeSet[kGauges];
}

void NoteGpuAlloc(GpuMemCat c, uint64_t bytes) noexcept
{
    const int i = static_cast<int>(c);
    if (i < 0 || i >= kGpuMemCats) return;
    g_gpuBytes[i].fetch_add(bytes, std::memory_order_relaxed);
    g_gpuCount[i].fetch_add(1, std::memory_order_relaxed);
}

void NoteGpuFree(GpuMemCat c, uint64_t bytes) noexcept
{
    const int i = static_cast<int>(c);
    if (i < 0 || i >= kGpuMemCats) return;
    g_gpuBytes[i].fetch_sub(bytes, std::memory_order_relaxed);
    g_gpuCount[i].fetch_sub(1, std::memory_order_relaxed);
}

void NoteGpuPlaced(int delta) noexcept { g_gpuPlaced.fetch_add(delta, std::memory_order_relaxed); }

void NoteUploadBytes(uint64_t bytes) noexcept { g_uploadBytes.fetch_add(bytes, std::memory_order_relaxed); }

void ReadGpuLedger(GpuLedger& out) noexcept
{
    out = GpuLedger{};
    for (int i = 0; i < kGpuMemCats; ++i)
    {
        out.bytes[i] = g_gpuBytes[i].load(std::memory_order_relaxed);
        out.count[i] = g_gpuCount[i].load(std::memory_order_relaxed);
        out.totalBytes += out.bytes[i];
        if (GpuMemCatIsLocal(static_cast<GpuMemCat>(i))) out.localBytes += out.bytes[i];
        else                                             out.nonLocalBytes += out.bytes[i];
    }
    out.placed = g_gpuPlaced.load(std::memory_order_relaxed);
}

// ---- Live GPU objects ----
namespace
{
    struct GpuObjects
    {
        std::mutex                                  mutex;
        std::unordered_map<uint64_t, GpuObjectInfo> live;
        uint64_t                                    next = 0;
    };
    // Never destroyed: D3D12 may release an object - and its ledger tag - after
    // static destruction has run.
    GpuObjects& Objects()
    {
        static GpuObjects* s_objects = new GpuObjects;
        return *s_objects;
    }
}

uint64_t RegisterGpuObject(GpuMemCat c, uint64_t bytes, const char* name) noexcept
{
    try
    {
        GpuObjects& o = Objects();
        std::lock_guard<std::mutex> lk(o.mutex);
        const uint64_t id = ++o.next;
        GpuObjectInfo& info = o.live[id];
        info.cat   = c;
        info.bytes = bytes;
        if (name) info.name = name;
        return id;
    }
    catch (...) { return 0; }   // the report misses it; the ledger still counts it
}

void UnregisterGpuObject(uint64_t id) noexcept
{
    if (!id) return;
    try
    {
        GpuObjects& o = Objects();
        std::lock_guard<std::mutex> lk(o.mutex);
        o.live.erase(id);
    }
    catch (...) {}
}

void ReadGpuObjects(std::vector<GpuObjectInfo>& out, size_t n)
{
    out.clear();
    {
        GpuObjects& o = Objects();
        std::lock_guard<std::mutex> lk(o.mutex);
        out.reserve(o.live.size());
        for (const auto& kv : o.live) out.push_back(kv.second);
    }
    std::sort(out.begin(), out.end(), [](const GpuObjectInfo& a, const GpuObjectInfo& b)
              { return a.bytes != b.bytes ? a.bytes > b.bytes : a.name < b.name; });
    if (n && out.size() > n) out.resize(n);
}

void SetGauge(Gauge g, double value) noexcept
{
    const int i = static_cast<int>(g);
    if (i < 0 || i >= kGauges) return;
    g_gauge[i].store(value, std::memory_order_relaxed);
    g_gaugeSet[i].store(true, std::memory_order_release);
}

bool ReadGauge(Gauge g, double& value) noexcept
{
    const int i = static_cast<int>(g);
    if (i < 0 || i >= kGauges || !g_gaugeSet[i].load(std::memory_order_acquire)) return false;
    value = g_gauge[i].load(std::memory_order_relaxed);
    return true;
}

namespace
{
    using Detail::ThreadState;

    inline void Bump(std::atomic<uint64_t>& c, uint64_t by = 1) noexcept
    {
        // Single writer (the owning thread): a plain load+store keeps the lock
        // prefix off the hot path; readers only ever load.
        c.store(c.load(std::memory_order_relaxed) + by, std::memory_order_relaxed);
    }

    // ---- Names ----
    constexpr uint32_t kMaxNames  = 8192;
    constexpr uint32_t kHashSlots = 16384;          // power of two, >= 2x names
    constexpr size_t   kArenaBytes = 512 * 1024;

    struct HashSlot
    {
        std::atomic<uint64_t> key{ 0 };             // FNV-1a of the content; 0 = empty
        std::atomic<uint16_t> id{ 0 };
    };

    std::atomic<const char*> g_names[kMaxNames]{};
    std::atomic<uint32_t>    g_nameCount{ 1 };      // 0 is "<unknown>"
    HashSlot                 g_hash[kHashSlots];
    char                     g_arena[kArenaBytes];
    size_t                   g_arenaUsed = 0;
    SRWLOCK                  g_nameLock = SRWLOCK_INIT;

    uint64_t Fnv1a(const char* s) noexcept
    {
        uint64_t h = 1469598103934665603ull;
        for (; *s; ++s) { h ^= static_cast<unsigned char>(*s); h *= 1099511628211ull; }
        return h ? h : 1ull;
    }

    // ---- Frame records ----
    FrameRecord           g_records[kFrameRing];

    // GPU frames (ProfilerGpu.h). Written on the render half, read by the
    // collector; a lock per copy is fine at one frame per frame.
    GpuFrameData          g_gpuFrames[kGpuRing];
    SRWLOCK               g_gpuLock = SRWLOCK_INIT;
    std::atomic<uint64_t> g_latestGpu{ ~0ull };
    SRWLOCK               g_recordLock = SRWLOCK_INIT;
    std::atomic<uint64_t> g_latestFrame{ ~0ull };
    std::atomic<FrameCompletedFn> g_frameCallback{ nullptr };
    std::atomic<bool>     g_osSampling{ true };

    // Main-thread-only state of the open window.
    struct PrevThread
    {
        // What the windows so far reported per slot, cumulative: the
        // baseline of the next split (SplitThreadWindow only ever adds to it).
        int64_t  reportedNs[kAccCount]{};
        bool     accValid = false;            // a baseline exists (this and the counters below)
        // The thread's last CONSISTENT time state: what a read that found the
        // writer mid-transition on every attempt falls back on.
        ThreadTime lastTime{};
        bool       lastTimeValid = false;
        uint64_t scopes = 0;
        uint64_t allocs = 0, allocBytes = 0, frees = 0;
        uint64_t cpuNs = 0;
        bool     cpuValid = false;
        uint64_t droppedSuspend = 0, orphaned = 0, droppedDeep = 0;
    };
    PrevThread g_prev[kMaxThreads];      // main thread (BeginFrame) only
    PrevThread g_prevOs[kMaxThreads];    // collector thread (SampleOs) only
    bool       g_osPrimed = false;       // collector thread only
    bool       g_windowOpen   = false;
    uint64_t   g_windowFrame  = 0;
    int64_t    g_windowBegin  = 0;
    uint64_t   g_prevCross    = 0;
    uint64_t   g_prevSysIdle = 0, g_prevSysKernel = 0, g_prevSysUser = 0, g_prevProc = 0;
    bool       g_prevSysValid = false;

    std::atomic<double> g_scopeCostNs{ 0.0 };
    std::atomic<bool>   g_allocCounting{ true };
    std::atomic<uint64_t> g_currentFrame{ 0 };
    std::atomic<VideoMemoryQueryFn> g_videoMemoryQuery{ nullptr };
    uint64_t g_prevUnattributedAllocs = 0;   // main thread (BeginFrame) only
    uint32_t g_prevPageFaults = 0;           // collector thread (SampleOs) only
    bool     g_prevPageFaultsValid = false;

    // TSC calibration against the steady clock (QPC). Refined over the whole
    // run, so it is exact to the clock resolution after the first second.
    uint64_t            g_tsc0 = 0;
    int64_t             g_tscNs0 = 0;
    std::atomic<double> g_tscHz{ 0.0 };

    uint64_t FileTimeU64(const FILETIME& f) noexcept
    {
        return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
    }

    size_t RingCapacity(Role role) noexcept
    {
        switch (role)
        {
        case Role::Main:        return size_t(1) << 18;   // 4 MB
        case Role::RenderOwner: return size_t(1) << 17;   // 2 MB
        case Role::Worker:      return size_t(1) << 16;   // 1 MB
        case Role::Other:       return size_t(1) << 14;   // 256 KB
        }
        return size_t(1) << 14;
    }

    ThreadState* ClaimSlot() noexcept
    {
        const uint32_t slot = Detail::g_threadClaimed.fetch_add(1, std::memory_order_acq_rel);
        if (slot >= static_cast<uint32_t>(kMaxThreads))
        {
            Detail::g_unregistered.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        ThreadState& ts = Detail::g_threads[slot];
        ts.index = static_cast<uint16_t>(slot);
        ts.osTid = ::GetCurrentThreadId();
        // Untracked from the moment the thread is known; the first frame
        // boundary after this takes its baseline, so nothing before that counts.
        ts.curSince = NowNs();
        ts.pubSince.store(ts.curSince, std::memory_order_relaxed);
        ULONG_PTR lo = 0, hi = 0;
        ::GetCurrentThreadStackLimits(&lo, &hi);
        ts.stackLo = static_cast<uintptr_t>(lo);
        ts.stackHi = static_cast<uintptr_t>(hi);
        ts.osHandle = ::OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, ts.osTid);
        return &ts;
    }

    void CopyName(ThreadState& ts, const char* name) noexcept
    {
        if (name && *name)
            std::snprintf(ts.name, sizeof(ts.name), "%s", name);
        else
            std::snprintf(ts.name, sizeof(ts.name), "Thread %lu", static_cast<unsigned long>(ts.osTid));
    }

    uint32_t LiveCount() noexcept
    {
        const uint32_t n = Detail::g_threadClaimed.load(std::memory_order_acquire);
        return n < static_cast<uint32_t>(kMaxThreads) ? n : static_cast<uint32_t>(kMaxThreads);
    }

    void CalibrateTsc(int64_t nowNs) noexcept
    {
        const uint64_t tsc = __rdtsc();
        if (!g_tsc0) { g_tsc0 = tsc; g_tscNs0 = nowNs; return; }
        const int64_t dNs = nowNs - g_tscNs0;
        if (dNs < 50'000'000) return;                       // 50 ms before the first estimate
        const double hz = static_cast<double>(tsc - g_tsc0) * 1e9 / static_cast<double>(dNs);
        g_tscHz.store(hz, std::memory_order_relaxed);
    }
} // namespace

// ---- Level ----
void SetLevel(Level level) noexcept
{
    Detail::g_level.store(static_cast<uint8_t>(level), std::memory_order_relaxed);
    Detail::g_allocOn.store(level != Level::Off && g_allocCounting.load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
}

void SetAllocCounting(bool on) noexcept
{
    g_allocCounting.store(on, std::memory_order_relaxed);
    Detail::g_allocOn.store(on && GetLevel() != Level::Off, std::memory_order_relaxed);
}

void SetVideoMemoryQuery(VideoMemoryQueryFn fn) noexcept
{
    g_videoMemoryQuery.store(fn, std::memory_order_release);
}

// ---- Names ----
NameId Intern(const char* name) noexcept
{
    if (!name || !*name) return 0;
    const uint64_t h = Fnv1a(name);
    uint32_t i = static_cast<uint32_t>(h) & (kHashSlots - 1);
    for (uint32_t probe = 0; probe < kHashSlots; ++probe, i = (i + 1) & (kHashSlots - 1))
    {
        const uint64_t k = g_hash[i].key.load(std::memory_order_acquire);
        if (k == h) return g_hash[i].id.load(std::memory_order_acquire);
        if (k == 0) break;
    }

    ::AcquireSRWLockExclusive(&g_nameLock);
    // Re-probe under the lock: inserts only ever happen here, so this answer
    // is final.
    i = static_cast<uint32_t>(h) & (kHashSlots - 1);
    uint32_t empty = kHashSlots;
    for (uint32_t probe = 0; probe < kHashSlots; ++probe, i = (i + 1) & (kHashSlots - 1))
    {
        const uint64_t k = g_hash[i].key.load(std::memory_order_relaxed);
        if (k == h)
        {
            const NameId id = g_hash[i].id.load(std::memory_order_relaxed);
            ::ReleaseSRWLockExclusive(&g_nameLock);
            return id;
        }
        if (k == 0) { empty = i; break; }
    }
    const size_t len = std::strlen(name);
    const uint32_t id = g_nameCount.load(std::memory_order_relaxed);
    if (empty == kHashSlots || id >= kMaxNames || g_arenaUsed + len + 1 > kArenaBytes)
    {
        ::ReleaseSRWLockExclusive(&g_nameLock);
        return 0;                                   // full: reported as "<unknown>"
    }
    char* dst = g_arena + g_arenaUsed;
    std::memcpy(dst, name, len + 1);
    g_arenaUsed += len + 1;
    g_names[id].store(dst, std::memory_order_release);
    g_nameCount.store(id + 1, std::memory_order_release);
    // id BEFORE key: a lock-free reader that sees the key must see the id.
    g_hash[empty].id.store(static_cast<uint16_t>(id), std::memory_order_release);
    g_hash[empty].key.store(h, std::memory_order_release);
    ::ReleaseSRWLockExclusive(&g_nameLock);
    return static_cast<NameId>(id);
}

const char* NameOf(NameId id) noexcept
{
    if (id == 0 || id >= g_nameCount.load(std::memory_order_acquire)) return "<unknown>";
    const char* s = g_names[id].load(std::memory_order_acquire);
    return s ? s : "<unknown>";
}

NameId LabelOf(NameId scope) noexcept
{
    // Indexed by NameId (16 bits): zero-initialised static storage, one
    // relaxed load per submission after the first sight of a scope.
    static std::atomic<NameId> s_label[65536];
    if (!scope) return 0;
    NameId v = s_label[scope].load(std::memory_order_relaxed);
    if (v) return v;
    const char* n = NameOf(scope);
    char buf[192];
    std::snprintf(buf, sizeof(buf), "%s/job", n ? n : "?");
    v = Intern(buf);   // content-interned: a racing thread gets the same id
    s_label[scope].store(v, std::memory_order_relaxed);
    return v;
}

uint32_t NameCount() noexcept { return g_nameCount.load(std::memory_order_acquire); }

// ---- Threads ----
namespace Detail
{
    ThreadState* AttachSlow() noexcept
    {
        if (Tls::state) return Tls::state;
        ThreadState* ts = ClaimSlot();
        if (!ts) return nullptr;
        CopyName(*ts, nullptr);
        ts->role = Role::Other;
        ts->live.store(true, std::memory_order_release);
        Tls::state = ts;
        return ts;
    }

    bool EnsureRing(ThreadState& ts) noexcept
    {
        if (ts.ring) return true;
        const size_t cap = RingCapacity(ts.role);
        void* p = ::VirtualAlloc(nullptr, cap * sizeof(Event), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!p) { Bump(ts.ringFailed); return false; }
        ts.mask = cap - 1;
        ts.ring = static_cast<Event*>(p);   // published by the first head release-store
        return true;
    }
}

void RegisterThread(const char* name, Role role) noexcept
{
    ThreadState* ts = Detail::Tls::state;
    if (!ts)
    {
        ts = ClaimSlot();
        if (!ts) return;
        Detail::Tls::state = ts;
    }
    CopyName(*ts, name);
    ts->role = role;
    ts->live.store(true, std::memory_order_release);

    if (name && *name)
    {
        // SetThreadDescription: the name PIX, Visual Studio and ETW show (Tracy's own SetThreadName does
        // not set it).
        wchar_t wide[48];
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, 48);
        if (n > 0) ::SetThreadDescription(::GetCurrentThread(), wide);
    }
}

uint32_t ThreadCount() noexcept { return LiveCount(); }

bool ReadThreadCounters(uint32_t index, ThreadCounters& out) noexcept
{
    if (index >= LiveCount()) return false;
    const ThreadState& ts = Detail::g_threads[index];
    if (!ts.live.load(std::memory_order_acquire)) return false;
    out.index = ts.index;
    out.role  = ts.role;
    out.osTid = ts.osTid;
    out.name  = ts.name;
    for (int c = 0; c < kAccCount; ++c)
        out.accNs[c] = ts.acc[c].load(std::memory_order_relaxed);
    out.curCat     = ts.pubCat.load(std::memory_order_relaxed);
    out.curSinceNs = ts.pubSince.load(std::memory_order_relaxed);
    out.scopes         = ts.scopes.load(std::memory_order_relaxed);
    out.head           = ts.head.load(std::memory_order_acquire);
    out.capacity       = out.head ? ts.mask + 1 : 0;
    out.droppedDeep    = ts.droppedDeep.load(std::memory_order_relaxed);
    out.droppedSuspend = ts.droppedSuspend.load(std::memory_order_relaxed);
    out.orphaned       = ts.orphaned.load(std::memory_order_relaxed);
    out.ringFailed     = ts.ringFailed.load(std::memory_order_relaxed);
    return true;
}

bool ReadThreadTime(uint32_t index, ThreadTime& out) noexcept
{
    if (index >= LiveCount()) return false;
    const ThreadState& ts = Detail::g_threads[index];
    if (!ts.live.load(std::memory_order_acquire)) return false;
    // Seqlock read. The writer's critical section is a handful of stores: one
    // RUNNING on another core leaves it within nanoseconds, so a few retries
    // catch it. One PREEMPTED inside it stays there for a scheduler quantum,
    // and waiting for it - or yielding main to it - would stall the frame
    // boundary for that long, so after the last attempt the read reports
    // failure and the caller uses its last consistent state. Never a torn
    // read: it can hold a transition's interval in acc[] while still saying
    // the thread is in the outgoing slot since before it - counted twice.
    constexpr int kAttempts = 16;
    for (int attempt = 0; attempt < kAttempts; ++attempt)
    {
        if (attempt) ::YieldProcessor();
        const uint32_t s1 = ts.accSeq.load(std::memory_order_acquire);
        if (s1 & 1u) continue;
        for (int c = 0; c < kAccCount; ++c) out.accNs[c] = ts.acc[c].load(std::memory_order_relaxed);
        out.cat     = ts.pubCat.load(std::memory_order_relaxed);
        out.sinceNs = ts.pubSince.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (ts.accSeq.load(std::memory_order_relaxed) == s1) return true;
    }
    return false;
}

uint64_t CrossThreadDrops() noexcept    { return Detail::g_crossThread.load(std::memory_order_relaxed); }
uint64_t UnregisteredThreads() noexcept { return Detail::g_unregistered.load(std::memory_order_relaxed); }

uint64_t RingHead(uint32_t index) noexcept
{
    if (index >= LiveCount()) return 0;
    return Detail::g_threads[index].head.load(std::memory_order_acquire);
}

uint64_t ReadEvents(uint32_t index, uint64_t from, Event* out, uint64_t maxOut,
                    uint64_t& copied, uint64_t& lost) noexcept
{
    copied = 0; lost = 0;
    if (index >= LiveCount()) return from;
    const ThreadState& ts = Detail::g_threads[index];
    const uint64_t head = ts.head.load(std::memory_order_acquire);
    if (head <= from || !ts.ring) return head > from ? head : from;
    const uint64_t cap = ts.mask + 1;
    uint64_t lo = from;
    if (head - lo > cap) { lost = (head - cap) - lo; lo = head - cap; }
    uint64_t n = head - lo;
    if (n > maxOut) n = maxOut;
    for (uint64_t i = 0; i < n; ++i)
        out[i] = ts.ring[(lo + i) & ts.mask];
    // The writer may have lapped the slots we just copied: anything below
    // head2 - cap can have been overwritten mid-copy. Drop those rather than
    // hand back a torn event.
    const uint64_t head2 = ts.head.load(std::memory_order_acquire);
    const uint64_t validFrom = head2 > cap ? head2 - cap : 0;
    uint64_t bad = 0;
    if (validFrom > lo) bad = (validFrom - lo) < n ? (validFrom - lo) : n;
    if (bad)
    {
        std::memmove(out, out + bad, static_cast<size_t>(n - bad) * sizeof(Event));
        lost += bad;
    }
    copied = n - bad;
    return lo + n;
}

bool ThreadCpuNs(uint32_t index, uint64_t& cpuNs) noexcept
{
    if (index >= LiveCount()) return false;
    const ThreadState& ts = Detail::g_threads[index];
    const double hz = g_tscHz.load(std::memory_order_relaxed);
    if (!ts.osHandle || hz <= 0.0) return false;
    ULONG64 cycles = 0;
    if (!::QueryThreadCycleTime(static_cast<HANDLE>(ts.osHandle), &cycles)) return false;
    cpuNs = static_cast<uint64_t>(static_cast<double>(cycles) * 1e9 / hz);
    return true;
}

double TscHz() noexcept { return g_tscHz.load(std::memory_order_relaxed); }

// ---- Markers ----
void Mark(NameId name) noexcept
{
    if (!Enabled(Level::Timeline)) return;
    ThreadState* ts = Detail::Tls::state;
    if (!ts && !(ts = Detail::AttachSlow())) return;
    if (!ts->ring && !Detail::EnsureRing(*ts)) return;
    Event& e = ts->ring[ts->written & ts->mask];
    e.startNs  = NowNs();
    e.dur100ns = 0;
    e.name     = name;
    e.frameTag = Detail::Tls::frameTag;
    e.catDepth = static_cast<uint8_t>(static_cast<uint8_t>(Cat::Marker) | (ts->depth << 3));
    ++ts->written;
    ts->head.store(ts->written, std::memory_order_release);
}

void Mark(const char* name) noexcept
{
    if (!Enabled(Level::Timeline)) return;
    Mark(Intern(name));
}

// ---- The scope's close ----
void Scope::End() noexcept
{
    const int64_t endNs = NowNs();
    ThreadState* own = m_ts;
    m_ts = nullptr;
    ThreadState* ts = Detail::Tls::state;
    if (ts != own)
    {
        // Resumed on another thread: never touch the other thread's state.
        Detail::CountCrossThreadDrop();
        return;
    }

    int32_t k = static_cast<int32_t>(ts->depth) - 1;
    while (k >= 0 && ts->open[k].seq != m_seq) --k;
    if (k < 0)
    {
        // Already popped as an orphan by an enclosing close: this scope spanned
        // a suspension and its span is not a measurement.
        Bump(ts->droppedSuspend);
        return;
    }

    const uintptr_t self = reinterpret_cast<uintptr_t>(this);
    const bool onStack = self >= ts->stackLo && self < ts->stackHi;
    if (!onStack && m_susp != ts->suspends)
    {
        // Lives in a coroutine frame and this thread suspended a coroutine in between: it spanned its own
        // suspension. Remove just this entry; the ones above it (e.g. the runtime's resume segment) are
        // legitimate.
        const bool wasTop = static_cast<uint32_t>(k) + 1 == ts->depth;
        for (uint32_t j = static_cast<uint32_t>(k); j + 1 < ts->depth; ++j)
            ts->open[j] = ts->open[j + 1];
        --ts->depth;
        if (wasTop)
            Detail::Transition(*ts, endNs, ts->depth ? ts->open[ts->depth - 1].cat
                                                     : static_cast<uint8_t>(kUntracked));
        Bump(ts->droppedSuspend);
        return;
    }

    if (static_cast<uint32_t>(k) + 1 != ts->depth)
        Bump(ts->orphaned, ts->depth - 1 - static_cast<uint32_t>(k));

    int64_t dur = endNs - m_start;
    if (dur < 0) dur = 0;
    ts->depth = static_cast<uint32_t>(k);
    // Whatever was innermost (this scope, or an orphan above it) ends here;
    // the parent - or nothing - is what the thread is doing from now on.
    Detail::Transition(*ts, endNs, k > 0 ? ts->open[k - 1].cat : static_cast<uint8_t>(kUntracked));
    Bump(ts->scopes);

    if (m_record && (ts->ring || Detail::EnsureRing(*ts)))
    {
        uint64_t units = (static_cast<uint64_t>(dur) + 50) / 100;
        if (units > 0xFFFFFFFFull) units = 0xFFFFFFFFull;
        Event& e = ts->ring[ts->written & ts->mask];
        e.startNs  = m_start;
        e.dur100ns = static_cast<uint32_t>(units);
        e.name     = m_id;
        e.frameTag = m_tag;
        e.catDepth = static_cast<uint8_t>(m_cat | (static_cast<uint32_t>(k) << 3));
        ++ts->written;
        ts->head.store(ts->written, std::memory_order_release);
    }
}


// ---- GPU frames ----
void SubmitGpuFrame(const GpuFrameData& d) noexcept
{
    if (!Enabled(Level::Stats)) return;
    ::AcquireSRWLockExclusive(&g_gpuLock);
    g_gpuFrames[d.frame % kGpuRing] = d;
    ::ReleaseSRWLockExclusive(&g_gpuLock);
    const uint64_t prev = g_latestGpu.load(std::memory_order_relaxed);
    if (prev == ~0ull || d.frame > prev) g_latestGpu.store(d.frame, std::memory_order_release);

    ::AcquireSRWLockExclusive(&g_recordLock);
    FrameRecord& r = g_records[d.frame % kFrameRing];
    if (r.valid && r.frame == d.frame)
    {
        r.gpuValid      = true;
        r.gpuCalibrated = d.calibrated;
        r.gpuRegions    = static_cast<uint16_t>(d.regions > 0xFFFFu ? 0xFFFFu : d.regions);
        r.gpuDropped    = static_cast<uint16_t>(d.dropped > 0xFFFFu ? 0xFFFFu : d.dropped);
        r.gpuBusyMs     = d.busyMs;
        for (int q = 0; q < 3; ++q) r.gpuQueueMs[q] = d.queueMs[q];
        r.gpuOverlapMs  = d.overlapMs;
        r.gpuFirstNs    = d.firstNs;
        r.gpuLastNs     = d.lastNs;
        r.gpuSubmitNs   = d.submitNs;
    }
    ::ReleaseSRWLockExclusive(&g_recordLock);
}

bool GetGpuFrame(uint64_t frame, GpuFrameData& out) noexcept
{
    bool ok = false;
    ::AcquireSRWLockShared(&g_gpuLock);
    const GpuFrameData& g = g_gpuFrames[frame % kGpuRing];
    if (g.valid && g.frame == frame && g.spanCount <= kMaxGpuSpans)
    { out = g; ok = true; }
    ::ReleaseSRWLockShared(&g_gpuLock);
    return ok;
}

uint64_t LatestGpuFrame() noexcept { return g_latestGpu.load(std::memory_order_acquire); }

bool PatchGpuDerived(uint64_t frame, const GpuDerivedPatch& patch) noexcept
{
    bool ok = false;
    ::AcquireSRWLockExclusive(&g_recordLock);
    FrameRecord& r = g_records[frame % kFrameRing];
    if (r.valid && r.frame == frame)
    {
        r.gpuDerived         = true;
        r.gpuPeriodMs        = patch.periodMs;
        r.gpuUtil            = patch.util;
        r.gpuStarveMs        = patch.starveMs;
        r.gpuBubbleMs        = patch.bubbleMs;
        r.gpuSubmitToStartMs = patch.submitToStartMs;
        r.gpuSubmitLate      = patch.submitLate;
        r.bound              = patch.bound;
        ok = true;
    }
    ::ReleaseSRWLockExclusive(&g_recordLock);
    return ok;
}

// ---- Frame records ----
void SetOsSampling(bool on) noexcept { g_osSampling.store(on, std::memory_order_relaxed); }
bool OsSampling() noexcept { return g_osSampling.load(std::memory_order_relaxed); }

void SetFrameCompletedCallback(FrameCompletedFn fn) noexcept
{
    g_frameCallback.store(fn, std::memory_order_release);
}

uint64_t LatestRecordedFrame() noexcept { return g_latestFrame.load(std::memory_order_acquire); }

double ScopeCostNs() noexcept { return g_scopeCostNs.load(std::memory_order_relaxed); }

double CalibrateScopeCost(uint32_t iterations) noexcept
{
    if (!iterations || !Enabled(Level::Stats)) return 0.0;
    static NameSlot s_slot{ "Profiler.Calibration" };
    // Warm the slot and the thread state first: the first scope interns the
    // name and may allocate the ring, and neither is a per-scope cost.
    { Scope warm(s_slot, Cat::Work, Level::Stats); }
    const int64_t t0 = NowNs();
    for (uint32_t i = 0; i < iterations; ++i)
    {
        Scope s(s_slot, Cat::Work, Level::Stats);
    }
    const double ns = static_cast<double>(NowNs() - t0) / static_cast<double>(iterations);
    g_scopeCostNs.store(ns, std::memory_order_relaxed);
    return ns;
}

bool GetFrameRecord(uint64_t frame, FrameRecord& out) noexcept
{
    bool ok = false;
    ::AcquireSRWLockShared(&g_recordLock);
    const FrameRecord& r = g_records[frame % kFrameRing];
    if (r.valid && r.frame == frame) { out = r; ok = true; }
    ::ReleaseSRWLockShared(&g_recordLock);
    return ok;
}

bool GetLatestFrameRecord(FrameRecord& out) noexcept
{
    const uint64_t f = g_latestFrame.load(std::memory_order_acquire);
    if (f == ~0ull) return false;
    return GetFrameRecord(f, out);
}

bool PatchTimeline(uint64_t frame, const TimelinePatch& patch) noexcept
{
    bool ok = false;
    ::AcquireSRWLockExclusive(&g_recordLock);
    FrameRecord& r = g_records[frame % kFrameRing];
    if (r.valid && r.frame == frame)
    {
        r.allIdleMs = patch.allIdleMs;
        for (int k = 0; k <= kFrameThreads; ++k) r.concurrencyMs[k] = patch.concurrencyMs[k];
        r.events        = patch.events;
        r.eventsLost    = patch.eventsLost;
        r.timelineValid = true;
        ok = true;
    }
    ::ReleaseSRWLockExclusive(&g_recordLock);
    return ok;
}

uint64_t CurrentFrame() noexcept { return g_currentFrame.load(std::memory_order_acquire); }

namespace
{
    // A thread's time per slot at the window edge `now`: what it published, plus the interval it is in.
    // Signed and not clamped: if the read returned a transition stamped after `now` (only the
    // reordering of two clock reads can, since the edge is taken after the reads), acc[] already holds
    // time past the edge and the negative interval cancels it - the sum over the slots is exactly now
    // minus the thread's registration instant.
    void CumulativeAt(const ThreadTime& t, int64_t now, int64_t (&cumNs)[kAccCount]) noexcept
    {
        for (int c = 0; c < kAccCount; ++c) cumNs[c] = static_cast<int64_t>(t.accNs[c]);
        if (t.cat < kAccCount) cumNs[t.cat] += now - t.sinceNs;
    }
}

void BeginFrame(uint64_t frame) noexcept
{
    Detail::Tls::frameTag = static_cast<uint8_t>(frame);
    g_currentFrame.store(frame, std::memory_order_release);
    const Level level = GetLevel();
    if (level == Level::Off)
    {
        // A window that was open when the profiler was switched off is
        // abandoned, not closed: its counters would describe two frames.
        g_windowOpen = false;
        return;
    }

    // Main-thread cost rule: nothing here may make a syscall per thread. OS CPU sampling
    // (QueryThreadCycleTime per thread, GetSystemTimes) costs ~73 us p50 / ~251 us p99 per frame, about
    // 90% of the profiler's main-thread cost, so it runs on the collector thread (SampleOs), woken at
    // this boundary. What stays here is a seqlock read per thread.
    const int64_t entryNs = NowNs();

    const uint32_t count = LiveCount();
    // Snapshot every thread's counters on main. The time states are read
    // FIRST and the window edge - one instant for all of them - is taken
    // after, so that no interval a read returned starts after the edge.
    struct Snap
    {
        bool       live = false;
        Role       role = Role::Other;
        ThreadTime time{};
        uint64_t   scopes = 0;
        uint64_t   allocs = 0, allocBytes = 0, frees = 0;
        uint64_t   droppedSuspend = 0, orphaned = 0, droppedDeep = 0;
    };
    Snap snap[kMaxThreads];
    uint32_t stale = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        const ThreadState& ts = Detail::g_threads[i];
        if (!ts.live.load(std::memory_order_acquire)) continue;
        Snap& s = snap[i];
        PrevThread& p = g_prev[i];
        if (ReadThreadTime(i, s.time))
        {
            p.lastTime      = s.time;
            p.lastTimeValid = true;
        }
        else if (p.lastTimeValid)
        {
            // The writer stayed inside a transition through every attempt.
            // Its last consistent state still sums to the elapsed time (all of
            // it since then in the slot it was in); the next split corrects
            // the categories.
            s.time = p.lastTime;
            ++stale;
        }
        else continue;   // never read consistently yet: a later record has it
        s.live = true;
        s.role = ts.role;
        s.scopes = ts.scopes.load(std::memory_order_relaxed);
        s.allocs     = ts.allocs.load(std::memory_order_relaxed);
        s.allocBytes = ts.allocBytes.load(std::memory_order_relaxed);
        s.frees      = ts.frees.load(std::memory_order_relaxed);
        s.droppedSuspend = ts.droppedSuspend.load(std::memory_order_relaxed);
        s.orphaned       = ts.orphaned.load(std::memory_order_relaxed);
        s.droppedDeep    = ts.droppedDeep.load(std::memory_order_relaxed);
    }
    // The window edge. A transition the reads above missed - stamped before
    // this instant, published after its thread was read - is counted here in
    // the outgoing slot; the next split takes it back.
    const int64_t now = NowNs();
    const uint64_t cross = Detail::g_crossThread.load(std::memory_order_relaxed);
    const uint64_t unattributed = Detail::g_allocsUnattributed.load(std::memory_order_relaxed);

    uint64_t completed = ~0ull;
    const bool closing = g_windowOpen && now > g_windowBegin;
    if (closing)
    {
        FrameRecord r{};
        r.frame    = g_windowFrame;
        r.beginNs  = g_windowBegin;
        r.endNs    = now;
        r.periodMs = static_cast<float>(static_cast<double>(now - g_windowBegin) * 1e-6);
        r.level    = level;
        r.valid    = true;
        r.osValid  = false;          // SampleOs patches it in
        r.osPoolCpuMs = -1.f;
        const double periodMs = static_cast<double>(r.periodMs);

        double wTotal = 0.0;
        uint32_t pool = 0, workers = 0;
        uint64_t dSusp = 0, dOrph = 0, dDeep = 0;
        int64_t takenBackNs = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            const Snap& s = snap[i];
            if (!s.live) continue;
            PrevThread& p = g_prev[i];
            int64_t cum[kAccCount];
            CumulativeAt(s.time, now, cum);
            if (!p.accValid)   // first sight: all of its time so far
                for (int64_t& v : p.reportedNs) v = 0;
            uint64_t dNs[kAccCount];
            takenBackNs += SplitThreadWindow(cum, s.time.cat, p.reportedNs, dNs);
            double catMs[kAccCount]{};
            for (int c = 0; c < kAccCount; ++c) catMs[c] = static_cast<double>(dNs[c]) * 1e-6;
            const uint32_t scopes = static_cast<uint32_t>(p.accValid ? s.scopes - p.scopes : 0);
            r.totalScopes += scopes;
            const uint32_t allocs = static_cast<uint32_t>(p.accValid ? s.allocs - p.allocs : 0);
            r.allocs     += allocs;
            r.frees      += static_cast<uint32_t>(p.accValid ? s.frees - p.frees : 0);
            r.allocBytes += p.accValid ? s.allocBytes - p.allocBytes : 0;
            if (s.role == Role::Main) r.mainScopes += scopes;
            dSusp += s.droppedSuspend - p.droppedSuspend;
            dOrph += s.orphaned - p.orphaned;
            dDeep += s.droppedDeep - p.droppedDeep;

            const bool poolThread = s.role == Role::Main || s.role == Role::RenderOwner ||
                                    s.role == Role::Worker;
            if (poolThread)
            {
                ++pool;
                wTotal += catMs[static_cast<int>(Cat::Work)];
                r.poolSpinMs += static_cast<float>(catMs[static_cast<int>(Cat::Spin)]);
            }
            const double untracked = catMs[kUntracked];
            switch (s.role)
            {
            case Role::Main:
                r.sMainMs          = static_cast<float>(catMs[static_cast<int>(Cat::Work)]);
                r.mainUntrackedMs  = static_cast<float>(untracked);
                r.mainWaitGpuMs    = static_cast<float>(catMs[static_cast<int>(Cat::WaitGpu)]);
                r.mainWaitJoinMs   = static_cast<float>(catMs[static_cast<int>(Cat::WaitJoin)]);
                r.mainWaitPacingMs = static_cast<float>(catMs[static_cast<int>(Cat::WaitPacing)]);
                break;
            case Role::RenderOwner:
                r.sLaneMs += static_cast<float>(catMs[static_cast<int>(Cat::Work)]);
                break;
            case Role::Worker:
                ++workers;
                r.workerWorkMs += static_cast<float>(catMs[static_cast<int>(Cat::Work)]);
                break;
            default: break;
            }

            if (r.threadCount < kFrameThreads)
            {
                ThreadFrame& t = r.threads[r.threadCount++];
                t.index = static_cast<uint16_t>(i);
                t.role  = s.role;
                for (int c = 0; c < kCatCount; ++c) t.catMs[c] = static_cast<float>(catMs[c]);
                t.untrackedMs = static_cast<float>(untracked);
                t.osCpuMs     = -1.f;
                t.scopes      = scopes;
                t.allocs      = allocs;
            }
        }
        r.poolThreads   = static_cast<uint8_t>(pool);
        r.workerThreads = static_cast<uint8_t>(workers);
        r.wTotalMs      = static_cast<float>(wTotal);
        r.util          = (pool && periodMs > 0.0)
                        ? static_cast<float>(wTotal / (static_cast<double>(pool) * periodMs)) : 0.f;
        r.droppedSuspend = static_cast<uint32_t>(dSusp);
        r.orphaned       = static_cast<uint32_t>(dOrph);
        r.droppedDeep    = static_cast<uint32_t>(dDeep);
        r.crossThread    = static_cast<uint32_t>(cross - g_prevCross);
        r.splitStale       = stale;
        r.splitTakenBackUs = static_cast<float>(static_cast<double>(takenBackNs) * 1e-3);
        r.allocs        += static_cast<uint32_t>(unattributed - g_prevUnattributedAllocs);
        // GPU ledger + gauges: a handful of relaxed loads.
        for (int i = 0; i < kGpuMemCats; ++i)
        {
            r.gpuLedgerBytes[i] = g_gpuBytes[i].load(std::memory_order_relaxed);
            r.gpuLedgerCount[i] = static_cast<int32_t>(g_gpuCount[i].load(std::memory_order_relaxed));
        }
        r.gpuPlacedCount = static_cast<int32_t>(g_gpuPlaced.load(std::memory_order_relaxed));
        r.uploadBytes    = g_uploadBytes.exchange(0, std::memory_order_relaxed);
        for (int i = 0; i < kGauges; ++i)
            r.gauges[i] = g_gaugeSet[i].load(std::memory_order_acquire)
                        ? static_cast<float>(g_gauge[i].load(std::memory_order_relaxed))
                        : std::numeric_limits<float>::quiet_NaN();
        // The profiler's own main-thread cost for this boundary: everything
        // above, measured from entry. (The publish and the callback below are a
        // lock and a notify.)
        r.boundaryUs     = static_cast<float>(static_cast<double>(NowNs() - entryNs) * 1e-3);

        ::AcquireSRWLockExclusive(&g_recordLock);
        g_records[r.frame % kFrameRing] = r;
        ::ReleaseSRWLockExclusive(&g_recordLock);
        g_latestFrame.store(r.frame, std::memory_order_release);
        completed = r.frame;
    }

    // The snapshot becomes the next window's baseline. A closed window's
    // split already moved reportedNs; with none closed (the first boundary,
    // or the level was off) the baseline is the time at this edge.
    for (uint32_t i = 0; i < count; ++i)
    {
        const Snap& s = snap[i];
        if (!s.live) continue;
        PrevThread& p = g_prev[i];
        if (!closing) CumulativeAt(s.time, now, p.reportedNs);
        p.accValid = true;
        p.scopes = s.scopes;
        p.allocs = s.allocs;
        p.allocBytes = s.allocBytes;
        p.frees = s.frees;
        p.droppedSuspend = s.droppedSuspend;
        p.orphaned = s.orphaned;
        p.droppedDeep = s.droppedDeep;
    }
    g_prevCross = cross;
    g_prevUnattributedAllocs = unattributed;
    g_windowOpen  = true;
    g_windowFrame = frame;
    g_windowBegin = now;

    if (completed != ~0ull)
        if (FrameCompletedFn fn = g_frameCallback.load(std::memory_order_acquire))
            fn(completed);
}

// ONE caller: the collector thread, right after it is woken by BeginFrame for
// `frame`. Samples every thread's OS CPU and the machine's load and patches the
// deltas since the previous sample into `frame`'s record. The window it
// measures is therefore the frame window shifted by the collector's wake-up
// latency (tens of microseconds when it is idle; up to one frame's processing
// when it is behind) - per-frame noise, not bias, and a window-level sum is
// exact.
void SampleOs(uint64_t frame) noexcept
{
    if (!g_osSampling.load(std::memory_order_relaxed)) return;
    const int64_t now = NowNs();
    CalibrateTsc(now);

    uint64_t sysIdle = 0, sysKernel = 0, sysUser = 0, proc = 0;
    bool sysValid = false;
    {
        FILETIME idle{}, kernel{}, user{}, c{}, e{}, k{}, u{};
        if (::GetSystemTimes(&idle, &kernel, &user) &&
            ::GetProcessTimes(::GetCurrentProcess(), &c, &e, &k, &u))
        {
            sysIdle = FileTimeU64(idle); sysKernel = FileTimeU64(kernel); sysUser = FileTimeU64(user);
            proc = FileTimeU64(k) + FileTimeU64(u);
            sysValid = true;
        }
    }

    // Process memory and video memory, sampled at the same instant as the CPU
    // times below (all on this thread, none on main).
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    const bool ramValid = ::GetProcessMemoryInfo(::GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)) != 0;
    VideoMemory vm{};
    const VideoMemoryQueryFn vq = g_videoMemoryQuery.load(std::memory_order_acquire);
    const bool vramValid = vq && vq(vm);
    uint32_t pageFaultDelta = 0;
    if (ramValid)
    {
        if (g_prevPageFaultsValid && pmc.PageFaultCount >= g_prevPageFaults)
            pageFaultDelta = pmc.PageFaultCount - g_prevPageFaults;
        g_prevPageFaults = pmc.PageFaultCount;
        g_prevPageFaultsValid = true;
    }

    float cpuMs[kMaxThreads];
    for (float& v : cpuMs) v = -1.f;
    const uint32_t count = LiveCount();
    for (uint32_t i = 0; i < count; ++i)
    {
        uint64_t ns = 0;
        const bool ok = ThreadCpuNs(i, ns);
        PrevThread& p = g_prevOs[i];
        if (ok && p.cpuValid && ns >= p.cpuNs)
            cpuMs[i] = static_cast<float>(static_cast<double>(ns - p.cpuNs) * 1e-6);
        p.cpuNs = ns;
        p.cpuValid = ok;
    }

    uint64_t sysBusy = 0, sysTotal = 0, procBusy = 0;
    bool haveSys = false;
    if (sysValid && g_prevSysValid)
    {
        const uint64_t busyNow  = (sysKernel - sysIdle) + sysUser;
        const uint64_t busyPrev = (g_prevSysKernel - g_prevSysIdle) + g_prevSysUser;
        sysBusy  = busyNow >= busyPrev ? busyNow - busyPrev : 0;
        sysTotal = (sysKernel + sysUser) - (g_prevSysKernel + g_prevSysUser);
        procBusy = proc >= g_prevProc ? proc - g_prevProc : 0;
        haveSys  = true;
    }
    if (sysValid)
    {
        g_prevSysIdle = sysIdle; g_prevSysKernel = sysKernel; g_prevSysUser = sysUser;
        g_prevProc = proc;
        g_prevSysValid = true;
    }

    const bool firstSample = !g_osPrimed;
    g_osPrimed = true;
    if (firstSample) return;   // the first sample is only a baseline

    ::AcquireSRWLockExclusive(&g_recordLock);
    FrameRecord& r = g_records[frame % kFrameRing];
    if (r.valid && r.frame == frame)
    {
        double pool = 0.0;
        bool poolValid = true;
        for (uint8_t t = 0; t < r.threadCount; ++t)
        {
            ThreadFrame& tf = r.threads[t];
            tf.osCpuMs = tf.index < kMaxThreads ? cpuMs[tf.index] : -1.f;
            const bool poolThread = tf.role == Role::Main || tf.role == Role::RenderOwner ||
                                    tf.role == Role::Worker;
            if (poolThread)
            {
                if (tf.osCpuMs >= 0.f) pool += tf.osCpuMs; else poolValid = false;
            }
        }
        r.osPoolCpuMs = poolValid ? static_cast<float>(pool) : -1.f;
        r.osValid = true;
        if (ramValid || vramValid)
        {
            r.memValid = true;
            if (ramValid)
            {
                r.workingSetBytes = pmc.WorkingSetSize;
                r.privateBytes    = pmc.PrivateUsage;
                r.pageFaults      = pageFaultDelta;
            }
            if (vramValid)
            {
                r.vramLocalBytes    = vm.localUsage;
                r.vramLocalBudget   = vm.localBudget;
                r.vramNonLocalBytes = vm.nonLocalUsage;
            }
        }
        if (haveSys)
        {
            r.sysBusy100ns  = sysBusy;
            r.sysTotal100ns = sysTotal;
            r.procBusy100ns = procBusy;
        }
    }
    ::ReleaseSRWLockExclusive(&g_recordLock);
}

} // namespace Diag::Prof

#else // !DIAG_PROFILER: every entry point is an inert stub (Retail)

namespace Diag::Prof
{
    void NoteGpuAlloc(GpuMemCat, uint64_t) noexcept {}
    void NoteGpuFree(GpuMemCat, uint64_t) noexcept {}
    void NoteGpuPlaced(int) noexcept {}
    void NoteUploadBytes(uint64_t) noexcept {}
    void ReadGpuLedger(GpuLedger& out) noexcept { out = GpuLedger{}; }
    uint64_t RegisterGpuObject(GpuMemCat, uint64_t, const char*) noexcept { return 0; }
    void UnregisterGpuObject(uint64_t) noexcept {}
    void ReadGpuObjects(std::vector<GpuObjectInfo>& out, size_t) { out.clear(); }
    void SetGauge(Gauge, double) noexcept {}
    bool ReadGauge(Gauge, double&) noexcept { return false; }
    namespace Detail
    {
        ThreadState* AttachSlow() noexcept { return nullptr; }
        bool EnsureRing(ThreadState&) noexcept { return false; }
        void CountCrossThreadDrop() noexcept {}
    }
    void SetLevel(Level) noexcept {}
    NameId Intern(const char*) noexcept { return 0; }
    const char* NameOf(NameId) noexcept { return "<unknown>"; }
    uint32_t NameCount() noexcept { return 0; }
    void RegisterThread(const char*, Role) noexcept {}
    void Mark(NameId) noexcept {}
    void Mark(const char*) noexcept {}
    uint32_t ThreadCount() noexcept { return 0; }
    bool ReadThreadCounters(uint32_t, ThreadCounters&) noexcept { return false; }
    uint64_t CrossThreadDrops() noexcept { return 0; }
    uint64_t UnregisteredThreads() noexcept { return 0; }
    uint64_t ReadEvents(uint32_t, uint64_t from, Event*, uint64_t, uint64_t& copied, uint64_t& lost) noexcept
    { copied = 0; lost = 0; return from; }
    uint64_t RingHead(uint32_t) noexcept { return 0; }
    bool ThreadCpuNs(uint32_t, uint64_t&) noexcept { return false; }
    double TscHz() noexcept { return 0.0; }
    void SetOsSampling(bool) noexcept {}
    bool OsSampling() noexcept { return false; }
    void SetFrameCompletedCallback(FrameCompletedFn) noexcept {}
    uint64_t LatestRecordedFrame() noexcept { return ~0ull; }
    bool GetFrameRecord(uint64_t, FrameRecord&) noexcept { return false; }
    bool GetLatestFrameRecord(FrameRecord&) noexcept { return false; }
    bool PatchTimeline(uint64_t, const TimelinePatch&) noexcept { return false; }
    void BeginFrame(uint64_t frame) noexcept { Detail::Tls::frameTag = static_cast<uint8_t>(frame); }
    bool ReadThreadTime(uint32_t, ThreadTime&) noexcept { return false; }
    double ScopeCostNs() noexcept { return 0.0; }
    void SampleOs(uint64_t) noexcept {}
    void SetAllocCounting(bool) noexcept {}
    uint64_t CurrentFrame() noexcept { return 0; }
    void SetVideoMemoryQuery(VideoMemoryQueryFn) noexcept {}
    void SubmitGpuFrame(const GpuFrameData&) noexcept {}
    bool GetGpuFrame(uint64_t, GpuFrameData&) noexcept { return false; }
    uint64_t LatestGpuFrame() noexcept { return ~0ull; }
    bool PatchGpuDerived(uint64_t, const GpuDerivedPatch&) noexcept { return false; }
    double CalibrateScopeCost(uint32_t) noexcept { return 0.0; }
}

#endif
