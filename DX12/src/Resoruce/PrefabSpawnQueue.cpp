// The prefab preload and the asynchronous spawn - see Resource/PrefabSpawnQueue.h.

#include "Resource/PrefabSpawnQueue.h"

#include "Resource/AssetFS.h"
#include "Resource/MaterialSerializer.h"   // LoadMaterial (the process cache)
#include "Resource/MeshLibrary.h"
#include "Resource/PrefabSerializer.h"     // PreparePrefabForSpawn
#include "Resource/PrefabSpawn.h"          // SpawnPrefab (the async commit)
#include "Resource/PrefabTemplate.h"
#include "Resource/ResourceManager.h"
#include "Resource/TextureSystem.h"
#include "Scene/SceneInstanceLoader.h"
#include "Renderer/Renderer.h"
#include "ECS/Components.h"                // MaterialComponent
#include "ECS/Guid.h"                      // GuidMintScope (the async commit)
#include "ECS/GuidComponent.h"             // PersistentTag / RuntimeSpawnedTag
#include "ECS/GuidRegistry.h"              // EnsureGuidOn
#include "ECS/HierarchyComponents.h"       // LocalTransform
#include "Harness/ControlChannelJobs.h"    // a request is a job (harness builds)
#include "Diagnostics/Profiler.h"
#include "System/CommandLine.h"
#include "System/Log.h"
#include "System/PathUtil.h"
#include "System/TaskSystem.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_set>

namespace
{
    using namespace Resource;

    namespace Q = ::Resource::PrefabSpawnQueue;
    using Clock = std::chrono::steady_clock;

    // A preload's stages, in order (Resource/PrefabSpawnQueue.h).
    enum class Stage : uint8_t
    {
        Read,       // worker: the .ipfb's bytes (+ its parse, with the spawn template on)
        Parse,      // main: its records into the spawn template; the models and materials it names
        Prepare,    // worker: the models (SceneInstanceLoader::PrepareModelPreload), the prefab's materials
        Publish,    // main, budgeted: each model's steps
        Upload,     // worker: a model's GPU buffers, on a loader upload context - then Publish again
        Register,   // main, budgeted: one material a step (shaders registered, its packet built)
        Acquire,    // main: every texture acquired (budgeted promotion)
        Warm,       // worker: every material's shaders and pipelines (+ the surface cache's shaders)
        Surface,    // main, a listed big step: the material surface cache
        Textures,   // textures = "wait": until every texture is resident (or failed)
        Ready, Failed, Stale, Released,
    };

    struct Request
    {
        Q::PreloadId        id = 0;
        std::string         path;   // as requested (the spawn template is keyed by it)
        std::string         key;    // canonical (dedup, IsPreloaded)
        Q::PreloadOptions   opts;
        int                 refs = 1;
        uint64_t            releaseTick = 0;        // State::ticks when refs reached 0 (the pins go 2 Ticks on)
        Stage               stage = Stage::Read;
        std::string         reason;
        bool                gpuPublished = false;   // its GPU half belongs to a world now
        bool                noop = false;           // -prefab_preload_off: ready, holding nothing
        uint64_t            clearCount = 0;          // Renderer::GetWorldClearCount when it began
        uint64_t            startFrame = 0, readyFrame = 0;
        double              mainMs = 0.0, maxStepMs = 0.0;
        uint32_t            steps = 0;
        // The worker hand-off: a job owns the fields it writes until `jobDone`
        // (release; main reads it with acquire).
        std::atomic<bool>   jobRunning{ false };
        std::atomic<bool>   jobDone{ false };
        std::string         jobError;   // a job that threw: what (the job's until jobDone)
        // Read
        std::string                 ipfbStamp;
        std::vector<uint8_t>        ipfbBytes;
        std::shared_ptr<PrefabSpawnParse> parsed;   // parsed on the Read worker
        bool                        ipfbRead = false;
        // Parse
        std::vector<std::string>    scenes, prefabMaterialPaths;
        // Prepare
        std::vector<std::shared_ptr<SceneInstanceLoader::ModelPreload>> models;
        std::vector<std::unique_ptr<MaterialComponent>>                 prefabMaterials;
        size_t                      modelCursor = 0;
        // Register: every distinct material once (the prefab's MaterialRef ones,
        // then each model's F-line ones), with whether a skinned part draws it.
        std::vector<const MaterialComponent*> materials;
        std::vector<uint8_t>                  materialSkinned;
        size_t                                regCursor = 0;
        std::vector<Renderer::PreloadMaterialWarm> warms;
        bool                        wantSurface = false;
        // Acquire: the paths, fed a few at a time (FeedTextures), and the handles so far.
        std::vector<std::string>    texturePaths;
        size_t                      texCursor = 0;
        std::vector<TextureHandle>  textures;
        size_t                      texSettled = 0;   // fed and no longer loading (Progress; never goes back)
    };

    // An asynchronous spawn request.
    struct AsyncRequest
    {
        Q::SpawnId                    id = 0;
        std::string                   path;
        float                         x = 0, y = 0, z = 0;
        ECS::Guid                     seed;
        Q::SpawnOptions               opts;
        Q::PreloadId                  preload = 0;
        uint64_t                      requestFrame = 0, commitFrame = 0;
        uint64_t                      clearCount = 0;
        std::function<void(Entity)>   onReady;
        Q::SpawnState                 state = Q::SpawnState::Loading;
        std::string                   reason;
        Entity                        root = NullEntity;
        uint32_t                      rootGen = 0;
        uint64_t                      jobId = 0;
    };

