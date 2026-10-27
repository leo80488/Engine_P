#include "Resource/ContentRoots.h"

#include "Resource/AssetFS.h"
#include "Resource/AssetGuid.h"
#include "Resource/SharedContent.h"
#include "System/CommandLine.h"   // -pak, -pak_sealed
#include "System/PathUtil.h"
#include "System/ProcessMemory.h"
#include "System/HostPolicy.h"   // Sys::IsEditorHost

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

void Resource::MountContentRoots()
{
    // Content roots, in priority order: a project asset shadows an engine one of the
    // same relative path. Both default to the working directory until SetProjectDir /
    // SetEngineDir run. game.ipak (the sealed archive a release ships) is mounted from
    // the read-only install root (the exe's directory), not as a bare "game.ipak": the
    // working directory is not the exe directory when the game starts from a shortcut,
    // the debugger or elevated. Without a pak, AssetFS::ReadFile falls back to loose files.
    ::Resource::AssetFS::Get().AddSearchRoot("project", Sys::ProjectDir());
    ::Resource::AssetFS::Get().AddSearchRoot("engine",  Sys::EngineDir());
    // A worktree sharing the main checkout's binary assets (Resource/SharedContent.h):
    // the LAST root, read-only. No-op without its config.
    ::Resource::SharedContent::Configure();

    // The identity index spans the same roots, in the same priority order: an index over
    // the project alone misses every engine identity, and the repair path would then
    // "fix" a good engine path into an empty string.
    ::Resource::AssetGuidIndex::Get().SetContentRoots({
        Sys::ProjectDir() + "/asset",
        Sys::EngineDir()  + "/engine",
    });

    ::Resource::AssetFS& fs = ::Resource::AssetFS::Get();
    std::string pak = Sys::ExeDir() + "/game.ipak";
#ifdef WITH_HARNESS
    // -pak=<file>: mount that pak instead of the install root's, so a build/ci exe tests a package's
    // pak without the package. Never in Retail: a shipped exe mounts only what it shipped with.
    const std::string named = Sys::CommandLine::Get().Get("-pak");
    if (!named.empty()) pak = named;
    // -pak_sealed: a path under asset/, engine/ or shaders/ the pak lacks is not looked for on disk,
    // so the loose tree cannot hide what the pak misses.
    fs.SetSealed(Sys::CommandLine::Get().Has("-pak_sealed"));
#endif
    if (fs.Mount(pak))
    {
        // An update ships patch_<n>.ipak beside game.ipak (tools/pak_tool.py patch): each over the
        // ones before it, in name order.
        std::vector<std::string> patches;
        std::error_code ec;
        const std::filesystem::path pakDir = Sys::U8Path(pak).parent_path();
        for (const auto& e : std::filesystem::directory_iterator(pakDir.empty() ? Sys::U8Path(".") : pakDir, ec))
        {
            const std::string name = Sys::PathToUtf8(e.path().filename());
            if (e.is_regular_file(ec) && name.rfind("patch_", 0) == 0 && e.path().extension() == ".ipak")
                patches.push_back(Sys::PathToUtf8(e.path()));
        }
        std::sort(patches.begin(), patches.end());
        for (const std::string& patch : patches) fs.MountPatch(patch);
    }
    Sys::LogProcessMemory("after the pak mount");
}

// With neither a pak nor a loose tree, boot succeeds and then every pass fails its
// shader load: the cause is buried under hundreds of "cannot read 'shaders/...'" lines
// and the window is black. The usual trigger is launching the exe from the MSBuild
// output tree (build\pkg\x64\<cfg>\: exe + DLLs, no assets) instead of the packaged
// tree tools/package_game.py writes to build\package\<cfg>\. The pak is found from the
// exe directory but loose files resolve through the working directory (AssetFS::ReadFile
// takes the relative path verbatim), so the error names both roots.
void Resource::RequireGameContent()
{
    // A game host only: the editor runs from a source tree and names its own missing
    // files. A runtime test, not #ifndef WITH_EDITOR: this file is compiled once for both exes.
    if (Sys::IsEditorHost()) return;
    std::vector<std::uint8_t> probe;
    if (!::Resource::AssetFS::Get().ReadFile("shaders/GBuffer.vs.hlsl", probe))
    {
        std::error_code cwdEc;
        const std::string cwd =
            Sys::PathToUtf8(std::filesystem::current_path(cwdEc));
        throw std::runtime_error(
            "No game assets found.\n\n"
            "Looked for a sealed 'game.ipak' in the install root (next to the exe):\n"
            "  " + Sys::ExeDir() + "\n"
            "and for a loose 'shaders/' tree in the working directory:\n"
            "  " + (cwdEc ? std::string("<unavailable>") : cwd) + "\n\n"
            "Neither is present.\n\n"
            "Run the packaged build (DX12\\build\\package\\<config>\\Game.exe), not "
            "the raw compiler output in DX12\\build\\pkg\\x64\\<config>\\.\n\n"
            "To (re)build the package:  python tools/package_game.py");
    }
}
