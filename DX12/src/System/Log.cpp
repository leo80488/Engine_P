#include "System/Log.h"
#include "System/CommandLine.h"   // -loglevel
#include "System/PathUtil.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <system_error>

namespace
{
    // The frame stamped on entries. Written once per frame by App (main
    // thread), read by every producer: relaxed is enough, a line logged on a
    // worker in the same instant as the increment may carry either value.
    std::atomic<std::uint64_t> g_frame{ 0 };
    std::atomic<std::uint32_t> g_nextRequest{ 0 };

    // Logger::ScopedContext state. thread_local, so a command running on the
    // main thread cannot claim a line a job worker logged at the same moment.
    thread_local LogSource                     t_source     = LogSource::Engine;
    thread_local std::uint32_t                 t_request    = 0;
    thread_local std::vector<CapturedLogLine>* t_capture    = nullptr;
    thread_local std::size_t                   t_captureMax = 0;
    thread_local std::size_t                   t_dropped    = 0;
    thread_local bool                          t_keepInFile = false;   // Logger::KeepInFile

    // Success is an INFO-level line that reads as good news.
    int LevelRank(LogLevel level)
    {
        switch (level)
        {
        case LogLevel::Warning: return 2;
        case LogLevel::Error:   return 3;
        default:                return 1;
        }
    }

    // The message's category, as the codebase writes it: a leading "[TAG]"
    // (the harness convention, which the scenario packs assert on) or a leading
    // "Name: " (the subsystem convention - "ShaderLibrary: ...",
    // "RunLedger: ..."). Returns the length of the tag text at the start of the
    // message; 0 = none. Parsed on the WORKER, so the producers pay nothing.
    std::uint16_t TagLength(const std::string& m)
    {
        constexpr std::size_t kMaxTag = 40;
        if (m.size() >= 3 && m[0] == '[')
        {
            const std::size_t close = m.find(']', 1);
            if (close == std::string::npos || close < 2 || close > kMaxTag) return 0;
            for (std::size_t i = 1; i < close; ++i)
            {
                const unsigned char c = static_cast<unsigned char>(m[i]);
                if (!(std::isalnum(c) || c == '_' || c == '-' || c == '.')) return 0;
            }
            return static_cast<std::uint16_t>(close + 1);
        }
        if (m.empty() || !std::isalpha(static_cast<unsigned char>(m[0]))) return 0;
        std::size_t i = 0;
        while (i < m.size() && i < kMaxTag &&
               (std::isalnum(static_cast<unsigned char>(m[i])) || m[i] == '_'))
            ++i;
        if (i + 1 < m.size() && m[i] == ':' && m[i + 1] == ' ')
            return static_cast<std::uint16_t>(i);
        return 0;
    }
}

const char* LogSourceName(LogSource source)
{
    switch (source)
    {
    case LogSource::Engine:  return "engine";
    case LogSource::Lua:     return "lua";
    case LogSource::Console: return "console";
    case LogSource::Control: return "control";
    default:                 return "unknown";
    }
}

void Logger::SetFrame(std::uint64_t frame)  { g_frame.store(frame, std::memory_order_relaxed); }
std::uint64_t Logger::CurrentFrame()        { return g_frame.load(std::memory_order_relaxed); }
std::uint32_t Logger::NextRequestId()
{
    std::uint32_t id = g_nextRequest.fetch_add(1, std::memory_order_relaxed) + 1;
    if (id == 0) id = g_nextRequest.fetch_add(1, std::memory_order_relaxed) + 1;   // 0 = none
    return id;
}

Logger::ScopedContext::ScopedContext(LogSource source, std::uint32_t requestId,
                                     std::vector<CapturedLogLine>* capture,
                                     std::size_t captureMax)
    : m_prevSource(t_source), m_prevRequest(t_request), m_prevCapture(t_capture),
      m_prevCaptureMax(t_captureMax), m_prevDropped(t_dropped)
{
    t_source     = source;
    t_request    = requestId;
    t_capture    = capture;
    t_captureMax = capture ? captureMax : 0;
    t_dropped    = 0;
}

