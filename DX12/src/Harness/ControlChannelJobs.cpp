#include "Harness/ControlChannelJobs.h"

#if defined(WITH_HARNESS) && defined(WITH_CONTROL)

#include "System/BootTimeline.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <sstream>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <stdexcept>

namespace Harness::Jobs
{
namespace
{
    using Json = nlohmann::json;

    enum class State { Queued, Running, Succeeded, Failed };

    struct Job
    {
        std::uint64_t            id = 0;
        std::string              kind, detail;
        State                    state = State::Queued;
        std::uint32_t            requestId = 0;
        double                   createdMs = 0, startedMs = 0, endedMs = 0;
        std::uint64_t            createdFrame = 0, endedFrame = 0;
        std::uint64_t            done = 0, total = 0;
        std::string              outputJson;
        std::string              errorCode, errorMessage;
        std::vector<std::string> errorLog;
    };

    constexpr size_t kKeepCompleted = 64;
    constexpr long   kWaitCapMs = 10000;   // under the client's patience, like `run`'s budget

    struct Registry
    {
        std::mutex              mutex;
        std::condition_variable changed;
        std::deque<Job>         jobs;       // oldest first
        std::uint64_t           nextId = 1;
        std::deque<std::uint64_t> completed; // completion order, not creation order
        bool                    stopping = false;
        std::atomic<std::uint64_t> frame{ 0 };
    };

    const char* StateName(State s)
    {
        switch (s)
        {
        case State::Queued:    return "queued";
        case State::Running:   return "running";
        case State::Succeeded: return "succeeded";
        default:               return "failed";
        }
    }

    bool Terminal(State s) { return s == State::Succeeded || s == State::Failed; }

    Registry& R()
    {
        static Registry s_registry;
        return s_registry;
    }

    Job* Find(Registry& r, std::uint64_t id)
    {
        for (Job& j : r.jobs)
            if (j.id == id) return &j;
        return nullptr;
    }

    // Called with the mutex held, after a job reached a terminal state. Active
    // jobs are never evicted; only completed history is bounded (kKeepCompleted,
    // the oldest completion goes first).
    void RetainCompleted(Registry& r, std::uint64_t id)
    {
        r.completed.push_back(id);
        while (r.completed.size() > kKeepCompleted)
        {
            const auto expired = r.completed.front();
            r.completed.pop_front();
            const auto it = std::find_if(r.jobs.begin(), r.jobs.end(),
                                        [expired](const Job& j) { return j.id == expired; });
            if (it != r.jobs.end()) r.jobs.erase(it);
        }
    }

    Json ToJson(const Job& j)
    {
        Json o;
        o["id"]           = j.id;
        o["session"]      = SessionIdentity();
        o["kind"]         = j.kind;
        o["detail"]       = j.detail;
        o["state"]        = StateName(j.state);
        o["requestId"]    = j.requestId;
        o["createdMs"]    = j.createdMs;
        o["createdFrame"] = j.createdFrame;
        if (j.startedMs > 0) o["startedMs"] = j.startedMs;
        if (Terminal(j.state))
        {
            o["endedMs"]    = j.endedMs;
            o["endedFrame"] = j.endedFrame;
            o["ms"]         = j.endedMs - (j.startedMs > 0 ? j.startedMs : j.createdMs);
        }
        if (j.total > 0) o["progress"] = Json{ { "done", j.done }, { "total", j.total } };
        // A failed job may carry output too: a partial result says what DID
        // happen (a hot reload that reached some holders and not others).
        if (j.state == State::Succeeded || (j.state == State::Failed && !j.outputJson.empty()))
        {
            Json out = Json::parse(j.outputJson.empty() ? "{}" : j.outputJson, nullptr, false);
            o["output"] = out.is_discarded() ? Json{ { "raw", j.outputJson } } : out;
        }
        if (j.state == State::Failed)
            o["error"] = Json{ { "code", j.errorCode }, { "message", j.errorMessage }, { "log", j.errorLog } };
        return o;
    }

    std::string Dump(const Json& j)
    {
        return j.dump(-1, ' ', false, Json::error_handler_t::replace);
    }

