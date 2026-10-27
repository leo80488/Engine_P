// ContactTracker: Jolt's sub-shape contact callbacks -> one Began / Ended per body pair
// (include/Physics/ContactTracker.h says what the events mean).

#include "Physics/ContactTracker.h"
#include "PhysicsInternal.h"   // BodyUserEntity

#include <Jolt/Jolt.h>

JPH_SUPPRESS_WARNINGS

#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/Shape/SubShapeIDPair.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace DX12Physics
{
namespace
{
    // The order within one step and body pair: a sub-shape that arrives is counted before another
    // one leaves (a compound handing a trigger from one part to the next never reaches zero).
    enum class RawKind : std::uint8_t { Added = 0, Removed = 1, BodyGone = 2 };

    struct Raw
    {
        std::uint32_t     step;
        std::uint32_t     bodyA;        // BodyID (index + sequence); for BodyGone, the removed body
        std::uint32_t     bodyB;
        std::uint32_t     subA;
        std::uint32_t     subB;
        Entity            a;            // Added; legacy Removed
        Entity            b;
        DirectX::XMFLOAT3 point;
        DirectX::XMFLOAT3 normal;
        RawKind           kind;
    };

    // The pair of bodies, order-free: the key of the counted contacts.
    std::uint64_t PairKey(std::uint32_t bodyA, std::uint32_t bodyB)
    {
        const std::uint32_t lo = (std::min)(bodyA, bodyB);
        const std::uint32_t hi = (std::max)(bodyA, bodyB);
        return (static_cast<std::uint64_t>(lo) << 32) | hi;
    }
    std::uint32_t PairLo(std::uint64_t key) { return static_cast<std::uint32_t>(key >> 32); }
    std::uint32_t PairHi(std::uint64_t key) { return static_cast<std::uint32_t>(key & 0xffffffffu); }

    struct PairState
    {
        Entity        a = NullEntity;       // the orientation of the first contact: a -> b
        Entity        b = NullEntity;
        int           contacts = 0;         // live sub-shape contacts
        bool          held = false;         // no contacts left, but a sleeping body still touches
        std::uint32_t awakeSince = 0;       // held, and both bodies awake since this step (0 = not yet)
    };
}

struct ContactTracker::Impl : public JPH::ContactListener
{
    explicit Impl(bool legacyMode) : legacy(legacyMode) {}

    // ---- JPH::ContactListener (Jolt worker threads) ----
    void OnContactAdded(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold,
                        JPH::ContactSettings& /*ioSettings*/) override
    {
        // User data is the owning Entity (PreAllSteps creates every body with it); 0 is an editor-local
        // or dead entity: no event.
        Raw r{};
        r.a = BodyUserEntity(body1.GetUserData());
        r.b = BodyUserEntity(body2.GetUserData());
        if (r.a == NullEntity || r.b == NullEntity) return;

        const JPH::Vec3 p = manifold.GetWorldSpaceContactPointOn1(0);
        const JPH::Vec3 n = manifold.mWorldSpaceNormal;
        r.step   = step.load(std::memory_order_relaxed);
        r.bodyA  = body1.GetID().GetIndexAndSequenceNumber();
        r.bodyB  = body2.GetID().GetIndexAndSequenceNumber();
        r.subA   = manifold.mSubShapeID1.GetValue();
        r.subB   = manifold.mSubShapeID2.GetValue();
        r.point  = { p.GetX(), p.GetY(), p.GetZ() };
        r.normal = { n.GetX(), n.GetY(), n.GetZ() };
        r.kind   = RawKind::Added;

        std::lock_guard<std::mutex> lock(mutex);
        if (legacy)
            entityOf[JPH::SubShapeIDPair(body1.GetID(), manifold.mSubShapeID1, body2.GetID(),
                                         manifold.mSubShapeID2)] = { r.a, r.b };
        raws.push_back(r);
    }

    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override
    {
        Raw r{};
        r.step  = step.load(std::memory_order_relaxed);
        r.bodyA = pair.GetBody1ID().GetIndexAndSequenceNumber();
        r.bodyB = pair.GetBody2ID().GetIndexAndSequenceNumber();
        r.subA  = pair.GetSubShapeID1().GetValue();
        r.subB  = pair.GetSubShapeID2().GetValue();
        r.kind  = RawKind::Removed;

        std::lock_guard<std::mutex> lock(mutex);
        if (legacy)
        {
            const auto it = entityOf.find(pair);
            if (it == entityOf.end()) return;   // a contact never recorded (null-entity body)
            r.a = it->second.first;
            r.b = it->second.second;
            entityOf.erase(it);
        }
        raws.push_back(r);
    }

    // ---- main thread ----
    void DrainLegacy(std::vector<Event>& out)
    {
        for (const Raw& r : scratch)
        {
            Event e;
            e.step   = r.step;
            e.a      = r.a;
            e.b      = r.b;
            e.point  = r.point;
            e.normal = r.normal;
            e.began  = (r.kind == RawKind::Added);
            out.push_back(e);
        }
    }

    void AddPair(std::uint64_t key, const PairState& state)
    {
        pairs[key] = state;
        byBody[PairLo(key)].insert(key);
        byBody[PairHi(key)].insert(key);
    }

    void ErasePair(std::uint64_t key)
    {
        for (const std::uint32_t body : { PairLo(key), PairHi(key) })
        {
            const auto it = byBody.find(body);
            if (it == byBody.end()) continue;
            it->second.erase(key);
            if (it->second.empty()) byBody.erase(it);
        }
        pairs.erase(key);
    }

    static void Emit(std::vector<Event>& out, std::uint32_t atStep, const PairState& state, bool began,
                     const DirectX::XMFLOAT3& point, const DirectX::XMFLOAT3& normal)
    {
        Event e;
        e.step   = atStep;
        e.a      = state.a;
        e.b      = state.b;
        e.point  = point;
        e.normal = normal;
        e.began  = began;
        out.push_back(e);
    }

    std::atomic<std::uint32_t> step{ 0 };
    const bool                 legacy;

    std::mutex       mutex;
    std::vector<Raw> raws;
    // Legacy only: the entities of each live sub-shape contact, for its removal callback.
    std::unordered_map<JPH::SubShapeIDPair, std::pair<Entity, Entity>> entityOf;

    // Main thread only.
    std::vector<Raw>                                       scratch;
    std::unordered_map<std::uint64_t, PairState>           pairs;
    std::unordered_map<std::uint32_t, std::unordered_set<std::uint64_t>> byBody;
};

ContactTracker::ContactTracker(bool legacy) : m_impl(std::make_unique<Impl>(legacy)) {}

ContactTracker::~ContactTracker() = default;

bool ContactTracker::Legacy() const { return m_impl->legacy; }

JPH::ContactListener* ContactTracker::Listener() { return m_impl.get(); }

void ContactTracker::BeginStep()
{
    m_impl->step.fetch_add(1, std::memory_order_relaxed);
}

std::uint32_t ContactTracker::Step() const
{
    return m_impl->step.load(std::memory_order_relaxed);
}

void ContactTracker::BodyRemoved(std::uint32_t bodyId)
{
    if (m_impl->legacy) return;
    Raw r{};
    r.step  = m_impl->step.load(std::memory_order_relaxed);
    r.bodyA = bodyId;
    r.bodyB = 0xffffffffu;
    r.kind  = RawKind::BodyGone;
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->raws.push_back(r);
}

void ContactTracker::Drain(const SleepQuery& isAsleep, std::vector<Event>& out)
{
    Impl& s = *m_impl;
    s.scratch.clear();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.scratch.swap(s.raws);
    }

    const std::size_t first = out.size();
    if (s.legacy)
    {
        s.DrainLegacy(out);
        return;
    }

    std::sort(s.scratch.begin(), s.scratch.end(), [](const Raw& x, const Raw& y)
    {
        return std::tie(x.step, x.bodyA, x.bodyB, x.kind, x.subA, x.subB)
             < std::tie(y.step, y.bodyA, y.bodyB, y.kind, y.subA, y.subB);
    });

    const DirectX::XMFLOAT3 zero{ 0.f, 0.f, 0.f };
    for (const Raw& r : s.scratch)
    {
        if (r.kind == RawKind::BodyGone)
        {
            const auto bodyIt = s.byBody.find(r.bodyA);
            if (bodyIt == s.byBody.end()) continue;
            const std::unordered_set<std::uint64_t> keys = bodyIt->second;   // ErasePair edits the index
            for (const std::uint64_t key : keys)
            {
                const auto it = s.pairs.find(key);
                if (it == s.pairs.end()) continue;
                Impl::Emit(out, r.step, it->second, false, zero, zero);
                s.ErasePair(key);
            }
            continue;
        }

        const std::uint64_t key = PairKey(r.bodyA, r.bodyB);
        if (r.kind == RawKind::Added)
        {
            auto it = s.pairs.find(key);
            if (it == s.pairs.end())
            {
                PairState fresh;
                fresh.a = r.a;
                fresh.b = r.b;
                s.AddPair(key, fresh);
                it = s.pairs.find(key);
                Impl::Emit(out, r.step, it->second, true, r.point, r.normal);
            }
            ++it->second.contacts;
            it->second.held       = false;
            it->second.awakeSince = 0;
            continue;
        }

        // Removed: an unknown pair is one already ended (a body removed under it), or never tracked.
        const auto it = s.pairs.find(key);
        if (it == s.pairs.end() || it->second.contacts <= 0) continue;
        if (--it->second.contacts > 0) continue;

        // The last contact is gone. A sleeping body takes its contacts with it while it still touches:
        // hold the pair; the wake-up adds the contacts back.
        if (isAsleep(PairLo(key)) || isAsleep(PairHi(key)))
        {
            it->second.held       = true;
            it->second.awakeSince = 0;
            continue;
        }
        Impl::Emit(out, r.step, it->second, false, zero, zero);
        s.ErasePair(key);
    }

    // A held pair whose bodies are both awake and found no contact for two steps (Jolt needs one to
    // rediscover a touch after a wake-up) was separated while it slept - moved by a teleport: it ended.
    const std::uint32_t now = s.step.load(std::memory_order_relaxed);
    std::vector<std::uint64_t> ended;
    for (auto& [key, state] : s.pairs)
    {
        if (!state.held) continue;
        if (isAsleep(PairLo(key)) || isAsleep(PairHi(key)))
        {
            state.awakeSince = 0;
            continue;
        }
        if (state.awakeSince == 0) { state.awakeSince = now; continue; }
        if (now - state.awakeSince >= 2) ended.push_back(key);
    }
    std::sort(ended.begin(), ended.end());
    for (const std::uint64_t key : ended)
    {
        Impl::Emit(out, now, s.pairs[key], false, zero, zero);
        s.ErasePair(key);
    }

    // Publish order: by step, then the entity pair.
    std::stable_sort(out.begin() + static_cast<std::ptrdiff_t>(first), out.end(),
                     [](const Event& x, const Event& y)
                     { return std::tie(x.step, x.a, x.b) < std::tie(y.step, y.a, y.b); });
}

void ContactTracker::Clear()
{
    Impl& s = *m_impl;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.raws.clear();
        s.entityOf.clear();
    }
    s.scratch.clear();
    s.pairs.clear();
    s.byBody.clear();
}
}
