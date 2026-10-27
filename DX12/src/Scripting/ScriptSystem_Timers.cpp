// ScriptSystem_Timers.cpp - Lua timers and tasks on two clocks. Engine.AfterDelay / Engine.Every run
// callbacks after seconds of the game clock ("game": stops with a pause and a hit-stop) or of real
// time ("real": keeps running); Engine.Run starts a coroutine that waits with Wait.Seconds /
// Wait.RealSeconds / Wait.Frames / Wait.Until. Real-time entries are ticked by TickTimers
// (ScriptTimerSystem, outside every set), game-time ones by Update. Everything belongs to the owner
// whose code created it and goes with it; an entry whose owner's set is suspended neither counts
// down nor fires until the set is back.

#include "Scripting/ScriptSystem.h"
#include "Scripting/LuaDoc.h"
#include "Scripting/ScriptOwner.h"
#include "Harness/RunLedger.h"
#include "System/Log.h"

#include <sol/sol.hpp>

#include "Resource/AssetFS.h"          // ScriptSystem_Internal.h
#include "ScriptSystem_Internal.h"     // private to src/Scripting, not on the include path

#include <algorithm>
#include <string>

namespace
{
    constexpr const char* kTaskMarker = "__task_wait";

    // Engine.Frame(), or 0 before App has bound it.
    lua_Integer FrameIndex(sol::state& lua)
    {
        sol::object frame = lua["Engine"]["Frame"];
        if (!frame.is<sol::function>()) return 0;
        sol::protected_function call = frame.as<sol::function>();
        const auto result = call();
        return result.valid() ? result.get<lua_Integer>() : 0;
    }

    // Wait.* are plain Lua over coroutine.yield: the marker tells the scheduler this yield is a
    // wait, and what for. A coroutine of Engine.Run is registered in __wait_tasks; Wait.Frames
    // hands any other caller (a control-channel async run) to its own implementation.
    constexpr const char* kWaitLua = R"lua(
Wait = Wait or {}
__wait_tasks = __wait_tasks or setmetatable({}, { __mode = 'k' })
local tasks = __wait_tasks
local function need(name)
  if not tasks[coroutine.running()] then
    error('Wait.' .. name .. ' is only valid inside a function started with Engine.Run', 3)
  end
end
Wait.Seconds = function(s)
  need('Seconds')
  return coroutine.yield('__task_wait', 'game', math.max(0, tonumber(s) or 0))
end
Wait.RealSeconds = function(s)
  need('RealSeconds')
  return coroutine.yield('__task_wait', 'real', math.max(0, tonumber(s) or 0))
end
Wait.Frames = function(n)
  need('Frames')
  return coroutine.yield('__task_wait', 'frames', math.max(1, math.floor(tonumber(n) or 1)))
end
Wait.Until = function(fn, maxFrames)
  maxFrames = math.floor(tonumber(maxFrames) or 600)
  for i = 0, maxFrames do
    if fn() then return true, i end
    if i == maxFrames then break end
    Wait.Frames(1)
  end
  return false, maxFrames
end
)lua";
}

// ==== Timers ====

