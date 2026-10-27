#include "Harness/ControlChannel.h"

#if defined(WITH_HARNESS) && defined(WITH_CONTROL)

#include "Harness/ControlQuery.h"
#include "Diagnostics/Profiler.h"
#include "Diagnostics/ProfilerCollector.h"
#include "Diagnostics/ProfilerFrame.h"
#include "Harness/GameFlowDump.h"
#include "Harness/RenderGraphDump.h"
#include "RenderGraph/FrameCapture.h"
#include "Harness/ControlChannelJobs.h"
#include "Harness/CaptureRequests.h"
#include "Resource/LuaSettingsBindings.h"   // the `setting` verb: Resource::ServeSettingVerb
#include "Scripting/LuaDoc.h"         // Control.* is described as it is bound
#include "Scripting/ScriptSystem.h"   // WorldBorrow is a nested type: fwd-decl is not enough
#include "System/PathUtil.h"
#include "System/BootTimeline.h"
#include "System/Check.h"
#include "System/CommandLine.h"
#include "System/HostPolicy.h"
#include "System/Log.h"
#include "System/Stage.h"

#include <sol/sol.hpp>
#include <nlohmann/json.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace Harness
{
namespace
{
    constexpr DWORD kInBuffer  = 8  * 1024;   // also the hard cap on a command
    constexpr DWORD kOutBuffer = 64 * 1024;

    // How long the pipe thread waits for the main thread to service a command.
    // A scene load blocks the frame loop for ~20 s, so a shorter deadline would
    // report a healthy engine as unresponsive during every load; a longer one makes
    // a wedged engine look merely slow. -control_timeout_ms=N (250..30000) shortens
    // it, for the tests of what a caller then sees
    // (tests/unit/test_workflow_recovery.py).
    constexpr DWORD kServiceTimeoutMs = 30000;

    // RAII for the impersonation window. RevertToSelf MUST run on every path:
    // leaving the thread impersonating a client would silently change the
    // privileges of everything that thread touches afterwards.
    struct ImpersonationScope
    {
        bool active = false;
        explicit ImpersonationScope(HANDLE pipe)
        {
            active = ::ImpersonateNamedPipeClient(pipe) != FALSE;
        }
        ~ImpersonationScope() { if (active) ::RevertToSelf(); }
        ImpersonationScope(const ImpersonationScope&)            = delete;
        ImpersonationScope& operator=(const ImpersonationScope&) = delete;
    };

    // The first word of a command ("run", "query", "editor", ...): the late
    // job's detail. Never the payload - it can be kilobytes of Lua.
    std::string VerbOf(const std::string& command)
    {
        const size_t sp = command.find_first_of(" \t\r\n");
        return command.substr(0, sp == std::string::npos ? command.size() : sp);
    }

    // A request that timed out AFTER it started (the late job, see
    // ServeCommand): its reply completes the job - "succeeded" with the reply
    // as the output when it was ok, else "failed" with the reply's error code,
    // message and log lines (and the reply as the output too).
    void CompleteLateJob(std::uint64_t job, const std::string& reply)
    {
        const nlohmann::json j = nlohmann::json::parse(reply, nullptr, false);
        if (!j.is_object())
        {
            Jobs::Fail(job, "bad_reply", "the late reply was not a JSON object", {});
            return;
        }
        if (j.value("ok", false))
        {
            Jobs::Succeed(job, reply);
            return;
        }
        std::vector<std::string> log;
        if (const auto it = j.find("log"); it != j.end() && it->is_array())
            for (const auto& line : *it)
                if (line.is_object()) log.push_back(line.value("text", std::string()));
        const std::string message = j.contains("message") && j["message"].is_string()
            ? j["message"].get<std::string>() : j.value("hint", std::string());
        Jobs::Fail(job, j.value("error", std::string("error")), message, std::move(log), reply);
    }

    std::string LastErrorText(DWORD e)
    {
        char buf[256] = {};
        ::FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                         nullptr, e, 0, buf, sizeof(buf) - 1, nullptr);
        std::string s(buf);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        return s + " (" + std::to_string(e) + ")";
    }

    // The client's executable ("python.exe"), for the editor's status bar and
    // the connect / disconnect log lines. Informational only, never a gate -
    // the DACL and the SID check decide who may connect. Empty when the
    // process cannot be opened.
    std::string ProcessImageName(ULONG pid)
    {
        HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) return {};
        wchar_t path[MAX_PATH] = {};
        DWORD n = MAX_PATH;
        const BOOL ok = ::QueryFullProcessImageNameW(h, 0, path, &n);
        ::CloseHandle(h);
        if (!ok) return {};
        const std::wstring full(path, n);
        const size_t slash = full.find_last_of(L"\\/");
        return Sys::WideToUtf8(slash == std::wstring::npos ? full : full.substr(slash + 1));
    }

    // The current process's user SID, as a heap copy the caller owns.
    bool CurrentUserSid(std::vector<BYTE>& out)
    {
        HANDLE tok = nullptr;
        if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &tok))
            return false;
        DWORD need = 0;
        ::GetTokenInformation(tok, TokenUser, nullptr, 0, &need);
        if (need == 0) { ::CloseHandle(tok); return false; }
        out.assign(need, 0);
        const BOOL ok = ::GetTokenInformation(tok, TokenUser, out.data(), need, &need);
        ::CloseHandle(tok);
        return ok != FALSE;
    }

    std::string JsonStr(const std::string& v)
    {
        std::string o = "\"";
        for (char c : v)
        {
            if (c == '"' || c == '\\') { o.push_back('\\'); o.push_back(c); }
            else if (static_cast<unsigned char>(c) < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
            else o.push_back(c);
        }
        return o + "\"";
    }

    const char* LevelName(LogLevel level)
    {
        switch (level)
        {
        case LogLevel::Success: return "success";
        case LogLevel::Info:    return "info";
        case LogLevel::Warning: return "warning";
        case LogLevel::Error:   return "error";
        default:                return "info";
        }
    }

    // Severity rank for `log level=`: success and info are one tier.
    int LevelRank(LogLevel level)
    {
        return level == LogLevel::Error ? 2 : level == LogLevel::Warning ? 1 : 0;
    }

    std::string OneLine(const std::string& s, std::size_t max)
    {
        std::string out;
        for (char c : s)
        {
            if (out.size() >= max) { out += "..."; break; }
            out.push_back(c == '\n' || c == '\r' || c == '\t' ? ' ' : c);
        }
        return out;
    }

    // `log [since=<seq>] [max=<n>] [level=<min>] [source=<name>] [tag=<TAG>]`.
    // Served ON THE PIPE THREAD, like `status` and `profile`: the logger's record
    // ring is mutex-guarded and knows nothing of World, so this answers while the
    // frame loop is blocked (a scene load, a hang) - exactly when "what did the
    // engine say last" is the question.
    //
    // `next` is the last seq EXAMINED (filtered-out lines are still examined): pass
    // it back as `since` to continue without re-reading. `dropped` counts lines that
    // left the ring before this reader asked. Without `since` it is a TAIL: the last
    // `max` lines, or with a filter the last `max` MATCHING lines anywhere in the
    // ring.
    std::string ServeLogVerb(const std::string& argsIn)
    {
        std::vector<std::string> a;
        {
            size_t i = 0;
            while (i < argsIn.size())
            {
                while (i < argsIn.size() && std::isspace(static_cast<unsigned char>(argsIn[i]))) ++i;
                size_t j = i;
                while (j < argsIn.size() && !std::isspace(static_cast<unsigned char>(argsIn[j]))) ++j;
                if (j > i) a.push_back(argsIn.substr(i, j - i));
                i = j;
            }
        }
        auto refuse = [](const char* code, const std::string& tok)
        {
            return std::string("{\"ok\":false,\"verb\":\"log\",\"error\":\"") + code +
                   "\",\"argument\":" + JsonStr(tok) +
                   ",\"usage\":\"log [since=<seq>] [max=<1..1000>] [level=success|info|warning|error] "
                   "[source=engine|lua|console|control] [tag=<TAG>]\"}";
        };

        std::uint64_t since = 0;
        bool          sinceSet = false;
        std::size_t   maxN = 200;
        int           minRank = 0;
        int           wantSource = -1;
        std::string   tag;
        for (const std::string& tok : a)
        {
            const size_t eq = tok.find('=');
            if (eq == std::string::npos) return refuse("unknown_argument", tok);
            const std::string key = tok.substr(0, eq);
            const std::string val = tok.substr(eq + 1);
            if (key == "since")
            {
                char* end = nullptr;
                since = std::strtoull(val.c_str(), &end, 10);
                if (val.empty() || (end && *end)) return refuse("bad_value", tok);
                sinceSet = true;
            }
            else if (key == "max")
            {
                char* end = nullptr;
                const long n = std::strtol(val.c_str(), &end, 10);
                if (val.empty() || (end && *end) || n < 1) return refuse("bad_value", tok);
                maxN = static_cast<std::size_t>(n > 1000 ? 1000 : n);
            }
            else if (key == "level")
            {
                if      (val == "success" || val == "info") minRank = 0;
                else if (val == "warning")                  minRank = 1;
                else if (val == "error")                    minRank = 2;
                else return refuse("bad_value", tok);
            }
            else if (key == "source")
            {
                wantSource = -1;
                for (int s = 0; s < static_cast<int>(LogSource::Count); ++s)
                    if (val == LogSourceName(static_cast<LogSource>(s))) wantSource = s;
                if (wantSource < 0) return refuse("bad_value", tok);
            }
            else if (key == "tag")
                tag = val;
            else
                return refuse("unknown_argument", tok);
        }

        Logger& lg = Logger::Get();
        const std::uint64_t latest = lg.LatestSeq();
        const bool filtered = minRank > 0 || wantSource >= 0 || !tag.empty();
        // No `since`: the tail. Unfiltered, the last max lines; FILTERED, the last
        // max lines that MATCH, found in the whole ring - `log tag=[X]` is "the latest
        // [X] lines", not "whichever of the latest max lines happen to be [X]".
        if (!sinceSet) since = (!filtered && latest > maxN) ? latest - maxN : 0;

        // At most one ring's worth is examined: a filter may reject most of it.
        std::vector<LogRecord> recs;
        std::uint64_t first = 0;
        lg.CopyRecordsSince(since, recs, Logger::kRingCapacity, &first);
        const std::uint64_t dropped = (!sinceSet && filtered) ? 0
                                    : (first > since + 1) ? first - since - 1 : 0;

        std::vector<const LogRecord*> match;
        std::uint64_t next = since;
        bool          truncated = false;
        for (const LogRecord& r : recs)
        {
            const bool want = LevelRank(r.level) >= minRank &&
                              (wantSource < 0 || static_cast<int>(r.source) == wantSource) &&
                              (tag.empty() || r.Tag() == tag);
            if (want && sinceSet && match.size() >= maxN) { truncated = true; break; }
            next = r.seq;   // examined (wanted or not)
            if (want) match.push_back(&r);
        }
        if (!sinceSet && match.size() > maxN)
            match.erase(match.begin(), match.end() - static_cast<std::ptrdiff_t>(maxN));

        std::vector<std::string> ones;
        ones.reserve(match.size());
        for (const LogRecord* r : match)
        {
            char head[160];
            std::snprintf(head, sizeof(head),
                "{\"seq\":%llu,\"frame\":%llu,\"level\":\"%s\",\"source\":\"%s\",\"thread\":%u,\"req\":%u,\"text\":",
                static_cast<unsigned long long>(r->seq), static_cast<unsigned long long>(r->frame),
                LevelName(r->level), LogSourceName(r->source), r->thread, r->requestId);
            ones.push_back(head + JsonStr(r->text) + "}");
        }

        // 44 KB of records keeps the reply inside the 48 KB budget ControlQuery
        // established for this 64 KB pipe. A tail keeps its NEWEST lines; a
        // `since` read keeps its oldest and resumes after the last one sent.
        constexpr std::size_t kBudget = 44 * 1024;
        std::size_t from = 0, to = ones.size(), total = 0;
        if (!sinceSet)
        {
            std::size_t i = ones.size();   // walk back from the newest
            while (i > 0 && total + ones[i - 1].size() + 1 <= kBudget) total += ones[--i].size() + 1;
            from = i;
            if (from > 0) truncated = true;
        }
        else
        {
            std::size_t i = 0;
            while (i < ones.size() && total + ones[i].size() + 1 <= kBudget) total += ones[i++].size() + 1;
            if (i < ones.size())
            {
                truncated = true;
                next = i > 0 ? match[i - 1]->seq : since;
            }
            to = i;
        }
        std::string body;
        for (std::size_t i = from; i < to; ++i)
        {
            if (i > from) body.push_back(',');
            body += ones[i];
        }
        const std::size_t count = to - from;

        char head[320];
        std::snprintf(head, sizeof(head),
            "{\"ok\":true,\"verb\":\"log\",\"latest\":%llu,\"first\":%llu,\"dropped\":%llu,"
            "\"next\":%llu,\"count\":%zu,\"truncated\":%s,\"records\":[",
            static_cast<unsigned long long>(latest), static_cast<unsigned long long>(first),
            static_cast<unsigned long long>(dropped), static_cast<unsigned long long>(next),
            count, truncated ? "true" : "false");
        return head + body + "]}";
    }

    // A request's OWN log lines back in its reply: `requestId` plus `log`
    // [{level, text}] (bounded), inserted before the closing brace of the JSON
    // object every verb returns. A reply that is not an object passes through.
    std::string AttachRequestLog(std::string reply, std::uint32_t id,
                                 const std::vector<CapturedLogLine>& lines, std::size_t dropped)
    {
        while (!reply.empty() && std::isspace(static_cast<unsigned char>(reply.back())))
            reply.pop_back();
        if (reply.size() < 2 || reply.front() != '{' || reply.back() != '}')
            return reply;

        constexpr std::size_t kBudget = 8 * 1024;
        std::string add = (reply.size() > 2 ? "," : "");
        add += "\"requestId\":" + std::to_string(id) + ",\"log\":[";
        std::size_t used = 0, kept = 0;
        for (const CapturedLogLine& l : lines)
        {
            const std::string text = l.message.size() > 400 ? l.message.substr(0, 400) + "..." : l.message;
            const std::string one = std::string("{\"level\":\"") + LevelName(l.level) +
                                    "\",\"text\":" + JsonStr(text) + "}";
            if (used + one.size() > kBudget) break;
            if (kept) add.push_back(',');
            add += one;
            used += one.size();
            ++kept;
        }
        add += "]";
        const std::size_t omitted = (lines.size() - kept) + dropped;
        if (omitted) add += ",\"logOmitted\":" + std::to_string(omitted);
        reply.insert(reply.size() - 1, add);
        return reply;
    }

    double SteadyMs()
    {
        using namespace std::chrono;
        return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
    }

    // `profile [status | level <name> | snapshot | window begin|end <name> |
    // capture <frames> [name]]`. Served ON THE PIPE THREAD, like `status`:
    // every profiler call below is thread-safe (atomics, the record ring's
    // lock, the collector's request queue), so it answers while the frame loop
    // is blocked - which is exactly when "capture the next 60 frames" is the
    // question worth asking.
    std::string ServeProfileVerb(const std::string& argsIn)
    {
        using namespace Diag::Prof;
        std::vector<std::string> a;
        {
            size_t i = 0;
            while (i < argsIn.size())
            {
                while (i < argsIn.size() && std::isspace(static_cast<unsigned char>(argsIn[i]))) ++i;
                size_t j = i;
                while (j < argsIn.size() && !std::isspace(static_cast<unsigned char>(argsIn[j]))) ++j;
                if (j > i) a.push_back(argsIn.substr(i, j - i));
                i = j;
            }
        }
        const std::string sub = a.empty() ? "status" : a[0];
        const char* noCollector =
            "\"error\":\"no_collector\",\"hint\":\"windows are written by the collector, which exists only "
            "when the engine was started with -profile=timeline (and -artifacts= for the files)\"";
        char buf[768];

        if (sub == "status")
        {
            CollectorStatus st;
            const bool running = GetCollectorStatus(st);
            std::snprintf(buf, sizeof(buf),
                "{\"ok\":true,\"verb\":\"profile\",\"sub\":\"status\",\"collector\":%s,\"level\":\"%s\","
                "\"currentFrame\":%llu,\"processed\":%llu,\"timelineFrames\":%llu,\"latestFrame\":%s,"
                "\"windowsRequested\":%u,\"windowsFinished\":%u,\"hitches\":%u,\"eventsLost\":%llu,\"out\":%s}",
                running ? "true" : "false", LevelName(GetLevel()),
                static_cast<unsigned long long>(CurrentFrame()),
                static_cast<unsigned long long>(st.processed), static_cast<unsigned long long>(st.timelineFrames),
                st.latestFrame == ~0ull ? "null" : std::to_string(st.latestFrame).c_str(),
                st.windowsRequested, st.windowsFinished, st.hitches,
                static_cast<unsigned long long>(st.eventsLost),
                ProfileOutputDir().empty() ? "null" : JsonStr(ProfileOutputDir()).c_str());
            return buf;
        }
        if (sub == "level")
        {
            if (a.size() != 2)
                return "{\"ok\":false,\"verb\":\"profile\",\"error\":\"usage\",\"usage\":\"profile level off|stats|timeline|detail\"}";
            const std::string prev = LevelName(GetLevel());
            if      (a[1] == "off")      SetLevel(Level::Off);
            else if (a[1] == "stats")    SetLevel(Level::Stats);
            else if (a[1] == "timeline") SetLevel(Level::Timeline);
            else if (a[1] == "detail")   SetLevel(Level::Detail);
            else return "{\"ok\":false,\"verb\":\"profile\",\"error\":\"unknown_level\",\"levels\":[\"off\",\"stats\",\"timeline\",\"detail\"]}";
            std::snprintf(buf, sizeof(buf),
                "{\"ok\":true,\"verb\":\"profile\",\"sub\":\"level\",\"level\":\"%s\",\"previous\":\"%s\"}",
                LevelName(GetLevel()), prev.c_str());
            return buf;
        }
        if (sub == "snapshot")
        {
            std::string rec;
            if (!LatestRecordJson(rec))
                return "{\"ok\":false,\"verb\":\"profile\",\"error\":\"no_record\","
                       "\"hint\":\"the level is off, or no frame has closed since it was switched on\"}";
            return "{\"ok\":true,\"verb\":\"profile\",\"sub\":\"snapshot\",\"record\":" + rec + "}";
        }
        if (sub == "window")
        {
            if (a.size() != 3 || (a[1] != "begin" && a[1] != "end"))
                return "{\"ok\":false,\"verb\":\"profile\",\"error\":\"usage\",\"usage\":\"profile window begin|end <name>\"}";
            const bool begin = a[1] == "begin";
            const uint64_t now = CurrentFrame();
            const bool ok = begin ? BeginNamedWindow(a[2]) : EndNamedWindow(a[2]);
            if (!ok) return std::string("{\"ok\":false,\"verb\":\"profile\",\"sub\":\"window\",") + noCollector + "}";
            // Concatenated, not snprintf'd: the name is the caller's and has no length cap.
            return std::string("{\"ok\":true,\"verb\":\"profile\",\"sub\":\"window\",\"action\":\"") +
                   (begin ? "begin" : "end") + "\",\"name\":" + JsonStr(a[2]) + ",\"" +
                   (begin ? "firstFrame" : "lastFrame") + "\":" + std::to_string(begin ? now + 1 : now) + "}";
        }
        if (sub == "capture")
        {
            char* end = nullptr;
            const unsigned long long n = a.size() >= 2 ? std::strtoull(a[1].c_str(), &end, 10) : 0;
            if (a.size() < 2 || a.size() > 3 || !end || *end || n == 0 || n > 100000)
                return "{\"ok\":false,\"verb\":\"profile\",\"error\":\"usage\",\"usage\":\"profile capture <frames 1-100000> [name]\"}";
            const std::string name = a.size() == 3 ? a[2] : std::string("capture");
            const uint64_t now = CurrentFrame();
            if (!RequestCapture(name, static_cast<uint32_t>(n)))
                return std::string("{\"ok\":false,\"verb\":\"profile\",\"sub\":\"capture\",") + noCollector + "}";
            return "{\"ok\":true,\"verb\":\"profile\",\"sub\":\"capture\",\"name\":" + JsonStr(name) +
                   ",\"frames\":" + std::to_string(n) + ",\"firstFrame\":" + std::to_string(now + 1) + "}";
        }
        return "{\"ok\":false,\"verb\":\"profile\",\"error\":\"unknown_subcommand\","
               "\"subcommands\":[\"status\",\"level <off|stats|timeline|detail>\",\"snapshot\","
               "\"window begin <name>\",\"window end <name>\",\"capture <frames> [name]\"]}";
    }

    // The SID of whoever is on the other end of the pipe. Requires the caller
    // to already be impersonating them.
    bool ImpersonatedSid(std::vector<BYTE>& out)
    {
        HANDLE tok = nullptr;
        if (!::OpenThreadToken(::GetCurrentThread(), TOKEN_QUERY, TRUE, &tok))
            return false;
        DWORD need = 0;
        ::GetTokenInformation(tok, TokenUser, nullptr, 0, &need);
        if (need == 0) { ::CloseHandle(tok); return false; }
        out.assign(need, 0);
        const BOOL ok = ::GetTokenInformation(tok, TokenUser, out.data(), need, &need);
        ::CloseHandle(tok);
        return ok != FALSE;
    }
}

