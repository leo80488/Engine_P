#include "Editor/PersonalFiles.h"

#include "System/CommandLine.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include "imgui/imgui.h"

#include <string>
#include <system_error>

namespace
{
    namespace fs = std::filesystem;

    // ImGui keeps the pointer in io.IniFilename, so the text must outlive the context.
    std::string g_imguiIniPath;

    bool HarnessReadsSeed()
    {
        const Sys::CommandLine& cl = Sys::CommandLine::Get();
#if defined(WITH_HARNESS) && WITH_HARNESS
        if (cl.Has("-editor_prefs_developer")) return false;
#endif
        return cl.Has("-unattended");
    }
}

namespace Editor::PersonalFiles
{
    fs::path SeedPath(const fs::path& live)
    {
        const fs::path name = live.filename();
        return Sys::U8Path(Sys::EngineDir()) / (name.stem().string() + ".default" + name.extension().string());
    }

    bool EnsureFromSeed(const fs::path& live)
    {
        std::error_code ec;
        if (fs::exists(live, ec)) return true;

        const fs::path seed = SeedPath(live);
        if (!fs::exists(seed, ec)) return false;

        fs::create_directories(live.parent_path(), ec);
        ec.clear();
        if (!fs::copy_file(seed, live, fs::copy_options::skip_existing, ec) || ec)
        {
            LOG_WARNING("PersonalFiles: could not make %s from %s (%s)", Sys::PathToUtf8(live).c_str(),
                        Sys::PathToUtf8(seed).c_str(), ec.message().c_str());
            return false;
        }
        LOG_INFO("PersonalFiles: %s did not exist, made from %s", Sys::PathToUtf8(live).c_str(),
                 Sys::PathToUtf8(seed).c_str());
        return true;
    }

    fs::path PrefsToRead(const fs::path& live)
    {
        std::error_code ec;
        if (HarnessReadsSeed())
        {
            const fs::path seed = SeedPath(live);
            const bool     found = fs::exists(seed, ec);
            LOG_INFO("[EDITOR_PREFS] source=seed file=%s%s", Sys::PathToUtf8(seed).c_str(),
                     found ? "" : " (missing: compiled-in defaults)");
            return found ? seed : fs::path();
        }
        const bool found = EnsureFromSeed(live);
        LOG_INFO("[EDITOR_PREFS] source=file file=%s%s", Sys::PathToUtf8(live).c_str(),
                 found ? "" : " (missing: compiled-in defaults)");
        return found ? live : fs::path();
    }

    void UseImGuiIni(ImGuiIO& io)
    {
        const fs::path live = Sys::U8Path(Sys::WritableDataDir()) / "imgui.ini";
        EnsureFromSeed(live);
        g_imguiIniPath = Sys::PathToUtf8(live);
        io.IniFilename = g_imguiIniPath.c_str();
    }
}
