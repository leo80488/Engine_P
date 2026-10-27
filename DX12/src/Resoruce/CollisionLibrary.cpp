#include "Resource/CollisionLibrary.h"

#include "Resource/AssetFS.h"
#include "Resource/AssetHeader.h"
#include "Resource/MeshLibIdentity.h"
#include "System/PathUtil.h"

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <filesystem>
#include <unordered_set>

namespace Resource::CollisionLibrary
{
namespace
{
    constexpr uint32_t kStride = 32;   // position + zeroed normal / uv (the collision layout)

    // One entry: its points and its triangle list (0-based within the entry).
    struct Entry
    {
        PointSet              points;
        std::vector<uint32_t> indices;
    };

    // A fan over the points: never read as triangles (Jolt builds the hull from
    // the points), there so DecodeMeshLibSlice accepts the entry.
    std::vector<uint32_t> Fan(size_t n)
    {
        std::vector<uint32_t> idx;
        for (uint32_t i = 1; i + 1 < static_cast<uint32_t>(n); ++i)
        {
            idx.push_back(0);
            idx.push_back(i);
            idx.push_back(i + 1);
        }
        return idx;
    }

    std::vector<uint8_t> BuildBlob(const std::vector<Entry>& entries)
    {
        if (entries.empty()) return {};
        std::vector<uint8_t>          vbytes;
        std::vector<uint32_t>         indices;
        std::vector<MeshLibraryEntry> table;
        table.reserve(entries.size());
        uint32_t vOffset = 0;
        for (const Entry& en : entries)
        {
            MeshLibraryEntry e{};
            e.vertexStart = vOffset;
            e.vertexCount = static_cast<uint32_t>(en.points.size());
            e.indexStart  = static_cast<uint32_t>(indices.size());
            float bmin[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, bmax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
            const size_t base = vbytes.size();
            vbytes.resize(base + en.points.size() * kStride, 0u);
            for (size_t i = 0; i < en.points.size(); ++i)
            {
                const DirectX::XMFLOAT3& p = en.points[i];
                std::memcpy(vbytes.data() + base + i * kStride, &p, sizeof(p));
                bmin[0] = (std::min)(bmin[0], p.x); bmax[0] = (std::max)(bmax[0], p.x);
                bmin[1] = (std::min)(bmin[1], p.y); bmax[1] = (std::max)(bmax[1], p.y);
                bmin[2] = (std::min)(bmin[2], p.z); bmax[2] = (std::max)(bmax[2], p.z);
            }
            // Indices are absolute in the file (rebased by vertexStart).
            for (uint32_t i : en.indices) indices.push_back(vOffset + i);
            e.indexCount = static_cast<uint32_t>(indices.size()) - e.indexStart;
            for (int k = 0; k < 3; ++k) { e.aabbMin[k] = bmin[k]; e.aabbMax[k] = bmax[k]; }
            e.defaultMaterialIdx = 0;
            e.flags              = 0;
            table.push_back(e);
            vOffset += static_cast<uint32_t>(en.points.size());
        }

        const uint32_t entryBytes = static_cast<uint32_t>(table.size() * sizeof(MeshLibraryEntry));
        const uint32_t vbBytes    = static_cast<uint32_t>(vbytes.size());
        const uint32_t ibBytes    = static_cast<uint32_t>(indices.size() * sizeof(uint32_t));
        MeshLibraryMetadata meta{};
        meta.meshCount    = static_cast<uint32_t>(table.size());
        meta.vertexCount  = vOffset;
        meta.indexCount   = static_cast<uint32_t>(indices.size());
        meta.vertexStride = static_cast<uint16_t>(kStride);
        meta.indexStride  = 4;
        meta.flags        = 0;
        AssetHeader header{};
        header.magic        = MAGIC_MESHLIB;
        header.version      = CurrentVersion(MAGIC_MESHLIB);
        header.resourceType = static_cast<uint16_t>(ResourceType::MeshLibrary);
        header.metadataSize = sizeof(MeshLibraryMetadata);
        header.dataSize     = entryBytes + vbBytes + ibBytes;

        std::vector<uint8_t> blob(sizeof(AssetHeader) + sizeof(MeshLibraryMetadata) + entryBytes + vbBytes + ibBytes);
        uint8_t* dst = blob.data();
        std::memcpy(dst, &header, sizeof(header)); dst += sizeof(header);
        std::memcpy(dst, &meta, sizeof(meta));     dst += sizeof(meta);
        std::memcpy(dst, table.data(), entryBytes); dst += entryBytes;
        if (vbBytes) { std::memcpy(dst, vbytes.data(), vbBytes); dst += vbBytes; }
        if (ibBytes) std::memcpy(dst, indices.data(), ibBytes);
        return blob;
    }

    // Every entry of a .meshlib blob: positions (whatever the stride) and indices
    // (rebased to the entry).
    bool ReadEntries(const std::vector<uint8_t>& blob, std::vector<Entry>& out, std::string& why)
    {
        out.clear();
        if (!ValidateHeader(blob.data(), blob.size(), MAGIC_MESHLIB)) return why = "not a .meshlib", false;
        const auto* meta    = GetMetadata<MeshLibraryMetadata>(blob.data());
        const uint8_t* pay  = GetPayload(blob.data());
        const size_t payOff = static_cast<size_t>(pay - blob.data());
        const size_t stride = meta->vertexStride;
        const size_t tableB = size_t(meta->meshCount) * sizeof(MeshLibraryEntry);
        const size_t vbB    = size_t(meta->vertexCount) * stride;
        const size_t ibB    = size_t(meta->indexCount) * sizeof(uint32_t);
        if (stride < sizeof(DirectX::XMFLOAT3) || payOff + tableB + vbB + ibB > blob.size())
            return why = "truncated", false;
        const auto*    table = reinterpret_cast<const MeshLibraryEntry*>(pay);
        const uint8_t* vb    = pay + tableB;
        const uint8_t* ibRaw = vb + vbB;
        for (uint32_t e = 0; e < meta->meshCount; ++e)
        {
            const MeshLibraryEntry& me = table[e];
            if (size_t(me.vertexStart) + me.vertexCount > meta->vertexCount ||
                size_t(me.indexStart) + me.indexCount > meta->indexCount)
                return why = "entry " + std::to_string(e) + " is out of range", false;
            Entry en;
            en.points.resize(me.vertexCount);
            for (uint32_t i = 0; i < me.vertexCount; ++i)
                std::memcpy(&en.points[i], vb + (size_t(me.vertexStart) + i) * stride, sizeof(DirectX::XMFLOAT3));
            en.indices.resize(me.indexCount);
            for (uint32_t i = 0; i < me.indexCount; ++i)
            {
                uint32_t v = 0;
                std::memcpy(&v, ibRaw + (size_t(me.indexStart) + i) * sizeof(uint32_t), sizeof(v));
                en.indices[i] = v - me.vertexStart;
            }
            out.push_back(std::move(en));
        }
        return true;
    }

    // FNV-1a over a point set's bytes, as 16 hex digits (a hull's identity).
    std::string PointsHash(const PointSet& pts)
    {
        uint64_t h = 14695981039346656037ull;
        const auto* b = reinterpret_cast<const uint8_t*>(pts.data());
        for (size_t i = 0; i < pts.size() * sizeof(DirectX::XMFLOAT3); ++i)
        {
            h ^= b[i];
            h *= 1099511628211ull;
        }
        return MeshKeyToHex(h);
    }
}

std::vector<uint8_t> BuildHullBlob(const std::vector<PointSet>& hulls)
{
    std::vector<Entry> entries;
    entries.reserve(hulls.size());
    for (const PointSet& h : hulls) entries.push_back({ h, Fan(h.size()) });
    return BuildBlob(entries);
}

bool ReadPoints(const std::string& path, std::vector<PointSet>& out, std::string* outError)
{
    out.clear();
    std::vector<uint8_t> blob;
    std::string why;
    std::vector<Entry> entries;
    if (!Sys::ReadFileBytes(path, blob) || !ReadEntries(blob, entries, why))
    {
        if (outError) *outError = "'" + path + "': " + (why.empty() ? std::string("cannot read") : why);
        return false;
    }
    for (Entry& e : entries) out.push_back(std::move(e.points));
    return true;
}

bool AppendHulls(const std::string& path, const std::vector<PointSet>& hulls, uint64_t ownerKey,
                 const std::string& ownerName, std::vector<uint64_t>& outKeys, std::string* outError,
                 uint32_t* outAppended)
{
    auto fail = [&](std::string why)
    {
        if (outError) *outError = "'" + path + "': " + why;
        return false;
    };
    outKeys.clear();
    if (outAppended) *outAppended = 0;
    for (const PointSet& h : hulls)
        if (h.size() < 4) return fail("a hull needs at least 4 points");

    std::vector<Entry> entries;
    MeshIdentity       id;
    std::vector<uint8_t> blob;
    // The existing library wherever the loader finds it (in a worktree that reads binary assets from
    // the main checkout, only there): a raw exists() would miss it, and the write below would replace
    // it with the new hulls alone. The write still goes to this checkout's path.
    if (AssetFS::Get().ExistsExact(path))
    {
        std::string why;
        if (!AssetFS::Get().ReadFile(path, blob) || !ReadEntries(blob, entries, why))
            return fail(why.empty() ? std::string("cannot read") : why);
        // Its own identity, else the one the loader synthesizes for a collision library (no model groups
        // it: every entry its own Mesh, positional keys): the keys its PR / C / V records already name.
        if (ParseMeshLibIdentity(blob.data(), blob.size(), id, &why) != MeshIdentityParse::Ok)
            id = SynthesizeMeshIdentity(MeshLibCanonicalPath(path), blob.data(), blob.size(), {}, {});
    }

    const std::string ownerSrc = "hull:" + MeshKeyToHex(ownerKey);
    uint32_t owner = UINT32_MAX;
    for (uint32_t m = 0; m < id.meshes.size(); ++m)
        if (id.meshes[m].sourceKey == ownerSrc) { owner = m; break; }

    uint32_t appended = 0;
    std::unordered_set<uint64_t> added;   // keys appended by this call (the index is rebuilt at the end)
    for (const PointSet& h : hulls)
    {
        const std::string src = ownerSrc + "#" + PointsHash(h);
        const uint64_t    key = MeshIdentityHash("sec|" + src);
        outKeys.push_back(key);
        // These points are here already, or earlier in this call.
        if (id.FindSection(key) >= 0 || !added.insert(key).second) continue;
        if (owner == UINT32_MAX)
        {
            MeshIdentity::Mesh m;
            m.key       = MeshIdentityHash("mesh|" + ownerSrc);
            m.name      = ownerName + " collision";
            m.sourceKey = ownerSrc;
            m.flags     = MESH_IDENT_HIDDEN;
            owner = static_cast<uint32_t>(id.meshes.size());
            id.meshes.push_back(std::move(m));
        }
        const uint32_t entry = static_cast<uint32_t>(entries.size());
        entries.push_back({ h, Fan(h.size()) });
        MeshIdentity::Section s;
        s.key       = key;
        s.slotName  = "hull" + std::to_string(id.meshes[owner].sections.size());
        s.sourceKey = src;
        s.ownerMesh = owner;
        id.sections.push_back(std::move(s));
        id.meshes[owner].sections.push_back(entry);
        MeshIdentity::Mesh& m = id.meshes[owner];
        for (const DirectX::XMFLOAT3& p : h)
        {
            const float v[3] = { p.x, p.y, p.z };
            for (int k = 0; k < 3; ++k)
            {
                if (m.sections.size() == 1 && &p == &h.front()) { m.aabbMin[k] = v[k]; m.aabbMax[k] = v[k]; }
                m.aabbMin[k] = (std::min)(m.aabbMin[k], v[k]);
                m.aabbMax[k] = (std::max)(m.aabbMax[k], v[k]);
            }
        }
        ++appended;
    }
    if (outAppended) *outAppended = appended;
    if (!appended) return true;   // every hull is in the file already: nothing to write

    // A file without an identity keeps the positional keys it was synthesized with, now written
    // down, as the backfill tool does.
    if (id.flags & IDNT_FLAG_SYNTHESIZED) id.flags = (id.flags & ~IDNT_FLAG_SYNTHESIZED) | IDNT_FLAG_BACKFILLED;
    if (id.modelKey == 0) id.modelKey = MeshIdentityHash("collision|" + MeshLibCanonicalPath(path));
    blob = BuildBlob(entries);
    id.RebuildIndex();
    std::string why;
    if (!WriteMeshLibIdentity(blob, id, &why)) return fail("identity not written: " + why);
    Sys::CreateParentDirs(path);
    if (!Sys::WriteFileBytesAtomic(path, blob.data(), blob.size())) return fail("cannot write");
    return true;
}
}
