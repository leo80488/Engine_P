#ifndef NOMINMAX
#define NOMINMAX
#endif

// MeshletData.cpp - reading and checking the MLET chunk (Resource/MeshletData.h).
// Everything here runs at MeshLibrary::Load, on every file: it must refuse a chunk that would make
// the mesh shader read outside the library (a vertex reference past the vertex buffer, a triangle
// naming a vertex its cluster does not have) and must never refuse the FILE - the caller drops the
// chunk and the library draws through the vertex shader.

#include "Resource/MeshletData.h"
#include "Resource/MeshLibTail.h"
#include "Resource/AssetHeader.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace Resource
{
namespace
{
    constexpr uint64_t kFnvOffset = 14695981039346656037ull;
    constexpr uint64_t kFnvPrime  = 1099511628211ull;

    template <typename T>
    T ReadPod(const uint8_t* p)
    {
        T v;
        std::memcpy(&v, p, sizeof(T));
        return v;
    }

    uint64_t Fnv(uint64_t h, const uint8_t* p, size_t n)
    {
        for (size_t i = 0; i < n; ++i)
        {
            h ^= p[i];
            h *= kFnvPrime;
        }
        return h;
    }

    bool Finite(float v) { return std::isfinite(v); }
}

uint64_t MeshLibCoreFingerprint(const uint8_t* blob, size_t size)
{
    if (!blob || size < sizeof(AssetHeader) + sizeof(MeshLibraryMetadata)) return 0;
    const AssetHeader h = ReadPod<AssetHeader>(blob);
    if (h.magic != MAGIC_MESHLIB || h.metadataSize < sizeof(MeshLibraryMetadata)) return 0;
    MeshLibraryMetadata m = ReadPod<MeshLibraryMetadata>(blob + sizeof(AssetHeader));
    const size_t tableOff   = sizeof(AssetHeader) + h.metadataSize;
    const size_t tableBytes = size_t(m.meshCount) * sizeof(MeshLibraryEntry);
    if (tableOff + tableBytes > size) return 0;
    // The tail's own bookkeeping is not geometry: writing or rewriting a chunk
    // (this one included) must not change the fingerprint.
    m.flags   &= ~MESHLIB_FLAG_HAS_TAIL;
    m.reserved = 0;
    uint64_t f = Fnv(kFnvOffset, reinterpret_cast<const uint8_t*>(&m), sizeof(m));
    f = Fnv(f, blob + tableOff, tableBytes);
    return f ? f : 1ull;
}

MeshletEntryRange MeshletChunkView::Entry(uint32_t e) const
{
    return ReadPod<MeshletEntryRange>(chunk + sizeof(MeshletChunkHeader) + size_t(e) * sizeof(MeshletEntryRange));
}

MeshletLevel MeshletChunkView::Level(uint32_t l) const
{
    return ReadPod<MeshletLevel>(chunk + sizeof(MeshletChunkHeader) +
                                 size_t(header.entryCount) * sizeof(MeshletEntryRange) +
                                 size_t(l) * sizeof(MeshletLevel));
}

MeshletDesc MeshletChunkView::Cluster(uint32_t c) const
{
    return ReadPod<MeshletDesc>(gpu + size_t(c) * sizeof(MeshletDesc));
}

uint32_t MeshletChunkView::VertexRef(uint32_t dwordIndex) const
{
    return ReadPod<uint32_t>(gpu + size_t(dwordIndex) * 4u);
}

MeshletChunkState ReadMeshletChunk(const uint8_t* blob, size_t size, MeshletChunkView& out, std::string* err)
{
    out = MeshletChunkView{};
    auto bad = [&](const std::string& why) { if (err) *err = why; out = MeshletChunkView{}; return MeshletChunkState::Malformed; };

    MeshLibTailDirectory dir;
    std::string          terr;
    switch (ReadMeshLibTailDirectory(blob, size, dir, &terr))
    {
    case MeshLibTailState::None:      return MeshletChunkState::NoChunk;
    case MeshLibTailState::Malformed: return bad("the tail does not read (" + terr + ")");
    case MeshLibTailState::Ok:        break;
    }
    const MeshLibChunkRef* ref = dir.Find(MESHLIB_CHUNK_MLET);
    if (!ref) return MeshletChunkState::NoChunk;
    if (ref->version != MESHLIB_MLET_VERSION)
        return bad("unknown MLET version " + std::to_string(ref->version));
    if (ref->size < sizeof(MeshletChunkHeader)) return bad("MLET smaller than its header");

    const uint8_t*           c = blob + ref->offset;
    const MeshletChunkHeader h = ReadPod<MeshletChunkHeader>(c);
    if (h.magic != MESHLET_CHUNK_MAGIC) return bad("MLET magic mismatch");

    const MeshLibraryMetadata meta = ReadPod<MeshLibraryMetadata>(blob + sizeof(AssetHeader));
    if (h.entryCount != meta.meshCount || h.vertexCount != meta.vertexCount || h.indexCount != meta.indexCount)
        return bad("built for another geometry (entries/vertices/indices " + std::to_string(h.entryCount) + "/" +
                   std::to_string(h.vertexCount) + "/" + std::to_string(h.indexCount) + ", the file has " +
                   std::to_string(meta.meshCount) + "/" + std::to_string(meta.vertexCount) + "/" +
                   std::to_string(meta.indexCount) + ")");
    if (h.coreFingerprint != MeshLibCoreFingerprint(blob, size))
        return bad("built for another geometry (the fingerprint of the metadata and entry table differs)");

    // ---- The layout ----
    const uint64_t cpuEnd = uint64_t(sizeof(MeshletChunkHeader)) + uint64_t(h.entryCount) * sizeof(MeshletEntryRange) +
                            uint64_t(h.levelCount) * sizeof(MeshletLevel);
    if (h.gpuByteOffset % 16 != 0) return bad("the GPU part is not 16-aligned");
    if (cpuEnd > h.gpuByteOffset) return bad("the CPU tables overrun the GPU part");
    const uint64_t gpuSize = uint64_t(h.clusterCount) * sizeof(MeshletDesc) +
                             4ull * (uint64_t(h.vertexRefCount) + uint64_t(h.triangleCount));
    if (gpuSize != h.gpuByteSize) return bad("gpuByteSize disagrees with the counts");
    if (uint64_t(h.gpuByteOffset) + gpuSize > ref->size) return bad("the GPU part overruns the chunk");

    out.header    = h;
    out.chunk     = c;
    out.chunkSize = ref->size;
    out.gpu       = c + h.gpuByteOffset;

    // ---- Entries and levels ----
    std::vector<uint8_t> owned(h.clusterCount, 0);
    for (uint32_t e = 0; e < h.entryCount; ++e)
    {
        const MeshletEntryRange r = out.Entry(e);
        if (r.levelCount == 0)
        {
            if (r.clusterCount != 0) return bad("entry " + std::to_string(e) + " has clusters but no level");
            continue;
        }
        if (uint64_t(r.firstCluster) + r.clusterCount > h.clusterCount)
            return bad("entry " + std::to_string(e) + "'s clusters run past the table");
        if (uint64_t(r.firstLevel) + r.levelCount > h.levelCount)
            return bad("entry " + std::to_string(e) + "'s levels run past the table");
        if (r.levelCount > 255) return bad("entry " + std::to_string(e) + " has more than 255 levels");
        uint32_t next = r.firstCluster;
        for (uint32_t l = 0; l < r.levelCount; ++l)
        {
            const MeshletLevel lv = out.Level(r.firstLevel + l);
            if (lv.firstCluster != next || lv.clusterCount == 0)
                return bad("entry " + std::to_string(e) + " level " + std::to_string(l) + " is not contiguous");
            next += lv.clusterCount;
        }
        if (next != r.firstCluster + r.clusterCount)
            return bad("entry " + std::to_string(e) + "'s levels do not cover its clusters");
        for (uint32_t k = r.firstCluster; k < next; ++k)
        {
            if (owned[k]) return bad("cluster " + std::to_string(k) + " belongs to two entries");
            owned[k] = 1;
        }
    }

    // ---- Clusters ----
    const uint64_t refBegin = uint64_t(h.clusterCount) * (sizeof(MeshletDesc) / 4);   // in DWORDs
    const uint64_t refEnd   = refBegin + h.vertexRefCount;
    const uint64_t triEnd   = refEnd + h.triangleCount;
    for (uint32_t k = 0; k < h.clusterCount; ++k)
    {
        const MeshletDesc d  = out.Cluster(k);
        const uint32_t    nv = MeshletVertexCount(d);
        const uint32_t    nt = MeshletTriangleCount(d);
        auto clusterBad = [&](const char* why) { return bad("cluster " + std::to_string(k) + ": " + why); };
        if (nv == 0 || nv > kMeshletMaxVerts) return clusterBad("vertex count out of range");
        if (nt == 0 || nt > kMeshletMaxTris) return clusterBad("triangle count out of range");
        if (d.vertexRefOffset < refBegin || uint64_t(d.vertexRefOffset) + nv > refEnd)
            return clusterBad("vertex references outside the table");
        if (d.triangleOffset < refEnd || uint64_t(d.triangleOffset) + nt > triEnd)
            return clusterBad("triangles outside the table");
        if (!Finite(d.cullCenter[0]) || !Finite(d.cullCenter[1]) || !Finite(d.cullCenter[2]) ||
            !Finite(d.cullRadius) || d.cullRadius < 0.f)
            return clusterBad("a non-finite culling sphere");
        if (!Finite(d.lodSelfRadius) || !Finite(d.lodParentRadius) || d.lodSelfRadius < 0.f ||
            d.lodParentRadius < 0.f || !(d.lodSelfError >= 0.f) || !(d.lodParentError >= d.lodSelfError))
            return clusterBad("LOD values that are negative, not finite or not monotonic");
        for (uint32_t i = 0; i < nv; ++i)
            if (out.VertexRef(d.vertexRefOffset + i) >= meta.vertexCount)
                return clusterBad("a vertex reference past the vertex buffer");
        for (uint32_t t = 0; t < nt; ++t)
        {
            const uint32_t tri = out.VertexRef(d.triangleOffset + t);
            if ((tri & 0xFFu) >= nv || ((tri >> 8) & 0xFFu) >= nv || ((tri >> 16) & 0xFFu) >= nv || (tri >> 24))
                return clusterBad("a triangle naming a vertex the cluster does not have");
        }
    }
    return MeshletChunkState::Ok;
}

float MeshletShadingDamage(const uint8_t* positions, size_t positionStride, const uint8_t* normals,
                           size_t normalStride, const uint32_t* indices, size_t indexCount)
{
    // cos(60 deg): a corner normal further off its triangle's plane normal
    // shades that corner like another face.
    constexpr float kMinCornerCos = 0.5f;
    double total = 0.0, damaged = 0.0;
    for (size_t t = 0; t + 2 < indexCount; t += 3)
    {
        float p[3][3], n[3][3];
        for (int k = 0; k < 3; ++k)
        {
            std::memcpy(p[k], positions + size_t(indices[t + k]) * positionStride, sizeof(p[k]));
            std::memcpy(n[k], normals + size_t(indices[t + k]) * normalStride, sizeof(n[k]));
        }
        const float e1[3] = { p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2] };
        const float e2[3] = { p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2] };
        const double c[3] = { double(e1[1]) * e2[2] - double(e1[2]) * e2[1],
                              double(e1[2]) * e2[0] - double(e1[0]) * e2[2],
                              double(e1[0]) * e2[1] - double(e1[1]) * e2[0] };
        const double area = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);   // twice the area
        if (!(area > 0.0) || !std::isfinite(area)) continue;   // degenerate: rasterizes nothing
        total += area;
        bool bad = false;
        for (int k = 0; k < 3 && !bad; ++k)
        {
            const double len = std::sqrt(double(n[k][0]) * n[k][0] + double(n[k][1]) * n[k][1] +
                                         double(n[k][2]) * n[k][2]);
            // A zero / non-finite normal shades nothing sensible either.
            const double d = (len > 0.0 && std::isfinite(len))
                ? (c[0] * n[k][0] + c[1] * n[k][1] + c[2] * n[k][2]) / (area * len) : -1.0;
            bad = !(d >= kMinCornerCos);
        }
        if (bad) damaged += area;
    }
    return total > 0.0 ? static_cast<float>(damaged / total) : 0.f;
}

