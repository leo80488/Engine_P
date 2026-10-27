#include "System/PsoCachePaths.h"

#include "System/CommandLine.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include <filesystem>
#include <system_error>

// PSO disk cache: read from two roots, write to exactly one.
// Load: prefer the writable copy (warmed by this machine's driver); when it is absent
// (fresh clone, install or first launch) fall back to a pre-warmed pso_cache.bin the
// packager may ship next to the exe in the read-only install root.
// Save: always the writable root - the project directory in the editor, the per-user
// root in Game.exe. Never the install root: it can be read-only (C:\Program Files), and a
// failed write there means every launch rebuilds every PSO from DXIL.
// A cache from another GPU/driver is harmless: D3D12 rejects it (DRIVER_VERSION_MISMATCH /
// ADAPTER_NOT_FOUND) and GraphicsDX12::InitPSOLibrary starts an empty library.
void Sys::ResolvePsoCachePaths(std::string& psoCacheWritePath, std::string& psoCacheShippedPath)
{
    psoCacheWritePath = Sys::WritableDataDir() + "/pso_cache.bin";
    psoCacheShippedPath = Sys::ExeDir() + "/pso_cache.bin";
#ifdef WITH_HARNESS
    // -pso_cache_dir=<dir> makes a run cold by construction. Both roots above are shared and
    // persistent, so a pack asserting PSO-library behaviour would otherwise test leftover state
    // from the previous run. A relative value resolves under -artifacts= (as
    // -preview_screenshot does), and the runner gives every run a fresh directory, so the
    // library cannot exist yet; an absolute value is taken as given, for a deliberately warm
    // cache across runs. Both roots are replaced: keeping the ExeDir fallback would let a
    // shipped pre-warmed cache warm the run. Applies to both hosts.
    {
        const std::string psoDir = Sys::CommandLine::Get().Get("-pso_cache_dir");
        if (!psoDir.empty())
        {
            std::filesystem::path root = Sys::U8Path(psoDir);
            if (root.is_relative() && Sys::HasArtifactDir())
                root = Sys::U8Path(Sys::ArtifactDir()) / root;
            std::error_code pec;
            std::filesystem::create_directories(root, pec);
            if (pec)
            {
                LOG_ERROR("-pso_cache_dir='%s' could not be created (%s); the "
                          "default PSO cache roots are used and this run is NOT "
                          "cold.", psoDir.c_str(), pec.message().c_str());
            }
            else
            {
                psoCacheWritePath   = Sys::PathToUtf8(root / "pso_cache.bin");
                psoCacheShippedPath = psoCacheWritePath;
                LOG_INFO("[PSO] disk library redirected to '%s' (-pso_cache_dir)",
                         psoCacheWritePath.c_str());
            }
        }
    }
#endif
}
