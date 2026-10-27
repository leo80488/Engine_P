#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <deque>
#include <fstream>
#include <mutex>
#include <queue>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

enum class LogLevel
{
    Success,
    Info,
    Warning,
    Error
};

// WHO produced a line. It travels with the entry from a thread-local context
// (Logger::ScopedContext), so LOG_* call sites need not carry it: code running
// inside a console command, a Lua binding or a control-channel request is
// attributed by the scope it runs in.
enum class LogSource : std::uint8_t
{
    Engine = 0,   // every line nobody claimed
    Lua,          // Log.* and print() from a script
    Console,      // the editor's command line (echo, result, and what it logged)
    Control,      // a -control pipe request (audit lines and what it logged)
    Count
};
const char* LogSourceName(LogSource source);

// Pending log work (producer pushes, consumer pops and writes)
struct LogEntry
{
    LogLevel level;
    std::string message;
    std::string file;
    int line;
    std::chrono::system_clock::time_point timestamp;
    // Captured on the PRODUCING thread at Log() time. The file does not carry
    // them: its line format is frozen (hundreds of scenario packs parse it).
    std::uint64_t frame     = 0;   // Logger::SetFrame value (App's frame counter)
    std::uint32_t thread    = 0;   // OS thread id
    std::uint32_t requestId = 0;   // ScopedContext request, 0 = none
    LogSource     source    = LogSource::Engine;
    bool          keepInFile = false;   // Logger::KeepInFile: written whatever the file's level
};

// One line as it went into the file, plus what the file does not carry.
// Kept in a bounded ring (Logger::kRingCapacity) that the editor's Output Log
// and the control channel's `log` verb read incrementally by seq.
struct LogRecord
{
    std::uint64_t seq       = 0;   // 1-based; strictly increasing in ring (= file) order
    std::uint64_t frame     = 0;
    std::uint32_t thread    = 0;
    std::uint32_t requestId = 0;
    LogLevel      level     = LogLevel::Info;
    LogSource     source    = LogSource::Engine;
    std::uint16_t tagLen    = 0;   // leading "[TAG]" or "Name" of the message; 0 = none
    std::uint32_t msgOffset = 0;   // where the message starts inside text
    std::string   text;            // exactly the line written to the file

    std::string_view Message() const
    {
        return msgOffset <= text.size() ? std::string_view(text).substr(msgOffset)
                                        : std::string_view();
    }
    std::string_view Tag() const { return Message().substr(0, tagLen); }
};

// A line copied synchronously, on the logging thread, into a ScopedContext's
// capture sink - so a caller can hand back exactly what its own call logged.
struct CapturedLogLine
{
    LogLevel    level;
    std::string message;
};

// Async singleton logger: producers only enqueue; the worker thread formats and writes to disk
class Logger
{
public:
    // ---- The record ring (Output Log panel, control channel `log` verb) ----
    // Every line the worker formats is pushed here as a LogRecord, with a seq
    // that increases by exactly one per line, in file order - also the lines
    // below the file's level (a shipped build's INFO). Readers poll INCREMENTALLY ("what is newer than the last seq I
    // saw"), so no reader deep-copies the buffer under m_mutex, the lock every
    // LOG_ call on every thread takes to enqueue.
    static constexpr std::size_t kRingCapacity = 8192;

    // Attribute every line logged ON THIS THREAD while alive to @p source /
    // @p requestId, and optionally copy each one's message into @p capture
    // (at most @p captureMax lines; the overflow is counted, see Dropped()).
    // Nests: the previous context is restored on destruction. Only lines
    // logged SYNCHRONOUSLY on the constructing thread are attributed - work a
    // command hands to a job or to a later frame is not.
    class ScopedContext
    {
    public:
        explicit ScopedContext(LogSource source, std::uint32_t requestId = 0,
                               std::vector<CapturedLogLine>* capture = nullptr,
                               std::size_t captureMax = 64);
        ~ScopedContext();
        ScopedContext(const ScopedContext&)            = delete;
        ScopedContext& operator=(const ScopedContext&) = delete;

        // Lines that matched this scope but exceeded captureMax.
        std::size_t Dropped() const;

    private:
        LogSource                     m_prevSource;
        std::uint32_t                 m_prevRequest;
        std::vector<CapturedLogLine>* m_prevCapture;
        std::size_t                   m_prevCaptureMax;
        std::size_t                   m_prevDropped;
    };

