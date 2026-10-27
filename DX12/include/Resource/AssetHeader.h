#pragma once

#include "Resource/ResourceHandle.h"
#include "System/Check.h"
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

// Every resource blob on disk starts with a fixed AssetHeader, then a type-specific
// metadata struct, then the raw payload:
//   [AssetHeader (24 bytes)] [XxxMetadata (metadataSize)] [payload (dataSize)]
// The resource layer reads the blob, validates the magic and exposes the metadata;
// the render layer hands GetPayload() bytes plus the metadata to the device.
namespace Resource
{
    // Magic numbers (4-CC style, readable in hex dumps)
    constexpr uint32_t MAGIC_TEXTURE  = 'RTXT'; // 0x52545854
    constexpr uint32_t MAGIC_MESH     = 'RMSH'; // 0x524D5348
    constexpr uint32_t MAGIC_SHADER   = 'RSDR'; // 0x52534452
    constexpr uint32_t MAGIC_MATERIAL = 'RMAT'; // 0x524D4154
    constexpr uint32_t MAGIC_SCENE    = 'RSCN'; // 0x5253434E
    constexpr uint32_t MAGIC_PREFAB   = 'RPFB'; // 0x52504642
    constexpr uint32_t MAGIC_SKELETON   = 'RSKL'; // 0x52534B4C
    constexpr uint32_t MAGIC_ANIMATION  = 'RANI'; // 0x52414E49
    constexpr uint32_t MAGIC_MORPH      = 'RMRF'; // 0x524D5246
    constexpr uint32_t MAGIC_MORPHLIB   = 'RMRL'; // combined .imorph: every submesh's morph block in one file
    constexpr uint32_t MAGIC_MORPHLIB2  = 'RMR2'; // MAGIC_MORPHLIB with sectionKey / normals, see below
    constexpr uint32_t MAGIC_WORLD      = 'RWLD'; // 0x52574C44
    constexpr uint32_t MAGIC_MESHLIB    = 'RMLB'; // 0x524D4C42 - .meshlib mesh library
    constexpr uint32_t MAGIC_AUDIO      = 'RACL'; // 0x5241434C - .aclip audio sample
    constexpr uint32_t MAGIC_PPPROFILE  = 'RPPP'; // 0x52505050 - .ppprofile post-process profile

    // ==== Format versions ====
    // Every magic has its own version line: `current` is what its writers stamp, `minimum` the
    // oldest file its reader still lifts to current (a registered migration chain, one step per
    // version: Resource/AssetMigration.h). A reader accepts [minimum, current]; a file above
    // current is refused with a reason, never read as if it were current. Bumping one format
    // touches that format only: add a row's `current`, add its chain step, keep a fixture of
    // the previous version (dx12-engine-guide references/resource-pipeline.md).
    // A magic with no row (a module's own container, e.g. MAGIC_NAVMESH) reads and writes
    // kUnlistedVersion only.
    struct AssetFormat
    {
        uint32_t    magic;
        const char* kind;     // what tools print: "material", "meshlib", ...
        uint16_t    current;
        uint16_t    minimum;
    };

    constexpr uint16_t kUnlistedVersion = 1;

    inline constexpr AssetFormat kAssetFormats[] = {
        { MAGIC_TEXTURE,   "texture",   1, 1 },
        { MAGIC_MESH,      "mesh",      1, 1 },
        { MAGIC_SHADER,    "shader",    1, 1 },
        { MAGIC_MATERIAL,  "material",  2, 1 },
        { MAGIC_SCENE,     "scene",     1, 1 },
        { MAGIC_PREFAB,    "prefab",    1, 1 },
        { MAGIC_SKELETON,  "skeleton",  1, 1 },
        { MAGIC_ANIMATION, "animation", 1, 1 },
        { MAGIC_MORPH,     "morph",     1, 1 },
        { MAGIC_MORPHLIB,  "morphlib",  1, 1 },
        { MAGIC_MORPHLIB2, "morphlib2", 1, 1 },
        { MAGIC_WORLD,     "world",     1, 1 },
        { MAGIC_MESHLIB,   "meshlib",   1, 1 },
        { MAGIC_AUDIO,     "audio",     1, 1 },
        { MAGIC_PPPROFILE, "ppprofile", 1, 1 },
    };

    // The row for @p magic, or nullptr when it has none.
    constexpr const AssetFormat* FindAssetFormat(uint32_t magic)
    {
        for (const AssetFormat& f : kAssetFormats)
            if (f.magic == magic)
                return &f;
        return nullptr;
    }

    constexpr uint16_t CurrentVersion(uint32_t magic)
    {
        const AssetFormat* f = FindAssetFormat(magic);
        return f ? f->current : kUnlistedVersion;
    }

    constexpr uint16_t MinimumVersion(uint32_t magic)
    {
        const AssetFormat* f = FindAssetFormat(magic);
        return f ? f->minimum : kUnlistedVersion;
    }