void ScriptSystem::FireTimers(bool realDomain, float dt)
{
    if (!m_timers || m_timers->entries.empty()) return;

    // The set check costs a lookup per due entry: skip it while nothing declares a set.
    bool anySet = false;
    for (const SystemEntry& s : m_systems)
        if (!s.systemSet.empty()) { anySet = true; break; }
    if (!anySet)
        for (const auto& [path, tmpl] : m_logicTemplates)
            if (!tmpl.systemSet.empty()) { anySet = true; break; }

    // Index loop over a snapshotted count: a callback may schedule (push_back reallocates) and
    // cancel; new entries wait for the next tick.
    const std::size_t count = m_timers->entries.size();
    for (std::size_t i = 0; i < count; ++i)
    {
        TimerQueue::Entry& e = m_timers->entries[i];
        if (!e.active || e.real != realDomain) continue;
        if (anySet && OwnerSuspended(e.owner)) continue;
        e.remaining -= dt;
        if (e.remaining > 0.f) continue;

        // Copy the callback out before invoking it: it may reallocate `entries`.
        sol::protected_function fn = e.fn;
        std::function<void()> native = e.native;
        const OwnerId owner = e.owner;
        const bool repeats = e.interval > 0.f;
        if (repeats)
        {
            e.remaining += e.interval;
            if (e.remaining <= 0.f) e.remaining = e.interval;
        }
        else
            e.active = false;

        OwnerScope as(*this, owner);
        bool stop = false;
        if (native)
            native();
        else
        {
            auto res = fn();
            if (!res.valid())
            {
                sol::error err = res;
                LOG_ERROR("Lua timer callback error: %s", err.what());
                stop = repeats;   // a repeating callback that errors is not called again
            }
            else if (repeats && res.get_type() == sol::type::boolean && !res.get<bool>())
                stop = true;      // returning false ends an Engine.Every
        }
        if (stop && i < m_timers->entries.size()) m_timers->entries[i].active = false;
    }

    std::size_t dead = 0;
    for (const auto& e : m_timers->entries) if (!e.active) ++dead;
    if (dead * 2 > m_timers->entries.size() && !m_timers->entries.empty())
    {
        std::vector<TimerQueue::Entry> live;
        live.reserve(m_timers->entries.size() - dead);
        for (auto& e : m_timers->entries) if (e.active) live.push_back(std::move(e));
        m_timers->entries = std::move(live);
    }
}

// Real time: Time.realDt / realElapsed, the expiry of timed requests, real-domain timers and tasks.
// Runs from ScriptTimerSystem, outside every SystemSet, so none of it stops with a game state.
void ScriptSystem::TickTimers(float realDt)
{
    // Published before the early-out below, so it updates every frame, not only on frames with a
    // pending timer. Time.dt / Time.elapsed are scaled (ScriptLogicSystem passes
    // ctx.scaledDeltaTime) and freeze with the game; Time.realDt / realElapsed keep running for
    // what must not freeze: a pause menu's animation, a rebind screen's "press a key" timeout.
    if (m_lua)
    {
        m_realElapsed += realDt;
        (*m_lua)["Time"]["realDt"]      = realDt;
        (*m_lua)["Time"]["realElapsed"] = m_realElapsed;
    }

    // A timed Time.RequestScale (a hit-stop) expires on real time whether or not a timer is pending.
    TickTimeRequests(realDt);

    const bool hasTimers = m_timers && !m_timers->entries.empty();
    const bool hasTasks  = m_tasks && !m_tasks->entries.empty();
    if (!hasTimers && !hasTasks && m_pendingDestroys.empty()) return;

    // Callbacks routinely call world-dependent bindings (Engine.IsAlive / DestroyEntity), which
    // guard on m_world, and this runs as its own phase with m_world null. Borrow only: this is
    // the flush point for deferred destroys, so a timer that destroys its own entity must take
    // effect now.
    WorldBorrow borrow(*this);
    FlushPendingDestroys();
    FireTimers(/*realDomain=*/true, realDt);
    TickTasks(/*realDomain=*/true, realDt);
}

// Game time: called from Update with the game dt.
void ScriptSystem::TickGameTimers(float dt)
{
    FireTimers(/*realDomain=*/false, dt);
    TickTasks(/*realDomain=*/false, dt);
}

// ==== Tasks ====

