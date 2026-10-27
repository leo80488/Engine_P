#pragma once

// Diag::Prof - the engine's own profiler.
// A per-thread, lock-free recorder of closed scopes plus a per-thread running total of self time by
// category. It sits beside Tracy, not instead of it: Tracy is compiled only into Profile and needs a
// live connection, while this works in Debug / Release / Profile, in Game.exe and the editor, and
// writes into the harness artifact directory where an agent can read it.
// Levels (runtime, SetLevel):
//   Off       every macro is one relaxed atomic load and a branch.
//   Stats     scopes are timed and folded into per-thread self-time-by-category counters (work /
//             spin / which wait); nothing goes to the event ring. The per-frame CPU utilisation
//             numbers come from this.
//   Timeline  as Stats, and every closed scope is also written to the thread's event ring (16 B),
//             so a window can be exported as a timeline.
//   Detail    as Timeline, plus the fine-grained PROF_SCOPE_D sites.
// Categories: a thread's state at an instant is the category of its innermost open scope; outside
// every scope it is "untracked" (on main a direct measure of instrumentation coverage, on a worker
// idle time). Waits are declared at the instrumentation site, never inferred from a list of names
// afterwards: a list is what somebody forgets to update when adding a wait. Spin is its own
// category: it burns a core without progress, neither work nor an idle wait.
// The coroutine rule: a scope must not span a coroutine suspension - its span would include
// whatever the owner did while the coroutine was parked (the renderPhaseMs trap, 9-18x inflated
// under pacing). The recorder enforces it:
//   * a scope that ends on a different thread than it began on is dropped;
//   * a scope living in a coroutine frame (not on the thread's stack) that observes a suspension
//     of that thread between begin and end is dropped;
//   * scopes left open by a suspended coroutine are popped as orphans when an enclosing stack
//     scope closes.
// Each is counted (DroppedSuspend / Orphaned / CrossThread), so "zero" is measured. Per-segment
// coroutine time is recorded by the runtime (OwnerTask::Control::Resume, CpuTask resume), not by
// scopes inside coroutine bodies.
// Cost: two steady_clock (QPC) reads and a few stores per scope at Stats or above; one load and a
// branch when Off.
// WITH_PROFILER (Engine.Cpp.props) compiles the recorder in. Without it (Retail) every macro is
// nothing and every function an inline no-op, so call sites need no #if of their own.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <new>
#include <tracy/Tracy.hpp>   // PROF_ZONE's Tracy half; a no-op without TRACY_ENABLE

#if defined(WITH_PROFILER) && WITH_PROFILER
#  define DIAG_PROFILER 1
#else
#  define DIAG_PROFILER 0
#endif

namespace Diag::Prof
{
    enum class Cat : uint8_t
    {
        Work       = 0,   // engine work making progress
        Spin       = 1,   // burning CPU while waiting (JoinSpin, pacing tail)
        WaitIdle   = 2,   // nothing to do (a worker with an empty queue)
        WaitJoin   = 3,   // blocked on other CPU threads (fork/join, owner drain)
        WaitGpu    = 4,   // blocked on the GPU (fence, Present)
        WaitPacing = 5,   // the frame limiter, by design
        WaitOther  = 6,   // IO, locks, anything else that blocks
        Marker     = 7,   // zero-duration instant; never "open"

        // Resolved at scope BEGIN, never stored: the enclosing scope's wait
        // category when that scope is itself a wait, else WaitJoin. For the
        // GENERIC blocking primitives (TaskGraph's Completion::Wait, the owner
        // queue's WaitForReady) which are reused under specific waits - the
        // upload-slot GPU wait runs RunUntil, and innermost-wins would
        // otherwise relabel a GPU wait as a CPU join.
        JoinOrParent = 15,
    };
    constexpr int kCatCount = 8;
    // Time accounting slots: the 8 categories plus "untracked" (no scope open).
    constexpr int kUntracked = 8;
    constexpr int kAccCount  = 9;
    const char* CatName(Cat c) noexcept;

