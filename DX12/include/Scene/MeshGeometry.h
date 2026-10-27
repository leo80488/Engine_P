#pragma once
// MeshGeometry: a .meshlib's geometry on the CPU, read from the file.
//
// MeshLibrary uploads a library and keeps no CPU copy, so a tool that needs the vertices reads
// the file again through AssetFS (which works on a packed build): Extract Mesh copies a Mesh's
// bytes; the Mesh Editor draws normals / tangents / UVs / bounds over its picture and counts
// what a Mesh holds. One reader and one layout (Resource::ComputeMeshLibVertexLayout) for both.
// No device, no World, any thread.

#include "Resource/AssetHeader.h"
#include "Resource/MeshLibIdentity.h"

#include <DirectXMath.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace MeshGeometry
{
    // One library file: its bytes, where its tables start, and the identity
    // MeshLibrary::Load would give it (the file's IDNT, else the synthesis).
    struct Library
    {
        std::string                   path;
        std::vector<uint8_t>          blob;
        Resource::MeshLibraryMetadata meta{};
        Resource::MeshLibVertexLayout layout{};   // from meta.flags; stride <= meta.vertexStride
        size_t                        entriesAt = 0, vbAt = 0, ibAt = 0;
        Resource::MeshIdentity        id;
        bool                          idFromFile = false;   // false: synthesized (the file has no readable IDNT tail)
    };

    // False, with @p why, for a missing, truncated or non-.meshlib file or a
    // layout this reader does not know (16-bit indices, a stride shorter than
    // the flags' attributes).
    bool ReadLibrary(const std::string& path, Library& lib, std::string& why);

    // Entry @p entry of the table (the caller checks entry < meta.meshCount).
    Resource::MeshLibraryEntry EntryAt(const Library& lib, uint32_t entry);

    // One entry's streams. Indices are LOCAL (re-based by the entry's
    // vertexStart); a triangle with an index outside the entry's own vertex
    // range is dropped and counted, never read past.
    struct Section
    {
        uint32_t                                      entry = 0;
        std::vector<DirectX::XMFLOAT3>                positions;
        std::vector<DirectX::XMFLOAT3>                normals;
        std::vector<DirectX::XMFLOAT4>                tangents;   // empty: the library has none
        std::array<std::vector<DirectX::XMFLOAT2>, 4> uv;         // channel c empty: absent
        std::vector<uint32_t>                         colors;     // RGBA8 (byte 0 = R); empty: absent
        std::vector<uint32_t>                         indices;    // local, whole triangles
        float                                         aabbMin[3] = { 0.f, 0.f, 0.f };   // the entry's stored AABB
        float                                         aabbMax[3] = { 0.f, 0.f, 0.f };
        uint32_t                                      droppedTriangles = 0;
    };
    bool DecodeSection(const Library& lib, uint32_t entry, Section& out, std::string& why);
}
