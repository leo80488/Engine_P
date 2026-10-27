#pragma once
// Resource/MeshletData.h - the MLET chunk of a .meshlib: the library's meshlets (clusters) and their
// LOD levels, built OFFLINE and stored in the file.
//
// Writers: every writer that changes geometry rebuilds it through Resource/MeshletBuilder.h (the
// import writer WriteModelImportFiles, Extract Mesh, the primitive bake); a file without one gets it
// from -assettool=meshlets. A library without an MLET draws through the vertex shader - the chunk is
// never built at load time.
//
// Layout (version 1): a small CPU-only table, then the GPU buffer's contents:
//   MeshletChunkHeader                          48 B
//   MeshletEntryRange  x entryCount             16 B each  (CPU)
//   MeshletLevel       x levelCount             16 B each  (CPU)
//   pad to 16 (from the chunk start)
//   --- GPU part: gpuByteOffset .. +gpuByteSize, uploaded as it is ---
//   MeshletDesc        x clusterCount           80 B each
//   uint32 vertexRefs  [vertexRefCount]         ABSOLUTE vertex indices into the library VB
//                                               (every LOD level points at the ORIGINAL vertices)
//   uint32 triangles   [triangleCount]          i0 | i1 << 8 | i2 << 16, each a cluster-local
//                                               index (< kMeshletMaxVerts)
// A cluster's vertexRefOffset / triangleOffset are DWORD indices from the start of the GPU part:
// the shader loads vertex ref i of a cluster at byte 4 * (vertexRefOffset + i).
//
// LOD: every cluster carries the sphere and error of the group that produced it (self) and of the
// group it belongs to (parent); it is drawn when projErr(parent) > tau && projErr(self) <= tau. The
// discrete LOD builder makes the group the whole entry, so an instance picks one level; a cluster
// DAG would change only the builder. A single-level build is the special case: every cluster is
// level 0, selfError 0, parentError FLT_MAX.
//
// Validated at load (ReadMeshletChunk): a chunk that fails is dropped with one warning and the
// library draws through the vertex shader - the file is never refused for it. coreFingerprint ties
// the chunk to the geometry it was built from, so a writer that changes the geometry and forgets
// the MLET is caught.
//
// Every read goes through memcpy: the chunk sits at an 8-aligned offset of a plain byte vector.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Resource
{
    constexpr uint32_t MESHLIB_CHUNK_MLET   = 'MLET';
    constexpr uint32_t MESHLIB_MLET_VERSION = 1;
    constexpr uint32_t MESHLET_CHUNK_MAGIC  = 'MLT1';

    // The cluster limits. 124 is a multiple of 4; the mesh shader runs 128 threads (the first 64 a
    // vertex each, the first 124 a triangle each). shaders/meshlet_shared.hlsli carries the same
    // values; a unit test pairs them.
    constexpr uint32_t kMeshletMaxVerts    = 64;
    constexpr uint32_t kMeshletMaxTris     = 124;
    constexpr float    kMeshletConeWeight  = 0.25f;
    constexpr uint32_t kMeshletASGroupSize = 32;    // clusters per amplification group
    constexpr uint32_t kMeshletMSGroupSize = 128;   // mesh shader threads

    // MeshletDesc::counts flags (bits 24..31).
    constexpr uint32_t MESHLET_FLAG_NO_CONE = 1u << 0;   // the cone is degenerate: never cone-culled

    // The "root" parent error: a level nothing coarser replaces.
    constexpr float kMeshletRootError = 3.402823466e+38f;   // FLT_MAX

#pragma pack(push, 1)
    struct MeshletChunkHeader
    {
        uint32_t magic;             // MESHLET_CHUNK_MAGIC
        uint32_t entryCount;        // == MeshLibraryMetadata::meshCount
        uint32_t vertexCount;       // == MeshLibraryMetadata::vertexCount
        uint32_t indexCount;        // == MeshLibraryMetadata::indexCount
        uint64_t coreFingerprint;   // MeshLibCoreFingerprint of the library it was built from
        uint32_t clusterCount;      // every entry, every level
        uint32_t vertexRefCount;
        uint32_t triangleCount;
        uint32_t levelCount;        // MeshletLevel records, every entry
        uint32_t gpuByteOffset;     // from the chunk start; 16-aligned
        uint32_t gpuByteSize;       // 80 * clusterCount + 4 * (vertexRefCount + triangleCount)
    };
    static_assert(sizeof(MeshletChunkHeader) == 48, "MeshletChunkHeader layout drift");

    // CPU: one per library entry.
    struct MeshletEntryRange
    {
        uint32_t firstCluster;      // covers every level of the entry, level 0 first
        uint32_t clusterCount;
        uint32_t firstLevel;        // into the MeshletLevel table
        uint32_t levelCount;        // 0 = the entry has no meshlets (skinned, or empty)
    };
    static_assert(sizeof(MeshletEntryRange) == 16, "MeshletEntryRange layout drift");

    // CPU: one per level of an entry. A level's clusters are contiguous.
    struct MeshletLevel
    {
        uint32_t firstCluster;      // absolute cluster index
        uint32_t clusterCount;
        float    minSelfError;      // statistics (assettool); the runtime uses the per-cluster errors
        float    maxParentError;
    };
    static_assert(sizeof(MeshletLevel) == 16, "MeshletLevel layout drift");

    // GPU: one per cluster. Mirrored by shaders/meshlet_shared.hlsli (MeshletDesc).
    struct MeshletDesc
    {
        float    cullCenter[3];     //  0  tight sphere, mesh space (frustum / Hi-Z)
        float    cullRadius;        // 12
        uint32_t coneAxisCutoff;    // 16  int8 snorm axis.xyz (bytes 0..2) + cutoff (byte 3):
                                    //     meshopt's *_s8, conservative
        uint32_t vertexRefOffset;   // 20  DWORD index from the GPU part start
        uint32_t triangleOffset;    // 24  DWORD index from the GPU part start
        uint32_t counts;            // 28  vertexCount | triCount << 8 | level << 16 | flags << 24
        float    lodSelfCenter[3];  // 32  the group that produced this cluster
        float    lodSelfRadius;     // 44
        float    lodParentCenter[3];// 48  the group this cluster belongs to
        float    lodParentRadius;   // 60
        float    lodSelfError;      // 64  mesh-space units; level 0 = 0
        float    lodParentError;    // 68  root = kMeshletRootError
        uint32_t pad[2];            // 72
    };
    static_assert(sizeof(MeshletDesc) == 80, "MeshletDesc layout drift");
#pragma pack(pop)

    inline uint32_t MeshletVertexCount(const MeshletDesc& d)   { return d.counts & 0xFFu; }
    inline uint32_t MeshletTriangleCount(const MeshletDesc& d) { return (d.counts >> 8) & 0xFFu; }
    inline uint32_t MeshletLevelOf(const MeshletDesc& d)       { return (d.counts >> 16) & 0xFFu; }
    inline uint32_t MeshletFlags(const MeshletDesc& d)         { return (d.counts >> 24) & 0xFFu; }
    inline uint32_t PackMeshletCounts(uint32_t verts, uint32_t tris, uint32_t level, uint32_t flags)
    {
        return (verts & 0xFFu) | ((tris & 0xFFu) << 8) | ((level & 0xFFu) << 16) | ((flags & 0xFFu) << 24);
    }

    // FNV-1a 64 over the library's MeshLibraryMetadata - MESHLIB_FLAG_HAS_TAIL
    // and the tail offset masked, so adding or rewriting any tail chunk leaves
    // it alone - followed by the entry table. 0 when the blob is not a .meshlib.
    uint64_t MeshLibCoreFingerprint(const uint8_t* blob, size_t size);

    // A parsed, validated MLET (pointers into the blob it was read from).
    struct MeshletChunkView
    {
        MeshletChunkHeader header{};
        const uint8_t*     chunk = nullptr;      // the chunk's first byte
        size_t             chunkSize = 0;
        const uint8_t*     gpu = nullptr;        // the GPU part (header.gpuByteSize bytes)

        MeshletEntryRange Entry(uint32_t e) const;
        MeshletLevel      Level(uint32_t l) const;
        MeshletDesc       Cluster(uint32_t c) const;
        uint32_t          VertexRef(uint32_t dwordIndex) const;   // a DWORD of the GPU part
    };

    // The MLET of the .meshlib in @p blob, checked against it: version, counts,
    // the fingerprint, every range, every cluster's limits and indices. Ok
    // fills @p out; NoChunk = the file has none; Malformed = it has one that
    // must not be drawn (@p err says why).
    enum class MeshletChunkState { NoChunk, Ok, Malformed };
    MeshletChunkState ReadMeshletChunk(const uint8_t* blob, size_t size, MeshletChunkView& out,
                                       std::string* err = nullptr);

    // ==== The shading check of a simplified level ====
    // A coarser level reuses the ORIGINAL vertices, normals included. Where a collapse stitched a
    // triangle across a hard edge - a box corner, a panel rim, the split vertices of a UV seam
    // (meshopt_simplifySloppy ignores attributes) - that triangle interpolates ANOTHER face's normal: a
    // box side shaded as its underside comes out black, and only far enough away for that level to be
    // picked. The geometric error does not see it.
    //
    // Damage = the share of a level's triangle AREA with a corner normal more than 60 deg off the
    // triangle's plane normal (level 0: exactly 0 when hard-edged, near 0 when smooth). The SHADED main
    // view stops an entry's chain before the first level scoring more than level 0 +
    // kMeshletMaxShadingDamage (MeshletDrawableLevels, run by MeshLibrary::Load; SceneMeshlet.as reads
    // the cutoff per mesh from MeshDescriptorHeap::MeshletInfo). Shadow views keep every level: a
    // depth-only caster needs its geometry within the error bound, not its normals. The files are not
    // changed - the check runs on what they hold.
    constexpr float kMeshletMaxShadingDamage = 0.01f;   // 1% of the level's area

    // @p indices name vertices of the two strided arrays (byte strides; each
    // position / normal is 3 floats, read unaligned).
    float MeshletShadingDamage(const uint8_t* positions, size_t positionStride,
                               const uint8_t* normals, size_t normalStride,
                               const uint32_t* indices, size_t indexCount);

    // For every entry of a validated @p mlet: how many of its levels, level 0
    // first, the shaded view may draw - the levels before the first that fails
    // the shading check against the library's vertex buffer (@p vertices:
    // position at byte 0, normal at byte 12 of each @p vertexStride-byte
    // vertex). Equal to the entry's levelCount when every level passes; an
    // entry without meshlets reports 0.
    std::vector<uint32_t> MeshletDrawableLevels(const MeshletChunkView& mlet, const uint8_t* vertices,
                                                size_t vertexStride);
}
