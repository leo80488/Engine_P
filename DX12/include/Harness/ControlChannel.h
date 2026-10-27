#pragma once
// Harness/ControlChannel.h - ask a RUNNING engine a question over a named pipe,
// instead of paying a full boot (~20 s to the first frame) per question.
//
// NOT A READ-ONLY CHANNEL. `query` evaluates a Lua EXPRESSION against the whole
// binding surface; a function call is an expression, so the expression-only
// wrapper removes assignments and statement sequencing and nothing else. Every
// one of these is a legal query:
//     Scene.Load("BaseScene")          -- mutates the world
//     Settings.set("display.vsync", 0) -- mutates settings
//     Engine.Quit(3)                   -- terminates the process
//     io.popen("whoami")               -- spawns a process
// Do not add an in-VM sandbox: it cannot hold. A denylisted _ENV is bypassed by
// `_G.io.popen(...)` (lbaselib registers _G); denying _G too fails, because any
// reachable Lua closure carries its own _ENV bound to the real globals; and
// io.popen is live (luaconf.h defines LUA_USE_WINDOWS, ScriptSystem opens
// sol::lib::io). A sandbox that looks enforced and is not is worse than none, so
// the limitation is stated here and in the bad-verb reply instead.
//
// The security boundary is two things, and only two:
//   1. The pipe DACL: one ACE for the current user's SID, plus
//      PIPE_REJECT_REMOTE_CLIENTS. Another account cannot reach it (a local
//      Administrator or SYSTEM still can - not fixable on Windows, not claimed).
//      The channel creates no privilege boundary: only the principal already
//      running the process can send a query.
//   2. WITH_CONTROL: compiled out of Retail (Engine.Cpp.props), and an explicit
//      opt-in elsewhere - the -control flag, the editor's Tools > AI Control
//      Channel / command palette, or Lua Control.start(). Never implicit; while
//      it is open the editor's status bar says so and names the client.
// The `run` verb executes a Lua chunk (statements, locals, loops) inside one
// frame, with a time budget and structured errors (ControlQuery::Run). It adds no
// capability `query` lacked and sits behind the same boundary; what it adds is
// one frame for a batch instead of one frame per value.
//
// Frame safety: the pipe thread never touches engine state - it owns the handle,
// the overlapped structures and a request slot, nothing else. Commands are
// serviced at ONE point per frame, after the PostRender phase, where the
// scheduler has flushed its deferred CommandBuffer and no pass is recording. At
// most one command per frame, and the drain never blocks the frame waiting for a
// client.
//
// Pipe-thread verbs (answer while the main thread is busy or blocked): `status`
// (frame, engineErrors, age of the last frame boundary, the main thread's
// declared Sys::Stage - "loading_scene: <path>" for 20 s is a load, not a hang -
// and a job summary), `profile`, `log` and `job` (Harness/ControlChannelJobs.h:
// work a request started that finishes later, e.g. an editor save, with its
// state, output or reason). `capture` and `pick` only file a job on the pipe
// thread (Harness/CaptureRequests.h).
//
// One slot, one caller: a request that outlives the 30 s service deadline
// (-control_timeout_ms shortens it) is ABANDONED - its caller gets
// engine_did_not_service_in_time, never the next caller's reply (slot sequence
// numbers). The reply says whether the request STARTED: one the main thread
// never took never runs ("started": false - sending it again is safe); one it
// took runs to its end, and its late reply completes the job the timeout named
// ("started": true, "jobId" - wait for that job; sending the request again would
// do it twice).

#include <functional>
#include <cstdint>
#include <optional>
#include <string>

class World;
class ScriptSystem;
struct lua_State;

namespace Harness
{
    /// What the editor's status bar and Lua Control.state() show. A copy, taken
    /// on the main thread; all-default when the channel is closed or compiled out.
    struct ControlChannelState
    {
        bool          enabled     = false;
        bool          stopPending = false;   // RequestStop() taken, closes at the next frame boundary
        std::string   startedBy;             // "flag" | "menu" | "lua"
        std::string   pipe;                  // \\.\pipe\dx12engine-ctl.<pid>
        bool          connected   = false;
        unsigned long clientPid   = 0;
        std::string   clientImage;           // the client's executable, e.g. "python.exe"
        std::uint64_t served      = 0;       // commands answered since Start
        std::string   lastVerb;
        double        lastAgeMs   = -1.0;    // < 0: none yet
    };

#if defined(WITH_HARNESS) && defined(WITH_CONTROL)

    class ControlChannel
    {
    public:
        static constexpr bool kCompiled = true;

        using EditorHandler = std::function<std::string(const std::string&)>;

        static ControlChannel& Get();

        /// Create the pipe and start the listener thread. Idempotent. Returns false
        /// and logs if the channel could not be opened: every Win32 failure on the
        /// security-descriptor path is fatal to Start(), because a pipe with a default
        /// descriptor would be reachable by principals the DACL exists to exclude.
        /// @p startedBy is what `status` reports: "flag" (-control at launch), "menu"
        /// (the editor's Tools menu / command palette) or "lua" (Control.start()).
        /// Main thread.
        bool Start(const char* startedBy = "flag");

