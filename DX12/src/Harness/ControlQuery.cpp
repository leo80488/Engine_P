#include "Harness/ControlQuery.h"

#if defined(WITH_HARNESS) && defined(WITH_CONTROL)

#include "Harness/ControlChannelJobs.h"
#include "Scripting/ScriptSystem.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include <sol/sol.hpp>
#include <nlohmann/json.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace Harness::ControlQuery
{
namespace
{
    // ==== the liveness hook ====
    // A count hook, because Lua has no time hook. 20000 instructions is well under a
    // millisecond of real work, so the deadline is observed with roughly that
    // granularity - fine for a 250 ms budget.
    //
    // It does NOT cover one failure mode: the hook fires between VM instructions,
    // so it cannot interrupt a single long-running C function (a query that calls
    // Scene.Load blocks for its full duration). That is why ControlChannel's own
    // service deadline is 30 s rather than tight - the two bounds cover different
    // things and both are needed.
    struct HookState
    {
        LONGLONG deadline = 0;
        bool     fired    = false;   // the budget ran out (tells `run`'s timeout from an error)
    };
    thread_local HookState t_hook;

    // The active path through the object graph, for cycle detection, indexed by
    // depth. The walker stops at min(Limits::maxDepth, kMaxDepthCeiling), so a caller
    // raising maxDepth past the ceiling cannot overrun it.
    constexpr int kMaxDepthCeiling = 32;
    thread_local const void* t_visiting[kMaxDepthCeiling] = {};

    void CountHook(lua_State* L, lua_Debug*)
    {
        LARGE_INTEGER now;
        ::QueryPerformanceCounter(&now);
        if (now.QuadPart > t_hook.deadline)
        {
            t_hook.fired = true;
            lua_sethook(L, nullptr, 0, 0);
            luaL_error(L, "control-query exceeded its time budget");
        }
    }

    // ==== JSON ====
    void EscapeInto(std::string& out, const char* s, size_t n, int maxLen)
    {
        out += '"';
        const bool truncated = static_cast<int>(n) > maxLen;
        if (truncated) n = static_cast<size_t>(maxLen);
        for (size_t i = 0; i < n; ++i)
        {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            switch (c)
            {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20)
                {
                    char u[8];
                    std::snprintf(u, sizeof(u), "\\u%04x", c);
                    out += u;
                }
                else out += static_cast<char>(c);
            }
        }
        if (truncated) out += "\\u2026";     // a visible ellipsis, not a lie
        out += '"';
    }

    void EscapeInto(std::string& out, const char* s, int maxLen)
    {
        EscapeInto(out, s, s ? std::char_traits<char>::length(s) : 0, maxLen);
    }

    void Number(std::string& out, lua_Number v)
    {
        // JSON has no inf/nan. A bare `inf` would make a reply the client cannot
        // parse, turning a legitimate answer into a transport error - so they become
        // strings.
        if (std::isnan(v))  { out += "\"nan\"";  return; }
        if (std::isinf(v))  { out += (v > 0) ? "\"inf\"" : "\"-inf\""; return; }
        // The SHORTEST text that parses back to the same double: 0.9, not
        // %.17g's 0.90000000000000002. Same value for a JSON reader, and a
        // reply of 20,000 floats is what lands in an agent's context.
        char buf[40];
        const auto r = std::to_chars(buf, buf + sizeof(buf), static_cast<double>(v));
        out.append(buf, r.ptr);
    }

    // Serialise the value at `idx` (absolute). Returns false once the total
    // budget is blown, so the caller can stop rather than build a reply that
    // will not fit the pipe.
    bool Serialize(lua_State* L, int idx, std::string& out,
                   int depth, const Limits& lim)
    {
        if (static_cast<int>(out.size()) > lim.maxTotalLen) return false;

        switch (lua_type(L, idx))
        {
        case LUA_TNIL:
        case LUA_TNONE:    out += "null"; return true;
        case LUA_TBOOLEAN: out += lua_toboolean(L, idx) ? "true" : "false"; return true;

        case LUA_TNUMBER:
            if (lua_isinteger(L, idx))
            {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%lld",
                              static_cast<long long>(lua_tointeger(L, idx)));
                out += buf;
            }
            else Number(out, lua_tonumber(L, idx));
            return true;

        case LUA_TSTRING:
        {
            size_t n = 0;
            const char* s = lua_tolstring(L, idx, &n);
            EscapeInto(out, s, n, lim.maxStringLen);
            return true;
        }

        case LUA_TTABLE:
            break;      // handled below

        default:
        {
            // Userdata, functions, threads. An engine handle is userdata, and the most
            // useful thing to say about it is what it prints as - sol2 gives most bound
            // types a __tostring. Reporting the type as well lets a caller tell "this is an
            // Entity" from "this is a table that happens to stringify".
            const char* tn = luaL_typename(L, idx);
            out += "{\"__type\":\"";
            out += tn;
            out += "\",\"__repr\":";
            if (luaL_callmeta(L, idx, "__tostring"))
            {
                size_t n = 0;
                const char* s = lua_tolstring(L, -1, &n);
                EscapeInto(out, s ? s : "", s ? n : 0, lim.maxStringLen);
                lua_pop(L, 1);
            }
            else
            {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%s: %p", tn, lua_topointer(L, idx));
                EscapeInto(out, buf, lim.maxStringLen);
            }
            out += "}";
            return true;
        }
        }

        // ---- table ----
        if (depth >= lim.maxDepth || depth >= kMaxDepthCeiling)
        {
            out += "\"<max-depth>\"";
            return true;
        }

        // A cycle is not exotic here: most engine tables reachable from the
        // globals refer back to something that refers to them. Without this the
        // walker recurses until the depth cap and produces a huge useless reply;
        // with it the caller sees exactly where the loop closed.
        //
        // Identity, not equality: two distinct tables with equal contents are
        // different nodes and both should be printed.
        const void* self = lua_topointer(L, idx);
        for (int d = 0; d < depth; ++d)
        {
            if (t_visiting[d] == self)
            {
                out += "\"<cycle>\"";
                return true;
            }
        }
        t_visiting[depth] = self;

        // Array or object? Lua cannot tell us directly. `lua_rawlen` > 0 is the
        // pragmatic test, and it matters: emitting an entity list as
        // {"1":...,"2":...} would force every caller to special-case it.
        const lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, idx));
        if (n > 0)
        {
            out += '[';
            lua_Integer emitted = 0;
            for (lua_Integer i = 1; i <= n && emitted < lim.maxEntries; ++i)
            {
                if (emitted) out += ',';
                lua_rawgeti(L, idx, static_cast<lua_Integer>(i));
                const bool ok = Serialize(L, lua_gettop(L), out, depth + 1, lim);
                lua_pop(L, 1);
                ++emitted;
                if (!ok) break;
            }
            if (n > emitted)
            {
                out += ",\"<+";
                out += std::to_string(static_cast<long long>(n - emitted));
                out += " more>\"";
            }
            out += ']';
            return static_cast<int>(out.size()) <= lim.maxTotalLen;
        }

        out += '{';
        int emitted = 0;
        bool overflow = false;
        lua_pushnil(L);
        while (lua_next(L, idx) != 0)
        {
            if (emitted >= lim.maxEntries || static_cast<int>(out.size()) > lim.maxTotalLen)
            {
                lua_pop(L, 2);          // key + value; ends the traversal
                overflow = true;
                break;
            }
            if (emitted) out += ',';

            // A JSON key must be a string. Numbers and everything else are stringified
            // rather than dropped - a dropped key is a silent lie about the table's
            // contents.
            const int kt = lua_type(L, -2);
            if (kt == LUA_TSTRING)
            {
                size_t kn = 0;
                const char* ks = lua_tolstring(L, -2, &kn);
                EscapeInto(out, ks, kn, 256);
            }
            else if (kt == LUA_TNUMBER)
            {
                // The key is COPIED before stringifying. lua_tolstring converts a number to
                // a string IN PLACE, and mutating the key on the stack breaks the lua_next
                // that is walking this table - an infinite loop, not an error.
                lua_pushvalue(L, -2);
                size_t kn = 0;
                const char* ks = lua_tolstring(L, -1, &kn);
                EscapeInto(out, ks, kn, 256);
                lua_pop(L, 1);
            }
            else
            {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "<%s>", luaL_typename(L, -2));
                EscapeInto(out, buf, 256);
            }
            out += ':';
            Serialize(L, lua_gettop(L), out, depth + 1, lim);
            lua_pop(L, 1);              // value; key stays for lua_next
            ++emitted;
        }
        if (overflow)
        {
            if (emitted) out += ',';
            out += "\"__truncated\":true";
        }
        out += '}';
        return static_cast<int>(out.size()) <= lim.maxTotalLen;
    }

    // ==== `run` ====
    // A batch read is the point of `run`, so its value limits are far wider than a
    // query's, and the pipe is not the ceiling: a reply over kRunPipeBudget goes to
    // a file, so with the requestId / log attachment the reply stays under the 64 KB
    // pipe out-buffer.
    Limits RunLimits()
    {
        Limits l;
        l.maxDepth     = 16;
        l.maxEntries   = 1 << 20;
        l.maxStringLen = 1 << 20;
        l.maxTotalLen  = 64 << 20;
        return l;
    }

    constexpr size_t kRunPipeBudget     = 40 * 1024;
    constexpr int    kRunDefaultBudgetMs = 2000;
    constexpr int    kRunMaxBudgetMs     = 10000;   // the channel's own service deadline is 30 s

    // `args` (JSON) -> one Lua value on the stack: objects become string-keyed
    // tables, arrays 1-based tables, null nil.
    void PushJson(lua_State* L, const nlohmann::json& j, int depth)
    {
        using V = nlohmann::json::value_t;
        if (depth > 32) { lua_pushnil(L); return; }
        switch (j.type())
        {
        case V::boolean:         lua_pushboolean(L, j.get<bool>() ? 1 : 0); break;
        case V::number_integer:  lua_pushinteger(L, static_cast<lua_Integer>(j.get<long long>())); break;
        case V::number_unsigned: lua_pushinteger(L, static_cast<lua_Integer>(j.get<unsigned long long>())); break;
        case V::number_float:    lua_pushnumber(L, j.get<double>()); break;
        case V::string:
        {
            const std::string& s = j.get_ref<const std::string&>();
            lua_pushlstring(L, s.data(), s.size());
            break;
        }
        case V::array:
        {
            lua_createtable(L, static_cast<int>(j.size()), 0);
            lua_Integer i = 1;
            for (const auto& e : j) { PushJson(L, e, depth + 1); lua_rawseti(L, -2, i++); }
            break;
        }
        case V::object:
        {
            lua_createtable(L, 0, static_cast<int>(j.size()));
            for (auto it = j.begin(); it != j.end(); ++it)
            {
                lua_pushlstring(L, it.key().data(), it.key().size());
                PushJson(L, it.value(), depth + 1);
                lua_rawset(L, -3);
            }
            break;
        }
        default:                 lua_pushnil(L); break;
        }
    }

    // pcall message handler: the error plus where it happened. A multi-line
    // chunk's error is useless without the line, and the chunk name is in it.
    int Traceback(lua_State* L)
    {
        const char* msg = lua_tostring(L, 1);
        luaL_traceback(L, L, msg ? msg : "(error object is not a string)", 1);
        return 1;
    }

    std::string RunError(const char* error, const std::string& message, const std::string& extra = {})
    {
        std::string out = "{\"ok\":false,\"verb\":\"run\",\"error\":\"";
        out += error;
        out += "\",\"message\":";
        EscapeInto(out, message.data(), message.size(), 4096);
        if (!extra.empty()) { out += ','; out += extra; }
        out += '}';
        return out;
    }

    bool WriteResultFile(const std::string& dir, const std::string& path, const std::string& body)
    {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(Sys::Utf8ToWide(dir)), ec);
        const std::wstring wpath = Sys::Utf8ToWide(path);
        const std::wstring wtmp  = wpath + L".tmp";
        {
            std::ofstream f(std::filesystem::path(wtmp), std::ios::binary | std::ios::trunc);
            if (!f) return false;
            f.write(body.data(), static_cast<std::streamsize>(body.size()));
            if (!f) return false;
        }
        // Temp + rename: a caller that reads the file the moment the reply
        // arrives never sees half of it.
        return ::MoveFileExW(wtmp.c_str(), wpath.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    }

    // ==== async `run`: the chunk as a coroutine ====
    // Wait.* are plain Lua over coroutine.yield: a yield of the marker below
    // suspends the run for n frames. Installed once per state, before a chunk (sync
    // or async) runs, so a SYNC run that waits gets a message that says what to do
    // instead of "attempt to index a nil value".
    constexpr const char* kWaitMarker = "__control_wait_frames";
    const char* const kWaitHelpers = R"lua(