Logger::ScopedContext::~ScopedContext()
{
    t_source     = m_prevSource;
    t_request    = m_prevRequest;
    t_capture    = m_prevCapture;
    t_captureMax = m_prevCaptureMax;
    t_dropped    = m_prevDropped;
}

std::size_t Logger::ScopedContext::Dropped() const { return t_dropped; }

Logger::KeepInFile::KeepInFile() : m_prev(t_keepInFile) { t_keepInFile = true; }
Logger::KeepInFile::~KeepInFile() { t_keepInFile = m_prev; }

Logger& Logger::Get()
{
    static Logger s_instance;
    return s_instance;
}

Logger::~Logger()
{
    Shutdown();
}

void Logger::Initialize(const std::string& fileName)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_initialized)
            return;

        // ---- Make the directory before anything touches the file ----
        // ofstream::open does NOT create missing parents, and its failure here is a
        // silent `return`, so the first run on a fresh machine or a fresh
        // -artifacts=<dir> would produce no log and say nothing.
        //
        // Guarded on a non-empty parent: the offline harnesses call Initialize()
        // with a bare filename, whose parent_path() is "", and
        // create_directories("") is an error, not a no-op.
        {
            std::error_code ec;
            const std::filesystem::path parent =
                Sys::U8Path(fileName).parent_path();
            if (!parent.empty())
                std::filesystem::create_directories(parent, ec);
            // Best-effort, like the rotate below: if this fails the open fails
            // too, and that is reported by m_initialized staying false.
        }

        // ---- Rotate before opening ----
        // Keep two generations: <file>.prev becomes <file>.prev2, <file> becomes
        // <file>.prev and the old .prev2 is dropped, so each launch starts its own log
        // and a crash two launches back is still on disk.
        //
        // Strictly AFTER the m_initialized guard: rotating first would let a second
        // Initialize() call rename the log being written, and the still-open handle
        // would keep appending to the renamed file.
        //
        // Best-effort: if the rename fails (file open in an editor, a second engine
        // instance) fall through and append. No LOG_ here: m_mutex is held and the
        // worker does not exist yet, so it would deadlock, not report.
        {
            std::error_code ec;
            const std::filesystem::path cur   = Sys::U8Path(fileName);
            const std::filesystem::path prev  = Sys::U8Path(fileName + ".prev");
            const std::filesystem::path prev2 = Sys::U8Path(fileName + ".prev2");
            if (std::filesystem::exists(cur, ec) && !ec)
            {
                std::filesystem::remove(prev2, ec);     // ignore "did not exist"
                ec.clear();
                std::filesystem::rename(prev, prev2, ec);
                ec.clear();
                std::filesystem::rename(cur, prev, ec);
                // ec != 0 -> append to the existing file; not worth failing over.
            }
        }

        // U8Path, not the narrow overload: fileName is an absolute path (e.g. under
        // %LOCALAPPDATA%), and a non-ASCII Windows account name would be mangled
        // through the ANSI code page and silently fail to open.
        // 'app' is only a safety net: after the rotate this is a fresh file.
        m_file.open(Sys::U8Path(fileName), std::ios::out | std::ios::app);
        m_initialized = m_file.is_open();
        if (!m_initialized)
            return;

        // A player's log holds what went wrong, not every INFO line; a harness build's packs read
        // INFO lines.
#ifdef WITH_HARNESS
        m_fileLevel = LogLevel::Info;
#else
        m_fileLevel = LogLevel::Warning;
#endif
        // -loglevel=info|warning|error: the lowest level the log FILE gets (the record ring, the
        // Output Log and the pipe's log verb get every line). A shipped build writes WARNING and up
        // unless told; package_smoke passes info to judge a Retail package from its log.
        const std::string level = Sys::CommandLine::Get().Get("-loglevel");
        if (level == "info")         m_fileLevel = LogLevel::Info;
        else if (level == "warning") m_fileLevel = LogLevel::Warning;
        else if (level == "error")   m_fileLevel = LogLevel::Error;

        m_running = true;
    }

    m_worker = std::thread(&Logger::ProcessLogs, this);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_enqueued.fetch_add(1, std::memory_order_release);
        LogEntry first{ LogLevel::Info, "========== Logger initialized ==========", "", 0,
                        std::chrono::system_clock::now() };
        first.keepInFile = true;
        m_queue.push(std::move(first));
    }
    m_cv.notify_one();
}