    // How this engine treats a file of format @p f at @p version: "current"; "upgradable" (read
    // through the migration chain); "newer" and "older_than_minimum" (refused).
    constexpr const char* VersionStatus(const AssetFormat& f, uint16_t version)
    {
        return version > f.current ? "newer" : version < f.minimum ? "older_than_minimum"
             : version < f.current ? "upgradable" : "current";
    }

    constexpr bool AssetFormatsAreSound()
    {
        for (const AssetFormat& f : kAssetFormats)
        {
            if (f.minimum < 1 || f.minimum > f.current)
                return false;
            if (FindAssetFormat(f.magic) != &f)   // a magic listed twice
                return false;
        }
        return true;
    }
    static_assert(AssetFormatsAreSound(), "kAssetFormats: minimum in 1..current, one row per magic");

#pragma pack(push, 1)
    // Fixed 24-byte header at offset 0 of every asset blob.
    struct AssetHeader
    {
        uint32_t magic;         // identifies resource type (MAGIC_TEXTURE etc.)
        uint16_t version;       // this magic's format version (kAssetFormats)
        uint16_t resourceType;  // ResourceType enum value
        uint32_t metadataSize;  // bytes occupied by the Metadata struct that follows
        uint32_t dataSize;      // bytes of raw payload that follows the Metadata
        uint32_t flags;         // reserved (compression / encryption flags)
        uint32_t reserved;      // padding to 24 bytes
    };
    static_assert(sizeof(AssetHeader) == 24, "AssetHeader size changed");

    // ==== Per-type metadata (stored right after AssetHeader) ====

    // Not trusted: read the DDS payload instead (Resource::DescribeTextureFile,
    // Resource/TextureInfo.h). TextureLoader never reads this struct, and `dimension` is
    // written three ways on disk: 3 (DirectXTex's TEX_DIMENSION_TEXTURE2D, cubes
    // included), 6 (the probe bake's "Cube") and 2 (some Python tools).
    struct TextureMetadata
    {
        uint32_t width;
        uint32_t height;
        uint32_t depth;        // 1 for 2-D, >1 for 3-D / array
        uint16_t mipLevels;
        uint16_t format;       // maps to DXGI_FORMAT (cast on the Render Layer)
        uint8_t  dimension;    // unreliable, see above (nominally 1=1D 2=2D 3=3D 6=Cube)
        uint8_t  padding[3];
    };

    struct MeshMetadata
    {
        uint32_t vertexCount;
        uint32_t indexCount;
        uint32_t vertexStride; // bytes per vertex
        uint32_t indexStride;  // 2 (uint16) or 4 (uint32)
        uint32_t subMeshCount; // SubMeshRecord entries after the index buffer;
                               // 0 or 1 = one submesh over the whole index buffer
        uint32_t padding;
    };

    // One sub-range of a merged mesh. The importer groups every aiMesh primitive of one
    // source scene node into one .imsh file and records their per-material index ranges
    // here; the runtime emits one MeshDescriptor slot per entry (all on the shared
    // VB / IB, differing only by indexByteOffset / vertexCount).
    //
    // Layout of a .imsh payload:
    //   [vertexCount x vertexStride bytes] vertices
    //   [indexCount  x indexStride  bytes] indices
    //   [subMeshCount x sizeof(SubMeshRecord)] submesh table
    struct SubMeshRecord
    {
        uint32_t indexStart;    // first index (into shared IB) for this submesh
        uint32_t indexCount;    // number of indices
        uint32_t materialIndex; // per-node index into the scene's F-line material list
        uint32_t _pad;
    };
    static_assert(sizeof(SubMeshRecord) == 16, "SubMeshRecord must be 16 bytes");

    struct ShaderMetadata
    {
        uint32_t bytecodeSize;
        uint8_t  stage;          // maps to RHI::ShaderStage
        uint8_t  shaderModel;    // maps to RHI::ShaderModel
        uint8_t  padding[2];
        char     entryPoint[64];
    };

    // Payload: raw key=value text of the .mat source file (null-terminated).
    struct MaterialMetadata
    {
        uint32_t textLength;     // byte count of the text payload (excluding null terminator)
        uint32_t reserved[3];
    };

    // .ppprofile - post-process profile asset (a shared look that volumes reference).
    // Payload: line-based "group_member=value" text (NUL-terminated); only overridden
    // properties are written - a key's presence encodes overrideState.
    struct PostProcessProfileMetadata
    {
        uint32_t textLength;     // byte count of the text payload (excluding null terminator)
        uint32_t reserved[3];
    };

    // .ipfb - prefab: a serialized ECS entity tree (hierarchy, transforms, materials,
    // mesh refs). Payload: line-based text (NUL-terminated); 'N' lines = nodes,
    // 'M' lines = materials.
    struct PrefabMetadata
    {
        uint32_t nodeCount;   // total node entries
        uint32_t textLength;  // byte count of text payload (excluding null terminator)
        uint32_t reserved[2];
    };