Wait = Wait or {}
local gameFrames = Wait.Frames   -- Engine.Run's (ScriptSystem_Timers.cpp): a task keeps it
Wait.Frames = function(n)
  if gameFrames and __wait_tasks and __wait_tasks[coroutine.running()] then return gameFrames(n) end
  n = math.floor(tonumber(n) or 1)
  if n < 1 then n = 1 end
  if not coroutine.isyieldable() then
    error('Wait.Frames needs an async run: run {"async": true, ...} - this chunk runs inside one frame', 2)
  end
  return coroutine.yield('__control_wait_frames', n)
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
    lua_State* g_waitInstalledFor = nullptr;

    struct AsyncRun
    {
        std::uint64_t jobId      = 0;
        std::uint32_t requestId  = 0;
        lua_State*    main       = nullptr;   // the state the coroutine belongs to
        lua_State*    co         = nullptr;
        int           ref        = LUA_NOREF; // registry ref: keeps the coroutine alive
        int           firstArgs  = -1;        // >= 0 until the first resume: values on co's stack
        std::uint64_t startFrame = 0;
        std::uint64_t resumeAt   = 0;
        std::uint64_t lastFrame  = 0;         // startFrame + maxFrames
        int           budgetMs   = kRunDefaultBudgetMs;
        int           resumes    = 0;
        double        luaMs      = 0;
        std::string   name, resultDir;
        std::vector<CapturedLogLine> log;     // what it logged, bounded
    };
    std::vector<AsyncRun> g_async;             // main thread only
    constexpr std::size_t   kMaxAsync            = 8;
    constexpr std::uint64_t kAsyncDefaultFrames  = 3600;
    constexpr std::uint64_t kAsyncMaxFrames      = 216000;   // an hour at 60 fps
    constexpr std::size_t   kAsyncMaxLogLines    = 64;

    void EnsureWaitHelpers(lua_State* L)
    {
        if (g_waitInstalledFor == L) return;
        if (luaL_dostring(L, kWaitHelpers) != LUA_OK)
        {
            LOG_ERROR("ControlQuery: installing Wait.* failed: %s", lua_tostring(L, -1));
            lua_pop(L, 1);
            return;
        }
        g_waitInstalledFor = L;
    }

    std::string LogJson(const std::vector<CapturedLogLine>& lines)
    {
        std::string out = "[";
        for (std::size_t i = 0; i < lines.size(); ++i)
        {
            if (i) out += ',';
            const char* lv = lines[i].level == LogLevel::Error   ? "error"
                           : lines[i].level == LogLevel::Warning ? "warning" : "info";
            out += "{\"level\":\"";
            out += lv;
            out += "\",\"text\":";
            EscapeInto(out, lines[i].message.data(), lines[i].message.size(), 2048);
            out += '}';
        }
        out += ']';
        return out;
    }

    std::vector<std::string> LogLines(const std::vector<CapturedLogLine>& lines)
    {
        std::vector<std::string> out;
        for (const CapturedLogLine& l : lines)
            if (l.level == LogLevel::Error || l.level == LogLevel::Warning) out.push_back(l.message);
        return out;
    }

    // One resume. Returns true when the run is over (its job is terminal and
    // its registry ref released) - the caller removes it.
    bool ResumeOne(AsyncRun& run, lua_State* L, std::uint64_t frame)
    {
        std::vector<CapturedLogLine> captured;
        int nres = 0, st = LUA_OK;
        LARGE_INTEGER freq, t0, t1;
        ::QueryPerformanceFrequency(&freq);
        ::QueryPerformanceCounter(&t0);
        {
            Logger::ScopedContext ctx(LogSource::Control, run.requestId, &captured, kAsyncMaxLogLines);
            int nargs = 0;
            if (run.firstArgs >= 0) { nargs = run.firstArgs; run.firstArgs = -1; }
            else { lua_pushinteger(run.co, static_cast<lua_Integer>(frame)); nargs = 1; }   // what Wait.Frames returns
            t_hook.deadline = t0.QuadPart + (freq.QuadPart * run.budgetMs) / 1000;
            t_hook.fired    = false;
            lua_sethook(run.co, CountHook, LUA_MASKCOUNT, 20000);
            st = lua_resume(run.co, L, nargs, &nres);
            lua_sethook(run.co, nullptr, 0, 0);
        }
        ::QueryPerformanceCounter(&t1);
        ++run.resumes;
        run.luaMs += static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
        for (CapturedLogLine& l : captured)
            if (run.log.size() < kAsyncMaxLogLines) run.log.push_back(std::move(l));

        char head[160];
        auto stats = [&]()
        {
            std::snprintf(head, sizeof(head), "\"frames\":%llu,\"resumes\":%d,\"luaMs\":%.2f",
                          static_cast<unsigned long long>(frame - run.startFrame), run.resumes, run.luaMs);
            return std::string(head);
        };
        auto finish = [&]()
        {
            luaL_unref(L, LUA_REGISTRYINDEX, run.ref);
            run.ref = LUA_NOREF;
            return true;
        };

        if (st == LUA_YIELD)
        {
            // Only Wait.* may yield this coroutine: anything else is a chunk
            // that yields from its own body, which we cannot resume sensibly.
            std::uint64_t n = 0;
            if (nres == 2 && lua_type(run.co, -2) == LUA_TSTRING &&
                std::strcmp(lua_tostring(run.co, -2), kWaitMarker) == 0 && lua_isinteger(run.co, -1))
                n = static_cast<std::uint64_t>(std::max<lua_Integer>(1, lua_tointeger(run.co, -1)));
            lua_pop(run.co, nres);
            if (n == 0)
            {
                Jobs::Fail(run.jobId, "bad_yield", "the chunk yielded outside Wait.Frames / Wait.Until "
                           "(coroutine.yield in the run's own body cannot be resumed)",
                           LogLines(run.log), "{" + stats() + "}");
                return finish();
            }
            run.resumeAt = frame + n;
            if (run.resumeAt > run.lastFrame)
            {
                Jobs::Fail(run.jobId, "timeout",
                           "the async run would wait past its maxFrames (" +
                           std::to_string(run.lastFrame - run.startFrame) + ") - it is stopped where it was; "
                           "nothing it already did is rolled back", LogLines(run.log), "{" + stats() + "}");
                return finish();
            }
            return false;
        }

        if (st != LUA_OK)
        {
            size_t n = 0;
            const char* msg = lua_tolstring(run.co, -1, &n);
            std::string m(msg ? msg : "?", msg ? n : 1);
            luaL_traceback(L, run.co, m.c_str(), 0);   // the coroutine's stack is still there
            const char* tb = lua_tostring(L, -1);
            std::vector<std::string> lines = LogLines(run.log);
            if (tb) lines.push_back(tb);
            lua_pop(L, 1);
            if (t_hook.fired)
                Jobs::Fail(run.jobId, "timeout", "a resume exceeded its " + std::to_string(run.budgetMs) +
                           " ms budget - stopped where it was; nothing it already did is rolled back",
                           lines, "{" + stats() + "}");
            else
                Jobs::Fail(run.jobId, "runtime", m, lines, "{" + stats() + "}");
            return finish();
        }

        // Finished: every return value, like a sync run.
        const int top = lua_gettop(run.co);
        const int firstValue = top - nres + 1;
        const Limits lim = RunLimits();
        for (auto& v : t_visiting) v = nullptr;
        std::string values = "[";
        bool complete = true;
        for (int i = firstValue; i <= top && complete; ++i)
        {
            if (i > firstValue) values += ',';
            complete = Serialize(run.co, i, values, 0, lim);
        }
        values += ']';
        lua_settop(run.co, 0);

        std::string out = "{" + stats() + ",\"count\":" + std::to_string(nres);
        if (!complete) out += ",\"truncated\":true";
        out += ",\"log\":" + LogJson(run.log);
        if (values.size() <= kRunPipeBudget)
            out += ",\"values\":" + values + "}";
        else
        {
            const std::string path = run.resultDir + "/run_job" + std::to_string(run.jobId) + ".json";
            const std::string body = "{\"values\":" + values + (complete ? "}" : ",\"truncated\":true}");
            if (!WriteResultFile(run.resultDir, path, body))
            {
                Jobs::Fail(run.jobId, "result_too_large", "the result (" + std::to_string(body.size()) +
                           " bytes) could not be written to '" + path + "'", LogLines(run.log), "{" + stats() + "}");
                return finish();
            }
            out += ",\"bytes\":" + std::to_string(body.size()) + ",\"resultFile\":";
            EscapeInto(out, path.data(), path.size(), 4096);
            out += '}';
        }
        Jobs::Succeed(run.jobId, out);
        return finish();
    }
}

