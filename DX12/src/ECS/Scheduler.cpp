#include "ECS/Scheduler.h"
#include "ECS/SystemRegistry.h"
#include "ECS/ISystem.h"
#include "ECS/FrameContext.h"
#include "ECS/CommandBuffer.h"
#include "ECS/ECS.h"
#include "ECS/ExclusiveResourceIds.h"   // kEventBus - worker-side grant plumbing
#include "Scripting/ScriptVM.h"          // kScriptVM grant
#include "Gameplay/GameFlowState.h"
#include "System/CommandLine.h"          // -accessguard_inject (harness)
#include "System/EventBus.h"            // ExclusiveGrant - see BatchPlan::busGrant
#include "System/TaskSystem.h"
#include "System/Log.h"
#include "Diagnostics/Profiler.h"
#include "Diagnostics/ProfilerCollector.h"   // -profile_parity feed
#include "Diagnostics/ProfilerFrame.h"       // CurrentFrame

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <algorithm>
#include <cstring>
#include <string>

namespace
{
    // Two systems conflict iff either is opaque, they overlap on a write (W-W), one writes what the
    // other reads (R-W either direction), or they share an ExclusiveResource id. AccessSet lives in
    // Scheduler.h because BatchPlan caches it for the AccessGuard scopes.
    using AccessSet = Scheduler::AccessSet;

    bool Conflict(const AccessSet& a, const AccessSet& b)
    {
        if (a.opaque || b.opaque) return true;
        for (const auto& w : a.writes) if (b.writes.count(w)) return true;
        for (const auto& w : a.writes) if (b.reads.count(w))  return true;
        for (const auto& r : a.reads)  if (b.writes.count(r)) return true;
        for (const auto& e : a.exclusives) if (b.exclusives.count(e)) return true;
        return false;
    }

#ifdef WITH_HARNESS
    struct AccessGuardProbe { int unused = 0; };

    const std::string& AccessGuardInjectTarget()
    {
        // -accessguard_inject=<system name>: that system reads a pool it never declared, so a pack has a
        // real violation for the strict-by-default verdict to catch (and for its opt-out to silence).
        static const std::string s_target = Sys::CommandLine::Get().Get("-accessguard_inject");
        return s_target;
    }
#endif
}

void Scheduler::LogGatingCliffOnce(const std::unordered_set<SystemSetId>& activeSets,
                                   bool unsetRuns)
{
    if (m_gatingCliffLogged || !m_registry) return;
    m_gatingCliffLogged = true;

    // When the first flow state becomes current every gate stops being pass-through at once: systems
    // running since boot go quiet in one frame, and the symptom ("physics stopped") points nowhere near
    // the cause ("that state's sets list does not name GameplaySimulation"). One line per silenced
    // system, once per session; one registry walk, at the first transition only.
    std::vector<const char*> silenced;
    for (size_t p = 0; p < static_cast<size_t>(TickPhase::COUNT); ++p)
    {
        for (const auto& system : m_registry->GetSystems(static_cast<TickPhase>(p)))
        {
            const auto memberships = m_registry->GetSystemSets(*system);
            bool runs = false;
            if (m_registry->IsAlwaysRun(*system))
            {
                runs = true;
            }
            else if (memberships.empty())
            {
                runs = unsetRuns;
            }
            else
            {
                for (SystemSetId set : memberships)
                    if (activeSets.find(set) != activeSets.end()) { runs = true; break; }
            }
            if (!runs) silenced.push_back(system->GetName());
        }
    }

    if (silenced.empty())
    {
        LOG_INFO("GameFlow gating is now ACTIVE; no registered system is gated off");
        return;
    }
    LOG_WARNING("GameFlow gating is now ACTIVE. %zu system(s) that ran during "
                "pass-through are now gated OFF -- if one of them is load-bearing, "
                "its SystemSet is missing from the current state's `sets`:",
                silenced.size());
    for (const char* name : silenced)
        LOG_WARNING("  gated off: %s", name);
}

