#include "System/FileWatcher.h"

#include "System/Log.h"
#include "System/PathUtil.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cwchar>

namespace HotReload
{

namespace
{
    std::string Narrow(const wchar_t* w, DWORD wlen)
    {
        if (wlen == 0) return {};
        const int len = WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(wlen),
                                            nullptr, 0, nullptr, nullptr);
        std::string out(len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(wlen),
                            out.data(), len, nullptr, nullptr);
        return out;
    }

    void Normalize(std::string& s)
    {
        for (char& c : s)
        {
            if (c == '\\') c = '/';
            else           c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        }
    }

    // Which events are worth waking the renderer for. @p p is already
    // normalised (lowercase, forward slashes, relative to the watched dir).
    //
    // Do not drop the shader_cache_dxil/ skip: DXC's blob cache lives inside
    // the watched shaders/ tree, so a cold cache would be a feedback loop
    // (compiling writes a .ishdr -> the watcher fires -> the shader + PSO
    // caches are dropped -> everything recompiles -> more writes). A warm cache
    // writes nothing, so the loop shows only after something invalidates every
    // blob (a cache-key change, a clean checkout, a DXC upgrade).
    bool WantsPath(const std::string& p)
    {
        if (p.empty()) return false;
        if (p.find("shader_cache_dxil/") != std::string::npos) return false;

        const size_t dot = p.rfind('.');
        if (dot == std::string::npos) return false;
        const std::string ext = p.substr(dot);
        return ext == ".hlsl" || ext == ".hlsli";
    }
}

bool FileWatcher::Wants(const std::string& normalizedPath) const
{
    // Empty filter: the built-in shader filter (WantsPath).
    if (m_extFilter.empty()) return WantsPath(normalizedPath);

    const size_t dot = normalizedPath.rfind('.');
    if (dot == std::string::npos) return false;
    const std::string ext = normalizedPath.substr(dot);
    for (const std::string& e : m_extFilter)
        if (ext == e) return true;
    return false;
}

bool FileWatcher::Init(const std::string& dir)
{
    if (m_running.load()) return true;

    m_dir = dir;

    HANDLE h = CreateFileW(
        Sys::Utf8ToWide(dir).c_str(), FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);

    if (h == INVALID_HANDLE_VALUE)
    {
        LOG_ERROR("FileWatcher::Init: CreateFile('%s') failed (err=%lu)",
                  dir.c_str(), GetLastError());
        return false;
    }

    m_dirHandle = h;
    m_running.store(true);
    m_thread = std::thread([this]{ WorkerLoop(); });

    LOG_INFO("FileWatcher: watching '%s'", dir.c_str());
    return true;
}

void FileWatcher::Shutdown()
{
    if (!m_running.exchange(false)) return;

    // Closing the handle wakes any in-flight ReadDirectoryChangesW with
    // ERROR_OPERATION_ABORTED, letting the worker thread exit cleanly. The
    // worker also checks m_running so it bails immediately on the next loop.
    if (m_dirHandle)
    {
        CancelIoEx(static_cast<HANDLE>(m_dirHandle), nullptr);
        CloseHandle(static_cast<HANDLE>(m_dirHandle));
        m_dirHandle = nullptr;
    }
    if (m_thread.joinable())
        m_thread.join();
}

std::vector<std::string> FileWatcher::PollChanges()
{
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_pending.empty()) return out;

    // extract() moves a key out without mutating it in place: a const_cast +
    // move would leave moved-from keys in their buckets (UB - a later hash
    // operation lands in the wrong bucket).
    out.reserve(m_pending.size());
    while (!m_pending.empty())
        out.push_back(std::move(m_pending.extract(m_pending.begin()).value()));
    return out;
}

void FileWatcher::WorkerLoop()
{
    // DWORD-aligned, as ReadDirectoryChangesW requires. 16 KB holds hundreds
    // of pending entries; an overflow reports zero bytes transferred.
    alignas(DWORD) std::byte buffer[16 * 1024];

    OVERLAPPED ov{};
    HANDLE evt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!evt) return;
    ov.hEvent = evt;

    while (m_running.load())
    {
        DWORD bytesReturned = 0;
        BOOL ok = ReadDirectoryChangesW(
            static_cast<HANDLE>(m_dirHandle),
            buffer, static_cast<DWORD>(sizeof(buffer)),
            TRUE,           // recursive
            FILE_NOTIFY_CHANGE_LAST_WRITE
            | FILE_NOTIFY_CHANGE_FILE_NAME
            | FILE_NOTIFY_CHANGE_SIZE,
            &bytesReturned, &ov, nullptr);

        if (!ok)
        {
            const DWORD err = GetLastError();
            if (err == ERROR_OPERATION_ABORTED) break;   // shutdown
            LOG_WARNING("FileWatcher: ReadDirectoryChangesW failed (err=%lu)", err);
            break;
        }

        // Wait for completion or shutdown.
        const DWORD wait = WaitForSingleObject(evt, INFINITE);
        if (wait != WAIT_OBJECT_0 || !m_running.load()) break;

        DWORD transferred = 0;
        if (!GetOverlappedResult(static_cast<HANDLE>(m_dirHandle),
                                 &ov, &transferred, FALSE))
            break;
        ResetEvent(evt);

        if (transferred == 0)
        {
            // Buffer overflow: the kernel dropped events and cannot say which.
            // Do not signal a blanket reload here: an overflow means a flood of
            // hundreds of events (a mass cache write; a human edit is 1-2 events),
            // and reloading would restart the recompile storm that caused it.
            // Worst case a developer re-saves the file.
            LOG_WARNING("FileWatcher: change buffer overflowed; events dropped "
                        "(re-save the file if a hot-reload was missed)");
            continue;
        }

        // Walk the linked list of FILE_NOTIFY_INFORMATION records.
        //
        // Every access is bounded against `transferred`. The offsets come from
        // the kernel and are well-formed in the normal case, but a truncated or
        // malformed record would otherwise hand Narrow() a garbage
        // FileNameLength and send WideCharToMultiByte reading off the end of
        // this 16 KB stack buffer.
        const std::byte* const end =
            buffer + (std::min)(static_cast<size_t>(transferred), sizeof(buffer));

        // Collected first, then inserted under ONE lock instead of one per
        // record, so a flood does not fight the main thread's drain for the mutex.
        std::vector<std::string> batch;

        const std::byte* p = buffer;
        while (p + offsetof(FILE_NOTIFY_INFORMATION, FileName) <= end)
        {
            const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(p);

            const std::byte* nameEnd = p + offsetof(FILE_NOTIFY_INFORMATION, FileName)
                                         + info->FileNameLength;
            if (info->FileNameLength == 0 || nameEnd > end)
                break;   // truncated record: stop rather than read past the buffer

            std::string path = Narrow(info->FileName,
                                      info->FileNameLength / sizeof(wchar_t));
            Normalize(path);

            if (Wants(path))
                batch.push_back(std::move(path));

            const DWORD next = info->NextEntryOffset;
            // 0 = last record. Anything smaller than the header would not
            // advance (or would walk backwards): treat it as malformed.
            if (next == 0 || next < offsetof(FILE_NOTIFY_INFORMATION, FileName))
                break;
            p += next;
        }

        if (!batch.empty())
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            for (auto& s : batch) m_pending.insert(std::move(s));
        }
    }

    CloseHandle(evt);
}

} // namespace HotReload
