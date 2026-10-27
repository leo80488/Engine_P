#pragma once

// ContactTracker - turns Jolt's per-sub-shape contact callbacks into one Began / Ended pair per
// pair of bodies.
//
// Jolt reports a contact per (body, sub-shape, body, sub-shape): a compound or mesh body entering a
// trigger gets one callback per sub-shape, and a body that goes to sleep gets OnContactRemoved for
// every contact it has. The tracker counts the sub-shape contacts of each BodyID pair (the id
// carries Jolt's slot generation) and reports
//   Began  when the first contact of a pair arrives,
//   Ended  when its last contact goes - unless a sleeping body took it away (the bodies still touch;
//          a later wake-up adds the contacts back without a new Began), or when one of the bodies is
//          removed (PhysicsSystem calls BodyRemoved: the destroyed or rebuilt body's own removal
//          callbacks arrive after the entity is gone).
// The callbacks run on Jolt's worker threads and only append raw records; Drain sorts them by
// (step, body ids, kind, sub-shape ids) and replays them on the main thread, so the event sequence
// does not depend on which worker saw a contact first (-workers=1 and the default agree).

#include "ECS/ECS.h"

#include <DirectXMath.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace JPH { class ContactListener; }

namespace DX12Physics
{
    class ContactTracker
    {
    public:
        // One event for the EventBus, in publish order. step is the physics step that produced it.
        struct Event
        {
            std::uint32_t     step   = 0;
            Entity            a      = NullEntity;
            Entity            b      = NullEntity;
            DirectX::XMFLOAT3 point  = { 0.f, 0.f, 0.f };
            DirectX::XMFLOAT3 normal = { 0.f, 0.f, 0.f };
            bool              began  = false;
        };

        // True when a BodyID (as PhysicsSystem hands it out) is a dynamic body that is asleep now.
        using SleepQuery = std::function<bool(std::uint32_t bodyId)>;

        // legacy: the per-sub-shape behaviour (one Began / Ended per sub-shape pair, in callback order,
        // sleep ends contacts, nothing is synthesized). The harness flag -physics_legacy_contacts.
        explicit ContactTracker(bool legacy);
        ~ContactTracker();

        ContactTracker(const ContactTracker&)            = delete;
        ContactTracker& operator=(const ContactTracker&) = delete;

        bool Legacy() const;

        // The listener PhysicsSystem registers with Jolt; owned by the tracker.
        JPH::ContactListener* Listener();

        // Counts the physics step about to run: the callbacks of that Update stamp its number on their
        // records. Call before each JPH::PhysicsSystem::Update.
        void BeginStep();

        // The number of the latest step begun (0 before the first).
        std::uint32_t Step() const;

        // A body left Jolt (a reaped or rebuilt body, or its entity was destroyed): every pair it was
        // in ends, now. Its removal callbacks in the next Update find no pair and are ignored.
        void BodyRemoved(std::uint32_t bodyId);

        // Main thread, once per frame after the steps: appends the Began / Ended events in publish order
        // (step, then entity pair). @p isAsleep is read at this moment.
        void Drain(const SleepQuery& isAsleep, std::vector<Event>& out);

        // Drops every record and pair (the world is being cleared).
        void Clear();

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