struct ReadResult { bool ok; DWORD bytes; DWORD err; };

struct ControlChannel::Impl
{
    HANDLE      pipe      = INVALID_HANDLE_VALUE;
    HANDLE      stopEvent = nullptr;   // manual-reset; every wait includes it
    HANDLE      doneEvent = nullptr;   // auto-reset; main thread -> pipe thread
    std::thread worker;
    std::atomic<bool> running{ false };

    // The one-command slot. There is no queue, by design: a queue would let a
    // client bank up work for the main thread to perform at a frame boundary.
    std::mutex  slotMutex;
    std::string request;               // filled by the pipe thread
    std::string response;              // filled by the main thread
    bool        hasRequest = false;
    bool        abandoned  = false;    // the pipe thread stopped waiting
    // Which request is in the slot. The main thread remembers the seq it is
    // serving: a reply for a seq that is no longer the slot's (it timed out and
    // a NEW request took the slot) is dropped instead of being handed to the
    // new request's caller, and a `frames` wait whose request is gone resets.
    std::uint64_t requestSeq = 0;
    // The seq the main thread last TOOK out of the slot to execute. A request
    // whose deadline passes after that has run, or is running: its effects
    // happen whatever its caller does next, so the timeout reply says
    // "started" and names the job its late reply will complete (lateJob, for
    // the request lateSeq) - the caller waits for that instead of sending the
    // request again. One that was never taken never runs: "started": false.
    std::uint64_t takenSeq     = 0;
    std::uint64_t deliveredSeq = 0;    // the last seq whose reply reached the slot in time
    std::uint64_t lateJob      = 0;
    std::uint64_t lateSeq      = 0;
    DWORD         serviceTimeoutMs = kServiceTimeoutMs;

