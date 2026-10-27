#include "Graphics/ShaderHotReload.h"

#include "Graphics/ShaderDiskCache.h"
#include "System/HotReloadReceipt.h"
#include "System/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <mutex>

namespace HotReload
{
namespace
{
    using Json = nlohmann::json;

    std::mutex               g_requestMutex;
    std::vector<std::string> g_requested;
    std::vector<std::string> g_watchedRoots;

    double NowMs()
    {
        using namespace std::chrono;
        return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
    }

    std::string Join(const std::vector<std::string>& v, std::size_t show)
    {
        std::string s;
        for (std::size_t i = 0; i < v.size() && i < show; ++i)
        {
            if (i) s += ", ";
            s += v[i];
        }
        if (v.size() > show) s += ", +" + std::to_string(v.size() - show) + " more";
        return s;
    }

    Json FailuresJson(const ShaderRecipes::Preflight& pf)
    {
        Json a = Json::array();
        for (const ShaderRecipes::Failure& f : pf.failures)
        {
            a.push_back(Json{ { "file", f.file }, { "entry", f.entry }, { "variants", f.variants },
                              { "preexisting", f.preexisting },
                              { "message", f.message.size() > 2048 ? f.message.substr(0, 2048) + "..."
                                                                   : f.message } });
        }
        return a;
    }

    Json RecipesJson(const ShaderRecipes::Preflight& pf)
    {
        return Json{ { "recorded", pf.recipes }, { "affected", pf.affected }, { "compiled", pf.compiled },
                     { "failed", pf.failed }, { "stillFailing", pf.stillFailing } };
    }

    std::string StillFailingNote(const ShaderRecipes::Preflight& pf)
    {
        return pf.stillFailing
            ? " (" + std::to_string(pf.stillFailing) + " variant(s) that did not compile before still do not)"
            : std::string();
    }