    // .iskel - skeleton asset: bone hierarchy, bind poses, animation clips.
    // Payload: a raw binary blob (SkeletonAsset POD + clip SOA data).
    //
    // With ISKEL_FLAG_EXTENDED in flags (reserved[1]) the payload has extended PMX data
    // after the standard blend data: grantSource[boneCount], grantRatio[boneCount],
    // transformOrder[boneCount], ikChainCount, then each IK chain's data.
    //
    // With ISKEL_FLAG_HUMANOID a HumanoidProfile POD (sizeof(HumanoidProfile), copied
    // verbatim) is appended at the very end of the payload, after the extended block if
    // any; a reader that stops earlier ignores it. See HumanoidBone.h.
    struct SkeletonMetadata
    {
        uint32_t boneCount;     // number of bones in the skeleton
        uint32_t clipCount;     // number of embedded ClipAssets
        uint32_t reserved[2];   // [0] = meshBlendCount, [1] = flags
    };
    constexpr uint32_t ISKEL_FLAG_EXTENDED = 0x01; // has grant/IK/transformOrder data
    constexpr uint32_t ISKEL_FLAG_HUMANOID = 0x02; // has a trailing HumanoidProfile (retarget role map)
    // boneRestAABBs are computed and stored in bind-model space - the space the
    // per-frame skin matrices (inverseBindPose x localToModel) map from, which the
    // renderer's bone-AABB merge transforms them with. A file without this flag carries
    // the section bytes but unusable values (unprocessed +-1e30 seeds, or bone-local
    // boxes the skin-matrix transform maps to garbage near the model origin), so the
    // loader ignores the section unless the flag is set.
    constexpr uint32_t ISKEL_FLAG_BONE_AABB_MODEL = 0x04;

    // .ianim - generic animation container (named channels, not bound to a skeleton):
    // any mix of bone / morph / camera tracks, so one file and loader serve VMD,
    // FBX / glTF and others. Clip layouts: AnimationImporter / AnimationSerializer.
    // Payload order:
    //   [bone clips x clipCount] [morph clips x morphClipCount]
    //   [camera clips x cameraClipCount]
    //   [notify section]           (ANIM_FLAG_HAS_NOTIFIES)
    //   [root motion / curves]     (ANIM_FLAG_HAS_TRACKS)
    //   [IK enable tracks]         (ANIM_FLAG_IK_ENABLE)
    //
    // cameraClipCount is 0 in a file without camera clips (the slot was always-zero
    // reserved space), so such a file reads zero camera clips and goes on to the tails -
    // no version bump. Each tail is appended last, so a reader that stops earlier
    // ignores it, and a file with the flag clear has none. Notify section
    // (AnimationSerializer.cpp):
    //   [uint32 notifyClipCount]                            (== clipCount)
    //   per clip: [uint32 jsonLen][jsonLen bytes]           (NotifyIO JSON string)
    struct AnimationMetadata
    {
        uint32_t clipCount;       // number of bone AnimClipData entries
        uint32_t morphClipCount;  // number of MorphClipData entries (0 = none)
        uint32_t flags;           // see ANIM_FLAG_* below
        uint32_t cameraClipCount; // number of CameraAnimClipData entries (0 = none)
    };
    constexpr uint32_t ANIM_FLAG_HAS_TRACKS = 0x10; // optional per-clip root motion / float curve JSON tail
    constexpr uint32_t ANIM_FLAG_POS_OFFSETS  = 0x1; // VMD: positions additive to rest pose
    constexpr uint32_t ANIM_FLAG_ROT_OFFSETS  = 0x2; // VMD: rotations compose with rest pose
    constexpr uint32_t ANIM_FLAG_HAS_NOTIFIES = 0x4; // tail notify section present
    constexpr uint32_t ANIM_FLAG_IK_ENABLE = 0x20; // IK enable-track tail (after the notify and track tails)
    constexpr uint32_t ANIM_FLAG_LEG_FK       = 0x8; // retargeted clip: legs baked FK, skip MMD leg IK
                                                     // (BindToSkeleton -> ClipAsset::legDriveMode = FK)

    // .imorph - morph target deltas (vertex blend shapes). Written only as the
    // combined MAGIC_MORPHLIB file below (one per model, one block per section with
    // moving targets); this struct is that file's per-block header. A single-block
    // MAGIC_MORPH file (read, never written) uses it as its whole metadata. Within a
    // block the deltas are dense over the section's vertices:
    // morphTargetCount x { name[64], float3 deltas[vertexCount] }.
    struct MorphTargetMetadata
    {
        uint32_t morphTargetCount; // targets in this block. Dense flavour: a prefix of
                                   // the skeleton-wide list (target i = skeleton morph i).
                                   // Sparse (MORPHLIB_FLAG_SPARSE_SLOTS): the local count.
        uint32_t vertexCount;      // vertices per mesh (each target has this many deltas);
                                   // MUST equal the section's vertex count - the loader
                                   // refuses a block that disagrees (a stale .imorph)
        uint32_t meshFileIndex;    // index into the .iscn F-lines (== meshlib entry id for
                                   // every importer that writes this file)
        uint32_t reserved;
    };