    std::vector<BYTE> ownSid;
    std::wstring      pipeName;
    std::string       discoveryPath;

    // Who opened the channel and what this engine is, for `status`: fixed at
    // Start, so the pipe thread reads them without a lock.
    std::string startedBy = "flag";
    std::string host;                  // "Editor" | "Game"
    bool        interactive = false;   // neither -unattended nor -hidden: a person's session

    // What is driving it and where, for `status` and the editor's status bar.
    // Written by both threads, so behind infoMutex.
    mutable std::mutex infoMutex;
    unsigned long clientPid = 0;       // 0 = no client connected
    std::string   clientImage;
    std::string   lastVerb;
    double        lastMs = 0.0;        // Sys::Boot::NowMs() of the last command
    std::string   scene;               // PublishContext
    std::string   mode;
    std::atomic<std::uint64_t> served{ 0 };

    // Published by the main thread every drain, read by the pipe thread with no
    // lock. Only trivially-copyable scalars live here for that reason.
    struct Snapshot
    {
        std::atomic<std::uint64_t> frame{ 0 };
        std::atomic<std::uint32_t> engineErrors{ 0 };
        std::atomic<bool>          draining{ false };
        // Sys::Boot::NowMs() at the last drain: how long ago the main thread
        // last reached the frame boundary (0 = never, e.g. still booting).
        std::atomic<double>        frameStampMs{ 0.0 };
    } snap;
};

// One overlapped read, cancellable by an event. Every read of the pipe goes
// through it: a NULL OVERLAPPED on this FILE_FLAG_OVERLAPPED handle is undefined
// behaviour (the kernel writes the byte count through a pointer whose lifetime
// the caller no longer controls).
static ReadResult ReadOverlapped(HANDLE pipe, HANDLE cancel,
                                 void* buffer, DWORD capacity)
{
    OVERLAPPED ov = {};
    ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return { false, 0, ::GetLastError() };

    DWORD bytes = 0;
    BOOL  ok    = ::ReadFile(pipe, buffer, capacity, &bytes, &ov);
    DWORD err   = ::GetLastError();

    if (!ok && err == ERROR_IO_PENDING)
    {
        HANDLE waits[2] = { ov.hEvent, cancel };
        if (::WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0)
        {
            ::CancelIoEx(pipe, &ov);
            ::CloseHandle(ov.hEvent);
            return { false, 0, ERROR_OPERATION_ABORTED };
        }
        ok  = ::GetOverlappedResult(pipe, &ov, &bytes, FALSE);
        err = ok ? ERROR_SUCCESS : ::GetLastError();
    }
    ::CloseHandle(ov.hEvent);
    return { ok != FALSE, bytes, ok ? ERROR_SUCCESS : err };
}

