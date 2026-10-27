#include "Editor/AssetCatalog.h"

#include "System/PathUtil.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>

namespace Editor
{
namespace
{
    std::string Lower(std::string s)
    {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    std::int64_t WriteTime(const std::filesystem::directory_entry& e)
    {
        std::error_code ec;
        const auto ft = e.last_write_time(ec);
        if (ec) return 0;
        const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(ft);
        return static_cast<std::int64_t>(std::chrono::system_clock::to_time_t(sys));
    }
}

AssetCatalog::~AssetCatalog()
{
    Stop();
}

void AssetCatalog::SetRoots(std::vector<std::string> roots)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_roots = std::move(roots);
}

void AssetCatalog::Refresh()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stop) return;
        m_requested = true;
        if (!m_worker.joinable()) m_worker = std::thread([this] { Run(); });
    }
    m_wake.notify_one();
}

std::shared_ptr<const AssetCatalog::Snapshot> AssetCatalog::Current() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_current;
}

std::vector<AssetCatalog::Entry> AssetCatalog::Search(const std::string& text, std::size_t limit, double* micros) const
{
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<Entry> out;
    const std::shared_ptr<const Snapshot> snap = Current();
    if (snap && !text.empty())
    {
        const std::string needle = Lower(text);
        for (int pass = 0; pass < 2 && out.size() < limit; ++pass)   // files, then folders
            for (const Entry& e : snap->entries)
            {
                if (e.folder != (pass == 1) || e.lowerName.find(needle) == std::string::npos) continue;
                out.push_back(e);
                if (out.size() >= limit) break;
            }
    }
    if (micros)
        *micros = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    return out;
}

void AssetCatalog::Stop()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_one();
    if (m_worker.joinable()) m_worker.join();
}

void AssetCatalog::Run()
{
    for (;;)
    {
        std::vector<std::string> roots;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock, [this] { return m_requested || m_stop; });
            if (m_stop) return;
            m_requested = false;
            roots = m_roots;
        }
        const auto t0 = std::chrono::steady_clock::now();
        auto snap = std::make_shared<Snapshot>();
        for (const std::string& root : roots)
        {
            std::error_code ec;
            for (auto it = std::filesystem::recursive_directory_iterator(Sys::U8Path(root), ec);
                 !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
            {
                {
                    // A shutdown mid-walk does not wait for the walk.
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (m_stop) return;
                }
                const std::filesystem::directory_entry& de = *it;
                Entry e;
                e.folder = de.is_directory(ec);
                e.path = Sys::PathToUtf8(de.path());
                std::replace(e.path.begin(), e.path.end(), '\\', '/');
                e.name = Sys::PathToUtf8(de.path().filename());
                e.lowerName = Lower(e.name);
                if (!e.folder)
                {
                    e.ext = Lower(Sys::PathToUtf8(de.path().extension()));
                    if (e.ext == ".meta") continue;   // a sidecar, never listed
                    e.size = de.file_size(ec);
                }
                e.modified = WriteTime(de);
                snap->entries.push_back(std::move(e));
            }
        }
        std::sort(snap->entries.begin(), snap->entries.end(),
                  [](const Entry& a, const Entry& b) { return a.path < b.path; });
        snap->buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::lock_guard<std::mutex> lock(m_mutex);
        snap->generation = ++m_generation;
        m_current = std::move(snap);
    }
}
}
