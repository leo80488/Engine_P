#pragma once

// EventBus - strongly-typed queued pub/sub.
//
// - One channel per event type, keyed by std::type_index (like ECS ComponentPool
//   storage): one hash map probe per Publish / Subscribe / Dispatch.
// - Publish is QUEUED: events go into a per-type vector and are delivered in
//   DispatchAll() / DispatchOne<E>(). Subscribers never run inside Publish, so
//   there is no reentrancy.
// - DispatchAll delivers in PUBLISH order across types (every Publish takes the next number
//   of one bus-wide sequence), so "A published before B" is "A delivered before B" whatever
//   their types; a channel's subscribers run in subscription order. Nothing depends on a
//   hash order.
// - An event published from inside a subscriber is delivered on the next
//   dispatch (the queue is taken before the first handler runs).
// - Unsubscribe during dispatch is safe: subscriptions are tombstoned
//   (active=false) and compacted opportunistically.
//
// No sync publish, no cross-thread publish: every call comes from the main
// thread. Publish / Subscribe / DispatchAll / DispatchOne check it with
// ENGINE_CHECK_ONCE (Unsubscribe and Clear do not), so a parallel system
// Publishing (unordered_map insertion racing DispatchAll) is a log line instead
// of heap corruption. The exception is a system that declared
// ExclusiveRes::kEventBus: the Scheduler may run it on a worker (the batch
// planner serializes it against every other bus user) and marks that worker
// with an ExclusiveGrant for the system's duration.
//
// Header-only: the singleton's state lives in a function-local static.

#include "System/Check.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <thread>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

class EventBus
{
public:
    using SubscriptionId = std::size_t;
    static constexpr SubscriptionId kInvalidSub = 0;

    // RAII grant the Scheduler holds around a worker-side system that
    // declared ExclusiveRes::kEventBus: the planner already serializes such
    // systems against every other bus user, so the thread check must not
    // fire for them. thread_local so a grant on one worker says nothing
    // about any other thread.
    struct ExclusiveGrant
    {
        ExclusiveGrant()  { Flag() = true;  }
        ~ExclusiveGrant() { Flag() = false; }
        ExclusiveGrant(const ExclusiveGrant&)            = delete;
        ExclusiveGrant& operator=(const ExclusiveGrant&) = delete;
        static bool Active() { return Flag(); }
    private:
        static bool& Flag() { static thread_local bool t_flag = false; return t_flag; }
    };

    static EventBus& Get()
    {
        static EventBus s_instance;
        return s_instance;
    }

    // Re-stamp which thread counts as "the main thread". The constructor
    // stamps the thread that first touched the bus; App calls this during
    // init so the stamp is deterministic rather than first-caller-wins.
    void BindMainThread() { m_mainThread = std::this_thread::get_id(); }

    // Enqueue an event. O(1) amortized.
    template <typename E>
    void Publish(E event)
    {
        ENGINE_CHECK_ONCE(OnValidThread(),
                          "EventBus::Publish(%s) from a worker thread without a "
                          "kEventBus grant — declare ExclusiveRes::kEventBus in the "
                          "publishing system's DeclareAccess", typeid(E).name());
        channel<E>().pending.push_back({ ++m_publishSeq, std::move(event) });
    }

    // Register a subscriber. Returns a non-zero id; pass to Unsubscribe<E>.
    template <typename E>
    SubscriptionId Subscribe(std::function<void(const E&)> callback)
    {
        ENGINE_CHECK_ONCE(OnValidThread(),
                          "EventBus::Subscribe(%s) off the main thread", typeid(E).name());
        auto& c = channel<E>();
        const SubscriptionId id = ++c.nextSubId;
        c.subs.push_back({ id, std::move(callback), true });
        return id;
    }

    // Marks the subscription inactive; safe to call from inside a callback.
    // Returns true if the id was found.
    template <typename E>
    bool Unsubscribe(SubscriptionId id)
    {
        if (id == kInvalidSub) return false;
        auto* ch = findChannel<E>();
        if (!ch) return false;
        for (auto& s : ch->subs)
            if (s.id == id && s.active) { s.active = false; return true; }
        return false;
    }

    // Deliver everything queued so far, across all channels, in publish order. Events a handler
    // publishes land in the next pass. Not re-entrant: a call from inside a handler returns.
    void DispatchAll()
    {
        ENGINE_CHECK_ONCE(OnValidThread(), "EventBus::DispatchAll off the main thread");
        if (m_dispatching) return;
        m_dispatching = true;
        for (IChannel* ch : m_order)
            ch->Take();
        for (;;)
        {
            IChannel* next = nullptr;
            std::uint64_t best = (std::numeric_limits<std::uint64_t>::max)();
            for (IChannel* ch : m_order)
            {
                const std::uint64_t seq = ch->NextSeq();
                if (seq < best) { best = seq; next = ch; }
            }
            if (!next) break;
            next->DeliverNext();
        }
        for (IChannel* ch : m_order)
            ch->Compact();
        m_dispatching = false;
    }