ControlChannel& ControlChannel::Get()
{
    static ControlChannel s_channel;
    return s_channel;
}

bool ControlChannel::Enabled() const
{
    return m_impl && m_impl->running.load(std::memory_order_relaxed);
}

bool ControlChannel::Start(const char* startedBy)
{
    if (m_impl) return true;               // idempotent

    auto impl = new Impl();
    impl->startedBy   = (startedBy && *startedBy) ? startedBy : "flag";
    impl->host        = Sys::IsEditorHost() ? "Editor" : "Game";
    impl->interactive = !Sys::CommandLine::Get().Has("-unattended") &&
                        !Sys::CommandLine::Get().Has("-hidden");

    // ---- 1. The security descriptor. EVERY call checked; ANY failure is fatal
    // to Start(). A pipe created with a default descriptor, or worse a NULL DACL,
    // would be reachable by exactly the principals the DACL exists to exclude - and
    // it would look like it had worked.
    if (!CurrentUserSid(impl->ownSid))
    {
        LOG_ERROR("ControlChannel: cannot read this process's user SID (%s); "
                  "refusing to open the pipe.",
                  LastErrorText(::GetLastError()).c_str());
        delete impl;
        return false;
    }

    LPWSTR sidStr = nullptr;
    if (!::ConvertSidToStringSidW(
            reinterpret_cast<TOKEN_USER*>(impl->ownSid.data())->User.Sid, &sidStr))
    {
        LOG_ERROR("ControlChannel: ConvertSidToStringSid failed (%s); refusing.",
                  LastErrorText(::GetLastError()).c_str());
        delete impl;
        return false;
    }

    // D: DACL, P: PROTECTED so no inherited ACE can widen it, one allow-ACE
    // granting GENERIC_ALL to this user and to nobody else. No Administrators
    // entry, no SYSTEM entry, no Everyone, and never a NULL DACL.
    const std::wstring sddl = std::wstring(L"D:P(A;;GA;;;") + sidStr + L")";
    ::LocalFree(sidStr);

    PSECURITY_DESCRIPTOR psd = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &psd, nullptr))
    {
        LOG_ERROR("ControlChannel: could not build the pipe DACL (%s); refusing.",
                  LastErrorText(::GetLastError()).c_str());
        delete impl;
        return false;
    }

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength              = sizeof(sa);
    sa.lpSecurityDescriptor = psd;
    sa.bInheritHandle       = FALSE;

    // ---- 2. The pipe. PID in the name so two engines never collide.
    wchar_t name[128];
    ::swprintf_s(name, L"\\\\.\\pipe\\dx12engine-ctl.%lu",
                 static_cast<unsigned long>(::GetCurrentProcessId()));
    impl->pipeName = name;

    impl->pipe = ::CreateNamedPipeW(
        name,
        // FIRST_PIPE_INSTANCE: without it, a process that got to this name first owns
        // it and we are silently added as a SECOND INSTANCE of somebody else's pipe.
        // With it, a squatter makes this fail - and we refuse rather than fall back to
        // another name, because a fallback would defeat the check.
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        // MESSAGE mode: one ReadFile is exactly one command. Byte mode would need a
        // framing layer whose failure mode is a half-written command parsed as a valid
        // one. REJECT_REMOTE_CLIENTS makes "named pipe" actually mean local - without
        // it the pipe is reachable over SMB.
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1,                       // one client at a time, by design
        kOutBuffer, kInBuffer,
        0, &sa);

    const DWORD createErr = ::GetLastError();
    ::LocalFree(psd);

    if (impl->pipe == INVALID_HANDLE_VALUE)
    {
        LOG_ERROR("ControlChannel: CreateNamedPipe failed (%s). If this is "
                  "ERROR_ACCESS_DENIED another process already owns the name — "
                  "refusing rather than using a fallback.",
                  LastErrorText(createErr).c_str());
        delete impl;
        return false;
    }

    impl->stopEvent = ::CreateEventW(nullptr, TRUE,  FALSE, nullptr);   // manual
    impl->doneEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);   // auto
    if (!impl->stopEvent || !impl->doneEvent)
    {
        LOG_ERROR("ControlChannel: CreateEvent failed (%s); refusing.",
                  LastErrorText(::GetLastError()).c_str());
        if (impl->stopEvent) ::CloseHandle(impl->stopEvent);
        if (impl->doneEvent) ::CloseHandle(impl->doneEvent);
        ::CloseHandle(impl->pipe);
        delete impl;
        return false;
    }

    impl->serviceTimeoutMs = static_cast<DWORD>(std::clamp(
        Sys::CommandLine::Get().GetInt("-control_timeout_ms", static_cast<int>(kServiceTimeoutMs)),
        250, static_cast<int>(kServiceTimeoutMs)));

    // A channel opened again after Stop (the editor's toggle): `job wait`
    // blocks again instead of returning at once on the old shutdown flag.
    Jobs::Resume();

    m_impl = impl;
    impl->running.store(true, std::memory_order_release);
    impl->worker = std::thread([this] { ThreadMain(); });

    // ---- 3. Discovery. The client should not have to guess a PID.
    if (Sys::HasArtifactDir())
    {
        impl->discoveryPath = Sys::ArtifactDir() + "/control.json";
        std::string j = "{\n  \"pipe\": \"";
        // The name is ASCII by construction; widen-narrow is not worth it.
        for (wchar_t c : impl->pipeName)
        {
            if (c == L'\\') j += "\\\\";
            else            j += static_cast<char>(c);
        }
        j += "\",\n  \"pid\": " +
             std::to_string(static_cast<unsigned long>(::GetCurrentProcessId())) +
             // The same list the unknown_verb reply advertises (Dispatch).
             ",\n  \"verbs\": [\"status\", \"profile\", \"log\", \"job\", \"capture\", \"pick\", \"query\", "
             "\"run\", \"frames\", \"flow\", \"rg\", \"setting\", \"editor\", \"quit\"]\n}\n";
        Sys::WriteFileBytes(impl->discoveryPath, j.data(), j.size());
    }

    LOG_INFO("ControlChannel: listening on %ls (single client, current user only; opened by %s)",
             impl->pipeName.c_str(), impl->startedBy.c_str());
    LOG_WARNING("ControlChannel: 'query' and 'run' evaluate Lua against the FULL binding "
                "surface — not read-only. Built into Debug, Release and Profile; only "
                "Retail strips it (Engine.Cpp.props).");
    return true;
}

void ControlChannel::RequestStop()
{
    if (!m_impl || m_stopRequested) return;
    m_stopRequested = true;
    LOG_INFO("ControlChannel: closing at the next frame boundary");
}

ControlChannelState ControlChannel::Snapshot() const
{
    ControlChannelState s;
    const Impl* impl = m_impl;
    if (!impl) return s;
    s.enabled     = impl->running.load(std::memory_order_relaxed);
    s.stopPending = m_stopRequested;
    s.startedBy   = impl->startedBy;
    s.pipe        = Sys::WideToUtf8(impl->pipeName);
    s.served      = impl->served.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(impl->infoMutex);
    s.connected   = impl->clientPid != 0;
    s.clientPid   = impl->clientPid;
    s.clientImage = impl->clientImage;
    s.lastVerb    = impl->lastVerb;
    s.lastAgeMs   = impl->lastMs > 0.0 ? Sys::Boot::NowMs() - impl->lastMs : -1.0;
    return s;
}

void ControlChannel::PublishContext(const std::string& scene, const char* mode)
{
    Impl* impl = m_impl;
    if (!impl) return;
    std::lock_guard<std::mutex> lk(impl->infoMutex);
    if (impl->scene != scene) impl->scene = scene;
    if (impl->mode != (mode ? mode : "")) impl->mode = mode ? mode : "";
}