bool Logger::IsInitialized() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_initialized;
}

void Logger::Shutdown()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_initialized)
            return;

        m_enqueued.fetch_add(1, std::memory_order_release);
        m_queue.push(LogEntry{
            LogLevel::Info,
            "========== Logger shutdown ==========",
            "",
            0,
            std::chrono::system_clock::now()
        });
        m_running = false;
    }
    m_cv.notify_one();

    if (m_worker.joinable())
        m_worker.join();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_file.is_open())
            m_file.close();
        m_initialized = false;
    }
}

const char* Logger::LevelToString(LogLevel level) const
{
    switch (level)
    {
    case LogLevel::Success: return "SUCCESS";
    case LogLevel::Info:    return "INFO";
    case LogLevel::Warning: return "WARNING";
    case LogLevel::Error:   return "ERROR";
    default:                return "UNKNOWN";
    }
}

void Logger::FormatEntry(const LogEntry& entry, std::time_t& cachedSec,
                         char (&cachedStamp)[20], std::string& out) const
{
    // Second-granularity timestamp cache.
    const std::time_t sec = std::chrono::system_clock::to_time_t(entry.timestamp);
    if (sec != cachedSec || cachedStamp[0] == '\0')
    {
        cachedSec = sec;
        std::tm localTime{};
        localtime_s(&localTime, &sec);
        std::strftime(cachedStamp, sizeof(cachedStamp), "%Y-%m-%d %H:%M:%S", &localTime);
    }

    // One reserve'd std::string, no ostringstreams or small concatenations.
    out.clear();
    out.reserve(96 + entry.file.size() + entry.message.size());
    out.push_back('[');
    out.append(cachedStamp);
    out.append("] [");
    out.append(LevelToString(entry.level));
    out.append("] [");
    out.append(entry.file);
    out.push_back(':');
    out.append(std::to_string(entry.line));
    out.append("] ");
    out.append(entry.message);
}

void Logger::ProcessLogs()
{
    // Reusable batch of entries drained per wake-up. One lock acquire covers
    // the whole batch (vs one per entry); large batches amortise the
    // condition_variable wake-up cost.
    std::vector<LogEntry> batch;
    batch.reserve(128);

    // The caller's timestamp cache for FormatEntry: rebuilt only when the
    // wall-clock second changes.
    std::time_t cachedSec = 0;
    char        cachedStamp[20] = { 0 };  // "YYYY-MM-DD HH:MM:SS\0"

    const bool debuggerAttached = (IsDebuggerPresent() != FALSE);

    while (true)
    {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] { return !m_queue.empty() || !m_running; });

            if (!m_running && m_queue.empty())
                break;

            // Drain the whole queue at once.
            while (!m_queue.empty())
            {
                batch.emplace_back(std::move(m_queue.front()));
                m_queue.pop();
            }
        }

        bool sawError = false;
        {
            // m_writeMutex, not m_mutex: FlushBlocking may be writing the same
            // ofstream from another thread (crash handler). Without this the two
            // interleave mid-line and the tail of the log becomes unreadable
            // exactly when it matters most.
            std::lock_guard<std::mutex> writeLock(m_writeMutex);

            std::string line;
            for (auto& entry : batch)
            {
                FormatEntry(entry, cachedSec, cachedStamp, line);

                // File write - "\n" (no flush). Error entries flush once per batch.
                WriteFileLine(entry, line);

                // OutputDebugStringA is a syscall. Skip it unless a debugger is
                // attached (ETL / sampling profilers do not attach as a debugger).
                if (debuggerAttached)
                {
                    std::string withNewLine = line;
                    withNewLine.push_back('\n');
                    OutputDebugStringA(withNewLine.c_str());
                }

                sawError = sawError || (entry.level == LogLevel::Error);

                // Record ring update (deque pop_front is O(1)).
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    // Move: FormatEntry clear()s + reserve()s its output, so a
                    // moved-from 'line' is fine to reuse next iteration.
                    PushRecord(entry, std::move(line));
                }
            }

            // Flush once per batch when an error appeared, not per error entry
            // (several errors often queue up back-to-back).
            if (sawError && m_file.is_open())
                m_file.flush();
        }

        batch.clear();
    }
}

