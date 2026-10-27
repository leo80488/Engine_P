#pragma once

// Internal to the ScriptSystem translation units - NOT a public engine header.
// Shared by ScriptSystem.cpp and ScriptSystem_Bindings.cpp (RegisterBindings is its own TU).
// The helpers here are pure and stateless, so `inline` is safe; a helper that owns state
// lives in exactly one .cpp.
// Include AFTER <sol/sol.hpp> and "Resource/AssetFS.h".

// ==== ScriptInternal helpers ====
namespace ScriptInternal
{
    // Loads a Lua asset through AssetFS (pak first, disk fallback) and runs it. Use this, not
    // sol::state::safe_script_file, which reads only from disk and misses scripts packed in
    // game.ipak. A missing file returns an invalid result, so callers' `!result.valid()` checks
    // fire and log their own path context.
    inline sol::protected_function_result RunScriptAsset(sol::state& lua, const std::string& path)
    {
        std::string src;
        if (Resource::AssetFS::Get().ReadFileText(path, src))
            return lua.safe_script(src, sol::script_pass_on_error, "@" + path);
        // Not in pak or on disk - return an invalid result.
        return lua.safe_script("error('asset not found')", sol::script_pass_on_error);
    }

    // Each entity's logic instances live as a Lua array at lua["__logic_instances"][entityId], slot
    // stored at [slot + 1] so it reads back as a Lua array; callers check the returned table's .valid().
    // Returns the entity's instance array, created on demand when `create` is set; otherwise an
    // invalid (nil) table when none exists.
    inline sol::table GetInstanceArray(sol::state& lua, uint32_t e, bool create)
    {
        sol::object o = lua["__logic_instances"][e];
        if (o.get_type() == sol::type::table) return o.as<sol::table>();
        if (!create) return sol::table{};
        sol::table t = lua.create_table();
        lua["__logic_instances"][e] = t;
        return t;
    }

    // Returns the slot's instance table, or an invalid (nil) table if the
    // entity has no array or the slot is empty.
    inline sol::table GetInstance(sol::state& lua, uint32_t e, std::size_t slot)
    {
        sol::object arr = lua["__logic_instances"][e];
        if (arr.get_type() != sol::type::table) return sol::table{};
        sol::object inst = arr.as<sol::table>()[slot + 1];
        if (inst.get_type() != sol::type::table) return sol::table{};
        return inst.as<sol::table>();
    }
}


// ==== pImpl struct definitions ====
// Shared by ScriptSystem.cpp and ScriptSystem_Bindings.cpp (RegisterBindings drives the
// timer / tween / bus queues directly). They live in this private header so the public
// Scripting/ScriptSystem.h never sees a sol:: type.
// Include AFTER "Scripting/ScriptSystem.h".

// LuaBus: string-keyed Lua event queue. Unsubscribe marks a subscriber inactive (tombstone);
// ScriptSystem::DispatchLuaEvents drains it and Compact drops the tombstones once more than half
// are inactive, as the C++ EventBus does.
struct ScriptSystem::LuaBus
{
    struct Sub
    {
        std::uint64_t             id;
        std::string               name;
        OwnerId                   owner;   // whose code subscribed; 0 = no owner
        sol::protected_function   fn;
        bool                      active;
    };

    struct Pending
    {
        std::string   name;
        sol::table    payload;
    };

    std::vector<Sub>     subs;
    std::vector<Pending> pending;
    std::uint64_t        nextId = 0;

    std::uint64_t Subscribe(std::string name, sol::protected_function fn, OwnerId owner)
    {
        const std::uint64_t id = ++nextId;
        subs.push_back({ id, std::move(name), owner, std::move(fn), true });
        return id;
    }

    // Drops every subscription @p owner made (its teardown / release).
    std::size_t RemoveOwned(OwnerId owner)
    {
        if (owner == 0) return 0;
        std::size_t n = 0;
        for (auto& s : subs)
            if (s.active && s.owner == owner) { s.active = false; ++n; }
        return n;
    }

    bool Unsubscribe(std::uint64_t id)
    {
        for (auto& s : subs)
            if (s.id == id && s.active) { s.active = false; return true; }
        return false;
    }