    enum class Level : uint8_t { Off = 0, Stats = 1, Timeline = 2, Detail = 3 };
    const char* LevelName(Level l) noexcept;

    enum class Role : uint8_t { Main = 0, RenderOwner = 1, Worker = 2, Other = 3 };
    const char* RoleName(Role r) noexcept;

    using NameId = uint16_t;   // 0 = "<unknown>"

    // One closed scope. Written once, at scope END, into the thread's ring.
    struct Event
    {
        int64_t  startNs;    // steady_clock nanoseconds (QPC-based, same epoch as every engine timer)
        uint32_t dur100ns;   // duration in 100 ns units, saturating at ~429 s
        NameId   name;
        uint8_t  frameTag;   // low 8 bits of the frame the thread was working for
        uint8_t  catDepth;   // category (low 3 bits) | nesting depth << 3
        Cat      Category() const noexcept { return static_cast<Cat>(catDepth & 7u); }
        uint32_t Depth()    const noexcept { return catDepth >> 3; }
        int64_t  EndNs()    const noexcept { return startNs + int64_t(dur100ns) * 100; }
    };
    static_assert(sizeof(Event) == 16, "the ring stores 16-byte events");

    constexpr int kMaxThreads = 64;   // registry slots (a thread that arrives later is not recorded)
    constexpr int kMaxDepth   = 31;   // 5 bits in Event::catDepth

    inline int64_t NowNs() noexcept
    {
        return std::chrono::steady_clock::now().time_since_epoch().count();
    }

    namespace Detail
    {
        struct ThreadState
        {
            struct Open { uint32_t seq; uint8_t cat; uint8_t label; NameId id; };

            // ---- Owner thread only (the hot path) ----
            Event*    ring      = nullptr;   // VirtualAlloc'd on the first ring write
            uint64_t  mask      = 0;         // capacity - 1 (power of two)
            uint64_t  written   = 0;         // writer's private copy of head
            uint32_t  nextSeq   = 0;
            uint32_t  suspends  = 0;         // coroutine suspensions on this thread
            uint32_t  depth     = 0;
            uintptr_t stackLo   = 0, stackHi = 0;

            // One open scope. `id` + `label` make the stack answer "what is this
            // thread working FOR" (CurrentLabel): label 1 = a named work scope,
            // whose tasks are named "<name>/job"; label 2 = such a label already
            // reopened on the thread running the task (its tasks keep the same
            // label); 0 = infrastructure (ParallelFor, chunk / job runners) or a
            // wait, never the name of the work below it.
            Open      open[kMaxDepth + 1]{};
            // What the thread is doing NOW (the innermost open scope's category,
            // kUntracked outside every scope) and since when. Every scope
            // boundary is a transition: the elapsed interval is added to the
            // outgoing category. That makes acc[] an exact time integral, and
            // lets the frame boundary split an interval that is still OPEN (a
            // worker's long idle wait) instead of charging it to whichever frame
            // the scope happens to close in.
            uint8_t   curCat    = static_cast<uint8_t>(kUntracked);
            int64_t   curSince  = 0;

            // ---- Published: one writer (the owner), any reader ----
            alignas(64) std::atomic<uint64_t> head{0};                 // events written
            // Seqlock over acc[] + pubCat/pubSince (odd = mid-write).
            std::atomic<uint32_t> accSeq{0};
            std::atomic<uint64_t> acc[kAccCount]{};                    // ns per innermost category
            std::atomic<uint8_t>  pubCat{ static_cast<uint8_t>(kUntracked) };
            std::atomic<int64_t>  pubSince{0};
            std::atomic<uint64_t> scopes{0};                           // scopes closed
            // Heap traffic of THIS thread (operator new/delete, NoteAlloc/NoteFree).
            // Each thread writes only its own counters; a free on another thread
            // than the allocation is that other thread's free.
            std::atomic<uint64_t> allocs{0};
            std::atomic<uint64_t> allocBytes{0};
            std::atomic<uint64_t> frees{0};
            std::atomic<uint64_t> droppedDeep{0};                      // > kMaxDepth
            std::atomic<uint64_t> droppedSuspend{0};                   // spanned a suspension
            std::atomic<uint64_t> orphaned{0};                         // popped by an outer close
            std::atomic<uint64_t> ringFailed{0};                       // ring allocation failed