void Logger::FlushBlocking()
{
    // Do the worker's job on THIS thread. Deliberately does not notify the CV or
    // join anything: the callers are crash handlers and fatal-error paths, where
    // the worker may already be dead, suspended by the unhandled-exception
    // filter, or blocked on the very lock about to be taken.
    //
    // Both locks use a bounded try_lock rather than a plain lock: a crash INSIDE
    // Logger::Log already owns m_mutex on this thread, and a blocking lock() on a
    // non-recursive mutex would deadlock the crash handler - "lost the last few
    // lines" would become "no crash report at all". Give a real flush a fair
    // chance to win the race, then proceed anyway.
    auto boundedLock = [](std::mutex& m) -> bool
    {
        for (int i = 0; i < 200; ++i)                       // ~200 ms worst case
        {
            if (m.try_lock()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    };

    // 1. Snapshot the queue. Entries the worker already moved into its private
    //    batch are not visible here; nothing can be done about those.
    std::vector<LogEntry> pending;
    const bool haveQueueLock = boundedLock(m_mutex);
    if (haveQueueLock)
    {
        pending.reserve(m_queue.size());
        while (!m_queue.empty())
        {
            pending.emplace_back(std::move(m_queue.front()));
            m_queue.pop();
        }
        m_mutex.unlock();
    }

    // 2. Write them out. Lock order matches ProcessLogs: m_writeMutex -> m_mutex.
    const bool haveWriteLock = boundedLock(m_writeMutex);

    std::time_t cachedSec = 0;
    char        cachedStamp[20] = { 0 };
    std::string line;
    for (const auto& entry : pending)
    {
        FormatEntry(entry, cachedSec, cachedStamp, line);
        // Without the write lock too: a crash's last lines matter more than an interleaved one.
        WriteFileLine(entry, line);
        // Safe to re-take m_mutex: 'pending' is only non-empty when we owned and
        // released it above, so this is not the crash-inside-Log() case.
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            PushRecord(entry, std::move(line));
        }
    }

    // 3. Push the OS buffer to disk. This is the part that actually matters if
    //    the process is about to be terminated.
    if (m_file.is_open())
        m_file.flush();

    if (haveWriteLock)
        m_writeMutex.unlock();
}

void Logger::WriteFileLine(const LogEntry& entry, const std::string& line)
{
    if (!entry.keepInFile && LevelRank(entry.level) < LevelRank(m_fileLevel)) return;
    // A runaway spam (a warning every frame for hours) must not fill a player's disk. A harness build's
    // packs read whole logs; theirs is far above what a run writes.
#ifdef WITH_HARNESS
    constexpr std::uint64_t kFileCapBytes = 1024ull * 1024 * 1024;
#else
    constexpr std::uint64_t kFileCapBytes = 64ull * 1024 * 1024;
#endif
    if (!m_file.is_open() || m_fileCapped) return;
    if (m_fileBytes + line.size() + 1 > kFileCapBytes)
    {
        static constexpr char kNote[] = "[log] the log reached its size cap for this run: no further lines are written";
        m_file.write(kNote, sizeof(kNote) - 1);
        m_file.put('\n');
        m_file.flush();
        m_fileCapped = true;
        return;
    }
    m_file.write(line.data(), static_cast<std::streamsize>(line.size()));
    m_file.put('\n');
    m_fileBytes += line.size() + 1;
}

void Logger::WriteLine(const std::string& line)
{
    // Kept for API compatibility; ProcessLogs does not call it (it writes m_file
    // itself, under m_writeMutex).
    if (m_file.is_open())
    {
        m_file.write(line.data(), static_cast<std::streamsize>(line.size()));
        m_file.put('\n');
    }
}

void Logger::PushRecord(const LogEntry& entry, std::string&& line)
{
    if (entry.level == LogLevel::Error)
    {
        m_errorCount.fetch_add(1, std::memory_order_relaxed);
        m_errorTotal.fetch_add(1, std::memory_order_relaxed);
    }
    else if (entry.level == LogLevel::Warning)
    {
        m_warningCount.fetch_add(1, std::memory_order_relaxed);
        m_warningTotal.fetch_add(1, std::memory_order_relaxed);
    }

    LogRecord rec;
    rec.seq       = m_ringSeq.load(std::memory_order_relaxed) + 1;   // m_mutex held: sole writer
    rec.frame     = entry.frame;
    rec.thread    = entry.thread;
    rec.requestId = entry.requestId;
    rec.level     = entry.level;
    rec.source    = entry.source;
    rec.tagLen    = TagLength(entry.message);
    // FormatEntry APPENDS the message last, so it is the line's suffix.
    rec.msgOffset = static_cast<std::uint32_t>(
        line.size() >= entry.message.size() ? line.size() - entry.message.size() : 0);
    rec.text      = std::move(line);

    m_ring.push_back(std::move(rec));
    while (m_ring.size() > kRingCapacity)
        m_ring.pop_front();
    m_ringSeq.store(m_ring.back().seq, std::memory_order_release);

    // Release, paired with the acquire load in DrainAndQuiesce: everything
    // above (including the file write the caller did before calling us) must be
    // visible to the thread that observes this increment.
    m_processed.fetch_add(1, std::memory_order_release);
}

std::uint64_t Logger::CopyRecordsSince(std::uint64_t afterSeq, std::vector<LogRecord>& out,
                                       std::size_t maxCount, std::uint64_t* firstAvailable) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_ring.empty())
    {
        if (firstAvailable) *firstAvailable = 0;
        return 0;
    }
    const std::uint64_t first = m_ring.front().seq;
    const std::uint64_t last  = m_ring.back().seq;
    if (firstAvailable) *firstAvailable = first;
    if (afterSeq >= last || maxCount == 0)
        return last;

    // Seqs in the ring are contiguous, so the start is an index, not a search.
    const std::size_t start = afterSeq < first ? 0 : static_cast<std::size_t>(afterSeq - first + 1);
    const std::size_t n     = (std::min)(maxCount, m_ring.size() - start);
    out.reserve(out.size() + n);
    for (std::size_t i = 0; i < n; ++i)
        out.push_back(m_ring[start + i]);
    return last;
}