    // Events wait here until ScriptSystem::Update drains them. While Update does not run (the
    // editor stopped or paused) nothing drains, so the queue is bounded: the oldest events go,
    // and a resume never delivers a burst of stale ones.
    static constexpr std::size_t kMaxPending = 1024;
    std::size_t dropped = 0;

    void Publish(std::string name, sol::table payload)
    {
        if (pending.size() >= kMaxPending)
        {
            pending.erase(pending.begin());
            ++dropped;
        }
        pending.push_back({ std::move(name), std::move(payload) });
    }

    void Compact()
    {
        std::size_t inactive = 0;
        for (const Sub& s : subs) if (!s.active) ++inactive;
        if (inactive * 2 > subs.size() && !subs.empty())
        {
            std::vector<Sub> live;
            live.reserve(subs.size() - inactive);
            for (Sub& s : subs) if (s.active) live.push_back(std::move(s));
            subs = std::move(live);
        }
    }
};

// TimerQueue: `Engine.AfterDelay(seconds, fn[, domain])` and `Engine.Every` storage. A "real"
// entry counts real (unscaled) dt (TickTimers), so hit-stop timers expire while the game clock is
// near zero; a "game" entry counts the game dt (TickGameTimers) and stops with a pause. Compacted
// only once expired entries are the majority, as in LuaBus.
struct ScriptSystem::TimerQueue
{
    struct Entry
    {
        std::uint64_t           id;
        float                   remaining;     // seconds to next fire, on the entry's clock
        sol::protected_function fn;
        bool                    active;
        OwnerId                 owner;         // whose code scheduled it; the callback runs as it
        float                   interval = 0.f;   // > 0: repeats (Engine.Every)
        bool                    real     = true;  // real-time domain, else game time
        std::function<void()>   native;           // a callback C++ started for the owner (RunAsOwner)
    };

    std::vector<Entry> entries;
    std::uint64_t      nextId = 0;

    std::uint64_t Schedule(float seconds, sol::protected_function fn, OwnerId owner,
                           bool real = true, float interval = 0.f)
    {
        const std::uint64_t id = ++nextId;
        entries.push_back({ id, seconds, std::move(fn), true, owner, interval, real, {} });
        return id;
    }

    std::uint64_t ScheduleNative(float seconds, std::function<void()> native, OwnerId owner, bool real)
    {
        const std::uint64_t id = ++nextId;
        entries.push_back({ id, seconds, {}, true, owner, 0.f, real, std::move(native) });
        return id;
    }

    bool Cancel(std::uint64_t id)
    {
        for (auto& e : entries)
            if (e.id == id && e.active) { e.active = false; return true; }
        return false;
    }

    std::size_t CancelOwned(OwnerId owner)
    {
        if (owner == 0) return 0;
        std::size_t n = 0;
        for (auto& e : entries)
            if (e.active && e.owner == owner) { e.active = false; ++n; }
        return n;
    }

    std::size_t Reassign(OwnerId from, OwnerId to)
    {
        if (from == 0) return 0;
        std::size_t n = 0;
        for (auto& e : entries)
            if (e.active && e.owner == from) { e.owner = to; ++n; }
        return n;
    }
};

// TaskQueue: `Engine.Run` coroutines, each waiting on a clock (Wait.Seconds / RealSeconds / Frames).
// The thread is anchored in the Lua registry until the task ends.
struct ScriptSystem::TaskQueue
{
    enum class Wait : std::uint8_t { Frames, GameSeconds, RealSeconds };

    struct Entry
    {
        std::uint64_t id;
        OwnerId       owner;      // whose code started it; it runs as it and ends with it
        lua_State*    co;
        int           ref;        // LUA_REGISTRYINDEX anchor of the thread
        Wait          wait;
        float         remaining;  // seconds left of a *Seconds wait
        int           frames;     // frames left of a Frames wait
        bool          active;
    };

    std::vector<Entry> entries;
    std::uint64_t      nextId = 0;
};