    // Lines logged on this thread while one is alive reach the file whatever its level: the
    // boot header (product, build, roots) a shipped build's WARNING-level file still starts with.
    class KeepInFile
    {
    public:
        KeepInFile();
        ~KeepInFile();
        KeepInFile(const KeepInFile&)            = delete;
        KeepInFile& operator=(const KeepInFile&) = delete;

    private:
        bool m_prev;
    };

    static Logger& Get();

    // No default: the caller must name the file it wants. A default would
    // quietly write a CWD-relative DX12Log.txt outside Sys::LogDir().
    void Initialize(const std::string& fileName);

    // Did Initialize() actually open a file? False means every LOG_ line this
    // process produces is formatted and dropped.
    //
    // The failure is otherwise invisible: Log() keeps enqueuing, FlushBlocking()
    // keeps counting, and run.json still reports an error tally and a `pass`
    // verdict - a green run with no evidence at all. main.cpp checks this
    // immediately after Initialize().
    bool IsInitialized() const;
    void Shutdown();

    // Synchronously drain the queue and flush the file, on the CALLING thread.
    // For a process about to die (crash handler, fatal error path, __debugbreak),
    // where the worker's "eventually" never arrives and the last - most
    // interesting - lines would be lost with the queue.
    //
    // Safe from ANY thread, including when the worker is dead or wedged: it does
    // not signal or wait on the worker, it does the worker's job itself. Entries
    // the worker already moved into its private batch cannot be recovered.
    void FlushBlocking();

    // General log (printf-style): formats and enqueues only; does not block on I/O
    void Log(LogLevel level, const char* file, int line, const char* format, ...);

    // Log() with an explicit SOURCE for this one line. The thread's
    // ScopedContext still supplies the request id and the capture sink - only
    // the source is overridden. For code that speaks for someone else: the Lua
    // bindings attribute a script's Log.* / print() lines to LogSource::Lua
    // (unless a console command or a control request is running that script,
    // which then keeps the line - see CurrentSource()).
    void LogFrom(LogSource source, LogLevel level, const char* file, int line, const char* format, ...);

    // The calling thread's ScopedContext, if any (Engine / 0 when none).
    static LogSource     CurrentSource();
    static std::uint32_t CurrentRequestId();

    // Specialized HRESULT logging (also queries the system error string)
    void LogHResult(const char* expr, HRESULT hr, const char* file, int line);

    // Copy up to @p maxCount records with seq > @p afterSeq, oldest first,
    // APPENDING to @p out. Returns the newest seq in the ring (0 = nothing yet).
    // @p firstAvailable, when given, receives the oldest seq still kept: if it
    // is greater than afterSeq + 1 the lines in between were pushed out of the
    // ring before this reader asked (the FILE still has them).
    std::uint64_t CopyRecordsSince(std::uint64_t afterSeq, std::vector<LogRecord>& out,
                                   std::size_t maxCount = static_cast<std::size_t>(-1),
                                   std::uint64_t* firstAvailable = nullptr) const;

    // Newest seq pushed into the ring. Lock-free.
    std::uint64_t LatestSeq() const { return m_ringSeq.load(std::memory_order_acquire); }

    // The newest @p maxCount records, for a crash handler: false, and nothing copied, when the
    // ring's lock is taken (the crash may have happened inside Log()).
    bool TryCopyRecent(std::size_t maxCount, std::vector<LogRecord>& out) const;

    // The lowest level the FILE gets (the ring gets every line): WARNING in a shipped build (no
    // harness), INFO otherwise; -loglevel=info|warning|error sets it.
    LogLevel FileLevel() const { return m_fileLevel; }

    // Reset the badge tallies. The ring and the FILE are untouched: the
    // panel's "Clear" empties the PANEL (its own mirror) and the tab badge,
    // and other readers of the ring (the control channel) are unaffected.
    void ResetBadgeTallies();

    // ---- Per-entry context ----
    // The frame number stamped on every entry logged from now on (App calls
    // this once per frame with the same counter Engine.Frame() returns).
    static void          SetFrame(std::uint64_t frame);
    static std::uint64_t CurrentFrame();

    // A fresh request id for ScopedContext (never 0).
    static std::uint32_t NextRequestId();

    // Running error/warning tallies since start (or last ResetBadgeTallies).
    // Counted on the worker as lines enter the ring, so every path
    // (Log, LogHResult, check failures) lands here uniformly. Cheap enough to
    // read every frame: they drive the Output Log tab's "(3!)" badge.
    uint32_t ErrorCount()   const { return m_errorCount.load(std::memory_order_relaxed); }
    uint32_t WarningCount() const { return m_warningCount.load(std::memory_order_relaxed); }

