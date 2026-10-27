#pragma once
// Which half of the frame this thread is executing, for the game/render split:
//   Extract - reads AND writes the World to fill the frame packet. Legal.
//   Render  - must never touch the World: the render half can run beside a game half that
//             is mutating pools for the next frame.
// Every World entry point checks it and logs one '[SPLITCHECK][RENDERCTX]' line naming the
// pool; the ThreadCtx::Exempt sites (grep them) are the known render-side reads left.
// Scope restores the previous value (ECS::AccessGuard::Scope clears to null): an Exempt
// sits inside a Render scope and must hand Render back, or the rest of the frame goes
// unchecked.
// Exempt is its own state, not None: reads pass, but CreateEntity/DestroyEntity/
// GetOrCreatePool still report. An Outliner delete inside the ImGui Exempt reaches
// World::DestroyEntity -> Renderer::OnEntityDestroyed -> a write into the live mapped
// upload slot, exactly what the check must catch.
// Header-only (no TU to register in two vcxproj files); in System/ so TaskSystem.h and
// Graphics/ include it without a layering inversion.

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <intrin.h>   // _ReturnAddress - audit-mode de-duplication by caller

namespace Sys::ThreadCtx
{
    enum : uint8_t
    {
        None             = 0,   // game phases, loop top, PostRender - anything goes
        Extract          = 1,   // World reads AND writes are legal here
        Render           = 2,   // no World access at all
        RenderReadExempt = 3,   // a listed render-side World READ awaiting its lane
    };

    inline uint8_t& Slot()
    {
        static thread_local uint8_t t_current = None;
        return t_current;
    }
    // Coroutines may migrate between workers. Keep the TLS lookup behind a
    // call boundary: MSVC /O2 can otherwise retain the old thread's TLS address
    // across co_await (covered by task_graph_test's multi-worker context test).
    __declspec(noinline) inline uint8_t Current() { return Slot(); }

    inline const char* Name(uint8_t v)
    {
        switch (v)
        {
        case Extract:          return "Extract";
        case Render:           return "Render";
        case RenderReadExempt: return "Render(exempt)";
        default:               return "None";
        }
    }

    // Total violations this process. Plotted as "Split.Violations" and printed in the
    // proof-of-life line, so "no hits" is distinguishable from "the tripwire never armed".
    inline std::atomic<uint32_t>& Violations()
    {
        static std::atomic<uint32_t> s_count{ 0 };
        return s_count;
    }

    // How many times an Exempt scope was entered (not how many sites exist: the constructor's
    // static is per-function, not per-caller). The site inventory is the grep.
    inline std::atomic<uint32_t>& ExemptEntries()
    {
        static std::atomic<uint32_t> s_count{ 0 };
        return s_count;
    }

    // DX12_VERIFY_RENDERCTX: 0 = off, 1 = strict Render (default), 2 = also audit
    // structural writes under Extract. Read once; _dupenv_s because getenv is a deprecation
    // error in this project's warning set.
    inline int Level()
    {
        static const int s_level = []() -> int
        {
            char*  buf = nullptr;
            size_t len = 0;
            int    lvl = 1;
            if (_dupenv_s(&buf, &len, "DX12_VERIFY_RENDERCTX") == 0 && buf)
            {
                lvl = std::atoi(buf);
                std::free(buf);
            }
            return lvl;
        }();
        return s_level;
    }

    // RAII, nesting, restores the previous value.
    struct Scope
    {
        uint8_t prev;
        explicit Scope(uint8_t v) : prev(Slot()) { Slot() = v; }
        ~Scope() { Slot() = prev; }
        Scope(const Scope&)            = delete;
        Scope& operator=(const Scope&) = delete;
    };

    // A named, temporary licence for one render-side World READ. `why` should
    // name the packet lane that will replace it, so the grep output doubles as
    // the backlog with its resolution attached.
    struct Exempt
    {
        Scope s;
        explicit Exempt(const char* /*why*/) : s(RenderReadExempt)
        {
            ExemptEntries().fetch_add(1, std::memory_order_relaxed);
        }
    };

    // True when a World pool READ is illegal on this thread right now.
    // Order matters: the thread_local test runs first and Level() only after it failed.
    // World::GetPool runs tens of thousands of times per frame, and Level()'s function-local
    // static carries a thread-safe-init guard; Current() is a plain byte load, None for almost
    // every caller.
    inline bool PoolReadForbidden()
    {
        return Current() == Render && Level() != 0;
    }

    // True when CREATING or DESTROYING entities / pools is illegal right now.
    // Stricter than the read rule: an Exempt does not bless structural change.
    inline bool StructuralForbidden()
    {
        const uint8_t c = Current();
        if (c != Render && c != RenderReadExempt) return false;
        return Level() != 0;
    }

    // Level 2 only: structural mutation from Extract is legal but listed, one line per
    // caller.
    inline bool StructuralAudited()
    {
        return Current() == Extract && Level() >= 2;
    }

    inline void NoteViolation()
    {
        Violations().fetch_add(1, std::memory_order_relaxed);
    }

    // Audit-mode de-duplication by caller, not by call site: ENGINE_CHECK_ONCE's flag belongs
    // to the expanding function, and every structural World mutation funnels through three
    // entry points in ECS.h, so a once-per-site report names one caller and hides the rest.
    // Level 2 wants every calling site, so key on the return address.
    // Fixed capacity, no allocation, no lock: a duplicate under a race costs one extra log
    // line, better than a mutex on an opt-in audit path.
    inline bool FirstTimeFromCaller(const void* ret)
    {
        static constexpr int kMax = 64;
        static const void*   s_seen[kMax]{};
        static std::atomic<int> s_count{ 0 };
        const int n = s_count.load(std::memory_order_relaxed);
        for (int i = 0; i < n && i < kMax; ++i)
            if (s_seen[i] == ret) return false;
        if (n >= kMax) return false;
        s_seen[n] = ret;
        s_count.store(n + 1, std::memory_order_relaxed);
        return true;
    }
}