// Lua `Control`: the same open / close a person reaches from the editor's
// Tools > AI Control Channel, for the Output Log's command line, scripts and
// the harness. Every call is main-thread.
void ControlChannel::RegisterLua(lua_State* L)
{
    if (!L) return;
    sol::state_view lua(L);
    sol::table control = lua["Control"].get_or_create<sol::table>();

    // Control.start() -> true when the channel is open afterwards.
    LuaDoc::Fn(control, "Control", "start", []() { return ControlChannel::Get().Start("lua"); },
        { .doc = "Open the control channel in this running engine - what -control does at launch "
                 "and the editor's Tools > AI Control Channel do. Already open: true, unchanged.",
          .returnsDoc = "true when the channel is open afterwards",
          .effect = LuaDoc::Effect::Process });

    // Control.stop() -> whether it was open. Closes at the next frame
    // boundary, so a chunk the channel itself runs still gets its reply.
    LuaDoc::Fn(control, "Control", "stop", []()
    {
        ControlChannel& ch = ControlChannel::Get();
        const bool wasOpen = ch.Enabled();
        ch.RequestStop();
        return wasOpen;
    },
        { .doc = "Close the control channel at the next frame boundary; a chunk the channel itself "
                 "runs still gets its reply. Control.start() opens it again.",
          .returnsDoc = "whether it was open",
          .effect = LuaDoc::Effect::Process });

    // Control.state() -> {enabled, stopPending, startedBy, pipe, connected,
    // clientPid, clientImage, served, lastVerb, lastAgeMs}.
    LuaDoc::Fn(control, "Control", "state", [](sol::this_state s) -> sol::table
    {
        sol::state_view v(s);
        const ControlChannelState st = ControlChannel::Get().Snapshot();
        sol::table t = v.create_table();
        t["enabled"]     = st.enabled;
        t["stopPending"] = st.stopPending;
        t["startedBy"]   = st.startedBy;
        t["pipe"]        = st.pipe;
        t["connected"]   = st.connected;
        t["clientPid"]   = st.clientPid;
        t["clientImage"] = st.clientImage;
        t["served"]      = st.served;
        t["lastVerb"]    = st.lastVerb;
        t["lastAgeMs"]   = st.lastAgeMs;
        return t;
    },
        { .doc = "The control channel's state: whether it is open, who opened it, the connected "
                 "client and how many commands it answered.",
          .returns = "{enabled: boolean, stopPending: boolean, startedBy: string, pipe: string, "
                     "connected: boolean, clientPid: integer, clientImage: string, served: integer, "
                     "lastVerb: string, lastAgeMs: number}",
          .returnsDoc = "lastAgeMs < 0: no command yet" });
}

void ControlChannel::Stop()
{
    if (!m_impl) return;
    Impl* impl = m_impl;

    if (impl->running.exchange(false, std::memory_order_acq_rel))
    {
        // A blocked ReadFile does not observe a flag, which is the whole reason
        // every wait in the thread is a WaitForMultipleObjects that includes
        // this event, and the reason the pipe is OVERLAPPED.
        ::SetEvent(impl->stopEvent);
        ::CancelIoEx(impl->pipe, nullptr);
        ::SetEvent(impl->doneEvent);        // release a thread waiting on us
        ControlQuery::AbandonAsync();       // waiting async runs fail "shutdown"
        Jobs::Shutdown();                   // ... and one inside `job wait`

        if (impl->worker.joinable()) impl->worker.join();
    }

    if (impl->pipe != INVALID_HANDLE_VALUE) ::CloseHandle(impl->pipe);
    if (impl->stopEvent) ::CloseHandle(impl->stopEvent);
    if (impl->doneEvent) ::CloseHandle(impl->doneEvent);

    if (!impl->discoveryPath.empty())
        ::DeleteFileW(Sys::Utf8ToWide(impl->discoveryPath).c_str());

    m_impl = nullptr;
    delete impl;

    // A later Start() (the editor's toggle) begins clean: no `frames` wait, no
    // audited request, no sequence number from the old slot.
    AbandonServing();
    m_stopRequested = false;
    LOG_INFO("ControlChannel: stopped");
}

// The pipe thread. It owns the handle and the overlapped structures and holds
// NO pointer to World, ScriptSystem or Renderer - the only engine data it can
// see is the atomic snapshot the main thread publishes.
void ControlChannel::ThreadMain()
{
    Impl* impl = m_impl;
    std::vector<char> buf(kInBuffer + 1);

    // Who is driving the engine, for `status`, the editor's status bar and one
    // log line per connection. Taken once the client's identity is verified.
    // The line is a "ControlChannel:" lifecycle line, NOT "[CONTROL]": that tag
    // is the request audit ("[CONTROL] #id"), which `log tag=[CONTROL]` readers
    // rely on - and a client that connects per command would otherwise slip
    // two lines between any two of its own reads (tests/unit/test_control_log.py).
    auto noteClient = [impl](bool connected)
    {
        ULONG pid = 0;
        std::string image;
        if (connected && ::GetNamedPipeClientProcessId(impl->pipe, &pid))
            image = ProcessImageName(pid);
        {
            std::lock_guard<std::mutex> lk(impl->infoMutex);
            if (!connected)
            {
                pid   = impl->clientPid;
                image = impl->clientImage;
            }
            impl->clientPid   = connected ? pid : 0;
            impl->clientImage = connected ? image : std::string();
        }
        LOG_INFO("ControlChannel: client %s: %s (pid %lu)", connected ? "connected" : "disconnected",
                 image.empty() ? "?" : image.c_str(), static_cast<unsigned long>(pid));
    };

    while (impl->running.load(std::memory_order_acquire))
    {
        OVERLAPPED ov = {};
        ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) break;

        BOOL connected = ::ConnectNamedPipe(impl->pipe, &ov);
        DWORD err = ::GetLastError();
        if (!connected && err == ERROR_IO_PENDING)
        {
            HANDLE waits[2] = { ov.hEvent, impl->stopEvent };
            const DWORD w = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (w != WAIT_OBJECT_0) { ::CancelIoEx(impl->pipe, &ov); ::CloseHandle(ov.hEvent); break; }
            DWORD moved = 0;
            ::GetOverlappedResult(impl->pipe, &ov, &moved, FALSE);
        }
        else if (!connected && err != ERROR_PIPE_CONNECTED)
        {
            ::CloseHandle(ov.hEvent);
            if (!impl->running.load(std::memory_order_acquire)) break;
            continue;
        }
        ::CloseHandle(ov.hEvent);

        // ---- serve this client until it goes away ----
        // `verified` is per CONNECTION, not per command: the identity cannot change
        // mid-connection, and re-impersonating on every message would cost a token
        // round-trip per query for no added guarantee.
        bool verified = false;
        while (impl->running.load(std::memory_order_acquire))
        {
            const ReadResult r =
                ReadOverlapped(impl->pipe, impl->stopEvent, buf.data(), kInBuffer);

            if (!r.ok && r.err == ERROR_MORE_DATA)
            {
                // Oversize message. Drain and DISCARD the remainder rather than accumulating
                // it: accumulating would let a client grow this buffer without bound, and
                // simply disconnecting would leave the tail of the message to be read as if it
                // were the next command.
                char sink[4096];
                for (;;)
                {
                    const ReadResult d = ReadOverlapped(impl->pipe, impl->stopEvent,
                                                        sink, sizeof(sink));
                    if (d.ok) break;                          // last chunk
                    if (d.err != ERROR_MORE_DATA) break;      // gone or cancelled
                }
                WriteResponse("{\"ok\":false,\"error\":\"too_large\","
                              "\"limit\":8192}");
                continue;
            }

            if (!r.ok || r.bytes == 0) break;      // client gone

            // ---- identity cross-check, ONCE per connection ----
            // AFTER the first read, never before: ImpersonateNamedPipeClient cannot produce
            // a usable token until the client has sent something. Called straight after
            // ConnectNamedPipe it yields an anonymous token, the SID comparison fails and
            // EVERY legitimate client is rejected (the client sees only an opaque
            // ERROR_PIPE_NOT_CONNECTED on write).
            //
            // The DACL already enforces this. This second, independent check catches a
            // mistake in the SDDL string by something other than the SDDL string, and gives
            // a rejected caller a log line rather than a silent kernel refusal. It fails
            // CLOSED.
            if (!verified)
            {
                ImpersonationScope imp(impl->pipe);
                std::vector<BYTE> theirs;
                const bool ok = imp.active && ImpersonatedSid(theirs) &&
                    ::EqualSid(reinterpret_cast<TOKEN_USER*>(theirs.data())->User.Sid,
                               reinterpret_cast<TOKEN_USER*>(impl->ownSid.data())->User.Sid);
                if (!ok)
                {
                    LOG_WARNING("ControlChannel: rejected a client whose identity "
                                "did not match this process's user (impersonate=%s).",
                                imp.active ? "ok" : "FAILED");
                    break;
                }
                verified = true;
                noteClient(true);
            }

            std::string cmd(buf.data(), r.bytes);
            WriteResponse(ServeCommand(cmd));
        }

        if (verified) noteClient(false);
        ::DisconnectNamedPipe(impl->pipe);
    }
}

