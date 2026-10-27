#pragma once

// FileWatcher - async directory watch via ReadDirectoryChangesW.
//
// A background thread receives write / rename / size events for files under a
// directory and pushes the changed paths into a deduplicating set. The main
// thread drains it via PollChanges(), which never blocks on filesystem I/O.
//
// Used by hot reload: shaders/ for .hlsl edits by default, other asset types
// through SetExtensions.

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace HotReload
{

class FileWatcher
{
public:
    FileWatcher() = default;
    ~FileWatcher() { Shutdown(); }

    FileWatcher(const FileWatcher&)            = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    // Begin watching @p dir recursively. Returns false if the directory can't
    // be opened. Call Shutdown() before destruction.
    bool Init(const std::string& dir);

    // Optional extension allow-list (lowercase, WITH the dot: ".itex").
    // Empty (the default) keeps the built-in shader filter: .hlsl/.hlsli,
    // skipping shader_cache_dxil/. Set BEFORE Init; the worker thread reads
    // it without a lock.
    void SetExtensions(std::vector<std::string> exts) { m_extFilter = std::move(exts); }

    // Stop the worker thread, close the directory handle. Idempotent.
    void Shutdown();

    // Drain the pending change set. Returns paths (relative to the watched
    // dir, lowercased, forward slashes) seen since the last call. Never
    // blocks. Empty result == nothing happened.
    std::vector<std::string> PollChanges();

private:
    void WorkerLoop();
    bool Wants(const std::string& normalizedPath) const;

    std::vector<std::string>       m_extFilter;   // see SetExtensions
    std::string                    m_dir;
    void*                          m_dirHandle = nullptr;   // HANDLE; void* keeps <windows.h> out of the header
    std::thread                    m_thread;
    std::atomic<bool>              m_running{false};
    std::mutex                     m_mutex;
    std::unordered_set<std::string> m_pending;             // dedup
};

} // namespace HotReload