    struct State
    {
        Q::Context                            ctx;
        std::vector<std::shared_ptr<Request>> requests;
        Q::PreloadId                          nextId = 1;
        Q::PreloadCounters                    stats;
        std::vector<AsyncRequest>             async;        // request order
        Q::SpawnId                            nextSpawnId = 1;
        Q::AsyncCounters                      asyncStats;
        uint64_t                              gameFrame = 0;  // CommitSpawns' (this frame's, from GameplayPreLogic on)
        std::mutex                            jobMutex;
        std::condition_variable               jobCv;
        int                                   jobsInFlight = 0;
        bool                                  shutDown = false;
        uint64_t                              frame = 0;
        uint64_t                              ticks = 0;      // Tick calls
    };

    double MsSince(Clock::time_point t0)
    {
        return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    }

    const char* StageWhat(Stage s)
    {
        switch (s)
        {
        case Stage::Read:     return "read";
        case Stage::Parse:    return "parse";
        case Stage::Prepare:  return "prepare";
        case Stage::Publish:  return "publish";
        case Stage::Upload:   return "upload";
        case Stage::Register: return "shaders";
        case Stage::Acquire:  return "textures";
        case Stage::Warm:     return "warm";
        case Stage::Surface:  return "surfaceCache";
        case Stage::Textures: return "textureWait";
        case Stage::Ready:    return "ready";
        case Stage::Failed:   return "failed";
        case Stage::Stale:    return "stale";
        default:              return "released";
        }
    }

    State& S()
    {
        static State s_instance;
        return s_instance;
    }

    std::string CanonicalPath(const std::string& p)
    {
        return Sys::RelativizeAssetPath(Sys::PathToUtf8(Sys::U8Path(p).lexically_normal()));
    }

    Request* Find(Q::PreloadId id)
    {
        for (const auto& r : S().requests) if (r->id == id) return r.get();
        return nullptr;
    }

    // Hand @p fn to a TaskSystem worker (Low: loading work). The request stays
    // alive while it runs (the job holds it).
    void RunJob(const std::shared_ptr<Request>& r, std::function<void(Request&)> fn)
    {
        State& s = S();
        r->jobError.clear();
        r->jobDone.store(false, std::memory_order_relaxed);
        r->jobRunning.store(true, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(s.jobMutex);
            ++s.jobsInFlight;
        }
        TaskSystem::Get().Push([r, fn = std::move(fn)]()
        {
            // A throw fails the request (TakeJob) instead of leaving the job
            // running forever - Tick would hold its pins and Shutdown wait for it
            // (TaskSystem.h: a counter-carrying task's decrement must survive one).
            try { fn(*r); }
            catch (const std::exception& e) { r->jobError = e.what(); }
            catch (...)                     { r->jobError = "an unknown exception"; }
            // Release: Tick reads the job's outputs once it sees this false.
            r->jobRunning.store(false, std::memory_order_release);
            r->jobDone.store(true, std::memory_order_release);
            State& st = S();
            {
                std::lock_guard<std::mutex> lk(st.jobMutex);
                --st.jobsInFlight;
            }
            st.jobCv.notify_all();
        });
    }

    // Hand back what the preload holds: its textures and its libraries.
    void ReleasePins(Request& r, IGraphicsDevice& gfx, MeshLibrary& meshLib, TextureSystem& texSys)
    {
        for (TextureHandle h : r.textures) texSys.Release(h, gfx);
        r.textures.clear();
        for (auto& m : r.models)
            if (m) SceneInstanceLoader::ReleaseModelPreload(*m, meshLib, gfx);
    }

    void Fail(Request& r, std::string why)
    {
        r.stage  = Stage::Failed;
        r.reason = std::move(why);
        ++S().stats.failed;
        LOG_WARNING("[PRELOAD] failed path=%s id=%u reason=%s", r.path.c_str(), r.id, r.reason.c_str());
    }

    // A job's hand-off on main: false = not finished yet, or it threw (then the
    // request has failed with what it threw).
    bool TakeJob(Request& r)
    {
        if (!r.jobDone.load(std::memory_order_acquire)) return false;
        r.jobDone.store(false, std::memory_order_relaxed);
        if (r.jobError.empty()) return true;
        Fail(r, "a worker step threw: " + r.jobError);
        return false;
    }

    void CollectMaterials(Request& r)
    {
        std::vector<std::string> paths;
        auto add = [&](const std::string& path, const MaterialComponent* mc, bool skinned)
        {
            const auto it = std::find(paths.begin(), paths.end(), path);
            if (it != paths.end())
            {
                if (skinned) r.materialSkinned[it - paths.begin()] = 1;
                return;
            }
            if (!mc) return;
            paths.push_back(path);
            r.materials.push_back(mc);
            r.materialSkinned.push_back(skinned ? 1 : 0);
        };
        // The models' F-line materials first: they say which parts are skinned.
        for (const auto& m : r.models)
        {
            const SceneInstanceLoader::ModelPreloadInfo info = SceneInstanceLoader::DescribeModelPreload(*m);
            for (size_t k = 0; k < info.materialPaths.size(); ++k)
                add(info.materialPaths[k], info.materials[k], info.materialSkinned[k] != 0);
        }
        // A MaterialRef replaying an F-line material is that material; one that
        // names another is drawn by the same part, skinned as it is - unknown
        // here, so skinned when the model has skinned parts (a skinned packet
        // differs only in the prev-position stream, never in its pipelines).
        bool anySkinned = std::any_of(r.materialSkinned.begin(), r.materialSkinned.end(),
                                      [](uint8_t v) { return v != 0; });
        for (size_t k = 0; k < r.prefabMaterialPaths.size(); ++k)
            add(r.prefabMaterialPaths[k], r.prefabMaterials[k].get(), anySkinned);
    }