    std::string Error(const char* code, const std::string& message)
    {
        return Dump(Json{ { "ok", false }, { "verb", "job" }, { "error", code }, { "message", message } });
    }
}

void NoteFrame(std::uint64_t frame)
{
    R().frame.store(frame, std::memory_order_relaxed);
}

const std::string& SessionIdentity()
{
    static const std::string s_id = [] {
        FILETIME start{}, exit{}, kernel{}, user{};
        if (!GetProcessTimes(GetCurrentProcess(), &start, &exit, &kernel, &user))
            throw std::runtime_error("job session identity unavailable");
        return std::to_string(GetCurrentProcessId()) + "-" +
            std::to_string((static_cast<std::uint64_t>(start.dwHighDateTime) << 32) | start.dwLowDateTime);
    }();
    return s_id;
}

static std::uint64_t CreateImpl(const char* kind, std::string detail, std::uint32_t requestId, bool bounded)
{
    Registry& r = R();
    std::lock_guard<std::mutex> lk(r.mutex);
    if (bounded && std::count_if(r.jobs.begin(), r.jobs.end(), [](const Job& j) { return !Terminal(j.state); }) >= kActiveAdmission)
        return 0;
    Job j;
    j.id           = r.nextId++;
    j.kind         = kind ? kind : "?";
    j.detail       = std::move(detail);
    j.requestId    = requestId;
    j.createdMs    = Sys::Boot::NowMs();
    j.createdFrame = r.frame.load(std::memory_order_relaxed);
    r.jobs.push_back(std::move(j));
    r.changed.notify_all();
    return r.jobs.back().id;
}

std::uint64_t Create(const char* kind, std::string detail, std::uint32_t requestId)
{ return CreateImpl(kind, std::move(detail), requestId, false); }
std::uint64_t TryCreate(const char* kind, std::string detail, std::uint32_t requestId)
{ return CreateImpl(kind, std::move(detail), requestId, true); }

void Start(std::uint64_t id)
{
    Registry& r = R();
    std::lock_guard<std::mutex> lk(r.mutex);
    if (Job* j = Find(r, id); j && j->state == State::Queued)
    {
        j->state     = State::Running;
        j->startedMs = Sys::Boot::NowMs();
        r.changed.notify_all();
    }
}

void Progress(std::uint64_t id, std::uint64_t done, std::uint64_t total)
{
    Registry& r = R();
    std::lock_guard<std::mutex> lk(r.mutex);
    if (Job* j = Find(r, id))
    {
        j->done  = done;
        j->total = total;
    }
}

void Succeed(std::uint64_t id, std::string outputJson)
{
    Registry& r = R();
    std::lock_guard<std::mutex> lk(r.mutex);
    if (Job* j = Find(r, id); j && !Terminal(j->state))
    {
        j->state      = State::Succeeded;
        j->endedMs    = Sys::Boot::NowMs();
        j->endedFrame = r.frame.load(std::memory_order_relaxed);
        j->outputJson = std::move(outputJson);
        RetainCompleted(r, id);
        r.changed.notify_all();
    }
}

void Fail(std::uint64_t id, std::string code, std::string message, std::vector<std::string> log,
          std::string outputJson)
{
    Registry& r = R();
    std::lock_guard<std::mutex> lk(r.mutex);
    if (Job* j = Find(r, id); j && !Terminal(j->state))
    {
        j->state        = State::Failed;
        j->endedMs      = Sys::Boot::NowMs();
        j->endedFrame   = r.frame.load(std::memory_order_relaxed);
        j->errorCode    = std::move(code);
        j->errorMessage = std::move(message);
        j->errorLog     = std::move(log);
        j->outputJson   = std::move(outputJson);
        RetainCompleted(r, id);
        r.changed.notify_all();
    }
}

Summary GetSummary()
{
    Registry& r = R();
    std::lock_guard<std::mutex> lk(r.mutex);
    Summary s;
    for (const Job& j : r.jobs)
        if (!Terminal(j.state)) ++s.active;
    s.lastId = r.nextId - 1;
    return s;
}

void Shutdown()
{
    Registry& r = R();
    std::lock_guard<std::mutex> lk(r.mutex);
    r.stopping = true;
    r.changed.notify_all();
}

void Resume()
{
    Registry& r = R();
    std::lock_guard<std::mutex> lk(r.mutex);
    r.stopping = false;
}

std::string ServeVerb(const std::string& args)
{
    std::istringstream in(args);
    std::string sub, idText, msText, sessionText;
    in >> sub >> idText >> msText >> sessionText;
    // get <id> [session=...] | wait <id> <ms> [session=...]
    if (sub == "get" && msText.starts_with("session=")) sessionText = msText;
    if (!sessionText.empty() && sessionText != "session=" + SessionIdentity())
        return Error("session_lost", "job reference belongs to another process; do not resend the operation");
    Registry& r = R();

    if (sub == "list")
    {
        std::lock_guard<std::mutex> lk(r.mutex);
        Json list = Json::array();
        for (auto it = r.jobs.rbegin(); it != r.jobs.rend(); ++it) list.push_back(ToJson(*it));
        return Dump(Json{ { "ok", true }, { "verb", "job" }, { "session", SessionIdentity() }, { "jobs", list } });
    }
    if (sub != "get" && sub != "wait")
        return Error("bad_request", "job get <id> | job list | job wait <id> [ms]");

    char* end = nullptr;
    const unsigned long long id = std::strtoull(idText.c_str(), &end, 10);
    if (idText.empty() || !end || *end != '\0' || id == 0)
        return Error("bad_request", "job " + sub + " needs a job id");

    std::unique_lock<std::mutex> lk(r.mutex);
    if (!Find(r, id))
        return Error(id < r.nextId ? "result_expired" : "unknown_job",
                     "no retained job " + idText + " (this process keeps all active jobs and 64 completed receipts)");

    bool timedOut = false;
    if (sub == "wait")
    {
        long ms = msText.empty() ? kWaitCapMs : std::strtol(msText.c_str(), nullptr, 10);
        ms = std::clamp(ms, 0L, kWaitCapMs);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        const bool done = r.changed.wait_until(lk, deadline, [&] {
            const Job* j = Find(r, id);
            return r.stopping || !j || Terminal(j->state);
        });
        timedOut = !done;
    }
    const Job* j = Find(r, id);
    if (!j) return Error("result_expired", "job " + idText + " completed and its receipt expired while waiting");
    Json out{ { "ok", true }, { "verb", "job" }, { "job", ToJson(*j) } };
    if (sub == "wait") out["timedOut"] = timedOut;
    return Dump(out);
}
} // namespace Harness::Jobs

#endif // WITH_HARNESS && WITH_CONTROL
