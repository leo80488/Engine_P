#pragma once
// Resource/MorphLibrary.h - reading and writing .imorph (morph target deltas).
// Read: every flavour - the single-block MAGIC_MORPH file, MAGIC_MORPHLIB dense and sparse, and
// MAGIC_MORPHLIB2 (section-keyed, with normal deltas) - as VIEWS into the file's bytes: one Block
// per section, its targets addressed through it. Nothing is copied until a caller asks.
// Write: MAGIC_MORPHLIB2, always sparse + section-keyed, normals optional.
// The layouts are in Resource/AssetHeader.h.

#include <DirectXMath.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Resource::MorphLibrary
{
    struct Block
    {
        uint32_t       meshFileIndex = 0;   // the F-line index (== the .meshlib entry id)
        uint64_t       sectionKey    = 0;   // v2: the entry's SectionKey; 0 = none
        uint32_t       vertexCount   = 0;
        uint32_t       count         = 0;   // the block's LOCAL target count
        bool           hasNormals    = false;
        const uint8_t* slots         = nullptr;   // uint32[count] (sparse); nullptr = identity (dense)
        const uint8_t* targets       = nullptr;   // count x { name[64], pos[vc] (, nrm[vc]) }

        size_t      TargetBytes() const { return 64 + size_t(vertexCount) * 12u * (hasNormals ? 2u : 1u); }
        std::string Name(uint32_t t) const;
        uint32_t    Slot(uint32_t t) const;                       // skeleton morph slot of local target t
        void        CopyPositions(uint32_t t, DirectX::XMFLOAT3* out) const;   // vertexCount float3
        bool        CopyNormals(uint32_t t, DirectX::XMFLOAT3* out) const;     // false: no normals
    };

    struct File
    {
        uint32_t           magic = 0;
        uint32_t           flags = 0;        // MORPHLIB_FLAG_* (0 for a single-block MAGIC_MORPH file)
        std::vector<Block> blocks;
        bool Keyed() const;
        // The block of the part at F-line @p meshFileIndex whose section key is
        // @p sectionKey: by KEY when both the file and the part have one (a key that
        // matches nothing is NO block - the file was written for other geometry),
        // else by meshFileIndex. nullptr when none.
        const Block* Find(uint32_t meshFileIndex, uint64_t sectionKey) const;
    };

    // Parse @p data (which must outlive @p out). False, with @p why, when it is not
    // a morph file or a block runs past the end.
    bool Read(const uint8_t* data, size_t size, File& out, std::string* why = nullptr);

    // ==== Writing ====
    struct TargetOut
    {
        std::string                    name;
        std::vector<DirectX::XMFLOAT3> pos;    // vertexCount
        std::vector<DirectX::XMFLOAT3> nrm;    // vertexCount, or empty (the file has none)
    };
    struct BlockOut
    {
        uint32_t               meshFileIndex = 0;
        uint64_t               sectionKey    = 0;   // 0 until known (RekeySections)
        uint32_t               vertexCount   = 0;
        std::vector<uint32_t>  slots;               // strictly increasing skeleton slots, one per target
        std::vector<TargetOut> targets;
    };
    // A MAGIC_MORPHLIB2 file (SPARSE | SECTION_KEYED, + HAS_NORMALS when every
    // target of every block carries normals). Empty when @p blocks is.
    std::vector<uint8_t> Build(const std::vector<BlockOut>& blocks);

    // Rewrite every block's sectionKey as @p keyOfEntry(meshFileIndex) - a v2 file
    // only (false otherwise, and for a malformed one). What the model writer does
    // once the library's keys are final (adopted from the previous import).
    bool RekeySections(std::vector<uint8_t>& blob, const std::function<uint64_t(uint32_t entry)>& keyOfEntry);

    // A morph's change of the unit normal, for a source that gives none: the
    // area-weighted smooth normal of base + delta minus that of base, over the
    // same triangles - the difference, so authored split normals stay under it.
    // Zero where nothing around a vertex moved, where its triangles collapse,
    // and where the normal would turn past 90 degrees (a fold: the vertex keeps
    // its rest normal). @p orientBy (the mesh's own normals, optional) turns each
    // vertex's pair to face the same way: a winding the importer flipped would
    // otherwise invert the delta.
    void ComputeNormalDeltas(const DirectX::XMFLOAT3* basePos, size_t vertexCount,
                             const uint32_t* indices, size_t indexCount,
                             const DirectX::XMFLOAT3* posDelta, DirectX::XMFLOAT3* outNrmDelta,
                             const DirectX::XMFLOAT3* orientBy = nullptr);
}