    // The texture paths every material samples (SyncMaterialTextures' set).
    std::vector<std::string> TexturePaths(const Request& r)
    {
        std::vector<std::string> out;
        std::unordered_set<std::string> seen;
        for (const MaterialComponent* m : r.materials)
        {
            if (!m) continue;
            for (int slot = 0; slot < MaterialComponent::TEXTURESLOT_COUNT; ++slot)
                if (!m->textures[slot].name.empty() && seen.insert(m->textures[slot].name).second)
                    out.push_back(m->textures[slot].name);
            for (const auto& [name, texture] : m->customTextures)
                if (!texture.name.empty() && seen.insert(texture.name).second) out.push_back(texture.name);
        }
        return out;
    }

    // At most -prefab_preload_tex_inflight (2) of the preload's textures acquired and not yet
    // resident at a time: each acquire queues a resource-manager read and decode (a Low TaskSystem
    // job), and a character's ~35 at once would take every worker and stall the frame's own batches.
    void FeedTextures(Request& r, TextureSystem& texSys, ResourceManager& rm, IGraphicsDevice& gfx)
    {
        if (r.texSettled >= r.texturePaths.size()) return;   // all fed and settled, or none listed yet
        static const int s_inflight = [] {
            const int v = Sys::CommandLine::Get().GetInt("-prefab_preload_tex_inflight", 2);
            return v > 0 ? v : 2;
        }();
        int inflight = 0;
        for (TextureHandle h : r.textures)
            if (texSys.GetStatus(h, rm).state == TextureSystem::Status::State::Loading) ++inflight;
        // Progress() counts these: fed (a path with no handle too) and no longer loading.
        r.texSettled = (std::max)(r.texSettled, r.texCursor - static_cast<size_t>(inflight));
        while (inflight < s_inflight && r.texCursor < r.texturePaths.size())
        {
            const TextureHandle h = texSys.AcquireBudgeted(r.texturePaths[r.texCursor++], rm, gfx);
            if (!h.IsValid()) continue;
            r.textures.push_back(h);
            if (texSys.GetStatus(h, rm).state == TextureSystem::Status::State::Loading) ++inflight;
        }
    }

    // A listed big step on this frame: exempt from the per-frame main-cost bound (maxTickMsNoBig
    // skips its frame).
    void NoteBigStep()
    {
        State& s = S();
        ++s.stats.bigSteps;
        if (!s.stats.bigStepFrames.empty() && s.stats.bigStepFrames.back() == s.frame) return;
        s.stats.bigStepFrames.push_back(s.frame);
        if (s.stats.bigStepFrames.size() > 16)
            s.stats.bigStepFrames.erase(s.stats.bigStepFrames.begin());
    }

    void LogReady(const Request& r)
    {
        LOG_INFO("[PRELOAD] ready path=%s id=%u frames=%llu steps=%u materials=%zu textures=%zu surfaceCache=%d "
                 "mainMs=%.2f maxStepMs=%.2f", r.path.c_str(), r.id,
                 static_cast<unsigned long long>(r.readyFrame - r.startFrame), r.steps, r.materials.size(),
                 r.textures.size(), r.wantSurface ? 1 : 0, r.mainMs, r.maxStepMs);
    }