std::string ControlChannel::ServeCommand(const std::string& cmd)
{
    Impl* impl = m_impl;

    // For `status` and the editor's status bar: how much, and what last.
    impl->served.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(impl->infoMutex);
        impl->lastVerb = VerbOf(cmd);
        impl->lastMs   = Sys::Boot::NowMs();
    }

    // `status` is answered entirely on this thread from the atomic snapshot, so an
    // agent can always tell a busy engine from a dead one - even during a 20-second
    // scene load when no drain happens.
    //
    // A stopped frame counter cannot tell a long load from a hang, so the reply also
    // carries:
    //   nowMs            Sys::Boot::NowMs() - the clock every other ms here uses
    //   mainFrameAgeMs   since the main thread last reached the frame boundary
    //                    (null before the first frame)
    //   stage            what the main thread DECLARED it is doing (Sys::Stage):
    //                    {name, detail, forMs, progress{done,total,what},
    //                    lastProgressAgeMs, depth}, or null - nothing long
    //   jobs             {active, lastId} (Harness/ControlChannelJobs.h)
    if (cmd.rfind("status", 0) == 0)
    {
        const double now = Sys::Boot::NowMs();
        char head[320];
        const double stamp = impl->snap.frameStampMs.load();
        std::snprintf(head, sizeof(head),
            "{\"ok\":true,\"verb\":\"status\",\"pid\":%lu,\"frame\":%llu,"
            "\"engineErrors\":%u,\"draining\":%s,\"nowMs\":%.1f,\"mainFrameAgeMs\":",
            static_cast<unsigned long>(::GetCurrentProcessId()),
            static_cast<unsigned long long>(impl->snap.frame.load()),
            impl->snap.engineErrors.load(),
            impl->snap.draining.load() ? "true" : "false", now);
        std::string out = head;
        if (stamp > 0.0)
        {
            char b[32];
            std::snprintf(b, sizeof(b), "%.1f", now - stamp);
            out += b;
        }
        else out += "null";

        out += ",\"stage\":";
        const Sys::Stage::Snapshot st = Sys::Stage::Current();
        if (!st.active) out += "null";
        else
        {
            char b[160];
            std::snprintf(b, sizeof(b), ",\"forMs\":%.1f,\"depth\":%u", now - st.sinceMs, st.depth);
            out += "{\"name\":" + JsonStr(st.name) + ",\"detail\":" + JsonStr(st.detail) + b;
            if (st.lastProgressMs > 0.0)
            {
                std::snprintf(b, sizeof(b), ",\"progress\":{\"done\":%llu,\"total\":%llu,\"what\":",
                              static_cast<unsigned long long>(st.done),
                              static_cast<unsigned long long>(st.total));
                out += b + JsonStr(st.what);
                std::snprintf(b, sizeof(b), "},\"lastProgressAgeMs\":%.1f", now - st.lastProgressMs);
                out += b;
            }
            out += "}";
        }
        const Jobs::Summary js = Jobs::GetSummary();
        char tail[96];
        std::snprintf(tail, sizeof(tail), ",\"jobs\":{\"active\":%u,\"lastId\":%llu}",
                      js.active, static_cast<unsigned long long>(js.lastId));
        out += tail;

        // Which engine this is, so a client facing several pipes can find the one a
        // person is looking at:
        //   host         "Editor" | "Game"
        //   interactive  neither -unattended nor -hidden
        //   startedBy    "flag" (-control) | "menu" (editor) | "lua" (Control.start())
        //   scene, mode  what the editor published (null in Game.exe)
        //   served       commands answered since the channel opened
        out += ",\"host\":" + JsonStr(impl->host);
        out += std::string(",\"interactive\":") + (impl->interactive ? "true" : "false");
        out += ",\"startedBy\":" + JsonStr(impl->startedBy);
        {
            std::lock_guard<std::mutex> lk(impl->infoMutex);
            out += ",\"scene\":" + (impl->scene.empty() ? std::string("null") : JsonStr(impl->scene));
            out += ",\"mode\":"  + (impl->mode.empty()  ? std::string("null") : JsonStr(impl->mode));
        }
        out += ",\"served\":" + std::to_string(impl->served.load(std::memory_order_relaxed));
        return out + "}";
    }

    // `job get|list|wait`: the jobs requests started (ControlChannelJobs.h).
    // Pipe thread, like status: a save's result is readable while the main
    // thread is busy, and `job wait` blocks THIS connection, never a frame.
    if (cmd.rfind("job", 0) == 0 &&
        (cmd.size() == 3 || std::isspace(static_cast<unsigned char>(cmd[3]))))
        return Jobs::ServeVerb(cmd.substr(3));

    // `capture {"path":..., "afterFrame":N}`: a picture of the main view, as a job
    // (CaptureRequests.h). Pipe thread: it only files the request;
    // HarnessCaptureSystem takes the picture at PostRender of that frame.
    if (cmd.rfind("capture", 0) == 0 &&
        (cmd.size() == 7 || std::isspace(static_cast<unsigned char>(cmd[7]))))
        return CaptureRequests::ServeVerb(cmd.substr(7), impl->snap.frame.load());

    // `pick {"x": .., "y": ..}`: which entity the main view draws at a point, as a
    // job (CaptureRequests.h). Pipe thread: it only files the request;
    // HarnessCaptureSystem asks the picking pass and resolves it.
    if (cmd.rfind("pick", 0) == 0 &&
        (cmd.size() == 4 || std::isspace(static_cast<unsigned char>(cmd[4]))))
        return CaptureRequests::ServePickVerb(cmd.substr(4));

    if (cmd.rfind("profile", 0) == 0 &&
        (cmd.size() == 7 || std::isspace(static_cast<unsigned char>(cmd[7]))))
        return ServeProfileVerb(cmd.substr(7));

    // `log` reads the logger's record ring: pipe thread, no frame needed.
    if (cmd.rfind("log", 0) == 0 &&
        (cmd.size() == 3 || std::isspace(static_cast<unsigned char>(cmd[3]))))
        return ServeLogVerb(cmd.substr(3));

    // Everything else needs the main thread.
    {
        std::lock_guard<std::mutex> lk(impl->slotMutex);
        if (impl->hasRequest)
            return "{\"ok\":false,\"error\":\"busy\"}";
        impl->request    = cmd;
        impl->response.clear();
        impl->hasRequest = true;
        impl->abandoned  = false;
        ++impl->requestSeq;
    }

    HANDLE waits[2] = { impl->doneEvent, impl->stopEvent };
    const DWORD w = ::WaitForMultipleObjects(2, waits, FALSE, impl->serviceTimeoutMs);
    if (w == WAIT_OBJECT_0)
    {
        std::lock_guard<std::mutex> lk(impl->slotMutex);
        return impl->response;
    }

    // Timed out or shutting down. Mark the request abandoned so a late drain
    // does not write into a slot nobody is reading, and tell the client
    // something true rather than hanging it - including whether it RAN
    // (Impl::takenSeq): a request the main thread already took runs to its
    // end whatever the client does next, so sending it again would do it
    // twice; its late reply completes a job instead (Drain).
    std::uint64_t lateJob = 0;
    {
        std::lock_guard<std::mutex> lk(impl->slotMutex);
        // The reply reached the slot between the wait giving up and this
        // lock: it is the answer, and the event it set must not satisfy the
        // NEXT request's wait with this one's (emptied) response.
        if (impl->deliveredSeq == impl->requestSeq)
        {
            ::ResetEvent(impl->doneEvent);
            return impl->response;
        }
        impl->abandoned  = true;
        impl->hasRequest = false;
        if (w != WAIT_OBJECT_0 + 1 && impl->takenSeq == impl->requestSeq)
        {
            lateJob = Jobs::Create("late_reply", VerbOf(cmd), 0);
            Jobs::Start(lateJob);
            impl->lateJob = lateJob;
            impl->lateSeq = impl->requestSeq;
        }
    }
    if (w == WAIT_OBJECT_0 + 1)
        return "{\"ok\":false,\"error\":\"shutting_down\"}";
    if (lateJob != 0)
        return "{\"ok\":false,\"error\":\"engine_did_not_service_in_time\",\"started\":true,\"jobId\":" +
               std::to_string(lateJob) +
               ",\"hint\":\"it started and runs to its end: its reply will be job " + std::to_string(lateJob) +
               " (job wait " + std::to_string(lateJob) + ") - sending it again would do it twice\"}";
    return "{\"ok\":false,\"error\":\"engine_did_not_service_in_time\",\"started\":false,"
           "\"hint\":\"it never started - nothing happened, sending it again is safe. The frame loop "
           "is blocked: a scene load, a hang, or a breakpoint (status says which)\"}";
}

