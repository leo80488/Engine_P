#pragma once
// Runtime enforcement of DeclareAccess.
//
// The Scheduler batches systems by their DECLARED reads/writes and runs a batch's members
// concurrently on workers. A system touching an undeclared pool there races the pool's owner,
// and the failure (unordered_map / vector mutation under contention) is heap corruption far
// from the cause. The guard sits on World::GetPool<T>, which GetComponent, EnsurePool
// consumers and every pool-cached iteration funnel through, and is compiled in every config,
// like RenderGraph's RG_VALIDATE_ACCESS. EventBus carries its own thread check.
//
// Cost: with no guard active (main thread, outside worker batches) one thread_local load and
// null test per GetPool; inside a worker-batched system one hash-set probe per GetPool, and
// systems cache pool pointers once per tick, so a well-behaved one pays it a few times.
// Header-only, so there is no TU to register in both vcxproj files.

#include "System/Check.h"   // ENGINE_CHECK_ONCE - all-config, log-and-continue
#include "System/ThreadContext.h"

#include <atomic>
#include <cstdint>
#include <typeindex>
#include <unordered_set>

// Render-context validation, compiled in EVERY config for the same reason
// RG_VALIDATE_ACCESS is (RenderGraph.h): the configuration this project
// actually runs is Release, and a check that only exists in Debug does not
// exist. Runtime-disable with DX12_VERIFY_RENDERCTX=0.
#ifndef ECS_VALIDATE_RENDERCTX
#define ECS_VALIDATE_RENDERCTX 1
#endif

namespace ECS::AccessGuard
{
    // What the Scheduler declared for the system currently executing on THIS
    // thread. Pointers alias the Scheduler's cached batch plan, which outlives
    // the dispatch (see Scheduler::BatchPlan::access).
    struct Declared
    {
        const std::unordered_set<std::type_index>* reads      = nullptr;
        const std::unordered_set<std::type_index>* writes     = nullptr;
        const char*                                systemName = "?";
        bool                                       opaque     = false;
    };

    inline const Declared*& Slot()
    {
        static thread_local const Declared* t_current = nullptr;
        return t_current;
    }

    inline const Declared* Current() { return Slot(); }

    // -accessguard_strict: the Scheduler also installs the scope around a DECLARED system that
    // runs alone (a batch of one), not only around concurrent batch members. A declaration is a
    // promise about every schedule, including one that runs a solo system beside others, so
    // strict mode makes an incomplete one fail even in a batch of one.
    inline bool& StrictFlag()
    {
        static bool s_instance = false;
        return s_instance;
    }
    inline void SetStrict(bool on) { StrictFlag() = on; }
    inline bool IsStrict() { return StrictFlag(); }

    // Every violation this process saw, not one per call site (the log line is once per site).
    // run.json publishes it as counters.accessGuardViolations and scenario_run fails a pack on it,
    // whatever the pack's engineErrors bound is.
    inline std::atomic<uint32_t>& Violations()
    {
        static std::atomic<uint32_t> s_count{ 0 };
        return s_count;
    }
    inline void NoteViolation() { Violations().fetch_add(1, std::memory_order_relaxed); }

    // Structural World changes are not expressible as a component read or write.
    // CreateEntity / DestroyEntity touch the entity tables every system reads (a destroy also
    // erases from EVERY pool and fires the listeners), and creating a pool inserts into the map
    // every GetPool probes - so a DECLARED system may not do them at all: route them through the
    // CommandBuffer (flushed at the phase boundary) or stay opaque.
    inline bool StructuralForbidden()
    {
        const Declared* d = Slot();
        return d && !d->opaque;
    }

    // RAII the Scheduler wraps around one system's Update while it runs concurrently with other
    // batch members (worker push + the calling thread's own share), and under
    // -accessguard_strict around a declared system running alone. Otherwise serial execution
    // installs nothing: a system running alone races nobody.
    struct Scope
    {
        explicit Scope(const Declared* d) { Slot() = d; }
        ~Scope() { Slot() = nullptr; }
        Scope(const Scope&)            = delete;
        Scope& operator=(const Scope&) = delete;
    };

    // True when access to pool type `t` is permitted right now: no guard active, an opaque
    // declaration (may touch anything - the planner already serialized it), or the type is in
    // the declared read/write sets.
    inline bool Allows(const std::type_index& t)
    {
        const Declared* d = Slot();
        if (!d || d->opaque) return true;
        return (d->reads  && d->reads->count(t)  != 0)
            || (d->writes && d->writes->count(t) != 0);
    }

    // World::GetPool's check. A violation logs ONE error per (call site, component type) and
    // continues - log-and-continue is the engine's check policy (System/Check.h): the access still
    // races, but the log names the system and the fix instead of the race surfacing as heap
    // corruption frames later with a clean stack.
    template <typename T>
    inline void CheckPoolAccess()
    {
        if (Allows(std::type_index(typeid(T)))) return;
        NoteViolation();
        const Declared* d = Slot();
        ENGINE_CHECK_ONCE(false,
            "System '%s' touched component pool '%s' WITHOUT declaring it — "
            "it is executing inside a PARALLEL batch and this access races "
            "the other members. Fix the system's DeclareAccess (add the "
            "read/write, or OpaqueAccess() if the set is data-driven).",
            d ? d->systemName : "?", typeid(T).name());
    }