            // ---- Identity: written before `live` is released, read after ----
            char      name[48]{};
            uint32_t  osTid    = 0;
            Role      role     = Role::Other;
            uint16_t  index    = 0;
            void*     osHandle = nullptr;    // HANDLE for QueryThreadCycleTime (may be null)
            std::atomic<bool> live{false};
        };

        struct Tls
        {
            static inline thread_local ThreadState* state    = nullptr;
            static inline thread_local uint8_t      frameTag = 0;
        };

        extern std::atomic<uint8_t> g_level;
        // Level >= Stats AND allocation counting not switched off (-profile_alloc=0).
        // One load on the operator-new hot path decides everything.
        extern std::atomic<bool>     g_allocOn;
        extern std::atomic<uint64_t> g_allocsUnattributed;   // threads with no registry slot

        ThreadState* AttachSlow() noexcept;            // register an unnamed thread
        bool EnsureRing(ThreadState& ts) noexcept;     // allocate the ring on first use
        void CountCrossThreadDrop() noexcept;

        // Owner thread only: close the current category's interval at @p t and
        // switch to @p next. Seqlock-published (the stores are plain MOVs on
        // x86; the fences only stop the compiler reordering them).
        inline void Transition(ThreadState& ts, int64_t t, uint8_t next) noexcept
        {
            const uint32_t s = ts.accSeq.load(std::memory_order_relaxed);
            ts.accSeq.store(s + 1, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_release);
            const int64_t d = t - ts.curSince;
            if (d > 0)
            {
                std::atomic<uint64_t>& a = ts.acc[ts.curCat];
                a.store(a.load(std::memory_order_relaxed) + static_cast<uint64_t>(d),
                        std::memory_order_relaxed);
            }
            ts.curCat   = next;
            ts.curSince = t;
            ts.pubCat.store(next, std::memory_order_relaxed);
            ts.pubSince.store(t, std::memory_order_relaxed);
            ts.accSeq.store(s + 2, std::memory_order_release);
        }
    }

    // ---- Level ----
    inline Level GetLevel() noexcept
    { return static_cast<Level>(Detail::g_level.load(std::memory_order_relaxed)); }
    inline bool  Enabled(Level need = Level::Stats) noexcept
    { return static_cast<uint8_t>(need) <= Detail::g_level.load(std::memory_order_relaxed); }
    void SetLevel(Level level) noexcept;

    // ---- Names ----
    // Content-interned (two different pointers to "Lighting" share one id) and thread-safe. The first
    // sight of a name takes a lock and copies it into a static arena; every later lookup is lock-free.
    // Never allocates on the CRT heap, so a unit test that counts operator new is not disturbed.
    NameId      Intern(const char* name) noexcept;
    const char* NameOf(NameId id) noexcept;
    uint32_t    NameCount() noexcept;

    // Constant-initialised per-site cache for a literal name: no magic-static
    // guard on the hot path, and no interning at all until a level is on.
    struct NameSlot
    {
        const char*         name;
        std::atomic<NameId> id{0};
        constexpr explicit NameSlot(const char* n) noexcept : name(n) {}
        NameId Get() noexcept
        {
            NameId v = id.load(std::memory_order_relaxed);
            if (!v) { v = Intern(name); id.store(v, std::memory_order_relaxed); }
            return v;
        }
    };

    // ---- Threads ----
    // Name the calling thread (also SetThreadDescription, so PIX / Visual Studio / a Perfetto export
    // show it). Safe to call more than once; the last name wins. A thread that records without
    // registering is attached automatically as Role::Other, "Thread <tid>".
    void RegisterThread(const char* name, Role role) noexcept;

