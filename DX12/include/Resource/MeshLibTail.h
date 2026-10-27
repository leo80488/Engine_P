#pragma once
// Resource/MeshLibTail.h - the generic TAIL of a .meshlib: a chunk directory after the index
// buffer (Resource/AssetHeader.h, MESHLIB_FLAG_HAS_TAIL).
//
//   MeshLibTailHeader { MESHLIB_TAIL_MAGIC, version 1, chunkCount }
//   MeshLibChunkDesc  { fourcc, version, offset (from blob start), size } x chunkCount
//   chunk payloads, each 8-aligned
//
// Every per-library extension is a chunk of its own: IDNT (the identity,
// Resource/MeshLibIdentity.h), MLET (the meshlets, Resource/MeshletData.h). A reader asks for
// the ONE chunk it needs and nothing here copies a chunk it was not asked for: the directory
// is parsed in place, and a loose file is read header, directory and that chunk only - a
// multi-megabyte MLET costs a key-table lookup (MeshLibKeyTableAt) nothing.
//
// Never bump the tail version (MESHLIB_TAIL_VERSION): every reader refuses an unknown tail
// version, so a bump would hide every chunk - the identity included - from all of them. A
// chunk carries its own version, and readers skip chunks they do not know.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Resource
{
    // One chunk as the directory names it.
    struct MeshLibChunkRef
    {
        uint32_t fourcc  = 0;
        uint32_t version = 0;
        uint32_t offset  = 0;   // from the BLOB start
        uint32_t size    = 0;   // bytes
    };

    enum class MeshLibTailState { None, Ok, Malformed };

    struct MeshLibTailDirectory
    {
        size_t                       tailOffset = 0;   // 0 without a tail
        std::vector<MeshLibChunkRef> chunks;           // file order

        // The first chunk with @p fourcc; null when the tail has none.
        const MeshLibChunkRef* Find(uint32_t fourcc) const
        {
            for (const MeshLibChunkRef& c : chunks)
                if (c.fourcc == fourcc) return &c;
            return nullptr;
        }
    };

    // The directory of the tail of the WHOLE .meshlib in @p blob: the tail header and the chunk
    // descriptors, every chunk checked to lie inside the file. No chunk byte is read. None = the
    // file has no tail; Malformed = it says it has one and the bytes disagree (@p err says how).
    MeshLibTailState ReadMeshLibTailDirectory(const uint8_t* blob, size_t size, MeshLibTailDirectory& out,
                                              std::string* err = nullptr);

    // Replace chunk @p fourcc - or append it - keeping every other chunk byte
    // for byte and in order (IDNT, when present, stays first). Rewrites the
    // flags, the tail offset and AssetHeader::dataSize; the geometry before
    // the tail is never touched. All-or-nothing; refuses a damaged tail.
    bool WriteMeshLibChunk(std::vector<uint8_t>& blob, uint32_t fourcc, uint32_t version,
                           const uint8_t* bytes, size_t size, std::string* err = nullptr);

    // Drop every chunk @p fourcc. With no chunk left the tail goes too (and
    // MESHLIB_FLAG_HAS_TAIL), leaving the file a tail-less writer makes. False only for an
    // unreadable core or a damaged tail; nothing to drop is success.
    bool RemoveMeshLibChunk(std::vector<uint8_t>& blob, uint32_t fourcc, std::string* err = nullptr);

    // One chunk of the file at @p path WITHOUT its geometry: a loose file is
    // read header, directory and that chunk only; a file only the AssetFS pak
    // holds, whole.
    enum class MeshLibChunkRead { NotMeshLib, NoChunk, Ok, Malformed };
    MeshLibChunkRead ReadMeshLibChunkAt(const std::string& path, uint32_t fourcc, std::vector<uint8_t>& bytes,
                                        uint32_t& version, std::string* err = nullptr);
}
