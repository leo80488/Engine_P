#pragma once

// SystemRegistry: owns every ISystem, partitioned by TickPhase.
//
// Populated once at engine startup by App::RegisterTickSystems: the core's systems in tick order,
// then the enabled engine modules' (their RegisterSystems, called at its end; AddInSetBefore places
// one among the core's). The Scheduler queries it by phase at tick time.

#include "ISystem.h"
#include "SystemSet.h"
#include "System/Check.h"   // ENGINE_CHECK - phase-tag drift is caught in Release too

#include <array>
#include <cassert>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class World;

namespace detail
{
    template <typename T, typename = void>
    struct HasStaticPhase : std::false_type {};

    template <typename T>
    struct HasStaticPhase<T, std::void_t<decltype(T::kPhase)>> : std::true_type {};
}

class SystemRegistry
{
public:
    // Constructs an ISystem in place in `phase`'s bucket and returns it, so a caller that wires
    // follow-up state (event listeners, a cached World pointer) need not look it up.
    // If TSystem derives from SystemInPhase<P>, P must equal @p phase; an ENGINE_CHECK catches drift
    // between the tag and the registration call.
    template <typename TSystem, typename... Args>
    TSystem& Add(TickPhase phase, Args&&... args)
    {
        static_assert(std::is_base_of_v<ISystem, TSystem>,
                      "TSystem must derive from ISystem");
        if constexpr (detail::HasStaticPhase<TSystem>::value)
        {
            // ENGINE_CHECK, not assert: the tag encodes threading guarantees (a serial-phase system
            // registered into a parallel phase races), and assert() is compiled out of Release, the
            // configuration that runs. On a mismatch registration proceeds under the caller's phase, with the
            // system named in the log.
            ENGINE_CHECK(TSystem::kPhase == phase,
                         "SystemRegistry::Add — phase arg disagrees with the "
                         "SystemInPhase<P> tag on '%s'; the tag's threading "
                         "guarantees do NOT apply where it was registered",
                         typeid(TSystem).name());
        }
        auto sys = std::make_unique<TSystem>(std::forward<Args>(args)...);
        TSystem& ref = *sys;
        m_phases[static_cast<size_t>(phase)].push_back(std::move(sys));
        ++m_generation;
        return ref;
    }

    // Overload for systems whose phase is fully baked into the type via
    // SystemInPhase<P>. Call sites read cleaner: Add<FooSystem>(args).
    template <typename TSystem, typename... Args>
        requires detail::HasStaticPhase<TSystem>::value
    TSystem& Add(Args&&... args)
    {
        return Add<TSystem>(TSystem::kPhase, std::forward<Args>(args)...);
    }

    // Construct a statically-phased system and assign its semantic SystemSet
    // in one registration statement. Phase still controls order; the set only
    // controls the independent game-flow gate.
    template <typename TSystem, typename... Args>
        requires detail::HasStaticPhase<TSystem>::value
    TSystem& AddInSet(std::string_view setName, Args&&... args)
    {
        TSystem& system = Add<TSystem>(std::forward<Args>(args)...);
        AssignToSet(system, setName);
        return system;
    }

    // A system no game-flow state may gate: it keeps the engine itself running (the animation chain
    // rotates a GPU slot's upload rings, the timer service ticks real time, the Lua dispatcher runs the
    // scripts that switch states). The Scheduler runs it in every state under every UnsetSystemPolicy,
    // and it is not an "unset" system of the flow's report. Without this a system that declares no set
    // runs only while the policy lets an unset one run (AlwaysRun, the default).
    template <typename TSystem, typename... Args>
        requires detail::HasStaticPhase<TSystem>::value
    TSystem& AddAlwaysRun(Args&&... args)
    {
        TSystem& system = Add<TSystem>(std::forward<Args>(args)...);
        m_alwaysRun.insert(&system);
        ++m_generation;
        return system;
    }
    bool IsAlwaysRun(const ISystem& system) const;

    // AddInSet, placed right before the first system of its phase named @p beforeName instead of
    // at the end: a module's system, registered after the core's, that must run ahead of a core
    // one (the phase's batches follow registration order). Without such a system it stays at the
    // end, and an ENGINE_CHECK names both.
    template <typename TSystem, typename... Args>
        requires detail::HasStaticPhase<TSystem>::value
    TSystem& AddInSetBefore(std::string_view setName, std::string_view beforeName, Args&&... args)
    {
        TSystem& system = AddInSet<TSystem>(setName, std::forward<Args>(args)...);
        MoveBefore(TSystem::kPhase, system, beforeName);
        return system;
    }

    // A system may belong to multiple sets; it runs when ANY membership is
    // active. Duplicate assignment is harmless. Empty names and hash
    // collisions are rejected.
    bool AssignToSet(ISystem& system, std::string_view setName);
    std::span<const SystemSetId> GetSystemSets(const ISystem& system) const;
    const std::unordered_map<SystemSetId, std::string>& GetSystemSetNames() const
    {
        return m_systemSetNames;
    }
    std::vector<const ISystem*> GetSystemsInSet(SystemSetId set) const;

    // OnRegister every system in registration / phase order. Call after
    // World is constructed and before the first Tick.
    void Initialize(World& world);

    // OnUnregister in reverse order. Call before World::Clear during engine shutdown - systems may
    // hold listener handles or cached pool pointers that must detach while the World is alive.
    void Shutdown(World& world);

    // Used by Scheduler::RunPhase. Span hides the underlying vector type
    // and forbids the caller from mutating membership at tick time.
    std::span<const std::unique_ptr<ISystem>> GetSystems(TickPhase phase) const;

    // Composition counter, bumped on every Add and every new set membership. The Scheduler uses it to
    // invalidate cached parallel-batch plans without diffing system lists every frame.
    uint64_t GetGeneration() const { return m_generation; }

private:
    void MoveBefore(TickPhase phase, const ISystem& system, std::string_view beforeName);

    std::array<std::vector<std::unique_ptr<ISystem>>,
               static_cast<size_t>(TickPhase::COUNT)> m_phases;
    std::unordered_set<const ISystem*> m_alwaysRun;
    std::unordered_map<const ISystem*, std::vector<SystemSetId>> m_systemSets;
    std::unordered_map<SystemSetId, std::string> m_systemSetNames;
    uint64_t m_generation = 1;
};
