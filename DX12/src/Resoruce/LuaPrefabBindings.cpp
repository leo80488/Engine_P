#include "Resource/LuaPrefabBindings.h"
#include "Scripting/LuaDoc.h"   // bindings described as they are bound

#include "ECS/ECS.h"
#include "ECS/HierarchyComponents.h"   // LocalTransform
#include "Renderer/Renderer.h"
#include "Resource/AssetManager.h"
#include "Resource/AnimationClipSystem.h"
#include "Resource/PrefabSpawn.h"       // Resource::SpawnPrefab (a runtime spawn: LoadPrefab + its census)
#include "Resource/PrefabSpawnQueue.h"  // Engine.PreloadPrefab / SpawnPrefabAsync
#include "Scripting/ScriptOwner.h"      // onReady runs as the script that asked

#include <algorithm>
#include <memory>

#include <sol/sol.hpp>

#include "Scripting/LuaEntityId.h"      // LuaEntity + LuaId::* (needs sol first)

#include <string>

namespace
{
    // A script's hold on a preload (Resource/PrefabSpawnQueue.h): Release, or
    // the handle's collection, drops it - the preload's pins go with the last.
    struct PreloadHandle
    {
        Resource::PrefabSpawnQueue::PreloadId id = 0;
        PreloadHandle() = default;
        explicit PreloadHandle(Resource::PrefabSpawnQueue::PreloadId i) : id(i) {}
        PreloadHandle(const PreloadHandle&) = delete;
        PreloadHandle& operator=(const PreloadHandle&) = delete;
        ~PreloadHandle() { Release(); }

        std::string State() const
        { return Resource::PrefabSpawnQueue::PreloadStateName(Resource::PrefabSpawnQueue::GetPreloadState(id)); }
        sol::table Progress(sol::this_state ts) const
        {
            const auto p = Resource::PrefabSpawnQueue::GetPreloadProgress(id);
            sol::state_view lv(ts);
            sol::table t = lv.create_table();
            t["done"]  = p.done;
            t["total"] = p.total;
            t["what"]  = p.what;
            return t;
        }
        std::string Reason() const { return Resource::PrefabSpawnQueue::GetPreloadReason(id); }
        void Release()
        {
            if (id) Resource::PrefabSpawnQueue::ReleasePreload(id);
            id = 0;
        }
    };

    // A script's handle on an asynchronous spawn - not an entity id.
    struct SpawnHandle
    {
        Resource::PrefabSpawnQueue::SpawnId id = 0;
        World*                              world = nullptr;
        std::string State() const
        { return Resource::PrefabSpawnQueue::SpawnStateName(Resource::PrefabSpawnQueue::GetSpawnState(id)); }
        std::string Reason() const { return Resource::PrefabSpawnQueue::GetSpawnReason(id); }
        sol::table Progress(sol::this_state ts) const
        {
            const auto p = Resource::PrefabSpawnQueue::GetSpawnProgress(id);
            sol::state_view lv(ts);
            sol::table t = lv.create_table();
            t["done"]  = p.done;
            t["total"] = p.total;
            t["what"]  = p.what;
            return t;
        }
        LuaEntity Entity() const
        {
            const ::Entity e = Resource::PrefabSpawnQueue::GetSpawnEntity(id);
            if (e == NullEntity || !world) return {};
            return LuaId::Out(*world, e);
        }
        bool Cancel() { return Resource::PrefabSpawnQueue::CancelSpawn(id); }
    };
}

