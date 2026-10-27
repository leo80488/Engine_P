#pragma once

// PrefabTemplate - the CPU half of a prefab, kept across runtime spawns.
//
// A runtime spawn (Resource::SpawnPrefab) instantiates inside a PrefabTemplate::
// Scope. While one is open, LoadPrefab and SceneInstanceLoader::Load take what
// does not depend on the instance from process-level caches:
//   - the .ipfb's bytes and parsed node / component records, and each model's
//     (.iscn) parsed records. Every use re-checks the file's stamp (size and
//     write time of the file AssetFS reads) and re-resolves the prefab's asset
//     references, so an edit or a re-import is a miss, never a stale hit;
//   - the model's skeleton registration: one per (.iskel, its virtual bones, the
//     .iskel's stamp) shared by every runtime spawn, instead of a new ~650 KB
//     entry in the append-only registry per spawn. Editor placements, undo,
//     paste and scene loads never use it: a placed model keeps its own
//     registration (SceneInstanceLoader.h), which the editor edits in place;
//   - a skinned part whose source buffers the renderer already shares (an
//     earlier instance uploaded them and no world clear has released them since)
//     is not decoded, copied or morph-extracted: it takes the shared path with no
//     CPU streams, and its .meshlib, .iskel and .imorph are not read.
//
// Everything that writes the World, the GuidRegistry or a GPU resource runs
// unchanged, so the instance equals the one LoadPrefab makes without a Scope:
// -prefab_templateverify=N re-instantiates the first N runtime spawns without
// one and compares the two (Resource/PrefabSpawn.h). -prefab_template_off turns
// the caches off.
//
// Metadata only (bytes, parsed records, a registry index, which parts are
// skinned). No decoded vertex data is kept: the GPU holds it, and after a world
// clear releases the shared sources the next spawn decodes again.
//
// Main thread only, like LoadPrefab.

#include <cstdint>
#include <string>
#include <vector>

namespace Resource::PrefabTemplate
{
    // RAII; nestable (a depth counter).
    class Scope
    {
    public:
        Scope();
        ~Scope();
        Scope(const Scope&)            = delete;
        Scope& operator=(const Scope&) = delete;
    };

    // A Scope is open and the caches are on.
    bool Active();
    // -prefab_template_off was NOT given.
    bool Enabled();

    // What the caches validate an input against: "<size>:<write time>" of the
    // loose file AssetFS reads for @p path, "pak" for a pak entry (it cannot
    // change under a running process), "" when there is no such file.
    std::string FileStamp(const std::string& path);

    // @p path's bytes through the cache: a hit (same stamp) hands back the bytes
    // read last time without touching the file; a miss reads through AssetFS and
    // keeps them. false = the file does not read (and nothing is kept).
    bool ReadFile(const std::string& path, std::vector<uint8_t>& out);
    // Stores bytes a preload's worker read, with the stamp it took BEFORE the
    // read (a write in between makes the next ReadFile a miss, never a stale
    // hit). An empty stamp keeps nothing.
    void Put(const std::string& path, const std::string& stamp, std::vector<uint8_t> bytes);

    // Running totals since boot (Lua Profiler.Spawn().template).
    struct Counters
    {
        uint64_t fileHits = 0, fileMisses = 0;              // ReadFile + the model records
        uint64_t skeletonReuses = 0, skeletonRegistrations = 0;
        uint64_t sharedParts = 0, decodedParts = 0;         // skinned parts, by path taken
    };
    Counters& Stats();
}