void ControlChannel::WriteResponse(const std::string& body)
{
    Impl* impl = m_impl;
    OVERLAPPED ov = {};
    ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return;

    DWORD written = 0;
    BOOL ok = ::WriteFile(impl->pipe, body.data(),
                          static_cast<DWORD>(body.size()), &written, &ov);
    if (!ok && ::GetLastError() == ERROR_IO_PENDING)
    {
        HANDLE waits[2] = { ov.hEvent, impl->stopEvent };
        // A deadline, not INFINITE: a client that connected and then stopped
        // reading must not pin this thread forever.
        if (::WaitForMultipleObjects(2, waits, FALSE, 5000) != WAIT_OBJECT_0)
            ::CancelIoEx(impl->pipe, &ov);
        else
            ::GetOverlappedResult(impl->pipe, &ov, &written, FALSE);
    }
    ::CloseHandle(ov.hEvent);
}

std::optional<int> ControlChannel::Drain(World& world, ScriptSystem& script,
                                         std::uint64_t frame, const std::function<void()>& beforeExecute)
{
    if (!m_impl) return std::nullopt;

    // RequestStop() (Control.stop(), the editor's toggle): here, at the frame
    // boundary and outside any request's execution, so the request that asked
    // for it had its reply sent during the frame in between.
    if (m_stopRequested)
    {
        Stop();
        return std::nullopt;
    }

    Impl* impl = m_impl;

    impl->snap.frame.store(frame, std::memory_order_relaxed);
    impl->snap.engineErrors.store(Logger::Get().ErrorTotal(), std::memory_order_relaxed);
    impl->snap.frameStampMs.store(Sys::Boot::NowMs(), std::memory_order_relaxed);
    Jobs::NoteFrame(frame);

    // Async runs resume here, every frame, whether or not a request is waiting:
    // between their Wait.Frames they hold no request slot. Same serial point and
    // the same render-lane park as a request.
    ControlQuery::TickAsync(script, frame, beforeExecute);

    std::string cmd;
    std::uint64_t seq = 0;
    {
        // try_to_lock, never a blocking lock: the frame loop must not wait on
        // the pipe thread under any circumstances. A missed frame just means
        // the command is serviced on the next one.
        std::unique_lock<std::mutex> lk(impl->slotMutex, std::try_to_lock);
        if (!lk.owns_lock()) return std::nullopt;
        // The request a `frames N` was serving timed out (slot empty), or was
        // replaced by a new one: forget its wait and its audit id, or the next
        // request inherits the old target and the old #id.
        if (m_servingSeq != 0 && (!impl->hasRequest || impl->requestSeq != m_servingSeq))
        {
            // A `frames` wait has no effects to report: its late job ends here.
            if (impl->lateJob != 0 && impl->lateSeq == m_servingSeq)
            {
                Jobs::Fail(impl->lateJob, "abandoned", "the frames wait ended when its caller stopped waiting", {});
                impl->lateJob = 0;
                impl->lateSeq = 0;
            }
            AbandonServing();
        }
        if (!impl->hasRequest) return std::nullopt;
        cmd = impl->request;
        seq = impl->requestSeq;
        impl->takenSeq = seq;    // from here on, a timeout means "it ran"
    }

    // No frame barrier when the channel is idle. Release slotMutex before
    // pumping device continuations; accepted commands may mutate any live state.
    if (beforeExecute) beforeExecute();
    impl->snap.draining.store(true, std::memory_order_relaxed);
    std::optional<std::string> reply =
        ExecuteOnMainThread(world, script, cmd, frame);
    impl->snap.draining.store(false, std::memory_order_relaxed);

    // nullopt means `frames N` is still counting down. The slot stays occupied
    // (so a second command gets "busy" rather than interleaving) and the client
    // stays blocked on doneEvent, which is exactly the semantics it asked for.
    if (!reply)
    {
        m_servingSeq = seq;
        return std::nullopt;
    }
    m_servingSeq = 0;

    {
        std::lock_guard<std::mutex> lk(impl->slotMutex);
        // Only the request that is STILL in the slot gets this reply. After a
        // timeout the pipe thread emptied the slot - and may already have put
        // a new request in it, which must not receive the old answer or lose
        // its own turn (the slot stays occupied for it).
        if (impl->hasRequest && impl->requestSeq == seq && !impl->abandoned)
        {
            impl->response     = *reply;
            impl->hasRequest   = false;
            impl->deliveredSeq = seq;
            ::SetEvent(impl->doneEvent);
        }
        else if (impl->lateJob != 0 && impl->lateSeq == seq)
        {
            // Its caller was told "started" and the job id: the reply is that
            // job's outcome, so nothing it did is lost or done twice.
            const std::uint64_t job = impl->lateJob;
            impl->lateJob = 0;
            impl->lateSeq = 0;
            CompleteLateJob(job, *reply);
            Logger::ScopedContext audit(LogSource::Control, m_lastRequestId);
            LOG_WARNING("[CONTROL] #%u -> late: its caller stopped waiting "
                        "(engine_did_not_service_in_time); the reply is job %llu",
                        m_lastRequestId, static_cast<unsigned long long>(job));
        }
        else
        {
            Logger::ScopedContext audit(LogSource::Control, m_lastRequestId);
            LOG_WARNING("[CONTROL] #%u -> dropped: its caller stopped waiting "
                        "(engine_did_not_service_in_time) before it finished", m_lastRequestId);
        }
    }

    std::optional<int> quit;
    if (m_pendingQuit)
    {
        quit = *m_pendingQuit;
        m_pendingQuit.reset();
    }
    return quit;
}

// The request this thread was serving across drains (a `frames N`) is gone.
void ControlChannel::AbandonServing()
{
    if (m_activeRequestId != 0)
    {
        Logger::ScopedContext audit(LogSource::Control, m_activeRequestId);
        LOG_WARNING("[CONTROL] #%u -> abandoned (the caller stopped waiting) ms=%.1f",
                    m_activeRequestId, SteadyMs() - m_activeRequestStartMs);
    }
    m_waitTargetFrame = 0;
    m_rgWaiting       = false;
    m_activeRequestId = 0;
    m_servingSeq      = 0;
}

// Audits and dispatches one request. MAIN THREAD, at the frame boundary.
std::optional<std::string> ControlChannel::ExecuteOnMainThread(
    World& world, ScriptSystem& script,
    const std::string& command, std::uint64_t frame)
{
    // Split "<verb> <payload>". The payload keeps its internal whitespace: a Lua
    // expression is mostly whitespace-insensitive but a string literal is not, so
    // trimming more than the one separating space would corrupt it.
    const size_t sp = command.find_first_of(" \t\r\n");
    const std::string verb = command.substr(0, sp);
    std::string payload = (sp == std::string::npos) ? std::string()
                                                    : command.substr(sp + 1);
    while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
        payload.pop_back();

    // AUDIT: one line when the request starts and one when it answers, both
    // source=control, in the same log / Output Log as everything else, so a `query`
    // that rewrites the world is visible to the person at the editor. A `frames N`
    // spans several drains and is audited once, as one request.
    if (m_activeRequestId == 0)
    {
        m_activeRequestId      = Logger::NextRequestId();
        m_activeRequestStartMs = SteadyMs();
        Logger::ScopedContext audit(LogSource::Control, m_activeRequestId);
        LOG_INFO("[CONTROL] #%u %s%s%s", m_activeRequestId, verb.c_str(),
                 payload.empty() ? "" : " ", OneLine(payload, 200).c_str());
    }
    const std::uint32_t id = m_activeRequestId;

    // What this request logs on this thread while it runs comes back in its
    // reply, so an error the expression caused arrives with the answer instead
    // of in a file the caller has to go and grep.
    std::vector<CapturedLogLine> captured;
    std::size_t dropped = 0;
    std::optional<std::string> reply;
    {
        Logger::ScopedContext ctx(LogSource::Control, id, &captured, 64);
        reply = Dispatch(world, script, verb, payload, frame);
        dropped = ctx.Dropped();
    }
    if (!reply) return std::nullopt;   // `frames N` still counting: the same request next drain

    m_lastRequestId   = id;
    m_activeRequestId = 0;
    std::string out = AttachRequestLog(std::move(*reply), id, captured, dropped);
    const size_t okAt = out.find("\"ok\":");
    const bool ok = okAt != std::string::npos && out.compare(okAt + 5, 4, "true") == 0;
    {
        Logger::ScopedContext audit(LogSource::Control, id);
        LOG_INFO("[CONTROL] #%u -> %s bytes=%zu ms=%.1f lines=%zu", id, ok ? "ok" : "error",
                 out.size(), SteadyMs() - m_activeRequestStartMs, captured.size() + dropped);
    }
    return out;
}

