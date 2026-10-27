#pragma once
// Resource/MeshLibIdentity.h - the IDENTITY layer of a .meshlib.
//
// A .meshlib entry is one material SECTION, and its meshId is its POSITION in the entry table:
// nothing in the core file names it, groups it into the author's object, or survives a
// re-import that reorders the source. This layer gives every entry a stable 64-bit SectionKey
// and a slot name, and groups entries into named Meshes (the author's objects) with a stable
// 64-bit MeshKey.
//
// KEYS ARE DERIVED ONCE - FNV-1a 64 of "<domain>|<sourceKey>", sourceKey being what the
// importer knows about the source ("n:<node path>", "pmx:<model>", "ue:<asset path>") - and
// then STORED. A re-import does not re-derive: AdoptMeshIdentityKeys matches the new meshes /
// sections to the previous file's and keeps the previous keys; only a new item gets a freshly
// derived key.
//
// A KEY IS UNIQUE WITHIN ITS MODEL, not globally: two models that both have a "RootNode/Body"
// derive the same MeshKey. Every reference is the pair (model, key); never index meshes by
// key alone.
//
// ON DISK: a tail after the index buffer. MeshLibraryMetadata::reserved is the tail's byte
// offset and MESHLIB_FLAG_HAS_TAIL says it is meaningful:
//   MeshLibTailHeader { MESHLIB_TAIL_MAGIC, version 1, chunkCount }
//   MeshLibChunkDesc  { fourcc, version, offset (from blob start), size } x chunkCount
//   chunk MESHLIB_CHUNK_IDNT v1:
//     MeshLibIdentityHeader
//     MeshLibMeshRecord    x meshCount
//     MeshLibSectionRecord x sectionCount           (== entry count, entry order)
//     uint32 sectionIndexList[sectionListCount]     (each mesh's entries, contiguous)
//     char   stringPool[stringPoolBytes]            (NUL-terminated UTF-8; offset 0 = "")
// The core readers derive their offsets from the counts in MeshLibraryMetadata and never read
// past the index buffer, and the vertex-layout helper looks only at flag bits 0-4, so the tail
// is invisible to them. Do not grow the 48-byte entry: several readers walk the table by its
// sizeof.
//
// A FILE WITHOUT A TAIL still gets an identity from SynthesizeMeshIdentity: keys from the entry
// index, meshes grouped by whatever the caller knows
// (SceneInstanceLoader::SynthesizeMeshLibIdentity uses the model's same-stem .iscn). The
// backfill tool (-assettool=meshlib_identity) writes EXACTLY that synthesized identity into
// the file, so backfilling never changes a key anything already refers to.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Resource
{
    // ---- Identity-level flags ----
    constexpr uint32_t IDNT_FLAG_BACKFILLED  = 1u << 0;  // written by the backfill tool
    constexpr uint32_t IDNT_FLAG_SYNTHESIZED = 1u << 1;  // made up in memory for a tail-less file
    // ---- Per-mesh flags ----
    constexpr uint32_t MESH_IDENT_SKINNED        = 1u << 0;
    constexpr uint32_t MESH_IDENT_HAS_MORPH      = 1u << 1;
    constexpr uint32_t MESH_IDENT_MIRROR_VARIANT = 1u << 2;  // an importer-made mirrored copy
    constexpr uint32_t MESH_IDENT_HIDDEN         = 1u << 3;  // not offered as a placeable mesh

    // An identity made from entry indices rather than by an importer: synthesized in memory for
    // a tail-less library, or that same identity written back by the backfill tool. It carries no
    // MESH_IDENT_SKINNED / HAS_MORPH.
    inline bool IsIndexDerivedIdentity(uint32_t identityFlags)
    {
        return (identityFlags & (IDNT_FLAG_SYNTHESIZED | IDNT_FLAG_BACKFILLED)) != 0;
    }

    // Does a Mesh move with a skeleton? An importer-written identity says so per Mesh, exactly:
    // a model with a skeleton may hold static Meshes (a prop under a node follows no bone), and a
    // model whose shape keys alone gave it a one-bone skeleton holds no skinned Mesh at all. An
    // index-derived identity cannot tell, so there every Mesh of a model that names a skeleton
    // (a K line) counts as skinned. The one rule behind Asset.meshes' `skinned`, the Content
    // Browser's Mesh item, the Mesh Editor and Extract Mesh.
    inline bool MeshCountsAsSkinned(uint32_t meshFlags, uint32_t identityFlags, bool modelNamesSkeleton)
    {
        return (meshFlags & MESH_IDENT_SKINNED) != 0 ||
               (modelNamesSkeleton && IsIndexDerivedIdentity(identityFlags));
    }

    // FNV-1a 64 over the bytes of `text`; never 0 (0 means "no key").
    uint64_t MeshIdentityHash(std::string_view text);
    // The spelling a .meshlib is identified by in this layer: lexically normalised, then
    // Sys::RelativizeAssetPath ('/' separators, project-relative). Also THE dedup key:
    // MeshLibrary::HashPath hashes it and the preview keys its maps on it (PreviewLibKey /
    // Renderer::PreviewAssetKey), so it must not fold case - the .ipak index is case-sensitive.
    std::string MeshLibCanonicalPath(const std::string& path);
    // 16 lowercase hex digits / its inverse (false on anything else).
    std::string MeshKeyToHex(uint64_t key);
    bool        MeshKeyFromHex(std::string_view hex, uint64_t& out);

    // ONE Mesh of a model as one string: "<model .iscn>#<MeshKey hex>" (the model
    // in MeshLibCanonicalPath's spelling). What the Content Browser's Copy Mesh
    // Reference copies and what the preview lane names a Mesh item by. Parse
    // splits on the LAST '#' and wants a `.iscn` before it and exactly 16 hex
    // digits after it; false on anything else.
    std::string MeshAssetRef(const std::string& modelPath, uint64_t meshKey);
    bool        ParseMeshAssetRef(std::string_view ref, std::string& outModel, uint64_t& outKey);

    struct MeshIdentity
    {
        struct Mesh
        {
            uint64_t              key = 0;
            std::string           name;        // the author's object name (UTF-8)
            std::string           sourceKey;   // the key's origin; a re-import matches it first
            std::vector<uint32_t> sections;    // entry ids, in the mesh's own order
            float                 aabbMin[3] = { 0.f, 0.f, 0.f };
            float                 aabbMax[3] = { 0.f, 0.f, 0.f };
            uint32_t              flags = 0;   // MESH_IDENT_*
        };
        struct Section
        {
            uint64_t    key = 0;
            std::string slotName;              // material slot name, raw (UTF-8)
            std::string sourceKey;
            uint32_t    ownerMesh = UINT32_MAX;// first mesh that lists this entry
            uint32_t    flags = 0;
        };

        bool Empty() const { return sections.empty(); }

        // Lookups. RebuildIndex() after changing keys by hand; every function in
        // this header that produces a MeshIdentity leaves the index built.
        void RebuildIndex();
        int  FindMesh(uint64_t key) const;          // -1 when absent
        int  FindSection(uint64_t key) const;       // == entry id, -1 when absent
        int  FindMeshByName(std::string_view name) const;   // first match, -1 when absent

        uint64_t             modelKey = 0;
        uint32_t             flags    = 0;     // IDNT_FLAG_*
        std::vector<Mesh>    meshes;
        std::vector<Section> sections;         // exactly one per entry, entry order

    private:
        std::unordered_map<uint64_t, uint32_t> m_meshByKey;
        std::unordered_map<uint64_t, uint32_t> m_sectionByKey;
    };

    // ---- Building ----
    // What an importer knows. sections.size() must equal the library's entry count; a mesh's
    // section list names entry ids. An entry no mesh lists gets a one-section mesh of its own
    // (named after its slot), so every entry is reachable as a Mesh.
    struct MeshIdentitySpec
    {
        struct Mesh
        {
            std::string           name;
            std::string           sourceKey;   // e.g. "n:RootNode/Body", "pmx:<model>", "ue:/Game/X/SM_Y"
            std::vector<uint32_t> sections;
            uint32_t              flags = 0;
        };
        struct Section
        {
            std::string slotName;
            std::string sourceKey;             // e.g. "<mesh sourceKey>/<slot>#<k>"
            uint32_t    flags = 0;
        };
        std::string          modelSource;      // e.g. "assimp|FionaFBX"
        std::vector<Mesh>    meshes;
        std::vector<Section> sections;
    };

    // Derive keys (a collision inside one model is resolved by suffixing the
    // source key with "#dup<n>" and logged) and compute each mesh's AABB from
    // the blob's entry table. `blob` must be a valid .meshlib (header checked).
    bool BuildMeshIdentity(const MeshIdentitySpec& spec,
                           const uint8_t* blob, size_t size,
                           MeshIdentity& out, std::string* err = nullptr);

    // A grouping hint for a tail-less file: one proposed Mesh.
    struct MeshGroupHint
    {
        std::string           name;
        std::vector<uint32_t> sections;
    };
    // The key SynthesizeMeshIdentity gives entry @p entry of a tail-less file.
    // It depends on nothing but the index, so it can be named without the
    // grouping (and without the .iscn) - e.g. by a tool that only knows a
    // (library, meshId) pair.
    uint64_t LegacySectionKey(uint32_t entry);
    // The key of entry @p entry of the file in @p blob: its IDNT key when the
    // file carries one, else LegacySectionKey(entry). 0 when entry is out of
    // range or the blob is not a .meshlib.
    uint64_t SectionKeyOfEntry(const uint8_t* blob, size_t size, uint32_t entry);

    // The identity of a tail-less file. Keys: section = hash("sec|legacy#<entry>"),
    // mesh = hash("mesh|legacy#<first entry of the group>") - positional, and NOT
    // path-qualified: keys are unique within one model and every reference is
    // (model, key), so moving the folder must not re-key it. `canonicalPath`
    // (Sys::RelativizeAssetPath of the file) only feeds the informational modelKey.
    // Groups that repeat an earlier group's section list are dropped (instancing).
    // slotNames may be shorter than the entry count; missing names become "section_<i>".
    MeshIdentity SynthesizeMeshIdentity(const std::string&                canonicalPath,
                                        const uint8_t* blob, size_t size,
                                        const std::vector<MeshGroupHint>& groups,
                                        const std::vector<std::string>&   slotNames);

    // ---- Reading / writing the file ----
    enum class MeshIdentityParse { NoTail, NoIdentity, Ok, Malformed };
    // `blob` must already have passed ValidateHeader(MAGIC_MESHLIB).
    MeshIdentityParse ParseMeshLibIdentity(const uint8_t* blob, size_t size,
                                           MeshIdentity& out, std::string* err = nullptr);

    // Replace (or add) the IDNT chunk of `blob`, keeping every other tail chunk
    // and every byte before the tail. Updates MeshLibraryMetadata (flags,
    // reserved = tail offset) and AssetHeader::dataSize. Refuses an identity
    // that does not fit the file (section count != entry count, ...).
    bool WriteMeshLibIdentity(std::vector<uint8_t>& blob, const MeshIdentity& id,
                              std::string* err = nullptr);

    // Where the geometry ends: 24 + metadataSize + entries + VB + IB, computed
    // from the counts the way every reader does. 0 when the blob is not a
    // well-formed .meshlib. Nothing before it ever changes when a tail is written.
    size_t MeshLibCoreEnd(const uint8_t* blob, size_t size);

    // Drop the tail - readable or not - and clear MESHLIB_FLAG_HAS_TAIL, leaving the file a
    // tail-less writer produces. For repairing a damaged tail (the backfill's -force); false when
    // the core is unreadable.
    bool StripMeshLibTail(std::vector<uint8_t>& blob, std::string* err = nullptr);

    // Structural check of an identity against an entry count: one section per
    // entry, every mesh section id in range, keys non-zero and unique.
    bool ValidateMeshIdentity(const MeshIdentity& id, uint32_t entryCount,
                              std::string* err = nullptr);

    // ---- Re-import ----
    struct MeshIdentityAdoption
    {
        struct MeshItem    { uint64_t key = 0; std::string name; };
        struct SectionItem { uint64_t key = 0; std::string mesh, slot; };
        struct MeshRenamed
        {
            // Name: its source key or a loose spelling of its name matched;
            // Explicit: MeshAdoptOptions::renames; Geometry: the geometry match.
            enum class How : uint8_t { Name, Explicit, Geometry };
            uint64_t    key = 0;
            std::string from, to;
            How         how = How::Name;
        };
        struct SlotRenamed { uint64_t key = 0; std::string mesh, from, to; };

        uint32_t meshesKept = 0, meshesNew = 0, meshesGone = 0;
        uint32_t sectionsKept = 0, sectionsNew = 0, sectionsGone = 0;

        // What the counts are: the report a re-import shows. Keys are the ones the items have AFTER
        // the adoption (a removed item's: the previous file's).
        bool                     previous = false;   // a previous identity was there to adopt from
        std::vector<MeshItem>    addedMeshes, removedMeshes;
        std::vector<MeshRenamed> renamedMeshes;      // kept the previous key under a new name
        // Within Meshes on both sides (a new / removed Mesh's own sections are
        // not listed - the Mesh line says it).
        std::vector<SectionItem> addedSections, removedSections;
        std::vector<SlotRenamed> renamedSlots;       // a kept section whose slot name changed
        std::vector<std::string> unresolvedRenames;  // an explicit rename naming no such Mesh

        // What someone should look at before it is written: a Mesh or a section
        // gone (references to it stop resolving), or a Mesh the GEOMETRY match
        // gave a previous key (a guess - an explicit rename is the author's word,
        // a name match is not a guess).
        bool NeedsReview() const
        {
            if (!removedMeshes.empty() || !removedSections.empty()) return true;
            for (const MeshRenamed& m : renamedMeshes)
                if (m.how == MeshRenamed::How::Geometry) return true;
            return false;
        }
    };

    // A Mesh renamed in the source keeps its identity: the fresh Mesh named `to` takes the key of
    // the previous Mesh `from` (its name, or its MeshKey as 16 hex digits).
    struct MeshRename
    {
        std::string from, to;
    };
    struct MeshAdoptOptions
    {
        std::vector<MeshRename> renames;   // matched before anything else
        // A previous Mesh no name matched and a fresh one no name matched whose
        // sections' geometry agrees (the same multiset of vertex / index counts),
        // one to one: the same Mesh, renamed in the source - it keeps its key.
        bool geometryRenames = false;
    };

    // Give `fresh` the keys of `previous` wherever an item can be matched:
    // meshes by an explicit rename (@p options), then source key, then exact
    // name, then loose name (case / separator folded), then (with
    // options->geometryRenames) by geometry one to one; sections inside a
    // matched mesh by source key, then slot name, then position when the
    // geometry agrees (same vertex and index count); then any section still
    // unmatched by source key anywhere, and - when `previous` was index-derived
    // (synthesized or backfilled) - by entry index with agreeing geometry. The
    // blobs supply the geometry comparison.
    MeshIdentityAdoption AdoptMeshIdentityKeys(const MeshIdentity& previous,
                                               const uint8_t* previousBlob, size_t previousSize,
                                               MeshIdentity& fresh,
                                               const uint8_t* freshBlob, size_t freshSize,
                                               const MeshAdoptOptions* options = nullptr);

    // ---- Section keys by path ----
    // For a reference that names a library entry by key where nothing holds the library loaded
    // (a scene override line's `srcSec=`, a collider's `meshSec=`): the section keys of the file
    // at a path. Its IDNT's keys; else - also when the tail does not parse - the positional
    // LegacySectionKey keys, exactly the synthesized identity's section keys and the ones a
    // re-import adopts.
    struct MeshLibKeyTable
    {
        std::vector<uint64_t>                  keys;       // [entry] -> SectionKey
        std::unordered_map<uint64_t, uint32_t> entryOf;    // SectionKey -> entry
        bool                                   ownIdentity = false;   // the file's IDNT (else LegacySectionKey)

        uint64_t KeyOf(uint32_t entry) const { return entry < keys.size() ? keys[entry] : 0; }
        int      Find(uint64_t key) const
        {
            const auto it = entryOf.find(key);
            return it == entryOf.end() ? -1 : static_cast<int>(it->second);
        }
    };
    // A loose file is read header and tail only (one without a tail: its first 48
    // bytes); a file only the AssetFS pak holds, whole. Cached per path, read again
    // when the loose file changes (size / write time); thread-safe. Null when the
    // file does not read as a .meshlib.
    std::shared_ptr<const MeshLibKeyTable> MeshLibKeyTableAt(const std::string& path);
}
