#include "Resource/MorphLibrary.h"

#include "Resource/AssetHeader.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Resource::MorphLibrary
{
std::string Block::Name(uint32_t t) const
{
    if (t >= count || !targets) return {};
    char buf[65];
    std::memcpy(buf, targets + size_t(t) * TargetBytes(), 64);
    buf[64] = '\0';
    return buf;
}

uint32_t Block::Slot(uint32_t t) const
{
    if (!slots) return t;   // dense: the local index IS the skeleton slot
    uint32_t s = 0;
    std::memcpy(&s, slots + size_t(t) * sizeof(uint32_t), sizeof(s));
    return s;
}

void Block::CopyPositions(uint32_t t, DirectX::XMFLOAT3* out) const
{
    std::memcpy(out, targets + size_t(t) * TargetBytes() + 64, size_t(vertexCount) * 12u);
}

bool Block::CopyNormals(uint32_t t, DirectX::XMFLOAT3* out) const
{
    if (!hasNormals) return false;
    std::memcpy(out, targets + size_t(t) * TargetBytes() + 64 + size_t(vertexCount) * 12u, size_t(vertexCount) * 12u);
    return true;
}

bool File::Keyed() const
{
    return magic == MAGIC_MORPHLIB2 && (flags & MORPHLIB_FLAG_SECTION_KEYED) != 0;
}

const Block* File::Find(uint32_t meshFileIndex, uint64_t sectionKey) const
{
    bool anyKey = false;
    for (const Block& b : blocks) anyKey |= b.sectionKey != 0;
    if (Keyed() && anyKey && sectionKey != 0)
    {
        for (const Block& b : blocks)
            if (b.sectionKey == sectionKey) return &b;
        return nullptr;   // keyed, and no block is this section's: the file is for other geometry
    }
    for (const Block& b : blocks)
        if (b.meshFileIndex == meshFileIndex) return &b;
    return nullptr;
}

bool Read(const uint8_t* data, size_t size, File& out, std::string* why)
{
    out = File{};
    auto fail = [&](const char* w)
    {
        if (why) *why = w;
        out.blocks.clear();
        return false;
    };
    if (!data || size < sizeof(AssetHeader)) return fail("not a morph file");
    const bool lib2 = ValidateHeader(data, size, MAGIC_MORPHLIB2);
    const bool lib  = !lib2 && ValidateHeader(data, size, MAGIC_MORPHLIB);
    const bool one  = !lib2 && !lib && ValidateHeader(data, size, MAGIC_MORPH);
    if (!lib2 && !lib && !one) return fail("not a morph file");
    const auto*    hdr = reinterpret_cast<const AssetHeader*>(data);
    const uint8_t* end = data + size;

    if (one)
    {
        // A MAGIC_MORPH file's single block: its metadata IS the block header, dense.
        if (hdr->metadataSize < sizeof(MorphTargetMetadata)) return fail("truncated");
        MorphTargetMetadata bm{};
        std::memcpy(&bm, GetMetadata<MorphTargetMetadata>(data), sizeof(bm));
        Block b;
        b.meshFileIndex = bm.meshFileIndex;
        b.vertexCount   = bm.vertexCount;
        b.count         = bm.morphTargetCount;
        b.targets       = GetPayload(data);
        if (b.targets > end || size_t(end - b.targets) < size_t(b.count) * b.TargetBytes()) return fail("truncated");
        out.magic = MAGIC_MORPH;
        out.blocks.push_back(b);
        return true;
    }

    if (hdr->metadataSize < sizeof(MorphLibraryMetadata)) return fail("truncated");
    MorphLibraryMetadata meta{};
    std::memcpy(&meta, GetMetadata<MorphLibraryMetadata>(data), sizeof(meta));
    out.magic = lib2 ? MAGIC_MORPHLIB2 : MAGIC_MORPHLIB;
    out.flags = meta.reserved[0];
    const bool sparse  = (out.flags & MORPHLIB_FLAG_SPARSE_SLOTS) != 0;
    const bool keyed   = lib2 && (out.flags & MORPHLIB_FLAG_SECTION_KEYED) != 0;
    const bool normals = lib2 && (out.flags & MORPHLIB_FLAG_HAS_NORMALS) != 0;
    const uint8_t* p = GetPayload(data);
    out.blocks.reserve(meta.entryCount);
    for (uint32_t e = 0; e < meta.entryCount; ++e)
    {
        if (p > end || size_t(end - p) < sizeof(MorphTargetMetadata)) return fail("truncated");
        MorphTargetMetadata bm{};
        std::memcpy(&bm, p, sizeof(bm));
        p += sizeof(bm);
        Block b;
        b.meshFileIndex = bm.meshFileIndex;
        b.vertexCount   = bm.vertexCount;
        b.count         = bm.morphTargetCount;
        b.hasNormals    = normals;
        if (keyed)
        {
            if (size_t(end - p) < sizeof(uint64_t)) return fail("truncated");
            std::memcpy(&b.sectionKey, p, sizeof(uint64_t));
            p += sizeof(uint64_t);
        }
        if (sparse)
        {
            if (size_t(end - p) < size_t(b.count) * sizeof(uint32_t)) return fail("truncated");
            b.slots = p;
            p += size_t(b.count) * sizeof(uint32_t);
        }
        b.targets = p;
        const size_t body = size_t(b.count) * b.TargetBytes();
        if (size_t(end - p) < body) return fail("truncated");
        p += body;
        out.blocks.push_back(b);
    }
    return true;
}

std::vector<uint8_t> Build(const std::vector<BlockOut>& blocks)
{
    if (blocks.empty()) return {};
    bool normals = true;
    size_t body = 0;
    for (const BlockOut& b : blocks)
    {
        body += sizeof(MorphTargetMetadata) + sizeof(uint64_t) + b.targets.size() * sizeof(uint32_t);
        for (const TargetOut& t : b.targets) normals &= t.nrm.size() == b.vertexCount;
    }
    for (const BlockOut& b : blocks)
        body += b.targets.size() * (64 + size_t(b.vertexCount) * 12u * (normals ? 2u : 1u));

    AssetHeader hdr{};
    hdr.magic        = MAGIC_MORPHLIB2;
    hdr.version      = CurrentVersion(MAGIC_MORPHLIB2);
    hdr.resourceType = static_cast<uint16_t>(ResourceType::Mesh);   // as every .imorph writer does
    hdr.metadataSize = sizeof(MorphLibraryMetadata);
    hdr.dataSize     = static_cast<uint32_t>(body);
    MorphLibraryMetadata meta{};
    meta.entryCount  = static_cast<uint32_t>(blocks.size());
    meta.reserved[0] = MORPHLIB_FLAG_SPARSE_SLOTS | MORPHLIB_FLAG_SECTION_KEYED | (normals ? MORPHLIB_FLAG_HAS_NORMALS : 0u);

    std::vector<uint8_t> blob(sizeof(hdr) + sizeof(meta) + body, 0);
    uint8_t* dst = blob.data();
    auto put = [&](const void* src, size_t n)
    {
        if (n) std::memcpy(dst, src, n);
        dst += n;
    };
    put(&hdr, sizeof(hdr));
    put(&meta, sizeof(meta));
    for (const BlockOut& b : blocks)
    {
        MorphTargetMetadata bm{};
        bm.morphTargetCount = static_cast<uint32_t>(b.targets.size());
        bm.vertexCount      = b.vertexCount;
        bm.meshFileIndex    = b.meshFileIndex;
        put(&bm, sizeof(bm));
        put(&b.sectionKey, sizeof(uint64_t));
        for (size_t t = 0; t < b.targets.size(); ++t)
        {
            const uint32_t slot = t < b.slots.size() ? b.slots[t] : static_cast<uint32_t>(t);
            put(&slot, sizeof(slot));
        }
        for (const TargetOut& t : b.targets)
        {
            char name[64] = {};
            std::memcpy(name, t.name.data(), (std::min)(t.name.size(), size_t(63)));
            put(name, 64);
            // A short delta run is zero-filled (the blob already is): never read past it.
            put(t.pos.data(), (std::min)(t.pos.size(), size_t(b.vertexCount)) * 12u);
            dst += (b.vertexCount - (std::min)(t.pos.size(), size_t(b.vertexCount))) * 12u;
            if (normals) put(t.nrm.data(), size_t(b.vertexCount) * 12u);
        }
    }
    return blob;
}

bool RekeySections(std::vector<uint8_t>& blob, const std::function<uint64_t(uint32_t entry)>& keyOfEntry)
{
    File f;
    if (!Read(blob.data(), blob.size(), f) || !f.Keyed()) return false;
    for (const Block& b : f.blocks)
    {
        // The key sits right before the slot table (a v2 file is always sparse).
        const uint8_t* at = (b.slots ? b.slots : b.targets) - sizeof(uint64_t);
        const uint64_t key = keyOfEntry(b.meshFileIndex);
        std::memcpy(blob.data() + (at - blob.data()), &key, sizeof(key));
    }
    return true;
}

void ComputeNormalDeltas(const DirectX::XMFLOAT3* basePos, size_t vertexCount, const uint32_t* indices,
                         size_t indexCount, const DirectX::XMFLOAT3* posDelta, DirectX::XMFLOAT3* outNrmDelta,
                         const DirectX::XMFLOAT3* orientBy)
{
    using DirectX::XMFLOAT3;
    std::vector<XMFLOAT3> nb(vertexCount, XMFLOAT3{ 0.f, 0.f, 0.f }), nm(vertexCount, XMFLOAT3{ 0.f, 0.f, 0.f });
    auto sub   = [](const XMFLOAT3& a, const XMFLOAT3& b) { return XMFLOAT3{ a.x - b.x, a.y - b.y, a.z - b.z }; };
    auto add   = [](const XMFLOAT3& a, const XMFLOAT3& b) { return XMFLOAT3{ a.x + b.x, a.y + b.y, a.z + b.z }; };
    auto cross = [](const XMFLOAT3& a, const XMFLOAT3& b)
    {
        return XMFLOAT3{ a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    };
    for (size_t i = 0; i + 2 < indexCount; i += 3)
    {
        const uint32_t a = indices[i], b = indices[i + 1], c = indices[i + 2];
        if (a >= vertexCount || b >= vertexCount || c >= vertexCount) continue;
        // Area-weighted: the unnormalised cross product.
        const XMFLOAT3 fb = cross(sub(basePos[b], basePos[a]), sub(basePos[c], basePos[a]));
        const XMFLOAT3 ma = add(basePos[a], posDelta[a]), mb = add(basePos[b], posDelta[b]),
                       mc = add(basePos[c], posDelta[c]);
        const XMFLOAT3 fm = cross(sub(mb, ma), sub(mc, ma));
        for (const uint32_t v : { a, b, c })
        {
            nb[v] = add(nb[v], fb);
            nm[v] = add(nm[v], fm);
        }
    }
    auto unit = [](const XMFLOAT3& v)
    {
        const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        return l > 1e-20f ? XMFLOAT3{ v.x / l, v.y / l, v.z / l } : XMFLOAT3{ 0.f, 0.f, 0.f };
    };
    for (size_t v = 0; v < vertexCount; ++v)
    {
        // Identical inputs give identical sums: nothing moved around v -> exactly 0.
        if (nb[v].x == nm[v].x && nb[v].y == nm[v].y && nb[v].z == nm[v].z)
        {
            outNrmDelta[v] = XMFLOAT3{ 0.f, 0.f, 0.f };
            continue;
        }
        const XMFLOAT3 b = unit(nb[v]), m = unit(nm[v]);
        // No turn to take where either side has no normal (no area around v:
        // the shape collapses its triangles), nor past 90 degrees: there the
        // surface FOLDED - the shape inverted its triangles (an eyelid squeezed
        // shut) - and blending toward a normal pointing into the head passes
        // through zero on the way. Such a vertex keeps its rest normal.
        const float bm = b.x * m.x + b.y * m.y + b.z * m.z;
        const bool  none = (b.x == 0.f && b.y == 0.f && b.z == 0.f) || (m.x == 0.f && m.y == 0.f && m.z == 0.f);
        if (none || bm < 0.f)
        {
            outNrmDelta[v] = XMFLOAT3{ 0.f, 0.f, 0.f };
            continue;
        }
        XMFLOAT3 d = sub(m, b);
        if (orientBy && b.x * orientBy[v].x + b.y * orientBy[v].y + b.z * orientBy[v].z < 0.f)
            d = XMFLOAT3{ -d.x, -d.y, -d.z };   // the triangles wind the other way than the normals point
        outNrmDelta[v] = d;
    }
}
}