    // One step of @p r's main half. true = it did main work (the budget counts
    // it); false = nothing to do on main now (a worker has it, or it is done).
    bool Step(const std::shared_ptr<Request>& self, IGraphicsDevice& gfx, MeshLibrary& meshLib,
              TextureSystem& texSys, ResourceManager& rm)
    {
        Request&  r        = *self;
        State&    s        = S();
        Renderer* renderer = s.ctx.renderer;
        for (;;)
        {
            switch (r.stage)
            {
            case Stage::Read:
            {
#if defined(WITH_HARNESS) && WITH_HARNESS
                // -prefab_preload_delay_frames (tests/unit/test_prefab_spawn_determinism.py): the first main
                // step waits this many frames after the request - a preload that takes longer, in frames (a
                // worker sleep does not do it: under -workers=1 the main thread's joins wait for the sleeper).
                static const int s_delayFrames = Sys::CommandLine::Get().GetInt("-prefab_preload_delay_frames", 0);
                if (s_delayFrames > 0 && s.frame < r.startFrame + static_cast<uint64_t>(s_delayFrames)) return false;
#endif
                if (!TakeJob(r)) return false;
                if (!r.ipfbRead) { Fail(r, "the prefab does not read"); return false; }
                r.stage = Stage::Parse;
                continue;
            }

            case Stage::Parse:
            {
                // The bytes into the spawn template, then LoadPrefab's own
                // read-and-parse through it: the records kept, the refs out.
                PROF_SCOPE("Preload.Parse");
                PrefabTemplate::Put(r.path, r.ipfbStamp, std::move(r.ipfbBytes));
                r.ipfbBytes = {};
                PrefabSpawnRefs refs;
                if (r.parsed)   // parsed on the Read worker - only into the caches here
                {
                    PutParsedPrefabForSpawn(r.path, *r.parsed, refs);
                    r.parsed.reset();
                }
                else if (!PreparePrefabForSpawn(r.path, refs)) { Fail(r, "the prefab does not parse"); return true; }
                r.scenes              = std::move(refs.scenes);
                r.prefabMaterialPaths = std::move(refs.materials);
                r.stage = Stage::Prepare;
                RunJob(self, [meshLibPtr = &meshLib](Request& q)
                {
                    PROF_SCOPE("Preload.Prepare");
                    for (const std::string& scene : q.scenes)
                        q.models.push_back(SceneInstanceLoader::PrepareModelPreload(scene, *meshLibPtr));
                    for (const std::string& m : q.prefabMaterialPaths)
                    {
                        auto mc = std::make_unique<MaterialComponent>();
                        if (!LoadMaterial(m, *mc)) mc.reset();
                        q.prefabMaterials.push_back(std::move(mc));
                    }
                });
                return true;
            }

            case Stage::Prepare:
                if (!TakeJob(r)) return false;
                for (const auto& m : r.models)
                {
                    const SceneInstanceLoader::ModelPreloadInfo info = SceneInstanceLoader::DescribeModelPreload(*m);
                    if (!info.ok) { Fail(r, info.error.empty() ? "a model does not prepare" : info.error); return false; }
                }
                // From here the preload's GPU half belongs to this world.
                r.gpuPublished = true;
                r.clearCount   = renderer->GetWorldClearCount();
                r.stage        = Stage::Publish;
                continue;

            case Stage::Publish:
                while (r.modelCursor < r.models.size())
                {
                    if (SceneInstanceLoader::PublishModelPreloadStep(*r.models[r.modelCursor], gfx, meshLib, *renderer))
                        return true;
                    // The model's GPU buffers are uploaded on a worker (the device's loader upload context); its
                    // publishes follow.
                    if (SceneInstanceLoader::ModelPreloadNeedsUpload(*r.models[r.modelCursor]))
                    {
                        r.stage = Stage::Upload;
                        RunJob(self, [gfxPtr = &gfx, model = r.models[r.modelCursor]](Request&)
                        {
                            PROF_SCOPE("Preload.Upload");
                            SceneInstanceLoader::UploadModelPreload(*model, *gfxPtr);
                        });
                        return true;
                    }
                    ++r.modelCursor;
                }
                CollectMaterials(r);
                r.stage = Stage::Register;
                continue;

            case Stage::Upload:
                if (!TakeJob(r)) return false;
                r.stage = Stage::Publish;   // the same model's publishes next
                continue;

            case Stage::Register:
                if (r.regCursor < r.materials.size())
                {
                    PROF_SCOPE("Preload.Register");
                    const size_t k = r.regCursor++;
                    r.warms.push_back(renderer->PreparePreloadMaterial(*r.materials[k], r.materialSkinned[k] != 0));
                    r.wantSurface = r.wantSurface || r.warms.back().surfaceCache;
                    return true;
                }
                r.stage = Stage::Acquire;
                continue;

            case Stage::Acquire:
            {
                PROF_SCOPE("Preload.AcquireTextures");
                r.texturePaths = TexturePaths(r);
                FeedTextures(r, texSys, rm, gfx);   // the rest as these settle (Tick)
                r.stage = Stage::Warm;
                RunJob(self, [renderer](Request& q)
                {
                    PROF_SCOPE("Preload.Warm");
                    for (const Renderer::PreloadMaterialWarm& w : q.warms) renderer->WarmPreloadMaterial(w);
                    if (q.wantSurface) renderer->WarmPreloadSurfaceCache();
                });
                return true;
            }

            case Stage::Warm:
                if (!TakeJob(r)) return false;
                if (r.wantSurface) { r.stage = Stage::Surface; continue; }
                r.stage = r.opts.waitTextures ? Stage::Textures : Stage::Ready;
                if (r.stage == Stage::Ready) break;
                continue;

            case Stage::Surface:
                // A listed big step (see NoteBigStep).
                (void)renderer->PrewarmSurfaceCache();
                NoteBigStep();
                r.stage = r.opts.waitTextures ? Stage::Textures : Stage::Ready;
                if (r.stage == Stage::Ready)
                {
                    r.readyFrame = s.frame;
                    ++s.stats.ready;
                    LogReady(r);
                }
                return true;

            case Stage::Textures:
            {
                bool settled = r.texCursor >= r.texturePaths.size();
                for (TextureHandle h : r.textures)
                    if (texSys.GetStatus(h, rm).state == TextureSystem::Status::State::Loading) { settled = false; break; }
                if (!settled) return false;
                r.stage = Stage::Ready;
                break;
            }

            default:
                return false;
            }
            // Just became ready.
            r.readyFrame = s.frame;
            ++s.stats.ready;
            LogReady(r);
            return false;
        }
    }
}

namespace Resource::PrefabSpawnQueue
{
    void Bind(const Context& ctx) { S().ctx = ctx; }

    const char* PreloadStateName(PreloadState st)
    {
        switch (st)
        {
        case PreloadState::Loading: return "loading";
        case PreloadState::Ready:   return "ready";
        case PreloadState::Failed:  return "failed";
        case PreloadState::Stale:   return "stale";
        default:                    return "released";
        }
    }

