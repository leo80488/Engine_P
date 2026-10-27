#include "System/HotReloadReceipt.h"

#include "Harness/ControlChannelJobs.h"
#include "System/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>

namespace HotReload
{
namespace
{
    using Json = nlohmann::json;

    struct Pending
    {
        std::string              kind;
        std::vector<std::string> files;
        std::uint64_t            detectedFrame = 0;
        bool                     job = false;
    };

    std::mutex                                g_mutex;
    std::unordered_map<std::uint64_t, Pending> g_open;
    std::atomic<std::uint64_t>                g_localIds{ 0 };

    constexpr std::size_t kMaxErrors     = 16;
    constexpr std::size_t kMaxErrorBytes = 4096;

    std::string FileList(const std::vector<std::string>& files, std::size_t show)
    {
        std::string s;
        for (std::size_t i = 0; i < files.size() && i < show; ++i)
        {
            if (i) s += ", ";
            s += files[i];
        }
        if (files.size() > show) s += ", +" + std::to_string(files.size() - show) + " more";
        return s;
    }
}

std::uint64_t Open(const char* kind, const std::vector<std::string>& files)
{
    const std::string k = kind ? kind : "?";
    std::uint64_t id = Harness::Jobs::Create(("hotreload." + k).c_str(), FileList(files, 3), 0);
    const bool job = id != 0;
    if (!job) id = g_localIds.fetch_add(1, std::memory_order_relaxed) + 1;   // Retail: no job registry
    std::lock_guard<std::mutex> lk(g_mutex);
    g_open[id] = Pending{ k, files, Logger::CurrentFrame(), job };
    return id;
}

void AddFiles(std::uint64_t id, const std::vector<std::string>& files)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_open.find(id);
    if (it == g_open.end()) return;
    std::vector<std::string>& have = it->second.files;
    for (const std::string& f : files)
        if (std::find(have.begin(), have.end(), f) == have.end())
            have.push_back(f);
}

void Start(std::uint64_t id)
{
    bool job = false;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        auto it = g_open.find(id);
        if (it == g_open.end()) return;
        job = it->second.job;
    }
    if (job) Harness::Jobs::Start(id);
}

void Close(std::uint64_t id, const Receipt& r)
{
    Pending p;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        auto it = g_open.find(id);
        if (it == g_open.end()) return;   // closed already
        p = std::move(it->second);
        g_open.erase(it);
    }

    std::vector<std::string> errors;
    for (const std::string& e : r.errors)
    {
        if (errors.size() >= kMaxErrors) break;
        errors.push_back(e.size() > kMaxErrorBytes ? e.substr(0, kMaxErrorBytes) + "..." : e);
    }

    Json out = Json::object();
    if (!r.detailJson.empty())
    {
        Json d = Json::parse(r.detailJson, nullptr, false);
        if (d.is_object()) out = std::move(d);
    }
    out["kind"]           = p.kind;
    out["files"]          = p.files;
    out["detectedFrame"]  = p.detectedFrame;
    out["effectiveFrame"] = r.effectiveFrame;
    out["ok"]             = r.ok;
    out["failed"]         = r.failed;
    if (r.errors.size() > errors.size()) out["errorsDropped"] = r.errors.size() - errors.size();
    const std::string outText = out.dump(-1, ' ', false, Json::error_handler_t::replace);

    if (p.job)
    {
        if (r.code.empty()) Harness::Jobs::Succeed(id, outText);
        else                Harness::Jobs::Fail(id, r.code, r.message, errors, outText);
    }

    // THE line (log tag [HOTRELOAD]); the first diagnostic rides along, the rest
    // are in the job.
    std::string line = "[HOTRELOAD] " + p.kind + " #" + std::to_string(id) + " " +
                       (r.code.empty() ? std::string("ok") : r.code) + ": " + r.message;
    if (!errors.empty())
    {
        line += "\n" + errors.front();
        if (r.errors.size() > 1)
            line += "\n(+" + std::to_string(r.errors.size() - 1) + " more in the receipt's job)";
    }
    if (r.code.empty() || r.code == "not_in_use") LOG_INFO("%s", line.c_str());
    else if (r.code == "restart_required")       LOG_WARNING("%s", line.c_str());
    else                                         LOG_ERROR("%s", line.c_str());
}
} // namespace HotReload
