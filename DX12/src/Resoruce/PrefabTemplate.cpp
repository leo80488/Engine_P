// The runtime spawn's template caches - see Resource/PrefabTemplate.h.

#include "Resource/PrefabTemplate.h"

#include "Resource/AssetFS.h"
#include "System/CommandLine.h"
#include "System/PathUtil.h"

#include <filesystem>
#include <system_error>
#include <unordered_map>

namespace
{
    int g_depth = 0;
    Resource::PrefabTemplate::Counters g_stats;

    struct CachedFile
    {
        std::string          stamp;
        std::vector<uint8_t> bytes;
    };
    std::unordered_map<std::string, CachedFile>& Files()
    {
        static std::unordered_map<std::string, CachedFile> s_files;
        return s_files;
    }
}

namespace Resource::PrefabTemplate
{
    Scope::Scope()  { ++g_depth; }
    Scope::~Scope() { if (g_depth > 0) --g_depth; }

    bool Enabled()
    {
        static const bool s_on = !Sys::CommandLine::Get().Has("-prefab_template_off");
        return s_on;
    }

    // The depth first: the flag is asked for only by a runtime spawn, so a run
    // that makes none does not read it.
    bool Active() { return g_depth > 0 && Enabled(); }

    std::string FileStamp(const std::string& path)
    {
        const AssetFS& fs = AssetFS::Get();
        // The pak first, as ReadFile does: a packaged build is sealed.
        if (fs.HasInPak(path)) return "pak";
        const std::string onDisk = fs.ResolveOnDisk(path);
        if (onDisk.empty()) return {};
        std::error_code ec;
        const auto p    = Sys::U8Path(onDisk);
        const auto size = std::filesystem::file_size(p, ec);
        if (ec) return {};
        const auto when = std::filesystem::last_write_time(p, ec);
        if (ec) return {};
        return std::to_string(size) + ":" + std::to_string(when.time_since_epoch().count());
    }

    bool ReadFile(const std::string& path, std::vector<uint8_t>& out)
    {
        const std::string stamp = FileStamp(path);
        auto& files = Files();
        if (!stamp.empty())
        {
            const auto it = files.find(path);
            if (it != files.end() && it->second.stamp == stamp)
            {
                out = it->second.bytes;
                ++g_stats.fileHits;
                return true;
            }
        }
        if (!AssetFS::Get().ReadFile(path, out))
        {
            files.erase(path);
            return false;
        }
        ++g_stats.fileMisses;
        if (!stamp.empty())
            files[path] = CachedFile{ stamp, out };
        else
            files.erase(path);
        return true;
    }

    void Put(const std::string& path, const std::string& stamp, std::vector<uint8_t> bytes)
    {
        if (stamp.empty()) return;
        Files()[path] = CachedFile{ stamp, std::move(bytes) };
    }

    Counters& Stats() { return g_stats; }
}