    PreloadId RequestPreload(const std::string& path, const PreloadOptions& opts)
    {
        State& s = S();
        if (s.shutDown || !s.ctx.renderer || path.empty()) return 0;
        const std::string key = CanonicalPath(path);
        for (const auto& r : s.requests)
            if (r->key == key && r->refs > 0 && r->stage != Stage::Failed && r->stage != Stage::Stale &&
                r->stage != Stage::Released &&
                // a world clear since it published: stale, its next Tick says so
                !(r->gpuPublished && s.ctx.renderer->GetWorldClearCount() != r->clearCount))
            {
                ++r->refs;
                // A stream preload already ready goes back to waiting when this
                // request wants its textures (an async spawn's does).
                if (opts.waitTextures && !r->opts.waitTextures && r->stage == Stage::Ready && !r->noop)
                    r->stage = Stage::Textures;
                r->opts.waitTextures = r->opts.waitTextures || opts.waitTextures;
                return r->id;
            }
        auto r = std::make_shared<Request>();
        r->id         = s.nextId++;
        r->path       = path;
        r->key        = key;
        r->opts       = opts;
        r->startFrame = s.frame;
        ++s.stats.requests;
        s.requests.push_back(r);
        // The prefab_preload pack's reverse arm: ready at once, nothing done.
        static const bool s_off = Sys::CommandLine::Get().Has("-prefab_preload_off");
        if (s_off)
        {
            r->stage = Stage::Ready;
            r->noop  = true;
            ++s.stats.ready;
            LOG_INFO("[PRELOAD] off path=%s id=%u (-prefab_preload_off: ready, nothing preloaded)", path.c_str(), r->id);
            return r->id;
        }
        LOG_INFO("[PRELOAD] request path=%s id=%u textures=%s", path.c_str(), r->id,
                 opts.waitTextures ? "wait" : "stream");
        RunJob(r, [](Request& q)
        {
            PROF_SCOPE("Preload.Read");
            q.ipfbStamp = PrefabTemplate::FileStamp(q.path);   // before the read: a write after it is a miss
            q.ipfbRead  = AssetFS::Get().ReadFile(q.path, q.ipfbBytes);
            // The parse too, when the spawn template is on (it fills the template's records); the Parse
            // stage then only puts it into the caches.
            if (q.ipfbRead && PrefabTemplate::Enabled())
                q.parsed = ParsePrefabForSpawn(q.path, q.ipfbBytes);
        });
        return r->id;
    }

    PreloadState GetPreloadState(PreloadId id)
    {
        const Request* r = Find(id);
        if (!r || r->refs <= 0) return PreloadState::Released;   // (its pins may still be held a frame)
        // A world clear since it began: the GPU half is gone (Tick drops the
        // pins on its next run; the answer is right away).
        if (r->gpuPublished && r->stage != Stage::Failed && r->stage != Stage::Released && r->refs > 0 &&
            S().ctx.renderer && S().ctx.renderer->GetWorldClearCount() != r->clearCount)
            return PreloadState::Stale;
        switch (r->stage)
        {
        case Stage::Ready:    return PreloadState::Ready;
        case Stage::Failed:   return PreloadState::Failed;
        case Stage::Stale:    return PreloadState::Stale;
        case Stage::Released: return PreloadState::Released;
        default:              return r->refs > 0 ? PreloadState::Loading : PreloadState::Released;
        }
    }

    PreloadProgress GetPreloadProgress(PreloadId id)
    {
        PreloadProgress p;
        const Request* r = Find(id);
        if (!r) { p.what = "released"; return p; }
        // read, parse, prepare, each model's steps, one per material, the
        // texture acquire, the surface cache (when wanted), the warm-up, and one
        // per texture once the acquire has listed them, done when no longer
        // loading (FeedTextures): a textures = "wait" preload's textureWait is
        // about half of a cold one, so it must not read as finished.
        uint32_t total = 3, done = 0;
        const Stage st = r->stage;
        if (st > Stage::Read)    ++done;
        if (st > Stage::Parse)   ++done;
        if (st > Stage::Prepare) ++done;
        // The models are the Prepare job's while it runs (it fills them).
        static const std::vector<std::shared_ptr<SceneInstanceLoader::ModelPreload>> kNoModels;
        const bool modelsBusy = (st == Stage::Prepare || st == Stage::Upload) &&
                                r->jobRunning.load(std::memory_order_acquire);
        for (const auto& m : modelsBusy ? kNoModels : r->models)
        {
            const SceneInstanceLoader::ModelPreloadInfo info = SceneInstanceLoader::DescribeModelPreload(*m);
            total += info.stepsTotal;
            done  += info.stepsDone;
        }
        total += static_cast<uint32_t>(r->materials.size()) + 2 + (r->wantSurface ? 1 : 0);
        done  += static_cast<uint32_t>(r->regCursor);
        total += static_cast<uint32_t>(r->texturePaths.size());
        done  += static_cast<uint32_t>(r->texSettled);
        if (st > Stage::Acquire) ++done;
        if (r->wantSurface && st > Stage::Surface) ++done;
        if (st > Stage::Warm) ++done;
        if (st == Stage::Ready) done = total;
        p.done  = (std::min)(done, total);
        p.total = total;
        p.what  = (st == Stage::Publish && r->modelCursor < r->models.size())
                ? std::string(SceneInstanceLoader::DescribeModelPreload(*r->models[r->modelCursor]).next)
                : std::string(StageWhat(st));
        if (p.what.empty()) p.what = StageWhat(st);
        return p;
    }