    // Combined .imorph (magic MAGIC_MORPHLIB): one file holds every submesh's morph
    // block, so the loader reads it once instead of one small MAGIC_MORPH file per
    // submesh. A distinct magic: a reader that predates it refuses the file whole.
    // metadataSize = sizeof(MorphLibraryMetadata); the payload is entryCount blocks,
    // each naming its submesh by meshFileIndex.
    //
    // ---- Two block flavours, selected by MorphLibraryMetadata::reserved[0] ----
    //
    // DENSE (flag clear):
    //   MorphTargetMetadata{ morphTargetCount = a prefix of the skeleton-wide
    //                        morph list, vertexCount, meshFileIndex }
    //   morphTargetCount x { char name[64] + vertexCount x float3 }
    //   Block index i is skeleton morph index i. The cost: a mesh with 100 of a rig's
    //   700 shared shapes pays for all ~700, 93% of them zeros, on disk and again in
    //   the GPU morph-delta buffers.
    //
    // SPARSE (MORPHLIB_FLAG_SPARSE_SLOTS set):
    //   MorphTargetMetadata{ morphTargetCount = k, the mesh's local target count,
    //                        vertexCount, meshFileIndex }
    //   uint32 skeletonSlot[k]     - strictly increasing, each < the .iskel's
    //                                morphTargetCount; local target l is skeleton
    //                                morph index skeletonSlot[l].
    //   k x { char name[64] + vertexCount x float3 }
    //   Only targets with a non-zero delta on this mesh are written; the names keep a
    //   block self-describing without the .iskel.
    //
    // The reader synthesises the identity slot table for a dense block, so everything
    // after SceneInstanceLoader has one code path: MeshSkinnedComponent carries
    // local -> skeleton and skeleton -> local tables and BuildSkinJobs scatters weights
    // through them. The GPU layout is the same for both: Skin.cs reads
    // g_MorphDeltas[m * vertexCount + v] with m local to the mesh.
    constexpr uint32_t MORPHLIB_FLAG_SPARSE_SLOTS = 1u << 0;

    // ---- MAGIC_MORPHLIB2: the same container with two more block properties ----
    // A distinct magic again: a MAGIC_MORPHLIB reader does not reject unknown
    // reserved[0] bits, so it would walk the new blocks with the old sizes and upload
    // garbage; with a new magic it refuses the file (no morphs, nothing worse).
    //   SECTION_KEYED: after each MorphTargetMetadata, `uint64 sectionKey` - the
    //     SectionKey of the .meshlib entry the block deforms. The loader finds a part's
    //     block by its key (the F-line's sec=) and still checks vertexCount;
    //     0 = no key (then meshFileIndex).
    //   HAS_NORMALS: each target is { name[64], float3 pos[vertexCount],
    //     float3 nrm[vertexCount] } - nrm = the morph's change of the unit normal
    //     (Skin.cs adds w * nrm and renormalises; the tangent follows).
    // Resource/MorphLibrary.h reads every flavour and writes this one.
    constexpr uint32_t MORPHLIB_FLAG_SECTION_KEYED = 1u << 1;
    constexpr uint32_t MORPHLIB_FLAG_HAS_NORMALS   = 1u << 2;

    struct MorphLibraryMetadata
    {
        uint32_t entryCount;       // number of submesh morph blocks in this file
        uint32_t reserved[3];      // reserved[0] = MORPHLIB_FLAG_* bits (0 = dense)
    };

    // .iscn - scene hierarchy container. Payload: line-based text (NUL-terminated)
    // describing nodes and mesh file references, in SceneImporter's format.
    struct SceneMetadata
    {
        uint32_t nodeCount;       // total node entries in payload
        uint32_t meshFileCount;   // total mesh file (F) entries in payload
        uint32_t textLength;      // byte count of text payload (excluding null terminator)
        uint32_t reserved;
    };

