#include "Graphics/ShaderRecipes.h"
#include "System/BoundedParallel.h"   // RunPreflight's compiles

#include "Graphics/ShaderDiskCache.h"
#include "Resource/AssetFS.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace ShaderRecipes
{
namespace
{
    struct Holder
    {
        const void* owner = nullptr;
        std::string tag;
        uint64_t    loadGen = 0;   // generation of its last load
    };

    struct Recipe
    {
        DxcCompiler::CompileOptions opts;
        std::string                 scanDir;
        bool                        lastOk = true;
        std::vector<Holder>         holders;
    };

    struct Registry
    {
        std::mutex                                mutex;
        std::unordered_map<std::string, Recipe>   recipes;      // by cachePath
        std::unordered_map<const void*, uint64_t> clearedGen;   // owner -> generation of its last ClearCaches
        std::atomic<uint64_t>                     generation{ 1 };
        // Every recipe ever noted (Snapshot) - never erased.
        std::unordered_map<std::string, Recorded> seen;         // by cachePath
    };

    // Never destroyed: a ShaderLibrary destructor that runs during static
    // teardown still calls ForgetOwner, and must not find a dead registry.
    Registry& R()
    {
        static Registry* s_registry = new Registry();
        return *s_registry;
    }

    // One spelling per file: absolute, lexically normal, '/' separators, ASCII
    // lower case (Windows paths are case-insensitive, and the watcher lowercases).
    std::string Key(const std::string& path)
    {
        std::error_code ec;
        const std::filesystem::path p   = Sys::U8Path(path);
        const std::filesystem::path abs = std::filesystem::absolute(p, ec);
        std::string s = Sys::PathToUtf8((ec ? p : abs).lexically_normal());
        for (char& c : s)
        {
            if (c == '\\')                 c = '/';
            else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return s;
    }

    // Does @p path, or anything it transitively #includes, sit in `changed`?
    // Memoised across the whole pre-flight, so a header shared by a hundred
    // recipes is scanned once. A file met again while its own scan is still
    // open (an include cycle) reads as "no" for that branch.
    struct ClosureScan
    {
        const std::unordered_set<std::string>& changed;
        std::unordered_map<std::string, int>   state;   // 0 open, 1 no, 2 yes

        bool Hits(const std::string& path, const std::string& scanDir)
        {
            const std::string k    = Key(path);
            const std::string memo = k + '|' + scanDir;
            if (auto it = state.find(memo); it != state.end()) return it->second == 2;
            if (changed.count(k)) { state[memo] = 2; return true; }
            state[memo] = 0;
            bool hit = false;
            for (const std::string& inc : ShaderDiskCache::IncludesOf(path, scanDir))
                if (Hits(inc, scanDir)) { hit = true; break; }
            state[memo] = hit ? 2 : 1;
            return hit;
        }
    };
}

void Note(const Load& l)
{
    if (l.cachePath.empty()) return;
    Registry& r = R();
    const uint64_t gen = r.generation.load(std::memory_order_acquire);
    std::lock_guard<std::mutex> lk(r.mutex);
    {
        Recorded& seen = r.seen[l.cachePath];
        seen.cachePath = l.cachePath;
        seen.opts      = l.opts;
        seen.ok        = l.ok;
        if (std::find(seen.tags.begin(), seen.tags.end(), l.tag) == seen.tags.end())
            seen.tags.push_back(l.tag);
    }
    Recipe& rec = r.recipes[l.cachePath];
    rec.opts    = l.opts;
    rec.scanDir = l.scanDir;
    rec.lastOk  = l.ok;
    for (Holder& h : rec.holders)
    {
        // A library is one holder whatever it is called; a pass is its tag.
        if (h.owner == l.owner && (l.owner != nullptr || h.tag == l.tag))
        {
            h.loadGen = gen;
            return;
        }
    }
    rec.holders.push_back(Holder{ l.owner, l.tag, gen });
}

void NoteOwnerCleared(const void* owner)
{
    if (!owner) return;
    Registry& r = R();
    std::lock_guard<std::mutex> lk(r.mutex);
    r.clearedGen[owner] = r.generation.load(std::memory_order_acquire);
}

void ForgetOwner(const void* owner)
{
    if (!owner) return;
    Registry& r = R();
    std::lock_guard<std::mutex> lk(r.mutex);
    for (auto it = r.recipes.begin(); it != r.recipes.end();)
    {
        auto& hs = it->second.holders;
        hs.erase(std::remove_if(hs.begin(), hs.end(),
                                [owner](const Holder& h) { return h.owner == owner; }),
                 hs.end());
        it = hs.empty() ? r.recipes.erase(it) : std::next(it);
    }
    r.clearedGen.erase(owner);
}

Preflight RunPreflight(const std::vector<std::string>& changedFiles)
{
    const auto t0 = std::chrono::steady_clock::now();
    Preflight pf;

    struct Item
    {
        std::string                 cachePath;
        DxcCompiler::CompileOptions opts;
        std::string                 scanDir;
        bool                        lastOk = true;
    };
    std::vector<Item> all;
    {
        Registry& r = R();
        std::lock_guard<std::mutex> lk(r.mutex);
        all.reserve(r.recipes.size());
        for (const auto& [cachePath, rec] : r.recipes)
            all.push_back(Item{ cachePath, rec.opts, rec.scanDir, rec.lastOk });
    }
    pf.recipes = static_cast<uint32_t>(all.size());

    std::unordered_set<std::string> changed;
    for (const std::string& f : changedFiles) changed.insert(Key(f));

    ClosureScan scan{ changed, {} };
    std::vector<const Item*> work;
    std::unordered_set<std::string> sources;
    for (const Item& it : all)
    {
        if (!scan.Hits(it.opts.sourceName, it.scanDir)) continue;
        work.push_back(&it);
        if (sources.insert(it.opts.sourceName).second) pf.sources.push_back(it.opts.sourceName);
    }
    std::sort(pf.sources.begin(), pf.sources.end());
    pf.affected = static_cast<uint32_t>(work.size());

    // Compile - nothing below touches a cache, a PSO or a file. The main thread
    // is one of the workers; DxcCompiler::Compile is thread-safe.
    struct Out
    {
        bool                 ok = false;
        std::vector<uint8_t> container;
        std::string          error;
    };
    std::vector<Out> outs(work.size());
    // At most 7 compiles beside this thread: each holds a compiler and its include closure in memory.
    Sys::RunBounded(static_cast<uint32_t>(work.size()), Sys::BurstHelpers(7), [&](uint32_t i)
    {
        const Item& it = *work[i];
        std::vector<uint8_t> src;
        if (!Resource::AssetFS::Get().ReadFile(it.opts.sourceName, src) || src.empty())
        {
            outs[i].error = "cannot read " + it.opts.sourceName;
            return;
        }
        const DxcCompiler::CompileResult cr = DxcCompiler::Compile(src.data(), src.size(), it.opts);
        if (!cr.ok)
        {
            outs[i].error = cr.errorMsg.empty() ? std::string("DXC compile failed (no diagnostics)")
                                                : cr.errorMsg;
            return;
        }
        outs[i].ok        = true;
        outs[i].container = ShaderDiskCache::BuildContainer(it.opts.entry, it.opts.stage,
                                                            cr.dxil.data(), cr.dxil.size());
    });

    for (size_t i = 0; i < work.size(); ++i)
    {
        const Item& it = *work[i];
        Out& o = outs[i];
        if (o.ok)
        {
            ++pf.compiled;
            pf.compiledKeys.push_back(it.cachePath);
            pf.containers.emplace_back(it.cachePath, std::move(o.container));
            continue;
        }
        const bool pre = !it.lastOk;
        ++(pre ? pf.stillFailing : pf.failed);
        auto g = std::find_if(pf.failures.begin(), pf.failures.end(), [&](const Failure& f)
        {
            return f.file == it.opts.sourceName && f.entry == it.opts.entry &&
                   f.message == o.error && f.preexisting == pre;
        });
        if (g == pf.failures.end())
            pf.failures.push_back(Failure{ it.opts.sourceName, it.opts.entry, o.error, 1, pre });
        else
            ++g->variants;
    }
    std::stable_sort(pf.failures.begin(), pf.failures.end(),
                     [](const Failure& a, const Failure& b) { return !a.preexisting && b.preexisting; });

    pf.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return pf;
}

uint32_t Commit(Preflight& pf)
{
    uint32_t written = 0;
    for (auto& [path, bytes] : pf.containers)
    {
        if (ShaderDiskCache::WriteFileAtomic(path, bytes)) ++written;
        else LOG_WARNING("ShaderRecipes: could not write '%s' - it recompiles from the source on first use",
                         path.c_str());
    }
    pf.containers.clear();
    pf.containers.shrink_to_fit();
    return written;
}

uint64_t BeginReload()
{
    return R().generation.fetch_add(1, std::memory_order_acq_rel) + 1;
}

Coverage CheckCoverage(const Preflight& pf, uint64_t generation)
{
    Coverage c;
    Registry& r = R();
    std::lock_guard<std::mutex> lk(r.mutex);
    std::unordered_set<std::string> seen;
    for (const std::string& key : pf.compiledKeys)
    {
        auto it = r.recipes.find(key);
        if (it == r.recipes.end()) continue;   // every holder is gone: nothing runs the old version
        for (const Holder& h : it->second.holders)
        {
            ++c.holders;
            bool reloaded = h.loadGen >= generation;
            if (!reloaded && h.owner)
            {
                auto cg = r.clearedGen.find(h.owner);
                reloaded = cg != r.clearedGen.end() && cg->second >= generation;
            }
            if (reloaded)
            {
                ++c.reloaded;
                continue;
            }
            std::string s = it->second.opts.sourceName + ":" + it->second.opts.entry +
                            " (" + (h.tag.empty() ? std::string("?") : h.tag) + ")";
            if (seen.insert(s).second) c.stale.push_back(std::move(s));
        }
    }
    std::sort(c.stale.begin(), c.stale.end());
    return c;
}

size_t Count()
{
    Registry& r = R();
    std::lock_guard<std::mutex> lk(r.mutex);
    return r.recipes.size();
}

std::vector<Recorded> Snapshot()
{
    Registry& r = R();
    std::vector<Recorded> out;
    {
        std::lock_guard<std::mutex> lk(r.mutex);
        out.reserve(r.seen.size());
        for (const auto& [path, rec] : r.seen) out.push_back(rec);
    }
    std::sort(out.begin(), out.end(), [](const Recorded& a, const Recorded& b) { return a.cachePath < b.cachePath; });
    return out;
}
} // namespace ShaderRecipes