void Resource::RegisterLuaPrefabBindings(sol::state& state, World& world, AssetManager& assetMgr,
                                         Renderer& renderer, AnimationClipSystem& animClipSys)
{
    sol::table engineTbl = state["Engine"];
    ::Resource::PrefabSpawnQueue::Bind({ &world, &assetMgr, &renderer, &animClipSys });

    // The preload's handle: a usertype global so the catalog and the LuaLS types see
    // its methods.
    state.new_usertype<PreloadHandle>("PrefabPreload", sol::no_constructor,
        "State",    &PreloadHandle::State,
        "Progress", &PreloadHandle::Progress,
        "Reason",   &PreloadHandle::Reason,
        "Release",  &PreloadHandle::Release);
    LuaDoc::Describe("PrefabPreload.new", "function",
        { .doc = "Not constructible: Engine.PreloadPrefab makes a PrefabPreload." }, {}, "PrefabPreload");
    LuaDoc::Describe("PrefabPreload.State", "function",
        { .doc = "Where the preload stands: \"loading\", \"ready\" (a spawn of the prefab now costs a warm "
                 "spawn), \"failed\" (Reason says why), \"stale\" (a world clear released its GPU half: "
                 "preload again) or \"released\".",
          .params = { { .name = "self", .type = "PrefabPreload" } }, .returns = "string" },
        { "PrefabPreload" }, "string");
    LuaDoc::Describe("PrefabPreload.Progress", "function",
        { .doc = "{done, total, what}: the preload's steps so far and the stage now (read, parse, prepare, "
                 "records, meshlib, skeleton, skinnedPart, shaders, textures, surfaceCache, warm, textureWait, "
                 "ready). done never goes back; total grows as the models are prepared and the textures are "
                 "listed (one step each, done once it is no longer loading).",
          .params = { { .name = "self", .type = "PrefabPreload" } }, .returns = "table" },
        { "PrefabPreload" }, "table");
    LuaDoc::Describe("PrefabPreload.Reason", "function",
        { .doc = "Why it failed or went stale (\"\" otherwise).",
          .params = { { .name = "self", .type = "PrefabPreload" } }, .returns = "string" },
        { "PrefabPreload" }, "string");
    LuaDoc::Describe("PrefabPreload.Release", "function",
        { .doc = "Drop this handle's hold: with the last one the preload's textures and mesh libraries are "
                 "released (a collected handle does the same) - after this frame's scene build, so an entity "
                 "spawned in this frame keeps them. Idempotent.",
          .params = { { .name = "self", .type = "PrefabPreload" } }, .effect = LuaDoc::Effect::World },
        { "PrefabPreload" }, "");

    // Engine.PreloadPrefab(path [, opts]) -> PrefabPreload.
    LuaDoc::Fn(engineTbl, "Engine", "PreloadPrefab",
        [](const std::string& path, sol::optional<sol::table> opts) -> std::shared_ptr<PreloadHandle>
        {
            ::Resource::PrefabSpawnQueue::PreloadOptions o;
            if (opts)
            {
                const sol::optional<std::string> tex = (*opts)["textures"];
                o.waitTextures = tex && *tex == "wait";
            }
            return std::make_shared<PreloadHandle>(::Resource::PrefabSpawnQueue::RequestPreload(path, o));
        },
            { .doc = "Preload a .ipfb (Async_Prefab_Instantiate_Plan.md AP3): its files read, decoded and "
                     "uploaded, its skeleton registered, its materials' shaders loaded and pipelines built - on "
                     "worker threads and in small steps a frame - so a later Engine.SpawnPrefab of it costs what "
                     "a warm spawn costs and its frames load no shader and build no pipeline. opts.textures = "
                     "\"wait\" also waits for every texture to be resident; \"stream\" (default) lets them "
                     "stream in as a spawn's would. The handle holds the prefab's assets until Release (or its "
                     "collection); a world clear makes it stale.",
              .params = { { .name = "path" },
                          { .name = "opts", .doc = "{textures = \"wait\"|\"stream\"}", .optional = true } },
              .returns = "PrefabPreload", .returnsDoc = "the preload's handle",
              .effect = LuaDoc::Effect::Job, .context = LuaDoc::Context::Any });

    // The asynchronous spawn's handle.
    state.new_usertype<SpawnHandle>("PrefabSpawn", sol::no_constructor,
        "State",    &SpawnHandle::State,
        "Reason",   &SpawnHandle::Reason,
        "Progress", &SpawnHandle::Progress,
        "Entity",   &SpawnHandle::Entity,
        "Cancel",   &SpawnHandle::Cancel);
    LuaDoc::Describe("PrefabSpawn.new", "function",
        { .doc = "Not constructible: Engine.SpawnPrefabAsync makes a PrefabSpawn." }, {}, "PrefabSpawn");
    LuaDoc::Describe("PrefabSpawn.State", "function",
        { .doc = "\"loading\" (preparing; no entity yet), \"ready\" (committed: Entity() is the root), "
                 "\"failed\" (Reason says why) or \"cancelled\" (Cancel, or a world clear before the commit).",
          .params = { { .name = "self", .type = "PrefabSpawn" } }, .returns = "string" },
        { "PrefabSpawn" }, "string");
    LuaDoc::Describe("PrefabSpawn.Reason", "function",
        { .doc = "Why it failed or was cancelled (\"\" otherwise).",
          .params = { { .name = "self", .type = "PrefabSpawn" } }, .returns = "string" },
        { "PrefabSpawn" }, "string");
    LuaDoc::Describe("PrefabSpawn.Progress", "function",
        { .doc = "{done, total, what}: its preload's progress while loading (PrefabPreload:Progress), "
                 "{1, 1, state} after.",
          .params = { { .name = "self", .type = "PrefabSpawn" } }, .returns = "table" },
        { "PrefabSpawn" }, "table");
    LuaDoc::Describe("PrefabSpawn.Entity", "function",
        { .doc = "The instance's root once committed and alive; 0 before the commit (the handle is not "
                 "an entity id - never store it as one).",
          .params = { { .name = "self", .type = "PrefabSpawn" } }, .returns = "EntityId" },
        { "PrefabSpawn" }, "EntityId");
    LuaDoc::Describe("PrefabSpawn.Cancel", "function",
        { .doc = "Before the commit: drop the request (State becomes \"cancelled\", onReady is never "
                 "called) - true. After it: false; destroy the entity instead.",
          .params = { { .name = "self", .type = "PrefabSpawn" } }, .returns = "boolean",
          .effect = LuaDoc::Effect::World },
        { "PrefabSpawn" }, "boolean");

    // Engine.SpawnPrefabAsync(path, x, y, z [, opts]) -> PrefabSpawn.
    LuaDoc::Fn(engineTbl, "Engine", "SpawnPrefabAsync",
        [&world](const std::string& path, float x, float y, float z, sol::optional<sol::table> opts,
                 sol::this_state ts) -> std::shared_ptr<SpawnHandle>
        {
            ::Resource::PrefabSpawnQueue::SpawnOptions o;
            sol::protected_function onReady;
            if (opts)
            {
                if (const sol::optional<int> f = (*opts)["frames"]) o.frames = (std::max)(0, *f);
                if (const sol::optional<bool> p = (*opts)["persistent"]) o.persistent = *p;
                if (const sol::optional<sol::protected_function> cb = (*opts)["onReady"]) onReady = *cb;
            }
            sol::state_view lv(ts);
            auto handle   = std::make_shared<SpawnHandle>();
            handle->world = &world;
            lua_State* L  = lv.lua_state();
            // onReady runs as the script that asked: what it subscribes or schedules goes with
            // that script, and when it has ended by the commit onReady does not run at all.
            const Scripting::OwnerToken owner = Scripting::CurrentOwner();
            // At the commit (GameplayPreLogic), queue the script side into this
            // frame's real-time tick (ScriptTimerSystem runs later in the phase): the
            // callback, then the "prefab.spawned" event.
            auto commit = [L, onReady, &world, path, handle, owner](::Entity root)
            {
                const LuaEntity id = LuaId::Out(world, root);
                if (onReady.valid())
                    Scripting::RunAsOwner(owner, [onReady, id]()
                    {
                        const auto res = onReady(id);
                        if (!res.valid())
                        {
                            const sol::error err = res;
                            LOG_ERROR("Lua Engine.SpawnPrefabAsync onReady error: %s", err.what());
                        }
                    });
                // The handle's id is read when the callback runs: the reverse arm
                // (-prefab_async_sync) commits inside RequestSpawn, before the id is known here.
                Scripting::RunAsOwner(0, [L, id, handle, path]()
                {
                    sol::state_view w(L);
                    sol::protected_function publish = w["Engine"]["Publish"];
                    if (publish.valid())
                    {
                        sol::table ev = w.create_table();
                        ev["handle"] = handle->id;
                        ev["entity"] = id;
                        ev["path"]   = path;
                        (void)publish("prefab.spawned", ev);
                    }
                });
            };
            handle->id = ::Resource::PrefabSpawnQueue::RequestSpawn(path, x, y, z, o, commit);
            return handle;
        },
            { .doc = "Spawn a .ipfb WITHOUT the frame hitch (Async_Prefab_Instantiate_Plan.md AP4): its files, "
                     "GPU buffers, shaders and pipelines are prepared on worker threads and in small steps a frame "
                     "(Engine.PreloadPrefab), and the instance is created at the start of a later frame, at (x, y, "
                     "z). The handle is NOT an entity: State() / Entity() say when the root exists. opts.onReady = "
                     "function(root) runs in the commit frame's script update, as the script that asked - what it "
                     "subscribes or schedules goes with that script, and it does not run at all when that script "
                     "ended first (then the event \"prefab.spawned\" {handle, entity, path} is published); opts.frames = N commits exactly N frames after the "
                     "request (deterministic; it finishes synchronously there if not prepared); opts.persistent = "
                     "true makes the root persistent at the commit (Engine.MakePersistent). The instance's Guids "
                     "come from a seed minted at the request, so they do not depend on the commit frame. A world "
                     "clear cancels a pending spawn; a pending spawn is not saved. The preparation waits for the "
                     "textures too: the instance appears with them, and its next frames promote none.",
              .params = { { .name = "path" }, { .name = "x" }, { .name = "y" }, { .name = "z" },
                          { .name = "opts", .doc = "{onReady = function(root), frames = N, persistent = bool}",
                            .optional = true } },
              .returns = "PrefabSpawn", .returnsDoc = "the spawn's handle",
              .effect = LuaDoc::Effect::Job, .context = LuaDoc::Context::Any });

    // Engine.SpawnPrefab(path, x, y, z) -> entityId. Instantiates a .ipfb tree (root +
    // descendants), then re-positions the root's LocalTransform.
    LuaDoc::Fn(engineTbl, "Engine", "SpawnPrefab",
        [&world, &assetMgr, &renderer, &animClipSys](const std::string& path,
                          float x, float y, float z) -> LuaEntity
        {
            // ::Resource - inside this namespace `Resource` names the class.
            Entity root = ::Resource::SpawnPrefab(
                path, world, assetMgr, &renderer, &animClipSys, nullptr, "Engine.SpawnPrefab");
            if (root == NullEntity) return {};
            if (auto* lt = world.GetComponent<LocalTransform>(root))
                lt->translation = { x, y, z };
            return LuaId::Out(world, root);
        },
            { .doc = "Instantiate a .ipfb prefab tree (root and descendants) and place the root at (x, y, z).", .params = { { .name = "path" }, { .name = "x" }, { .name = "y" }, { .name = "z" } }, .returnsDoc = "the root", .effect = LuaDoc::Effect::World, .context = LuaDoc::Context::Update });
}