    // The frame the calling thread is working for. Carried into every event it
    // records (low 8 bits). Main sets it at the loop top, the render-owner lane
    // per packet, workers inherit it from whoever queued the task.
    inline void    SetFrameTag(uint64_t frame) noexcept { Detail::Tls::frameTag = static_cast<uint8_t>(frame); }
    inline uint8_t FrameTag() noexcept { return Detail::Tls::frameTag; }
    inline void    SetFrameTagByte(uint8_t tag) noexcept { Detail::Tls::frameTag = tag; }

    // Called by the coroutine runtime whenever a frame suspends on this thread
    // (TaskHandle::Awaiter / OwnerTask::Awaiter). Feeds the coroutine rule.
    inline void NoteSuspend() noexcept
    {
#if DIAG_PROFILER
        if (Detail::ThreadState* ts = Detail::Tls::state) ++ts->suspends;
#endif
    }

    // Heap traffic, called from the global operator new/delete replacement
    // (src/System/TracyMemory.cpp). Must never allocate, never take a lock and
    // never register a thread: it runs inside operator new.
    inline void NoteAlloc(size_t bytes) noexcept
    {
#if DIAG_PROFILER
        if (!Detail::g_allocOn.load(std::memory_order_relaxed)) return;
        if (Detail::ThreadState* ts = Detail::Tls::state)
        {
            ts->allocs.store(ts->allocs.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
            ts->allocBytes.store(ts->allocBytes.load(std::memory_order_relaxed) + bytes,
                                 std::memory_order_relaxed);
        }
        else Detail::g_allocsUnattributed.fetch_add(1, std::memory_order_relaxed);
#else
        (void)bytes;
#endif
    }
    inline void NoteFree() noexcept
    {
#if DIAG_PROFILER
        if (!Detail::g_allocOn.load(std::memory_order_relaxed)) return;
        if (Detail::ThreadState* ts = Detail::Tls::state)
            ts->frees.store(ts->frees.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
#endif
    }
    // -profile_alloc=0 switches the counting off (the replacement stays and
    // forwards to the CRT). On by default whenever the level is on.
    void SetAllocCounting(bool on) noexcept;

    // Zero-duration instant on the calling thread's timeline (Timeline+).
    void Mark(NameId name) noexcept;
    void Mark(const char* name) noexcept;

    // ---- The scope ----
    // Tag: an infrastructure scope (TaskSystem.ParallelFor, TaskGraph.Job, the chunk runners,
    // TaskWorker.High) that must never become the label of the work submitted beneath it - see
    // CurrentLabel.
    struct Generic {};
    // An already-interned name (a task label carried across threads).
    struct ById { NameId id; };

    class Scope
    {
    public:
        Scope(NameSlot& slot, Cat cat, Level need) noexcept
        {
#if DIAG_PROFILER
            if (static_cast<uint8_t>(need) <= Detail::g_level.load(std::memory_order_relaxed))
                Begin(slot.Get(), cat, cat == Cat::Work ? 1 : 0);
#else
            (void)slot; (void)cat; (void)need;
#endif
        }
        Scope(NameSlot& slot, Cat cat, Level need, Generic) noexcept
        {
#if DIAG_PROFILER
            if (static_cast<uint8_t>(need) <= Detail::g_level.load(std::memory_order_relaxed))
                Begin(slot.Get(), cat, 0);
#else
            (void)slot; (void)cat; (void)need;
#endif
        }
        // A task label reopened on the thread that runs the task. 0 = none.
        Scope(ById named, Cat cat, Level need) noexcept
        {
#if DIAG_PROFILER
            if (named.id &&
                static_cast<uint8_t>(need) <= Detail::g_level.load(std::memory_order_relaxed))
                Begin(named.id, cat, cat == Cat::Work ? 2 : 0);
#else
            (void)named; (void)cat; (void)need;
#endif
        }
        // Runtime name (a system, a render pass). Interned by content, so a
        // recycled std::string buffer can never be mistaken for another name.
        Scope(const char* dynamicName, Cat cat, Level need) noexcept
        {
#if DIAG_PROFILER
            if (dynamicName &&
                static_cast<uint8_t>(need) <= Detail::g_level.load(std::memory_order_relaxed))
                Begin(Intern(dynamicName), cat, cat == Cat::Work ? 1 : 0);
#else
            (void)dynamicName; (void)cat; (void)need;
#endif
        }
        ~Scope()
        {
#if DIAG_PROFILER
            if (m_ts) End();
#endif
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
#if DIAG_PROFILER
        void Begin(NameId id, Cat cat, uint8_t label) noexcept
        {
            Detail::ThreadState* ts = Detail::Tls::state;
            if (!ts && !(ts = Detail::AttachSlow())) return;
            if (ts->depth >= static_cast<uint32_t>(kMaxDepth))
            {
                ts->droppedDeep.store(ts->droppedDeep.load(std::memory_order_relaxed) + 1,
                                      std::memory_order_relaxed);
                return;
            }
            m_record = static_cast<uint8_t>(Level::Timeline) <=
                       Detail::g_level.load(std::memory_order_relaxed);
            uint8_t c = static_cast<uint8_t>(cat);
            if (cat == Cat::JoinOrParent)
            {
                c = static_cast<uint8_t>(Cat::WaitJoin);
                if (ts->depth)
                {
                    const uint8_t parent = ts->open[ts->depth - 1].cat;
                    if (parent >= static_cast<uint8_t>(Cat::WaitIdle) &&
                        parent <= static_cast<uint8_t>(Cat::WaitOther))
                        c = parent;
                }
            }
            const int64_t t = NowNs();
            Detail::Transition(*ts, t, c);
            m_seq  = ++ts->nextSeq;
            ts->open[ts->depth++] = { m_seq, c, label, id };
            m_ts   = ts;
            m_id   = id;
            m_cat  = c;
            m_tag  = Detail::Tls::frameTag;
            m_susp = ts->suspends;
            m_start = t;
        }
        void End() noexcept;     // Profiler.cpp

        Detail::ThreadState* m_ts = nullptr;
        int64_t  m_start  = 0;
        uint32_t m_seq    = 0;
        uint32_t m_susp   = 0;
        NameId   m_id     = 0;
        uint8_t  m_cat    = 0;
        uint8_t  m_tag    = 0;
        bool     m_record = false;
#endif
    };

    // Sequential stages inside one function whose locals cross the stage boundaries
    // (Renderer::RenderAsync's RenderCPU.* marks): Next() closes the current stage and opens the next
    // one in place. Close() ends the chain; call it before a co_await if the stages live in a
    // coroutine, or the open stage is dropped by the coroutine rule (and counted).
    class StageChain
    {
    public:
        StageChain() = default;
        void Next(NameSlot& slot) noexcept
        {
#if DIAG_PROFILER
            Close();
            m_scope = new (m_storage) Scope(slot, Cat::Work, Level::Stats);
#else
            (void)slot;
#endif
        }
        void Close() noexcept
        {
#if DIAG_PROFILER
            if (m_scope) { m_scope->~Scope(); m_scope = nullptr; }
#endif
        }
        ~StageChain() { Close(); }
        StageChain(const StageChain&) = delete;
        StageChain& operator=(const StageChain&) = delete;
    private:
#if DIAG_PROFILER
        alignas(Scope) unsigned char m_storage[sizeof(Scope)];
        Scope* m_scope = nullptr;
#endif
    };

    // "<name>/job" for a scope name, interned once (lock-free after the first
    // sight). A task label is a DIFFERENT name from the scope it came from on
    // purpose: work a system spawned onto the pool is not that system's own
    // scope, and anything that sums a name across threads (the CPU parity
    // check against the Scheduler's timers) must not see the two as one.
    NameId LabelOf(NameId scope) noexcept;

    // The label for work submitted from the calling thread: "<innermost open
    // named work scope>/job", or the reopened label itself when the thread is
    // already running a labelled task. ParallelFor, SubmitCpu and
    // CpuTask::Start capture it on the submitting thread and reopen it inside
    // the worker's generic chunk / job scope (PROF_LABEL), so a worker's time
    // reads "Anim.Chain/job" instead of "TaskGraph.Job". Generic scopes and
    // waits are skipped; 0 below Timeline (only the timeline keeps names) or
    // outside every named scope. Owner thread only.
    inline NameId CurrentLabel() noexcept
    {
#if DIAG_PROFILER
        if (static_cast<uint8_t>(Level::Timeline) > Detail::g_level.load(std::memory_order_relaxed))
            return 0;
        const Detail::ThreadState* ts = Detail::Tls::state;
        if (!ts) return 0;
        for (uint32_t k = ts->depth; k-- > 0;)
        {
            if (ts->open[k].label == 2) return ts->open[k].id;
            if (ts->open[k].label == 1) return LabelOf(ts->open[k].id);
        }
#endif
        return 0;
    }

    // ---- Per-thread counters (the Stats-level data) ----
    // Cumulative, monotonic; the frame boundary (BeginFrame) differences them.
    struct ThreadCounters
    {
        uint16_t index = 0;
        Role     role  = Role::Other;
        uint32_t osTid = 0;
        const char* name = "";
        // Time per innermost category up to the thread's LAST transition, plus
        // the interval still open (curCat since curSinceNs). Field by field,
        // NOT one consistent state: time accounting uses ReadThreadTime below.
        uint64_t accNs[kAccCount]{};
        uint8_t  curCat = static_cast<uint8_t>(kUntracked);
        int64_t  curSinceNs = 0;
        uint64_t scopes = 0;
        uint64_t head   = 0;           // events written to the ring
        uint64_t capacity = 0;         // ring capacity (0 = no ring yet)
        uint64_t droppedDeep = 0, droppedSuspend = 0, orphaned = 0, ringFailed = 0;
    };
    uint32_t ThreadCount() noexcept;
    bool     ReadThreadCounters(uint32_t index, ThreadCounters& out) noexcept;

    // A thread's published time state: per accounting slot up to its LAST
    // transition, and the slot it has been in since. Its time per slot at an
    // instant t is accNs plus (t - sinceNs) in slot `cat`.
    struct ThreadTime
    {
        uint64_t accNs[kAccCount]{};
        uint8_t  cat     = static_cast<uint8_t>(kUntracked);
        int64_t  sinceNs = 0;
    };
    // Consistent (seqlock) read. false if the thread is not live, or if its
    // writer was inside a transition on every attempt - preempted in the
    // middle of its few stores - and `out` is then unspecified: a torn read is
    // never handed out. Take the instant you evaluate it at AFTER the read,
    // so that it is not earlier than the sinceNs the read returned.
    bool     ReadThreadTime(uint32_t index, ThreadTime& out) noexcept;
    uint64_t CrossThreadDrops() noexcept;
    uint64_t UnregisteredThreads() noexcept;   // threads that found the registry full

    // Copy the still-valid events of thread @p index with sequence numbers in [from, head) into @p out
    // (appended). Returns the new cursor (= head at the time of the copy). @p lost receives how many
    // requested events the ring had already overwritten: the reader fell behind.
    uint64_t ReadEvents(uint32_t index, uint64_t from, Event* out, uint64_t maxOut,
                        uint64_t& copied, uint64_t& lost) noexcept;
    uint64_t RingHead(uint32_t index) noexcept;

    // OS-scheduled CPU of a registered thread (QueryThreadCycleTime, converted
    // with the TSC frequency calibrated against QPC). false when unavailable.
    bool ThreadCpuNs(uint32_t index, uint64_t& cpuNs) noexcept;
} // namespace Diag::Prof

// ==== Instrumentation macros ====
// Engine-only scopes: they add no Tracy zone. Put them beside an existing ZoneScopedN, never
// instead of one, while a ZoneValue may still refer to it. PROF_ZONE / PROF_ZONE_WAIT add both (a
// uniquely named Tracy zone + the engine scope) for new sites.
#define DIAG_PROF_CAT2(a, b) a##b
#define DIAG_PROF_CAT1(a, b) DIAG_PROF_CAT2(a, b)
#define DIAG_PROF_UNIQ(base) DIAG_PROF_CAT1(base, __LINE__)

#if DIAG_PROFILER
#  define DIAG_PROF_SCOPE_EX(nameLit, cat, lvl)                                            \
      static ::Diag::Prof::NameSlot DIAG_PROF_UNIQ(_diagProfSlot_){ nameLit };              \
      ::Diag::Prof::Scope DIAG_PROF_UNIQ(_diagProfScope_)(DIAG_PROF_UNIQ(_diagProfSlot_), cat, lvl)
// Infrastructure scope: never the label of the work submitted beneath it.
#  define PROF_SCOPE_GENERIC(nameLit)                                                       \
      static ::Diag::Prof::NameSlot DIAG_PROF_UNIQ(_diagProfSlot_){ nameLit };              \
      ::Diag::Prof::Scope DIAG_PROF_UNIQ(_diagProfScope_)(DIAG_PROF_UNIQ(_diagProfSlot_),   \
          ::Diag::Prof::Cat::Work, ::Diag::Prof::Level::Stats, ::Diag::Prof::Generic{})
// Reopen a captured task label (a NameId, 0 = nothing) as the innermost scope.
#  define PROF_LABEL(idExpr)                                                                \
      ::Diag::Prof::Scope DIAG_PROF_UNIQ(_diagProfLabel_)(::Diag::Prof::ById{ (idExpr) },    \
          ::Diag::Prof::Cat::Work, ::Diag::Prof::Level::Timeline)
#  define PROF_SCOPE_DYN(nameExpr)                                                          \
      ::Diag::Prof::Scope DIAG_PROF_UNIQ(_diagProfScope_)((nameExpr),                        \
          ::Diag::Prof::Cat::Work, ::Diag::Prof::Level::Stats)
#  define PROF_WAIT_DYN(catName, nameExpr)                                                  \
      ::Diag::Prof::Scope DIAG_PROF_UNIQ(_diagProfScope_)((nameExpr),                        \
          ::Diag::Prof::Cat::catName, ::Diag::Prof::Level::Stats)
#else
#  define DIAG_PROF_SCOPE_EX(nameLit, cat, lvl) ((void)0)
#  define PROF_SCOPE_GENERIC(nameLit)          ((void)0)
#  define PROF_LABEL(idExpr)                   ((void)0)
#  define PROF_SCOPE_DYN(nameExpr)             ((void)0)
#  define PROF_WAIT_DYN(catName, nameExpr)     ((void)0)
#endif

#define PROF_SCOPE(nameLit)          DIAG_PROF_SCOPE_EX(nameLit, ::Diag::Prof::Cat::Work, ::Diag::Prof::Level::Stats)
#define PROF_SCOPE_D(nameLit)        DIAG_PROF_SCOPE_EX(nameLit, ::Diag::Prof::Cat::Work, ::Diag::Prof::Level::Detail)
#define PROF_WAIT(catName, nameLit)  DIAG_PROF_SCOPE_EX(nameLit, ::Diag::Prof::Cat::catName, ::Diag::Prof::Level::Stats)
#define PROF_SPIN(nameLit)           DIAG_PROF_SCOPE_EX(nameLit, ::Diag::Prof::Cat::Spin, ::Diag::Prof::Level::Stats)

#define PROF_ZONE(nameLit)                                                                  \
    ZoneNamedN(DIAG_PROF_UNIQ(_diagProfTracy_), nameLit, true);                             \
    PROF_SCOPE(nameLit)
#define PROF_ZONE_WAIT(catName, nameLit)                                                    \
    ZoneNamedN(DIAG_PROF_UNIQ(_diagProfTracy_), nameLit, true);                             \
    PROF_WAIT(catName, nameLit)
