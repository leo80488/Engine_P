#pragma once
// Resource/MeshletBuilder.h - builds the MLET chunk (Resource/MeshletData.h) of a .meshlib with
// meshoptimizer, including the discrete LOD levels.
//
// Per entry (one material section):
//   level 0   meshopt_buildMeshlets (64 vertices / 124 triangles / cone weight 0.25), each cluster
//             meshopt_optimizeMeshlet'ed, its tight sphere and normal cone from
//             meshopt_computeMeshletBounds.
//   level k   (options.lod) the ORIGINAL indices simplified to 1/2^k of the triangles by
//             meshopt_simplifyWithAttributes (normal + uv0, ErrorAbsolute); under a 15% cut it
//             retries with meshopt_simplifySloppy, and the chain stops when that cannot cut 15%
//             either or the previous level fits one cluster. Errors are monotonic
//             (e_k = max(e_k, e_k-1)). Every level indexes the ORIGINAL vertices, so the mesh
//             shader's vertex fetch is the same for all. A level that interpolates other faces'
//             normals (what sloppy does to a hard-edged mesh) is still built - the shadows draw it -
//             but the shaded view stops before it (the shading check in MeshletData.h, applied at
//             load); the build info records each level's damage.
//   LOD data  the entry is one group: every level-k cluster stores the entry's bounding sphere as
//             both LOD spheres, selfError e_k and parentError e_k+1 (last level: kMeshletRootError),
//             so one instance selects exactly one level.
//
// Deterministic: entries build independently (in parallel when options.threads != 1) and are laid
// out in entry order; the bytes never depend on the thread count.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Resource
{
    struct MeshIdentity;   // Resource/MeshLibIdentity.h

    struct MeshletBuildOptions
    {
        bool     lod     = true;    // discrete LOD levels; false = level 0 only
        uint32_t threads = 0;       // 0 = the hardware's; 1 = serial. Never changes the result.
        // Entries to build, by entry id; empty = every entry. One left out keeps
        // levelCount 0 and draws through the vertex shader (a skinned section).
        std::vector<uint8_t> entryMask;
    };

    // What one entry became (reported by the [MESHLET] build log).
    struct MeshletEntryBuildInfo
    {
        bool                  built     = false;
        uint32_t              triangles = 0;      // level 0
        float                 radius    = 0.f;    // the entry's bounding sphere, mesh space
        std::vector<uint32_t> levelTriangles;     // [level]
        std::vector<float>    levelErrors;        // [level] selfError (mesh space)
        // [level] the shading check's damage over level 0's (MeshletData.h):
        // the shaded view stops before the first level above
        // kMeshletMaxShadingDamage; the shadows draw them all.
        std::vector<float>    levelShadingDamage;
        uint32_t              sloppyLevels = 0;   // levels meshopt_simplifySloppy made
        std::string           stop;               // why the level chain ended ("" = single level)
    };

    struct MeshletBuildStats
    {
        uint32_t entries        = 0;    // in the library
        uint32_t entriesBuilt   = 0;    // with meshlets
        uint32_t clusters       = 0;    // every level
        uint32_t level0Clusters = 0;
        uint64_t triangles      = 0;    // every level
        uint64_t level0Triangles = 0;
        uint32_t vertexRefs     = 0;
        uint32_t levelRecords   = 0;    // MeshletLevel records
        uint32_t maxLevels      = 0;
        uint32_t sloppyLevels   = 0;
        size_t   chunkBytes     = 0;
        size_t   gpuBytes       = 0;
        double   ms             = 0.0;
        std::vector<MeshletEntryBuildInfo> perEntry;
    };

    // The MLET chunk's bytes for the .meshlib in @p blob. False (and @p err)
    // only when the library itself does not read; an entry that cannot be
    // clustered (no triangles, an index past the vertex buffer) just gets no
    // meshlets. A library with no entry to build yields an EMPTY @p chunk.
    bool BuildMeshletChunk(const uint8_t* blob, size_t size, const MeshletBuildOptions& options,
                           std::vector<uint8_t>& chunk, MeshletBuildStats* stats = nullptr,
                           std::string* err = nullptr);

    // Build and store it in @p blob's tail, replacing any MLET (every other
    // chunk is kept). An empty build REMOVES the MLET: a library with nothing
    // to draw through meshlets carries none.
    bool AddMeshletsToMeshLib(std::vector<uint8_t>& blob, const MeshletBuildOptions& options,
                              MeshletBuildStats* stats = nullptr, std::string* err = nullptr);

    // The entries that draw static - what a build should cover. An importer's identity says which
    // Mesh is skinned (MESH_IDENT_SKINNED); an index-derived identity of a model that names a skeleton
    // counts every Mesh skinned (Resource::MeshCountsAsSkinned). An entry is in when any static Mesh
    // lists it, or when no Mesh lists it.
    std::vector<uint8_t> StaticEntryMask(const MeshIdentity& id, uint32_t entryCount, bool modelNamesSkeleton);

    // The `[MESHLET] build` log line for @p path; with @p perEntry also one line per built entry
    // (triangles, radius, levels).
    void LogMeshletBuild(const char* who, const std::string& path, const MeshletBuildStats& st, bool perEntry);
}