namespace
{
    // "Phase.<name>": the Tracy zones' convention, distinct from the systems' own scopes. A phase and a
    // system can share a name (PhysicsInterpolation); the prefix keeps them apart in a trace and stops
    // the profiler's CPU parity check double-counting that name.
    const char* PhaseScopeName(TickPhase phase)
    {
        static const auto s_names = [] {
            std::array<std::string, static_cast<size_t>(TickPhase::COUNT)> n;
            for (size_t i = 0; i < n.size(); ++i) n[i] = std::string("Phase.") + kPhaseDescriptors[i].name;
            return n;
        }();
        return s_names[static_cast<size_t>(phase)].c_str();
    }
}

void Scheduler::AddPhaseEdgeHook(TickPhase phase, PhaseEdge edge, PhaseEdgeHook hook)
{
    m_edgeHooks[static_cast<size_t>(phase)][static_cast<size_t>(edge)].push_back(std::move(hook));
}

void Scheduler::RunPhase(TickPhase phase, World& world, FrameContext& ctx)
{
    auto& hooks = m_edgeHooks[static_cast<size_t>(phase)];
    for (const PhaseEdgeHook& hook : hooks[static_cast<size_t>(PhaseEdge::Begin)])
        hook(world, ctx);
    RunPhaseSystems(phase, world, ctx);
    for (const PhaseEdgeHook& hook : hooks[static_cast<size_t>(PhaseEdge::End)])
        hook(world, ctx);
}