    // .meshlib - mesh-library container: each mesh is a first-class entry with a
    // meshId, so scene-graph nodes reference it unambiguously and per-entity materials
    // work.
    //
    // Layout at the blob root:
    //   [AssetHeader]
    //   [MeshLibraryMetadata]
    //   [MeshLibraryEntry x meshCount]       - entry table, indexed by meshId
    //   [vertexCount x vertexStride bytes]   - one shared vertex pool
    //   [indexCount x 4 bytes]               - one shared uint32 index pool
    //
    // meshId = position in the entry table (0..meshCount-1). Each entry has its own
    // vertex / index range in the shared pools, its own AABB and an optional default
    // material index (into the scene's material registry).
    //
    // ---- MeshLibraryMetadata::flags bits ----
    // The interleaved vertex layout follows from these flags via
    // ComputeMeshLibVertexLayout() below: pos(12) + normal(12) + uv0(8) always (base
    // stride 32), then tangent / uv1 / color in that order when their bit is set, then
    // UV2 / UV3. The importer (writer) and MeshManager (reader) must derive offsets and
    // stride from that one helper.
    //
    // bit 0: float4 tangent (16 bytes) - xyz = tangent direction, w = bitangent
    //        handedness sign +-1 (flags = HAS_TANGENT -> stride 48; flags = 0 -> 32).
    // bit 1: float2 uv1 (8 bytes)      - second UV set (lightmap / detail / blend).
    // bit 2: R8G8B8A8 color (4 bytes)  - packed per-vertex color (RGBA, 0..255).
    constexpr uint32_t MESHLIB_FLAG_HAS_TANGENT = 1u << 0;
    constexpr uint32_t MESHLIB_FLAG_HAS_UV1     = 1u << 1;
    constexpr uint32_t MESHLIB_FLAG_HAS_COLOR   = 1u << 2;

    // bits 3/4: optional float2 UV2/UV3 (8 bytes each).
    constexpr uint32_t MESHLIB_FLAG_HAS_UV2 = 1u << 3;
    constexpr uint32_t MESHLIB_FLAG_HAS_UV3 = 1u << 4;

    // bit 5: not a vertex stream. The file carries a tail after the index buffer
    // (MeshLibraryMetadata::reserved = its byte offset from the blob start): a chunk
    // directory whose first chunk is the identity table (Resource/MeshLibIdentity.h).
    // ComputeMeshLibVertexLayout ignores the bit, and a reader that stops at the index
    // buffer never sees the tail. Put new per-library data (meshlets, morphs) in more
    // chunks - never in a new layout bit, never in MeshLibraryEntry.
    constexpr uint32_t MESHLIB_FLAG_HAS_TAIL = 1u << 5;

    struct MeshLibraryMetadata
    {
        uint32_t meshCount;       // count of MeshLibraryEntry entries
        uint32_t vertexCount;     // total vertices in the shared pool
        uint32_t indexCount;      // total indices in the shared pool
        uint16_t vertexStride;    // bytes per vertex: 32 base, 48 with tangent,
                                  // more with the other streams (see flags)
        uint16_t indexStride;     // 4 (uint32 indices only)
        uint32_t flags;           // see MESHLIB_FLAG_* above. bit 0 = has tangent.
        uint32_t reserved;        // == the tail's byte offset when MESHLIB_FLAG_HAS_TAIL
                                  // is set, else 0
    };

    // .aclip - audio clip (PCM; the codec field is reserved for Vorbis / Opus).
    // Payload layout:
    //   [data bytes x dataSize]              raw PCM (or codec-specific) bytes
    //   [AudioCueRecord x cueCount]          optional cue / marker table
    //
    // Sample rate / channel count / bit depth match XAudio2's WAVEFORMATEX, so the
    // engine can SubmitSourceBuffer the data block directly.
    enum AudioCodec : uint32_t
    {
        AUDIO_CODEC_PCM    = 0,   // raw PCM, runtime-zero-copy
        AUDIO_CODEC_ADPCM  = 1,   // reserved
        AUDIO_CODEC_VORBIS = 2,   // reserved
        AUDIO_CODEC_OPUS   = 3,   // reserved
    };

    enum AudioLoadStrategy : uint32_t
    {
        AUDIO_LOAD_FULL       = 0,   // the whole data block resident; the default
        AUDIO_LOAD_COMPRESSED = 1,   // reserved (decode-on-load to PCM)
        AUDIO_LOAD_STREAMING  = 2,   // reserved (streamer thread feeds buffers)
    };

    struct AudioClipMetadata
    {
        uint32_t codec;               // AudioCodec
        uint16_t channels;            // 1 / 2 / 6 / 8
        uint16_t bitsPerSample;       // 8 / 16 / 24 / 32
        uint32_t sampleRate;          // Hz
        uint32_t sampleCount;         // total frames (samples per channel)
        uint32_t blockAlign;          // bytes per frame (== channels * bitsPerSample/8 for PCM)

        float    duration;            // seconds
        float    peakAmplitude;       // 0..1, for mixer normalization (0 if not measured)
        float    loudnessLUFS;        // integrated loudness; 0 if not measured

        uint32_t strategy;            // AudioLoadStrategy
        uint32_t loopStart;           // sample index; 0xFFFFFFFF = no loop
        uint32_t loopEnd;             // sample index; 0xFFFFFFFF = no loop

        uint32_t cueCount;            // count of AudioCueRecord entries appended after data
        uint32_t reserved;
    };
    static_assert(sizeof(AudioClipMetadata) == 52, "AudioClipMetadata layout drift");

    struct AudioCueRecord
    {
        uint32_t sampleOffset;        // sample-index marker
        char     name[32];            // e.g. "footstep_left", "beat_1"
        uint8_t  _pad[28];            // pad to 64 bytes for cheap row scanning
    };
    static_assert(sizeof(AudioCueRecord) == 64, "AudioCueRecord layout drift");

