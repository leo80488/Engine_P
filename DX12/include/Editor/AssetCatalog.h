#pragma once

// AssetCatalog - the asset tree's metadata (path, name, extension, size, modified time) walked on a
// worker thread, so the Content Browser's search over every folder never touches the disk in the UI
// pass. Refresh() asks for a new walk (an import, a rename, a delete, a file-watcher event, the
// browser's periodic rescan); readers take the latest Snapshot, which is immutable. Editor-only.

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Editor
{
    class AssetCatalog
    {
    public:
        struct Entry
        {
            std::string   path;    // forward slashes, as the browser lists it
            std::string   name;    // the file name
            std::string   lowerName;
            std::string   ext;     // lower case, with the dot ("" for a folder)
            std::uint64_t size = 0;
            std::int64_t  modified = 0;   // time_t
            bool          folder = false;
        };

        struct Snapshot
        {
            std::vector<Entry> entries;
            double             buildMs = 0.0;      // the worker's walk
            std::uint64_t      generation = 0;     // 1 for the first walk
        };

        AssetCatalog() = default;
        ~AssetCatalog();
        AssetCatalog(const AssetCatalog&) = delete;
        AssetCatalog& operator=(const AssetCatalog&) = delete;

        // The directories walked (the project's asset/, the engine's content).
        void SetRoots(std::vector<std::string> roots);
        // Asks for a new walk; one already running is followed by another.
        void Refresh();
        // The latest finished walk (nullptr before the first).
        std::shared_ptr<const Snapshot> Current() const;
        // Up to @p limit entries whose name contains @p text (no case), files before folders, in
        // path order; @p micros receives the search's time.
        std::vector<Entry> Search(const std::string& text, std::size_t limit, double* micros = nullptr) const;
        // Joins the worker (the editor's shutdown; also the destructor).
        void Stop();

    private:
        void Run();

        mutable std::mutex              m_mutex;
        std::condition_variable         m_wake;
        std::thread                     m_worker;
        std::vector<std::string>        m_roots;
        std::shared_ptr<const Snapshot> m_current;
        std::uint64_t                   m_generation = 0;
        bool                            m_requested = false;
        bool                            m_stop = false;
    };
}
