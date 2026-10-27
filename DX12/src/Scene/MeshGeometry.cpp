#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Scene/MeshGeometry.h"

#include "Resource/AssetFS.h"
#include "Scene/SceneInstanceLoader.h"   // SynthesizeMeshLibIdentity

#include <cstring>

using namespace Resource;

namespace MeshGeometry
{

bool ReadLibrary(const std::string& path, Library& lib, std::string& why)
{
    lib = Library{};
    lib.path = path;
    if (!AssetFS::Get().ReadFile(path, lib.blob)) { why = "cannot read '" + path + "'"; return false; }
    const std::vector<uint8_t>& b = lib.blob;
    if (b.size() < sizeof(AssetHeader) + sizeof(MeshLibraryMetadata)) { why = "'" + path + "' is truncated"; return false; }
    AssetHeader h{};
    std::memcpy(&h, b.data(), sizeof(h));
    if (h.magic != MAGIC_MESHLIB || h.metadataSize < sizeof(MeshLibraryMetadata))
    {
        why = "'" + path + "' is not a .meshlib";
        return false;
    }
    std::memcpy(&lib.meta, b.data() + sizeof(AssetHeader), sizeof(lib.meta));
    const MeshLibraryMetadata& m = lib.meta;
    lib.layout = ComputeMeshLibVertexLayout(m.flags);
    if (m.indexStride != 4 || m.vertexStride == 0 || lib.layout.stride > m.vertexStride)
    {
        why = "'" + path + "' has an unsupported layout";
        return false;
    }
    lib.entriesAt = sizeof(AssetHeader) + h.metadataSize;
    lib.vbAt      = lib.entriesAt + size_t(m.meshCount) * sizeof(MeshLibraryEntry);
    lib.ibAt      = lib.vbAt + size_t(m.vertexCount) * m.vertexStride;
    if (lib.ibAt + size_t(m.indexCount) * 4 > b.size()) { why = "'" + path + "' is truncated"; return false; }

    std::string perr;
    lib.idFromFile = ParseMeshLibIdentity(b.data(), b.size(), lib.id, &perr) == MeshIdentityParse::Ok;
    if (!lib.idFromFile)
        lib.id = SceneInstanceLoader::SynthesizeMeshLibIdentity(path, b.data(), b.size());
    return true;
}

MeshLibraryEntry EntryAt(const Library& lib, uint32_t entry)
{
    MeshLibraryEntry out{};
    std::memcpy(&out, lib.blob.data() + lib.entriesAt + size_t(entry) * sizeof(MeshLibraryEntry), sizeof(out));
    return out;
}

bool DecodeSection(const Library& lib, uint32_t entry, Section& out, std::string& why)
{
    out = Section{};
    out.entry = entry;
    if (entry >= lib.meta.meshCount)
    {
        why = "entry " + std::to_string(entry) + " is past the end of '" + lib.path + "'";
        return false;
    }
    const MeshLibraryEntry e = EntryAt(lib, entry);
    if (uint64_t(e.vertexStart) + e.vertexCount > lib.meta.vertexCount ||
        uint64_t(e.indexStart) + e.indexCount > lib.meta.indexCount)
    {
        why = "entry " + std::to_string(entry) + " of '" + lib.path + "' points outside its buffers";
        return false;
    }
    std::memcpy(out.aabbMin, e.aabbMin, sizeof(out.aabbMin));
    std::memcpy(out.aabbMax, e.aabbMax, sizeof(out.aabbMax));

    const MeshLibVertexLayout& L = lib.layout;
    const uint32_t stride = lib.meta.vertexStride;
    const uint8_t* vb = lib.blob.data() + lib.vbAt + size_t(e.vertexStart) * stride;
    const uint32_t n  = e.vertexCount;
    auto has = [](uint32_t off) { return off != 0xFFFFFFFFu; };

    out.positions.resize(n);
    out.normals.resize(n);
    out.uv[0].resize(n);
    if (has(L.tangentOffset)) out.tangents.resize(n);
    if (has(L.uv1Offset))     out.uv[1].resize(n);
    if (has(L.uv2Offset))     out.uv[2].resize(n);
    if (has(L.uv3Offset))     out.uv[3].resize(n);
    if (has(L.colorOffset))   out.colors.resize(n);
    for (uint32_t v = 0; v < n; ++v)
    {
        const uint8_t* p = vb + size_t(v) * stride;
        std::memcpy(&out.positions[v], p + L.posOffset,    12);
        std::memcpy(&out.normals[v],   p + L.normalOffset, 12);
        std::memcpy(&out.uv[0][v],     p + L.uv0Offset,    8);
        if (has(L.tangentOffset)) std::memcpy(&out.tangents[v], p + L.tangentOffset, 16);
        if (has(L.uv1Offset))     std::memcpy(&out.uv[1][v],    p + L.uv1Offset,     8);
        if (has(L.uv2Offset))     std::memcpy(&out.uv[2][v],    p + L.uv2Offset,     8);
        if (has(L.uv3Offset))     std::memcpy(&out.uv[3][v],    p + L.uv3Offset,     8);
        if (has(L.colorOffset))   std::memcpy(&out.colors[v],   p + L.colorOffset,   4);
    }

    // Whole triangles only: a stray index drops its triangle, it never reads
    // past the entry's vertices.
    const uint8_t* ib = lib.blob.data() + lib.ibAt + size_t(e.indexStart) * 4;
    const uint32_t tris = e.indexCount / 3;
    out.indices.reserve(size_t(tris) * 3);
    for (uint32_t t = 0; t < tris; ++t)
    {
        uint32_t idx[3];
        std::memcpy(idx, ib + size_t(t) * 12, 12);
        bool ok = true;
        for (uint32_t& i : idx)
        {
            if (i < e.vertexStart || i - e.vertexStart >= n) { ok = false; break; }
            i -= e.vertexStart;
        }
        if (!ok) { ++out.droppedTriangles; continue; }
        out.indices.insert(out.indices.end(), idx, idx + 3);
    }
    return true;
}

} // namespace MeshGeometry