    std::string GetPreloadReason(PreloadId id)
    {
        const Request* r = Find(id);
        if (!r) return "released";
        if (GetPreloadState(id) == PreloadState::Stale && r->reason.empty()) return "the world was cleared";
        return r->reason;
    }

    void ReleasePreload(PreloadId id)
    {
        Request* r = Find(id);
        if (!r || r->refs <= 0) return;
        if (--r->refs == 0)
        {
            r->releaseTick = S().ticks;
            LOG_INFO("[PRELOAD] released path=%s id=%u", r->path.c_str(), r->id);
        }
        // The pins go at the second Tick from now, not the next: App's AssetTick
        // (this frame's Tick) runs BEFORE this frame's BuildRenderScene, where the
        // material sync of an entity spawned this frame acquires its textures -
        // a pin dropped first destroys every texture only the preload held, and
        // the sync acquires and creates them all again at the next Tick (a spawn
        // and a Release in the same frame; the async commit releases at once).
    }

    bool IsPreloaded(const std::string& path)
    {
        const std::string key = CanonicalPath(path);
        for (const auto& r : S().requests)
            if (r->key == key && r->refs > 0 && !r->noop && GetPreloadState(r->id) == PreloadState::Ready) return true;
        return false;
    }

    void Tick(IGraphicsDevice& gfx, MeshLibrary& meshLib, TextureSystem& texSys, ResourceManager& rm, uint64_t frame)
    {
        State& s = S();
        s.frame = frame;
        ++s.ticks;
        if (s.requests.empty() || s.shutDown || !s.ctx.renderer) return;
        PROF_SCOPE("Preload.Tick");
        const auto t0 = Clock::now();
        static const double s_budgetMs = [] {
            // 1.5 ms: the steps are publishes and shader registrations, and the frame's whole main cost
            // stays under 2 ms (the load_upload_async pack).
            const double v = Sys::CommandLine::Get().GetFloat("-prefab_preload_budget_ms", 1.5f);
            return v > 0.0 ? v : 1.5;
        }();

        // Releases and world clears first: both hand the pins back.
        for (const auto& r : s.requests)
        {
            const bool released = r->refs <= 0 && r->stage != Stage::Released && s.ticks >= r->releaseTick + 2;
            const bool cleared  = r->refs > 0 && r->gpuPublished && r->stage != Stage::Failed &&
                                  r->stage != Stage::Stale && r->stage != Stage::Released &&
                                  s.ctx.renderer->GetWorldClearCount() != r->clearCount;
            if (!released && !cleared) continue;
            // A job in flight finishes first (it holds what it writes).
            if (r->jobRunning.load(std::memory_order_acquire)) continue;
            ReleasePins(*r, gfx, meshLib, texSys);
            if (cleared)
            {
                r->stage  = Stage::Stale;
                r->reason = "the world was cleared (frame " + std::to_string(frame) + ")";
                ++s.stats.stale;
                LOG_INFO("[PRELOAD] stale path=%s id=%u - %s", r->path.c_str(), r->id, r->reason.c_str());
            }
            else
                r->stage = Stage::Released;
        }
        // Forget the released ones (no job holds them).
        s.requests.erase(std::remove_if(s.requests.begin(), s.requests.end(), [](const std::shared_ptr<Request>& r) {
            return r->stage == Stage::Released && !r->jobRunning.load(std::memory_order_acquire);
        }), s.requests.end());

        // The preloads' remaining textures, a few in flight at a time (FeedTextures;
        // main-only fields - no job touches them, so a running Warm job does not stop it).
        for (const auto& r : s.requests)
            if (r->refs > 0 && r->stage != Stage::Failed && r->stage != Stage::Stale && r->stage != Stage::Released)
                FeedTextures(*r, texSys, rm, gfx);

        // A texture too large for the texture budget was promoted on this frame
        // (alone; TextureSystem::Tick ran just before): a listed big step.
        if (texSys.LastTickOversizedBudgetedPromotions() > 0) NoteBigStep();

        // -prefab_preload_log: one line per main step (its kind and cost).
        static const bool s_logSteps = Sys::CommandLine::Get().Has("-prefab_preload_log");
        // The steps, round robin, while the budget lasts - the budget the
        // budgeted texture promotions just used counts against it (one per-frame
        // share for the preloads' main work); one step at least, unless those
        // promotions used it all.
        const double texMs = texSys.LastTickBudgetedMs();
        const double budgetMs = s_budgetMs - texMs;
        bool any = budgetMs > 0.0, first = true;
        double lastStepMs = 0.0;   // the next step is started only if one like the last still fits
        while (any)
        {
            any = false;
            for (const auto& r : s.requests)
            {
                if (r->refs <= 0) continue;
                if (!first && MsSince(t0) + lastStepMs > budgetMs) break;
                const std::string what = s_logSteps ? GetPreloadProgress(r->id).what : std::string();
                const auto ts = Clock::now();
                if (!Step(r, gfx, meshLib, texSys, rm)) continue;
                const double ms = MsSince(ts);
                lastStepMs = ms;
                if (s_logSteps)
                    LOG_INFO("[PRELOAD] step id=%u frame=%llu kind=%s ms=%.3f", r->id,
                             static_cast<unsigned long long>(frame), what.c_str(), ms);
                ++r->steps;
                r->mainMs   += ms;
                r->maxStepMs = (std::max)(r->maxStepMs, ms);
                ++s.stats.steps;
                any   = true;
                first = false;
            }
            if (MsSince(t0) + lastStepMs > budgetMs) break;
        }
        const double tickMs = MsSince(t0);
        s.stats.mainMs   += tickMs;
        s.stats.maxTickMs = (std::max)(s.stats.maxTickMs, tickMs);
        if (s.stats.bigStepFrames.empty() || s.stats.bigStepFrames.back() != frame)
            s.stats.maxTickMsNoBig = (std::max)(s.stats.maxTickMsNoBig, tickMs + texMs);
    }

