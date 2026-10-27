#include "Platform/PlatformServices.h"

#include "System/Log.h"
#include "System/PathUtil.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>

namespace
{
    // Achievements and stats in <writable>/platform/local.json, written at each change (a few a session).
    class LocalPlatformServices final : public IPlatformServices
    {
    public:
        LocalPlatformServices() { Load(); }

        const char* Name() const override { return "local"; }

        bool UnlockAchievement(const std::string& id) override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (id.empty() || !m_unlocked.insert(id).second) return false;
            LOG_INFO("[PLATFORM] achievement unlocked: %s", id.c_str());
            return Save();
        }

        bool IsAchievementUnlocked(const std::string& id) const override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_unlocked.count(id) != 0;
        }

        std::vector<std::string> UnlockedAchievements() const override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return { m_unlocked.begin(), m_unlocked.end() };
        }

        bool ResetAchievement(const std::string& id) override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_unlocked.erase(id) == 0) return false;
            return Save();
        }

        bool SetStat(const std::string& id, double value) override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (id.empty()) return false;
            m_stats[id] = value;
            return Save();
        }

        double GetStat(const std::string& id) const override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto it = m_stats.find(id);
            return it == m_stats.end() ? 0.0 : it->second;
        }

        void SetPresence(const std::string& key, const std::string& value) override
        {
            // Nobody to tell without a store; the log says what a store would show.
            LOG_INFO("[PLATFORM] presence %s = %s", key.c_str(), value.c_str());
        }

        std::string SaveFolder() const override { return Sys::ProjectDataDir(); }

    private:
        std::string File() const { return Sys::WritableDataDir() + "/platform/local.json"; }

        void Load()
        {
            std::ifstream in(Sys::U8Path(File()));
            if (!in) return;
            try
            {
                nlohmann::json j;
                in >> j;
                // Locals, not j.value(...).items() in the loop: items() would view a temporary already gone.
                const nlohmann::json unlocked = j.value("achievements", nlohmann::json::array());
                const nlohmann::json stats    = j.value("stats", nlohmann::json::object());
                for (const auto& a : unlocked) m_unlocked.insert(a.get<std::string>());
                for (const auto& [k, v] : stats.items()) m_stats[k] = v.get<double>();
            }
            catch (const std::exception& e)
            {
                LOG_WARNING("[PLATFORM] %s does not parse (%s): starting empty", File().c_str(), e.what());
            }
        }

        // Caller holds m_mutex. Written to a temporary and moved over, so a crash mid-write keeps the old file.
        bool Save() const
        {
            nlohmann::json j;
            j["achievements"] = std::vector<std::string>(m_unlocked.begin(), m_unlocked.end());
            j["stats"]        = m_stats;
            std::error_code ec;
            const std::filesystem::path path = Sys::U8Path(File());
            std::filesystem::create_directories(path.parent_path(), ec);
            const std::filesystem::path tmp = path.wstring() + L".tmp";
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                out << j.dump(2) << '\n';
                if (!out) return false;
            }
            std::filesystem::rename(tmp, path, ec);
            return !ec;
        }

        mutable std::mutex            m_mutex;
        std::set<std::string>         m_unlocked;
        std::map<std::string, double> m_stats;
    };

    std::mutex                         g_mutex;
    std::unique_ptr<IPlatformServices> g_services;
}

namespace Plat
{
    IPlatformServices& Services()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_services) g_services = std::make_unique<LocalPlatformServices>();
        return *g_services;
    }

    void SetServices(std::unique_ptr<IPlatformServices> services)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_services = std::move(services);
        if (g_services) LOG_INFO("[PLATFORM] services: %s", g_services->Name());
    }
}