    // One entry of the library = one material section of a source object (the
    // importers split objects per material). meshId is the entry's position in this
    // table, so it is not stable across a re-import. Indices are absolute into the
    // library-wide VB; vertexStart / vertexCount describe the contiguous range this
    // entry's indices land in (a reader that wants local indices subtracts vertexStart).
    // Keep this struct 48 bytes: engine and tool readers walk the table with
    // sizeof(MeshLibraryEntry). New per-entry data goes in a tail chunk.
    struct MeshLibraryEntry
    {
        uint32_t vertexStart;        // first vertex index in shared VB
        uint32_t vertexCount;        // number of vertices this mesh owns
        uint32_t indexStart;         // first index into shared IB
        uint32_t indexCount;         // number of indices this mesh draws
        float    aabbMin[3];         // mesh-local AABB
        float    aabbMax[3];
        uint32_t defaultMaterialIdx; // index into scene's material registry
                                     // (kInvalidMaterial = 0xFFFFFFFF)
        uint32_t flags;              // reserved
    };
    static_assert(sizeof(MeshLibraryEntry) == 48, "MeshLibraryEntry must be 48 bytes");

    // ==== .meshlib tail (MESHLIB_FLAG_HAS_TAIL) ====
    // [MeshLibTailHeader][MeshLibChunkDesc x chunkCount][chunk payloads, 8-aligned]
    // Readers parse with memcpy (the blob is not guaranteed 8-aligned in memory).
    constexpr uint32_t MESHLIB_TAIL_MAGIC  = 'MLTL';
    constexpr uint32_t MESHLIB_CHUNK_IDNT  = 'IDNT';   // identity table, Resource/MeshLibIdentity.h
    constexpr uint16_t MESHLIB_TAIL_VERSION = 1;
    constexpr uint32_t MESHLIB_IDNT_VERSION = 1;

    struct MeshLibTailHeader
    {
        uint32_t magic;           // MESHLIB_TAIL_MAGIC
        uint16_t version;         // MESHLIB_TAIL_VERSION
        uint16_t chunkCount;
    };
    static_assert(sizeof(MeshLibTailHeader) == 8, "MeshLibTailHeader layout drift");

    struct MeshLibChunkDesc
    {
        uint32_t fourcc;          // MESHLIB_CHUNK_*
        uint32_t version;         // the chunk's own format version
        uint32_t offset;          // from the BLOB start
        uint32_t size;            // bytes
    };
    static_assert(sizeof(MeshLibChunkDesc) == 16, "MeshLibChunkDesc layout drift");

    // IDNT v1: [MeshLibIdentityHeader][MeshLibMeshRecord x meshCount]
    //          [MeshLibSectionRecord x sectionCount][uint32 sectionIndexList[sectionListCount]]
    //          [char stringPool[stringPoolBytes]]   (NUL-terminated UTF-8; offset 0 == "")
    struct MeshLibIdentityHeader
    {
        uint64_t modelKey;
        uint32_t meshCount;
        uint32_t sectionCount;      // MUST equal MeshLibraryMetadata::meshCount (entry i <-> section i)
        uint32_t sectionListCount;  // total length of sectionIndexList
        uint32_t stringPoolBytes;
        uint32_t flags;             // IDNT_FLAG_*
        uint32_t reserved;
    };
    static_assert(sizeof(MeshLibIdentityHeader) == 32, "MeshLibIdentityHeader layout drift");

    struct MeshLibMeshRecord
    {
        uint64_t key;
        uint32_t nameOffset;        // into the string pool
        uint32_t sourceKeyOffset;
        uint32_t firstInList;       // this mesh's run in sectionIndexList
        uint32_t sectionCount;
        float    aabbMin[3];        // union of its sections, mesh-local
        float    aabbMax[3];
        uint32_t flags;             // MESH_IDENT_*
        uint32_t reserved;
    };
    static_assert(sizeof(MeshLibMeshRecord) == 56, "MeshLibMeshRecord layout drift");

    struct MeshLibSectionRecord
    {
        uint64_t key;
        uint32_t slotNameOffset;
        uint32_t sourceKeyOffset;
        uint32_t ownerMesh;         // first mesh that lists this entry (UINT32_MAX: none)
        uint32_t flags;
        uint32_t reserved[2];
    };
    static_assert(sizeof(MeshLibSectionRecord) == 32, "MeshLibSectionRecord layout drift");
#pragma pack(pop)

