#include "Harness/AssetToolShaderCook.h"

#ifdef WITH_HARNESS

#include "Graphics/DxcCompiler.h"
#include "Graphics/ShaderDiskCache.h"
#include "Resource/AssetFS.h"
#include "System/CommandLine.h"
#include "System/Log.h"
#include "System/PathUtil.h"
#include "System/Sha256.h"

#include <nlohmann/json.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace Harness
{
namespace
{
    using Json = nlohmann::json;

    constexpr int kDone = 0, kRefused = 1, kUsage = 2;
    // Where every key of a variant manifest lives: the runtime's cache folder.
    constexpr const char kCachePrefix[] = "shaders/shader_cache_dxil/";

    struct Job
    {
        std::string                 key;
        std::string                 rel;      // the key below shaders/shader_cache_dxil/
        DxcCompiler::CompileOptions opts;
        // Filled by the worker.
        enum class Outcome { Pending, Compiled, Failed, MissingSource, WriteFailed } outcome = Outcome::Pending;
        std::string                 message;
        bool                        warned = false;
    };

    bool ParseStage(const std::string& s, RHI::ShaderStage& out)
    {
        static const struct { const char* name; RHI::ShaderStage stage; } kStages[] = {
            { "MS", RHI::ShaderStage::MS }, { "AS", RHI::ShaderStage::AS }, { "VS", RHI::ShaderStage::VS },
            { "HS", RHI::ShaderStage::HS }, { "DS", RHI::ShaderStage::DS }, { "GS", RHI::ShaderStage::GS },
            { "PS", RHI::ShaderStage::PS }, { "CS", RHI::ShaderStage::CS }, { "LIB", RHI::ShaderStage::LIB },
        };
        for (const auto& st : kStages)
            if (s == st.name) { out = st.stage; return true; }
        return false;
    }

    Json CompilerIdentity()
    {
        Json j = Json::object();
        wchar_t buf[MAX_PATH] = {};
        if (HMODULE m = ::GetModuleHandleW(L"dxcompiler.dll"))
        {
            const DWORD n = ::GetModuleFileNameW(m, buf, MAX_PATH);
            if (n > 0 && n < MAX_PATH)
            {
                const std::string path = Sys::PathToUtf8(std::filesystem::path(buf));
                std::uint64_t bytes = 0;
                j["path"]   = path;
                j["sha256"] = Sys::Sha256FileHex(path, &bytes);
                j["bytes"]  = bytes;
            }
        }
        return j;
    }
}

int RunShaderCook(Json& out)
{
    auto& cl = Sys::CommandLine::Get();
    const std::string manifestPath = cl.Get("-manifest");
    std::string outDir = cl.Get("-out");
    auto usage = [&out](const std::string& message)
    {
        out = Json{ { "ok", false }, { "error", "usage" }, { "message", message } };
        return kUsage;
    };
    if (manifestPath.empty() || outDir.empty())
        return usage("shadercook -manifest=<variants.json> -out=<dir> [-cook_jobs=N]");
    while (!outDir.empty() && (outDir.back() == '/' || outDir.back() == '\\')) outDir.pop_back();

    std::vector<uint8_t> text;
    if (!Sys::ReadFileBytes(manifestPath, text) || text.empty())
        return usage("cannot read -manifest='" + manifestPath + "'");
    Json doc = Json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
    if (!doc.is_object() || !doc.contains("variants") || !doc["variants"].is_array())
        return usage("'" + manifestPath + "' is not a variant list ({\"variants\": [...]})");

    // ---- The jobs: every variant, as LoadOrCompile would compile it ----
    std::vector<Job> jobs;
    Json refused = Json::array();
    for (const Json& v : doc["variants"])
    {
        Job job;
        job.key = v.value("key", std::string());
        RHI::ShaderStage stage{};
        if (job.key.rfind(kCachePrefix, 0) != 0 || !ParseStage(v.value("stage", std::string()), stage) ||
            v.value("source", std::string()).empty())
        {
            refused.push_back(Json{ { "key", job.key }, { "why", "not a shaders/shader_cache_dxil/ key with a "
                                                                   "source and a stage" } });
            continue;
        }
        job.rel                   = job.key.substr(sizeof(kCachePrefix) - 1);
        job.opts.sourceName       = v.value("source", std::string());
        job.opts.entry            = v.value("entry", std::string("main"));
        job.opts.stage            = stage;
        job.opts.includeDir       = v.value("includeDir", std::string());
        job.opts.debug            = false;   // the shipping compile (-O3), whatever recorded it
        job.opts.hlsl2021         = v.value("hlsl2021", false);
        job.opts.enable16BitTypes = v.value("enable16BitTypes", false);
        for (const Json& d : v.value("defines", Json::array()))
            if (d.is_array() && d.size() == 2)
                job.opts.defines.push_back({ d[0].get<std::string>(), d[1].get<std::string>() });
        jobs.push_back(std::move(job));
    }

    // ---- Compile, in parallel (DxcCompiler is thread-safe) ----
    const auto t0 = std::chrono::steady_clock::now();
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    unsigned nJobs = static_cast<unsigned>(std::clamp(cl.GetInt("-cook_jobs", static_cast<int>(std::min(hw > 1 ? hw - 1 : 1u, 8u))), 1, 64));
    nJobs = std::min<unsigned>(nJobs, static_cast<unsigned>(std::max<size_t>(jobs.size(), 1)));
    std::atomic<size_t> next{ 0 };
    auto worker = [&]()
    {
        for (size_t i = next.fetch_add(1); i < jobs.size(); i = next.fetch_add(1))
        {
            Job& job = jobs[i];
            std::vector<uint8_t> src;
            if (!Resource::AssetFS::Get().ReadFile(job.opts.sourceName, src) || src.empty())
            {
                job.outcome = Job::Outcome::MissingSource;
                job.message = "cannot read '" + job.opts.sourceName + "'";
                continue;
            }
            const DxcCompiler::CompileResult cr = DxcCompiler::Compile(src.data(), src.size(), job.opts);
            if (!cr.ok)
            {
                job.outcome = Job::Outcome::Failed;
                job.message = cr.errorMsg.empty() ? std::string("DXC compile failed (no diagnostics)") : cr.errorMsg;
                continue;
            }
            job.warned = !cr.errorMsg.empty();
            const std::vector<uint8_t> blob =
                ShaderDiskCache::BuildContainer(job.opts.entry, job.opts.stage, cr.dxil.data(), cr.dxil.size());
            if (!ShaderDiskCache::WriteFileAtomic(outDir + "/" + job.rel, blob))
            {
                job.outcome = Job::Outcome::WriteFailed;
                job.message = "cannot write '" + outDir + "/" + job.rel + "'";
                continue;
            }
            job.outcome = Job::Outcome::Compiled;
        }
    };
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < nJobs; ++t) pool.emplace_back(worker);
    worker();
    for (std::thread& t : pool) t.join();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    // ---- The report ----
    uint32_t compiled = 0, warnings = 0;
    Json failed = Json::array(), missing = Json::array(), writeFailed = Json::array();
    for (const Job& job : jobs)
    {
        if (job.warned) ++warnings;
        switch (job.outcome)
        {
        case Job::Outcome::Compiled: ++compiled; break;
        case Job::Outcome::MissingSource: missing.push_back(job.key); break;
        case Job::Outcome::WriteFailed: writeFailed.push_back(Json{ { "key", job.key }, { "message", job.message } }); break;
        default:
            failed.push_back(Json{ { "key", job.key }, { "source", job.opts.sourceName }, { "entry", job.opts.entry },
                                   { "stage", static_cast<int>(job.opts.stage) }, { "message", job.message } });
            break;
        }
    }
    const bool ok = failed.empty() && missing.empty() && writeFailed.empty() && refused.empty();
    out = Json{ { "ok", ok }, { "manifest", manifestPath }, { "out", outDir }, { "variants", jobs.size() + refused.size() },
                { "compiled", compiled }, { "failed", failed }, { "missingSource", missing },
                { "writeFailed", writeFailed }, { "refused", refused }, { "warnings", warnings },
                { "compiler", CompilerIdentity() }, { "jobs", nJobs }, { "ms", ms } };
    LOG_INFO("[SHADERCOOK] variants=%zu compiled=%u failed=%zu missing=%zu refused=%zu warnings=%u jobs=%u ms=%.0f out=%s",
             jobs.size() + refused.size(), compiled, failed.size(), missing.size(), refused.size(), warnings, nJobs, ms,
             outDir.c_str());
    return ok ? kDone : kRefused;
}
} // namespace Harness

#endif // WITH_HARNESS