std::string Evaluate(ScriptSystem& script, const std::string& expr,
                     const Limits& lim)
{
    sol::state* lua = script.GetLua();
    if (!lua)
        return "{\"ok\":false,\"error\":\"no_lua_state\"}";

    lua_State* L = lua->lua_state();
    const int top = lua_gettop(L);

    // `return (...)` - the parentheses matter twice over. They make a bare
    // expression legal after `return`, and they truncate a multi-value call to ONE
    // value, so `query Foo()` cannot flood the reply with a hundred returns.
    //
    // The newline before `)` is deliberate: without it a trailing line comment in
    // the expression would swallow the closing paren.
    std::string chunk = "return (";
    chunk += expr;
    chunk += "\n)";

    // Mode "t": TEXT ONLY, never "bt". Lua's bytecode loader is not hardened
    // against hostile input - a crafted chunk corrupts memory - so accepting
    // bytecode over a pipe would make a query channel a memory-safety problem
    // regardless of who is on the other end.
    if (luaL_loadbufferx(L, chunk.c_str(), chunk.size(), "=control-query", "t") != LUA_OK)
    {
        std::string out = "{\"ok\":false,\"error\":\"syntax\",\"message\":";
        size_t n = 0;
        const char* msg = lua_tolstring(L, -1, &n);
        EscapeInto(out, msg ? msg : "?", msg ? n : 1, 1024);
        out += "}";
        lua_settop(L, top);
        return out;
    }

    LARGE_INTEGER freq, now;
    ::QueryPerformanceFrequency(&freq);
    ::QueryPerformanceCounter(&now);
    t_hook.deadline = now.QuadPart + (freq.QuadPart * lim.timeoutMs) / 1000;
    lua_sethook(L, CountHook, LUA_MASKCOUNT, 20000);

    const int rc = lua_pcall(L, 0, 1, 0);

    // Clear the hook on BOTH paths. A count hook left installed would fire inside
    // every later script the engine runs this frame, against a deadline that has
    // already passed - every gameplay script would die with "exceeded its time
    // budget" and the cause would look like anything except a control query.
    lua_sethook(L, nullptr, 0, 0);

    std::string out;
    if (rc != LUA_OK)
    {
        out = "{\"ok\":false,\"error\":\"runtime\",\"message\":";
        size_t n = 0;
        const char* msg = lua_tolstring(L, -1, &n);
        EscapeInto(out, msg ? msg : "?", msg ? n : 1, 2048);
        out += "}";
    }
    else
    {
        for (auto& v : t_visiting) v = nullptr;
        out = "{\"ok\":true,\"verb\":\"query\",\"value\":";
        const bool complete = Serialize(L, lua_gettop(L), out, 0, lim);
        out += complete ? "}" : ",\"truncated\":true}";
    }

    lua_settop(L, top);
    return out;
}

