#pragma once
// Harness/ControlQuery.h - evaluate one Lua expression (or run one chunk) for
// the control channel.
//
// Read ControlChannel.h first. THIS IS NOT A SANDBOX. The expression-only
// wrapper is a typo guard that removes assignment and statement sequencing; a
// function call is an expression, so anything callable from Lua is callable
// from here. The bounds below are LIVENESS protection - they stop a mistyped
// query from wedging the frame loop - and nothing more. Never describe them as
// security controls.

#include <cstdint>
#include <functional>
#include <string>

class ScriptSystem;

namespace Harness::ControlQuery
{
    struct Limits
    {
        int  timeoutMs   = 250;   // wall clock, enforced by a count hook
        int  maxDepth    = 6;     // table nesting
        int  maxEntries  = 256;   // keys per table
        int  maxStringLen= 4096;  // per string value
        int  maxTotalLen = 48 * 1024;  // whole JSON body; the pipe out-buffer is 64 KB
    };

    /// Evaluate `expr` and return a complete JSON reply object. Never throws; a
    /// Lua error becomes {"ok":false,...}.
    std::string Evaluate(ScriptSystem& script, const std::string& expr,
                         const Limits& limits = {});

    /// The `run` verb: execute a Lua CHUNK - statements, locals, loops - inside the
    /// current frame, so a batch read is one snapshot instead of one query per value
    /// (the main thread serves one request per frame). @p request is the raw chunk,
    /// or a JSON object {"chunk"|"file", "args", "budgetMs", "name"}: `args` reaches
    /// the chunk as `...` (JSON -> Lua), `budgetMs` (default 2000, capped at 10000)
    /// bounds it with the same count hook as a query, `name` labels it in error
    /// messages. Every return value comes back in "values"; a reply too large for
    /// the pipe is written to `<resultDir>/run_<requestId>.json` and the reply names
    /// the file. Errors are structured - bad_request, file, syntax, runtime (with a
    /// traceback), timeout - and a timeout says the chunk's side effects are NOT
    /// rolled back. Same boundary as a query: not read-only, not a sandbox. Never
    /// throws.
    std::string Run(ScriptSystem& script, const std::string& request,
                    std::uint32_t requestId, std::uint64_t frame,
                    const std::string& resultDir);

    /// `run {"async": true, "maxFrames": N, ...}` runs the chunk as a COROUTINE.
    /// `Wait.Frames(n)` suspends it for n frames (returning the frame index it
    /// resumes on) and `Wait.Until(fn [, max])` polls fn once a frame; between
    /// resumes the request slot is FREE - the reply is a jobId at once, and the
    /// chunk's return values arrive as the job's output (`job wait`), like a save's.
    /// Each resume has budgetMs; the whole run has maxFrames (default 3600) and fails
    /// "timeout" past it. A sync `run` that calls Wait.Frames gets a clear error.
    ///
    /// TickAsync: main thread, at the channel's serial point, EVERY frame - resume
    /// the async runs whose wait is over. @p beforeResume parks the render lane; it
    /// is called once, and only when a resume is due.
    void TickAsync(ScriptSystem& script, std::uint64_t frame, const std::function<void()>& beforeResume);
    /// The channel stops: every pending async run's job fails "shutdown". Lua is
    /// not touched (the state may already be gone).
    void AbandonAsync();
}
