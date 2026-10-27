#include "Graphics/PsoPrepare.h"

#include "Graphics/IGraphicsDevice.h"
#include "Graphics/PSOCache.h"            // the miss policy once prepared
#include "Resource/AssetFS.h"
#include "System/BootTimeline.h"
#include "System/CommandLine.h"
#include "System/HostPolicy.h"
#include "System/Log.h"
#include "System/PathUtil.h"
#include "System/PlayerText.h"   // the loading line
#include "System/RunWatchdog.h"
#include "System/Stage.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <thread>
#include <vector>

namespace Gfx::PsoPrepare
{
namespace
{
    struct State
    {
        bool                              active = false;
        std::string                       cachePath;
        std::unique_ptr<Sys::Stage::Scope> stage;
        std::uint32_t                     lastFinished = 0;
        float                             progress = 0.f;
    };

    State& S()
    {
        static State s_state;
        return s_state;
    }

    bool ReadFileBytes(const std::string& path, std::vector<std::uint8_t>& out)
    {
        std::ifstream f = Sys::OpenIfstream(path, std::ios::binary);
        if (!f) return false;
        out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        return !out.empty();
    }

    // -pso_miss=skip|stall, once this library holds the manifest's pipelines: what one it lacks costs in
    // play. skip (a shipped game's default): built in the background, its draws skipped until it is
    // ready; stall (a harness build's default: a test wants the build counted, not hidden): built where
    // it is asked for, the frame waiting.
    void ApplyMissPolicy()
    {
#ifdef WITH_HARNESS
        const char* fallback = "stall";
#else
        const char* fallback = "skip";
#endif
        const std::string policy = Sys::CommandLine::Get().Get("-pso_miss", fallback);
        if (policy != "skip" && policy != "stall")
            LOG_WARNING("-pso_miss=%s: skip or stall; stalling", policy.c_str());
        PSOCache::SetDeferMisses(policy == "skip");
        LOG_INFO("[PSO_PREPARE] a pipeline the library lacks %s", policy == "skip"
                 ? "is built in the background, its draws skipped until it is ready"
                 : "is built where it is asked for");
    }

    // The manifest's bytes and where they came from; empty when none ships.
    std::string FindManifest(std::vector<std::uint8_t>& bytes)
    {
        const auto& cl = Sys::CommandLine::Get();
        if (cl.Has("-pso_manifest"))
        {
            const std::string path = cl.Get("-pso_manifest");
            if (ReadFileBytes(path, bytes)) return path;
            LOG_ERROR("-pso_manifest: cannot read '%s'", path.c_str());
            return {};
        }
        const std::string beside = Sys::ExeDir() + "/pso_manifest.bin";
        if (ReadFileBytes(beside, bytes)) return beside;
        if (Resource::AssetFS::Get().HasInPak("pso_manifest.bin") &&
            Resource::AssetFS::Get().ReadFile("pso_manifest.bin", bytes))
            return "game.ipak:pso_manifest.bin";
        return {};
    }
}

void Begin(IGraphicsDevice& device, const std::string& cacheFilePath)
{
    State& s = S();
    const auto& cl = Sys::CommandLine::Get();
    const bool quietByDefault = Sys::IsEditorHost() || cl.Has("-unattended");
    const std::string policy = cl.Get("-pso_prepare", quietByDefault ? "off" : "auto");
    if (policy == "off") return;
    if (policy != "auto" && policy != "force")
    {
        LOG_WARNING("-pso_prepare=%s: auto, force or off; not preparing", policy.c_str());
        return;
    }
    std::vector<std::uint8_t> manifest;
    const std::string from = FindManifest(manifest);
    if (from.empty())
    {
        LOG_INFO("[PSO_PREPARE] no manifest ships: pipelines build on first use");
        return;
    }
    if (cacheFilePath.empty())
    {
        LOG_INFO("[PSO_PREPARE] no PSO library file: nothing to prepare into");
        return;
    }
    const std::string indexPath = cacheFilePath + ".prepared";
    if (policy == "force")
    {
        std::error_code ec;
        std::filesystem::remove(Sys::U8Path(indexPath), ec);
    }
    const unsigned cores = (std::max)(1u, std::thread::hardware_concurrency());
    const int threads = cl.GetInt("-pso_threads", static_cast<int>(cores > 1 ? cores - 1 : 1));

    if (!device.StartPipelinePrecompile(manifest, indexPath, static_cast<std::uint32_t>((std::max)(1, threads))))
    {
        const RHI::PipelinePrecompileStatus st = device.GetPipelinePrecompileStatus();
        LOG_INFO("[PSO_PREPARE] not preparing (%s): %u pipelines in %s", st.skipped.c_str(), st.total, from.c_str());
        if (st.skipped == "already prepared") ApplyMissPolicy();
        return;
    }
    s.active    = true;
    s.cachePath = cacheFilePath;
    s.stage     = std::make_unique<Sys::Stage::Scope>("pso_prepare", from);
    s.lastFinished = 0;
    s.progress     = 0.f;
    Sys::Boot::Mark("psoPrepBegin");
    LOG_INFO("[PSO_PREPARE] preparing %u pipelines from %s", device.GetPipelinePrecompileStatus().total, from.c_str());
}

void Poll(IGraphicsDevice& device)
{
    State& s = S();
    if (!s.active) return;
    const RHI::PipelinePrecompileStatus st = device.GetPipelinePrecompileStatus();
    s.progress = st.total ? static_cast<float>(st.finished) / static_cast<float>(st.total) : 1.f;
    if (st.finished != s.lastFinished)
    {
        // Progress is not a hang: each finished pipeline buys the watchdog another minute.
        s.lastFinished = st.finished;
        Sys::Stage::Progress(st.finished, st.total, "pipelines");
        Sys::ExtendRunWatchdog(60.0);
    }
    if (!st.done) return;
    device.FinishPipelinePrecompile(/*cancel*/ false, s.cachePath);
    const RHI::PipelinePrecompileStatus end = device.GetPipelinePrecompileStatus();
    LOG_INFO("[PSO_PREPARE] done built=%u present=%u failed=%u total=%u ms=%.0f", end.built, end.present, end.failed,
             end.total, end.elapsedMs);
    Sys::Boot::Mark("psoPrepEnd");
    s.stage.reset();
    s.active   = false;
    s.progress = 1.f;
    if (end.failed == 0) ApplyMissPolicy();
}

void Cancel(IGraphicsDevice& device)
{
    State& s = S();
    if (!s.active) return;
    device.FinishPipelinePrecompile(/*cancel*/ true, s.cachePath);
    const RHI::PipelinePrecompileStatus end = device.GetPipelinePrecompileStatus();
    LOG_INFO("[PSO_PREPARE] cancelled built=%u present=%u failed=%u total=%u ms=%.0f", end.built, end.present,
             end.failed, end.total, end.elapsedMs);
    s.stage.reset();
    s.active = false;
}

bool Active()
{
    return S().active;
}

float Progress()
{
    return S().progress;
}

std::string Message()
{
    const int pct = static_cast<int>(S().progress * 100.f + 0.5f);
    char buf[160];
    // Said once: the first launch, or after a graphics driver update.
    std::snprintf(buf, sizeof(buf), Sys::Text(Sys::PlayerText::PreparingShaders), pct);
    return buf;
}
}