// TweenQueue: Engine.MoveTo / RotateTo / ScaleTo storage. Each tween drives ONE channel
// (translation / rotation / scale) of an entity's LocalTransform from a captured start to a
// target over a duration with an easing curve, then optionally calls onDone. Ticked with game
// dt (TickTweens) so a tween freezes on pause. A new tween on the same (entity, channel)
// replaces the old one; cancelled / finished entries are compacted like TimerQueue's.
struct ScriptSystem::TweenQueue
{
    enum class Channel : std::uint8_t { Translation, Rotation, Scale };
    enum class Easing  : std::uint8_t { Linear, EaseIn, EaseOut, EaseInOut, Smoothstep };

    struct Entry
    {
        Entity                  entity;
        Channel                 channel;
        Easing                  easing;
        DirectX::XMFLOAT4       from;       // xyz (+w for rotation quat)
        DirectX::XMFLOAT4       to;
        float                   elapsed;
        float                   duration;
        sol::protected_function onDone;     // may be invalid
        bool                    active;
        OwnerId                 owner;      // onDone runs as it; its release drops onDone only
    };

    std::vector<Entry> entries;

    // Parses an easing name. An unknown name returns Smoothstep and sets *ok = false, so callers
    // can warn instead of silently using the wrong curve.
    static Easing ParseEasing(const std::string& s, bool* ok = nullptr)
    {
        if (ok) *ok = true;
        if (s == "linear")               return Easing::Linear;
        if (s == "easeIn"  || s == "in") return Easing::EaseIn;
        if (s == "easeOut" || s == "out")return Easing::EaseOut;
        if (s == "easeInOut" || s == "inout") return Easing::EaseInOut;
        if (s == "smoothstep")           return Easing::Smoothstep;
        if (ok) *ok = false;
        return Easing::Smoothstep;       // default - gentle accel + decel
    }

    static float Apply(Easing e, float t)
    {
        if (t < 0.f) t = 0.f; else if (t > 1.f) t = 1.f;
        switch (e)
        {
            case Easing::Linear:     return t;
            case Easing::EaseIn:     return t * t;
            case Easing::EaseOut:    return 1.f - (1.f - t) * (1.f - t);
            case Easing::EaseInOut:  return t < 0.5f ? 2.f * t * t
                                                     : 1.f - 2.f * (1.f - t) * (1.f - t);
            case Easing::Smoothstep: return t * t * (3.f - 2.f * t);
        }
        return t;
    }

    // Replace any existing tween on the same (entity, channel); push fresh.
    void Add(Entry e)
    {
        for (auto& x : entries)
            if (x.active && x.entity == e.entity && x.channel == e.channel)
            {
                x = std::move(e);
                return;
            }
        entries.push_back(std::move(e));
    }

    // Cancel every tween on an entity. Returns how many were cancelled.
    int CancelEntity(Entity entity)
    {
        int n = 0;
        for (auto& x : entries)
            if (x.active && x.entity == entity) { x.active = false; ++n; }
        return n;
    }

    std::size_t Reassign(OwnerId from, OwnerId to)
    {
        if (from == 0) return 0;
        std::size_t n = 0;
        for (auto& x : entries)
            if (x.active && x.owner == from) { x.owner = to; ++n; }
        return n;
    }

    // An owner that ended keeps its tweens moving (the entity still wants to arrive) but loses
    // the callback into its torn-down state.
    std::size_t DropOwnedCallbacks(OwnerId owner)
    {
        if (owner == 0) return 0;
        std::size_t n = 0;
        for (auto& x : entries)
            if (x.active && x.owner == owner && x.onDone.valid())
            {
                x.onDone = sol::protected_function{};
                ++n;
            }
        return n;
    }
};

// AnimEventQueue: anim notifies queued by the EventBus drain (EventDelivery) and fired from
// Update(), where m_world is valid (see ScriptSystem.h).
struct ScriptSystem::AnimEventQueue
{
    struct Entry
    {
        Entity      e   = NullEntity;
        uint32_t    gen = 0;
        std::string name;
        sol::object payload;    // built at queue time; no world needed for that
    };
    std::vector<Entry> pending;
};
