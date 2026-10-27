#pragma once

// ISystem - base class for everything the Scheduler ticks.
//
// One ISystem belongs to exactly one TickPhase. The Scheduler calls Update once per frame (or once
// per fixed step in fixed-timestep phases). Systems declare the component types they Read / Write
// via DeclareAccess so the Scheduler can batch them in parallel; a system without a declaration is
// treated as "read+write everything" and serialised.

#include "TickPhase.h"
#include "ECS/EngineMode.h"

#include <typeindex>
#include <unordered_set>

class World;
struct FrameContext;

// Records the component-type access set for one system. Two systems can run concurrently iff
// neither is opaque AND no W-W or R-W overlap exists on any component or exclusive resource.
//
// The default (ISystem::DeclareAccess) is OPAQUE: "I touch arbitrary global state, do not co-batch
// me". To opt INTO parallel batching, override DeclareAccess and call Read/Write (or nothing, if the
// system truly touches no state).
class SystemAccessBuilder
{
public:
    template <typename T>
    SystemAccessBuilder& Read()
    {
        m_reads.insert(std::type_index(typeid(T)));
        return *this;
    }

    template <typename T>
    SystemAccessBuilder& Write()
    {
        m_writes.insert(std::type_index(typeid(T)));
        return *this;
    }

    // For non-component shared state (the Jolt PhysicsSystem singleton, an AudioEngine voice pool, the
    // bindless descriptor heap): the same ID on two systems -> they cannot run concurrently. The caller
    // picks IDs out of a private namespace.
    SystemAccessBuilder& ExclusiveResource(uint64_t resourceId)
    {
        m_exclusives.insert(resourceId);
        return *this;
    }

    // Marks this system as touching arbitrary global state: the Scheduler serialises it against every
    // other system in the same phase. This is the default (ISystem::DeclareAccess), so a system nobody
    // audited stays safe.
    SystemAccessBuilder& OpaqueAccess() { m_opaque = true; return *this; }

    const std::unordered_set<std::type_index>& Reads()      const { return m_reads; }
    const std::unordered_set<std::type_index>& Writes()     const { return m_writes; }
    const std::unordered_set<uint64_t>&        Exclusives() const { return m_exclusives; }
    bool                                       IsOpaque()   const { return m_opaque; }

private:
    std::unordered_set<std::type_index> m_reads;
    std::unordered_set<std::type_index> m_writes;
    std::unordered_set<uint64_t>        m_exclusives;
    bool                                m_opaque = false;
};

class ISystem
{
public:
    virtual ~ISystem() = default;

    // Phase this system lives in. SystemInPhase<P> implements this for you.
    virtual TickPhase   GetPhase() const = 0;

    // Stable identifier for profilers / ImGui phase visualisation.
    virtual const char* GetName()  const = 0;

    // Which engine modes this system runs in. The Scheduler skips the system when this mask does not
    // intersect the active mode's mask (EngineMode.h / ModeToMask). Default Always: render, transform
    // propagation, skinning and the editor camera run regardless of Edit/Play. Simulation systems
    // (physics / AI / script / gameplay / audio) return SystemRunMask::Play so they tick only while the
    // sim advances.
    virtual SystemRunMask RunMask() const { return SystemRunMask::Always; }

    // Called once at startup, in registration order: cache World references, subscribe to the event
    // bus. Do not mutate game state here: the Scheduler has not entered the tick loop yet.
    virtual void OnRegister(World& /*world*/) {}

    // Called once at shutdown, in REVERSE registration order.
    virtual void OnUnregister(World& /*world*/) {}

    // Per-frame work. For systems in fixed-timestep phases this may be
    // called 0 or N times per render frame; FrameContext::isFixedTickPhase
    // is true while inside the catch-up loop.
    virtual void Update(World& world, const FrameContext& ctx) = 0;

    // Structural prologue, called by the Scheduler SERIALLY on the RunPhase thread before any system of
    // the phase runs, for every system that will run this phase. A DECLARED system may not change World
    // structure inside Update (AccessGuard's [ACCESSGUARD][STRUCT] tripwire), but an EMPTY pool its
    // Update would otherwise create on first use (EnsurePool of a declared Write) is not game state:
    // create it here. World::Clear drops pools, so this runs every phase; an existing pool costs one map
    // lookup.
    virtual void PrepareStructure(World& /*world*/) {}

    // Default: marks the system as Opaque so the Scheduler serialises it
    // against every other system in the same phase. Override + call
    // Read/Write to opt INTO parallel batching.
    virtual void DeclareAccess(SystemAccessBuilder& builder) const
    {
        builder.OpaqueAccess();
    }
};

// CRTP-ish tag base: derive from SystemInPhase<TickPhase::X> to bake the phase into the type.
// SystemRegistry::Add ENGINE_CHECKs that an explicit phase argument matches the static one.
template <TickPhase PHASE>
class SystemInPhase : public ISystem
{
public:
    static constexpr TickPhase kPhase = PHASE;
    TickPhase GetPhase() const override final { return PHASE; }
};