void ScriptSystem::ResumeTask(std::size_t index, int nargs)
{
    lua_State* co = m_tasks->entries[index].co;
    const OwnerId owner = m_tasks->entries[index].owner;
    lua_State* main = m_lua->lua_state();

    WorldBorrow borrow(*this);
    OwnerScope  as(*this, owner);
    int nres = 0;
    const int status = lua_resume(co, main, nargs, &nres);

    // Re-fetched: the task may have started others.
    TaskQueue::Entry& e = m_tasks->entries[index];
    auto finish = [&]
    {
        luaL_unref(main, LUA_REGISTRYINDEX, e.ref);
        e.ref    = LUA_NOREF;
        e.co     = nullptr;
        e.active = false;
    };

    if (status == LUA_YIELD)
    {
        bool ok = false;
        if (nres == 3 && lua_type(co, -3) == LUA_TSTRING && lua_type(co, -2) == LUA_TSTRING
            && lua_isnumber(co, -1) && std::string(lua_tostring(co, -3)) == kTaskMarker)
        {
            const std::string kind = lua_tostring(co, -2);
            const double      v    = lua_tonumber(co, -1);
            if (kind == "game")        { e.wait = TaskQueue::Wait::GameSeconds; e.remaining = static_cast<float>(v); ok = true; }
            else if (kind == "real")   { e.wait = TaskQueue::Wait::RealSeconds; e.remaining = static_cast<float>(v); ok = true; }
            else if (kind == "frames") { e.wait = TaskQueue::Wait::Frames;      e.frames = static_cast<int>(v);        ok = true; }
        }
        lua_pop(co, nres);
        if (!ok)
        {
            LOG_ERROR("Engine.Run task #%llu yielded outside Wait.*: stopped",
                      static_cast<unsigned long long>(e.id));
            Harness::RunLedger::Get().NoteScriptFault("Engine.Run", "yielded outside Wait.*");
            finish();
        }
        return;
    }

    if (status == LUA_OK)
    {
        lua_pop(co, nres);
        finish();
        return;
    }

    const char* msg = lua_tostring(co, -1);
    std::string text = msg ? msg : "?";
    luaL_traceback(main, co, text.c_str(), 0);
    const char* tb = lua_tostring(main, -1);
    LOG_ERROR("Engine.Run task #%llu error: %s", static_cast<unsigned long long>(e.id), tb ? tb : text.c_str());
    Harness::RunLedger::Get().NoteScriptFault("Engine.Run", text.c_str());
    lua_pop(main, 1);
    finish();
}

void ScriptSystem::TickTasks(bool realDomain, float dt)
{
    if (!m_tasks || m_tasks->entries.empty()) return;

    bool anySet = false;
    for (const SystemEntry& s : m_systems)
        if (!s.systemSet.empty()) { anySet = true; break; }
    if (!anySet)
        for (const auto& [path, tmpl] : m_logicTemplates)
            if (!tmpl.systemSet.empty()) { anySet = true; break; }

    const std::size_t count = m_tasks->entries.size();
    for (std::size_t i = 0; i < count; ++i)
    {
        TaskQueue::Entry& e = m_tasks->entries[i];
        if (!e.active) continue;
        const bool realWait = e.wait != TaskQueue::Wait::GameSeconds;
        if (realWait != realDomain) continue;
        if (anySet && OwnerSuspended(e.owner)) continue;

        bool due = false;
        switch (e.wait)
        {
        case TaskQueue::Wait::Frames:       due = --e.frames <= 0; break;
        case TaskQueue::Wait::GameSeconds:
        case TaskQueue::Wait::RealSeconds:  e.remaining -= dt; due = e.remaining <= 0.f; break;
        }
        if (!due) continue;
        int nargs = 0;
        if (e.wait == TaskQueue::Wait::Frames && !Scripting::LegacyTimers())
        {
            // Wait.Frames returns the frame it resumed in, as the control channel's async run does.
            lua_pushinteger(e.co, FrameIndex(*m_lua));
            nargs = 1;
        }
        ResumeTask(i, nargs);
    }

    std::size_t dead = 0;
    for (const auto& e : m_tasks->entries) if (!e.active) ++dead;
    if (dead * 2 > m_tasks->entries.size() && !m_tasks->entries.empty())
    {
        std::vector<TaskQueue::Entry> live;
        live.reserve(m_tasks->entries.size() - dead);
        for (auto& e : m_tasks->entries) if (e.active) live.push_back(std::move(e));
        m_tasks->entries = std::move(live);
    }
}