std::string Run(ScriptSystem& script, const std::string& request,
                std::uint32_t requestId, std::uint64_t frame,
                const std::string& resultDir)
{
    sol::state* lua = script.GetLua();
    if (!lua)
        return RunError("no_lua_state", "the script system has no Lua state");

    // ---- the request: a raw chunk, or a JSON object ----
    std::string chunk, file, name;
    int budgetMs = kRunDefaultBudgetMs;
    bool async = false;                        // a coroutine, answered with a jobId
    long long maxFrames = static_cast<long long>(kAsyncDefaultFrames);
    nlohmann::json args;                       // null = the chunk gets no `...`
    const size_t first = request.find_first_not_of(" \t\r\n");
    if (first != std::string::npos && request[first] == '{')
    {
        nlohmann::json j;
        try { j = nlohmann::json::parse(request); }
        catch (const std::exception& e)
        {
            return RunError("bad_request", std::string("the run request is not valid JSON: ") + e.what());
        }
        if (!j.is_object())
            return RunError("bad_request", "the run request must be a JSON object");
        // Closed vocabulary: a typo'd "budgetMS" must not silently mean the default.
        for (auto it = j.begin(); it != j.end(); ++it)
            if (it.key() != "chunk" && it.key() != "file" && it.key() != "args" &&
                it.key() != "budgetMs" && it.key() != "name" && it.key() != "async" &&
                it.key() != "maxFrames")
                return RunError("bad_request", "unknown field \"" + it.key() +
                                "\" (known: chunk, file, args, budgetMs, name, async, maxFrames)");
        if (j.contains("chunk") == j.contains("file"))
            return RunError("bad_request", "give exactly one of \"chunk\" or \"file\"");
        try
        {
            if (j.contains("chunk")) chunk = j["chunk"].get<std::string>();
            else                     file  = j["file"].get<std::string>();
            if (j.contains("budgetMs")) budgetMs = j["budgetMs"].get<int>();
            if (j.contains("name"))     name     = j["name"].get<std::string>();
            if (j.contains("args"))     args     = j["args"];
            if (j.contains("async"))    async    = j["async"].get<bool>();
            if (j.contains("maxFrames")) maxFrames = j["maxFrames"].get<long long>();
        }
        catch (const std::exception& e)
        {
            return RunError("bad_request", std::string("a field has the wrong type: ") + e.what());
        }
    }
    else
        chunk = request;

    if (!file.empty())
    {
        if (!Sys::ReadFileText(file, chunk))
            return RunError("file", "cannot read '" + file + "'");
        if (name.empty())
            name = file.substr(file.find_last_of("/\\") + 1);
    }
    if (name.empty())
        name = "run#" + std::to_string(requestId);
    bool budgetClamped = false;
    if (budgetMs < 1) budgetMs = 1;
    if (budgetMs > kRunMaxBudgetMs) { budgetMs = kRunMaxBudgetMs; budgetClamped = true; }

    lua_State* L = lua->lua_state();
    EnsureWaitHelpers(L);

    if (async)
    {
        if (maxFrames < 1) maxFrames = 1;
        if (static_cast<std::uint64_t>(maxFrames) > kAsyncMaxFrames) maxFrames = static_cast<long long>(kAsyncMaxFrames);
        if (g_async.size() >= kMaxAsync)
            return RunError("busy", std::to_string(kMaxAsync) + " async runs are already waiting - `job wait` one first");
        const std::string chunkName = "=" + name;
        const int top = lua_gettop(L);
        lua_State* co = lua_newthread(L);
        const int ref = luaL_ref(L, LUA_REGISTRYINDEX);   // pops the thread
        if (luaL_loadbufferx(L, chunk.c_str(), chunk.size(), chunkName.c_str(), "t") != LUA_OK)
        {
            size_t n = 0;
            const char* msg = lua_tolstring(L, -1, &n);
            const std::string m(msg ? msg : "?", msg ? n : 1);
            lua_settop(L, top);
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
            return RunError("syntax", m);
        }
        int nargs = 0;
        if (!args.is_null()) { PushJson(L, args, 0); nargs = 1; }
        lua_xmove(L, co, nargs + 1);                     // the function and its `...`
        lua_settop(L, top);

        AsyncRun run;
        run.jobId      = Jobs::TryCreate("run", name, requestId);
        if (!run.jobId)
        {
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
            return RunError("busy", "active job admission limit reached; wait for accepted work");
        }
        run.requestId  = requestId;
        run.main       = L;
        run.co         = co;
        run.ref        = ref;
        run.firstArgs  = nargs;
        run.startFrame = frame;
        run.lastFrame  = frame + static_cast<std::uint64_t>(maxFrames);
        run.budgetMs   = budgetMs;
        run.name       = name;
        run.resultDir  = resultDir;
        Jobs::Start(run.jobId);
        // The first slice runs NOW, in this request's frame, like a sync run.
        const bool over = ResumeOne(run, L, frame);
        const std::uint64_t jobId = run.jobId;
        if (!over) g_async.push_back(std::move(run));
        char out[256];
        std::snprintf(out, sizeof(out),
            "{\"ok\":true,\"verb\":\"run\",\"async\":true,\"jobId\":%llu,\"frame\":%llu,"
            "\"budgetMs\":%d,\"maxFrames\":%lld,\"finished\":%s}",
            static_cast<unsigned long long>(jobId), static_cast<unsigned long long>(frame), budgetMs,
            maxFrames, over ? "true" : "false");
        return std::string(out);
    }

    const int top = lua_gettop(L);

    // Text only ("t"), never bytecode - the same reason as Evaluate.
    lua_pushcfunction(L, Traceback);           // the message handler, at top + 1
    const std::string chunkName = "=" + name;
    if (luaL_loadbufferx(L, chunk.c_str(), chunk.size(), chunkName.c_str(), "t") != LUA_OK)
    {
        size_t n = 0;
        const char* msg = lua_tolstring(L, -1, &n);
        const std::string m(msg ? msg : "?", msg ? n : 1);
        lua_settop(L, top);
        return RunError("syntax", m);
    }
    int nargs = 0;
    if (!args.is_null()) { PushJson(L, args, 0); nargs = 1; }

    LARGE_INTEGER freq, t0, t1;
    ::QueryPerformanceFrequency(&freq);
    ::QueryPerformanceCounter(&t0);
    t_hook.deadline = t0.QuadPart + (freq.QuadPart * budgetMs) / 1000;
    t_hook.fired    = false;
    lua_sethook(L, CountHook, LUA_MASKCOUNT, 20000);
    const int rc = lua_pcall(L, nargs, LUA_MULTRET, top + 1);
    lua_sethook(L, nullptr, 0, 0);             // both paths - see Evaluate
    ::QueryPerformanceCounter(&t1);
    const double ms = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 /
                      static_cast<double>(freq.QuadPart);

    char head[192];
    std::snprintf(head, sizeof(head), "\"ms\":%.2f,\"frame\":%llu,\"budgetMs\":%d%s",
                  ms, static_cast<unsigned long long>(frame), budgetMs,
                  budgetClamped ? ",\"budgetClamped\":true" : "");

    if (rc != LUA_OK)
    {
        size_t n = 0;
        const char* msg = lua_tolstring(L, -1, &n);
        const std::string m(msg ? msg : "?", msg ? n : 1);
        lua_settop(L, top);
        if (t_hook.fired)
            return RunError("timeout", "the chunk exceeded its " + std::to_string(budgetMs) + " ms budget",
                            std::string(head) + ",\"sideEffects\":\"kept - a timeout stops the chunk where "
                            "it was; nothing it already did is rolled back\"");
        return RunError("runtime", m, head);
    }

    // ---- every return value ----
    const int firstValue = top + 2;            // above the message handler
    const int lastValue  = lua_gettop(L);
    const Limits lim = RunLimits();
    for (auto& v : t_visiting) v = nullptr;
    std::string values = "[";
    bool complete = true;
    for (int i = firstValue; i <= lastValue && complete; ++i)
    {
        if (i > firstValue) values += ',';
        complete = Serialize(L, i, values, 0, lim);
    }
    values += ']';
    const int count = lastValue >= firstValue ? lastValue - firstValue + 1 : 0;
    lua_settop(L, top);

    std::string out = "{\"ok\":true,\"verb\":\"run\",";
    out += head;
    out += ",\"count\":" + std::to_string(count);
    if (!complete) out += ",\"truncated\":true";
    if (values.size() <= kRunPipeBudget)
    {
        out += ",\"values\":";
        out += values;
        out += '}';
        return out;
    }

    // Too big for the pipe: the whole result goes to a file the caller reads.
    const std::string path = resultDir + "/run_" + std::to_string(requestId) + ".json";
    const std::string body = "{\"values\":" + values + (complete ? "}" : ",\"truncated\":true}");
    if (!WriteResultFile(resultDir, path, body))
        return RunError("result_too_large", "the result (" + std::to_string(body.size()) +
                        " bytes) is larger than the pipe allows and could not be written to '" +
                        path + "'", head);
    out += ",\"bytes\":" + std::to_string(body.size());
    out += ",\"resultFile\":";
    EscapeInto(out, path.data(), path.size(), 4096);
    out += '}';
    return out;
}

