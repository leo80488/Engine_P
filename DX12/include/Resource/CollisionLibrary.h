#pragma once
// A model's collision library (<Model>_collision.meshlib): the point clouds of its hull prims. A
// hull entry is its points in the base 32-byte layout (position + zeroed normal / uv) with a
// trivial fan index list (DecodeMeshLibSlice refuses an entry with no indices; Jolt reads only the
// points). A `PR type=hull` record names an entry by its SectionKey, so appending never moves one;
// a file without an identity block (e.g. Tokyo_collision.meshlib) keeps the positional keys its
// records already name (LegacySectionKey).

#include <DirectXMath.h>

#include <cstdint>
#include <string>
#include <vector>

namespace Resource::CollisionLibrary
{
    using PointSet = std::vector<DirectX::XMFLOAT3>;

    // A library holding @p hulls, one entry each, in order. No identity.
    std::vector<uint8_t> BuildHullBlob(const std::vector<PointSet>& hulls);

    // Every entry's points of the .meshlib at @p path (any library: the vertex
    // positions of each entry). False when it does not read as one.
    bool ReadPoints(const std::string& path, std::vector<PointSet>& out, std::string* outError = nullptr);

    // Append @p hulls to the collision library at @p path (created when missing) and write it
    // (temp + rename). Existing entries keep their place and key; a new hull gets the key
    // "sec|hull:<owner>#<points hash>", so a hull already in the file is not appended again (its key
    // is returned). New sections form one hidden Mesh per @p ownerKey, named "<ownerName> collision".
    // @p outKeys: each hull's SectionKey, in order; @p outAppended: how many were new. An entry is
    // never removed (a V record names one by position), so a hull no record names stays in the file.
    bool AppendHulls(const std::string& path, const std::vector<PointSet>& hulls, uint64_t ownerKey,
                     const std::string& ownerName, std::vector<uint64_t>& outKeys,
                     std::string* outError = nullptr, uint32_t* outAppended = nullptr);
}