std::size_t ScriptSystem::CancelTasks(OwnerId owner, bool all)
{
    if (!m_tasks || !m_lua) return 0;
    std::size_t n = 0;
    for (TaskQueue::Entry& e : m_tasks->entries)
    {
        if (!e.active || (!all && e.owner != owner)) continue;
        luaL_unref(m_lua->lua_state(), LUA_REGISTRYINDEX, e.ref);
        e.ref    = LUA_NOREF;
        e.co     = nullptr;
        e.active = false;
        ++n;
    }
    if (all) m_tasks->entries.clear();
    return n;
}

// ==== Lua: Engine.AfterDelay / Every / CancelDelay / Run / CancelRun ====
void ScriptSystem::RegisterTimerBindings()
{
    if (luaL_dostring(m_lua->lua_state(), kWaitLua) != LUA_OK)
    {
        LOG_ERROR("ScriptSystem: installing Wait.* failed: %s", lua_tostring(m_lua->lua_state(), -1));
        lua_pop(m_lua->lua_state(), 1);
    }

    sol::table engine = (*m_lua)["Engine"];

    // "game" | "real"; nullopt = the domain was named wrongly.
    auto domainOf = [](const sol::optional<std::string>& name, const char* fn) -> sol::optional<bool>
    {
        if (!name || *name == "real") return true;
        if (*name == "game") return Scripting::LegacyTimers() ? true : false;
        LOG_WARNING("Engine.%s: domain must be \"game\" or \"real\" (got \"%s\")", fn, name->c_str());
        return sol::nullopt;
    };

    LuaDoc::Fn(engine, "Engine", "AfterDelay",
        [this, domainOf](float seconds, sol::protected_function fn,
                         sol::optional<std::string> domain) -> std::uint64_t
        {
            if (!fn.valid())
            { LOG_WARNING("Engine.AfterDelay: invalid callback"); return 0ull; }
            const sol::optional<bool> real = domainOf(domain, "AfterDelay");
            if (!real) return 0ull;
            if (seconds < 0.f) seconds = 0.f;
            // The callback runs as, and is cancelled with, the script that scheduled it.
            return m_timers->Schedule(seconds, std::move(fn), m_currentOwner, *real, 0.f);
        },
        { .doc = "Call fn once after seconds. domain \"real\" (the default) counts real time and keeps running through a pause or a hit-stop; \"game\" counts the game clock and stops with them. Cancelled with the script that scheduled it (its scene, System or entity); while that script's SystemSet is suspended the timer holds its remaining time.",
          .params = { { .name = "seconds" }, { .name = "fn" },
                      { .name = "domain", .doc = "\"real\" (default) or \"game\"", .optional = true } },
          .returnsDoc = "timer id for Engine.CancelDelay; 0 when fn or domain is invalid", .effect = LuaDoc::Effect::World });

    LuaDoc::Fn(engine, "Engine", "Every",
        [this, domainOf](float seconds, sol::protected_function fn,
                         sol::optional<std::string> domain) -> std::uint64_t
        {
            if (!fn.valid())
            { LOG_WARNING("Engine.Every: invalid callback"); return 0ull; }
            const sol::optional<bool> real = domainOf(domain, "Every");
            if (!real) return 0ull;
            if (seconds <= 0.f)
            { LOG_WARNING("Engine.Every: seconds must be > 0"); return 0ull; }
            return m_timers->Schedule(seconds, std::move(fn), m_currentOwner, *real, seconds);
        },
        { .doc = "Call fn every seconds until cancelled; fn returning false (or raising an error) ends it. At most one call per frame. domain and ownership as Engine.AfterDelay (\"real\" default, \"game\" stops with a pause).",
          .params = { { .name = "seconds", .doc = "> 0" }, { .name = "fn" },
                      { .name = "domain", .doc = "\"real\" (default) or \"game\"", .optional = true } },
          .returnsDoc = "timer id for Engine.CancelDelay; 0 when invalid", .effect = LuaDoc::Effect::World });

    LuaDoc::Fn(engine, "Engine", "CancelDelay",
        [this](std::uint64_t id) -> bool { return m_timers->Cancel(id); },
        { .doc = "Cancel an Engine.AfterDelay or Engine.Every timer; false when it already fired or was cancelled.",
          .params = { { .name = "id" } }, .effect = LuaDoc::Effect::World });

    LuaDoc::Fn(engine, "Engine", "Run",
        [this](sol::this_state ts, sol::protected_function fn, sol::variadic_args args) -> std::uint64_t
        {
            if (!fn.valid())
            { LOG_WARNING("Engine.Run: invalid function"); return 0ull; }
            lua_State* L  = ts.L;
            lua_State* co = lua_newthread(L);
            // Registered as a task, so Wait.* accept it, and anchored until it ends.
            lua_getglobal(L, "__wait_tasks");
            lua_pushvalue(L, -2);
            lua_pushboolean(L, 1);
            lua_rawset(L, -3);
            lua_pop(L, 1);
            const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
            fn.push(co);
            int nargs = 0;
            for (auto a : args)
            {
                lua_pushvalue(L, a.stack_index());
                lua_xmove(L, co, 1);
                ++nargs;
            }
            const std::uint64_t id = ++m_tasks->nextId;
            m_tasks->entries.push_back(TaskQueue::Entry{ id, m_currentOwner, co, ref,
                                                         TaskQueue::Wait::Frames, 0.f, 0, true });
            // Runs at once, up to its first Wait.
            ResumeTask(m_tasks->entries.size() - 1, nargs);
            return id;
        },
        { .doc = "Start fn(...) as a task, a coroutine that runs now up to its first Wait.* call and then continues on its own: Wait.Seconds(s) waits game time (stops with a pause), Wait.RealSeconds(s) real time, Wait.Frames(n) n frames, Wait.Until(pred [, maxFrames]) polls pred each frame and returns true, frames waited (false when maxFrames - default 600 - ran out). The task belongs to the script that started it and ends with it; an error stops it and is logged as a script fault.",
          .params = { { .name = "fn" }, { .name = "...", .doc = "passed to fn" } },
          .returnsDoc = "task id for Engine.CancelRun", .effect = LuaDoc::Effect::World });

    LuaDoc::Fn(engine, "Engine", "CancelRun",
        [this](std::uint64_t id) -> bool
        {
            for (TaskQueue::Entry& e : m_tasks->entries)
            {
                if (!e.active || e.id != id) continue;
                luaL_unref(m_lua->lua_state(), LUA_REGISTRYINDEX, e.ref);
                e.ref    = LUA_NOREF;
                e.co     = nullptr;
                e.active = false;
                return true;
            }
            return false;
        },
        { .doc = "Stop an Engine.Run task where it waits; false when it already ended.",
          .params = { { .name = "id" } }, .effect = LuaDoc::Effect::World });

    // C++ code that starts a callback for a script (an asynchronous spawn's onReady) runs it as the
    // owner of the script that asked.
    Scripting::SetOwnerBackend(
        [this]() -> Scripting::OwnerToken { return m_currentOwner; },
        [this](Scripting::OwnerToken owner, std::function<void()> fn)
        {
            if (owner != 0 && m_owners.find(owner) == m_owners.end())
            {
                LOG_INFO("[SCRIPTSCOPE] a callback was dropped: the script that asked for it has ended");
                return;
            }
            m_timers->ScheduleNative(0.f, std::move(fn), owner, /*real=*/true);
        });
}