    // Interleaved .meshlib vertex layout: the single source of truth for the importer
    // (which packs the VB) and MeshManager (which builds the MeshDescriptor stream
    // offsets). Offsets follow only from MeshLibraryMetadata::flags, so a file keeps
    // its byte layout (flags 0 -> 32 bytes, HAS_TANGENT -> 48) while new attributes
    // append:
    //   pos(12) + normal(12) + uv0(8)  always (base 32 bytes)
    //   + tangent(16) if HAS_TANGENT
    //   + uv1(8)      if HAS_UV1
    //   + color(4)    if HAS_COLOR     (R8G8B8A8 packed)
    //   + uv2(8)      if HAS_UV2
    //   + uv3(8)      if HAS_UV3
    // Every field is a multiple of 4 bytes, so all offsets stay 4-byte aligned, as the
    // PVF fetch path's ByteAddressBuffer Load / Load2 / Load4 requires.
    struct MeshLibVertexLayout
    {
        uint32_t stride        = 0;
        uint32_t posOffset     = 0;
        uint32_t normalOffset  = 0;
        uint32_t uv0Offset     = 0;
        uint32_t tangentOffset = 0xFFFFFFFFu;  // 0xFFFFFFFF when the stream is absent
        uint32_t uv1Offset     = 0xFFFFFFFFu;
        uint32_t uv2Offset     = 0xFFFFFFFFu;
        uint32_t uv3Offset     = 0xFFFFFFFFu;
        uint32_t colorOffset   = 0xFFFFFFFFu;
    };

    inline MeshLibVertexLayout ComputeMeshLibVertexLayout(uint32_t flags)
    {
        MeshLibVertexLayout L;
        uint32_t off = 0;
        L.posOffset    = off; off += 12;
        L.normalOffset = off; off += 12;
        L.uv0Offset    = off; off += 8;
        if (flags & MESHLIB_FLAG_HAS_TANGENT) { L.tangentOffset = off; off += 16; }
        if (flags & MESHLIB_FLAG_HAS_UV1)     { L.uv1Offset     = off; off += 8;  }
        if (flags & MESHLIB_FLAG_HAS_COLOR)   { L.colorOffset   = off; off += 4;  }
        if (flags & MESHLIB_FLAG_HAS_UV2) { L.uv2Offset = off; off += 8; }
        if (flags & MESHLIB_FLAG_HAS_UV3) { L.uv3Offset = off; off += 8; }
        L.stride = off;
        return L;
    }

    // Packs a float RGBA color (0..1) into R8G8B8A8 (byte 0 = R ... byte 3 = A),
    // matching HLSL FetchR8G8B8A8 in pvf_fetch.hlsli.
    inline uint32_t PackColorRGBA8(float r, float g, float b, float a)
    {
        auto q = [](float v) -> uint32_t {
            float c = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
            return static_cast<uint32_t>(c * 255.f + 0.5f);
        };
        return (q(r)) | (q(g) << 8) | (q(b) << 16) | (q(a) << 24);
    }

    // ==== Accessors ====
    // No copies, pointer arithmetic only; each needs a non-null blob of sufficient size.
    // A null blob is reported (once per accessor: these sit on hot paths) and answered
    // with nullptr - an address just past 0 would pass every caller's null test.

    inline const AssetHeader* GetHeader(const uint8_t* blob)
    {
        if (!ENGINE_CHECK_ONCE(blob != nullptr, "Resource::GetHeader: null asset blob"))
            return nullptr;
        return reinterpret_cast<const AssetHeader*>(blob);
    }

    // Cast the Metadata region to the concrete struct T.
    // Caller is responsible for matching T to the correct ResourceType.
    //
    // Example (a loader's Load(path, data)):
    //   const uint8_t* blob = data.data();
    //   if (!ENGINE_CHECK(GetHeader(blob)->magic == MAGIC_TEXTURE, "not a texture blob"))
    //       return;
    //   const TextureMetadata* meta = GetMetadata<TextureMetadata>(blob);
    //   DXGI_FORMAT fmt = static_cast<DXGI_FORMAT>(meta->format);
    template<typename T>
    inline const T* GetMetadata(const uint8_t* blob)
    {
        if (!ENGINE_CHECK_ONCE(blob != nullptr, "Resource::GetMetadata: null asset blob"))
            return nullptr;
        return reinterpret_cast<const T*>(blob + sizeof(AssetHeader));
    }

    // Returns a pointer to the raw payload (DDS bytes, vertex data, DXBC bytecode, ...).
    inline const uint8_t* GetPayload(const uint8_t* blob)
    {
        if (!ENGINE_CHECK_ONCE(blob != nullptr, "Resource::GetPayload: null asset blob"))
            return nullptr;
        const AssetHeader* h = GetHeader(blob);
        return blob + sizeof(AssetHeader) + h->metadataSize;
    }