    // ==== The asynchronous spawn ====
    const char* SpawnStateName(SpawnState st)
    {
        switch (st)
        {
        case SpawnState::Loading:   return "loading";
        case SpawnState::Ready:     return "ready";
        case SpawnState::Failed:    return "failed";
        default:                    return "cancelled";
        }
    }

    namespace
    {
        AsyncRequest* FindSpawn(SpawnId id)
        {
            for (AsyncRequest& r : S().async) if (r.id == id) return &r;
            return nullptr;
        }

        // A request that will not commit: its preload goes, its job says why.
        void EndSpawn(AsyncRequest& r, SpawnState st, std::string why)
        {
            State& s = S();
            r.state  = st;
            r.reason = std::move(why);
            if (r.preload) ReleasePreload(r.preload);
            r.preload = 0;
            r.onReady = nullptr;
            if (st == SpawnState::Cancelled) ++s.asyncStats.cancelled;
            if (st == SpawnState::Failed)    ++s.asyncStats.failed;
            Harness::Jobs::Fail(r.jobId, st == SpawnState::Cancelled ? "cancelled" : "spawn_failed", r.reason, {});
            LOG_INFO("[SPAWN_ASYNC] %s path=%s id=%u reason=%s", SpawnStateName(st), r.path.c_str(), r.id,
                     r.reason.c_str());
        }

        // The commit: the instance, with the request's seed, every Guid it mints
        // derived from that seed (GuidMintScope).
        void CommitOne(AsyncRequest& r, World& world, bool ready)
        {
            State& s = S();
            const auto tc = Clock::now();
            Entity root = NullEntity;
            ECS::GuidStats::BeginCommitWindow();
            {
                ECS::GuidMintScope mint(r.seed);
                root = ::Resource::SpawnPrefab(r.path, world, *s.ctx.assetMgr, s.ctx.renderer, s.ctx.clips, &r.seed,
                                             "Engine.SpawnPrefabAsync");
                if (root != NullEntity && r.opts.persistent)
                {
                    world.AddComponent<PersistentTag>(root, {});
                    world.AddComponent<RuntimeSpawnedTag>(root, {});
                    (void)ECS::EnsureGuidOn(world, root);
                }
            }
            ECS::GuidStats::EndCommitWindow();
            const double ms = MsSince(tc);
            if (!ready)
            {
                ++s.asyncStats.late;
                s.asyncStats.lateMs += ms;
            }
            if (root == NullEntity) { EndSpawn(r, SpawnState::Failed, "the prefab did not instantiate"); return; }
            if (auto* lt = world.GetComponent<LocalTransform>(root)) lt->translation = { r.x, r.y, r.z };
            if (r.preload) ReleasePreload(r.preload);
            r.preload     = 0;
            r.root        = root;
            r.rootGen     = world.GetGeneration(root);
            r.commitFrame = s.gameFrame;
            r.state       = SpawnState::Ready;
            ++s.asyncStats.commits;
            Harness::Jobs::Succeed(r.jobId, "{\"entity\":" + std::to_string(static_cast<uint32_t>(root)) +
                                                ",\"commitFrame\":" + std::to_string(r.commitFrame) + "}");
            LOG_INFO("[SPAWN_ASYNC] commit path=%s id=%u requestFrame=%llu frame=%llu waited=%llu ready=%d ms=%.2f",
                     r.path.c_str(), r.id, static_cast<unsigned long long>(r.requestFrame),
                     static_cast<unsigned long long>(r.commitFrame),
                     static_cast<unsigned long long>(r.commitFrame - r.requestFrame), ready ? 1 : 0, ms);
            std::function<void(Entity)> cb = std::move(r.onReady);
            r.onReady = nullptr;
            if (cb) cb(root);
        }
    }

    SpawnId RequestSpawn(const std::string& path, float x, float y, float z, const SpawnOptions& opts,
                         std::function<void(Entity)> onReady)
    {
        State& s = S();
        if (s.shutDown || !s.ctx.renderer || !s.ctx.world || !s.ctx.assetMgr || path.empty()) return 0;
        AsyncRequest r;
        r.id           = s.nextSpawnId++;
        r.path         = path;
        r.x = x; r.y = y; r.z = z;
        r.opts         = opts;
        r.seed         = ECS::Guid::Generate();   // now, in request order
        r.requestFrame = s.gameFrame;
        r.clearCount   = s.ctx.renderer->GetWorldClearCount();
        r.onReady      = std::move(onReady);
        r.jobId        = Harness::Jobs::TryCreate("prefab.spawn", path, Logger::CurrentRequestId());
        Harness::Jobs::Start(r.jobId);
        ++s.asyncStats.requests;
        LOG_INFO("[SPAWN_ASYNC] request path=%s id=%u frame=%llu frames=%d", path.c_str(), r.id,
                 static_cast<unsigned long long>(r.requestFrame), opts.frames);
        // The prefab_spawn_async pack's reverse arm: the request spawns now.
        static const bool s_sync = Sys::CommandLine::Get().Has("-prefab_async_sync");
        s.async.push_back(std::move(r));
        if (s_sync)
        {
            CommitOne(s.async.back(), *s.ctx.world, /*ready*/ true);
            return s.async.back().id;
        }
        // Its textures too: the commit then promotes nothing - a stream preload
        // still promoting at the commit would have its pending textures taken
        // out of the budget by the new entities' own (ordinary) acquires and
        // created all at once on the next frame.
        PreloadOptions po;
        po.waitTextures = true;
        s.async.back().preload = RequestPreload(path, po);
        return s.async.back().id;
    }