    // AddComponent / RemoveComponent change the pool's membership: a WRITE, whatever the
    // system declared about reading it.
    template <typename T>
    inline void CheckPoolWrite()
    {
        const Declared* d = Slot();
        if (!d || d->opaque) return;
        if (d->writes && d->writes->count(std::type_index(typeid(T))) != 0) return;
        NoteViolation();
        ENGINE_CHECK_ONCE(false,
            "System '%s' added/removed a '%s' component WITHOUT declaring a "
            "WRITE of it - a membership change is a write. Fix the system's "
            "DeclareAccess.",
            d->systemName, typeid(T).name());
    }

    // ---- Render-context tripwire (game/render split) ----
    // A TEMPLATE so the ENGINE_CHECK_ONCE flag inside is per component type: the lambda-static in
    // Check.h belongs to the function that expands the macro, so a shared non-template reporter
    // would let the first violating type mask every other one.
    // Reads are permitted under Sys::ThreadCtx::RenderReadExempt (a listed site awaiting its
    // packet lane); structural mutation is not (ECS_SPLITCHECK_STRUCTURAL).
    template <typename T>
    inline void CheckRenderCtxPool()
    {
#if ECS_VALIDATE_RENDERCTX
        if (!Sys::ThreadCtx::PoolReadForbidden()) return;
        Sys::ThreadCtx::NoteViolation();
        ENGINE_CHECK_ONCE(false,
            "[SPLITCHECK][RENDERCTX] component pool '%s' touched from RENDER "
            "context — render-side code must not read the World. Feed it from a "
            "FramePacket lane, or wrap the site in Sys::ThreadCtx::Exempt(\"<lane>\") "
            "if it is a known item on the split backlog.",
            typeid(T).name());
#endif
    }
}

// Structural World mutation (create/destroy an entity or a pool), checked at the CALL SITE
// so each site owns its once-flag (the reason CheckRenderCtxPool is a template). Expanded
// inside ECS.h's entry points. Two levels:
//   forbidden - Render or Render(exempt): a real split violation.
//   audited   - Extract at DX12_VERIFY_RENDERCTX=2: legal, but every hit is a write a
//               FramePacket lane should take over; logged once per calling site.
#if ECS_VALIDATE_RENDERCTX
#define ECS_SPLITCHECK_STRUCTURAL(opName)                                      \
    do {                                                                       \
        if (::Sys::ThreadCtx::StructuralForbidden())                           \
        {                                                                      \
            ::Sys::ThreadCtx::NoteViolation();                                 \
            ENGINE_CHECK_ONCE(false,                                           \
                "[SPLITCHECK][RENDERCTX] World::%s from %s context — the "     \
                "render side must not change World structure; route it through "\
                "the game thread (a destroy lane / deferred command).",        \
                (opName),                                                      \
                ::Sys::ThreadCtx::Name(::Sys::ThreadCtx::Current()));          \
        }                                                                      \
        else if (::Sys::ThreadCtx::StructuralAudited()                         \
              && ::Sys::ThreadCtx::FirstTimeFromCaller(_ReturnAddress()))      \
        {                                                                      \
            /* ENGINE_CHECK, not _ONCE: the once-flag belongs to THIS macro    \
             * expansion, and all of AddComponent/EnsurePool funnel through    \
             * one GetOrCreatePool site - so _ONCE would report a single       \
             * caller and hide the rest, which is the opposite of what an      \
             * inventory is for. FirstTimeFromCaller does the de-duplication   \
             * by return address instead: one line per calling site. */        \
            ENGINE_CHECK(false,                                                \
                "[SPLITCHECK][EXTRACTSTRUCT] World::%s during Extract from "   \
                "caller %p — legal today; listed so the packet milestone can " \
                "relocate it.",                                                \
                (opName), _ReturnAddress());                                   \
        }                                                                      \
    } while (0)
#else
#define ECS_SPLITCHECK_STRUCTURAL(opName) ((void)0)
#endif

// A structural World change from a DECLARED system (see
// AccessGuard::StructuralForbidden). Per call site, like the split checks.
#define ECS_ACCESSGUARD_STRUCTURAL(opName)                                     \
    do {                                                                       \
        if (::ECS::AccessGuard::StructuralForbidden())                         \
        {                                                                      \
            ::ECS::AccessGuard::NoteViolation();                               \
            ENGINE_CHECK_ONCE(false,                                           \
                "[ACCESSGUARD][STRUCT] System '%s' called World::%s - a "      \
                "declared system must not change World structure (it may "     \
                "run beside any other system). Queue it on the "               \
                "CommandBuffer, or make the system opaque.",                   \
                ::ECS::AccessGuard::Current()->systemName, (opName));          \
        }                                                                      \
    } while (0)

// Non-templated World entry points that are reads (IsAlive / GetEntities). Each
// call site gets its own flag, so one violator does not hide the next.
#if ECS_VALIDATE_RENDERCTX
#define ECS_SPLITCHECK_READ(opName)                                            \
    do {                                                                       \
        if (::Sys::ThreadCtx::PoolReadForbidden())                             \
        {                                                                      \
            ::Sys::ThreadCtx::NoteViolation();                                 \
            ENGINE_CHECK_ONCE(false,                                           \
                "[SPLITCHECK][RENDERCTX] World::%s from RENDER context — feed " \
                "it from a FramePacket lane.", (opName));                      \
        }                                                                      \
    } while (0)
#else
#define ECS_SPLITCHECK_READ(opName) ((void)0)
#endif