    // The text of a text container (.iscene / .ipfb / .imat / .imatinst / .iscn): the
    // payload up to its NUL, bounded by the blob too (for callers that check only the
    // magic). Writers store the text and one NUL (dataSize = text + 1). Do not take all
    // dataSize bytes: when the text does not end in '\n' (a Python tool's may not, e.g.
    // engine_tools.imat) the NUL lands inside the last value, so a last `path=<p>` would
    // read "<p>\0". An .iscn written without a NUL (dataSize = text) is all text.
    inline std::string_view GetPayloadText(const uint8_t* blob, size_t blobSize)
    {
        if (!blob || blobSize < sizeof(AssetHeader))
            return {};
        const AssetHeader* h   = GetHeader(blob);
        const size_t       off = sizeof(AssetHeader) + static_cast<size_t>(h->metadataSize);
        if (off > blobSize)
            return {};
        const size_t avail = blobSize - off;
        const size_t size  = h->dataSize < avail ? static_cast<size_t>(h->dataSize) : avail;
        const char*  text  = reinterpret_cast<const char*>(blob + off);
        return std::string_view(text, strnlen(text, size));
    }

    // ==== Header validation ====
    enum class HeaderStatus : uint8_t
    {
        Ok,
        TooShort,    // fewer bytes than an AssetHeader
        WrongMagic,  // some other format
        Newer,       // version above this engine's current for the magic
        Older,       // version below the minimum the reader still lifts
        Truncated,   // the header declares more bytes than the blob holds
    };

    struct HeaderCheck
    {
        HeaderStatus status       = HeaderStatus::TooShort;
        uint32_t     expectedMagic = 0;
        uint32_t     foundMagic    = 0;
        uint16_t     version       = 0;   // what the file says
        uint16_t     current       = 0;   // what this engine writes for expectedMagic
        uint16_t     minimum       = 0;   // the oldest it reads
    };

    // True under -asset_strict_version (harness builds): a version other than the current one
    // is refused, as the single global ASSET_VERSION did before each format had its own line.
    // Defined in Resoruce/AssetVersions.cpp.
    bool StrictAssetVersions();

    inline HeaderCheck CheckHeader(const uint8_t* blob, size_t blobSize, uint32_t expectedMagic)
    {
        HeaderCheck c;
        c.expectedMagic = expectedMagic;
        c.current       = CurrentVersion(expectedMagic);
        c.minimum       = MinimumVersion(expectedMagic);
        if (!blob || blobSize < sizeof(AssetHeader))
            return c;
        const AssetHeader* h = GetHeader(blob);
        c.foundMagic = h->magic;
        c.version    = h->version;
        if (h->magic != expectedMagic)
        {
            c.status = HeaderStatus::WrongMagic;
            return c;
        }
        if (h->version > c.current)
        {
            c.status = HeaderStatus::Newer;
            return c;
        }
        if (h->version < c.minimum || (h->version != c.current && StrictAssetVersions()))
        {
            c.status = HeaderStatus::Older;
            return c;
        }
        const size_t expectedTotal = sizeof(AssetHeader) + h->metadataSize + h->dataSize;
        c.status = blobSize >= expectedTotal ? HeaderStatus::Ok : HeaderStatus::Truncated;
        return c;
    }

    // A magic as written in source, the most significant byte first: 'RMAT' (a byte that is not
    // printable shows as '?').
    std::string MagicText(uint32_t magic);

    // The stable machine name of a status: "ok", "too_short", "wrong_magic", "newer", "older",
    // "truncated".
    const char* HeaderStatusName(HeaderStatus status);

    // One sentence for a failed check, the text a loader's failure reason and the log carry:
    // "material header: version 3 is newer than this engine reads (1..2) - written by a newer build".
    std::string DescribeHeaderCheck(const HeaderCheck& check);

    // Why a loader that reads the blob its own way (it checks the size itself) must refuse it: the
    // DescribeHeaderCheck text for a wrong magic, a version above current or one below minimum;
    // empty otherwise.
    std::string HeaderRefusal(const uint8_t* blob, size_t blobSize, uint32_t expectedMagic);

    // Logs a refused version once per (magic, version). Called by ValidateHeader; a loader that
    // reports its own reason need not call it.
    void ReportUnsupportedVersion(const HeaderCheck& check);

    // Validate magic and version before trusting the rest of the header. A version outside
    // [MinimumVersion, CurrentVersion] of the magic is logged (once) and refused.
    inline bool ValidateHeader(const uint8_t* blob, size_t blobSize, uint32_t expectedMagic)
    {
        const HeaderCheck c = CheckHeader(blob, blobSize, expectedMagic);
        if (c.status == HeaderStatus::Newer || c.status == HeaderStatus::Older)
            ReportUnsupportedVersion(c);
        return c.status == HeaderStatus::Ok;
    }

    // The same, and on failure @p why holds the reason (what a loader returns as its failure
    // reason, so Asset.get and the resource manager's Failed entry say it).
    inline bool ValidateHeader(const uint8_t* blob, size_t blobSize, uint32_t expectedMagic, std::string& why)
    {
        const HeaderCheck c = CheckHeader(blob, blobSize, expectedMagic);
        if (c.status == HeaderStatus::Newer || c.status == HeaderStatus::Older)
            ReportUnsupportedVersion(c);
        if (c.status != HeaderStatus::Ok)
            why = DescribeHeaderCheck(c);
        return c.status == HeaderStatus::Ok;
    }
}
