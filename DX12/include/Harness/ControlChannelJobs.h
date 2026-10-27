#pragma once
// Harness/ControlChannelJobs.h - work the control pipe started that finishes
// LATER, as jobs.
//
// A scene save runs at the next frame boundary, not inside the request that
// asked for it, so the request answers a jobId, and the pipe's `job` verb -
// served on the PIPE thread, so it answers while the main thread is busy -
// returns the job:
//
//   job get <id>          {"ok":true,"verb":"job","job":{...}}
//   job list              {"ok":true,"verb":"job","session":...,"jobs":[...]}
//                         newest created first: all active jobs + the last 64
//                         completed receipts
//   job wait <id> [ms]    the job once it is terminal, else after ms (default
//                         and cap 10000) with "timedOut":true - never an error
// A trailing session=<SessionIdentity()> on get / wait (after the ms on wait)
// refuses a reference from another process (session_lost).
//
// A job: {id, session, kind, detail, state: queued|running|succeeded|failed,
// requestId (the [CONTROL] #id that created it), createdMs / startedMs / endedMs
// (Sys::Boot::NowMs), createdFrame / endedFrame, progress {done,total},
// output {...}, error {code, message, log[]}}. QUEUED IS NOT SUCCESS: only
// "succeeded" means the work was done; a failed job says why, with the lines
// the work logged.
//
// The engine also files work nobody requested here when a caller has to wait
// for it: every hot reload is a job of kind "hotreload.<shader|lua|material>"
// with requestId 0 (System/HotReloadReceipt.h) - a script edits a file, then
// waits for the first such job newer than the lastId it saw in `status`.
//
// A request whose service deadline passed AFTER the main thread took it runs
// to its end; its late reply completes a job of kind "late_reply" that the
// timeout reply names ("started": true, "jobId"), so a caller that timed out
// waits for that instead of sending the request again (ControlChannel.h).
//
// In-process only - no journal, no idempotency key: a client that lost a reply
// finds its job with `job list`. The main thread creates / starts / finishes
// jobs; any thread reads them. Completed receipts expire in completion order
// (result_expired); an id this process never issued is unknown_job. Active jobs
// are never evicted. Requested work uses TryCreate, which refuses at 256 active
// jobs before anything is scheduled; internal late replies and hot reloads use
// Create, so started work always keeps a receipt (an admission limit, not a
// hard cap on the total).

#include <cstdint>
#include <string>
#include <vector>

namespace Harness::Jobs
{
#if defined(WITH_HARNESS) && defined(WITH_CONTROL)

    // The frame jobs are stamped with (ControlChannel::Drain, every frame).
    void NoteFrame(std::uint64_t frame);

    // A new job in state "queued"; returns its id (never 0). @p requestId is
    // the [CONTROL] #id that asked for it (Logger::CurrentRequestId()).
    std::uint64_t Create(const char* kind, std::string detail, std::uint32_t requestId);
    // For requested work: returns 0 (refuse before queueing anything) when
    // kActiveAdmission jobs are active. Create never refuses: internal late
    // replies must always get a receipt.
    std::uint64_t TryCreate(const char* kind, std::string detail, std::uint32_t requestId);
    inline constexpr std::uint32_t kActiveAdmission = 256;
    // Id 0 is accepted and ignored by the rest, so a caller that did not get a
    // job (a human's Ctrl+S) needs no branch.
    void Start(std::uint64_t id);
    void Progress(std::uint64_t id, std::uint64_t done, std::uint64_t total);
    // @p outputJson: a JSON object's text ("{}" when there is nothing to say).
    void Succeed(std::uint64_t id, std::string outputJson);
    // @p outputJson (optional): what the work DID get done, shown as the failed
    // job's "output" next to its "error".
    void Fail(std::uint64_t id, std::string code, std::string message, std::vector<std::string> log,
              std::string outputJson = {});

    // The `job` verb (pipe thread). @p args is what follows "job".
    std::string ServeVerb(const std::string& args);

    struct Summary
    {
        std::uint32_t active = 0;    // queued + running
        std::uint64_t lastId = 0;
    };
    Summary GetSummary();
    // The process's job session id (pid + process start time): stable for this OS
    // process's lifetime, independent of -seed.
    const std::string& SessionIdentity();

    // Wakes any `job wait` so the pipe thread can stop (ControlChannel::Stop).
    void Shutdown();
    // Undoes Shutdown for a channel opened again in the same process (the
    // editor's Tools > AI Control Channel toggle): `job wait` blocks again.
    // Jobs recorded before the stop stay readable.
    void Resume();

#else   // inert stub: Retail has no control channel

    inline void NoteFrame(std::uint64_t) {}
    inline std::uint64_t Create(const char*, std::string, std::uint32_t) { return 0; }
    inline std::uint64_t TryCreate(const char*, std::string, std::uint32_t) { return 0; }
    inline void Start(std::uint64_t) {}
    inline void Progress(std::uint64_t, std::uint64_t, std::uint64_t) {}
    inline void Succeed(std::uint64_t, std::string) {}
    inline void Fail(std::uint64_t, std::string, std::string, std::vector<std::string>, std::string = {}) {}
    inline void Shutdown() {}
    inline void Resume() {}

#endif
}