void Scheduler::RunPhaseSystems(TickPhase phase, World& world, FrameContext& ctx)
{
    if (!m_registry) return;
    // One engine-profiler scope per phase, named by the phase table, so every
    // RunPhase call site in App is covered without touching it.
    PROF_SCOPE_DYN(PhaseScopeName(phase));

    const size_t  pIdx   = static_cast<size_t>(phase);
    const auto    systems = m_registry->GetSystems(phase);

    if (systems.empty())
    {
        m_phaseLastMs[pIdx] = 0.f;
        m_systemLastMs[pIdx].clear();
        return;
    }

    auto& sysMs = m_systemLastMs[pIdx];
    sysMs.assign(systems.size(), 0.f);

    using clock = std::chrono::steady_clock;
    using ms_f  = std::chrono::duration<float, std::milli>;

    const auto phaseStart = clock::now();

    const PhaseDescriptor& desc = kPhaseDescriptors[pIdx];
    const bool tryParallel = desc.allowsParallel
                          && !desc.requiresMainThread
                          && ctx.jobSystem
                          && systems.size() > 1;

    // Read GameFlow once at the phase edge. The active-set snapshot is immutable
    // for the entire phase, so parallel workers never touch transition state.
    const bool setGatingActive = m_gameFlow && m_gameFlow->GatingActive();
    const std::unordered_set<SystemSetId> activeSets = setGatingActive
        ? m_gameFlow->ActiveSystemSetSnapshot()
        : std::unordered_set<SystemSetId>{};
    // Resolved once per phase for the same reason as the snapshot: a system with
    // no declared set must get the same answer for the whole phase, whatever a
    // Lua chunk loading mid-frame does to the policy.
    const bool unsetRuns = !setGatingActive || m_gameFlow->UnsetSystemRuns();

    // A pause alone (no state) gates too, but is not the cliff that line explains.
    if (setGatingActive && m_gameFlow->HasCurrentState()) LogGatingCliffOnce(activeSets, unsetRuns);

    // Run-mask + SystemSet gates. EngineMode answers editor transport
    // (Edit/Play/Paused); SystemSet answers game-flow semantics. They remain
    // independent and are evaluated once before a system is dispatched.
    const SystemRunMask activeMask = ModeToMask(ctx.mode);

    auto shouldRun = [&](size_t i)
    {
        if (!HasAnyFlag(systems[i]->RunMask(), activeMask)) return false;
        if (!setGatingActive) return true;
        if (m_registry->IsAlwaysRun(*systems[i])) return true;

        const auto memberships = m_registry->GetSystemSets(*systems[i]);
        if (memberships.empty()) return unsetRuns;
        for (SystemSetId set : memberships)
            if (activeSets.find(set) != activeSets.end()) return true;
        return false;
    };

    // Structural prologue: serially, before any system of the phase runs - see
    // ISystem::PrepareStructure.
    for (size_t i = 0; i < systems.size(); ++i)
        if (shouldRun(i)) systems[i]->PrepareStructure(world);

    // Dispatch one already-gated system with timing.
    auto runOne = [&](size_t i)
    {
        // Per-system Tracy CPU zone. This is the single point where a system's Update runs, shared by the
        // serial (main-thread) and parallel (worker) paths, so one zone names every system on whatever
        // thread runs it. No-op when TRACY_ENABLE is undefined.
#ifdef TRACY_ENABLE
        const char* zoneName = systems[i]->GetName();
        // csvexport -u exports source-location names, not ZoneName overrides.
        ZoneTransientN(systemZone, zoneName, true);
#endif
        // Same single dispatch point for the engine profiler: every system, on
        // whatever thread runs it (content-interned, so GetName() may return a
        // std::string buffer).
        PROF_SCOPE_DYN(systems[i]->GetName());
        // Ops this system queues in the CommandBuffer replay in its registration order, whichever thread
        // it runs on.
        CommandBuffer::WriterScope cmdWriter(static_cast<uint32_t>(i) + 1u);
#ifdef WITH_HARNESS
        if (!AccessGuardInjectTarget().empty() && AccessGuardInjectTarget() == systems[i]->GetName())
        {
            static std::atomic<bool> s_announced{ false };
            if (!s_announced.exchange(true))
                LOG_INFO("[ACCESSGUARD] inject: '%s' reads a pool it did not declare", systems[i]->GetName());
            (void)world.GetPool<AccessGuardProbe>();
        }
#endif
        const auto sysStart = clock::now();
        systems[i]->Update(world, ctx);
        sysMs[i] = ms_f(clock::now() - sysStart).count();
        // -profile_parity: this timer is the independent reading the profiler's
        // scope above is checked against (one relaxed load when off).
        if (Diag::Prof::ParityEnabled())
            Diag::Prof::NoteExternalTiming(Diag::Prof::CurrentFrame(), systems[i]->GetName(), sysMs[i]);
    };

    if (!tryParallel)
    {
        // Serial path - every system in registration order.
        for (size_t i = 0; i < systems.size(); ++i)
            if (shouldRun(i)) runOne(i);
    }
    else
    {
        // Build (or reuse cached) per-phase batch plan.
        BatchPlan& plan = m_batchPlans[pIdx];
        const uint64_t curGen = m_registry->GetGeneration();
        if (plan.generation != curGen || plan.batches.empty())
        {
            plan.batches.clear();

            // Built INTO the plan: AccessGuard scopes alias these sets while
            // a batched system executes, so they must outlive the dispatch.
            auto& access = plan.access;
            access.assign(systems.size(), AccessSet{});
            plan.busGrant.assign(systems.size(), 0);
            plan.vmGrant.assign(systems.size(), 0);
            for (size_t i = 0; i < systems.size(); ++i)
            {
                SystemAccessBuilder b;
                systems[i]->DeclareAccess(b);
                access[i].reads      = b.Reads();
                access[i].writes     = b.Writes();
                access[i].exclusives = b.Exclusives();
                access[i].opaque     = b.IsOpaque();
                // kEventBus declarers may touch the bus from a worker; the planner serializes them against other
                // bus users, and the dispatch vouches for the thread with an EventBus::ExclusiveGrant so the bus's
                // thread check holds. A declared (non-opaque) system holding kScriptVM may enter the Lua VM from a
                // worker under a Sys::ScriptVM::Grant (an opaque one runs alone, on main).
                plan.vmGrant[i] = (!access[i].opaque &&
                                   access[i].exclusives.count(ExclusiveRes::kScriptVM)) ? 1 : 0;
                plan.busGrant[i] = (access[i].opaque ||
                                    access[i].exclusives.count(ExclusiveRes::kEventBus)) ? 1 : 0;
            }

            // Linear-greedy: a system joins the current last batch when it conflicts with no one in it, else
            // starts a new batch. Batch order = registration order, which preserves any "A produces what B
            // reads" intent not captured as an R-W declaration.
            for (size_t i = 0; i < systems.size(); ++i)
            {
                bool joined = false;
                if (!plan.batches.empty())
                {
                    bool conflictsWithBatch = false;
                    for (size_t inBatch : plan.batches.back())
                    {
                        if (Conflict(access[i], access[inBatch]))
                        {
                            conflictsWithBatch = true;
                            break;
                        }
                    }
                    if (!conflictsWithBatch)
                    {
                        plan.batches.back().push_back(i);
                        joined = true;
                    }
                }
                if (!joined)
                    plan.batches.push_back({ i });
            }
            plan.generation = curGen;

            // Regression tripwire: opaque systems are the reason a phase
            // degenerates to size-1 batches. Logged once per plan rebuild
            // (registry generation bump), not per frame.
            size_t opaqueCount = 0, maxBatch = 0;
            for (const auto& a : access) if (a.opaque) ++opaqueCount;
            for (const auto& b : plan.batches) maxBatch = (std::max)(maxBatch, b.size());
            LOG_INFO("Scheduler[%s]: %zu systems -> %zu batches (max width %zu, %zu opaque)",
                     desc.name, systems.size(), plan.batches.size(), maxBatch, opaqueCount);
            // The plan itself: which systems share a batch. One line per rebuild, so a declaration change that
            // moves a batch is visible.
            {
                std::string line;
                for (const auto& b : plan.batches)
                {
                    line += '[';
                    for (size_t k = 0; k < b.size(); ++k)
                    {
                        if (k) line += ", ";
                        line += systems[b[k]]->GetName();
                        if (access[b[k]].opaque) line += '*';
                    }
                    line += "] ";
                }
                LOG_INFO("[SCHEDPLAN] phase=%s batches=%zu plan=%s", desc.name,
                         plan.batches.size(), line.c_str());
            }
            if (m_planDump)
            {
                auto names = [](const std::unordered_set<std::type_index>& s)
                {
                    std::vector<std::string> v;
                    for (const auto& t : s)
                    {
                        std::string n = t.name();
                        for (const char* p : { "struct ", "class " })
                            if (n.rfind(p, 0) == 0) n.erase(0, std::strlen(p));
                        v.push_back(std::move(n));
                    }
                    std::sort(v.begin(), v.end());
                    std::string out;
                    for (const auto& n : v) { if (!out.empty()) out += ','; out += n; }
                    return out;
                };
                for (size_t i = 0; i < systems.size(); ++i)
                {
                    std::vector<uint64_t> ex(access[i].exclusives.begin(), access[i].exclusives.end());
                    std::sort(ex.begin(), ex.end());
                    std::string exs;
                    for (uint64_t x : ex) { if (!exs.empty()) exs += ','; exs += std::to_string(x); }
                    LOG_INFO("[SCHEDACCESS] phase=%s idx=%zu sys=%s opaque=%d excl=%s reads=%s writes=%s",
                             desc.name, i, systems[i]->GetName(), access[i].opaque ? 1 : 0,
                             exs.c_str(), names(access[i].reads).c_str(),
                             names(access[i].writes).c_str());
                }
            }
        }

        // Dispatch - one batch at a time, sync between.
        for (const auto& batch : plan.batches)
        {
            ZoneScopedN("Scheduler.Batch");
            ZoneValue(batch.size());
            PROF_SCOPE("Scheduler.Batch");
            // Filter the whole batch before dispatch. This is the near-zero-cost
            // set gate: an inactive gameplay set queues no worker jobs at all.
            std::vector<size_t> runnable;
            runnable.reserve(batch.size());
            for (size_t idx : batch)
                if (shouldRun(idx)) runnable.push_back(idx);

            if (runnable.empty()) continue;
            if (runnable.size() == 1)
            {
                // -accessguard_strict: hold a solo declared system to its declaration too - a later schedule may
                // not run it alone.
                if (ECS::AccessGuard::IsStrict() && runnable[0] < plan.access.size() &&
                    !plan.access[runnable[0]].opaque)
                {
                    const size_t idx = runnable[0];
                    ECS::AccessGuard::Declared guard{};
                    guard.reads      = &plan.access[idx].reads;
                    guard.writes     = &plan.access[idx].writes;
                    guard.opaque     = false;
                    guard.systemName = systems[idx]->GetName();
                    ECS::AccessGuard::Scope accessScope(&guard);
                    runOne(idx);
                }
                else
                    runOne(runnable[0]);
                continue;
            }

            // Parallel batch: push K-1 systems to workers and run one on the calling thread - a caller parked
            // in doneCv.wait() wastes a core plus two queue round-trips, which makes 2-3-system batches slower
            // than serial. Not TaskSystem::WaitAll: it drains every queued task and would sync against
            // unrelated background work (resource loads).
            std::atomic<uint32_t>   remaining(static_cast<uint32_t>(runnable.size() - 1));
            std::mutex              doneMtx;
            std::condition_variable doneCv;
            // First exception escaping any system in this batch. Written under
            // doneMtx (the lock the decrement already takes), so it needs no
            // extra synchronisation and cannot race with the join below.
            std::exception_ptr      firstErr;

            for (size_t k = 1; k < runnable.size(); ++k)
            {
                const size_t idx = runnable[k];
                ctx.jobSystem->Push(
                    [&, idx]()
                    {
                        // A throwing Update() must not skip the decrement below: TaskSystem::WorkerLoop
                        // catches escaped exceptions, so without this try `remaining` never reaches 0 and
                        // the caller parks on doneCv forever. Same shape as TaskSystem::ParallelFor.
                        std::exception_ptr err;
                        try
                        {
                            // DeclareAccess enforced: while this system runs beside its batch-mates,
                            // GetPool checks every touch against the declared sets (AccessGuard.h).
                            ECS::AccessGuard::Declared guard{};
                            if (idx < plan.access.size())
                            {
                                guard.reads      = &plan.access[idx].reads;
                                guard.writes     = &plan.access[idx].writes;
                                guard.opaque     = plan.access[idx].opaque;
                                guard.systemName = systems[idx]->GetName();
                            }
                            else
                                guard.opaque = true;   // no plan info -> do not lie
                            ECS::AccessGuard::Scope accessScope(&guard);

                            std::optional<Sys::ScriptVM::Grant> vmGrant;
                            if (idx < plan.vmGrant.size() && plan.vmGrant[idx]) vmGrant.emplace();
                            if (idx < plan.busGrant.size() && plan.busGrant[idx])
                            {
                                EventBus::ExclusiveGrant grant;
                                runOne(idx);
                            }
                            else
                            {
                                runOne(idx);
                            }
                        }
                        catch (...)
                        {
                            err = std::current_exception();
                        }

                        // Decrement under the lock and notify while still holding it. This is a lifetime
                        // guard, not just a lost-wakeup guard: doneMtx / doneCv / remaining are
                        // stack-local to RunPhase, and the caller may destroy them the instant it sees
                        // remaining == 0. Its matching lock acquisition below blocks until this critical
                        // section has unwound; without that it could free doneMtx while this thread is
                        // still in notify_one() / ~lock_guard. Batches are narrow, so the uncontended
                        // locks are cheap.
                        std::lock_guard<std::mutex> lk(doneMtx);
                        if (err && !firstErr)
                            firstErr = err;
                        if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
                            doneCv.notify_one();
                    },
                    TaskSystem::TaskPriority::High);
            }

            // The calling thread contributes instead of waiting. It must not unwind here either: the workers
            // still reference the stack-local remaining / doneMtx / doneCv, so throwing past the join below
            // would free the mutex under them. Park the exception and rethrow after the join.
            {
                std::exception_ptr err;
                try
                {
                    // The calling thread is a batch member too and runs concurrently with the workers pushed above,
                    // so the same DeclareAccess contract is enforced on it.
                    const size_t idx0 = runnable[0];
                    ECS::AccessGuard::Declared guard{};
                    if (idx0 < plan.access.size())
                    {
                        guard.reads      = &plan.access[idx0].reads;
                        guard.writes     = &plan.access[idx0].writes;
                        guard.opaque     = plan.access[idx0].opaque;
                        guard.systemName = systems[idx0]->GetName();
                    }
                    else
                        guard.opaque = true;
                    ECS::AccessGuard::Scope accessScope(&guard);
                    runOne(idx0);
                }
                catch (...)
                {
                    err = std::current_exception();
                }
                if (err)
                {
                    std::lock_guard<std::mutex> lk(doneMtx);
                    if (!firstErr) firstErr = err;
                }
            }

            // Fast path: batch-mates usually finish while batch[0] runs on this thread. Spin briefly on the
            // atomic to skip a kernel wait in the common case; batches are sub-millisecond.
            {
                ZoneScopedN("Scheduler.JoinSpin");
                PROF_SPIN("Scheduler.JoinSpin");
                for (int spin = 0;
                     remaining.load(std::memory_order_acquire) != 0 && spin < 4096;
                     ++spin)
                {
                    YieldProcessor();
                }
            }
            // Always acquire doneMtx before leaving the batch, even if the spin saw remaining == 0: the last
            // worker decrements and notifies while holding doneMtx, so taking it here waits for that critical
            // section and no worker still references doneMtx / doneCv / remaining when they go out of scope.
            // Do not skip the lock on the fast path: the caller would free the mutex mid-notify. The predicate
            // covers the not-yet-finished case with no lost wakeups.
            {
                ZoneScopedN("Scheduler.JoinWait");
                PROF_WAIT(WaitJoin, "Scheduler.JoinWait");
                std::unique_lock<std::mutex> lk(doneMtx);
                doneCv.wait(lk, [&] {
                    return remaining.load(std::memory_order_acquire) == 0;
                });
            }

            // Everyone has joined and nothing references the stack-locals any
            // more, so it is finally safe to unwind. Unlocked read: every writer
            // stored firstErr under doneMtx BEFORE its decrement, and the wait
            // above re-acquired doneMtx, so those stores happen-before this.
            if (firstErr)
                std::rethrow_exception(firstErr);
        }
    }

    m_phaseLastMs[pIdx] = ms_f(clock::now() - phaseStart).count();

    // Flush deferred structural changes: phase boundaries are the only place AddComponent /
    // RemoveComponent / DestroyEntity may mutate the live World, so a system in phase N+1 sees what
    // phase N published, never a partial mid-iteration state.
    if (ctx.commandBuffer)
        ctx.commandBuffer->Flush(world);
}

std::vector<PhaseDebugInfo> Scheduler::GetDebugInfo() const
{
    std::vector<PhaseDebugInfo> out;
    out.reserve(static_cast<size_t>(TickPhase::COUNT));
    if (!m_registry) return out;

    for (size_t p = 0; p < static_cast<size_t>(TickPhase::COUNT); ++p)
    {
        const TickPhase phase  = static_cast<TickPhase>(p);
        const auto      systems = m_registry->GetSystems(phase);

        PhaseDebugInfo info;
        info.phase       = phase;
        info.name        = kPhaseDescriptors[p].name;
        info.lastFrameMs = m_phaseLastMs[p];
        info.systems.reserve(systems.size());

        const auto& sysMs = m_systemLastMs[p];
        for (size_t i = 0; i < systems.size(); ++i)
        {
            const float ms = (i < sysMs.size()) ? sysMs[i] : 0.f;
            info.systems.push_back({ systems[i]->GetName(), ms });
        }
        out.push_back(std::move(info));
    }
    return out;
}