    // Never-reset totals for the WHOLE process lifetime. Harness verdicts read
    // THESE: ErrorCount() is a UI badge that ResetBadgeTallies() (the editor Log
    // panel's clear button) zeroes, so a human or a Lua scenario clearing the
    // panel would turn a failing automated run green. Both pairs are
    // incremented from the same place.
    uint32_t ErrorTotal()   const { return m_errorTotal.load(std::memory_order_relaxed); }
    uint32_t WarningTotal() const { return m_warningTotal.load(std::memory_order_relaxed); }

    // Block until every entry enqueued BEFORE this call has been formatted and
    // written, then flush the file. Returns false if @p timeoutMs elapsed with
    // entries still outstanding.
    //
    // A run verdict from ErrorTotal() is wrong if sampled while entries are in
    // flight, and FlushBlocking cannot close that gap: entries the worker already
    // moved into its private batch (a local vector in ProcessLogs) are invisible
    // to it, so an ENGINE_CHECK in a run's last frames would go uncounted.
    //
    // Do not turn this into a wait on m_cv: m_cv is the worker's wake-up and
    // Log() notifies it with notify_one, so a second waiter intermittently
    // steals the worker's wake-up and stalls the very drain it waits for (a
    // rare, timing-dependent shutdown hang). This polls a pair of counters.
    bool DrainAndQuiesce(unsigned timeoutMs = 3000);

private:
    Logger() = default;
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    const char* LevelToString(LogLevel level) const;
    // Worker loop: pop tasks, format, write to file, and update the record ring.
    void ProcessLogs();
    void WriteLine(const std::string& line);
    // The single accounting point for a formatted entry: updates the badge
    // tallies, the never-reset totals, the record ring and the processed
    // counter, in that order. BOTH drain sites (the worker and FlushBlocking)
    // route through it; a new drain site must too, or its lines go uncounted
    // and DrainAndQuiesce never sees m_processed reach its target.
    // Caller must hold m_mutex. @p line is the formatted file line, taken by
    // rvalue to keep the move; @p entry supplies the metadata.
    void PushRecord(const LogEntry& entry, std::string&& line);
    // Shared body of Log / LogFrom: format, capture, stamp, enqueue.
    void LogV(LogSource source, LogLevel level, const char* file, int line,
              const char* format, va_list args);
    // Build the "[stamp] [LEVEL] [file:line] msg" text for one entry.
    // @p cachedSec / @p cachedStamp are the caller's second-granularity
    // timestamp cache (localtime_s + strftime cost ~microseconds, and every
    // entry inside one second shares the prefix). Shared by the worker loop and
    // FlushBlocking so the two can never drift into different line formats.
    void FormatEntry(const LogEntry& entry, std::time_t& cachedSec,
                     char (&cachedStamp)[20], std::string& out) const;
    // One line to the file, under m_writeMutex: none below the file's level (unless @p entry is kept),
    // and past the run's size cap (kFileCapBytes) a closing note and nothing more - the record ring and
    // the Output Log keep everything.
    void WriteFileLine(const LogEntry& entry, const std::string& line);

    std::ofstream m_file;
    mutable std::mutex m_mutex;
    // Guards m_file WRITES only. m_mutex guards the queue / record ring and is
    // released before the slow formatting + I/O, so producers do not stall
    // behind a disk write; this lock keeps the worker and FlushBlocking (on
    // another thread) from writing m_file at once.
    // Lock order is ALWAYS m_writeMutex -> m_mutex, never the reverse.
    std::mutex m_writeMutex;
    std::queue<LogEntry> m_queue;
    std::thread m_worker;
    std::condition_variable m_cv;
    std::atomic<bool> m_running{ false };
    bool m_initialized = false;
    // The record ring (kRingCapacity); a std::deque, so pop_front is O(1).
    // Guarded by m_mutex. Its seqs are CONTIGUOUS (push increments by one,
    // only the front is popped), which makes CopyRecordsSince an index
    // computation.
    std::deque<LogRecord> m_ring;
    std::atomic<std::uint64_t> m_ringSeq{ 0 };   // seq of m_ring.back(); 0 = empty

    // Badge tallies (ErrorCount() / WarningCount()). Reset by ResetBadgeTallies.
    std::atomic<uint32_t> m_errorCount{ 0 };
    std::atomic<uint32_t> m_warningCount{ 0 };

    // Verdict totals (ErrorTotal() / WarningTotal()). NEVER reset.
    std::atomic<uint32_t> m_errorTotal{ 0 };
    std::atomic<uint32_t> m_warningTotal{ 0 };