    bool StartsWithNoCase(const std::string& s, const std::string& prefix)
    {
        if (s.size() < prefix.size()) return false;
        for (std::size_t i = 0; i < prefix.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
                return false;
        return true;
    }
}

void NoteWatchedShaderRoot(const std::string& rootedPrefix)
{
    std::lock_guard<std::mutex> lk(g_requestMutex);
    g_watchedRoots.push_back(rootedPrefix);
}

bool IsWatchedShaderPath(const std::string& rootedPath)
{
    std::lock_guard<std::mutex> lk(g_requestMutex);
    for (const std::string& r : g_watchedRoots)
        if (StartsWithNoCase(rootedPath, r)) return true;
    return false;
}

void RequestShaderReload(const std::string& path)
{
    std::lock_guard<std::mutex> lk(g_requestMutex);
    if (std::find(g_requested.begin(), g_requested.end(), path) == g_requested.end()) g_requested.push_back(path);
}

std::vector<std::string> TakeRequestedShaderReloads()
{
    std::lock_guard<std::mutex> lk(g_requestMutex);
    std::vector<std::string> out;
    out.swap(g_requested);
    return out;
}

void ShaderReloader::Note(const std::vector<std::string>& rooted, const std::vector<std::string>& absolute)
{
    if (rooted.empty() && absolute.empty()) return;
    // A shader file changed, appeared or went: the remembered include lists
    // may resolve to the wrong file now (ShaderDiskCache::ForgetIncludeLists).
    ShaderDiskCache::ForgetIncludeLists();
    if (m_batch.id == 0) m_batch.id = Open("shader", rooted);
    else                 AddFiles(m_batch.id, rooted);   // the receipt names every file of the batch
    for (const std::string& f : rooted)
        if (std::find(m_batch.rooted.begin(), m_batch.rooted.end(), f) == m_batch.rooted.end())
            m_batch.rooted.push_back(f);
    for (const std::string& f : absolute)
        if (std::find(m_batch.absolute.begin(), m_batch.absolute.end(), f) == m_batch.absolute.end())
            m_batch.absolute.push_back(f);
    m_batch.lastMs = NowMs();
}

void ShaderReloader::Tick(std::uint64_t presentedFrames, const std::function<void()>& apply)
{
    // The first frame drawn with the new shaders (m_effectiveFrame) is over:
    // this is the top of the next one.
    if (m_inFlight && presentedFrames > m_effectiveFrame) Finish();

    if (m_inFlight || m_batch.id == 0) return;
    if (NowMs() - m_batch.lastMs < kDebounceMs) return;   // still being written

    Batch batch = std::move(m_batch);
    m_batch = Batch{};
    Start(batch.id);

    ShaderRecipes::Preflight pf = ShaderRecipes::RunPreflight(batch.absolute);
    const std::string files = Join(batch.rooted, 3);

    if (pf.affected == 0)
    {
        Receipt rc;
        rc.code    = "not_in_use";
        rc.message = "no loaded shader includes " + files + " - nothing was reloaded";
        rc.detailJson = Json{ { "recipes", RecipesJson(pf) }, { "compileMs", pf.ms } }.dump(
                            -1, ' ', false, Json::error_handler_t::replace);
        Close(batch.id, rc);
        return;
    }

    if (pf.Refused())
    {
        // Nothing was cleared and nothing was written: every pass keeps the
        // bytecode - and the PSOs - it had before the edit.
        Receipt rc;
        rc.code    = "compile_failed";
        rc.failed  = pf.failed;
        rc.ok      = pf.compiled;
        rc.message = std::to_string(pf.failed) + " shader variant(s) of " + Join(pf.sources, 3) +
                     " no longer compile - nothing was reloaded, the previous shaders stay in use" +
                     StillFailingNote(pf);
        for (const ShaderRecipes::Failure& f : pf.failures)
            rc.errors.push_back(f.file + ":" + f.entry + " (" + std::to_string(f.variants) + " variant(s)" +
                                (f.preexisting ? ", failed before too" : "") + ")\n" + f.message);
        rc.detailJson = Json{ { "sources", pf.sources }, { "recipes", RecipesJson(pf) },
                              { "compileMs", pf.ms }, { "failures", FailuresJson(pf) } }.dump(
                              -1, ' ', false, Json::error_handler_t::replace);
        Close(batch.id, rc);
        return;
    }

    m_written = ShaderRecipes::Commit(pf);

    // The receipt's error window opens here: everything logged from now until
    // the effective frame was presented belongs to this reload.
    Logger& lg = Logger::Get();
    lg.DrainAndQuiesce(500);
    m_startSeq   = lg.LatestSeq();
    m_generation = ShaderRecipes::BeginReload();

    const double t0 = NowMs();
    apply();
    m_applyMs = NowMs() - t0;

    m_pf             = std::move(pf);
    m_flightId       = batch.id;
    m_effectiveFrame = presentedFrames;   // the frame this iteration draws is the first with them
    m_inFlight       = true;
}

void ShaderReloader::Finish()
{
    m_inFlight = false;

    Logger& lg = Logger::Get();
    lg.DrainAndQuiesce(500);
    std::vector<LogRecord> recs;
    lg.CopyRecordsSince(m_startSeq, recs, Logger::kRingCapacity);
    std::vector<std::string> windowErrors;
    for (const LogRecord& r : recs)
        if (r.level == LogLevel::Error) windowErrors.emplace_back(r.Message());

    const ShaderRecipes::Coverage cov = ShaderRecipes::CheckCoverage(m_pf, m_generation);

    Receipt rc;
    rc.ok             = m_pf.compiled;
    rc.effectiveFrame = m_effectiveFrame;
    const std::string frame = std::to_string(m_effectiveFrame);
    if (!windowErrors.empty())
    {
        rc.code    = "errors_after_reload";
        rc.failed  = static_cast<std::uint32_t>(windowErrors.size());
        rc.message = "reloaded for frame " + frame + ", but " + std::to_string(windowErrors.size()) +
                     " error(s) were logged while the new shaders took effect - read them";
        rc.errors  = std::move(windowErrors);
    }
    else if (!cov.stale.empty())
    {
        rc.code    = "restart_required";
        rc.failed  = cov.holders - cov.reloaded;
        rc.message = "in effect from frame " + frame + " except " + std::to_string(cov.stale.size()) +
                     " holder(s) with no hot-reload support, which keep the old shader until a restart: " +
                     Join(cov.stale, 3);
    }
    else
    {
        char ms[32];
        std::snprintf(ms, sizeof(ms), "%.0f", m_pf.ms);
        rc.message = "in effect from frame " + frame + ": " + std::to_string(m_pf.compiled) +
                     " variant(s) of " + Join(m_pf.sources, 3) + " recompiled in " + ms + " ms" +
                     StillFailingNote(m_pf);
    }
    rc.detailJson = Json{ { "sources", m_pf.sources }, { "recipes", RecipesJson(m_pf) },
                          { "compileMs", m_pf.ms }, { "applyMs", m_applyMs }, { "written", m_written },
                          { "holders", Json{ { "total", cov.holders }, { "reloaded", cov.reloaded } } },
                          { "stale", cov.stale }, { "failures", FailuresJson(m_pf) } }.dump(
                          -1, ' ', false, Json::error_handler_t::replace);
    Close(m_flightId, rc);
    m_pf = ShaderRecipes::Preflight{};
}
} // namespace HotReload