std::vector<uint32_t> MeshletDrawableLevels(const MeshletChunkView& mlet, const uint8_t* vertices, size_t vertexStride)
{
    std::vector<uint32_t> drawable(mlet.header.entryCount, 0);
    std::vector<uint32_t> tris;
    // A level's triangles as library vertex indices (the clusters' references).
    auto levelTriangles = [&](const MeshletLevel& lv)
    {
        tris.clear();
        for (uint32_t c = lv.firstCluster; c < lv.firstCluster + lv.clusterCount; ++c)
        {
            const MeshletDesc d = mlet.Cluster(c);
            for (uint32_t t = 0; t < MeshletTriangleCount(d); ++t)
            {
                const uint32_t packed = mlet.VertexRef(d.triangleOffset + t);
                for (int k = 0; k < 3; ++k)
                    tris.push_back(mlet.VertexRef(d.vertexRefOffset + ((packed >> (8 * k)) & 0xFFu)));
            }
        }
        return MeshletShadingDamage(vertices, vertexStride, vertices + 12, vertexStride, tris.data(), tris.size());
    };
    for (uint32_t e = 0; e < mlet.header.entryCount; ++e)
    {
        const MeshletEntryRange r = mlet.Entry(e);
        drawable[e] = r.levelCount;
        if (r.levelCount < 2) continue;
        const float base = levelTriangles(mlet.Level(r.firstLevel));
        for (uint32_t l = 1; l < r.levelCount; ++l)
            if (levelTriangles(mlet.Level(r.firstLevel + l)) - base > kMeshletMaxShadingDamage)
            {
                drawable[e] = l;
                break;
            }
    }
    return drawable;
}
}
