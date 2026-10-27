#pragma once

// OwnerAccounting - owner-active time of the render half.
// One owner thread runs both the game half and every render-half owner segment: graph-overlap
// interleaves, it does not run in parallel. A span from render start to render finish
// (renderPhaseMs, renderThreadMs, drawMs, restMs) therefore swallows the pacing sleep and the next
// frame's game half (about 9.5x inflated under pacing). Packs depend on those fields, so this sits
// beside them.
// It measures time spent executing owner node bodies, summed per render frame rather than per
// bracket: a render frame's continuations run during the next frame's Input / Gameplay / AI /
// Physics pumps and finish in the blocking drain at the Animation boundary, so a bracket-local
// counter would charge frame F's work to F+1. Attribution is by tag:
//   tag = renderFrame + 1          (0 = not a render-half node)
// A TagScope around OwnerTask::Start stamps the root node; NodeScope republishes the executing
// node's tag as the ambient value for the body, and every suspension point in that body submits its
// continuation from the owner thread (OwnerTask::promise_type::Awaiter::await_suspend ->
// Control::Schedule -> OwnerTaskQueue::SubmitImpl), so the whole coroutine chain inherits it.
// Cost: one steady_clock::now() pair per render node; a tag-0 node (the game half's
// FrameSlot.OwnerResume, teardown drains) reads no clock.
// Threading: one writer at a time, the thread owning the render half - main on the serial path, the
// RenderOwnerLane under -renderowner (BeginRenderFrame and every NodeScope -> Charge()) - while main
// calls Read() / DroppedCharges() at the frame-timing publish. So the ring is a seqlock with atomic
// fields:
//   writer: seq -> odd (relaxed), release fence, payload (relaxed), seq -> even (release)
//   reader: seq (acquire), payload (relaxed), acquire fence, seq again - unchanged and even, or the
//           read is refused
// A refused read publishes an absent value like a recycled slot (the gpuValid contract) and counts
// in TornReads(), which -renderownerverify asserts stays 0 from the main side.
// The ambient tag and the blocking-drain depth are thread_local, so a worker that submits to an
// owner queue contributes tag 0 instead of inheriting an unrelated frame. parkHookFires, m_dropped
// and m_torn are cross-thread atomics.
// Header-only and outside Harness/ for the same reason as FrameTiming: the numbers must survive
// into a shipped Game.exe (Engine.Cpp.props excludes src/Harness from Retail).

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace Diag
{
    // Why a frame drain happened. Order is ABI for nothing (only DrainCauseName and the counters index
    // it), but keep Count last and Other at 0 (a default-constructed cause).
    enum class DrainCause : uint8_t
    {
        Other = 0,
        Barrier,        // the Animation / Render-phase barrier, the serial boundary
        RenderHalf,     // RenderHalf's own drains (entry, !allowOverlap, error)
        Pump,           // PumpFrameWork found the frame complete
        EntityDestroy,  // Renderer::OnEntityDestroyed
        WorldClear,     // Renderer::OnWorldClear
        PiPRect,        // Renderer::SetGamePiPRect
        Settings,       // EngineSettings::ApplyTo
        LuaRequest,     // Lua VFX afterimage / DDGI probe watch / ripple
        ResourceOp,     // GraphicsDX12 Create/Destroy on the device-frame thread
        DeviceFlush,    // WaitForPreviousFrame / WaitIdle / FlushAndWait
        Swapchain,      // swap-chain service
        Capture,        // screenshot / capture readback
        HotReload,      // shader hot reload
        AssetWatch,     // editor asset change
        PlayModeEdge,   // Edit <-> Play snapshot / restore
        Harness,        // harness one-shots, file hooks, control channel
        Quit,           // shutdown / run unwind
        Count
    };
    inline const char* DrainCauseName(DrainCause c) noexcept
    {
        static constexpr const char* kNames[] = {
            "other", "barrier", "renderHalf", "pump", "destroy", "worldClear", "pipRect",
            "settings", "lua", "resourceOp", "deviceFlush", "swapchain", "capture",
            "hotReload", "assetWatch", "playEdge", "harness", "quit" };
        static_assert(sizeof(kNames) / sizeof(kNames[0]) == static_cast<size_t>(DrainCause::Count),
                      "DrainCauseName out of step with DrainCause");
        const auto i = static_cast<size_t>(c);
        return i < static_cast<size_t>(DrainCause::Count) ? kNames[i] : "?";
    }

    class OwnerAccounting
    {
    public:
        // Deep enough that a render frame's tail continuations (one frame later, worst case two) are still
        // addressable when the game half publishes them. Power of two: the slot index is a mask in
        // practice, on the per-node path.
        static constexpr size_t kSlots = 8;

        struct Frame
        {
            uint64_t tag      = 0;   // renderFrame + 1; 0 = slot never begun
            double   ownerMs  = 0.0; // summed node-body wall time on the owner
            uint32_t viaPump  = 0;   // continuations executed by PumpFrameWork
            uint32_t viaDrain = 0;   // continuations executed inside a blocking drain
        };

        // Opened around OwnerTask::Start (FramePipeline::RenderHalf / RenderOwnerLoop) so the root node of
        // the render chain is stamped. Nested uses restore, never clear.
        class TagScope
        {
        public:
            explicit TagScope(uint64_t tag) noexcept : m_prev(s_ambient) { s_ambient = tag; }
            ~TagScope() { s_ambient = m_prev; }
            TagScope(const TagScope&) = delete;
            TagScope& operator=(const TagScope&) = delete;
        private:
            uint64_t m_prev;
        };

        // Opened by FramePipeline::DrainFrameWork and the lane drain's blocking wait. Whatever a Pump
        // executes while it is open ran in a blocking drain rather than an opportunistic PumpFrameWork -
        // the split that shows how much overlap window is left.
        class BlockingScope
        {
        public:
            BlockingScope() noexcept { ++s_blocking; }
            ~BlockingScope() { --s_blocking; }
            BlockingScope(const BlockingScope&) = delete;
            BlockingScope& operator=(const BlockingScope&) = delete;
        };

        // ---- Per-node hook ----
        // RAII around one OwnerTaskQueue node body. Publishes the node's tag as ambient (so continuations
        // submitted from inside inherit it) and charges the elapsed wall time to that node's render frame.
        class NodeScope
        {
        public:
            explicit NodeScope(uint64_t tag) noexcept
                : m_tag(tag), m_prev(s_ambient)
            {
                s_ambient = tag;
                if (m_tag) m_start = std::chrono::steady_clock::now();
            }
            ~NodeScope()
            {
                s_ambient = m_prev;
                if (!m_tag) return;
                const double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - m_start).count();
                Get().Charge(m_tag, ms, s_blocking > 0);
            }
            NodeScope(const NodeScope&) = delete;
            NodeScope& operator=(const NodeScope&) = delete;
        private:
            uint64_t m_tag;
            uint64_t m_prev;
            std::chrono::steady_clock::time_point m_start{};
        };

        // Read()'s retry bound: far more than a single-writer 8-slot ring needs.
        static constexpr int kReadAttempts = 3;

        // ---- Drains by cause ----
        // `calls` counts every FramePipeline::DrainFrameWork, `blocking` / `blockingNs` only those that
        // waited for a render frame still in flight (the first drain of a frame collapses its game /
        // render overlap; the rest are cheap). Monotonic; [DRAIN] prints window deltas.
        struct DrainStats
        {
            std::atomic<uint32_t> calls{ 0 };
            std::atomic<uint32_t> blocking{ 0 };
            std::atomic<uint64_t> blockingNs{ 0 };
        };

        // ---- Singleton ----
        static OwnerAccounting& Get() { static OwnerAccounting s_instance; return s_instance; }

        // ---- Ambient tag (owner thread) ----
        static uint64_t AmbientTag() noexcept { return s_ambient; }

        static bool InBlockingDrain() noexcept { return s_blocking > 0; }

        // ---- Per-node hook ----
        // Arms a slot for @p renderFrame. Called once per frame by FramePipeline::RenderHalf (serial path)
        // or RenderOwnerLoop (the -renderowner lane) before the chain starts, so a stale tenant can never
        // be added to.
        void BeginRenderFrame(uint64_t renderFrame) noexcept
        {
            const uint64_t tag = renderFrame + 1;
            Slot& s = m_slots[tag % kSlots];
            const uint64_t seq = s.seq.load(std::memory_order_relaxed);
            s.seq.store(seq + 1, std::memory_order_relaxed);          // odd: writing
            std::atomic_thread_fence(std::memory_order_release);
            s.tag.store(tag, std::memory_order_relaxed);
            s.ownerMs.store(0.0, std::memory_order_relaxed);
            s.viaPump.store(0u, std::memory_order_relaxed);
            s.viaDrain.store(0u, std::memory_order_relaxed);
            s.seq.store(seq + 2, std::memory_order_release);          // even: published
        }

        // Render-owner thread only (main on the serial path, the lane under -renderowner). One writer at a
        // time lets the payload use a plain relaxed load + store instead of read-modify-writes. A charge
        // whose slot a later frame already reused is dropped and counted, never misattributed; the oracle
        // asserts the count stays 0, which is what proves kSlots deep enough.
        void Charge(uint64_t tag, double ms, bool blocking) noexcept
        {
            Slot& s = m_slots[tag % kSlots];
            if (s.tag.load(std::memory_order_relaxed) != tag)
            { m_dropped.fetch_add(1, std::memory_order_relaxed); return; }
            const uint64_t seq = s.seq.load(std::memory_order_relaxed);
            s.seq.store(seq + 1, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_release);
            s.ownerMs.store(s.ownerMs.load(std::memory_order_relaxed) + ms,
                            std::memory_order_relaxed);
            if (blocking) s.viaDrain.store(s.viaDrain.load(std::memory_order_relaxed) + 1u,
                                           std::memory_order_relaxed);
            else          s.viaPump.store(s.viaPump.load(std::memory_order_relaxed) + 1u,
                                          std::memory_order_relaxed);
            s.seq.store(seq + 2, std::memory_order_release);
        }

        // False when the slot was recycled or the seqlock refused the read: the consumer must publish an
        // absent value, never a confident 0.00 (the gpuValid contract). TornReads() tells the cases apart
        // (-renderownerverify asserts it stays 0): a torn read is the main-side symptom of a publication
        // race, an untorn recycled slot means the ring is too shallow.
        // Bounded retries, never a spin: this runs inside the frame-timing bracket on main, and a writer
        // descheduled mid-slot must not stall the game loop.
        bool Read(uint64_t renderFrame, Frame& out) const noexcept
        {
            const uint64_t tag = renderFrame + 1;
            const Slot& s = m_slots[tag % kSlots];
            for (int attempt = 0; attempt < kReadAttempts; ++attempt)
            {
                const uint64_t s1 = s.seq.load(std::memory_order_acquire);
                if (s1 & 1ull) continue;                  // a write is in progress
                Frame f{};
                f.tag      = s.tag.load(std::memory_order_relaxed);
                f.ownerMs  = s.ownerMs.load(std::memory_order_relaxed);
                f.viaPump  = s.viaPump.load(std::memory_order_relaxed);
                f.viaDrain = s.viaDrain.load(std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_acquire);
                if (s.seq.load(std::memory_order_relaxed) != s1) continue;
                if (f.tag != tag) return false;           // recycled: absent, not torn
                out = f;
                return true;
            }
            m_torn.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        uint64_t DroppedCharges() const noexcept
        { return m_dropped.load(std::memory_order_relaxed); }

        // Reads main had to refuse because the slot was mid-write. Expected to stay 0; the counter is the
        // evidence.
        uint64_t TornReads() const noexcept
        { return m_torn.load(std::memory_order_relaxed); }

        // ---- Drains by cause ----
        void NoteDrainCall(DrainCause c) noexcept
        { drains[static_cast<size_t>(c)].calls.fetch_add(1, std::memory_order_relaxed); }
        void NoteDrainBlocked(DrainCause c, uint64_t ns) noexcept
        {
            auto& d = drains[static_cast<size_t>(c)];
            d.blocking.fetch_add(1, std::memory_order_relaxed);
            d.blockingNs.fetch_add(ns, std::memory_order_relaxed);
        }

    private:
        // One seqlock cell per ring slot. `Frame` stays a plain aggregate because it is the API; the
        // storage is atomic.
        struct Slot
        {
            std::atomic<uint64_t> seq{ 0 };      // even = stable, odd = mid-write
            std::atomic<uint64_t> tag{ 0 };
            std::atomic<double>   ownerMs{ 0.0 };
            std::atomic<uint32_t> viaPump{ 0 };
            std::atomic<uint32_t> viaDrain{ 0 };
        };
        static_assert(std::atomic<double>::is_always_lock_free,
            "the accounting ring must not take a lock on the per-node path");

        static inline thread_local uint64_t s_ambient  = 0;
        static inline thread_local int      s_blocking = 0;

    public:
        // Fires of GraphicsDX12::DrainFrameIfNeeded's park hook. Monotonic; the
        // per-frame figure is a delta taken at the CPU bracket close. Atomic
        // because the hook is reachable from resource paths on any thread.
        std::atomic<uint32_t> parkHookFires{ 0 };

        // ---- Drains by cause ----
        // Indexed by DrainCause: every FramePipeline::DrainFrameWork is charged to the reason it was
        // called.
        DrainStats drains[static_cast<size_t>(DrainCause::Count)]{};
        // Carries the cause across the park hook: DrainFrameIfNeeded stores it, the hook reads it and
        // passes it to DrainFrameWork.
        std::atomic<uint8_t> pendingHookCause{ 0 };

    private:
        Slot m_slots[kSlots]{};
        std::atomic<uint64_t> m_dropped{ 0 };
        mutable std::atomic<uint64_t> m_torn{ 0 };
    };
} // namespace Diag