    SpawnState GetSpawnState(SpawnId id)
    {
        const AsyncRequest* r = FindSpawn(id);
        if (!r) return SpawnState::Cancelled;
        // A world clear cancels it (CommitSpawns says so in the log; the answer is right away).
        if (r->state == SpawnState::Loading && S().ctx.renderer &&
            S().ctx.renderer->GetWorldClearCount() != r->clearCount)
            return SpawnState::Cancelled;
        return r->state;
    }

    std::string GetSpawnReason(SpawnId id)
    {
        const AsyncRequest* r = FindSpawn(id);
        if (!r) return "unknown";
        if (GetSpawnState(id) == SpawnState::Cancelled && r->reason.empty()) return "the world was cleared";
        return r->reason;
    }

    PreloadProgress GetSpawnProgress(SpawnId id)
    {
        const AsyncRequest* r = FindSpawn(id);
        PreloadProgress p;
        if (!r) { p.what = "cancelled"; return p; }
        if (r->state == SpawnState::Loading && r->preload) return GetPreloadProgress(r->preload);
        p.done = p.total = 1;
        p.what = SpawnStateName(r->state);
        return p;
    }

    Entity GetSpawnEntity(SpawnId id)
    {
        const AsyncRequest* r = FindSpawn(id);
        World* w = S().ctx.world;
        if (!r || r->state != SpawnState::Ready || !w || !w->IsAlive(r->root) || w->GetGeneration(r->root) != r->rootGen)
            return NullEntity;
        return r->root;
    }

    bool CancelSpawn(SpawnId id)
    {
        AsyncRequest* r = FindSpawn(id);
        if (!r || r->state != SpawnState::Loading) return false;
        EndSpawn(*r, SpawnState::Cancelled, "cancelled");
        return true;
    }

    void CommitSpawns(World& world, uint64_t frame)
    {
        State& s = S();
        s.gameFrame = frame;
        s.asyncStats.pending = 0;
        if (s.async.empty() || s.shutDown || !s.ctx.renderer) return;
        PROF_SCOPE("Spawn.AsyncCommit");
        const auto t0 = Clock::now();
        int commits = 0;
        for (AsyncRequest& r : s.async)
        {
            if (r.state != SpawnState::Loading) continue;
            if (s.ctx.renderer->GetWorldClearCount() != r.clearCount)
            {
                EndSpawn(r, SpawnState::Cancelled, "the world was cleared");
                continue;
            }
            const bool     fixed = r.opts.frames >= 0;
            const uint64_t due   = r.requestFrame + static_cast<uint64_t>((std::max)(1, r.opts.frames));
            if (fixed && frame < due) { ++s.asyncStats.pending; continue; }
            const PreloadState ps = GetPreloadState(r.preload);
            if (!fixed && ps == PreloadState::Failed)
            {
                EndSpawn(r, SpawnState::Failed, "the preload failed: " + GetPreloadReason(r.preload));
                continue;
            }
            const bool ready = ps == PreloadState::Ready;
            // At the first point it is ready, within this frame's share; a
            // fixed-frame commit happens on its frame whatever it costs.
            if (!fixed && (!ready || commits >= 2 || MsSince(t0) >= 3.0)) { ++s.asyncStats.pending; continue; }
            CommitOne(r, world, ready);
            ++commits;
        }
        // Forget old finished requests (a handle asking after them answers cancelled).
        size_t finished = 0;
        for (const AsyncRequest& r : s.async) if (r.state != SpawnState::Loading) ++finished;
        if (finished > 256)
        {
            size_t drop = finished - 256;
            s.async.erase(std::remove_if(s.async.begin(), s.async.end(), [&drop](const AsyncRequest& r) {
                if (drop == 0 || r.state == SpawnState::Loading) return false;
                --drop;
                return true;
            }), s.async.end());
        }
    }

    const AsyncCounters& AsyncStats() { return S().asyncStats; }

    void Shutdown(IGraphicsDevice& gfx, MeshLibrary& meshLib, TextureSystem& texSys)
    {
        State& s = S();
        if (s.shutDown) return;
        s.shutDown = true;
        // The pending spawns go first (their callbacks hold script references).
        for (AsyncRequest& r : s.async) r.onReady = nullptr;
        s.async.clear();
        {
            std::unique_lock<std::mutex> lk(s.jobMutex);
            s.jobCv.wait(lk, [&s] { return s.jobsInFlight == 0; });
        }
        for (const auto& r : s.requests) ReleasePins(*r, gfx, meshLib, texSys);
        s.requests.clear();
    }

    const PreloadCounters& Stats() { return S().stats; }
}