void TickAsync(ScriptSystem& script, std::uint64_t frame, const std::function<void()>& beforeResume)
{
    if (g_async.empty()) return;
    bool due = false;
    for (const AsyncRun& r : g_async) due = due || frame >= r.resumeAt;
    if (!due) return;

    sol::state* lua = script.GetLua();
    lua_State* L = lua ? lua->lua_state() : nullptr;
    if (beforeResume) beforeResume();
    ScriptSystem::WorldBorrow borrow(script);   // the same borrow a sync run needs
    for (std::size_t i = 0; i < g_async.size();)
    {
        AsyncRun& r = g_async[i];
        if (frame < r.resumeAt) { ++i; continue; }
        bool over = false;
        if (L != r.main)
        {
            // The Lua state it lived in is gone: nothing to resume, nothing to unref.
            Jobs::Fail(r.jobId, "lua_state_reset", "the script system's Lua state was replaced while the run waited",
                       LogLines(r.log));
            over = true;
        }
        else
            over = ResumeOne(r, L, frame);
        if (over) g_async.erase(g_async.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
    }
}

void AbandonAsync()
{
    for (AsyncRun& r : g_async)
        Jobs::Fail(r.jobId, "shutdown", "the control channel stopped while the async run waited", LogLines(r.log));
    g_async.clear();
}

}  // namespace Harness::ControlQuery

#endif
