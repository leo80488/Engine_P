#pragma once

// Runtime prefab spawns that do not happen all at once: the preload and the asynchronous spawn.
//
// The preload (Lua Engine.PreloadPrefab): everything the first runtime spawn of a prefab would
// read, decode, register, upload, load and build, done before it and spread over frames, so that
// spawn costs what a warm spawn costs and its frames load no shader and build no pipeline:
//
//   worker (TaskSystem Low)   the .ipfb's bytes (and its parse, with the spawn template on); each
//                             model's records, .meshlib (read + parsed), .iskel, skinned parts'
//                             streams and morph blocks; every material (.imat, the process
//                             cache) - SceneInstanceLoader::PrepareModelPreload
//   main, budgeted            at App's AssetTick (the render lane idle), at most
//                             -prefab_preload_budget_ms (1.5) of steps a frame, one at least:
//                             the .ipfb's records into the spawn template, each library
//                             committed, the template's skeleton registered, one skinned part's
//                             shared source a step; the materials' custom shaders registered
//                             (Renderer::PreparePreloadMaterial); their textures acquired
//                             budgeted (TextureSystem::AcquireBudgeted: promoted under the
//                             texture budget)
//   worker                    a model's GPU buffers, on the device's loader upload context
//   main, a listed big step   the material surface cache's atlas and bake resources, when a
//                             material would page it (Renderer::PrewarmSurfaceCache)
//   worker                    each material's shaders loaded and every pass's pipelines built
//                             (Renderer::WarmPreloadMaterial)
//
// Ready = all of that done (+ every texture resident with textures = "wait"). A preload holds
// what it pulled in - its libraries and textures - until its last handle is released, and one
// frame past it: the pins go at the second Tick after the release, so an entity spawned in the
// release's frame acquires them in that frame's scene build first. A world clear releases the
// GPU half (the shared skinned sources, the world's libraries): the preload answers stale and
// drops its pins; preload again to re-publish (the CPU half - template, material cache,
// registrations, shaders, pipelines - stays).
// -prefab_preload_off: a request is ready at once and does nothing (the prefab_preload pack's
// reverse arm).
//
// The asynchronous spawn (Lua Engine.SpawnPrefabAsync): a request mints the instance seed at
// once, on main, in request order (the Guid a synchronous spawn would have drawn), and starts (or
// shares) a preload of the prefab that waits for its textures (a shared stream preload is
// upgraded), so the instance is committed with its own textures and its next frames promote
// none. The entity does not exist until the commit (CommitSpawns, App's frame loop right before
// GameplayPreLogic), in request order: at the first commit point the preload is ready (at most
// 2 commits / 3 ms a frame), or, with `frames`, exactly that many frames after the request (at
// least one; a preload not ready by then finishes synchronously there: a late commit).
// The commit is Resource::SpawnPrefab with the request's seed, inside an ECS::GuidMintScope of
// it: the instance's identities depend on the request alone, never on the frame that commits it.
// A world clear cancels every pending request; a pending request is not saved.
// -prefab_async_sync (the prefab_spawn_async pack's reverse arm): a request spawns synchronously.
//
// Main-thread API (the workers are internal).

#include "ECS/ECS.h"   // Entity

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class World;
class Renderer;
class IGraphicsDevice;

namespace Resource
{
    class AssetManager;
    class AnimationClipSystem;
    class MeshLibrary;
    class ResourceManager;
    class TextureSystem;
}

namespace Resource::PrefabSpawnQueue
{
    // What the queue works with, from the Lua binding (the engine's own).
    struct Context
    {
        World*               world    = nullptr;
        AssetManager*        assetMgr = nullptr;
        Renderer*            renderer = nullptr;
        AnimationClipSystem* clips    = nullptr;
    };
    void Bind(const Context& ctx);

    using PreloadId = uint32_t;            // 0 = none
    struct PreloadOptions
    {
        bool waitTextures = false;         // textures = "wait": ready only once every texture is resident
    };
    // A preload of @p path - an existing live one of the same path (loading or
    // ready) gains a reference instead of starting another.
    PreloadId RequestPreload(const std::string& path, const PreloadOptions& opts);