    // Drain one channel, for a caller that needs to choose when a specific
    // event type is delivered (e.g. flush script-facing events at a
    // different point than engine-internal ones). Like DispatchAll it returns at once when
    // called from inside a handler.
    template <typename E>
    void DispatchOne()
    {
        ENGINE_CHECK_ONCE(OnValidThread(),
                          "EventBus::DispatchOne(%s) off the main thread", typeid(E).name());
        auto* ch = findChannel<E>();
        if (!ch || m_dispatching) return;
        m_dispatching = true;
        ch->Take();
        while (ch->NextSeq() != (std::numeric_limits<std::uint64_t>::max)())
            ch->DeliverNext();
        ch->Compact();
        m_dispatching = false;
    }

    // Wipe all queues + subscribers. Intended for shutdown / test harnesses.
    void Clear()
    {
        m_order.clear();
        m_channels.clear();
    }

private:
    struct IChannel
    {
        virtual ~IChannel() = default;
        // Moves the queued events into the batch being delivered.
        virtual void Take() = 0;
        // The publish number of the next undelivered event of the batch; max when it is empty.
        virtual std::uint64_t NextSeq() const = 0;
        virtual void DeliverNext() = 0;
        virtual void Compact() = 0;
    };

    template <typename E>
    struct Channel : IChannel
    {
        struct Sub
        {
            SubscriptionId                 id;
            std::function<void(const E&)>  callback;
            bool                           active;
        };
        struct Item
        {
            std::uint64_t seq;
            E             event;
        };

        std::vector<Item> pending;        // enqueued by Publish, taken by Take
        std::vector<Item> batch;          // the events being delivered
        std::size_t       cursor = 0;
        std::vector<Sub>  subs;
        SubscriptionId    nextSubId = 0;

        void Take() override
        {
            // Taken before the first handler runs, so a handler that publishes <E> lands in the
            // next pass: forward progress even if a subscriber always republishes.
            if (batch.empty())
                std::swap(batch, pending);
        }

        std::uint64_t NextSeq() const override
        {
            return cursor < batch.size() ? batch[cursor].seq
                                         : (std::numeric_limits<std::uint64_t>::max)();
        }

        void DeliverNext() override
        {
            // Index + bound snapshot, not range-for: a handler may Subscribe, and the push_back
            // reallocation would dangle a `Sub&`. Handlers subscribed mid-dispatch first fire on
            // the NEXT event.
            const E& event = batch[cursor].event;
            const std::size_t count = subs.size();
            for (std::size_t i = 0; i < count; ++i)
            {
                if (!subs[i].active) continue;
                auto callback = subs[i].callback;
                callback(event);
            }
            if (++cursor >= batch.size())
            {
                batch.clear();
                cursor = 0;
            }
        }

        void Compact() override
        {
            // Compact tombstones once more than half the subscriptions are
            // inactive: a tight inner loop without compacting every frame.
            std::size_t inactiveCount = 0;
            for (const Sub& s : subs) if (!s.active) ++inactiveCount;
            if (inactiveCount * 2 > subs.size() && !subs.empty())
            {
                std::vector<Sub> live;
                live.reserve(subs.size() - inactiveCount);
                for (Sub& s : subs) if (s.active) live.push_back(std::move(s));
                subs = std::move(live);
            }
        }
    };

    EventBus() : m_mainThread(std::this_thread::get_id()) {}

    bool OnValidThread() const
    {
        return std::this_thread::get_id() == m_mainThread || ExclusiveGrant::Active();
    }

    template <typename E>
    Channel<E>& channel()
    {
        const std::type_index ti{ typeid(E) };
        auto it = m_channels.find(ti);
        if (it != m_channels.end())
            return *static_cast<Channel<E>*>(it->second.get());

        auto owned = std::make_unique<Channel<E>>();
        Channel<E>* raw = owned.get();
        m_order.push_back(raw);
        m_channels.emplace(ti, std::move(owned));
        return *raw;
    }

    template <typename E>
    Channel<E>* findChannel()
    {
        const std::type_index ti{ typeid(E) };
        auto it = m_channels.find(ti);
        return it == m_channels.end()
            ? nullptr
            : static_cast<Channel<E>*>(it->second.get());
    }

    std::unordered_map<std::type_index, std::unique_ptr<IChannel>> m_channels;
    std::vector<IChannel*> m_order;   // creation order: the walk of DispatchAll never hashes

    // The last publish number handed out; one sequence for the whole bus.
    std::uint64_t m_publishSeq = 0;
    bool          m_dispatching = false;

    // Stamped by the constructor (first Get()) and re-stamped by App's
    // BindMainThread() call; see OnValidThread().
    std::thread::id m_mainThread;
};