bool Logger::TryCopyRecent(std::size_t maxCount, std::vector<LogRecord>& out) const
{
    std::unique_lock<std::mutex> lock(m_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    const std::size_t n = (std::min)(maxCount, m_ring.size());
    out.insert(out.end(), m_ring.end() - static_cast<std::ptrdiff_t>(n), m_ring.end());
    return true;
}

bool Logger::DrainAndQuiesce(unsigned timeoutMs)
{
    // Fast path when the worker is already stopping or stopped. Logger::Shutdown
    // pushes a final entry, clears m_running and joins; called in that window a
    // poll loop would spin for the whole timeout with nothing left to drain it.
    if (!m_running.load(std::memory_order_relaxed))
    {
        FlushBlocking();
        return true;
    }

    // Snapshot the target BEFORE polling: entries enqueued after this call are
    // explicitly not our problem, and waiting for them in a live engine that
    // logs every frame would never terminate.
    const std::uint64_t target = m_enqueued.load(std::memory_order_acquire);

    for (unsigned waited = 0; waited < timeoutMs; ++waited)
    {
        if (m_processed.load(std::memory_order_acquire) >= target)
        {
            // Everything is formatted and written; push the OS buffer to disk.
            std::lock_guard<std::mutex> writeLock(m_writeMutex);
            if (m_file.is_open()) m_file.flush();
            return true;
        }
        // Nudge rather than wait: a second waiter on m_cv would steal the
        // worker's own wake-up (see DrainAndQuiesce in Log.h).
        m_cv.notify_one();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Timed out. Do what we still can rather than returning silently: the caller
    // is about to write a verdict and a partial log beats none.
    FlushBlocking();
    return false;
}

void Logger::ResetBadgeTallies()
{
    // Badge tallies only. m_errorTotal / m_warningTotal are deliberately NOT
    // reset here: they are the run verdict, and clicking "clear" in the editor
    // Log panel must not be able to turn a failing automated run green. The
    // ring is not cleared either: the control channel's `log` verb reads it.
    m_errorCount.store(0, std::memory_order_relaxed);
    m_warningCount.store(0, std::memory_order_relaxed);
}

void Logger::Log(LogLevel level, const char* file, int line, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    LogV(t_source, level, file, line, format, args);
    va_end(args);
}

void Logger::LogFrom(LogSource source, LogLevel level, const char* file, int line, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    LogV(source, level, file, line, format, args);
    va_end(args);
}

LogSource     Logger::CurrentSource()    { return t_source; }
std::uint32_t Logger::CurrentRequestId() { return t_request; }

void Logger::LogV(LogSource source, LogLevel level, const char* file, int line,
                  const char* format, va_list args)
{
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);

    std::string message(buffer);
    std::string fileStr(file ? file : "");

    // A ScopedContext with a sink on THIS thread wants its own copy, now -
    // synchronously, before the entry goes to the asynchronous worker.
    if (t_capture)
    {
        if (t_capture->size() < t_captureMax) t_capture->push_back(CapturedLogLine{ level, message });
        else                                  ++t_dropped;
    }

    LogEntry entry{ level, std::move(message), std::move(fileStr), line,
                    std::chrono::system_clock::now() };
    entry.frame     = g_frame.load(std::memory_order_relaxed);
    entry.thread    = static_cast<std::uint32_t>(::GetCurrentThreadId());
    entry.requestId = t_request;
    entry.source    = source;
    entry.keepInFile = t_keepInFile;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_enqueued.fetch_add(1, std::memory_order_release);
        m_queue.push(std::move(entry));
    }
    m_cv.notify_one();
}

