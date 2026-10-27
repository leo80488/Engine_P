#pragma once

// Runtime prefab spawns. A runtime spawn is a gameplay instantiation of a .ipfb
// (Engine.SpawnPrefab, Engine.SpawnUIPrefab, a Sequencer spawnable, a save game's respawn). It
// makes exactly what Resource::LoadPrefab makes (the same call), measures itself and logs a
// census of which inputs were still cold - the part of the hitch a preload would have taken off
// the frame:
//
//   [SPAWN] path=<.ipfb> ms=<sync ms> coldMeshlib=N coldMaterials=N coldShaders=N coldTextures=N
//           reads=N skeletons=N skelRegistry=N entities=N templated=0|1 preloaded=0|1 ok=0|1
//           caller=<api>
//
//   coldMeshlib    .meshlib files this spawn read (a library already loaded is not one)
//   coldMaterials  .imat / .imatinst files it parsed (the process cache serves the rest)
//   coldShaders    custom-shader keys of its materials no material had resolved yet: each
//                  costs its first gather the registration, the .ishdr load, the DXC
//                  reflection and the //@ annotation parse
//   coldTextures   texture paths of its materials not GPU-resident yet (they stream in
//                  afterwards and are promoted on a later frame)
//   reads          files read on the spawning thread during the call
//   skeletons      skeleton registrations the spawn added to the append-only registry
//   skelRegistry   the registry's length after it
//   templated      the spawn ran inside the template (Resource/PrefabTemplate.h): 0 only
//                  under -prefab_template_off
//   preloaded      a ready preload of the prefab held it (Engine.PreloadPrefab,
//                  Resource/PrefabSpawnQueue.h): the cold items are then 0
//
// Editor placements, undo, paste and scene loads call LoadPrefab directly: they are not
// runtime spawns and write no census.
//
// Main thread only, like LoadPrefab.

#include "ECS/ECS.h"
#include "ECS/Guid.h"   // ECS::Guid (the instance seed)

#include <atomic>
#include <cstdint>
#include <string>

class Renderer;

namespace Resource
{
    class AssetManager;
    class AnimationClipSystem;

    struct SpawnCensus
    {
        std::string path;              // the .ipfb, as the caller named it
        std::string caller;            // "Engine.SpawnPrefab", ...
        uint64_t    frame = 0;         // the frame it ran in
        double      ms = 0.0;          // wall time of the synchronous spawn
        bool        ok = false;        // a root came back
        uint32_t    entities = 0;      // the instance's subtree
        uint32_t    coldMeshlib = 0;
        uint32_t    coldMaterials = 0;
        uint32_t    coldShaders = 0;
        uint32_t    coldTextures = 0;
        uint32_t    reads = 0;
        uint32_t    skeletons = 0;
        uint32_t    skelRegistry = 0;  // SkeletonRegistry::Count() after the spawn
        bool        templated = false; // LoadPrefab ran inside PrefabTemplate::Scope
        bool        preloaded = false; // a ready Engine.PreloadPrefab of it held it (PrefabSpawnQueue.h)
    };

    // Instantiates @p path as a runtime spawn: LoadPrefab (same arguments, same result) inside
    // the spawn template (Resource/PrefabTemplate.h), plus the census, logged as one [SPAWN] INFO
    // line and kept as SpawnStats::Last. @p caller names the API in the line.
    // -prefab_templateverify=N (harness): the first N are instantiated a second time without the
    // template and compared ([PREFAB_TEMPLATE_VERIFY]).
    Entity SpawnPrefab(const std::string& path, World& world, AssetManager& assetMgr,
                       Renderer* renderer, AnimationClipSystem* animClipSys,
                       const ECS::Guid* instanceSeed = nullptr,
                       const char* caller = "Engine.SpawnPrefab");

    namespace SpawnStats
    {
        // Running totals since boot, like Graphics/UploadStats.h: the harness's per-frame record
        // (frames.jsonl spawns / spawnSyncMs) and Lua Profiler.Spawn() difference two reads. Written
        // on the main thread.
        inline std::atomic<uint64_t> spawns{ 0 };
        inline std::atomic<uint64_t> syncNs{ 0 };

        // The newest runtime spawn's census; false before the first. Main thread.
        bool Last(SpawnCensus& out);
    }
}