std::optional<std::string> ControlChannel::Dispatch(
    World& world, ScriptSystem& script, const std::string& verb,
    const std::string& payload, std::uint64_t frame)
{
    if (verb == "editor")
    {
        if (!m_editorHandler) return std::string("{\"ok\":false,\"error\":\"editor_unavailable\"}");
        return m_editorHandler(payload);
    }

    // ---- frames N: block the caller until N more frames have been presented.
    // An agent that mutates state and immediately queries reads the value BEFORE
    // the systems that consume it have run; `frames N` lets it wait for them.
    // Sleeping in the client cannot replace it - wall-clock time is not frames, and
    // a stalled engine makes them arbitrarily different.
    if (verb == "frames")
    {
        if (m_waitTargetFrame == 0)
        {
            long n = std::strtol(payload.c_str(), nullptr, 10);
            if (n < 1) n = 1;
            // Capped well under the pipe thread's 30 s service deadline: a
            // request to wait longer than the client will wait would always
            // resolve as a timeout, which reads as an engine fault rather than
            // as the caller asking for the impossible.
            if (n > 600) n = 600;
            m_waitTargetFrame = frame + static_cast<std::uint64_t>(n);
        }
        if (frame < m_waitTargetFrame) return std::nullopt;   // keep waiting

        m_waitTargetFrame = 0;
        char out[128];
        std::snprintf(out, sizeof(out),
            "{\"ok\":true,\"verb\":\"frames\",\"frame\":%llu}",
            static_cast<unsigned long long>(frame));
        return std::string(out);
    }

    // ---- quit [code]: an orderly shutdown, not TerminateProcess. App gets the
    // code back and runs its normal exit path, so the PSO library is saved and
    // run.json is finalised - the two things a hard kill silently discards.
    if (verb == "quit")
    {
        const int code = payload.empty()
            ? 0 : static_cast<int>(std::strtol(payload.c_str(), nullptr, 10));
        m_pendingQuit = code;
        char out[96];
        std::snprintf(out, sizeof(out),
            "{\"ok\":true,\"verb\":\"quit\",\"exitCode\":%d}", code);
        return std::string(out);
    }

    // ---- flow [state|history|switch <name>]: the whole game-flow state machine
    // as one JSON object, identical in shape to what -dumpflow writes.
    //
    // BEFORE `query` on purpose. Most of this is reachable as Lua -
    // GameFlow.Snapshot(), .History(), .Diagnostics() - but only as three round
    // trips returning three shapes, and the SET MEMBERSHIP (which C++ and Lua
    // systems a gate is holding back) is not on the Lua surface at all. That
    // membership answers the question this verb exists for: the state is right, so
    // why did nothing run.
    //
    // The body lives in GameFlowDump so the reply and the file come out of ONE
    // builder and cannot drift (see GameFlowDump.h). The history is capped there to
    // the last 16 records to stay inside the 48 KB reply budget of this 64 KB pipe.
    if (verb == "flow")
    {
        GameFlowProbe& probe = GameFlowProbe::Get();
        if (!probe.Bound())
            return std::string(
                "{\"ok\":false,\"verb\":\"flow\",\"error\":\"flow_unbound\","
                "\"hint\":\"App never bound the GameFlow probe, so there is "
                "nothing to report - see Harness/GameFlowDump.h\"}");

        return ServeFlowVerb(*probe.flow, *probe.systems, *probe.scripts, payload);
    }

    // ---- rg [summary | pass <name> | resource <name> | edges | frame |
    // journal [<resource>] | full]: one frame as it EXECUTED - the main graph
    // (which passes ran, Phase A's barriers, the declared edges) and the device
    // (every command list, the submission, the barrier journal, the observed
    // edges) - RenderGraph/FrameCapture.h. Built only when asked and only on the
    // render half, so this is a `frames`-style wait: the first drain asks, later
    // drains answer from the first capture published after the ask (no_snapshot
    // after 120 frames). The views come out of the builder -rgdump writes with
    // (Harness/RenderGraphDump.h).
    if (verb == "rg")
    {
        RG::FrameCaptureBoard& board = RG::FrameCaptures();
        if (!m_rgWaiting)
        {
            m_rgWaitSeq   = board.Request();
            m_rgWaitFrame = frame;
            m_rgWaiting   = true;
        }
        if (board.PublishedSequence() <= m_rgWaitSeq)
        {
            if (frame < m_rgWaitFrame + 120) return std::nullopt;   // keep waiting
            m_rgWaiting = false;
            return std::string(
                "{\"ok\":false,\"verb\":\"rg\",\"error\":\"no_snapshot\","
                "\"message\":\"no frame capture was published within 120 frames of the "
                "request - no frame was rendered (or its EndFrame failed)\"}");
        }
        m_rgWaiting = false;
        const std::shared_ptr<const RG::FrameCapture> cap = board.Latest();
        if (!cap)
            return std::string("{\"ok\":false,\"verb\":\"rg\",\"error\":\"no_snapshot\"}");
        const std::string root = Sys::HasArtifactDir() ? Sys::ArtifactDir() : Sys::LogDir();
        return ServeRgVerb(*cap, payload, root + "/control");
    }

    // ---- setting list [<prefix>] | get | where | set <path> <value> | clear:
    // the player settings through the settings store - Resource::ServeSettingVerb,
    // the same writer as Lua's Settings.setRuntime / clearRuntime. `set` is the
    // Runtime layer: applied at once (that setting alone), never saved.
    if (verb == "setting")
        return Resource::ServeSettingVerb(payload);

    if (verb == "query")
    {
        if (payload.empty())
            return std::string("{\"ok\":false,\"error\":\"empty_query\"}");

        // MANDATORY. ScriptSystem::Update sets m_world = nullptr on the way out, so at
        // a frame boundary the script system has no world. Without this borrow `status`
        // works but virtually every query returns nil while the log fills with "called
        // outside Update" - which looks like a broken binding.
        ScriptSystem::WorldBorrow borrow(script);
        (void)world;
        return ControlQuery::Evaluate(script, payload);
    }

    // ---- run <chunk> | run {"chunk"|"file","args","budgetMs","name"}: a Lua
    // CHUNK - statements, locals, loops - executed inside THIS frame. The main
    // thread serves one request per frame, so reading 200 values with `query`
    // costs 200+ frames and returns 200 different frames; one `run` is one frame
    // and one snapshot. Same boundary as query - see ControlQuery::Run. A result
    // too large for the pipe is written under the run's artifacts (else its log
    // folder), in control/, and the reply names the file.
    if (verb == "run")
    {
        if (payload.empty())
            return std::string("{\"ok\":false,\"verb\":\"run\",\"error\":\"empty_run\"}");
        ScriptSystem::WorldBorrow borrow(script);   // the same borrow query needs
        (void)world;
        const std::string root = Sys::HasArtifactDir() ? Sys::ArtifactDir() : Sys::LogDir();
        return ControlQuery::Run(script, payload, m_activeRequestId, frame, root + "/control");
    }

    // Unknown verb. The reply carries the honesty statement rather than only
    // the header: a caller discovering the protocol interactively sees the
    // limitation at the moment it matters.
    return std::string(
        "{\"ok\":false,\"error\":\"unknown_verb\","
        "\"verbs\":[\"status\",\"profile\",\"log\",\"job\",\"capture\",\"pick\",\"query\",\"run\",\"frames\",\"flow\",\"rg\",\"setting\",\"editor\",\"quit\"],"
        "\"note\":\"'query' evaluates a Lua EXPRESSION and 'run' a Lua CHUNK against the "
        "full binding surface. Neither is read-only or sandboxed: a function call is an "
        "expression. The boundary is the pipe DACL plus WITH_CONTROL, which is "
        "compiled out of Retail; opt-in via -control.\"}");
}

}  // namespace Harness

#endif  // WITH_HARNESS && WITH_CONTROL