    // Drain accounting for DrainAndQuiesce. m_enqueued rises at every queue
    // push; m_processed rises once per entry in PushRecord. The queue
    // being empty is NOT sufficient (the worker may hold a full batch in a
    // local vector), so the counters, not the container, are the truth.
    std::atomic<uint64_t> m_enqueued{ 0 };
    std::atomic<uint64_t> m_processed{ 0 };

    // ---- The file's size this run (WriteFileLine; m_writeMutex) ----
    std::uint64_t m_fileBytes  = 0;
    bool          m_fileCapped = false;
    LogLevel      m_fileLevel  = LogLevel::Info;   // FileLevel(); set by Initialize
};

// Convert a Windows wide string to a UTF-8 std::string for logging/output
std::string WStringToString(const wchar_t* wstr);

// ==== Logging macros: every level is written in every configuration ====

#define LOG_INTERNAL(level, format, ...) \
    do { \
        Logger::Get().Log((level), __FILE__, __LINE__, (format), ##__VA_ARGS__); \
    } while (0)

// ==== Per-subsystem runtime diagnostics (LOG_DIAG) ====
// Separate from LOG_TRACE, with one switch per subsystem: these fire
// periodically while the app runs (each throttled to every N frames), and a
// single global verbosity flag would bury one subsystem's lines under the
// rest (DDGI under CreateBuffer lines).
//
// LOG_DIAG still COMPILES its arguments when disabled (if constexpr keeps the
// discarded branch well-formed), so a disabled line cannot silently rot as the
// surrounding code changes, and variables that exist only for the message do
// not trigger unused-variable warnings. Nothing is evaluated at runtime.
#ifndef ENGINE_LOG_SCENE_AS
    #define ENGINE_LOG_SCENE_AS    0   // DDGISceneAS: TLAS/BLAS build summary + non-opaque count
#endif
#ifndef ENGINE_LOG_DDGI_VOLUME
    #define ENGINE_LOG_DDGI_VOLUME 0   // DDGIVolumeManager: per-volume CB state
#endif
#ifndef ENGINE_LOG_SCENE_DIFF
    #define ENGINE_LOG_SCENE_DIFF  0   // Renderer_Scene: BVH dirty-diff counters
#endif
#ifndef ENGINE_LOG_VRAM
    #define ENGINE_LOG_VRAM        0   // GraphicsDX12: periodic VRAM poll
                                       // (the OVER BUDGET warning is separate
                                       // and always fires: do not gate it)
#endif

#define LOG_DIAG(enabled, format, ...)                                            do { if constexpr ((enabled) != 0)                                                 { LOG_INTERNAL(LogLevel::Info, (format), ##__VA_ARGS__); } } while (0)

// ==== LOG_TRACE - per-resource creation chatter, OFF by default ====
// One line per buffer / texture / shader / PSO / sampler at load time: not
// per-frame, but thousands of them bury the lines that matter. Set
// ENGINE_LOG_TRACE to 1 when debugging resource creation. Compiles to
// nothing when off: no cost and no unused-argument warning.
#ifndef ENGINE_LOG_TRACE
    #define ENGINE_LOG_TRACE 0
#endif
#if ENGINE_LOG_TRACE
    #define LOG_TRACE(format, ...) LOG_INTERNAL(LogLevel::Info, (format), ##__VA_ARGS__)
#else
    #define LOG_TRACE(format, ...) do { } while (0)
#endif

#define LOG_SUCCESS(format, ...) LOG_INTERNAL(LogLevel::Success, (format), ##__VA_ARGS__)
#define LOG_INFO(format, ...)    LOG_INTERNAL(LogLevel::Info,    (format), ##__VA_ARGS__)
#define LOG_WARNING(format, ...) LOG_INTERNAL(LogLevel::Warning, (format), ##__VA_ARGS__)
#define LOG_ERROR(format, ...)   LOG_INTERNAL(LogLevel::Error,   (format), ##__VA_ARGS__)

// HRESULT helper macro (use after DX calls)
#define LOG_HRESULT(expr, hr)                                             \
    do {                                                                  \
        Logger::Get().LogHResult((expr), (hr), __FILE__, __LINE__);       \
    } while (0)

// Resource loading macros
#define LOG_RESOURCE_LOADING(name)                                        \
    LOG_INFO("Loading resource: %s", (name))

#define LOG_RESOURCE_LOADED(name)                                         \
    LOG_SUCCESS("Loaded resource: %s", (name))

#define LOG_RESOURCE_FAILED(name)                                         \
    LOG_ERROR("Failed to load resource: %s", (name))