void Logger::LogHResult(const char* expr, HRESULT hr, const char* file, int line)
{
    char* messageBuffer = nullptr;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                  FORMAT_MESSAGE_FROM_SYSTEM |
                  FORMAT_MESSAGE_IGNORE_INSERTS;

    DWORD length = FormatMessageA(
        flags,
        nullptr,
        static_cast<DWORD>(hr),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&messageBuffer),
        0,
        nullptr);

    std::string hrMessage;
    if (length != 0 && messageBuffer != nullptr)
    {
        hrMessage.assign(messageBuffer, length);
        LocalFree(messageBuffer);
    }

    std::ostringstream oss;
    oss << "HRESULT failed: " << expr
        << " (hr=0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr) << ")";

    if (!hrMessage.empty())
    {
        while (!hrMessage.empty() && (hrMessage.back() == '\n' || hrMessage.back() == '\r'))
            hrMessage.pop_back();
        oss << " - " << hrMessage;
    }

    Log(LogLevel::Error, file, line, "%s", oss.str().c_str());
}

std::string WStringToString(const wchar_t* wstr)
{
    if (!wstr)
        return {};

    int sizeNeeded = WideCharToMultiByte(
        CP_UTF8,
        0,
        wstr,
        -1,
        nullptr,
        0,
        nullptr,
        nullptr);

    if (sizeNeeded <= 0)
        return {};

    std::string strTo(static_cast<size_t>(sizeNeeded - 1), 0);

    WideCharToMultiByte(
        CP_UTF8,
        0,
        wstr,
        -1,
        &strTo[0],
        sizeNeeded,
        nullptr,
        nullptr);

    return strTo;
}