        /// Signal the thread, cancel its outstanding I/O and join. A later Start()
        /// opens it again (the editor's toggle). Main thread, never from inside Drain's
        /// request execution - see RequestStop.
        void Stop();

        /// Close the channel at the next frame boundary (the top of Drain), not now.
        /// Control.stop() runs INSIDE a request that Drain is executing, and stopping
        /// there would join the pipe thread that is about to send that request's reply.
        /// The editor's toggle takes the same path.
        void RequestStop();

        bool Enabled() const;

        /// A copy of the state for the editor's status bar / Control.state().
        ControlChannelState Snapshot() const;

        /// The scene and mode ("edit" | "play" | "paused" | "game") `status` reports,
        /// so a client can tell a person's editor from a hidden harness engine. Main
        /// thread; the editor publishes them every frame while the channel is open.
        void PublishContext(const std::string& scene, const char* mode);

        /// Lua `Control.start()` / `Control.stop()` / `Control.state()`.
        static void RegisterLua(lua_State* L);

        void SetEditorHandler(EditorHandler handler) { m_editorHandler = std::move(handler); }

        /// Service at most ONE command. Called from the single frame-boundary site in
        /// App::Run; @p beforeExecute runs before a command executes or an async run
        /// resumes (App drains its in-flight frame work there). Returns a quit code when
        /// the client asked to terminate, so App keeps ownership of the quit latch rather
        /// than this class calling PostQuitMessage behind its back.
        std::optional<int> Drain(World& world, ScriptSystem& script,
                                 std::uint64_t frame, const std::function<void()>& beforeExecute = {});

    private:
        struct Impl;

        ControlChannel() = default;

        /// The pipe thread's whole life. Touches no engine state.
        void ThreadMain();

        /// Answer one command. Runs ON THE PIPE THREAD: `status` is served here from
        /// the atomic snapshot, everything else is handed to the main thread and waited
        /// for with a deadline.
        std::string ServeCommand(const std::string& command);

        void WriteResponse(const std::string& body);

        /// Runs ON THE MAIN THREAD, from Drain, at the frame boundary. Returns nullopt
        /// for "not finished - call me again next frame", which is how `frames N` waits
        /// without blocking the frame loop. Every request it serves is AUDITED:
        /// `[CONTROL] #<id> <verb> <payload>` when it starts, `[CONTROL] #<id> -> ...`
        /// when it answers, both source=control, and the lines the request logged on
        /// this thread come back in the reply.
        std::optional<std::string> ExecuteOnMainThread(
            World& world, ScriptSystem& script,
            const std::string& command, std::uint64_t frame);

        /// The verb dispatch ExecuteOnMainThread audits and wraps.
        std::optional<std::string> Dispatch(
            World& world, ScriptSystem& script, const std::string& verb,
            const std::string& payload, std::uint64_t frame);

        void AbandonServing();

        Impl* m_impl = nullptr;
        EditorHandler m_editorHandler;

        /// Set by the `quit` verb, consumed by Drain's caller. Main-thread only.
        std::optional<int> m_pendingQuit;

        /// `frames N` target; 0 when not waiting. Main-thread only.
        std::uint64_t m_waitTargetFrame = 0;

        /// `rg`: asked the frame-capture board (RG::FrameCaptures) and waiting for a
        /// publish past m_rgWaitSeq (since frame m_rgWaitFrame; no_snapshot after 120
        /// frames). Main-thread only.
        bool          m_rgWaiting   = false;
        std::uint64_t m_rgWaitSeq   = 0;
        std::uint64_t m_rgWaitFrame = 0;

        /// The audited request in flight (0 = none): a `frames N` spans several
        /// drains and is ONE request. Main-thread only.
        std::uint32_t m_activeRequestId = 0;
        double        m_activeRequestStartMs = 0.0;
        /// The last request that finished (for the "reply dropped" audit line).
        std::uint32_t m_lastRequestId = 0;

        /// The slot sequence number of the request being served across drains (a
        /// `frames N`); 0 = none. When the slot no longer holds that request - it timed
        /// out, or a new one replaced it - AbandonServing() resets the wait and the
        /// audit id so the next request starts clean.
        std::uint64_t m_servingSeq = 0;

        /// RequestStop() was called; Drain stops at its top. Main-thread only.
        bool m_stopRequested = false;
    };

#else   // inert stub: signatures identical, every call folds away

    class ControlChannel
    {
    public:
        static constexpr bool kCompiled = false;

        static ControlChannel& Get() { static ControlChannel s_instance; return s_instance; }
        bool Start(const char* = "flag") { return false; }
        void Stop()        {}
        void RequestStop() {}
        bool Enabled() const { return false; }
        ControlChannelState Snapshot() const { return {}; }
        void PublishContext(const std::string&, const char*) {}
        static void RegisterLua(lua_State*) {}
        void SetEditorHandler(std::function<std::string(const std::string&)>) {}
        std::optional<int> Drain(World&, ScriptSystem&, std::uint64_t, const std::function<void()>& = {})
        {
            return std::nullopt;
        }
    };

#endif
}