    enum class PreloadState { Loading, Ready, Failed, Stale, Released };
    const char* PreloadStateName(PreloadState s);   // "loading" | "ready" | "failed" | "stale" | "released"
    PreloadState GetPreloadState(PreloadId id);
    struct PreloadProgress
    {
        uint32_t    done  = 0, total = 0;  // steps
        std::string what;                  // the stage now ("read", "prepare", "meshlib", ...)
    };
    PreloadProgress GetPreloadProgress(PreloadId id);
    std::string     GetPreloadReason(PreloadId id);  // why it failed / went stale ("" otherwise)
    // Drop one reference; the last one releases the pins - at the second Tick
    // from now (after this frame's scene build). Idempotent per id.
    void            ReleasePreload(PreloadId id);

    // A ready preload holds @p path now (the spawn census's `preloaded`).
    bool IsPreloaded(const std::string& path);

    // Once a frame, MAIN, at App's AssetTick (the render lane idle, outside the
    // device frame; after TextureSystem::Tick). @p frame: the frame's number.
    void Tick(IGraphicsDevice& gfx, MeshLibrary& meshLib, TextureSystem& texSys, ResourceManager& rm,
              uint64_t frame);
    // App shutdown, before the systems tear down: waits for the workers, then
    // releases every pin. Calls after it are no-ops.
    void Shutdown(IGraphicsDevice& gfx, MeshLibrary& meshLib, TextureSystem& texSys);

    // ==== The asynchronous spawn ====
    struct SpawnOptions
    {
        int  frames     = -1;      // >= 0: commit `frames` frames after the request's frame (0 and 1: the next one)
        bool persistent = false;   // MakePersistent at the commit (the save game's spawned recipes)
    };
    using SpawnId = uint32_t;      // 0 = none
    // @p onReady: called at the commit with the instance's root - never for a
    // request that fails or is cancelled (the Lua binding queues the script's
    // callback from it, into this frame's timers).
    SpawnId RequestSpawn(const std::string& path, float x, float y, float z, const SpawnOptions& opts,
                         std::function<void(Entity root)> onReady);
    enum class SpawnState { Loading, Ready, Failed, Cancelled };
    const char*     SpawnStateName(SpawnState s);   // "loading" | "ready" | "failed" | "cancelled"
    SpawnState      GetSpawnState(SpawnId id);
    std::string     GetSpawnReason(SpawnId id);
    PreloadProgress GetSpawnProgress(SpawnId id);
    Entity          GetSpawnEntity(SpawnId id);    // the root once committed (and alive), else NullEntity
    // Before the commit: the request is dropped (state cancelled). After it:
    // false - the entity is the caller's to destroy.
    bool            CancelSpawn(SpawnId id);
    // The commit point: MAIN, App's frame loop right before GameplayPreLogic
    // (between phases: nothing iterates the World). @p frame: the frame's number.
    void CommitSpawns(World& world, uint64_t frame);

    struct AsyncCounters
    {
        uint64_t requests = 0, commits = 0, cancelled = 0, failed = 0;
        uint64_t late     = 0;         // frames-commits whose preload was not ready (finished synchronously)
        double   lateMs   = 0.0;       // ... their synchronous time
        uint32_t pending  = 0;         // requests not committed yet (now)
    };
    const AsyncCounters& AsyncStats();

    // Running totals since boot (Lua Profiler.Spawn().preload).
    struct PreloadCounters
    {
        uint64_t requests = 0, ready = 0, failed = 0, stale = 0;
        uint64_t steps    = 0;             // main steps
        double   mainMs   = 0.0;           // main time in Tick (steps + checks)
        double   maxTickMs = 0.0;          // the dearest frame's Tick
        // The dearest frame's Tick + the texture system's budgeted share, over the frames without a
        // listed big step (the main cost per frame).
        double   maxTickMsNoBig = 0.0;
        uint64_t bigSteps = 0;             // listed big steps (the surface cache)
        std::vector<uint64_t> bigStepFrames;   // their frames (the last 16)
    };
    const PreloadCounters& Stats();
}
